//===- DWPUtil.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DWPUtil.h"
#include "Error.h"
#include "llvm/ADT/AddressRanges.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugAbbrev.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLoc.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugRnglists.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFUnitIndex.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"
#include <mutex>
#include <system_error>

using namespace llvm;
using namespace llvm::object;

namespace llvm {
namespace dwarfutil {

static std::optional<uint64_t> getUnitDWOId(const DWARFDie &Die);

static std::string getUnitName(DWARFUnit &Unit) {
  DWARFDie UnitDie = Unit.getUnitDIE(/*ExtractUnitDIEOnly=*/true);
  if (!UnitDie)
    return "";

  return dwarf::toString(UnitDie.find(dwarf::DW_AT_name), "");
}

static std::string getUnitDWOName(DWARFUnit &Unit) {
  DWARFDie UnitDie = Unit.getUnitDIE(/*ExtractUnitDIEOnly=*/true);
  if (!UnitDie)
    return "";

  return dwarf::toString(
      UnitDie.find({dwarf::DW_AT_dwo_name, dwarf::DW_AT_GNU_dwo_name}), "");
}

static bool isCompanionMainUnit(DWARFUnit &Unit) {
  switch (Unit.getUnitType()) {
  case dwarf::DW_UT_compile:
  case dwarf::DW_UT_skeleton:
    break;
  default:
    return false;
  }

  return Unit.getDWOId().has_value() && !getUnitDWOName(Unit).empty();
}

static DenseMap<uint64_t, SmallVector<DWARFUnit *, 1>>
indexCompanionMainUnits(DWARFContext &Context) {
  DenseMap<uint64_t, SmallVector<DWARFUnit *, 1>> UnitsByDWOId;
  for (const std::unique_ptr<DWARFUnit> &CU : Context.compile_units()) {
    if (!isCompanionMainUnit(*CU))
      continue;
    UnitsByDWOId[*CU->getDWOId()].push_back(CU.get());
  }
  return UnitsByDWOId;
}

static Expected<OwningBinary<Binary>> openBinary(StringRef FileName) {
  Expected<OwningBinary<Binary>> BinOrErr = createBinary(FileName);
  if (!BinOrErr)
    return createFileError(FileName, BinOrErr.takeError());

  if (!BinOrErr->getBinary()->isObject())
    return createFileError(FileName, createError("unsupported input file"));

  return std::move(*BinOrErr);
}

static std::optional<StringRef> getDWPSectionName(DWARFSectionKind Kind) {
  switch (Kind) {
  case DW_SECT_INFO:
    return ".debug_info.dwo";
  case DW_SECT_ABBREV:
    return ".debug_abbrev.dwo";
  case DW_SECT_EXT_LOC:
    return ".debug_loc.dwo";
  case DW_SECT_STR_OFFSETS:
    return ".debug_str_offsets.dwo";
  case DW_SECT_RNGLISTS:
    return ".debug_rnglists.dwo";
  default:
    return std::nullopt;
  }
}

static std::string getDWPSectionNameForError(DWARFSectionKind Kind) {
  if (std::optional<StringRef> Name = getDWPSectionName(Kind))
    return std::string(*Name);
  return std::string(toString(Kind));
}

struct RetainedDWPRewriteUnitAnalysis {
  uint64_t DWOId = 0;
  RetainedDWPUnitInfo RetainedUnit;
  DenseSet<uint64_t> RetainedOffsets;
  DenseMap<uint64_t, SmallVector<uint64_t, 4>> RetainedChildrenByOffset;
  DenseMap<uint64_t, uint64_t> StringIndexRemap;
  DenseMap<uint64_t, uint64_t> DirectStringOffsetRemap;
  DenseMap<uint64_t, uint64_t> LocationOffsetRemap;
  DenseMap<uint64_t, uint64_t> RangeListOffsetRemap;
  SmallVector<uint64_t, 8> IndexedStringOffsetsInNewOrder;
  SmallVector<uint64_t, 8> DirectStringOffsetsInOrder;
};

struct RetainedDWPRewriteUnitContents {
  uint64_t DWOId = 0;
  std::string Info;
  std::string Abbrev;
  std::string StringOffsets;
  std::string Loc;
  std::string Rnglists;
};

struct RetainedDWPRewriteUnitDescriptor {
  uint64_t DWOId = 0;
  uint64_t UnitOffset = 0;
  size_t PackageUnitIndex = 0;
  size_t SortedUnitIndex = 0;
};

struct RetainedDWPCollectUnitDescriptor {
  uint64_t DWOId = 0;
  uint64_t UnitOffset = 0;
  size_t PackageUnitIndex = 0;
};

struct LoadedRetainedDWPUnit {
  explicit LoadedRetainedDWPUnit(OwningBinary<Binary> DWPBinary)
      : DWPBinary(std::move(DWPBinary)) {}

  OwningBinary<Binary> DWPBinary;
  std::unique_ptr<DWARFContext> DWPContext;
  DenseMap<DWARFSectionKind, StringRef> SectionContents;
  StringRef DebugStrContents;
  DWARFDie UnitDIE;
};

static Error collectDWPSectionContents(
    const ObjectFile &DWPObject, DenseMap<DWARFSectionKind, StringRef> &SectionContents,
    StringRef &DebugStrContents,
    DenseMap<DWARFSectionKind, uint64_t> *SectionSizes = nullptr) {
  for (const object::SectionRef &Section : DWPObject.sections()) {
    Expected<StringRef> NameOrErr = Section.getName();
    if (!NameOrErr)
      return NameOrErr.takeError();
    Expected<StringRef> ContentsOrErr = Section.getContents();
    if (!ContentsOrErr)
      return ContentsOrErr.takeError();
    if (*NameOrErr == ".debug_str.dwo")
      DebugStrContents = *ContentsOrErr;

    for (DWARFSectionKind Kind : {DW_SECT_INFO, DW_SECT_ABBREV,
                                  DW_SECT_STR_OFFSETS, DW_SECT_RNGLISTS,
                                  DW_SECT_EXT_LOC}) {
      if (std::optional<StringRef> ExpectedName = getDWPSectionName(Kind);
          ExpectedName && *NameOrErr == *ExpectedName) {
        if (SectionSizes)
          (*SectionSizes)[Kind] = Section.getSize();
        SectionContents[Kind] = *ContentsOrErr;
        break;
      }
    }
  }
  return Error::success();
}

static Expected<DWARFUnit *> getIndexedDWPCompileUnit(DWARFContext &DWPContext,
                                                      uint64_t DWOId,
                                                      uint64_t UnitOffset,
                                                      size_t PackageUnitIndex) {
  size_t PackageIndex = 0;
  for (const std::unique_ptr<DWARFUnit> &CU : DWPContext.dwo_compile_units()) {
    if (PackageIndex++ != PackageUnitIndex)
      continue;

    std::optional<uint64_t> CandidateId = CU->getDWOId();
    DWARFDie UnitDIE = CU->getUnitDIE(/*ExtractUnitDIEOnly=*/false);
    if (!CandidateId || *CandidateId != DWOId || !UnitDIE ||
        UnitDIE.getOffset() != UnitOffset)
      return createStringError(
          std::errc::invalid_argument,
          formatv("retained package unit descriptor mismatch for DWO_id {0:x16}",
                  DWOId)
              .str()
              .c_str());

    return CU.get();
  }

  return createStringError(
      std::errc::invalid_argument,
      formatv("package unit index {0} is out of bounds for retained DWO_id "
              "{1:x16}",
              PackageUnitIndex, DWOId)
          .str()
          .c_str());
}

static Expected<DWARFUnit *>
getRetainedDWPCompileUnit(DWARFContext &DWPContext,
                          const RetainedDWPRewriteUnitDescriptor &Descriptor) {
  return getIndexedDWPCompileUnit(DWPContext, Descriptor.DWOId,
                                  Descriptor.UnitOffset,
                                  Descriptor.PackageUnitIndex);
}

static Expected<LoadedRetainedDWPUnit>
loadIndexedDWPUnit(StringRef DWPFileName, uint64_t DWOId, uint64_t UnitOffset,
                   size_t PackageUnitIndex) {
  Expected<OwningBinary<Binary>> DWPBinOrErr = openBinary(DWPFileName);
  if (!DWPBinOrErr)
    return DWPBinOrErr.takeError();

  LoadedRetainedDWPUnit Loaded(std::move(*DWPBinOrErr));
  auto *DWPObject = cast<ObjectFile>(Loaded.DWPBinary.getBinary());
  Loaded.DWPContext = DWARFContext::create(*DWPObject);
  if (!Loaded.DWPContext->isDWP())
    return createStringError(
        std::errc::invalid_argument,
        formatv("file '{0}' is not a DWARF package file", DWPFileName)
            .str()
            .c_str());

  if (Error Err = collectDWPSectionContents(*DWPObject, Loaded.SectionContents,
                                            Loaded.DebugStrContents))
    return std::move(Err);

  Expected<DWARFUnit *> CUOrErr = getIndexedDWPCompileUnit(
      *Loaded.DWPContext, DWOId, UnitOffset, PackageUnitIndex);
  if (!CUOrErr)
    return CUOrErr.takeError();

  Loaded.UnitDIE = (*CUOrErr)->getUnitDIE(/*ExtractUnitDIEOnly=*/false);
  if (!Loaded.UnitDIE)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to load retained package unit DIE for DWO_id {0:x16}",
                DWOId)
            .str()
            .c_str());

  return Loaded;
}

static Expected<LoadedRetainedDWPUnit>
loadRetainedDWPUnitForRewrite(StringRef DWPFileName,
                              const RetainedDWPRewriteUnitDescriptor &Descriptor) {
  return loadIndexedDWPUnit(DWPFileName, Descriptor.DWOId, Descriptor.UnitOffset,
                            Descriptor.PackageUnitIndex);
}

static Expected<SmallVector<RetainedDWPRewriteUnitDescriptor, 4>>
buildRetainedDWPRewriteUnitDescriptors(
    DWARFContext &DWPContext, ArrayRef<RetainedDWPPackageUnitInfo> RetainedUnits) {
  DenseMap<uint64_t, size_t> SortedUnitIndicesByOffset;
  for (size_t I = 0; I != RetainedUnits.size(); ++I)
    SortedUnitIndicesByOffset.try_emplace(RetainedUnits[I].Unit.UnitOffset, I);

  SmallVector<RetainedDWPRewriteUnitDescriptor, 4> Descriptors;
  Descriptors.reserve(RetainedUnits.size());
  size_t PackageIndex = 0;
  for (const std::unique_ptr<DWARFUnit> &CU : DWPContext.dwo_compile_units()) {
    DWARFDie UnitDIE = CU->getUnitDIE(/*ExtractUnitDIEOnly=*/false);
    if (!UnitDIE) {
      ++PackageIndex;
      continue;
    }

    auto It = SortedUnitIndicesByOffset.find(UnitDIE.getOffset());
    if (It == SortedUnitIndicesByOffset.end()) {
      ++PackageIndex;
      continue;
    }

    const RetainedDWPPackageUnitInfo &Unit = RetainedUnits[It->second];
    std::optional<uint64_t> DWOId = CU->getDWOId();
    if (!DWOId || *DWOId != Unit.Unit.DWOId)
      return createStringError(
          std::errc::invalid_argument,
          formatv("retained package unit mismatch at offset 0x{0:x}",
                  Unit.Unit.UnitOffset)
              .str()
              .c_str());

    RetainedDWPRewriteUnitDescriptor &Descriptor = Descriptors.emplace_back();
    Descriptor.DWOId = *DWOId;
    Descriptor.UnitOffset = UnitDIE.getOffset();
    Descriptor.PackageUnitIndex = PackageIndex;
    Descriptor.SortedUnitIndex = It->second;
    ++PackageIndex;
  }

  if (Descriptors.size() != RetainedUnits.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to reconstruct retained package order: expected {0} "
                "units, found {1}",
                RetainedUnits.size(), Descriptors.size())
            .str()
            .c_str());

  return Descriptors;
}

static Expected<SmallVector<RetainedDWPCollectUnitDescriptor, 4>>
buildRetainedDWPCollectUnitDescriptors(DWARFContext &DWPContext,
                                       const DWPLinkMap &LinkMap) {
  SmallVector<RetainedDWPCollectUnitDescriptor, 4> Descriptors;
  Descriptors.reserve(LinkMap.LinkedUnits.size());
  size_t PackageIndex = 0;
  for (const std::unique_ptr<DWARFUnit> &CU : DWPContext.dwo_compile_units()) {
    std::optional<uint64_t> DWOId = CU->getDWOId();
    DWARFDie UnitDIE = CU->getUnitDIE(/*ExtractUnitDIEOnly=*/false);
    if (!DWOId || !LinkMap.findLinkedUnit(*DWOId)) {
      ++PackageIndex;
      continue;
    }
    if (!UnitDIE)
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to load retained package unit DIE for DWO_id {0:x16}",
                  *DWOId)
              .str()
              .c_str());

    RetainedDWPCollectUnitDescriptor &Descriptor = Descriptors.emplace_back();
    Descriptor.DWOId = *DWOId;
    Descriptor.UnitOffset = UnitDIE.getOffset();
    Descriptor.PackageUnitIndex = PackageIndex;
    ++PackageIndex;
  }

  if (Descriptors.size() != LinkMap.LinkedUnits.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to reconstruct retained package unit descriptors: "
                "expected {0}, found {1}",
                LinkMap.LinkedUnits.size(), Descriptors.size())
            .str()
            .c_str());

  return Descriptors;
}

static std::optional<StringRef>
getRewrittenUnitContents(DWARFSectionKind Kind,
                         const RetainedDWPRewriteUnitContents &Contents) {
  switch (Kind) {
  case DW_SECT_INFO:
    return StringRef(Contents.Info);
  case DW_SECT_ABBREV:
    return StringRef(Contents.Abbrev);
  case DW_SECT_STR_OFFSETS:
    return StringRef(Contents.StringOffsets);
  case DW_SECT_RNGLISTS:
    return StringRef(Contents.Rnglists);
  case DW_SECT_EXT_LOC:
    return StringRef(Contents.Loc);
  default:
    return std::nullopt;
  }
}

static Error validateRewrittenUnitContentsSize(DWARFSectionKind Kind,
                                               uint64_t DWOId,
                                               uint64_t ExpectedSize,
                                               StringRef Contents) {
  if (Contents.size() == ExpectedSize)
    return Error::success();

  return createStringError(
      std::errc::invalid_argument,
      formatv("rewritten {0} size mismatch for DWO_id {1:x16}: expected "
              "0x{2:x}, got 0x{3:x}",
              getDWPSectionNameForError(Kind), DWOId, ExpectedSize,
              Contents.size())
          .str()
          .c_str());
}

static bool shouldRewriteRetainedReferenceAttr(dwarf::Attribute Attr) {
  switch (Attr) {
  case dwarf::DW_AT_type:
  case dwarf::DW_AT_specification:
  case dwarf::DW_AT_abstract_origin:
  case dwarf::DW_AT_object_pointer:
  case dwarf::DW_AT_containing_type:
    return true;
  default:
    return false;
  }
}

static uint64_t getUnitRelativeDIEOffset(const DWARFDie &Die) {
  return Die.getOffset() - Die.getDwarfUnit()->getOffset();
}

static DWARFDie getDIEForUnitRelativeOffset(const DWARFDie &UnitDIE,
                                            uint64_t UnitRelativeOffset) {
  return UnitDIE.getDwarfUnit()->getDIEForOffset(
      UnitDIE.getDwarfUnit()->getOffset() + UnitRelativeOffset);
}

static Expected<uint64_t> getPackageContributionBase(const DWPLinkMap &LinkMap,
                                                     const DWARFDie &UnitDIE,
                                                     DWARFSectionKind Kind) {
  std::optional<uint64_t> DWOId = getUnitDWOId(UnitDIE);
  if (!DWOId)
    return createStringError(std::errc::invalid_argument,
                             "unit DIE does not belong to a split unit");

  const PackageUnitInfo *Package = LinkMap.findPackageUnit(*DWOId);
  if (!Package)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to find package unit for DWO_id {0:x16}", *DWOId)
            .str()
            .c_str());

  for (const PackageUnitInfo::SectionContributionInfo &Contribution :
       Package->Contributions)
    if (Contribution.Kind == Kind)
      return Contribution.Offset;

  return createStringError(
      std::errc::invalid_argument,
      formatv("unable to find package contribution {0} for DWO_id {1:x16}",
              toString(Kind), *DWOId)
          .str()
          .c_str());
}

static Expected<PackageUnitInfo::SectionContributionInfo>
getPackageContribution(const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE,
                       DWARFSectionKind Kind) {
  std::optional<uint64_t> DWOId = getUnitDWOId(UnitDIE);
  if (!DWOId)
    return createStringError(std::errc::invalid_argument,
                             "unit DIE does not belong to a split unit");

  const PackageUnitInfo *Package = LinkMap.findPackageUnit(*DWOId);
  if (!Package)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to find package unit for DWO_id {0:x16}", *DWOId)
            .str()
            .c_str());

  for (const PackageUnitInfo::SectionContributionInfo &Contribution :
       Package->Contributions)
    if (Contribution.Kind == Kind)
      return Contribution;

  return createStringError(
      std::errc::invalid_argument,
      formatv("unable to find package contribution {0} for DWO_id {1:x16}",
              toString(Kind), *DWOId)
          .str()
          .c_str());
}

static void appendFixedUnsigned(std::string &Out, uint64_t Value,
                                unsigned ByteSize, bool IsLittleEndian) {
  switch (ByteSize) {
  case 1:
    Out.push_back(static_cast<char>(Value));
    return;
  case 2: {
    char Buffer[2];
    if (IsLittleEndian)
      support::endian::write16le(Buffer, static_cast<uint16_t>(Value));
    else
      support::endian::write16be(Buffer, static_cast<uint16_t>(Value));
    Out.append(Buffer, sizeof(Buffer));
    return;
  }
  case 4: {
    char Buffer[4];
    if (IsLittleEndian)
      support::endian::write32le(Buffer, static_cast<uint32_t>(Value));
    else
      support::endian::write32be(Buffer, static_cast<uint32_t>(Value));
    Out.append(Buffer, sizeof(Buffer));
    return;
  }
  case 8: {
    char Buffer[8];
    if (IsLittleEndian)
      support::endian::write64le(Buffer, Value);
    else
      support::endian::write64be(Buffer, Value);
    Out.append(Buffer, sizeof(Buffer));
    return;
  }
  default:
    llvm_unreachable("unexpected fixed-width write");
  }
}

static bool isStringIndexForm(dwarf::Form Form) {
  switch (Form) {
  case dwarf::DW_FORM_GNU_str_index:
  case dwarf::DW_FORM_strx:
  case dwarf::DW_FORM_strx1:
  case dwarf::DW_FORM_strx2:
  case dwarf::DW_FORM_strx3:
  case dwarf::DW_FORM_strx4:
    return true;
  default:
    return false;
  }
}

static bool isDirectStringOffsetForm(dwarf::Form Form) {
  switch (Form) {
  case dwarf::DW_FORM_strp:
    return true;
  default:
    return false;
  }
}

static bool isLocationSectionOffsetAttr(dwarf::Attribute Attr, dwarf::Form Form) {
  return Attr == dwarf::DW_AT_location && Form == dwarf::DW_FORM_sec_offset;
}

static bool isRangesSectionOffsetAttr(dwarf::Attribute Attr, dwarf::Form Form) {
  return Attr == dwarf::DW_AT_ranges && Form == dwarf::DW_FORM_sec_offset;
}

static Error appendEncodedScalarFormValue(std::string &Out, dwarf::Form Form,
                                          uint64_t Value,
                                          const DWARFUnit &Unit) {
  switch (Form) {
  case dwarf::DW_FORM_ref1:
  case dwarf::DW_FORM_strx1:
    appendFixedUnsigned(Out, Value, 1, Unit.isLittleEndian());
    return Error::success();
  case dwarf::DW_FORM_ref2:
  case dwarf::DW_FORM_strx2:
    appendFixedUnsigned(Out, Value, 2, Unit.isLittleEndian());
    return Error::success();
  case dwarf::DW_FORM_ref4:
  case dwarf::DW_FORM_strx4:
    appendFixedUnsigned(Out, Value, 4, Unit.isLittleEndian());
    return Error::success();
  case dwarf::DW_FORM_ref8:
    appendFixedUnsigned(Out, Value, 8, Unit.isLittleEndian());
    return Error::success();
  case dwarf::DW_FORM_strx3:
    appendFixedUnsigned(Out, Value, 3, Unit.isLittleEndian());
    return Error::success();
  case dwarf::DW_FORM_ref_udata:
  case dwarf::DW_FORM_strx:
  case dwarf::DW_FORM_GNU_str_index: {
    raw_string_ostream OS(Out);
    encodeULEB128(Value, OS);
    OS.flush();
    return Error::success();
  }
  default:
    return createStringError(
        std::errc::invalid_argument,
        formatv("unsupported rewritten reference form {0}", dwarf::FormEncodingString(Form))
            .str()
            .c_str());
  }
}

static uint64_t getEncodedScalarFormValueSize(dwarf::Form Form, uint64_t Value,
                                              const DWARFUnit &Unit) {
  switch (Form) {
  case dwarf::DW_FORM_ref1:
  case dwarf::DW_FORM_strx1:
    return 1;
  case dwarf::DW_FORM_ref2:
  case dwarf::DW_FORM_strx2:
    return 2;
  case dwarf::DW_FORM_ref4:
  case dwarf::DW_FORM_strx4:
    return 4;
  case dwarf::DW_FORM_ref8:
    return 8;
  case dwarf::DW_FORM_strx3:
    return 3;
  case dwarf::DW_FORM_ref_udata:
  case dwarf::DW_FORM_strx:
  case dwarf::DW_FORM_GNU_str_index: {
    std::string Buffer;
    raw_string_ostream OS(Buffer);
    encodeULEB128(Value, OS);
    OS.flush();
    return Buffer.size();
  }
  default:
    return 0;
  }
}

static Expected<std::pair<uint64_t, uint64_t>>
getDIEAttributeByteRange(const DWARFDie &Die, uint32_t AttrIndex) {
  const DWARFAbbreviationDeclaration *AbbrevDecl =
      Die.getAbbreviationDeclarationPtr();
  if (!AbbrevDecl)
    return createStringError(std::errc::invalid_argument,
                             "expected a non-null DIE abbreviation");
  DWARFUnit &Unit = *Die.getDwarfUnit();

  uint64_t BeginOffset =
      AbbrevDecl->getAttributeOffsetFromIndex(AttrIndex, Die.getOffset(), Unit);
  uint64_t EndOffset = BeginOffset;
  if (AttrIndex + 1 < AbbrevDecl->getNumAttributes()) {
    EndOffset = AbbrevDecl->getAttributeOffsetFromIndex(AttrIndex + 1,
                                                        Die.getOffset(), Unit);
    return std::pair<uint64_t, uint64_t>(BeginOffset, EndOffset);
  }

  const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
  DWARFFormValue Value = Spec.getFormValue();
  if (!Value.extractValue(Unit.getDebugInfoExtractor(), &EndOffset,
                          Unit.getFormParams(), &Unit.getContext(), &Unit))
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to decode DIE attribute {0} at offset 0x{1:x}",
                dwarf::AttributeString(AbbrevDecl->getAttrByIndex(AttrIndex)),
                BeginOffset)
            .str()
            .c_str());

  return std::pair<uint64_t, uint64_t>(BeginOffset, EndOffset);
}

static Expected<DenseMap<uint64_t, uint64_t>>
buildRetainedStringIndexRemap(const DWARFDie &UnitDIE,
                              const DenseSet<uint64_t> &RetainedOffsets) {
  DenseMap<uint64_t, uint64_t> Remap;
  SmallVector<DWARFDie, 16> Worklist;
  Worklist.push_back(UnitDIE);
  while (!Worklist.empty()) {
    DWARFDie Die = Worklist.pop_back_val();
    if (!Die || Die.isNULL())
      continue;

    const DWARFAbbreviationDeclaration *AbbrevDecl =
        Die.getAbbreviationDeclarationPtr();
    if (AbbrevDecl) {
      for (uint32_t AttrIndex = 0; AttrIndex < AbbrevDecl->getNumAttributes();
           ++AttrIndex) {
        dwarf::Form Form = AbbrevDecl->getFormByIndex(AttrIndex);
        if (!isStringIndexForm(Form))
          continue;

        Expected<std::pair<uint64_t, uint64_t>> RangeOrErr =
            getDIEAttributeByteRange(Die, AttrIndex);
        if (!RangeOrErr)
          return RangeOrErr.takeError();
        const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
        DWARFFormValue Value = Spec.getFormValue();
        uint64_t Offset = RangeOrErr->first;
        if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                                &Offset, Die.getDwarfUnit()->getFormParams(),
                                &Die.getDwarfUnit()->getContext(),
                                Die.getDwarfUnit()))
          return createStringError(
              std::errc::invalid_argument,
              formatv("unable to decode string index form at DIE 0x{0:x}",
                      Die.getOffset())
                  .str()
                  .c_str());

        uint64_t OldIndex = Value.getRawUValue();
        Remap.try_emplace(OldIndex, Remap.size());
      }
    }

    for (DWARFDie Child : Die.children()) {
      if (!RetainedOffsets.contains(getUnitRelativeDIEOffset(Child)))
        continue;
      Worklist.push_back(Child);
    }
  }

  return Remap;
}

static Expected<uint64_t>
readStringOffsetsEntry(const DWARFDie &UnitDIE, StringRef InputStringOffsetContents,
                       uint64_t Index) {
  DWARFUnit &Unit = *UnitDIE.getDwarfUnit();
  const auto &Contribution = Unit.getStringOffsetsTableContribution();
  if (!Contribution)
    return createStringError(std::errc::invalid_argument,
                             "missing string offsets contribution");

  const uint64_t Base = Unit.getStringOffsetsBase();
  const uint8_t ItemSize = Unit.getDwarfStringOffsetsByteSize();
  const uint64_t EntryOffset = Base + Index * ItemSize;
  if (EntryOffset + ItemSize > InputStringOffsetContents.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("string offsets entry {0} exceeds .debug_str_offsets.dwo bounds",
                Index)
            .str()
            .c_str());

  DWARFDataExtractor StrOffsetsData(InputStringOffsetContents,
                                    Unit.isLittleEndian(), 0);
  uint64_t Cursor = EntryOffset;
  if (ItemSize == 4)
    return StrOffsetsData.getU32(&Cursor);
  return StrOffsetsData.getU64(&Cursor);
}

static Expected<SmallVector<uint64_t, 8>>
collectRetainedStringOffsetsInNewIndexOrder(
    const DWARFDie &UnitDIE, StringRef InputStringOffsetContents,
    const DenseMap<uint64_t, uint64_t> &StringIndexRemap) {
  SmallVector<std::pair<uint64_t, uint64_t>, 8> IndexedEntries;
  IndexedEntries.reserve(StringIndexRemap.size());

  for (const auto &It : StringIndexRemap) {
    Expected<uint64_t> StringOffsetOrErr =
        readStringOffsetsEntry(UnitDIE, InputStringOffsetContents, It.first);
    if (!StringOffsetOrErr)
      return StringOffsetOrErr.takeError();
    IndexedEntries.emplace_back(It.second, *StringOffsetOrErr);
  }

  llvm::sort(IndexedEntries);
  SmallVector<uint64_t, 8> StringOffsetsInOrder;
  StringOffsetsInOrder.reserve(IndexedEntries.size());
  for (const auto &[NewIndex, OldStringOffset] : IndexedEntries) {
    (void)NewIndex;
    StringOffsetsInOrder.push_back(OldStringOffset);
  }
  return StringOffsetsInOrder;
}

static Expected<StringRef> getDebugStrEntry(StringRef InputStringContents,
                                            uint64_t Offset) {
  if (Offset >= InputStringContents.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("string offset 0x{0:x} exceeds .debug_str.dwo bounds", Offset)
            .str()
            .c_str());

  size_t End = InputStringContents.find('\0', Offset);
  if (End == StringRef::npos)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unterminated string at .debug_str.dwo offset 0x{0:x}", Offset)
            .str()
            .c_str());
  return InputStringContents.slice(Offset, End);
}

static Expected<DenseMap<uint64_t, uint64_t>>
buildRetainedLocationOffsetRemap(const DWPLinkMap &LinkMap,
                                 const RetainedDWPUnitInfo &RetainedUnit,
                                 const DWARFDie &UnitDIE,
                                 StringRef InputLocContents) {
  SmallVector<uint64_t, 8> RetainedLocationOffsets;
  for (const RetainedDWPDieInfo &RetainedDie : RetainedUnit.RetainedDIEs) {
    DWARFDie Die = getDIEForUnitRelativeOffset(UnitDIE, RetainedDie.Offset);
    if (!Die || Die.isNULL())
      continue;

    const DWARFAbbreviationDeclaration *AbbrevDecl =
        Die.getAbbreviationDeclarationPtr();
    if (!AbbrevDecl)
      continue;

    for (uint32_t AttrIndex = 0; AttrIndex < AbbrevDecl->getNumAttributes();
         ++AttrIndex) {
      dwarf::Form Form = AbbrevDecl->getFormByIndex(AttrIndex);
      dwarf::Attribute Attr = AbbrevDecl->getAttrByIndex(AttrIndex);
      if (!isLocationSectionOffsetAttr(Attr, Form))
        continue;

      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      Expected<std::pair<uint64_t, uint64_t>> RangeOrErr =
          getDIEAttributeByteRange(Die, AttrIndex);
      if (!RangeOrErr)
        return RangeOrErr.takeError();
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode location section offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());

      std::optional<uint64_t> OldOffset = Value.getAsSectionOffset();
      if (!OldOffset)
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to read location section offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      RetainedLocationOffsets.push_back(*OldOffset);
    }
  }

  llvm::sort(RetainedLocationOffsets);
  RetainedLocationOffsets.erase(
      llvm::unique(RetainedLocationOffsets), RetainedLocationOffsets.end());

  DenseMap<uint64_t, uint64_t> Remap;
  if (RetainedLocationOffsets.empty())
    return Remap;

  Expected<PackageUnitInfo::SectionContributionInfo> ContributionOrErr =
      getPackageContribution(LinkMap, UnitDIE, DW_SECT_EXT_LOC);
  if (!ContributionOrErr)
    return ContributionOrErr.takeError();

  uint64_t NextOffset = 0;
  if (ContributionOrErr->Offset + ContributionOrErr->Length > InputLocContents.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv(".debug_loc.dwo contribution [0x{0:x}, 0x{1:x}) exceeds section bounds",
                ContributionOrErr->Offset,
                ContributionOrErr->Offset + ContributionOrErr->Length)
            .str()
            .c_str());
  StringRef ContributionContents =
      InputLocContents.slice(ContributionOrErr->Offset,
                             ContributionOrErr->Offset + ContributionOrErr->Length);
  DWARFDataExtractor LocData(ContributionContents,
                             UnitDIE.getDwarfUnit()->isLittleEndian(),
                             UnitDIE.getDwarfUnit()->getAddressByteSize());
  DWARFDebugLoclists LocTable(LocData, UnitDIE.getDwarfUnit()->getVersion());
  std::optional<uint64_t> DWOId = getUnitDWOId(UnitDIE);
  for (uint64_t OldOffset : RetainedLocationOffsets) {
    uint64_t EndOffset = OldOffset;
    if (Error Err = LocTable.visitLocationList(
            &EndOffset, [](const DWARFLocationEntry &) { return true; }))
      return createStringError(
          std::errc::invalid_argument,
          formatv("failed to inspect .debug_loc.dwo list for DWO_id {0}: "
                  "unit-offset=0x{1:x}: {2}",
                  DWOId ? formatv("{0:x16}", *DWOId).str() : "<none>",
                  OldOffset, toString(std::move(Err)))
              .str()
              .c_str());
    if (EndOffset < OldOffset)
      return createStringError(
          std::errc::invalid_argument,
          formatv("invalid location list range [0x{0:x}, 0x{1:x})",
                  OldOffset, EndOffset)
              .str()
              .c_str());
    Remap.try_emplace(OldOffset, NextOffset);
    NextOffset += EndOffset - OldOffset;
  }

  return Remap;
}

static Expected<DenseMap<uint64_t, uint64_t>>
buildRetainedRangeListOffsetRemap(const DWPLinkMap &LinkMap,
                                  const RetainedDWPUnitInfo &RetainedUnit,
                                  const DWARFDie &UnitDIE,
                                  StringRef InputRnglistsContents) {
  if (UnitDIE.getDwarfUnit()->getVersion() < 5 || InputRnglistsContents.empty())
    return DenseMap<uint64_t, uint64_t>();

  SmallVector<uint64_t, 8> RetainedRangeOffsets;
  for (const RetainedDWPDieInfo &RetainedDie : RetainedUnit.RetainedDIEs) {
    DWARFDie Die = getDIEForUnitRelativeOffset(UnitDIE, RetainedDie.Offset);
    if (!Die || Die.isNULL())
      continue;

    const DWARFAbbreviationDeclaration *AbbrevDecl =
        Die.getAbbreviationDeclarationPtr();
    if (!AbbrevDecl)
      continue;

    for (uint32_t AttrIndex = 0; AttrIndex < AbbrevDecl->getNumAttributes();
         ++AttrIndex) {
      dwarf::Form Form = AbbrevDecl->getFormByIndex(AttrIndex);
      dwarf::Attribute Attr = AbbrevDecl->getAttrByIndex(AttrIndex);
      if (!isRangesSectionOffsetAttr(Attr, Form))
        continue;

      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      Expected<std::pair<uint64_t, uint64_t>> RangeOrErr =
          getDIEAttributeByteRange(Die, AttrIndex);
      if (!RangeOrErr)
        return RangeOrErr.takeError();
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode range list offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());

      std::optional<uint64_t> OldOffset = Value.getAsSectionOffset();
      if (!OldOffset)
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to read range list offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      RetainedRangeOffsets.push_back(*OldOffset);
    }
  }

  llvm::sort(RetainedRangeOffsets);
  RetainedRangeOffsets.erase(llvm::unique(RetainedRangeOffsets),
                             RetainedRangeOffsets.end());

  DenseMap<uint64_t, uint64_t> Remap;
  if (RetainedRangeOffsets.empty())
    return Remap;

  Expected<uint64_t> ContributionBaseOrErr =
      getPackageContributionBase(LinkMap, UnitDIE, DW_SECT_RNGLISTS);
  if (!ContributionBaseOrErr)
    return ContributionBaseOrErr.takeError();

  DWARFUnit &Unit = *UnitDIE.getDwarfUnit();
  DWARFDataExtractor RangesData(InputRnglistsContents, Unit.isLittleEndian(),
                                Unit.getAddressByteSize());

  uint64_t NextOffset = 0;
  uint64_t ContributionBase = *ContributionBaseOrErr;
  for (uint64_t OldOffset : RetainedRangeOffsets) {
    uint64_t AbsoluteOldOffset = ContributionBase + OldOffset;
    uint64_t EndOffset = AbsoluteOldOffset;
    DWARFDebugRnglist List;
    if (Error Err =
            List.extract(RangesData, /*HeaderOffset=*/0, &EndOffset,
                         ".debug_rnglists.dwo", "range"))
      return std::move(Err);
    if (EndOffset < AbsoluteOldOffset || EndOffset > InputRnglistsContents.size())
      return createStringError(
          std::errc::invalid_argument,
          formatv("invalid range list slice [0x{0:x}, 0x{1:x})",
                  AbsoluteOldOffset, EndOffset)
              .str()
              .c_str());
    Remap.try_emplace(OldOffset, NextOffset);
    NextOffset += EndOffset - AbsoluteOldOffset;
  }

  return Remap;
}

static ArrayRef<uint64_t> getRetainedChildOffsets(
    uint64_t ParentOffset,
    const DenseMap<uint64_t, SmallVector<uint64_t, 4>> &RetainedChildrenByOffset) {
  auto It = RetainedChildrenByOffset.find(ParentOffset);
  if (It == RetainedChildrenByOffset.end())
    return {};
  return It->second;
}

static Expected<uint64_t>
computeRetainedDIESerializedSize(const DWARFDie &Die, StringRef InputInfoContents,
                                 const DenseSet<uint64_t> &RetainedOffsets,
                                 const DenseMap<uint64_t, uint64_t> &NewOffsets,
                                 const DenseMap<uint64_t, uint64_t> &StringIndexRemap,
                                 const DenseMap<uint64_t, uint64_t> &LocationOffsetRemap,
                                 const DenseMap<uint64_t, uint64_t> &DirectStringOffsetRemap,
                                 const DenseMap<uint64_t, uint64_t> &RangeListOffsetRemap);

static Expected<uint64_t>
computeRetainedDIESubtreeSize(const DWARFDie &Die, StringRef InputInfoContents,
                              const DenseSet<uint64_t> &RetainedOffsets,
                              const DenseMap<uint64_t, uint64_t> &NewOffsets,
                              const DenseMap<uint64_t, uint64_t> &StringIndexRemap,
                              const DenseMap<uint64_t, uint64_t> &LocationOffsetRemap,
                              const DenseMap<uint64_t, uint64_t> &DirectStringOffsetRemap,
                              const DenseMap<uint64_t, uint64_t> &RangeListOffsetRemap) {
  Expected<uint64_t> OwnSizeOrErr = computeRetainedDIESerializedSize(
      Die, InputInfoContents, RetainedOffsets, NewOffsets, StringIndexRemap,
      LocationOffsetRemap, DirectStringOffsetRemap, RangeListOffsetRemap);
  if (!OwnSizeOrErr)
    return OwnSizeOrErr.takeError();

  uint64_t TotalSize = *OwnSizeOrErr;
  for (DWARFDie Child : Die.children()) {
    if (!RetainedOffsets.contains(getUnitRelativeDIEOffset(Child)))
      continue;
    Expected<uint64_t> ChildSizeOrErr = computeRetainedDIESubtreeSize(
        Child, InputInfoContents, RetainedOffsets, NewOffsets,
        StringIndexRemap, LocationOffsetRemap, DirectStringOffsetRemap,
        RangeListOffsetRemap);
    if (!ChildSizeOrErr)
      return ChildSizeOrErr.takeError();
    TotalSize += *ChildSizeOrErr;
  }

  if (Die.hasChildren())
    TotalSize += 1;
  return TotalSize;
}

static Expected<uint64_t>
computeRetainedDIESerializedSize(const DWARFDie &Die, StringRef InputInfoContents,
                                 const DenseSet<uint64_t> &RetainedOffsets,
                                 const DenseMap<uint64_t, uint64_t> &NewOffsets,
                                 const DenseMap<uint64_t, uint64_t> &StringIndexRemap,
                                 const DenseMap<uint64_t, uint64_t> &LocationOffsetRemap,
                                 const DenseMap<uint64_t, uint64_t> &DirectStringOffsetRemap,
                                 const DenseMap<uint64_t, uint64_t> &RangeListOffsetRemap) {
  const DWARFAbbreviationDeclaration *AbbrevDecl =
      Die.getAbbreviationDeclarationPtr();
  if (!AbbrevDecl)
    return uint64_t(1);

  uint64_t Size = AbbrevDecl->getCodeByteSize();
  for (uint32_t AttrIndex = 0; AttrIndex < AbbrevDecl->getNumAttributes();
       ++AttrIndex) {
    Expected<std::pair<uint64_t, uint64_t>> RangeOrErr =
        getDIEAttributeByteRange(Die, AttrIndex);
    if (!RangeOrErr)
      return RangeOrErr.takeError();

    dwarf::Form Form = AbbrevDecl->getFormByIndex(AttrIndex);
    dwarf::Attribute Attr = AbbrevDecl->getAttrByIndex(AttrIndex);
    if (isLocationSectionOffsetAttr(Attr, Form)) {
      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode location section offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      std::optional<uint64_t> OldOffset = Value.getAsSectionOffset();
      auto It =
          OldOffset ? LocationOffsetRemap.find(*OldOffset) : LocationOffsetRemap.end();
      if (!OldOffset || It == LocationOffsetRemap.end())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing rewritten location offset for DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      Size += RangeOrErr->second - RangeOrErr->first;
      continue;
    }

    if (isStringIndexForm(Form)) {
      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode string index form at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      uint64_t OldIndex = Value.getRawUValue();
      auto It = StringIndexRemap.find(OldIndex);
      if (It == StringIndexRemap.end())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing rewritten string index for DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      Size += getEncodedScalarFormValueSize(Form, It->second, *Die.getDwarfUnit());
      continue;
    }

    DWARFDie ReferencedDie = Die.getAttributeValueAsReferencedDie(Attr);
    if (!ReferencedDie || !shouldRewriteRetainedReferenceAttr(Attr)) {
      Size += RangeOrErr->second - RangeOrErr->first;
      continue;
    }

    if (isDirectStringOffsetForm(Form)) {
      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode direct string offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      std::optional<uint64_t> OldOffset = Value.getAsCStringOffset();
      auto It = OldOffset ? DirectStringOffsetRemap.find(*OldOffset)
                          : DirectStringOffsetRemap.end();
      if (!OldOffset || It == DirectStringOffsetRemap.end())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing rewritten direct string offset for DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      Size += RangeOrErr->second - RangeOrErr->first;
      continue;
    }

    if (isRangesSectionOffsetAttr(Attr, Form)) {
      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode range list offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      std::optional<uint64_t> OldOffset = Value.getAsSectionOffset();
      auto It = OldOffset ? RangeListOffsetRemap.find(*OldOffset)
                          : RangeListOffsetRemap.end();
      if (Die.getDwarfUnit()->getVersion() < 5 && RangeListOffsetRemap.empty()) {
        Size += RangeOrErr->second - RangeOrErr->first;
        continue;
      }
      if (!OldOffset || It == RangeListOffsetRemap.end())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing rewritten range list offset for DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      Size += RangeOrErr->second - RangeOrErr->first;
      continue;
    }

    switch (Form) {
    case dwarf::DW_FORM_ref1:
    case dwarf::DW_FORM_ref2:
    case dwarf::DW_FORM_ref4:
    case dwarf::DW_FORM_ref8:
      Size += RangeOrErr->second - RangeOrErr->first;
      break;
    case dwarf::DW_FORM_ref_udata: {
    auto It = NewOffsets.find(ReferencedDie.getOffset());
    if (It == NewOffsets.end()) {
      Size += RangeOrErr->second - RangeOrErr->first;
      continue;
    }
      Size += getEncodedScalarFormValueSize(Form, It->second, *Die.getDwarfUnit());
      break;
    }
    default:
      Size += RangeOrErr->second - RangeOrErr->first;
      break;
    }
  }

  return Size;
}

static Error assignRetainedDIEOffsets(
    const DWARFDie &UnitDIE, const DWARFDie &Die,
                                      StringRef InputInfoContents,
                                      const DenseSet<uint64_t> &RetainedOffsets,
                                      const DenseMap<uint64_t, SmallVector<uint64_t, 4>>
                                          &RetainedChildrenByOffset,
                                      const DenseMap<uint64_t, uint64_t> &PrevOffsets,
                                      const DenseMap<uint64_t, uint64_t> &StringIndexRemap,
                                      const DenseMap<uint64_t, uint64_t> &LocationOffsetRemap,
                                      const DenseMap<uint64_t, uint64_t> &DirectStringOffsetRemap,
                                      const DenseMap<uint64_t, uint64_t> &RangeListOffsetRemap,
                                      DenseMap<uint64_t, uint64_t> &NewOffsets,
                                      uint64_t &NextOffset) {
  NewOffsets[Die.getOffset()] = NextOffset;
  Expected<uint64_t> OwnSubtreeSizeOrErr = computeRetainedDIESerializedSize(
      Die, InputInfoContents, RetainedOffsets, PrevOffsets, StringIndexRemap,
      LocationOffsetRemap, DirectStringOffsetRemap, RangeListOffsetRemap);
  if (!OwnSubtreeSizeOrErr)
    return OwnSubtreeSizeOrErr.takeError();
  NextOffset += *OwnSubtreeSizeOrErr;

  for (uint64_t ChildOffset : getRetainedChildOffsets(
           getUnitRelativeDIEOffset(Die), RetainedChildrenByOffset)) {
    DWARFDie Child = getDIEForUnitRelativeOffset(UnitDIE, ChildOffset);
    if (!Child || Child.isNULL())
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to materialize retained child DIE 0x{0:x} for unit "
                  "DWO_id {1:x16}",
                  ChildOffset, getUnitDWOId(UnitDIE).value_or(0))
              .str()
              .c_str());
    if (Error Err = assignRetainedDIEOffsets(UnitDIE, Child, InputInfoContents,
                                             RetainedOffsets,
                                             RetainedChildrenByOffset,
                                             PrevOffsets, StringIndexRemap,
                                             LocationOffsetRemap,
                                             DirectStringOffsetRemap,
                                             RangeListOffsetRemap, NewOffsets,
                                             NextOffset))
      return Err;
  }

  if (Die.hasChildren())
    ++NextOffset;
  return Error::success();
}

static Expected<DenseMap<uint64_t, uint64_t>>
computeRetainedDIEOffsetMap(const DWARFDie &UnitDIE, StringRef InputInfoContents,
                            const DenseSet<uint64_t> &RetainedOffsets,
                            const DenseMap<uint64_t, SmallVector<uint64_t, 4>>
                                &RetainedChildrenByOffset,
                            const DenseMap<uint64_t, uint64_t> &StringIndexRemap,
                            const DenseMap<uint64_t, uint64_t> &LocationOffsetRemap,
                            const DenseMap<uint64_t, uint64_t> &DirectStringOffsetRemap,
                            const DenseMap<uint64_t, uint64_t> &RangeListOffsetRemap) {
  DenseMap<uint64_t, uint64_t> Offsets;
  Offsets.reserve(RetainedOffsets.size());
  const uint64_t UnitOffset = UnitDIE.getDwarfUnit()->getOffset();
  const uint64_t HeaderSize = UnitDIE.getDwarfUnit()->getHeaderSize();
  for (const uint64_t Offset : RetainedOffsets)
    Offsets[UnitOffset + Offset] = Offset;

  for (unsigned Iter = 0; Iter != 8; ++Iter) {
    DenseMap<uint64_t, uint64_t> NewOffsets;
    NewOffsets.reserve(Offsets.size());
    uint64_t NextOffset = HeaderSize;
    if (Error Err = assignRetainedDIEOffsets(UnitDIE, UnitDIE,
                                             InputInfoContents, RetainedOffsets,
                                             RetainedChildrenByOffset, Offsets,
                                             StringIndexRemap,
                                             LocationOffsetRemap,
                                             DirectStringOffsetRemap,
                                             RangeListOffsetRemap, NewOffsets,
                                             NextOffset))
      return std::move(Err);
    if (NewOffsets == Offsets)
      return NewOffsets;
    Offsets = std::move(NewOffsets);
  }

  return createStringError(std::errc::invalid_argument,
                           "retained DIE layout did not converge");
}

static Error serializeRetainedDIESubtree(
    const DWARFDie &UnitDIE, const DWARFDie &Die, StringRef InputInfoContents,
    const DenseSet<uint64_t> &RetainedOffsets,
    const DenseMap<uint64_t, SmallVector<uint64_t, 4>> &RetainedChildrenByOffset,
    const DenseMap<uint64_t, uint64_t> &NewOffsets,
    const DenseMap<uint64_t, uint64_t> &StringIndexRemap,
    const DenseMap<uint64_t, uint64_t> &LocationOffsetRemap,
    const DenseMap<uint64_t, uint64_t> &DirectStringOffsetRemap,
    const DenseMap<uint64_t, uint64_t> &RangeListOffsetRemap,
    std::string &Out) {
  const DWARFAbbreviationDeclaration *AbbrevDecl =
      Die.getAbbreviationDeclarationPtr();
  if (!AbbrevDecl) {
    Out.push_back('\0');
    return Error::success();
  }

  uint64_t AttrsBegin = Die.getOffset() + AbbrevDecl->getCodeByteSize();
  Out.append(InputInfoContents.slice(Die.getOffset(), AttrsBegin));

  for (uint32_t AttrIndex = 0; AttrIndex < AbbrevDecl->getNumAttributes();
       ++AttrIndex) {
    Expected<std::pair<uint64_t, uint64_t>> RangeOrErr =
        getDIEAttributeByteRange(Die, AttrIndex);
    if (!RangeOrErr)
      return RangeOrErr.takeError();

    dwarf::Form Form = AbbrevDecl->getFormByIndex(AttrIndex);
    dwarf::Attribute Attr = AbbrevDecl->getAttrByIndex(AttrIndex);
    if (isLocationSectionOffsetAttr(Attr, Form)) {
      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode location section offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      std::optional<uint64_t> OldOffset = Value.getAsSectionOffset();
      auto It =
          OldOffset ? LocationOffsetRemap.find(*OldOffset) : LocationOffsetRemap.end();
      if (!OldOffset || It == LocationOffsetRemap.end())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing rewritten location offset for DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      appendFixedUnsigned(Out, It->second, RangeOrErr->second - RangeOrErr->first,
                          Die.getDwarfUnit()->isLittleEndian());
      continue;
    }

    if (isStringIndexForm(Form)) {
      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode string index form at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      uint64_t OldIndex = Value.getRawUValue();
      auto It = StringIndexRemap.find(OldIndex);
      if (It == StringIndexRemap.end())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing rewritten string index for DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      if (Error Err = appendEncodedScalarFormValue(Out, Form, It->second,
                                                   *Die.getDwarfUnit()))
        return Err;
      continue;
    }

    if (isDirectStringOffsetForm(Form)) {
      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode direct string offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      std::optional<uint64_t> OldOffset = Value.getAsCStringOffset();
      auto It = OldOffset ? DirectStringOffsetRemap.find(*OldOffset)
                          : DirectStringOffsetRemap.end();
      if (!OldOffset || It == DirectStringOffsetRemap.end())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing rewritten direct string offset for DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      appendFixedUnsigned(Out, It->second, RangeOrErr->second - RangeOrErr->first,
                          Die.getDwarfUnit()->isLittleEndian());
      continue;
    }

    if (isRangesSectionOffsetAttr(Attr, Form)) {
      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode range list offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      std::optional<uint64_t> OldOffset = Value.getAsSectionOffset();
      auto It = OldOffset ? RangeListOffsetRemap.find(*OldOffset)
                          : RangeListOffsetRemap.end();
      if (Die.getDwarfUnit()->getVersion() < 5 && RangeListOffsetRemap.empty()) {
        Out.append(InputInfoContents.slice(RangeOrErr->first, RangeOrErr->second));
        continue;
      }
      if (!OldOffset || It == RangeListOffsetRemap.end())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing rewritten range list offset for DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      appendFixedUnsigned(Out, It->second, RangeOrErr->second - RangeOrErr->first,
                          Die.getDwarfUnit()->isLittleEndian());
      continue;
    }

    DWARFDie ReferencedDie = Die.getAttributeValueAsReferencedDie(Attr);
    if (!ReferencedDie || !shouldRewriteRetainedReferenceAttr(Attr)) {
      Out.append(InputInfoContents.slice(RangeOrErr->first, RangeOrErr->second));
      continue;
    }

    auto It = NewOffsets.find(ReferencedDie.getOffset());
    if (It == NewOffsets.end()) {
      Out.append(InputInfoContents.slice(RangeOrErr->first, RangeOrErr->second));
      continue;
    }

    switch (Form) {
    case dwarf::DW_FORM_ref1:
    case dwarf::DW_FORM_ref2:
    case dwarf::DW_FORM_ref4:
    case dwarf::DW_FORM_ref8:
    case dwarf::DW_FORM_ref_udata:
      if (Error Err = appendEncodedScalarFormValue(Out, Form, It->second,
                                                   *Die.getDwarfUnit()))
        return Err;
      break;
    default:
      Out.append(InputInfoContents.slice(RangeOrErr->first, RangeOrErr->second));
      break;
    }
  }

  for (uint64_t ChildOffset : getRetainedChildOffsets(
           getUnitRelativeDIEOffset(Die), RetainedChildrenByOffset)) {
    DWARFDie Child = getDIEForUnitRelativeOffset(UnitDIE, ChildOffset);
    if (!Child || Child.isNULL())
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to materialize retained child DIE 0x{0:x} for unit "
                  "DWO_id {1:x16}",
                  ChildOffset, getUnitDWOId(UnitDIE).value_or(0))
              .str()
              .c_str());
    if (Error Err = serializeRetainedDIESubtree(UnitDIE, Child,
                                                InputInfoContents,
                                                RetainedOffsets,
                                                RetainedChildrenByOffset,
                                                NewOffsets,
                                                StringIndexRemap,
                                                LocationOffsetRemap,
                                                DirectStringOffsetRemap,
                                                RangeListOffsetRemap, Out))
      return Err;
  }

  if (Die.hasChildren())
    Out.push_back('\0');
  return Error::success();
}

static void patchUnitLengthField(std::string &UnitContents,
                                 dwarf::DwarfFormat Format,
                                 bool IsLittleEndian) {
  uint8_t LengthFieldSize = dwarf::getUnitLengthFieldByteSize(Format);
  uint64_t UnitLength = UnitContents.size() - LengthFieldSize;
  assert((Format == dwarf::DWARF32 || UnitLength >= 0xffffffffULL) &&
         "unexpected DWARF64 unit length");

  if (Format == dwarf::DWARF32) {
    if (IsLittleEndian)
      support::endian::write32le(UnitContents.data(), static_cast<uint32_t>(UnitLength));
    else
      support::endian::write32be(UnitContents.data(), static_cast<uint32_t>(UnitLength));
    return;
  }

  if (IsLittleEndian) {
    support::endian::write32le(UnitContents.data(), 0xffffffffU);
    support::endian::write64le(UnitContents.data() + 4, UnitLength);
  } else {
    support::endian::write32be(UnitContents.data(), 0xffffffffU);
    support::endian::write64be(UnitContents.data() + 4, UnitLength);
  }
}

static Expected<std::string>
rewriteRetainedDWPInfoUnit(const RetainedDWPUnitInfo &RetainedUnit,
                           const DenseSet<uint64_t> &RetainedOffsets,
                           const DenseMap<uint64_t, SmallVector<uint64_t, 4>>
                               &RetainedChildrenByOffset,
                           const DWARFDie &UnitDIE,
                           StringRef InputInfoContents,
                           const DenseMap<uint64_t, uint64_t> &StringIndexRemap,
                           const DenseMap<uint64_t, uint64_t> &LocationOffsetRemap,
                           const DenseMap<uint64_t, uint64_t> &DirectStringOffsetRemap,
                           const DenseMap<uint64_t, uint64_t> &RangeListOffsetRemap) {
  Expected<DenseMap<uint64_t, uint64_t>> NewOffsetsOrErr =
      computeRetainedDIEOffsetMap(UnitDIE, InputInfoContents, RetainedOffsets,
                                  RetainedChildrenByOffset, StringIndexRemap,
                                  LocationOffsetRemap, DirectStringOffsetRemap,
                                  RangeListOffsetRemap);
  if (!NewOffsetsOrErr)
    return NewOffsetsOrErr.takeError();

  DWARFUnit &Unit = *UnitDIE.getDwarfUnit();
  uint64_t HeaderBegin = Unit.getOffset();
  uint64_t HeaderEnd = HeaderBegin + Unit.getHeaderSize();
  if (HeaderEnd > InputInfoContents.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("unit header exceeds .debug_info.dwo bounds: [0x{0:x}, 0x{1:x})",
                HeaderBegin, HeaderEnd)
            .str()
            .c_str());

  std::string Rewritten;
  Rewritten.append(InputInfoContents.slice(HeaderBegin, HeaderEnd));
  if (Error Err = serializeRetainedDIESubtree(UnitDIE, UnitDIE, InputInfoContents,
                                              RetainedOffsets,
                                              RetainedChildrenByOffset,
                                              *NewOffsetsOrErr,
                                              StringIndexRemap,
                                              LocationOffsetRemap,
                                              DirectStringOffsetRemap,
                                              RangeListOffsetRemap, Rewritten))
    return std::move(Err);

  patchUnitLengthField(Rewritten, Unit.getFormat(), Unit.isLittleEndian());
  return Rewritten;
}

static void serializeAbbreviationDeclaration(std::string &Out,
                                            const DWARFAbbreviationDeclaration &Decl) {
  raw_string_ostream OS(Out);
  encodeULEB128(Decl.getCode(), OS);
  encodeULEB128(Decl.getTag(), OS);
  OS << char(Decl.hasChildren() ? dwarf::DW_CHILDREN_yes
                                : dwarf::DW_CHILDREN_no);
  for (const auto &AttrSpec : Decl.attributes()) {
    encodeULEB128(AttrSpec.Attr, OS);
    encodeULEB128(AttrSpec.Form, OS);
    if (AttrSpec.isImplicitConst())
      encodeSLEB128(AttrSpec.getImplicitConstValue(), OS);
  }
  encodeULEB128(0, OS);
  encodeULEB128(0, OS);
  OS.flush();
}

static Expected<std::string>
rewriteRetainedDWPAbbrevUnit(const RetainedDWPUnitInfo &RetainedUnit,
                             const DWARFDie &UnitDIE,
                             StringRef InputAbbrevContents) {
  DenseSet<uint32_t> UsedCodes;
  SmallVector<uint32_t, 8> UsedCodesInOrder;
  for (const RetainedDWPDieInfo &RetainedDie : RetainedUnit.RetainedDIEs) {
    DWARFDie Die = getDIEForUnitRelativeOffset(UnitDIE, RetainedDie.Offset);
    if (!Die || Die.isNULL())
      continue;
    const DWARFAbbreviationDeclaration *Decl = Die.getAbbreviationDeclarationPtr();
    if (!Decl)
      continue;
    if (UsedCodes.insert(Decl->getCode()).second)
      UsedCodesInOrder.push_back(Decl->getCode());
  }

  DataExtractor AbbrevData(InputAbbrevContents, UnitDIE.getDwarfUnit()->isLittleEndian(),
                           0);
  DWARFDebugAbbrev DebugAbbrev(AbbrevData);
  Expected<const DWARFAbbreviationDeclarationSet *> SetOrErr =
      DebugAbbrev.getAbbreviationDeclarationSet(
          UnitDIE.getDwarfUnit()->getAbbreviationsOffset());
  if (!SetOrErr)
    return SetOrErr.takeError();

  std::string Rewritten;
  for (uint32_t Code : UsedCodesInOrder) {
    const DWARFAbbreviationDeclaration *Decl =
        (*SetOrErr)->getAbbreviationDeclaration(Code);
    if (!Decl)
      return createStringError(
          std::errc::invalid_argument,
          formatv("missing abbreviation code {0} for retained unit 0x{1:x16}",
                  Code, RetainedUnit.DWOId)
              .str()
              .c_str());
    serializeAbbreviationDeclaration(Rewritten, *Decl);
  }

  Rewritten.push_back('\0');
  return Rewritten;
}

static Expected<std::string> rewriteRetainedDWPStringOffsetsUnit(
    const DWARFDie &UnitDIE,
    ArrayRef<uint64_t> IndexedStringOffsetsInNewOrder,
    const DenseMap<uint64_t, uint64_t> &StringOffsetRemap) {
  DWARFUnit &Unit = *UnitDIE.getDwarfUnit();
  const uint8_t ItemSize = Unit.getDwarfStringOffsetsByteSize();
  std::string Rewritten;
  Rewritten.reserve(IndexedStringOffsetsInNewOrder.size() * ItemSize);

  for (uint64_t OldStringOffset : IndexedStringOffsetsInNewOrder) {
    auto It = StringOffsetRemap.find(OldStringOffset);
    if (It == StringOffsetRemap.end())
      return createStringError(
          std::errc::invalid_argument,
          formatv("missing rewritten string offset for old offset 0x{0:x}",
                  OldStringOffset)
              .str()
              .c_str());
    appendFixedUnsigned(Rewritten, It->second, ItemSize, Unit.isLittleEndian());
  }

  return Rewritten;
}

struct RetainedDWPStringSectionInfo {
  std::string Contents;
  DenseMap<uint64_t, DenseMap<uint64_t, uint64_t>> StringOffsetRemapsByDWOId;
};

static Expected<DenseMap<uint64_t, uint64_t>>
buildRetainedDirectStringOffsetRemap(const RetainedDWPUnitInfo &RetainedUnit,
                                     const DWARFDie &UnitDIE) {
  DenseMap<uint64_t, uint64_t> Remap;
  for (const RetainedDWPDieInfo &RetainedDie : RetainedUnit.RetainedDIEs) {
    DWARFDie Die = getDIEForUnitRelativeOffset(UnitDIE, RetainedDie.Offset);
    if (!Die || Die.isNULL())
      continue;

    const DWARFAbbreviationDeclaration *AbbrevDecl =
        Die.getAbbreviationDeclarationPtr();
    if (!AbbrevDecl)
      continue;

    for (uint32_t AttrIndex = 0; AttrIndex < AbbrevDecl->getNumAttributes();
         ++AttrIndex) {
      dwarf::Form Form = AbbrevDecl->getFormByIndex(AttrIndex);
      if (!isDirectStringOffsetForm(Form))
        continue;

      const auto &Spec = *(AbbrevDecl->attributes().begin() + AttrIndex);
      Expected<std::pair<uint64_t, uint64_t>> RangeOrErr =
          getDIEAttributeByteRange(Die, AttrIndex);
      if (!RangeOrErr)
        return RangeOrErr.takeError();
      DWARFFormValue Value = Spec.getFormValue();
      uint64_t Offset = RangeOrErr->first;
      if (!Value.extractValue(Die.getDwarfUnit()->getDebugInfoExtractor(),
                              &Offset, Die.getDwarfUnit()->getFormParams(),
                              &Die.getDwarfUnit()->getContext(),
                              Die.getDwarfUnit()))
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to decode direct string offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());

      std::optional<uint64_t> OldOffset = Value.getAsCStringOffset();
      if (!OldOffset)
        return createStringError(
            std::errc::invalid_argument,
            formatv("unable to read direct string offset at DIE 0x{0:x}",
                    Die.getOffset())
                .str()
                .c_str());
      Remap.try_emplace(*OldOffset, 0);
    }
  }

  return Remap;
}

static SmallVector<uint64_t, 8>
getSortedRetainedDirectStringOffsets(
    const DenseMap<uint64_t, uint64_t> &DirectStringOffsetRemap) {
  SmallVector<uint64_t, 8> Offsets;
  Offsets.reserve(DirectStringOffsetRemap.size());
  for (const auto &OffsetIt : DirectStringOffsetRemap)
    Offsets.push_back(OffsetIt.first);
  llvm::sort(Offsets);
  return Offsets;
}

static Expected<RetainedDWPStringSectionInfo> buildRetainedDWPStringSection(
    ArrayRef<RetainedDWPRewriteUnitDescriptor> Descriptors,
    StringRef InputStringContents,
    ArrayRef<RetainedDWPRewriteUnitAnalysis> UnitAnalyses) {
  RetainedDWPStringSectionInfo Result;
  StringMap<uint64_t> StringOffsetsByValue;

  assert(Descriptors.size() == UnitAnalyses.size() &&
         "retained unit analyses must align with retained units");
  for (const RetainedDWPRewriteUnitDescriptor &Descriptor : Descriptors) {
    uint64_t DWOId = Descriptor.DWOId;
    const RetainedDWPRewriteUnitAnalysis &Analysis =
        UnitAnalyses[Descriptor.SortedUnitIndex];
    assert(Analysis.DWOId == DWOId &&
           "retained unit analysis must match retained unit order");

    DenseMap<uint64_t, uint64_t> &StringOffsetRemap =
        Result.StringOffsetRemapsByDWOId[DWOId];
    for (uint64_t OldStringOffset : Analysis.IndexedStringOffsetsInNewOrder) {
      if (StringOffsetRemap.contains(OldStringOffset))
        continue;

      Expected<StringRef> StringOrErr =
          getDebugStrEntry(InputStringContents, OldStringOffset);
      if (!StringOrErr)
        return StringOrErr.takeError();

      auto [ValueIt, Inserted] =
          StringOffsetsByValue.try_emplace(*StringOrErr, Result.Contents.size());
      if (Inserted) {
        Result.Contents.append(StringOrErr->data(), StringOrErr->size());
        Result.Contents.push_back('\0');
      }
      StringOffsetRemap[OldStringOffset] = ValueIt->second;
    }

    for (uint64_t OldStringOffset : Analysis.DirectStringOffsetsInOrder) {
      if (StringOffsetRemap.contains(OldStringOffset))
        continue;

      Expected<StringRef> StringOrErr =
          getDebugStrEntry(InputStringContents, OldStringOffset);
      if (!StringOrErr)
        return StringOrErr.takeError();

      auto [ValueIt, Inserted] =
          StringOffsetsByValue.try_emplace(*StringOrErr, Result.Contents.size());
      if (Inserted) {
        Result.Contents.append(StringOrErr->data(), StringOrErr->size());
        Result.Contents.push_back('\0');
      }
      StringOffsetRemap[OldStringOffset] = ValueIt->second;
    }
  }

  return Result;
}

static Expected<std::string> rewriteRetainedDWPLocUnit(
    const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE, StringRef InputLocContents,
    const DenseMap<uint64_t, uint64_t> &LocationOffsetRemap) {
  if (LocationOffsetRemap.empty())
    return std::string();
  Expected<PackageUnitInfo::SectionContributionInfo> ContributionOrErr =
      getPackageContribution(LinkMap, UnitDIE, DW_SECT_EXT_LOC);
  if (!ContributionOrErr)
    return ContributionOrErr.takeError();
  if (ContributionOrErr->Offset + ContributionOrErr->Length > InputLocContents.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv(".debug_loc.dwo contribution [0x{0:x}, 0x{1:x}) exceeds section bounds",
                ContributionOrErr->Offset,
                ContributionOrErr->Offset + ContributionOrErr->Length)
            .str()
            .c_str());

  DWARFUnit &Unit = *UnitDIE.getDwarfUnit();
  StringRef ContributionContents =
      InputLocContents.slice(ContributionOrErr->Offset,
                             ContributionOrErr->Offset + ContributionOrErr->Length);
  DWARFDataExtractor LocData(ContributionContents, Unit.isLittleEndian(),
                             Unit.getAddressByteSize());
  DWARFDebugLoclists LocTable(LocData, Unit.getVersion());

  SmallVector<std::pair<uint64_t, uint64_t>, 8> IndexedEntries;
  IndexedEntries.reserve(LocationOffsetRemap.size());
  for (const auto &It : LocationOffsetRemap)
    IndexedEntries.emplace_back(It.second, It.first);
  llvm::sort(IndexedEntries);

  std::string Rewritten;
  std::optional<uint64_t> DWOId = getUnitDWOId(UnitDIE);
  for (const auto &[NewOffset, OldOffset] : IndexedEntries) {
    uint64_t EndOffset = OldOffset;
    if (Error Err = LocTable.visitLocationList(
            &EndOffset, [](const DWARFLocationEntry &) { return true; }))
      return createStringError(
          std::errc::invalid_argument,
          formatv("failed to rewrite .debug_loc.dwo list for DWO_id {0}: "
                  "new-offset=0x{1:x}, unit-offset=0x{2:x}: {3}",
                  DWOId ? formatv("{0:x16}", *DWOId).str() : "<none>",
                  NewOffset, OldOffset, toString(std::move(Err)))
              .str()
              .c_str());
    if (EndOffset > ContributionContents.size())
      return createStringError(
          std::errc::invalid_argument,
          formatv("location list [0x{0:x}, 0x{1:x}) exceeds .debug_loc.dwo "
                  "contribution bounds",
                  OldOffset, EndOffset)
              .str()
              .c_str());
    Rewritten.append(ContributionContents.slice(OldOffset, EndOffset));
  }

  return Rewritten;
}

static Expected<std::string> rewriteRetainedDWPRnglistsUnit(
    const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE,
    StringRef InputRnglistsContents,
    const DenseMap<uint64_t, uint64_t> &RangeListOffsetRemap) {
  if (RangeListOffsetRemap.empty())
    return std::string();
  Expected<uint64_t> ContributionBaseOrErr =
      getPackageContributionBase(LinkMap, UnitDIE, DW_SECT_RNGLISTS);
  if (!ContributionBaseOrErr)
    return ContributionBaseOrErr.takeError();

  DWARFUnit &Unit = *UnitDIE.getDwarfUnit();
  DWARFDataExtractor RangesData(InputRnglistsContents, Unit.isLittleEndian(),
                                Unit.getAddressByteSize());

  SmallVector<std::pair<uint64_t, uint64_t>, 8> IndexedEntries;
  IndexedEntries.reserve(RangeListOffsetRemap.size());
  for (const auto &It : RangeListOffsetRemap)
    IndexedEntries.emplace_back(It.second, It.first);
  llvm::sort(IndexedEntries);

  std::string Rewritten;
  uint64_t ContributionBase = *ContributionBaseOrErr;
  for (const auto &[NewOffset, OldOffset] : IndexedEntries) {
    (void)NewOffset;
    uint64_t AbsoluteOldOffset = ContributionBase + OldOffset;
    uint64_t EndOffset = AbsoluteOldOffset;
    DWARFDebugRnglist List;
    if (Error Err =
            List.extract(RangesData, /*HeaderOffset=*/0, &EndOffset,
                         ".debug_rnglists.dwo", "range"))
      return std::move(Err);
    if (EndOffset > InputRnglistsContents.size())
      return createStringError(
          std::errc::invalid_argument,
          formatv("range list [0x{0:x}, 0x{1:x}) exceeds .debug_rnglists.dwo "
                  "bounds",
                  AbsoluteOldOffset, EndOffset)
              .str()
              .c_str());
    Rewritten.append(InputRnglistsContents.slice(AbsoluteOldOffset, EndOffset));
  }

  return Rewritten;
}

static std::string formatRetainedContribution(
    const RetainedDWPSectionContributionInfo &Contribution);

static AddressRanges collectExecutableAddressRanges(const ObjectFile &ObjFile) {
  AddressRanges TextAddressRanges;
  for (const object::SectionRef &Sect : ObjFile.sections()) {
    if (!Sect.isText())
      continue;
    const uint64_t Size = Sect.getSize();
    if (Size == 0)
      continue;
    const uint64_t StartAddr = Sect.getAddress();
    TextAddressRanges.insert({StartAddr, StartAddr + Size});
  }
  return TextAddressRanges;
}

static void normalizeAddressRanges(
    SmallVectorImpl<SkeletonUnitInfo::AddressRangeInfo> &Ranges) {
  llvm::sort(Ranges, [](const SkeletonUnitInfo::AddressRangeInfo &LHS,
                        const SkeletonUnitInfo::AddressRangeInfo &RHS) {
    return std::tie(LHS.SectionIndex, LHS.LowPC, LHS.HighPC) <
           std::tie(RHS.SectionIndex, RHS.LowPC, RHS.HighPC);
  });

  SmallVector<SkeletonUnitInfo::AddressRangeInfo, 4> Normalized;
  for (const auto &Range : Ranges) {
    if (Range.LowPC >= Range.HighPC)
      continue;

    if (Normalized.empty() ||
        Normalized.back().SectionIndex != Range.SectionIndex ||
        Normalized.back().HighPC < Range.LowPC) {
      Normalized.push_back(Range);
      continue;
    }

    Normalized.back().HighPC =
        std::max(Normalized.back().HighPC, Range.HighPC);
  }

  Ranges = std::move(Normalized);
}

static void finalizeLiveUnitInfo(SkeletonUnitInfo &Info) {
  normalizeAddressRanges(Info.ResolvedRanges);
  normalizeAddressRanges(Info.LiveRanges);

  for (SkeletonUnitInfo::SubprogramInfo &Subprogram : Info.Subprograms) {
    normalizeAddressRanges(Subprogram.ResolvedRanges);
    normalizeAddressRanges(Subprogram.LiveRanges);
  }

  llvm::sort(Info.Subprograms, [](const SkeletonUnitInfo::SubprogramInfo &LHS,
                                  const SkeletonUnitInfo::SubprogramInfo &RHS) {
    if (LHS.isLive() != RHS.isLive())
      return LHS.isLive();
    if (!LHS.ResolvedRanges.empty() && !RHS.ResolvedRanges.empty() &&
        std::tie(LHS.ResolvedRanges.front().LowPC,
                 LHS.ResolvedRanges.front().HighPC,
                 LHS.Name) !=
            std::tie(RHS.ResolvedRanges.front().LowPC,
                     RHS.ResolvedRanges.front().HighPC,
                     RHS.Name))
      return std::tie(LHS.ResolvedRanges.front().LowPC,
                      LHS.ResolvedRanges.front().HighPC, LHS.Name) <
             std::tie(RHS.ResolvedRanges.front().LowPC,
                      RHS.ResolvedRanges.front().HighPC, RHS.Name);
    return LHS.Name < RHS.Name;
  });

  llvm::sort(Info.LiveRootOffsets);
  Info.LiveRootOffsets.erase(llvm::unique(Info.LiveRootOffsets),
                             Info.LiveRootOffsets.end());
  llvm::sort(Info.RetainedDIEOffsets);
  Info.RetainedDIEOffsets.erase(llvm::unique(Info.RetainedDIEOffsets),
                                Info.RetainedDIEOffsets.end());
  Info.RetainedDIEOffsetSet.clear();
  Info.RetainedDIEOffsetSet.insert(Info.RetainedDIEOffsets.begin(),
                                   Info.RetainedDIEOffsets.end());
}

static std::optional<uint64_t> getUnitDWOId(const DWARFDie &Die) {
  if (!Die)
    return std::nullopt;
  return Die.getDwarfUnit()->getDWOId();
}

static bool hasAnyLiveRange(const SkeletonUnitInfo &Info,
                            const DWARFAddressRangesVector &Ranges) {
  for (const DWARFAddressRange &Range : Ranges) {
    if (Info.containsLiveRange(Range.LowPC, Range.HighPC))
      return true;
  }
  return false;
}

static void copyAddressRanges(
    SmallVectorImpl<SkeletonUnitInfo::AddressRangeInfo> &Output,
    const DWARFAddressRangesVector &Ranges) {
  for (const DWARFAddressRange &Range : Ranges) {
    auto &StoredRange = Output.emplace_back();
    StoredRange.LowPC = Range.LowPC;
    StoredRange.HighPC = Range.HighPC;
    StoredRange.SectionIndex = Range.SectionIndex;
  }
}

static void keepLiveRanges(
    const AddressRanges &ExecutableRanges,
    SmallVectorImpl<SkeletonUnitInfo::AddressRangeInfo> &Output,
    const DWARFAddressRangesVector &Input) {
  for (const DWARFAddressRange &Range : Input) {
    std::optional<AddressRange> TextRange =
        ExecutableRanges.getRangeThatContains(Range.LowPC);
    if (!TextRange || TextRange->end() < Range.HighPC)
      continue;

    auto &StoredRange = Output.emplace_back();
    StoredRange.LowPC = Range.LowPC;
    StoredRange.HighPC = Range.HighPC;
    StoredRange.SectionIndex = Range.SectionIndex;
  }
}

static const SkeletonUnitInfo::SubprogramInfo *
findLiveSubprogram(const SkeletonUnitInfo &Info) {
  for (const SkeletonUnitInfo::SubprogramInfo &Subprogram : Info.Subprograms) {
    if (Subprogram.isLive())
      return &Subprogram;
  }
  return nullptr;
}

static bool addRetainedDIEOffset(uint64_t Offset, SkeletonUnitInfo &Info,
                                 DenseSet<uint64_t> &SeenOffsets,
                                 SmallVectorImpl<uint64_t> *NewOffsets =
                                     nullptr) {
  if (!SeenOffsets.insert(Offset).second)
    return false;
  Info.RetainedDIEOffsets.push_back(Offset);
  if (NewOffsets)
    NewOffsets->push_back(Offset);
  return true;
}

static void addRetainedDIEChain(const DWARFDie &Die, SkeletonUnitInfo &Info,
                                DenseSet<uint64_t> &SeenOffsets,
                                SmallVectorImpl<uint64_t> *NewOffsets =
                                    nullptr) {
  for (DWARFDie Current = Die; Current; Current = Current.getParent()) {
    uint64_t Offset = getUnitRelativeDIEOffset(Current);
    addRetainedDIEOffset(Offset, Info, SeenOffsets, NewOffsets);
  }
}

// Subtree retention is context-independent: walking a subtree only inserts the
// unit-relative DIE offsets reachable through child edges. Once a subtree has
// been fully expanded, revisiting it cannot add any new retained DIEs.
static void addRetainedDIESubtree(const DWARFDie &RootDie, SkeletonUnitInfo &Info,
                                  DenseSet<uint64_t> &SeenOffsets,
                                  DenseSet<uint64_t> &ExpandedSubtreeOffsets,
                                  SmallVectorImpl<uint64_t> *NewOffsets =
                                      nullptr) {
  uint64_t RootOffset = getUnitRelativeDIEOffset(RootDie);
  if (ExpandedSubtreeOffsets.contains(RootOffset))
    return;

  SmallVector<std::pair<DWARFDie, bool>, 8> Worklist;
  Worklist.emplace_back(RootDie, false);

  while (!Worklist.empty()) {
    auto [Die, PostVisit] = Worklist.pop_back_val();
    uint64_t Offset = getUnitRelativeDIEOffset(Die);
    if (PostVisit) {
      ExpandedSubtreeOffsets.insert(Offset);
      continue;
    }

    if (ExpandedSubtreeOffsets.contains(Offset))
      continue;

    addRetainedDIEOffset(Offset, Info, SeenOffsets, NewOffsets);
    Worklist.emplace_back(Die, true);

    for (DWARFDie Child : reverse(Die.children()))
      Worklist.emplace_back(Child, false);
  }
}

static void addReferencedDIEClosure(const DWARFDie &RootDie,
                                    SkeletonUnitInfo &Info,
                                    DenseSet<uint64_t> &SeenOffsets,
                                    DenseSet<uint64_t> &ExpandedSubtreeOffsets,
                                    DenseSet<uint64_t> &ExpandedReferenceOffsets) {
  SmallVector<DWARFDie, 8> Worklist;
  DenseSet<uint64_t> SeenDIEOffsets;
  Worklist.push_back(RootDie);

  while (!Worklist.empty()) {
    DWARFDie Die = Worklist.pop_back_val();
    uint64_t Offset = getUnitRelativeDIEOffset(Die);
    if (!SeenDIEOffsets.insert(Offset).second)
      continue;
    if (ExpandedReferenceOffsets.contains(Offset))
      continue;

    for (dwarf::Attribute Attr : {dwarf::DW_AT_type,
                                  dwarf::DW_AT_specification,
                                  dwarf::DW_AT_abstract_origin,
                                  dwarf::DW_AT_object_pointer,
                                  dwarf::DW_AT_containing_type}) {
      DWARFDie ReferencedDie = Die.getAttributeValueAsReferencedDie(Attr);
      if (!ReferencedDie)
        continue;

      addRetainedDIEChain(ReferencedDie, Info, SeenOffsets);
      addRetainedDIESubtree(ReferencedDie, Info, SeenOffsets,
                            ExpandedSubtreeOffsets);
      Worklist.push_back(ReferencedDie);
    }

    for (DWARFDie Child : reverse(Die.children()))
      Worklist.push_back(Child);

    // Reference expansion is cached only after all reference attrs on this DIE
    // have been processed and any newly referenced DIEs were queued.
    ExpandedReferenceOffsets.insert(Offset);
  }
}

// Some retained declaration DIEs are only reached via unit-local ref attrs
// hanging off other retained declarations. Saturate the retained set so those
// references are rewritten to live DIEs instead of dangling after GC.
static void expandRetainedReferenceClosure(const DWARFDie &UnitDIE,
                                           SkeletonUnitInfo &Info,
                                           DenseSet<uint64_t> &ExpandedSubtreeOffsets,
                                           DenseSet<uint64_t> &ExpandedReferenceOffsets) {
  DenseSet<uint64_t> SeenOffsets(Info.RetainedDIEOffsets.begin(),
                                 Info.RetainedDIEOffsets.end());
  SmallVector<uint64_t, 16> Worklist(Info.RetainedDIEOffsets.begin(),
                                     Info.RetainedDIEOffsets.end());

  while (!Worklist.empty()) {
    uint64_t Offset = Worklist.pop_back_val();
    if (ExpandedReferenceOffsets.contains(Offset))
      continue;

    DWARFDie Die = getDIEForUnitRelativeOffset(UnitDIE, Offset);
    if (!Die || Die.isNULL())
      continue;

    for (dwarf::Attribute Attr : {dwarf::DW_AT_type,
                                  dwarf::DW_AT_specification,
                                  dwarf::DW_AT_abstract_origin,
                                  dwarf::DW_AT_object_pointer,
                                  dwarf::DW_AT_containing_type}) {
      DWARFDie ReferencedDie = Die.getAttributeValueAsReferencedDie(Attr);
      if (!ReferencedDie)
        continue;

      addRetainedDIEChain(ReferencedDie, Info, SeenOffsets, &Worklist);
      addRetainedDIESubtree(ReferencedDie, Info, SeenOffsets,
                            ExpandedSubtreeOffsets, &Worklist);
    }

    // Cache this DIE only after all newly referenced retained DIEs are enqueued.
    ExpandedReferenceOffsets.insert(Offset);
  }
}

size_t SkeletonUnitInfo::getLiveSubprogramCount() const {
  return llvm::count_if(Subprograms, [](const SubprogramInfo &Subprogram) {
    return Subprogram.isLive();
  });
}

bool SkeletonUnitInfo::containsRetainedDIEOffset(uint64_t Offset) const {
  return RetainedDIEOffsetSet.contains(Offset);
}

Expected<bool> isLiveSubprogramDIE(const DWPLinkMap &LinkMap,
                                   const DWARFDie &SubprogramDIE) {
  if (!SubprogramDIE || !SubprogramDIE.isSubprogramDIE())
    return createStringError(std::errc::invalid_argument,
                             "expected a DW_TAG_subprogram DIE");

  std::optional<uint64_t> DWOId = getUnitDWOId(SubprogramDIE);
  if (!DWOId)
    return createStringError(std::errc::invalid_argument,
                             "subprogram DIE does not belong to a split unit");

  const LinkedSplitUnit *Linked = LinkMap.findLinkedUnit(*DWOId);
  if (!Linked)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to find linked split unit for DWO_id {0:x16}", *DWOId)
            .str()
            .c_str());

  Expected<DWARFAddressRangesVector> RangesOrErr =
      SubprogramDIE.getAddressRanges();
  if (!RangesOrErr)
    return RangesOrErr.takeError();

  return hasAnyLiveRange(Linked->Skeleton, *RangesOrErr);
}

Expected<bool> shouldRetainDWPDIE(const DWPLinkMap &LinkMap,
                                  const DWARFDie &DIE) {
  if (!DIE)
    return createStringError(std::errc::invalid_argument,
                             "expected a valid DIE");

  std::optional<uint64_t> DWOId = getUnitDWOId(DIE);
  if (!DWOId)
    return createStringError(std::errc::invalid_argument,
                             "DIE does not belong to a split unit");

  const LinkedSplitUnit *Linked = LinkMap.findLinkedUnit(*DWOId);
  if (!Linked)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to find linked split unit for DWO_id {0:x16}", *DWOId)
            .str()
            .c_str());

  return Linked->Skeleton.containsRetainedDIEOffset(getUnitRelativeDIEOffset(DIE));
}

Expected<SmallVector<DWARFDie, 16>>
collectRetainedDWPDIEs(const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE) {
  if (!UnitDIE)
    return createStringError(std::errc::invalid_argument,
                             "expected a valid unit DIE");

  SmallVector<DWARFDie, 16> RetainedDIEs;
  SmallVector<DWARFDie, 8> Worklist;
  Worklist.push_back(UnitDIE);

  while (!Worklist.empty()) {
    DWARFDie Die = Worklist.pop_back_val();
    Expected<bool> ShouldRetainOrErr = shouldRetainDWPDIE(LinkMap, Die);
    if (!ShouldRetainOrErr)
      return ShouldRetainOrErr.takeError();

    if (*ShouldRetainOrErr)
      RetainedDIEs.push_back(Die);

    for (DWARFDie Child : reverse(Die.children()))
      Worklist.push_back(Child);
  }

  return RetainedDIEs;
}

Expected<SmallVector<RetainedDWPDieInfo, 16>>
collectRetainedDWPDieInfos(const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE) {
  Expected<SmallVector<DWARFDie, 16>> RetainedDIEsOrErr =
      collectRetainedDWPDIEs(LinkMap, UnitDIE);
  if (!RetainedDIEsOrErr)
    return RetainedDIEsOrErr.takeError();

  SmallVector<RetainedDWPDieInfo, 16> Infos;
  Infos.reserve(RetainedDIEsOrErr->size());

  for (const DWARFDie &Die : *RetainedDIEsOrErr) {
    RetainedDWPDieInfo &Info = Infos.emplace_back();
    Info.Offset = getUnitRelativeDIEOffset(Die);
    Info.Tag = Die.getTag();

    DWARFDie Parent = Die.getParent();
    if (Parent)
      Info.ParentOffset = getUnitRelativeDIEOffset(Parent);
  }

  llvm::sort(Infos, [](const RetainedDWPDieInfo &LHS,
                       const RetainedDWPDieInfo &RHS) {
    return LHS.Offset < RHS.Offset;
  });

  return Infos;
}

Expected<RetainedDWPUnitInfo>
collectRetainedDWPUnitInfo(const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE) {
  if (!UnitDIE)
    return createStringError(std::errc::invalid_argument,
                             "expected a valid unit DIE");

  std::optional<uint64_t> DWOId = getUnitDWOId(UnitDIE);
  if (!DWOId)
    return createStringError(std::errc::invalid_argument,
                             "unit DIE does not belong to a split unit");

  Expected<SmallVector<RetainedDWPDieInfo, 16>> RetainedDIEInfosOrErr =
      collectRetainedDWPDieInfos(LinkMap, UnitDIE);
  if (!RetainedDIEInfosOrErr)
    return RetainedDIEInfosOrErr.takeError();

  RetainedDWPUnitInfo Info;
  Info.DWOId = *DWOId;
  Info.UnitOffset = UnitDIE.getOffset();
  Info.UnitName = dwarf::toString(UnitDIE.find(dwarf::DW_AT_name), "");
  Info.RetainedDIEs = std::move(*RetainedDIEInfosOrErr);
  return Info;
}

template <typename ItemT, typename ResultT, typename FnT>
static Error executeRetainedDWPUnitTasks(ArrayRef<ItemT> Items,
                                         const Options &Options,
                                         MutableArrayRef<ResultT> Results,
                                         FnT &&Fn);

static Expected<SmallVector<RetainedDWPUnitInfo, 4>>
collectRetainedDWPUnitsFromContext(const DWPLinkMap &LinkMap,
                                   DWARFContext &DWPContext) {
  SmallVector<RetainedDWPUnitInfo, 4> Units;
  for (const std::unique_ptr<DWARFUnit> &CU : DWPContext.dwo_compile_units()) {
    std::optional<uint64_t> DWOId = CU->getDWOId();
    if (!DWOId || !LinkMap.findLinkedUnit(*DWOId))
      continue;

    Expected<RetainedDWPUnitInfo> UnitInfoOrErr =
        collectRetainedDWPUnitInfo(LinkMap,
                                   CU->getUnitDIE(/*ExtractUnitDIEOnly=*/false));
    if (!UnitInfoOrErr)
      return UnitInfoOrErr.takeError();

    Units.push_back(std::move(*UnitInfoOrErr));
  }

  llvm::sort(Units, [](const RetainedDWPUnitInfo &LHS,
                       const RetainedDWPUnitInfo &RHS) {
    return std::tie(LHS.DWOId, LHS.UnitOffset) <
           std::tie(RHS.DWOId, RHS.UnitOffset);
  });

  return Units;
}

static Expected<RetainedDWPUnitInfo>
collectRetainedDWPUnitTask(const DWPLinkMap &LinkMap, StringRef DWPFileName,
                           const RetainedDWPCollectUnitDescriptor &Descriptor) {
  Expected<LoadedRetainedDWPUnit> LoadedOrErr = loadIndexedDWPUnit(
      DWPFileName, Descriptor.DWOId, Descriptor.UnitOffset,
      Descriptor.PackageUnitIndex);
  if (!LoadedOrErr)
    return LoadedOrErr.takeError();

  return collectRetainedDWPUnitInfo(LinkMap, LoadedOrErr->UnitDIE);
}

static Expected<SmallVector<RetainedDWPPackageUnitInfo, 4>>
buildRetainedDWPPackageUnits(const DWPLinkMap &LinkMap,
                             SmallVectorImpl<RetainedDWPUnitInfo> &Units) {
  SmallVector<RetainedDWPPackageUnitInfo, 4> PackageUnits;
  PackageUnits.reserve(Units.size());

  for (RetainedDWPUnitInfo &Unit : Units) {
    const PackageUnitInfo *Package = LinkMap.findPackageUnit(Unit.DWOId);
    if (!Package)
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to find package contribution for retained DWO_id {0:x16}",
                  Unit.DWOId)
              .str()
              .c_str());

    RetainedDWPPackageUnitInfo &Summary = PackageUnits.emplace_back();
    Summary.Unit = std::move(Unit);
    Summary.Contributions = Package->Contributions;
  }

  llvm::sort(PackageUnits, [](const RetainedDWPPackageUnitInfo &LHS,
                              const RetainedDWPPackageUnitInfo &RHS) {
    return std::tie(LHS.Unit.DWOId, LHS.Unit.UnitOffset) <
           std::tie(RHS.Unit.DWOId, RHS.Unit.UnitOffset);
  });

  return PackageUnits;
}

Expected<SmallVector<RetainedDWPUnitInfo, 4>>
collectRetainedDWPUnits(const DWPLinkMap &LinkMap, StringRef DWPFileName) {
  Expected<OwningBinary<Binary>> DWPBinOrErr = openBinary(DWPFileName);
  if (!DWPBinOrErr)
    return DWPBinOrErr.takeError();

  auto *DWPObject = cast<ObjectFile>(DWPBinOrErr->getBinary());
  std::unique_ptr<DWARFContext> DWPContext = DWARFContext::create(*DWPObject);
  if (!DWPContext->isDWP())
    return createStringError(
        std::errc::invalid_argument,
        formatv("file '{0}' is not a DWARF package file", DWPFileName)
            .str()
            .c_str());

  return collectRetainedDWPUnitsFromContext(LinkMap, *DWPContext);
}

static Expected<SmallVector<RetainedDWPPackageUnitInfo, 4>>
collectRetainedDWPPackageUnitsFromContext(const DWPLinkMap &LinkMap,
                                          DWARFContext &DWPContext) {
  Expected<SmallVector<RetainedDWPUnitInfo, 4>> UnitsOrErr =
      collectRetainedDWPUnitsFromContext(LinkMap, DWPContext);
  if (!UnitsOrErr)
    return UnitsOrErr.takeError();

  return buildRetainedDWPPackageUnits(LinkMap, *UnitsOrErr);
}

static Expected<SmallVector<RetainedDWPPackageUnitInfo, 4>>
collectRetainedDWPPackageUnitsInParallel(const DWPLinkMap &LinkMap,
                                         DWARFContext &DWPContext,
                                         const Options &Options,
                                         StringRef DWPFileName) {
  Expected<SmallVector<RetainedDWPCollectUnitDescriptor, 4>> DescriptorsOrErr =
      buildRetainedDWPCollectUnitDescriptors(DWPContext, LinkMap);
  if (!DescriptorsOrErr)
    return DescriptorsOrErr.takeError();

  SmallVector<RetainedDWPUnitInfo, 4> Units(DescriptorsOrErr->size());
  if (Error Err = executeRetainedDWPUnitTasks(
          ArrayRef<RetainedDWPCollectUnitDescriptor>(*DescriptorsOrErr), Options,
          MutableArrayRef<RetainedDWPUnitInfo>(Units),
          [&](size_t, const RetainedDWPCollectUnitDescriptor &Descriptor)
              -> Expected<RetainedDWPUnitInfo> {
            return collectRetainedDWPUnitTask(LinkMap, DWPFileName, Descriptor);
          }))
    return std::move(Err);

  return buildRetainedDWPPackageUnits(LinkMap, Units);
}

Expected<SmallVector<RetainedDWPPackageUnitInfo, 4>>
collectRetainedDWPPackageUnits(const DWPLinkMap &LinkMap, StringRef DWPFileName) {
  Expected<OwningBinary<Binary>> DWPBinOrErr = openBinary(DWPFileName);
  if (!DWPBinOrErr)
    return DWPBinOrErr.takeError();

  auto *DWPObject = cast<ObjectFile>(DWPBinOrErr->getBinary());
  std::unique_ptr<DWARFContext> DWPContext = DWARFContext::create(*DWPObject);
  if (!DWPContext->isDWP())
    return createStringError(
        std::errc::invalid_argument,
        formatv("file '{0}' is not a DWARF package file", DWPFileName)
            .str()
            .c_str());

  return collectRetainedDWPPackageUnitsFromContext(LinkMap, *DWPContext);
}

static bool shouldUseRetainedDWPRewriteParallelism(const Options &Options,
                                                   size_t NumUnits) {
  if (!Options.NumThreadsExplicit || NumUnits <= 1)
    return false;
  return Options.NumThreads == 0 || Options.NumThreads > 1;
}

template <typename ItemT, typename ResultT, typename FnT>
static Error executeRetainedDWPUnitTasks(ArrayRef<ItemT> Items,
                                         const Options &Options,
                                         MutableArrayRef<ResultT> Results,
                                         FnT &&Fn) {
  assert(Items.size() == Results.size() &&
         "retained unit task results must align with retained units");

  if (!shouldUseRetainedDWPRewriteParallelism(Options, Items.size())) {
    for (size_t I = 0; I != Items.size(); ++I) {
      Expected<ResultT> ResultOrErr = Fn(I, Items[I]);
      if (!ResultOrErr)
        return ResultOrErr.takeError();
      Results[I] = std::move(*ResultOrErr);
    }
    return Error::success();
  }

  unsigned ThreadCount = Options.NumThreads > 0
                             ? static_cast<unsigned>(Options.NumThreads)
                             : 0;
  DefaultThreadPool Pool(hardware_concurrency(ThreadCount));
  ThreadPoolTaskGroup Group(Pool);
  std::mutex ErrorMutex;
  Error Err = Error::success();
  for (size_t I = 0; I != Items.size(); ++I) {
    Group.async([&, I] {
      Expected<ResultT> ResultOrErr = Fn(I, Items[I]);
      if (!ResultOrErr) {
        std::lock_guard<std::mutex> Guard(ErrorMutex);
        Err = joinErrors(std::move(Err), ResultOrErr.takeError());
        return;
      }
      Results[I] = std::move(*ResultOrErr);
    });
  }
  Group.wait();
  return std::move(Err);
}

static Expected<RetainedDWPRewriteUnitAnalysis>
analyzeRetainedDWPRewriteUnit(const DWPLinkMap &LinkMap,
                              const RetainedDWPUnitInfo &RetainedUnit,
                              const DWARFDie &UnitDIE, StringRef StringOffsetsContents,
                              StringRef LocContents,
                              StringRef RnglistsContents) {
  RetainedDWPRewriteUnitAnalysis Analysis;
  Analysis.RetainedUnit = RetainedUnit;
  Analysis.DWOId = Analysis.RetainedUnit.DWOId;
  Analysis.RetainedChildrenByOffset.reserve(
      Analysis.RetainedUnit.RetainedDIEs.size());
  for (const RetainedDWPDieInfo &RetainedDie : Analysis.RetainedUnit.RetainedDIEs) {
    Analysis.RetainedOffsets.insert(RetainedDie.Offset);
    if (RetainedDie.ParentOffset)
      Analysis.RetainedChildrenByOffset[*RetainedDie.ParentOffset].push_back(
          RetainedDie.Offset);
  }

  Expected<DenseMap<uint64_t, uint64_t>> StringIndexRemapOrErr =
      buildRetainedStringIndexRemap(UnitDIE, Analysis.RetainedOffsets);
  if (!StringIndexRemapOrErr)
    return StringIndexRemapOrErr.takeError();
  Analysis.StringIndexRemap = std::move(*StringIndexRemapOrErr);

  Expected<SmallVector<uint64_t, 8>> IndexedStringOffsetsOrErr =
      collectRetainedStringOffsetsInNewIndexOrder(
          UnitDIE, StringOffsetsContents, Analysis.StringIndexRemap);
  if (!IndexedStringOffsetsOrErr)
    return IndexedStringOffsetsOrErr.takeError();
  Analysis.IndexedStringOffsetsInNewOrder =
      std::move(*IndexedStringOffsetsOrErr);

  Expected<DenseMap<uint64_t, uint64_t>> DirectStringOffsetRemapOrErr =
      buildRetainedDirectStringOffsetRemap(Analysis.RetainedUnit, UnitDIE);
  if (!DirectStringOffsetRemapOrErr)
    return DirectStringOffsetRemapOrErr.takeError();
  Analysis.DirectStringOffsetRemap = std::move(*DirectStringOffsetRemapOrErr);
  Analysis.DirectStringOffsetsInOrder =
      getSortedRetainedDirectStringOffsets(Analysis.DirectStringOffsetRemap);

  Expected<DenseMap<uint64_t, uint64_t>> LocationOffsetRemapOrErr =
      buildRetainedLocationOffsetRemap(LinkMap, Analysis.RetainedUnit, UnitDIE,
                                       LocContents);
  if (!LocationOffsetRemapOrErr)
    return LocationOffsetRemapOrErr.takeError();
  Analysis.LocationOffsetRemap = std::move(*LocationOffsetRemapOrErr);

  Expected<DenseMap<uint64_t, uint64_t>> RangeListOffsetRemapOrErr =
      buildRetainedRangeListOffsetRemap(LinkMap, Analysis.RetainedUnit, UnitDIE,
                                        RnglistsContents);
  if (!RangeListOffsetRemapOrErr)
    return RangeListOffsetRemapOrErr.takeError();
  Analysis.RangeListOffsetRemap = std::move(*RangeListOffsetRemapOrErr);

  return Analysis;
}

static Expected<RetainedDWPRewriteUnitAnalysis>
analyzeRetainedDWPRewriteUnitTask(
    const DWPLinkMap &LinkMap, StringRef DWPFileName,
    const RetainedDWPPackageUnitInfo &RetainedUnit,
    const RetainedDWPRewriteUnitDescriptor &Descriptor) {
  Expected<LoadedRetainedDWPUnit> LoadedOrErr =
      loadRetainedDWPUnitForRewrite(DWPFileName, Descriptor);
  if (!LoadedOrErr)
    return LoadedOrErr.takeError();

  return analyzeRetainedDWPRewriteUnit(
      LinkMap, RetainedUnit.Unit, LoadedOrErr->UnitDIE,
      LoadedOrErr->SectionContents.lookup(DW_SECT_STR_OFFSETS),
      LoadedOrErr->SectionContents.lookup(DW_SECT_EXT_LOC),
      LoadedOrErr->SectionContents.lookup(DW_SECT_RNGLISTS));
}

static Expected<RetainedDWPRewriteUnitContents>
rewriteRetainedDWPUnit(const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE,
                       const DenseMap<DWARFSectionKind, StringRef> &SectionContents,
                       const RetainedDWPRewriteUnitAnalysis &Analysis,
                       const DenseMap<uint64_t, uint64_t> &StringOffsetRemap) {
  RetainedDWPRewriteUnitContents Contents;
  Contents.DWOId = Analysis.DWOId;

  Expected<std::string> RewrittenInfoOrErr = rewriteRetainedDWPInfoUnit(
      Analysis.RetainedUnit, Analysis.RetainedOffsets,
      Analysis.RetainedChildrenByOffset, UnitDIE,
      SectionContents.lookup(DW_SECT_INFO),
      Analysis.StringIndexRemap, Analysis.LocationOffsetRemap,
      StringOffsetRemap, Analysis.RangeListOffsetRemap);
  if (!RewrittenInfoOrErr)
    return RewrittenInfoOrErr.takeError();
  Contents.Info = std::move(*RewrittenInfoOrErr);

  Expected<std::string> RewrittenAbbrevOrErr = rewriteRetainedDWPAbbrevUnit(
      Analysis.RetainedUnit, UnitDIE, SectionContents.lookup(DW_SECT_ABBREV));
  if (!RewrittenAbbrevOrErr)
    return RewrittenAbbrevOrErr.takeError();
  Contents.Abbrev = std::move(*RewrittenAbbrevOrErr);

  Expected<std::string> RewrittenStrOffsetsOrErr =
      rewriteRetainedDWPStringOffsetsUnit(UnitDIE,
                                          Analysis.IndexedStringOffsetsInNewOrder,
                                          StringOffsetRemap);
  if (!RewrittenStrOffsetsOrErr)
    return RewrittenStrOffsetsOrErr.takeError();
  Contents.StringOffsets = std::move(*RewrittenStrOffsetsOrErr);

  Expected<std::string> RewrittenLocOrErr = rewriteRetainedDWPLocUnit(
      LinkMap, UnitDIE, SectionContents.lookup(DW_SECT_EXT_LOC),
      Analysis.LocationOffsetRemap);
  if (!RewrittenLocOrErr)
    return RewrittenLocOrErr.takeError();
  Contents.Loc = std::move(*RewrittenLocOrErr);

  Expected<std::string> RewrittenRnglistsOrErr = rewriteRetainedDWPRnglistsUnit(
      LinkMap, UnitDIE, SectionContents.lookup(DW_SECT_RNGLISTS),
      Analysis.RangeListOffsetRemap);
  if (!RewrittenRnglistsOrErr)
    return RewrittenRnglistsOrErr.takeError();
  Contents.Rnglists = std::move(*RewrittenRnglistsOrErr);

  return Contents;
}

static Expected<RetainedDWPRewriteUnitContents>
rewriteRetainedDWPUnitTask(const DWPLinkMap &LinkMap, StringRef DWPFileName,
                           const RetainedDWPRewriteUnitDescriptor &Descriptor,
                           const RetainedDWPRewriteUnitAnalysis &Analysis,
                           const DenseMap<uint64_t, uint64_t> &StringOffsetRemap) {
  Expected<LoadedRetainedDWPUnit> LoadedOrErr =
      loadRetainedDWPUnitForRewrite(DWPFileName, Descriptor);
  if (!LoadedOrErr)
    return LoadedOrErr.takeError();

  return rewriteRetainedDWPUnit(LinkMap, LoadedOrErr->UnitDIE,
                                LoadedOrErr->SectionContents, Analysis,
                                StringOffsetRemap);
}

Expected<RetainedDWPRewritePlan>
collectRetainedDWPRewritePlan(const DWPLinkMap &LinkMap, const Options &Options) {
  Expected<OwningBinary<Binary>> DWPBinOrErr = openBinary(Options.DWPFileName);
  if (!DWPBinOrErr)
    return DWPBinOrErr.takeError();
  auto *DWPObject = cast<ObjectFile>(DWPBinOrErr->getBinary());
  std::unique_ptr<DWARFContext> DWPContext = DWARFContext::create(*DWPObject);

  RetainedDWPRewritePlan Plan;
  Plan.Units = LinkMap.RetainedPackageUnits;
  bool UseParallel =
      shouldUseRetainedDWPRewriteParallelism(Options, Plan.Units.size());

  DenseMap<DWARFSectionKind, uint64_t> SectionSizes;
  DenseMap<DWARFSectionKind, StringRef> SectionContents;
  StringRef DebugStrContents;
  if (Error Err = collectDWPSectionContents(*DWPObject, SectionContents,
                                            DebugStrContents, &SectionSizes))
    return std::move(Err);

  Expected<SmallVector<RetainedDWPRewriteUnitDescriptor, 4>> DescriptorsOrErr =
      buildRetainedDWPRewriteUnitDescriptors(*DWPContext, Plan.Units);
  if (!DescriptorsOrErr)
    return DescriptorsOrErr.takeError();
  const SmallVector<RetainedDWPRewriteUnitDescriptor, 4> &Descriptors =
      *DescriptorsOrErr;

  SmallVector<RetainedDWPRewriteUnitAnalysis, 4> UnitAnalyses(Plan.Units.size());
  if (UseParallel) {
    SmallVector<RetainedDWPRewriteUnitAnalysis, 4> PackageOrderAnalyses(
        Descriptors.size());
    if (Error Err = executeRetainedDWPUnitTasks(
            ArrayRef<RetainedDWPRewriteUnitDescriptor>(Descriptors), Options,
            MutableArrayRef<RetainedDWPRewriteUnitAnalysis>(PackageOrderAnalyses),
            [&](size_t, const RetainedDWPRewriteUnitDescriptor &Descriptor)
                -> Expected<RetainedDWPRewriteUnitAnalysis> {
              return analyzeRetainedDWPRewriteUnitTask(
                  LinkMap, Options.DWPFileName,
                  Plan.Units[Descriptor.SortedUnitIndex], Descriptor);
            }))
      return std::move(Err);
    for (size_t I = 0; I != Descriptors.size(); ++I)
      UnitAnalyses[Descriptors[I].SortedUnitIndex] =
          std::move(PackageOrderAnalyses[I]);
  } else {
    for (const RetainedDWPRewriteUnitDescriptor &Descriptor : Descriptors) {
      Expected<DWARFUnit *> CUOrErr =
          getRetainedDWPCompileUnit(*DWPContext, Descriptor);
      if (!CUOrErr)
        return CUOrErr.takeError();

      Expected<RetainedDWPRewriteUnitAnalysis> AnalysisOrErr =
          analyzeRetainedDWPRewriteUnit(
              LinkMap, Plan.Units[Descriptor.SortedUnitIndex].Unit,
              (*CUOrErr)->getUnitDIE(/*ExtractUnitDIEOnly=*/false),
              SectionContents.lookup(DW_SECT_STR_OFFSETS),
              SectionContents.lookup(DW_SECT_EXT_LOC),
              SectionContents.lookup(DW_SECT_RNGLISTS));
      if (!AnalysisOrErr)
        return AnalysisOrErr.takeError();
      UnitAnalyses[Descriptor.SortedUnitIndex] = std::move(*AnalysisOrErr);
    }
  }

  Expected<RetainedDWPStringSectionInfo> RewrittenStringSectionOrErr =
      buildRetainedDWPStringSection(Descriptors, DebugStrContents,
                                    UnitAnalyses);
  if (!RewrittenStringSectionOrErr)
    return RewrittenStringSectionOrErr.takeError();
  const DenseMap<uint64_t, DenseMap<uint64_t, uint64_t>> &StringOffsetRemaps =
      RewrittenStringSectionOrErr->StringOffsetRemapsByDWOId;

  SmallVector<RetainedDWPRewriteUnitContents, 4> UnitContents(Plan.Units.size());
  if (UseParallel) {
    SmallVector<RetainedDWPRewriteUnitContents, 4> PackageOrderContents(
        Descriptors.size());
    if (Error Err = executeRetainedDWPUnitTasks(
            ArrayRef<RetainedDWPRewriteUnitDescriptor>(Descriptors), Options,
            MutableArrayRef<RetainedDWPRewriteUnitContents>(PackageOrderContents),
            [&](size_t, const RetainedDWPRewriteUnitDescriptor &Descriptor)
                -> Expected<RetainedDWPRewriteUnitContents> {
              const RetainedDWPRewriteUnitAnalysis &Analysis =
                  UnitAnalyses[Descriptor.SortedUnitIndex];
              return rewriteRetainedDWPUnitTask(
                  LinkMap, Options.DWPFileName, Descriptor, Analysis,
                  StringOffsetRemaps.lookup(Descriptor.DWOId));
            }))
      return std::move(Err);
    for (size_t I = 0; I != Descriptors.size(); ++I)
      UnitContents[Descriptors[I].SortedUnitIndex] =
          std::move(PackageOrderContents[I]);
  } else {
    for (const RetainedDWPRewriteUnitDescriptor &Descriptor : Descriptors) {
      Expected<DWARFUnit *> CUOrErr =
          getRetainedDWPCompileUnit(*DWPContext, Descriptor);
      if (!CUOrErr)
        return CUOrErr.takeError();

      Expected<RetainedDWPRewriteUnitContents> ContentsOrErr =
          rewriteRetainedDWPUnit(
              LinkMap, (*CUOrErr)->getUnitDIE(/*ExtractUnitDIEOnly=*/false),
              SectionContents, UnitAnalyses[Descriptor.SortedUnitIndex],
              StringOffsetRemaps.lookup(Descriptor.DWOId));
      if (!ContentsOrErr)
        return ContentsOrErr.takeError();
      UnitContents[Descriptor.SortedUnitIndex] = std::move(*ContentsOrErr);
    }
  }

  DenseMap<uint64_t, size_t> RewriteUnitIndices;
  for (size_t I = 0; I != UnitContents.size(); ++I)
    RewriteUnitIndices.try_emplace(UnitContents[I].DWOId, I);

  for (size_t I = 0; I != Plan.Units.size(); ++I) {
    const RetainedDWPPackageUnitInfo &Unit = Plan.Units[I];
    const RetainedDWPRewriteUnitContents &RewrittenUnitContents = UnitContents[I];
    for (const PackageUnitInfo::SectionContributionInfo &Contribution :
         Unit.Contributions) {
      RetainedDWPSectionContributionInfo &Stored =
          Plan.Contributions.emplace_back();
      Stored.DWOId = Unit.Unit.DWOId;
      Stored.Kind = Contribution.Kind;
      if (std::optional<StringRef> SectionName = getDWPSectionName(Contribution.Kind))
        Stored.SectionName = std::string(*SectionName);
      Stored.SectionSize = SectionSizes.lookup(Contribution.Kind);
      Stored.Offset = Contribution.Offset;
      Stored.InputLength = Contribution.Length;
      Stored.Length = Contribution.Length;
      if (std::optional<StringRef> RewrittenContents =
              getRewrittenUnitContents(Contribution.Kind, RewrittenUnitContents))
        Stored.Length = RewrittenContents->size();
    }
  }

  llvm::sort(Plan.Contributions,
             [](const RetainedDWPSectionContributionInfo &LHS,
                const RetainedDWPSectionContributionInfo &RHS) {
               return std::tie(LHS.Kind, LHS.Offset, LHS.Length, LHS.DWOId) <
                       std::tie(RHS.Kind, RHS.Offset, RHS.Length, RHS.DWOId);
             });

  DenseMap<DWARFSectionKind, uint64_t> NextOutputOffsets;
  for (RetainedDWPSectionContributionInfo &Contribution : Plan.Contributions) {
    Contribution.OutputOffset = NextOutputOffsets[Contribution.Kind];
    NextOutputOffsets[Contribution.Kind] += Contribution.Length;
  }

  for (DWARFSectionKind Kind : {DW_SECT_INFO, DW_SECT_ABBREV,
                                DW_SECT_STR_OFFSETS, DW_SECT_RNGLISTS,
                                DW_SECT_EXT_LOC}) {
    SmallVector<RetainedDWPSectionContributionInfo, 8> Contributions =
        Plan.getContributionsForKind(Kind);
    if (Contributions.empty())
      continue;

    RetainedDWPSectionPlan &SectionPlan = Plan.SectionPlans.emplace_back();
    SectionPlan.Kind = Kind;
    SectionPlan.SectionName = Contributions.front().SectionName;
    SectionPlan.InputSize = Contributions.front().SectionSize;
    SectionPlan.OutputSize = Plan.getTotalContributionLength(Kind);
    SectionPlan.Contributions = std::move(Contributions);

    SectionPlan.Contents.reserve(SectionPlan.OutputSize);
    for (const RetainedDWPSectionContributionInfo &Contribution :
         SectionPlan.Contributions) {
      auto It = RewriteUnitIndices.find(Contribution.DWOId);
      assert(It != RewriteUnitIndices.end() &&
             "missing rewritten unit contents");
      std::optional<StringRef> RewrittenContents =
          getRewrittenUnitContents(Kind, UnitContents[It->second]);
      assert(RewrittenContents &&
             "rewrite section plan expects rewritten contents for known kinds");
      if (Error Err = validateRewrittenUnitContentsSize(
              Kind, Contribution.DWOId, Contribution.Length, *RewrittenContents))
        return Err;
      SectionPlan.Contents.append(*RewrittenContents);
    }

    RetainedDWPOutputSection &OutputSection = Plan.OutputSections.emplace_back();
    OutputSection.Kind = Kind;
    OutputSection.Name = SectionPlan.SectionName;
    OutputSection.Contents = SectionPlan.Contents;
  }

  if (!RewrittenStringSectionOrErr->Contents.empty()) {
    RetainedDWPOutputSection &OutputSection = Plan.OutputSections.emplace_back();
    OutputSection.Kind = DW_SECT_EXT_unknown;
    OutputSection.Name = ".debug_str.dwo";
    OutputSection.Contents = RewrittenStringSectionOrErr->Contents;
  }

  for (DWARFSectionKind Kind : {DW_SECT_INFO, DW_SECT_ABBREV,
                                DW_SECT_STR_OFFSETS, DW_SECT_RNGLISTS,
                                DW_SECT_EXT_LOC}) {
    const RetainedDWPOutputSection *Section = Plan.getOutputSection(Kind);
    if (!Section)
      continue;
    Plan.OutputManifest.Sections.push_back(*Section);
    RetainedDWPOutputLayoutEntry &Layout =
        Plan.OutputManifest.Layout.emplace_back();
    Layout.Kind = Section->Kind;
    Layout.Name = Section->Name;
    Layout.Offset = Plan.OutputManifest.TotalSize;
    Layout.Size = Section->Contents.size();
    RetainedDWPOutputRecord &Record = Plan.OutputManifest.Records.emplace_back();
    Record.Kind = Section->Kind;
    Record.Name = Section->Name;
    Record.PayloadOffset = Layout.Offset;
    Record.PayloadSize = Layout.Size;
    Plan.OutputManifest.TotalSize += Section->Contents.size();
  }

  for (const RetainedDWPOutputSection &Section : Plan.OutputSections) {
    if (Section.Kind != DW_SECT_EXT_unknown)
      continue;
    Plan.OutputManifest.Sections.push_back(Section);
    RetainedDWPOutputLayoutEntry &Layout =
        Plan.OutputManifest.Layout.emplace_back();
    Layout.Kind = Section.Kind;
    Layout.Name = Section.Name;
    Layout.Offset = Plan.OutputManifest.TotalSize;
    Layout.Size = Section.Contents.size();
    RetainedDWPOutputRecord &Record = Plan.OutputManifest.Records.emplace_back();
    Record.Kind = Section.Kind;
    Record.Name = Section.Name;
    Record.PayloadOffset = Layout.Offset;
    Record.PayloadSize = Layout.Size;
    Plan.OutputManifest.TotalSize += Section.Contents.size();
  }

  Plan.OutputManifest.PayloadImage.reserve(Plan.OutputManifest.TotalSize);
  for (const RetainedDWPOutputLayoutEntry &Layout : Plan.OutputManifest.Layout) {
    const RetainedDWPOutputSection *Section = Plan.getOutputSection(Layout.Kind);
    assert(Section && "layout must reference an existing output section");
    Plan.OutputManifest.PayloadImage.append(Section->Contents);
  }

  return Plan;
}

SmallVector<RetainedDWPSectionContributionInfo, 8>
RetainedDWPRewritePlan::getContributionsForKind(DWARFSectionKind Kind) const {
  SmallVector<RetainedDWPSectionContributionInfo, 8> Filtered;
  for (const RetainedDWPSectionContributionInfo &Contribution : Contributions)
    if (Contribution.Kind == Kind)
      Filtered.push_back(Contribution);
  return Filtered;
}

uint64_t
RetainedDWPRewritePlan::getTotalContributionLength(DWARFSectionKind Kind) const {
  uint64_t TotalLength = 0;
  for (const RetainedDWPSectionContributionInfo &Contribution : Contributions)
    if (Contribution.Kind == Kind)
      TotalLength += Contribution.Length;
  return TotalLength;
}

const RetainedDWPSectionPlan *
RetainedDWPRewritePlan::getSectionPlan(DWARFSectionKind Kind) const {
  for (const RetainedDWPSectionPlan &Plan : SectionPlans)
    if (Plan.Kind == Kind)
      return &Plan;
  return nullptr;
}

const RetainedDWPOutputSection *
RetainedDWPRewritePlan::getOutputSection(DWARFSectionKind Kind) const {
  for (const RetainedDWPOutputSection &Section : OutputSections)
    if (Section.Kind == Kind)
      return &Section;
  return nullptr;
}

std::optional<StringRef>
RetainedDWPOutputManifest::getPayloadSlice(DWARFSectionKind Kind) const {
  for (const RetainedDWPOutputLayoutEntry &Entry : Layout) {
    if (Entry.Kind != Kind)
      continue;
    if (Entry.Offset + Entry.Size > PayloadImage.size())
      return std::nullopt;
    return StringRef(PayloadImage).slice(Entry.Offset, Entry.Offset + Entry.Size);
  }
  return std::nullopt;
}

const RetainedDWPOutputRecord *
RetainedDWPOutputManifest::findRecord(DWARFSectionKind Kind) const {
  for (const RetainedDWPOutputRecord &Record : Records)
    if (Record.Kind == Kind)
      return &Record;
  return nullptr;
}

static Error collectSubprograms(DWARFDie RootDie,
                                const AddressRanges &ExecutableRanges,
                                SkeletonUnitInfo &Info,
                                DenseSet<uint64_t> &ExpandedSubtreeOffsets,
                                DenseSet<uint64_t> &ExpandedReferenceOffsets) {
  SmallVector<DWARFDie, 8> Worklist;
  DenseSet<uint64_t> RetainedOffsets;
  Worklist.push_back(RootDie);

  while (!Worklist.empty()) {
    DWARFDie Die = Worklist.pop_back_val();
    if (Die.isSubprogramDIE()) {
      Expected<DWARFAddressRangesVector> RangesOrErr = Die.getAddressRanges();
      if (!RangesOrErr)
        return RangesOrErr.takeError();
      if (!RangesOrErr->empty()) {
        SkeletonUnitInfo::SubprogramInfo Subprogram;
        Subprogram.Offset = getUnitRelativeDIEOffset(Die);
        Subprogram.Name =
            dwarf::toString(Die.findRecursively({dwarf::DW_AT_linkage_name,
                                                 dwarf::DW_AT_name}),
                            "");
        copyAddressRanges(Subprogram.ResolvedRanges, *RangesOrErr);
        keepLiveRanges(ExecutableRanges, Subprogram.LiveRanges, *RangesOrErr);

        if (Subprogram.isLive()) {
          Info.LiveRootOffsets.push_back(getUnitRelativeDIEOffset(Die));
          addRetainedDIEChain(Die, Info, RetainedOffsets);
          addRetainedDIESubtree(Die, Info, RetainedOffsets,
                                ExpandedSubtreeOffsets);
          addReferencedDIEClosure(Die, Info, RetainedOffsets,
                                  ExpandedSubtreeOffsets,
                                  ExpandedReferenceOffsets);
          for (const auto &Range : Subprogram.LiveRanges)
            Info.LiveRanges.push_back(Range);
        }

        Info.Subprograms.push_back(std::move(Subprogram));
      }
    }

    for (DWARFDie Child : reverse(Die.children()))
      Worklist.push_back(Child);
  }
  return Error::success();
}

static Error validateLiveSubprograms(const DWPLinkMap &LinkMap,
                                     const DWARFDie &RootDie) {
  SmallVector<DWARFDie, 8> Worklist;
  Worklist.push_back(RootDie);

  while (!Worklist.empty()) {
    DWARFDie Die = Worklist.pop_back_val();
    if (Die.isSubprogramDIE()) {
      Expected<DWARFAddressRangesVector> RangesOrErr = Die.getAddressRanges();
      if (!RangesOrErr)
        return RangesOrErr.takeError();
      if (!RangesOrErr->empty()) {
        Expected<bool> IsLiveOrErr = isLiveSubprogramDIE(LinkMap, Die);
        if (!IsLiveOrErr)
          return IsLiveOrErr.takeError();
      }
    }

    for (DWARFDie Child : reverse(Die.children()))
      Worklist.push_back(Child);
  }

  return Error::success();
}

static Error validateRetainedDIEs(const DWPLinkMap &LinkMap,
                                  const DWARFDie &RootDie) {
  Expected<RetainedDWPUnitInfo> RetainedUnitInfoOrErr =
      collectRetainedDWPUnitInfo(LinkMap, RootDie);
  if (!RetainedUnitInfoOrErr)
    return RetainedUnitInfoOrErr.takeError();

  std::optional<uint64_t> DWOId = getUnitDWOId(RootDie);
  if (!DWOId)
    return createStringError(std::errc::invalid_argument,
                             "unit DIE does not belong to a split unit");

  const LinkedSplitUnit *Linked = LinkMap.findLinkedUnit(*DWOId);
  if (!Linked)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to find linked split unit for DWO_id {0:x16}", *DWOId)
            .str()
            .c_str());

  if (RetainedUnitInfoOrErr->RetainedDIEs.size() !=
      Linked->Skeleton.RetainedDIEOffsets.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("retained DIE count mismatch for DWO_id {0:x16}: {1} DIEs vs "
                "{2} offsets",
                *DWOId, RetainedUnitInfoOrErr->RetainedDIEs.size(),
                Linked->Skeleton.RetainedDIEOffsets.size())
            .str()
            .c_str());

  DenseSet<uint64_t> RetainedOffsets;
  for (const RetainedDWPDieInfo &Info : RetainedUnitInfoOrErr->RetainedDIEs)
    RetainedOffsets.insert(Info.Offset);

  if (RetainedUnitInfoOrErr->DWOId != *DWOId)
    return createStringError(
        std::errc::invalid_argument,
        formatv("retained unit DWO_id mismatch: expected {0:x16}, got {1:x16}",
                *DWOId, RetainedUnitInfoOrErr->DWOId)
            .str()
            .c_str());

  for (const RetainedDWPDieInfo &Info : RetainedUnitInfoOrErr->RetainedDIEs) {
    if (Info.ParentOffset && !RetainedOffsets.contains(*Info.ParentOffset))
      return createStringError(
          std::errc::invalid_argument,
          formatv("retained DIE 0x{0:x} references non-retained parent 0x{1:x}",
                  Info.Offset, *Info.ParentOffset)
              .str()
              .c_str());
  }

  return Error::success();
}

static Error resolveSplitUnitInfo(DWARFUnit &SkeletonUnit,
                                  const AddressRanges &ExecutableRanges,
                                  SkeletonUnitInfo &Info,
                                  StringRef DWPFileName) {
  DenseSet<uint64_t> ExpandedSubtreeOffsets;
  DenseSet<uint64_t> ExpandedReferenceOffsets;
  DWARFDie SplitUnitDie = SkeletonUnit.getNonSkeletonUnitDIE(
      /*ExtractUnitDIEOnly=*/false, DWPFileName);
  if (!SplitUnitDie)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to resolve split unit for DWO_id {0:x16} from '{1}'",
                Info.DWOId, DWPFileName)
            .str()
            .c_str());

  Info.SplitUnitName =
      dwarf::toString(SplitUnitDie.find(dwarf::DW_AT_name), "");

  if (std::optional<object::SectionedAddress> BaseAddress =
          SplitUnitDie.getDwarfUnit()->getBaseAddress())
    Info.BaseAddress = BaseAddress->Address;

  Info.RetainedDIEOffsets.push_back(getUnitRelativeDIEOffset(SplitUnitDie));

  Expected<DWARFAddressRangesVector> RangesOrErr =
      SplitUnitDie.getAddressRanges();
  if (!RangesOrErr)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to resolve address ranges for DWO_id {0:x16}: {1}",
                Info.DWOId, toString(RangesOrErr.takeError()))
            .str()
            .c_str());
  copyAddressRanges(Info.ResolvedRanges, *RangesOrErr);

  if (Error Err = collectSubprograms(SplitUnitDie, ExecutableRanges, Info,
                                     ExpandedSubtreeOffsets,
                                     ExpandedReferenceOffsets))
    return createStringError(
        std::errc::invalid_argument,
        formatv("unable to inspect subprogram ranges for DWO_id {0:x16}: {1}",
                Info.DWOId, toString(std::move(Err)))
            .str()
            .c_str());
  expandRetainedReferenceClosure(SplitUnitDie, Info, ExpandedSubtreeOffsets,
                                 ExpandedReferenceOffsets);

  finalizeLiveUnitInfo(Info);
  return Error::success();
}

static Error collectSkeletonUnits(DWARFContext &Context, DWPLinkMap &LinkMap,
                                  const AddressRanges &ExecutableRanges,
                                  StringRef DWPFileName) {
  for (const std::unique_ptr<DWARFUnit> &CU : Context.compile_units()) {
    if (!isCompanionMainUnit(*CU))
      continue;

    if (CU->getVersion() != 4)
      return createStringError(
          std::errc::invalid_argument,
          formatv("unsupported companion compile unit version {0} in '{1}'; "
                  "current DWP support only handles DWARF4",
                  CU->getVersion(), getUnitName(*CU))
              .str()
              .c_str());

    SkeletonUnitInfo &Info = LinkMap.SkeletonUnits.emplace_back();
    Info.DWOId = *CU->getDWOId();
    Info.Name = getUnitName(*CU);
    Info.DWOName = getUnitDWOName(*CU);

    if (Error Err =
            resolveSplitUnitInfo(*CU, ExecutableRanges, Info, DWPFileName))
      return Err;
  }

  return Error::success();
}

static Error validateSupportedDWPBoundary(const ObjectFile &DWPObject,
                                          DWARFContext &DWPContext,
                                          StringRef DWPFileName) {
  if (DWPContext.getNumDWOTypeUnits() != 0)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unsupported DWP input '{0}': type units are not supported in "
                "the current DWARF4-only path",
                DWPFileName)
            .str()
            .c_str());

  for (const SectionRef &Section : DWPObject.sections()) {
    Expected<StringRef> NameOrErr = Section.getName();
    if (!NameOrErr)
      return NameOrErr.takeError();
    StringRef Name = *NameOrErr;
    if (Name == ".debug_tu_index" || Name == ".debug_types.dwo")
      return createStringError(
          std::errc::invalid_argument,
          formatv("unsupported DWP input '{0}': section '{1}' belongs to the "
                  "type-unit path, which is not supported yet",
                  DWPFileName, Name)
              .str()
              .c_str());
    if (Name == ".debug_loclists.dwo" || Name == ".debug_rnglists.dwo")
      return createStringError(
          std::errc::invalid_argument,
          formatv("unsupported DWP input '{0}': section '{1}' belongs to the "
                  "DWARF5 package path, while current support is limited to "
                  "DWARF4",
                  DWPFileName, Name)
              .str()
              .c_str());
  }

  for (const std::unique_ptr<DWARFUnit> &CU : DWPContext.dwo_compile_units()) {
    if (CU->getVersion() != 4)
      return createStringError(
          std::errc::invalid_argument,
          formatv("unsupported DWP input '{0}': package compile unit '{1}' has "
                  "DWARF version {2}, but current support only handles "
                  "DWARF4",
                  DWPFileName, getUnitName(*CU), CU->getVersion())
              .str()
              .c_str());
  }

  return Error::success();
}

static void collectPackageUnits(DWARFContext &Context, DWPLinkMap &LinkMap) {
  const DWARFUnitIndex &CUIndex = Context.getCUIndex();
  LinkMap.CUIndexVersion = CUIndex.getVersion();
  LinkMap.CUIndexColumnKinds.assign(CUIndex.getColumnKinds().begin(),
                                    CUIndex.getColumnKinds().end());

  for (const std::unique_ptr<DWARFUnit> &CU : Context.dwo_compile_units()) {
    std::optional<uint64_t> DWOId = CU->getDWOId();
    if (!DWOId)
      continue;

    PackageUnitInfo &Info = LinkMap.PackageUnits.emplace_back();
    Info.DWOId = *DWOId;
    Info.Name = getUnitName(*CU);
    Info.DWOName = getUnitDWOName(*CU);

    const DWARFUnitIndex::Entry *IndexEntry = CUIndex.getFromHash(*DWOId);
    if (!IndexEntry)
      continue;

    for (DWARFSectionKind Kind : CUIndex.getColumnKinds()) {
      const auto *Contribution = IndexEntry->getContribution(Kind);
      if (!Contribution)
        continue;

      auto &Stored = Info.Contributions.emplace_back();
      Stored.Kind = Kind;
      Stored.Offset = Contribution->getOffset();
      Stored.Length = Contribution->getLength();
    }
  }
}

static Error validatePackageUnits(const DWPLinkMap &LinkMap,
                                  StringRef DWPFileName) {
  for (const PackageUnitInfo &Info : LinkMap.PackageUnits) {
    if (Info.Contributions.empty())
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to find CU index contribution for DWO_id {0:x16} "
                  "in '{1}'",
                  Info.DWOId, DWPFileName)
              .str()
              .c_str());
  }

  return Error::success();
}

static std::string formatContribution(
    const PackageUnitInfo::SectionContributionInfo &Contribution) {
  return formatv("{0}: off={1:x}, len={2:x}", toString(Contribution.Kind),
                 Contribution.Offset, Contribution.Length)
      .str();
}

static std::string formatRetainedContribution(
    const RetainedDWPSectionContributionInfo &Contribution) {
  return formatv("{0}: section='{1}', size={2:x}, off={3:x}, in-len={4:x}, "
                 "len={5:x}, out={6:x}, dwo_id={7:x16}",
                 toString(Contribution.Kind), Contribution.SectionName,
                 Contribution.SectionSize, Contribution.Offset,
                 Contribution.InputLength, Contribution.Length,
                 Contribution.OutputOffset, Contribution.DWOId)
      .str();
}

static std::string
formatAddressRange(const SkeletonUnitInfo::AddressRangeInfo &Range) {
  return formatv("[{0:x}, {1:x}) section={2}", Range.LowPC, Range.HighPC,
                 Range.SectionIndex)
      .str();
}

bool SkeletonUnitInfo::containsLiveAddress(uint64_t Address) const {
  for (const AddressRangeInfo &Range : LiveRanges) {
    if (Range.LowPC <= Address && Address < Range.HighPC)
      return true;
  }
  return false;
}

bool SkeletonUnitInfo::containsLiveRange(uint64_t LowPC, uint64_t HighPC) const {
  if (LowPC >= HighPC)
    return false;

  for (const AddressRangeInfo &Range : LiveRanges) {
    if (Range.LowPC <= LowPC && HighPC <= Range.HighPC)
      return true;
  }
  return false;
}

const SkeletonUnitInfo *DWPLinkMap::findSkeletonUnit(uint64_t DWOId) const {
  auto It = SkeletonUnitIndices.find(DWOId);
  if (It != SkeletonUnitIndices.end())
    return &SkeletonUnits[It->second];
  return nullptr;
}

const PackageUnitInfo *DWPLinkMap::findPackageUnit(uint64_t DWOId) const {
  auto It = PackageUnitIndices.find(DWOId);
  if (It != PackageUnitIndices.end())
    return &PackageUnits[It->second];
  return nullptr;
}

const LinkedSplitUnit *DWPLinkMap::findLinkedUnit(uint64_t DWOId) const {
  auto It = LinkedUnitIndices.find(DWOId);
  if (It != LinkedUnitIndices.end())
    return &LinkedUnits[It->second];
  return nullptr;
}

const RetainedDWPPackageUnitInfo *
DWPLinkMap::findRetainedPackageUnit(uint64_t DWOId) const {
  auto It = RetainedPackageUnitIndices.find(DWOId);
  if (It != RetainedPackageUnitIndices.end())
    return &RetainedPackageUnits[It->second];
  return nullptr;
}

void DWPLinkMap::rebuildLookupCaches() {
  SkeletonUnitIndices.clear();
  for (size_t I = 0; I != SkeletonUnits.size(); ++I)
    SkeletonUnitIndices.try_emplace(SkeletonUnits[I].DWOId, I);

  PackageUnitIndices.clear();
  for (size_t I = 0; I != PackageUnits.size(); ++I)
    PackageUnitIndices.try_emplace(PackageUnits[I].DWOId, I);

  LinkedUnitIndices.clear();
  for (size_t I = 0; I != LinkedUnits.size(); ++I)
    LinkedUnitIndices.try_emplace(LinkedUnits[I].Skeleton.DWOId, I);

  RetainedPackageUnitIndices.clear();
  for (size_t I = 0; I != RetainedPackageUnits.size(); ++I)
    RetainedPackageUnitIndices.try_emplace(RetainedPackageUnits[I].Unit.DWOId, I);
}

static Error validateRetainedRewritePlan(const DWPLinkMap &LinkMap) {
  for (DWARFSectionKind Kind : {DW_SECT_INFO, DW_SECT_ABBREV,
                                DW_SECT_STR_OFFSETS, DW_SECT_RNGLISTS,
                                DW_SECT_EXT_LOC}) {
    SmallVector<RetainedDWPSectionContributionInfo, 8> Contributions =
        LinkMap.RewritePlan.getContributionsForKind(Kind);
    if (Contributions.empty())
      continue;

    llvm::sort(Contributions,
               [](const RetainedDWPSectionContributionInfo &LHS,
                  const RetainedDWPSectionContributionInfo &RHS) {
                 return std::tie(LHS.Offset, LHS.Length, LHS.DWOId) <
                        std::tie(RHS.Offset, RHS.Length, RHS.DWOId);
               });

    for (size_t I = 1; I < Contributions.size(); ++I) {
      const auto &Prev = Contributions[I - 1];
      const auto &Curr = Contributions[I];
      if (Prev.Offset + Prev.InputLength > Curr.Offset)
        return createStringError(
            std::errc::invalid_argument,
            formatv("overlapping retained rewrite contributions in {0}: {1} "
                    "overlaps {2}",
                    toString(Kind), formatRetainedContribution(Prev),
                    formatRetainedContribution(Curr))
                .str()
                .c_str());
    }

    uint64_t ExpectedOutputOffset = 0;
    for (const auto &Contribution : Contributions) {
      if (Contribution.SectionName.empty())
        return createStringError(
            std::errc::invalid_argument,
            formatv("missing input section name for retained contribution {0}",
                    formatRetainedContribution(Contribution))
                .str()
                .c_str());
      if (Contribution.Offset + Contribution.InputLength > Contribution.SectionSize)
        return createStringError(
            std::errc::invalid_argument,
            formatv("retained contribution exceeds input section bounds: {0}",
                    formatRetainedContribution(Contribution))
                .str()
                .c_str());
      if (Contribution.OutputOffset != ExpectedOutputOffset)
        return createStringError(
            std::errc::invalid_argument,
            formatv("non-contiguous output offset in {0}: expected 0x{1:x}, "
                    "got {2}",
                    toString(Kind), ExpectedOutputOffset,
                    formatRetainedContribution(Contribution))
                .str()
                .c_str());
      ExpectedOutputOffset += Contribution.Length;
    }
  }

  if (LinkMap.RewritePlan.Units.size() != LinkMap.RetainedPackageUnits.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("rewrite plan unit count mismatch: expected {0}, got {1}",
                LinkMap.RetainedPackageUnits.size(),
                LinkMap.RewritePlan.Units.size())
            .str()
            .c_str());

  for (const RetainedDWPSectionPlan &Plan : LinkMap.RewritePlan.SectionPlans) {
    if (Plan.Contributions.empty())
      return createStringError(
          std::errc::invalid_argument,
          formatv("empty rewrite section plan for {0}", toString(Plan.Kind))
              .str()
              .c_str());
    if (Plan.OutputSize !=
        LinkMap.RewritePlan.getTotalContributionLength(Plan.Kind))
      return createStringError(
          std::errc::invalid_argument,
          formatv("rewrite section output size mismatch for {0}: expected "
                  "0x{1:x}, got 0x{2:x}",
                  toString(Plan.Kind),
                  LinkMap.RewritePlan.getTotalContributionLength(Plan.Kind),
                  Plan.OutputSize)
              .str()
              .c_str());
    if (Plan.Contents.size() != Plan.OutputSize)
      return createStringError(
          std::errc::invalid_argument,
          formatv("materialized rewrite section size mismatch for {0}: "
                  "expected 0x{1:x}, got 0x{2:x}",
                  toString(Plan.Kind), Plan.OutputSize, Plan.Contents.size())
              .str()
              .c_str());

    const RetainedDWPOutputSection *OutputSection =
        LinkMap.RewritePlan.getOutputSection(Plan.Kind);
    if (!OutputSection)
      return createStringError(
          std::errc::invalid_argument,
          formatv("missing output section for rewrite plan {0}",
                  toString(Plan.Kind))
              .str()
              .c_str());
    if (OutputSection->Name != Plan.SectionName)
      return createStringError(
          std::errc::invalid_argument,
          formatv("output section name mismatch for {0}: expected '{1}', got "
                  "'{2}'",
                  toString(Plan.Kind), Plan.SectionName, OutputSection->Name)
              .str()
              .c_str());
    if (OutputSection->Contents != Plan.Contents)
      return createStringError(
          std::errc::invalid_argument,
          formatv("output section payload mismatch for {0}", toString(Plan.Kind))
              .str()
              .c_str());
  }

  uint64_t ComputedTotalSize = 0;
  StringSet<> SeenOutputSectionNames;
  for (size_t I = 0; I < LinkMap.RewritePlan.OutputManifest.Sections.size(); ++I) {
    const RetainedDWPOutputSection &Section =
        LinkMap.RewritePlan.OutputManifest.Sections[I];
    ComputedTotalSize += Section.Contents.size();
    if (!SeenOutputSectionNames.insert(Section.Name).second)
      return createStringError(
          std::errc::invalid_argument,
          formatv("rewrite output manifest contains duplicate section '{0}'",
                  Section.Name)
              .str()
              .c_str());
  }
  if (LinkMap.RewritePlan.OutputManifest.Layout.size() !=
      LinkMap.RewritePlan.OutputManifest.Sections.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("rewrite output manifest layout count mismatch: expected {0}, "
                "got {1}",
                LinkMap.RewritePlan.OutputManifest.Sections.size(),
                LinkMap.RewritePlan.OutputManifest.Layout.size())
            .str()
            .c_str());
  if (LinkMap.RewritePlan.OutputManifest.Records.size() !=
      LinkMap.RewritePlan.OutputManifest.Sections.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("rewrite output manifest record count mismatch: expected {0}, "
                "got {1}",
                LinkMap.RewritePlan.OutputManifest.Sections.size(),
                LinkMap.RewritePlan.OutputManifest.Records.size())
            .str()
            .c_str());
  for (size_t I = 0; I < LinkMap.RewritePlan.OutputManifest.Layout.size(); ++I) {
    const RetainedDWPOutputLayoutEntry &Layout =
        LinkMap.RewritePlan.OutputManifest.Layout[I];
    const RetainedDWPOutputSection &Section =
        LinkMap.RewritePlan.OutputManifest.Sections[I];
    const RetainedDWPOutputRecord &Record =
        LinkMap.RewritePlan.OutputManifest.Records[I];
    if (Layout.Kind != Section.Kind || Layout.Name != Section.Name ||
        Layout.Size != Section.Contents.size())
      return createStringError(
          std::errc::invalid_argument,
          formatv("rewrite output layout entry mismatch for section '{0}'",
                  Section.Name)
              .str()
              .c_str());
    if (Record.Kind != Section.Kind || Record.Name != Section.Name ||
        Record.PayloadOffset != Layout.Offset ||
        Record.PayloadSize != Layout.Size)
      return createStringError(
          std::errc::invalid_argument,
          formatv("rewrite output record mismatch for section '{0}'",
                  Section.Name)
              .str()
              .c_str());
    if (I != 0) {
      const RetainedDWPOutputLayoutEntry &Prev =
          LinkMap.RewritePlan.OutputManifest.Layout[I - 1];
      if (Prev.Offset + Prev.Size != Layout.Offset)
        return createStringError(
            std::errc::invalid_argument,
            formatv("rewrite output layout is not contiguous between '{0}' and "
                    "'{1}'",
                    Prev.Name, Layout.Name)
                .str()
                .c_str());
    } else if (Layout.Offset != 0) {
      return createStringError(
          std::errc::invalid_argument,
          formatv("first rewrite output layout entry does not start at zero: "
                  "'{0}' offset=0x{1:x}",
                  Layout.Name, Layout.Offset)
              .str()
              .c_str());
    }
  }
  if (ComputedTotalSize != LinkMap.RewritePlan.OutputManifest.TotalSize)
    return createStringError(
        std::errc::invalid_argument,
        formatv("rewrite output manifest total size mismatch: expected 0x{0:x}, "
                "got 0x{1:x}",
                LinkMap.RewritePlan.OutputManifest.TotalSize,
                ComputedTotalSize)
            .str()
            .c_str());
  if (LinkMap.RewritePlan.OutputManifest.PayloadImage.size() !=
      LinkMap.RewritePlan.OutputManifest.TotalSize)
    return createStringError(
        std::errc::invalid_argument,
        formatv("rewrite output manifest payload image size mismatch: expected "
                "0x{0:x}, got 0x{1:x}",
                LinkMap.RewritePlan.OutputManifest.TotalSize,
                LinkMap.RewritePlan.OutputManifest.PayloadImage.size())
            .str()
            .c_str());
  for (const RetainedDWPOutputSection &Section :
       LinkMap.RewritePlan.OutputManifest.Sections) {
    std::optional<StringRef> Slice =
        LinkMap.RewritePlan.OutputManifest.getPayloadSlice(Section.Kind);
    if (!Slice)
      return createStringError(
          std::errc::invalid_argument,
          formatv("missing payload slice for output section '{0}'", Section.Name)
              .str()
              .c_str());
    if (*Slice != Section.Contents)
      return createStringError(
          std::errc::invalid_argument,
          formatv("payload slice mismatch for output section '{0}'", Section.Name)
              .str()
              .c_str());
    const RetainedDWPOutputRecord *Record =
        LinkMap.RewritePlan.OutputManifest.findRecord(Section.Kind);
    if (!Record)
      return createStringError(
          std::errc::invalid_argument,
          formatv("missing output record for section '{0}'", Section.Name)
              .str()
              .c_str());
  }

  return Error::success();
}

Expected<DWPLinkMap> loadDWPLinkMap(const object::ObjectFile &InputFile,
                                    const Options &Options) {
  assert(Options.hasDWPInput() && "DWP link map requested without DWP input");

  DWPLinkMap LinkMap;
  std::unique_ptr<DWARFContext> InputContext = DWARFContext::create(InputFile);
  AddressRanges ExecutableRanges = collectExecutableAddressRanges(InputFile);
  if (Error Err = collectSkeletonUnits(*InputContext, LinkMap, ExecutableRanges,
                                       Options.DWPFileName))
    return std::move(Err);
  LinkMap.rebuildLookupCaches();

  if (LinkMap.SkeletonUnits.empty())
    return createStringError(
        std::errc::invalid_argument,
        formatv("input '{0}' does not contain any companion compile units "
                "with DWO_id",
                Options.InputFileName)
            .str()
            .c_str());

  Expected<OwningBinary<Binary>> DWPBinOrErr = openBinary(Options.DWPFileName);
  if (!DWPBinOrErr)
    return DWPBinOrErr.takeError();

  auto *DWPObject = cast<ObjectFile>(DWPBinOrErr->getBinary());
  std::unique_ptr<DWARFContext> DWPContext = DWARFContext::create(*DWPObject);
  if (!DWPContext->isDWP())
    return createStringError(
        std::errc::invalid_argument,
        formatv("file '{0}' is not a DWARF package file", Options.DWPFileName)
            .str()
            .c_str());

  if (Error Err =
          validateSupportedDWPBoundary(*DWPObject, *DWPContext, Options.DWPFileName))
    return std::move(Err);

  if (!DWPContext->getCUIndex())
    return createStringError(
        std::errc::invalid_argument,
        formatv("file '{0}' does not contain a valid .debug_cu_index",
                Options.DWPFileName)
            .str()
            .c_str());

  collectPackageUnits(*DWPContext, LinkMap);
  if (Error Err = validatePackageUnits(LinkMap, Options.DWPFileName))
    return std::move(Err);
  LinkMap.rebuildLookupCaches();

  DenseMap<uint64_t, const PackageUnitInfo *> PackageUnitsById;
  for (const PackageUnitInfo &Info : LinkMap.PackageUnits) {
    if (!PackageUnitsById.try_emplace(Info.DWOId, &Info).second)
      return createStringError(
          std::errc::invalid_argument,
          formatv("duplicate package unit with DWO_id {0:x16} in '{1}'",
                  Info.DWOId, Options.DWPFileName)
              .str()
              .c_str());
  }

  for (const SkeletonUnitInfo &Info : LinkMap.SkeletonUnits) {
    auto PackageInfo = PackageUnitsById.find(Info.DWOId);
    if (PackageInfo == PackageUnitsById.end())
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to find matching package unit for DWO_id {0:x16}",
                  Info.DWOId)
              .str()
              .c_str());

    LinkedSplitUnit &Linked = LinkMap.LinkedUnits.emplace_back();
    Linked.Skeleton = Info;
    Linked.Package = *PackageInfo->second;
  }
  LinkMap.rebuildLookupCaches();

  DenseMap<uint64_t, SmallVector<DWARFUnit *, 1>> CompanionUnitsByDWOId =
      indexCompanionMainUnits(*InputContext);
  DenseMap<uint64_t, size_t> NextCompanionUnitIndexByDWOId;
  for (const SkeletonUnitInfo &Info : LinkMap.SkeletonUnits) {
    auto It = CompanionUnitsByDWOId.find(Info.DWOId);
    if (It == CompanionUnitsByDWOId.end())
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to refind companion compile unit for DWO_id {0:x16}",
                  Info.DWOId)
              .str()
              .c_str());

    size_t &NextIndex = NextCompanionUnitIndexByDWOId[Info.DWOId];
    if (NextIndex >= It->second.size())
      return createStringError(
          std::errc::invalid_argument,
          formatv("companion compile unit index overflow for DWO_id {0:x16}",
                  Info.DWOId)
              .str()
              .c_str());

    DWARFDie SplitUnitDie =
        It->second[NextIndex++]->getNonSkeletonUnitDIE(
            /*ExtractUnitDIEOnly=*/false, Options.DWPFileName);
    if (!SplitUnitDie)
      continue;
    if (Error Err = validateLiveSubprograms(LinkMap, SplitUnitDie))
      return Err;
    if (Error Err = validateRetainedDIEs(LinkMap, SplitUnitDie))
      return Err;
  }

  Expected<SmallVector<RetainedDWPPackageUnitInfo, 4>> RetainedUnitsOrErr =
      shouldUseRetainedDWPRewriteParallelism(Options, LinkMap.LinkedUnits.size())
          ? collectRetainedDWPPackageUnitsInParallel(LinkMap, *DWPContext,
                                                     Options, Options.DWPFileName)
          : collectRetainedDWPPackageUnitsFromContext(LinkMap, *DWPContext);
  if (!RetainedUnitsOrErr)
    return RetainedUnitsOrErr.takeError();
  if (RetainedUnitsOrErr->size() != LinkMap.LinkedUnits.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("retained DWP unit count mismatch: expected {0}, got {1}",
                LinkMap.LinkedUnits.size(), RetainedUnitsOrErr->size())
            .str()
            .c_str());

  LinkMap.RetainedPackageUnits = std::move(*RetainedUnitsOrErr);
  LinkMap.rebuildLookupCaches();

  Expected<RetainedDWPRewritePlan> RewritePlanOrErr =
      collectRetainedDWPRewritePlan(LinkMap, Options);
  if (!RewritePlanOrErr)
    return RewritePlanOrErr.takeError();
  if (RewritePlanOrErr->Units.size() != LinkMap.RetainedPackageUnits.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("retained DWP rewrite plan unit count mismatch: expected {0}, "
                "got {1}",
                LinkMap.RetainedPackageUnits.size(),
                RewritePlanOrErr->Units.size())
            .str()
            .c_str());

  LinkMap.RewritePlan = std::move(*RewritePlanOrErr);
  if (Error Err = validateRetainedRewritePlan(LinkMap))
    return Err;

  return LinkMap;
}

Error validateRetainedDWPOutputContainerLinkage(const Options &Options,
                                                const DWPLinkMap &LinkMap) {
  Expected<OwningBinary<Binary>> MainBinOrErr = openBinary(Options.InputFileName);
  if (!MainBinOrErr)
    return MainBinOrErr.takeError();

  auto *MainObject = cast<ObjectFile>(MainBinOrErr->getBinary());
  std::unique_ptr<DWARFContext> MainContext = DWARFContext::create(*MainObject);
  DenseMap<uint64_t, SmallVector<DWARFUnit *, 1>> CompanionUnitsByDWOId =
      indexCompanionMainUnits(*MainContext);
  DenseMap<uint64_t, size_t> NextCompanionUnitIndexByDWOId;
  size_t RevalidatedUnits = 0;

  for (const LinkedSplitUnit &ExpectedUnit : LinkMap.LinkedUnits) {
    auto It = CompanionUnitsByDWOId.find(ExpectedUnit.Skeleton.DWOId);
    if (It == CompanionUnitsByDWOId.end())
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to refind companion compile unit for DWO_id {0:x16}",
                  ExpectedUnit.Skeleton.DWOId)
              .str()
              .c_str());

    size_t &NextIndex = NextCompanionUnitIndexByDWOId[ExpectedUnit.Skeleton.DWOId];
    if (NextIndex >= It->second.size())
      return createStringError(
          std::errc::invalid_argument,
          formatv("companion compile unit index overflow for DWO_id {0:x16}",
                  ExpectedUnit.Skeleton.DWOId)
              .str()
              .c_str());

    DWARFDie SplitUnitDie =
        It->second[NextIndex++]->getNonSkeletonUnitDIE(
            /*ExtractUnitDIEOnly=*/false, Options.OutputFileName);
    if (!SplitUnitDie)
      return createStringError(
          std::errc::invalid_argument,
          formatv("unable to resolve split unit for emitted DWP output and "
                  "DWO_id {0:x16}",
                  ExpectedUnit.Skeleton.DWOId)
              .str()
              .c_str());

    if (Error Err = validateLiveSubprograms(LinkMap, SplitUnitDie))
      return Err;
    if (Error Err = validateRetainedDIEs(LinkMap, SplitUnitDie))
      return Err;
    ++RevalidatedUnits;
  }

  if (RevalidatedUnits != LinkMap.LinkedUnits.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("revalidated DWP linked unit count mismatch: expected {0}, got "
                "{1}",
                LinkMap.LinkedUnits.size(), RevalidatedUnits)
            .str()
            .c_str());

  return Error::success();
}

void logDWPLinkMap(const DWPLinkMap &LinkMap, const Options &Options) {
  if (!Options.Verbose)
    return;

  verbose(formatv("Detected {0} companion compile units in '{1}'",
                  LinkMap.SkeletonUnits.size(), Options.InputFileName),
          Options.Verbose);
  verbose(formatv("Detected {0} package compile units in '{1}'",
                  LinkMap.PackageUnits.size(), Options.DWPFileName),
          Options.Verbose);
  verbose(formatv("Prepared rewrite plan: units={0}, contributions={1}",
                  LinkMap.RewritePlan.Units.size(),
                  LinkMap.RewritePlan.Contributions.size()),
          Options.Verbose);
  verbose(formatv("Prepared output manifest: sections={0}, total-size={1:x}",
                  LinkMap.RewritePlan.OutputManifest.Sections.size(),
                  LinkMap.RewritePlan.OutputManifest.TotalSize),
          Options.Verbose);
  verbose(formatv("Prepared output image: size={0:x}",
                  LinkMap.RewritePlan.OutputManifest.PayloadImage.size()),
          Options.Verbose);
  for (const RetainedDWPOutputLayoutEntry &Layout :
       LinkMap.RewritePlan.OutputManifest.Layout)
    verbose(formatv("  output-layout '{0}': offset={1:x}, size={2:x}",
                    Layout.Name, Layout.Offset, Layout.Size),
            Options.Verbose);
  for (const RetainedDWPOutputRecord &Record :
       LinkMap.RewritePlan.OutputManifest.Records)
    verbose(formatv("  output-record '{0}': payload-offset={1:x}, payload-size={2:x}",
                    Record.Name, Record.PayloadOffset, Record.PayloadSize),
            Options.Verbose);
  for (const RetainedDWPOutputSection &Section :
       LinkMap.RewritePlan.OutputManifest.Sections)
    if (std::optional<StringRef> Slice =
            LinkMap.RewritePlan.OutputManifest.getPayloadSlice(Section.Kind))
      verbose(formatv("  output-slice '{0}': size={1:x}", Section.Name,
                      Slice->size()),
              Options.Verbose);
  for (DWARFSectionKind Kind : {DW_SECT_INFO, DW_SECT_ABBREV,
                                DW_SECT_STR_OFFSETS, DW_SECT_RNGLISTS,
                                DW_SECT_EXT_LOC}) {
    const RetainedDWPSectionPlan *SectionPlan =
        LinkMap.RewritePlan.getSectionPlan(Kind);
    if (!SectionPlan)
      continue;
    verbose(formatv("  rewrite-section {0}: contributions={1}, total-length={2:x}",
                    toString(Kind), SectionPlan->Contributions.size(),
                    SectionPlan->OutputSize),
            Options.Verbose);
    verbose(formatv("    rewrite-input-section '{0}': size={1:x}, materialized={2:x}",
                    SectionPlan->SectionName, SectionPlan->InputSize,
                    SectionPlan->Contents.size()),
            Options.Verbose);
    if (const RetainedDWPOutputSection *OutputSection =
            LinkMap.RewritePlan.getOutputSection(Kind))
      verbose(formatv("    rewrite-output-section '{0}': size={1:x}",
                      OutputSection->Name, OutputSection->Contents.size()),
              Options.Verbose);
    for (const auto &Contribution : SectionPlan->Contributions)
      verbose(formatv("    rewrite-contribution {0}",
                      formatRetainedContribution(Contribution)),
              Options.Verbose);
  }

  for (const LinkedSplitUnit &Linked : LinkMap.LinkedUnits) {
    verbose(formatv("Matched DWO_id {0:x16}: main-unit='{1}' package='{2}'",
                    Linked.Skeleton.DWOId, Linked.Skeleton.DWOName,
                    Linked.Package.DWOName),
            Options.Verbose);

    if (!Linked.Skeleton.SplitUnitName.empty())
      verbose(formatv("  split-unit='{0}'", Linked.Skeleton.SplitUnitName),
              Options.Verbose);

    if (Linked.Skeleton.BaseAddress)
      verbose(formatv("  base-address={0:x}", *Linked.Skeleton.BaseAddress),
              Options.Verbose);

    for (const auto &Contribution : Linked.Package.Contributions)
      verbose(formatv("  {0}", formatContribution(Contribution)),
              Options.Verbose);

    for (const auto &Range : Linked.Skeleton.ResolvedRanges)
      verbose(formatv("  range {0}", formatAddressRange(Range)),
              Options.Verbose);

    for (const auto &Range : Linked.Skeleton.LiveRanges)
      verbose(formatv("  live-range {0}", formatAddressRange(Range)),
              Options.Verbose);

    verbose(formatv("  addressable-subprograms={0}",
                    Linked.Skeleton.Subprograms.size()),
            Options.Verbose);
    verbose(formatv("  live-roots={0}", Linked.Skeleton.LiveRootOffsets.size()),
            Options.Verbose);
    verbose(formatv("  retained-dies={0}",
                    Linked.Skeleton.RetainedDIEOffsets.size()),
            Options.Verbose);
    verbose(formatv("  live-subprograms={0}",
                    Linked.Skeleton.getLiveSubprogramCount()),
            Options.Verbose);

    if (const auto *SampleSubprogram = findLiveSubprogram(Linked.Skeleton)) {
      if (!SampleSubprogram->Name.empty())
        verbose(formatv("  sample-subprogram='{0}'", SampleSubprogram->Name),
                Options.Verbose);
      for (const auto &Range : SampleSubprogram->LiveRanges)
        verbose(formatv("  sample-range {0}", formatAddressRange(Range)),
                Options.Verbose);
    }

    if (const auto *RetainedPackage =
            LinkMap.findRetainedPackageUnit(Linked.Skeleton.DWOId))
      verbose(formatv("  retained-package-dies={0}",
                      RetainedPackage->Unit.RetainedDIEs.size()),
              Options.Verbose);
  }
}

} // namespace dwarfutil
} // namespace llvm
