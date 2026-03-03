#include "llvm/Transforms/Utils/SplitModuleCG.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Value.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include <algorithm>
#include <cassert>
#include <iterator>
#include <memory>
#include <queue>
#include <utility>
#include <vector>
#include <mutex>
#include <numa.h>

std::mutex mtx;

using namespace llvm;

#define DEBUG_TYPE "split-module-CG"

namespace {

static cl::opt<float> LargeFnFactor(
    "split-module-CG-large-function-threshold", cl::init(2.0f),
    cl::Hidden,
    cl::desc(
        "consider a function as large and needing special treatment when the "
        "cost of importing it into a partition"
        "exceeds the average cost of a partition by this factor; e.g. 2.0 "
        "means if the function and its dependencies is 2 times bigger than "
        "an average partition; 0 disables large functions handling entirely"));

static cl::opt<float> LargeFnOverlapForMerge(
    "split-module-CG-large-function-merge-overlap", cl::init(1.0f),
    cl::Hidden,
    cl::desc(
        "defines how much overlap between two large function's dependencies "
        "is needed to put them in the same partition"));

static cl::opt<bool> enableSplitCallGraph(
    "enable-split-callgraph", cl::Hidden, cl::init(true),
    cl::desc("Control split to how many partitions in thinlto backend."));

static cl::opt<bool> enablePrintSimplifyCallGraph(
    "enable-print-simplify-callgraph", cl::Hidden, cl::init(false),
    cl::desc("print SimplifyCallGraph"));

static cl::opt<int> SplitCGDepthThreshold(
    "split-callgraph-depth-threshold", cl::Hidden, cl::init(10),
    cl::desc("If the number of stack calls exceeds the specified number of"
             "layers, the stack is split."));

static cl::opt<int> SplitCGLayersCount(
    "split-callgraph-layers-count", cl::Hidden, cl::init(1),
    cl::desc("Number of layers of call stack splitting"));

static cl::opt<float> CGSizeFactor(
    "split-callgraph-size-threshold", cl::Hidden, cl::init(3.0f),
    cl::desc("consider spliting the callgraph when"
             "exceeds the average cost of a partition by this factor; e;g. 3.0"));

static cl::opt<int> SplitCGFunctionSizeThreshold(
    "split-function-size-threshold", cl::Hidden, cl::init(500),
    cl::desc("split the large function from the callgraph as the new root;"
             "e.g. the codesize of function over the cost of 500."));

static cl::opt<bool> enableInlineClusterEstimation(
    "enable-inline-profit-estimation", cl::Hidden, cl::init(false),
    cl::desc("avoid spliting caller and callee when the callee can be inline."));

static cl::opt<bool> SplitBasedHotFuncs(
    "split-based-on-hot-func", cl::Hidden, cl::init(false),
    cl::desc(""));

static cl::opt<bool> CloneHotExternalOnly(
    "clone-hot-external-only", cl::Hidden, cl::init(false),
    cl::desc(""));


static cl::opt<bool> BindToNuma(
    "bind-to-numa", cl::Hidden, cl::init(false),
    cl::desc("binding to numa before clone module"));

using GetTTIFn = function_ref<const TargetTransformInfo &(Function &)>;
using PartitionID = unsigned;

static void externalize(GlobalValue *GV) {
  if (GV->hasLocalLinkage()) {
    GV->setLinkage(GlobalValue::ExternalLinkage);
    GV->setVisibility(GlobalValue::HiddenVisibility);
  }

  // Unnamed entities must be named consistently between modules. setName will
  // give a distinct name to each such entity.
  if (!GV->hasName())
    GV->setName("__llvmsplit_unnamed");
}

/// Calculates how much overlap there is between \p A and \p B.
/// \return A number between 0.0 and 1.0, where 1.0 means A == B and 0.0 means A
/// and B have no shared elements. Kernels do not count in overlap calculation.
static float calculateOverlap(const DenseSet<const Function *> &A,
                              const DenseSet<const Function *> &B,
                              const DenseSet<const Function *> &EntryFuncs) {
  DenseSet<const Function *> Total;
  for (const auto *F : A) {
    if (!EntryFuncs.count(F))
      Total.insert(F);
  }

  if (Total.empty())
    return 0.0f;

  unsigned NumCommon = 0;
  for (const auto *F : B) {
    if (EntryFuncs.count(F))
      continue;

    auto [It, Inserted] = Total.insert(F);
    if (!Inserted)
      ++NumCommon;
  }

  return static_cast<float>(NumCommon) / Total.size();
}

/// Performs all of the partitioning work on \p M.
/// \param M Module to partition.
/// \param NumParts Number of partitions to create.
/// \param ModuleCost Total cost of all functions in \p M.
/// \param FnCosts Map of Function -> Cost
/// \param WorkList Functions and their dependencies to process in order.
/// \param EntryFuncs Entry functions.
/// \returns The created partitions (a vector of size \p NumParts)
static std::vector<DenseSet<const Function *>>
doPartitioning(Module &M, unsigned NumParts,
               CostType ModuleCost,
               const DenseMap<const Function *, CostType> &FnCosts,
               const SmallVector<FunctionWithDependencies> &WorkList,
               const DenseSet<const Function *> &EntryFuncs) {
    LLVM_DEBUG(dbgs() << "\n--Partitioning Starts--\n");

    std::vector<DenseSet<const Function *>> Partitions;
    Partitions.resize(NumParts);
    if (NumParts == 0)
      return Partitions;
    const CostType LargeFnThreshold =
      LargeFnFactor ? CostType(((ModuleCost / NumParts) * LargeFnFactor))
                    : std::numeric_limits<CostType>::max();

    auto ComparePartitions = [](const std::pair<PartitionID, CostType> &a,
                                const std::pair<PartitionID, CostType> &b) {
    // When two partitions have the same cost, assign to the one with the
    // biggest ID first. This allows us to put things in P0 last, because P0 may
    // have other stuff added later.
    if (a.second == b.second)
      return a.first < b.first;
    return a.second > b.second;
  };

  // We can't use priority_queue here because we need to be able to access any
  // element. This makes this a bit inefficient as we need to sort it again
  // everytime we change it, but it's a very small array anyway (likely under 64
  // partitions) so it's a cheap operation.
  std::vector<std::pair<PartitionID, CostType>> BalancingQueue;
  for (unsigned I = 0; I < NumParts; ++I)
    BalancingQueue.emplace_back(I, 0);

  // Helper function to handle assigning a function to a partition. This takes
  // care of updating the balancing queue.
  const auto AssignToPartition = [&](PartitionID PID,
                                     const FunctionWithDependencies &FWD) {
    auto &FnsInPart = Partitions[PID];
    FnsInPart.insert(FWD.F);
    FnsInPart.insert(FWD.Dependencies.begin(), FWD.Dependencies.end());

    {std::lock_guard<std::mutex> lock(mtx);
    LLVM_DEBUG(dbgs() << "assign " << FWD.F->getName() << " to P" << PID << "\n");
    if (!FWD.Dependencies.empty())
      LLVM_DEBUG(dbgs() << FWD.Dependencies.size() << " dependencies added\n");}

    // Update the balancing queue. we scan backwards because in the common case
    // the partition is at the end.
    for (auto &[QueuePID, Cost] : reverse(BalancingQueue)) {
      if (QueuePID == PID) {
        CostType NewCost = 0;
        for (auto *Fn : Partitions[PID])
          NewCost += FnCosts.at(Fn);
        Cost = NewCost;
      }
    }

    sort(BalancingQueue, ComparePartitions);
  };

  for (auto &CurFn : WorkList) {
    // When a function has indirect calls, it must stay in the first partition
    // alongside every reachable non-entry function. This is a nightmare case
    // for splitting as it severely limits what we can do.
    if (CurFn.HasAliasesCall) {
      {std::lock_guard<std::mutex> lock(mtx);
      LLVM_DEBUG(dbgs() << "Function with indirect call(s): " << CurFn.F->getName()
                        << " defaulting to P0\n");}
      AssignToPartition(0, CurFn);
      continue;
    }

    // If the function is an ifunc, it must stay in the every partition.
    if (CurFn.HasIfuncResolver) {
      {std::lock_guard<std::mutex> lock(mtx);
      LLVM_DEBUG(dbgs() << "Function with ifunc call(s): " << CurFn.F->getName()
                        << " defaulting to P_i\n");}
      for (int part_i = 0; part_i < NumParts; ++part_i) {
        AssignToPartition(part_i, CurFn);
      }
      continue;
    }

    // If the function is in a comdat, it must stay in the first partition.
    if (CurFn.HasComdatMember) {
      {std::lock_guard<std::mutex> lock(mtx);
      LLVM_DEBUG(dbgs() << "Function with comdat member(s): " << CurFn.F->getName()
                        << " defaulting to P0\n");}
      AssignToPartition(0, CurFn);
      continue;
    }

    // Be smart with large functions to avoid duplicating their dependencies.
    if (CurFn.isLarge(LargeFnThreshold)) {
      assert(LargeFnOverlapForMerge >= 0.0f && LargeFnOverlapForMerge <= 1.0f);

      bool Assigned = false;
      for (const auto &[PID, Fns] : enumerate(Partitions)) {
        float Overlap = calculateOverlap(CurFn.Dependencies, Fns, EntryFuncs);
        if (Overlap > LargeFnOverlapForMerge) {
          LLVM_DEBUG(dbgs() << "  selecting P" << PID << "\n");
          AssignToPartition(PID, CurFn);
          Assigned = true;
        }
      }

      if (Assigned)
        continue;
    }
    // Normal "load-balancing", assign to partition with least pressure.
    auto [PID, CurCost] = BalancingQueue.back();
    AssignToPartition(PID, CurFn);
  }

  return Partitions;
}
}

void SplitModuleCG::calculateFunctionCosts() {
  ModuleCost = 0;
  for (auto &Fn : M) {
    if (Fn.isDeclaration())
      continue;

    CostType FnCost = 0;
    for (const auto &BB : Fn) {
      CostType CostVal = std::distance(BB.begin(), BB.end());
      FnCost += CostVal;
    }
    assert(FnCost != 0);
    FuncsCosts[&Fn] = FnCost;
    assert((ModuleCost + FnCost) >= ModuleCost && "Overflow!");
    ModuleCost += FnCost;
  }
}

void SplitModuleCG::getHotFunction() {
  ProfileSummaryInfo PSI(M);
  if(!PSI.hasProfileSummary())
    return;	  

  for (Function &F : M) {
    if (F.hasFnAttribute(Attribute::Hot) || PSI.isFunctionEntryHot(&F)) {
      HotFuncs.insert(&F);
    }
  }
}

void SplitModuleCG::getLargeFunction() {
  for (auto &FCItem : FuncsCosts) {
    if (FCItem.second > SplitCGFunctionSizeThreshold) {
      LargeFuncs.insert(FCItem.first);
      externalize(const_cast<Function *>(FCItem.first));
      externalFunction[FCItem.first] = true;
    }
  }
}

void SplitModuleCG::getAliasFunction() {
  for (GlobalAlias &GA : M.aliases()) {
    const GlobalObject *GO = GA.getAliaseeObject();
    if (const auto *Funcs = dyn_cast<Function>(GO)) {
      AliasesFuncs.insert(Funcs);
    }
  }
}

void SplitModuleCG::getIfuncFunction() {
  for (GlobalIFunc &GA : M.ifuncs()) {
    GlobalObject *GO = GA.getResolverFunction();
    if (auto *Funcs = dyn_cast<Function>(GO)) {
      Funcs->setLinkage(GlobalValue::WeakODRLinkage);
      Funcs->setVisibility(GlobalValue::DefaultVisibility);
      llvm::Comdat *C = Funcs->getParent()->getOrInsertComdat(Funcs->getName());
      C->setSelectionKind(Comdat::SelectionKind::Any);
      Funcs->setComdat(C);
      IfuncFuncs.insert(Funcs);
      GA.setComdat(C);
      if (externalFunction.count(Funcs))
        externalFunction.erase(Funcs);
    }
  }
}


void SplitModuleCG::calculateEntryFuncs() {
  // First, find all the entry functions with an in-degree of 0
  // (i.e., those that are not called by any function).
  SmallVector<const Function *> WorkList;
  DenseSet<const Function *> FindedFuncs;
  for (auto &NodePair : *SCG) {
    SimplifyCallGraphNode *SCGNode = NodePair.second.get();
    Function *F = SCGNode->getFunction();
    if (F && SCGNode->getNumReferences() == 0) {
      EntryFuncs.insert(F);
      FindedFuncs.insert(F);
      WorkList.push_back(F);
    }
  }

    // Find all the functions that can be found through the entry functions.
  while (!WorkList.empty()) {
    const auto &CurFn = *WorkList.pop_back_val();
    assert(!CurFn.isDeclaration());
    for (auto &SCGNode : *SCG->at(&CurFn)) {
      auto *Callee = SCGNode->getFunction();
      if (!Callee || Callee->isDeclaration())
        continue;

        auto [It, Inserted] = FindedFuncs.insert(Callee);
      if (Inserted)
        WorkList.push_back(Callee);
    }
  }

  // Traverse all SCCs and add those that have not yet been included
  // in FindedFuncs.
  scc_iterator<CallGraph*> CGI = scc_begin(&CG);
  for (scc_iterator<CallGraph *> SCCI = scc_begin(&CG); !SCCI.isAtEnd();
       ++SCCI) {
    const std::vector<CallGraphNode *> &curSCC = *SCCI;
    if (curSCC.size() == 1 && !SCCI.hasCycle())
      continue;
    if (!FindedFuncs.count(curSCC[0]->getFunction())) {
      EntryFuncs.insert(curSCC[0]->getFunction());
      for (CallGraphNode *CGN : curSCC)
      {
        FindedFuncs.insert(CGN->getFunction());
      }
    }
  }

  // For debug: output all exit
  {std::lock_guard<std::mutex> lock(mtx);
  LLVM_DEBUG(dbgs() << M.getModuleIdentifier() << "function enties are:  ");
  for (auto *Entry : EntryFuncs) {
    LLVM_DEBUG(dbgs() << Entry->getName() << "   ");
  }
  LLVM_DEBUG(dbgs() << "\n");
  }
}

void SplitModuleCG::UpdateFWDInfo(llvm::FunctionWithDependencies &FWD) {
  FWD.Dependencies.clear();
  FWD.HasAliasesCall = false;
  FWD.HasIfuncResolver = false;
  FWD.HasComdatMember = false;
  addAllDependencies(*SCG, *FWD.F, FWD.Dependencies, externalFunction);
  FWD.TotalCost = FuncsCosts.lookup(FWD.F);
  if (AliasesFuncs.count(FWD.F))
    FWD.HasAliasesCall = true;
  if (IfuncFuncs.count(FWD.F))
    FWD.HasIfuncResolver = true;
  if (ComdatFuncs.count(FWD.F))
    FWD.HasComdatMember = true;

  for (const auto *Dep : FWD.Dependencies) {
    FWD.TotalCost += FuncsCosts.lookup(Dep);
    if (AliasesFuncs.count(Dep))
      FWD.HasAliasesCall = true;
    if (IfuncFuncs.count(Dep))
      FWD.HasIfuncResolver = true;
    if (ComdatFuncs.count(Dep))
      FWD.HasComdatMember = true;
  }
}

void SplitModuleCG::splitLargeCG(SmallVector<llvm::FunctionWithDependencies> &WorkList) {
  SmallVector<size_t> NewWorkList;
  for (size_t i = 0; i < WorkList.size(); ++i) {
    NewWorkList.push_back(i);
  }

  const CostType LargeFnThreshold =
      CGSizeFactor ? CostType(((ModuleCost / N) * CGSizeFactor))
                   : std::numeric_limits<CostType>::max();

  DenseSet<const Function *> NewEntryFuncs;
  while (!NewWorkList.empty()) {
    size_t index = NewWorkList.pop_back_val();
    llvm::FunctionWithDependencies &FWD = WorkList[index];
    if (!FWD.isLarge(LargeFnThreshold) || FWD.SplitedLayer >= SplitCGLayersCount)
      continue;

    auto *CallNode = SCG->getOrInsertFunction(FWD.F);
    for (auto &CalleeNode : *SCG->at(FWD.F)) {
      auto *Callee = CalleeNode->getFunction();
      if (AliasesFuncs.count(Callee)|| HotFuncs.count(Callee) || !CalleeNode->CheckCallDepth())
        continue;

      if (enableInlineClusterEstimation)
        // Do not split the callgraph edge if caller and callee are in the same cluster.
        if (IPE->fromSameCluster(FWD.F, Callee))
          continue;

      // split
      CallNode->removeCalledFunction(CalleeNode);
      externalize(Callee);
      externalFunction[Callee] = true;
      NewEntryFuncs.insert(Callee);
    }

    int SplitedLayer = FWD.SplitedLayer;
    for (auto *F : NewEntryFuncs) {
      if (EntryFuncs.find(F) != EntryFuncs.end())
        continue;
      WorkList.emplace_back(*SCG, FuncsCosts, F, 
                            AliasesFuncs, externalFunction, IfuncFuncs, ComdatFuncs);
      WorkList[WorkList.size() - 1].SplitedLayer = SplitedLayer + 1;
      NewWorkList.push_back(WorkList.size() - 1);
    }

    UpdateFWDInfo(WorkList[index]);
  }
}

void SplitModuleCG::calculateComdatMembers() {
  for (GlobalValue &GValue : M.global_values()) {
    if (Comdat *C = GValue.getComdat()) {
      ComdatMembers[C].insert(&GValue);
    }
  }

  for (auto &ComdatMember : ComdatMembers) {
    if (ComdatMember.second.size() > 1) {
      for (auto *GValue : ComdatMember.second) {
        if (auto *F = dyn_cast<Function>(GValue))
          ComdatFuncs.insert(F);
      }
    }
  }
}

using Clock = std::chrono::high_resolution_clock;
using Ms = std::chrono::milliseconds;

void SplitModuleCG::SplitModule(TargetMachine *TM, ModuleCreationCallback ModuleCallback,
    bool PreserveLocals) {
  for (Function &F : M) {
    if (!F.hasAddressTaken()) {
      ChangeLinkageFuncs[F.getName()] = false;
    } else {
      externalize(&F);
    }
    if (!F.isDeclaration() && (F.hasExternalLinkage() || !F.isDefinitionExact()))
      externalFunction[&F] = true;
  }
  for (GlobalVariable &GV : M.globals())
    externalize(&GV);

  calculateComdatMembers();
  getIfuncFunction();

  SmallVector<FunctionWithDependencies> WorkList;
  for (auto *F : EntryFuncs) {
    WorkList.emplace_back(*SCG, FuncsCosts, F, 
                          AliasesFuncs, externalFunction, IfuncFuncs, ComdatFuncs);
  }

  if (enableInlineClusterEstimation && enableSplitCallGraph)
    IPE = std::make_unique<InlineClusterEstimation>(M, CG, TM);

  if (enableSplitCallGraph)
    splitLargeCG(WorkList);
  LLVM_DEBUG(dbgs() << " WorkList size " << WorkList.size() << "\n");

  {
    DenseSet<const Function *> SeenFunctions;
    for (const auto &FWD : WorkList) {
      SeenFunctions.insert(FWD.F);
      SeenFunctions.insert(FWD.Dependencies.begin(), FWD.Dependencies.end());
    }
    for (auto &F : M) {
      // If this function is not part of any kernel's dependencies and isn't
      // directly called, consider it as a root.
      if (!F.isDeclaration() && !SeenFunctions.count(&F)) {
        {
          std::lock_guard<std::mutex> lock(mtx);
          LLVM_DEBUG(dbgs() << "!!!! lost function!!!! " << F.getName() << "\n");
        }
      }
    }
  }
  // Sort the worklist so the most expensive roots are seen first.
  sort(WorkList, [&](auto &A, auto &B) {
    // Sort by total cost, and if the total cost is identical, sort
    // alphabetically
    if (A.TotalCost == B.TotalCost)
      return A.F->getName() < B.F->getName();
    return A.TotalCost > B.TotalCost;
  });

  // For debug: CG assign result
  {
    std::lock_guard<std::mutex> lock(mtx);
    LLVM_DEBUG(dbgs() << "result: \n");
    for (auto FWD : WorkList) {
      LLVM_DEBUG(dbgs() << "[root] " << FWD.F->getName()
                        << " (totalCost:" << FWD.TotalCost
                        << " indirect:" << FWD.HasAliasesCall
			<< "Fun cost: " << FuncsCosts[FWD.F]
                        << ")\n");
      for (auto * F : FWD.Dependencies) {
        LLVM_DEBUG(dbgs() << " [dependency] " << F->getName() << " "
                          << externalFunction.count(F)<< " "<< FuncsCosts[F] << "\n");
      }
    }
  }

  auto Partitions = doPartitioning(M, N, ModuleCost, FuncsCosts, WorkList, EntryFuncs);
  assert(Partitions.size() == N);

  // If we didn't externalize GVs, then local GVs need to be conservatively
  // imported into [dependency]every module (including their initializers), and then cleaned
  // up afterwards.
  const auto NeedsConservativeImport = [&](const GlobalValue *GV) {
    // We conservatively import private/internal GVs into every module and clean
    // them up afterwards.
    const auto *Var = dyn_cast<GlobalVariable>(GV);
    return Var && Var->hasLocalLinkage();
  };

  int MainNuma;
  if (BindToNuma && numa_available() == 0)
    MainNuma = numa_node_of_cpu(sched_getcpu());
 
  unsigned TotalFnImpls = 0;
  SmallString<0> BC;
  raw_svector_ostream BCOS(BC);
  WriteBitcodeToFile(M, BCOS);
  // auto SharedBC = std::make_shared<std::string>(BC.str().str());
  Expected<BitcodeModule> BMOrErr = parseBitcodeFileStream(MemoryBufferRef(BC.str(), "ld-temp.o"));
  if (!BMOrErr)
    report_fatal_error("Failed to read bitcode");
  BitcodeModule BM = std::move(BMOrErr.get());
  for (unsigned I = 0; I < N; ++I) {
    auto TimeStart = Clock::now();
    PartitionThreadPool->async([&, I]() {

      if (BindToNuma && numa_available() == 0) {
        numa_run_on_node(MainNuma);
      }

      const auto &FnsInPart = Partitions[I];

      std::unique_ptr<Module> MPart;
      llvm::lto::LTOLLVMContext Ctx(C);
      {
        Expected<std::unique_ptr<Module>> MOrErr = BM.parseModule(Ctx);
        if (!MOrErr)
          report_fatal_error("Failed to read bitcode");
        std::unique_ptr<Module> MInCtx = std::move(MOrErr.get());
        ValueToValueMapTy VMap;
        MPart =
          CloneModule(*MInCtx, VMap, [&](const GlobalValue *GV) {
            // Functions go in their assigned partition.
            if (const auto *newFn = dyn_cast<Function>(GV)) {
              const auto *Fn = M.getFunction(newFn->getName());
              return FnsInPart.contains(Fn);
	    }

            if (NeedsConservativeImport(GV))
              return true;

            // Everything else goes in the first partition.
            return I == 0;
          });
      }

    // collect symbols to rename
    auto checkPromoted = [&](const GlobalValue &GV) {
      // now is external (not local), but not in external set.
      if ((!GV.hasLocalLinkage() || ChangeLinkageFuncs.count(GV.getName())) && !OriginalExternals.contains(GV.getName())) {
        std::lock_guard<std::mutex> lock(mtx);
        if (PromotedRenames.count(GV.getName()))
          return;
        std::string NewName =	 
             GV.getName().str() + "_" + M.getModuleIdentifier();	 
         PromotedRenames[GV.getName()] = NewName;
      }
    };
    for (const auto &GV : MPart->global_values())
      checkPromoted(GV);

    // Clean-up conservatively imported GVs without any users.
    for (auto &GV : make_early_inc_range(MPart->globals())) {
      if (NeedsConservativeImport(&GV) && GV.use_empty())
        GV.eraseFromParent();
    }

    for (auto &func : MPart->functions()) {
      auto Fn = M.getFunction(func.getName());
      std::lock_guard<std::mutex> lock(mtx);
      if (externalFunction.count(Fn) && !func.isDeclaration() && (HotFuncs.count(Fn) || !CloneHotExternalOnly) && !IfuncFuncs.count(Fn)) {
        if (!externalFunction[Fn]) {
          func.setLinkage(GlobalValue::AvailableExternallyLinkage);
          func.setSubprogram(nullptr);
          func.setComdat(nullptr);
        } else {
          externalFunction[Fn] = false;
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(mtx);
      LLVM_DEBUG(dbgs() << MPart->getModuleIdentifier() << "  : \n");
      for (auto &F : *MPart) {
        if (!F.isDeclaration())
          LLVM_DEBUG(dbgs() << "   [Function: ] " << F.getName() << " " << F.getLinkage() << "\n");
      }
    }
   auto TimeEnd = Clock::now();
    auto Elapsed = std::chrono::duration_cast<Ms>(TimeEnd - TimeStart);
    {
      std::lock_guard<std::mutex> lock(mtx);
      LLVM_DEBUG(dbgs() << "partition "<< I  << "  : " << Elapsed.count() << " ms\n");
    }
    ModuleCallback(std::move(MPart));
    });
  }
  PartitionThreadPool->wait();
}

SplitModuleCG::SplitModuleCG(Module &M, const llvm::lto::Config &C,
                             unsigned LimitPartition, ThreadPool *PartitionThreadPool)
    : M(M), CG(M), N(LimitPartition), PartitionThreadPool(PartitionThreadPool), C(C) {
  // record origin externals
  auto recordIfExternal = [&](const GlobalValue &GV) {
    if (!GV.hasLocalLinkage())
      OriginalExternals.insert(GV.getName());
  };
  for (const auto &GV : M.global_values())
    recordIfExternal(GV);
  calculateFunctionCosts();
  getAliasFunction();
  if (SplitCGFunctionSizeThreshold != 0)
    getLargeFunction();

  if(SplitBasedHotFuncs)
    getHotFunction();

  LLVM_DEBUG(dbgs() << HotFuncs.size() <<" hot functions in module "  << M.getName()<<" \n");


  SCG = std::make_unique<SimplifyCallGraph>(CG, LargeFuncs, HotFuncs, AliasesFuncs);
  calculateEntryFuncs();
  if (N == 0 || N > EntryFuncs.size()) {
    N = EntryFuncs.size();
  }
  N = N == 0 ? 1 : N;
}

void SimplifyCallGraph::createSimplifyCallGraph() {
  for (auto &NodePair : CG) {
    CallGraphNode *CGNode = NodePair.second.get();
    Function *F = CGNode->getFunction();
    if (!F || F->isDeclaration())
      continue;

    SimplifyCallGraphNode *SCGNode = getOrInsertFunction(F);
    // deal with indirect call
    if (F->hasAddressTaken()) {
      for (auto *User : F->users()) {
        Instruction *CallInst = nullptr;
        if (auto *Inst = dyn_cast<Instruction>(User)) {
          CallInst = Inst;
        } else if (auto *CE = dyn_cast<ConstantExpr>(User)) {
          for (auto *CEUser : CE->users()) {
            if (auto *CEInst = dyn_cast<Instruction>(CEUser)) {
              CallInst = CEInst;
              break;
            }
          }
        }
        if (CallInst) {
          auto ParentFunc = CallInst->getFunction();
          if (ParentFunc && ParentFunc != F) {
            SimplifyCallGraphNode *ParentSCGNode = getOrInsertFunction(ParentFunc);
            ParentSCGNode->addCalledFunction(SCGNode);
          }
        }
      }
    }
    for (const auto &CGNodeItem : *CGNode) {
      Function *Called = CGNodeItem.second->getFunction();
      if (!Called) {
        // deal with alias
        auto *CallInst = cast<CallBase>(*CGNodeItem.first);
        if (CallInst) {
          llvm::Value *CalledVal = CallInst->getCalledOperand();
          if (llvm::isa<llvm::GlobalAlias>(CalledVal)) {
            AliasesFuncs.insert(F);
          }
        }
      }
      if (!Called || Called->isDeclaration() ||
          (LargeFuncs.find(Called) != LargeFuncs.end() && ( (HotFuncs.find(Called) == HotFuncs.end()) || !SplitBasedHotFuncs) &&
           !AliasesFuncs.count(Called)))
        continue;
      SCGNode->addCalledFunction(getOrInsertFunction(Called));
    }
  }

  if (enablePrintSimplifyCallGraph)
    print();
}

void SimplifyCallGraph::print() {
  {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto &SCGItem : FunctionMap) {
      LLVM_DEBUG(dbgs() << "Call graph node for function: '"
                        << SCGItem.first->getName() << "' #uses="
                        << SCGItem.second->getNumReferences() << "\n");

      for (const auto &callee : *SCGItem.second) {
        LLVM_DEBUG(dbgs() <<"          Calls function : '"
                          << callee->getFunction()->getName() << " '\n");
      }
    }
  }
}

SimplifyCallGraphNode *
SimplifyCallGraph::getOrInsertFunction(const Function *F) {
  auto &SCGN = FunctionMap[F];
  if (SCGN)
    return SCGN.get();

  SCGN =
      std::make_unique<SimplifyCallGraphNode>(this, const_cast<Function *>(F));
  return SCGN.get();
}

bool SimplifyCallGraphNode::dfsSimplifyCallGraph(
    SimplifyCallGraphNode *SCGNode,
    DenseMap<SimplifyCallGraphNode *, bool> &visited, int CurDepth) {
  if (CurDepth > SplitCGDepthThreshold)
    return true;

  if (SCGNode->size() == 0)
    return false;

  bool IsOverThreshold = false;
  visited[SCGNode] = true;
  for (auto *CalledNode : *SCGNode) {
    if (visited.find(CalledNode) != visited.end() &&
        visited[CalledNode] == true)
      continue;

    IsOverThreshold |= dfsSimplifyCallGraph(CalledNode, visited, CurDepth + 1);
    if (IsOverThreshold)
      break;
  }
  visited[SCGNode] = false;
  return IsOverThreshold;
}

bool SimplifyCallGraphNode::CheckCallDepth() {
  DenseMap<SimplifyCallGraphNode *, bool> visited;
  return dfsSimplifyCallGraph(this, visited, 0);
}

/// Reconstruct the analysis results and build the disjoint clusters based on inline cost model.
InlineClusterEstimation::InlineClusterEstimation(Module &M, CallGraph &CG, TargetMachine *TM)
    : M(M), CG(CG), TM(TM) {
  Triple TargetTriple(M.getTargetTriple());
  TLII = std::make_unique<TargetLibraryInfoImpl>(TargetTriple);
  TLI = std::make_unique<TargetLibraryInfo>(*TLII);

  GetTTI = [this](Function &F) -> TargetTransformInfo & {
    auto &TTI = TTIs[&F];
    if (!TTI) {
      TTI = std::make_unique<TargetTransformInfo>(this->TM->getTargetTransformInfo(F));
    }
    return *TTI;
  };
  GetAC = [this](Function &F) -> AssumptionCache & {
    auto &AC = ACs[&F];
    if (!AC) {
      AC = std::make_unique<AssumptionCache>(F);
    }
    return *AC;
  };
  GetTLI = [this](Function &F) -> const TargetLibraryInfo & {
    return *TLI;
  };

  addTransitiveCallToClusters();
}

bool InlineClusterEstimation::fromSameCluster(const Function *A, const Function *B){
  return findFromClusters(A) == findFromClusters(B);
}

void InlineClusterEstimation::addTransitiveCallToClusters() {
  auto isInlineViable = [this](const Function *Caller, const Function *Callee) -> bool {
    Function *A = const_cast<Function *>(Caller);
    Function *B = const_cast<Function *>(Callee);
    for (Instruction &I : instructions(A)) {
      if (CallBase *CB = dyn_cast<CallBase>(&I))
        if (CB->getCalledFunction() == B) {
          InlineCost IC = getInlineCost(*CB, B, getInlineParams(), GetTTI(*B), GetAC, GetTLI);
          return IC.isAlways() || (!IC.isNever() && IC.getCost() < IC.getThreshold());
        }
    }
    return false;
  };

  for (auto &F : M.functions())
    if (!F.isDeclaration())
      insertToCluster(&F);
  
  for (auto &NodePair : CG) {
    CallGraphNode *CGNode = NodePair.second.get();
    const Function *Caller = CGNode->getFunction();
    if (!Caller || Caller->isDeclaration())
      continue;

    for (const auto &CGNodeItem : *CGNode) {
      const Function *Callee = CGNodeItem.second->getFunction();
      if (!Callee || Caller->isDeclaration())
        continue;

      if (isInlineViable(Caller, Callee))
        unite(Caller, Callee);
    }
  }
}

void InlineClusterEstimation::insertToCluster(const Function *A) {
  if (ClusterRoot.find(A) == ClusterRoot.end()) {
    ClusterRoot[A] = A;
    ClusterRank[A] = 0;
  }
}

const Function *InlineClusterEstimation::findFromClusters(const Function *A) {
  insertToCluster(A);
  if (ClusterRoot[A] == A)
    return A;
  return ClusterRoot[A] = findFromClusters(ClusterRoot[A]);
}

void InlineClusterEstimation::unite(const Function *A, const Function *B) {
  const Function *RootA = findFromClusters(A);
  const Function *RootB = findFromClusters(B);
  if (RootA != RootB) {
    if (ClusterRank[RootA] < ClusterRank[RootB])
      std::swap(RootA, RootB);
    if (ClusterRank[RootA] == ClusterRank[RootB])
      ++ClusterRank[RootA];
    ClusterRoot[RootB] = RootA;
  }
}
