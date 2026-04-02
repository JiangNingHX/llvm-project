//===- DWPUtil.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_DWARFUTIL_DWPUTIL_H
#define LLVM_TOOLS_LLVM_DWARFUTIL_DWPUTIL_H

#include "Options.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFUnitIndex.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <optional>
#include <string>

namespace llvm {
class DWARFDie;

namespace object {
class ObjectFile;
}

namespace dwarfutil {

struct SkeletonUnitInfo {
  struct AddressRangeInfo {
    uint64_t LowPC = 0;
    uint64_t HighPC = 0;
    uint64_t SectionIndex = 0;
  };

  struct SubprogramInfo {
    uint64_t Offset = 0;
    std::string Name;
    SmallVector<AddressRangeInfo, 2> ResolvedRanges;
    SmallVector<AddressRangeInfo, 2> LiveRanges;

    bool isLive() const { return !LiveRanges.empty(); }
  };

  uint64_t DWOId = 0;
  std::string Name;
  std::string DWOName;
  std::string SplitUnitName;
  std::optional<uint64_t> BaseAddress;
  SmallVector<AddressRangeInfo, 4> ResolvedRanges;
  SmallVector<AddressRangeInfo, 4> LiveRanges;
  SmallVector<SubprogramInfo, 8> Subprograms;
  SmallVector<uint64_t, 8> LiveRootOffsets;
  SmallVector<uint64_t, 16> RetainedDIEOffsets;
  DenseSet<uint64_t> RetainedDIEOffsetSet;

  bool hasLiveCode() const { return !LiveRanges.empty(); }
  bool containsLiveAddress(uint64_t Address) const;
  bool containsLiveRange(uint64_t LowPC, uint64_t HighPC) const;
  bool containsRetainedDIEOffset(uint64_t Offset) const;
  size_t getLiveSubprogramCount() const;
};

struct PackageUnitInfo {
  struct SectionContributionInfo {
    DWARFSectionKind Kind = DW_SECT_EXT_unknown;
    uint64_t Offset = 0;
    uint64_t Length = 0;
  };

  uint64_t DWOId = 0;
  std::string Name;
  std::string DWOName;
  SmallVector<SectionContributionInfo, 4> Contributions;
};

struct LinkedSplitUnit {
  SkeletonUnitInfo Skeleton;
  PackageUnitInfo Package;
};

struct RetainedDWPDieInfo {
  uint64_t Offset = 0;
  std::optional<uint64_t> ParentOffset;
  dwarf::Tag Tag = dwarf::DW_TAG_null;
};

struct RetainedDWPUnitInfo {
  uint64_t DWOId = 0;
  uint64_t UnitOffset = 0;
  std::string UnitName;
  SmallVector<RetainedDWPDieInfo, 16> RetainedDIEs;
};

struct RetainedDWPPackageUnitInfo {
  RetainedDWPUnitInfo Unit;
  SmallVector<PackageUnitInfo::SectionContributionInfo, 4> Contributions;
};

struct RetainedDWPSectionContributionInfo {
  uint64_t DWOId = 0;
  DWARFSectionKind Kind = DW_SECT_EXT_unknown;
  std::string SectionName;
  uint64_t SectionSize = 0;
  uint64_t Offset = 0;
  uint64_t InputLength = 0;
  uint64_t Length = 0;
  uint64_t OutputOffset = 0;
};

struct RetainedDWPSectionPlan {
  DWARFSectionKind Kind = DW_SECT_EXT_unknown;
  std::string SectionName;
  uint64_t InputSize = 0;
  uint64_t OutputSize = 0;
  std::string Contents;
  SmallVector<RetainedDWPSectionContributionInfo, 8> Contributions;
};

struct RetainedDWPOutputSection {
  DWARFSectionKind Kind = DW_SECT_EXT_unknown;
  std::string Name;
  std::string Contents;
};

struct RetainedDWPOutputLayoutEntry {
  DWARFSectionKind Kind = DW_SECT_EXT_unknown;
  std::string Name;
  uint64_t Offset = 0;
  uint64_t Size = 0;
};

struct RetainedDWPOutputRecord {
  DWARFSectionKind Kind = DW_SECT_EXT_unknown;
  std::string Name;
  uint64_t PayloadOffset = 0;
  uint64_t PayloadSize = 0;
};

struct RetainedDWPOutputManifest {
  SmallVector<RetainedDWPOutputSection, 8> Sections;
  SmallVector<RetainedDWPOutputLayoutEntry, 8> Layout;
  SmallVector<RetainedDWPOutputRecord, 8> Records;
  uint64_t TotalSize = 0;
  std::string PayloadImage;

  std::optional<StringRef> getPayloadSlice(DWARFSectionKind Kind) const;
  const RetainedDWPOutputRecord *findRecord(DWARFSectionKind Kind) const;
};

struct RetainedDWPRewritePlan {
  SmallVector<RetainedDWPPackageUnitInfo, 4> Units;
  SmallVector<RetainedDWPSectionContributionInfo, 16> Contributions;
  SmallVector<RetainedDWPSectionPlan, 8> SectionPlans;
  SmallVector<RetainedDWPOutputSection, 8> OutputSections;
  RetainedDWPOutputManifest OutputManifest;

  SmallVector<RetainedDWPSectionContributionInfo, 8>
  getContributionsForKind(DWARFSectionKind Kind) const;
  uint64_t getTotalContributionLength(DWARFSectionKind Kind) const;
  const RetainedDWPSectionPlan *getSectionPlan(DWARFSectionKind Kind) const;
  const RetainedDWPOutputSection *getOutputSection(DWARFSectionKind Kind) const;
};

struct DWPLinkMap {
  uint32_t CUIndexVersion = 0;
  SmallVector<DWARFSectionKind, 8> CUIndexColumnKinds;
  SmallVector<SkeletonUnitInfo, 4> SkeletonUnits;
  SmallVector<PackageUnitInfo, 4> PackageUnits;
  SmallVector<LinkedSplitUnit, 4> LinkedUnits;
  SmallVector<RetainedDWPPackageUnitInfo, 4> RetainedPackageUnits;
  RetainedDWPRewritePlan RewritePlan;
  DenseMap<uint64_t, size_t> SkeletonUnitIndices;
  DenseMap<uint64_t, size_t> PackageUnitIndices;
  DenseMap<uint64_t, size_t> LinkedUnitIndices;
  DenseMap<uint64_t, size_t> RetainedPackageUnitIndices;

  const SkeletonUnitInfo *findSkeletonUnit(uint64_t DWOId) const;
  const PackageUnitInfo *findPackageUnit(uint64_t DWOId) const;
  const LinkedSplitUnit *findLinkedUnit(uint64_t DWOId) const;
  const RetainedDWPPackageUnitInfo *findRetainedPackageUnit(uint64_t DWOId) const;
  void rebuildLookupCaches();
};

Expected<DWPLinkMap> loadDWPLinkMap(const object::ObjectFile &InputFile,
                                    const Options &Options);

Expected<bool> isLiveSubprogramDIE(const DWPLinkMap &LinkMap,
                                   const DWARFDie &SubprogramDIE);
Expected<bool> shouldRetainDWPDIE(const DWPLinkMap &LinkMap,
                                  const DWARFDie &DIE);
Expected<SmallVector<DWARFDie, 16>>
collectRetainedDWPDIEs(const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE);
Expected<SmallVector<RetainedDWPDieInfo, 16>>
collectRetainedDWPDieInfos(const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE);
Expected<RetainedDWPUnitInfo>
collectRetainedDWPUnitInfo(const DWPLinkMap &LinkMap, const DWARFDie &UnitDIE);
Expected<SmallVector<RetainedDWPUnitInfo, 4>>
collectRetainedDWPUnits(const DWPLinkMap &LinkMap, StringRef DWPFileName);
Expected<SmallVector<RetainedDWPPackageUnitInfo, 4>>
collectRetainedDWPPackageUnits(const DWPLinkMap &LinkMap, StringRef DWPFileName);
Expected<RetainedDWPRewritePlan>
collectRetainedDWPRewritePlan(const DWPLinkMap &LinkMap, const Options &Options);
Error validateRetainedDWPOutputContainerLinkage(const Options &Options,
                                                const DWPLinkMap &LinkMap);

void logDWPLinkMap(const DWPLinkMap &LinkMap, const Options &Options);

} // namespace dwarfutil
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_DWARFUTIL_DWPUTIL_H
