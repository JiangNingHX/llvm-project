#ifndef LLVM_TRANSFORMS_UTILS_SPLITMODULECG_H
#define LLVM_TRANSFORMS_UTILS_SPLITMODULECG_H

#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Support/InstructionCost.h"
#include "llvm/Support/ThreadPool.h"
#include <memory>

namespace llvm {

class SimplifyCallGraph;
class SimplifyCallGraphNode;

using CostType = InstructionCost::CostType;

class SimplifyCallGraph {
  using FunctionMapTy =
      std::map<const Function *, std::unique_ptr<SimplifyCallGraphNode>>;

  /// A map from \c Function* to \c SimplifyCallGraphNode*.
  FunctionMapTy FunctionMap;

public:
  explicit SimplifyCallGraph (CallGraph &CG,
                              DenseSet<const Function *> &LargeFuncs,
                              DenseSet<const Function *> &HotFuncs,
                              DenseSet<const Function *> &AliasesFuncs)
         : CG(CG), LargeFuncs(LargeFuncs), HotFuncs(HotFuncs), AliasesFuncs(AliasesFuncs) {
    createSimplifyCallGraph();
  }
  ~SimplifyCallGraph() {};

  using iterator = FunctionMapTy::iterator;
  using const_iterator = FunctionMapTy::const_iterator;

  /// Returns the module the call graph corresponds to.
  inline iterator begin() { return FunctionMap.begin(); }
  inline iterator end() { return FunctionMap.end(); }
  inline const_iterator begin() const { return FunctionMap.begin(); }
  inline const_iterator end() const { return FunctionMap.end(); }

  /// Returns the call graph node for the provided function.
  inline const SimplifyCallGraphNode *operator[](const Function *F) const {
    const_iterator I = FunctionMap.find(F);
    assert(I != FunctionMap.end() && "Function not in callgraph!");
    return I->second.get();
  }

  /// Returns the call graph node for the provided function.
  inline SimplifyCallGraphNode *operator[](const Function *F) {
    const_iterator I = FunctionMap.find(F);
    assert(I != FunctionMap.end() && "Function not in callgraph!");
    return I->second.get(); 
  }

  /// Returns the call graph node for the provided function.
  inline const SimplifyCallGraphNode *at(const Function *F) const {
    const_iterator I = FunctionMap.find(F);
    assert(I != FunctionMap.end() && "Function not in callgraph!");
    return I->second.get();
  }

  /// Returns the call graph node for the provided function.
  inline SimplifyCallGraphNode *at(const Function *F) {
    const_iterator I = FunctionMap.find(F);
    assert(I != FunctionMap.end() && "Function not in callgraph!");
    return I->second.get();
  }

  void createSimplifyCallGraph();
  void print();
  SimplifyCallGraphNode *getOrInsertFunction(const Function *F);

private:
  CallGraph &CG;
  DenseSet<const Function *> &LargeFuncs;
  DenseSet<const Function *> &HotFuncs;
  DenseSet<const Function *> &AliasesFuncs;
};

class SimplifyCallGraphNode {
public:
  using CalledFunctionsSet = DenseSet<SimplifyCallGraphNode *>;
  inline SimplifyCallGraphNode(SimplifyCallGraph *SCG, Function *F)
      : SCG(SCG), F(F) {}

  SimplifyCallGraphNode(const SimplifyCallGraphNode &) = delete;
  SimplifyCallGraphNode &operator=(const SimplifyCallGraphNode &) = delete;

  ~SimplifyCallGraphNode() {}

  Function *getFunction() const { return F; }

  unsigned getNumReferences() const { return NumReferences; }

  using iterator = DenseSet<SimplifyCallGraphNode *>::iterator;
  using const_iterator = DenseSet<SimplifyCallGraphNode *>::const_iterator;

  inline iterator begin() { return CalledFunctions.begin(); }
  inline iterator end() { return CalledFunctions.end(); }
  inline const_iterator begin() const { return CalledFunctions.begin(); }
  inline const_iterator end() const { return CalledFunctions.end(); }
  inline bool empty() const { return CalledFunctions.empty(); }
  inline unsigned size() const { return (unsigned)CalledFunctions.size(); }

  bool dfsSimplifyCallGraph(SimplifyCallGraphNode *SCGNode,
                            DenseMap<SimplifyCallGraphNode *, bool> &visited,
                            int CurDepth);
  bool CheckCallDepth();

  void addCalledFunction(SimplifyCallGraphNode *Called) {
    auto [It, Inserted] = CalledFunctions.insert(Called);
    if (Inserted)
      Called->AddRef();
  }

  void removeCalledFunction(SimplifyCallGraphNode *Called) {
    auto NumRemoved = CalledFunctions.erase(Called);
    if (NumRemoved > 0)
      Called->DropRef();
  }

private:
  friend class SimplifyCallGraph;

  SimplifyCallGraph *SCG;
  Function *F;

  DenseSet<SimplifyCallGraphNode *> CalledFunctions;
  unsigned NumReferences = 0;

  void DropRef() { --NumReferences; }
  void AddRef() { ++NumReferences; }
};

/// Helper class to generate disjoint clusters based on the inline viability
/// estimation for the callgraph so avoiding to lose inline oppotunities by
/// doing callgraph split by keeping functions in each cluster in one partition.
class InlineClusterEstimation {
public:
  InlineClusterEstimation(Module &M, CallGraph &CG, TargetMachine *TM);
  bool fromSameCluster(const Function *A, const Function *B);
private:
  Module &M;
  CallGraph &CG;
  TargetMachine *TM;

  /// Reconstruct and cache necessary analysis results.
  DenseMap<Function *, std::unique_ptr<AssumptionCache>> ACs;
  DenseMap<Function *, std::unique_ptr<TargetTransformInfo>> TTIs;
  std::unique_ptr<TargetLibraryInfoImpl> TLII;
  std::unique_ptr<TargetLibraryInfo> TLI;
  std::function<AssumptionCache &(Function &)> GetAC;
  std::function<const TargetLibraryInfo &(Function &)> GetTLI;
  std::function<TargetTransformInfo &(Function &)> GetTTI;
  DenseMap<const Function *, const Function *> ClusterRoot;

  /// Internal methods to build the clusters from callgraph nodes.
  DenseMap<const Function *, unsigned> ClusterRank;
  void addTransitiveCallToClusters();
  void insertToCluster(const Function *A);
  const Function *findFromClusters(const Function *A);
  void unite(const Function *A, const Function *B);
};

/// Adds the functions that \p F may call to \p Fns, then recurses into each
/// callee until all reachable functions have been gathered.
///
/// \param CG Call graph for \p F's module.
/// \param F Current function to look at.
/// \param Fns[out] Resulting list of functions.
static void addAllDependencies(SimplifyCallGraph &SCG, const Function &F,
                               DenseSet<const Function *> &Fns,
                               DenseMap<const Function *, bool> &externalFunction) {
  assert(!F.isDeclaration());

  SmallVector<const Function *> WorkList({&F});

  while (!WorkList.empty()) {
    const auto &CurFn = *WorkList.pop_back_val();
    assert(!CurFn.isDeclaration());

    // Scan for an indirect call. If such a call is found, we have to
    // conservatively assume this can call all non-entrypoint functions in the module.
    for (auto &SCGNode : *SCG.at(&CurFn)) {
      auto *Callee = SCGNode->getFunction();
      if (!Callee || Callee->isDeclaration())
        continue;
      if (Callee != &F)
      {
        auto [It, Inserted] = Fns.insert(Callee);
        if (Inserted)
          WorkList.push_back(Callee);
      }
    }
  }
}

struct FunctionWithDependencies {
  FunctionWithDependencies(SimplifyCallGraph &SCG,
                           const DenseMap<const Function *, CostType> &FnCosts,
                           const Function *F,
                           const DenseSet<const Function *> &AliasesFuncs,
                           DenseMap<const Function *, bool> &externalFunction,
			                     const DenseSet<const Function *> &IfuncFuncs,
                           const DenseSet<const Function *> &ComdatFuncs)
      : F(F) {
    addAllDependencies(SCG, *F, Dependencies, externalFunction);
    if (AliasesFuncs.count(F))
      HasAliasesCall = true;
    // If the function is an ifunc resolver, it must stay in the every partition.
    if (IfuncFuncs.count(F))
      HasIfuncResolver = true;
    // If the function is in a comdat, it must stay in the first partition.
    if (ComdatFuncs.count(F))
      HasComdatMember = true;

    TotalCost = FnCosts.at(F);
    for (const auto *Dep : Dependencies) {
      TotalCost += FnCosts.lookup(Dep);
      if (AliasesFuncs.count(Dep))
        HasAliasesCall = true;
      if (IfuncFuncs.count(Dep))
        HasIfuncResolver = true;
      if (ComdatFuncs.count(Dep))
        HasComdatMember = true;
    }
  }

  const Function *F = nullptr;
  DenseSet<const Function *> Dependencies;
  /// Whether \p F or any of its \ref Dependencies contains an indirect call.
  bool HasAliasesCall = false;
  bool HasIfuncResolver = false;
  bool HasComdatMember = false;
  
  CostType TotalCost = 0;
  int SplitedLayer = 0;

  /// \returns true if this function and its dependencies can be considered
  /// large according to \p Threshold.
  bool isLarge(CostType Threshold) const {
    return TotalCost > Threshold && !Dependencies.empty();
  }
};

/// Splits the module M into N linkable partitions. The function ModuleCallback
/// is called N times passing each individual partition as the MPart argument.
class SplitModuleCG {
public:
  using ModuleCreationCallback =
      function_ref<void(std::unique_ptr<Module> MPart)>;
  SplitModuleCG(Module &M, unsigned LimitPartition = 0, ThreadPool *PartitionThreadPool = nullptr);
  void SplitModule(TargetMachine *TM, ModuleCreationCallback ModuleCallback,
      bool PreserveLocals);

  unsigned getPartitionNum() { return N; }
  StringSet<> &getOriginalExternals() { return OriginalExternals; }
  StringMap<std::string> &getPromotedRenames() { return PromotedRenames; }

private:
  unsigned N;
  Module &M;
  CallGraph CG;
  std::unique_ptr<SimplifyCallGraph> SCG;
  std::unique_ptr<InlineClusterEstimation> IPE = nullptr;
  CostType ModuleCost;
  DenseSet<const Function *> EntryFuncs;
  DenseSet<const Function *> LargeFuncs;
  DenseSet<const Function *> HotFuncs;
  DenseSet<const Function *> DependenciesForMain;
  DenseSet<const Function *> AliasesFuncs;
  DenseSet<const Function *> IfuncFuncs;
  DenseSet<const Function *> ComdatFuncs;
  StringSet<> OriginalExternals;
  StringMap<std::string> PromotedRenames;
  DenseMap<const Function *, bool> externalFunction;
  DenseMap<const Function *, CostType> FuncsCosts;
  ThreadPool *PartitionThreadPool;
  DenseMap<const Comdat *, DenseSet<const GlobalValue *>> ComdatMembers;

  void calculateEntryFuncs();
  void calculateFunctionCosts();
  void calculateComdatMembers();
  void getLargeFunction();
  void getHotFunction();
  void getAliasFunction();
  void getIfuncFunction();
  void splitLargeCG(SmallVector<llvm::FunctionWithDependencies> &WorkList);
  void UpdateFWDInfo(llvm::FunctionWithDependencies &FWD);
  bool shouldCloneFunction(const Function *Fn);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_SPLITMODULECG_H
