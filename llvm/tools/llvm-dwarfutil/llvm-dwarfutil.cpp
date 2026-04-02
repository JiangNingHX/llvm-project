//=== llvm-dwarfutil.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DWPUtil.h"
#include "DebugInfoLinker.h"
#include "Error.h"
#include "Options.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFVerifier.h"
#include "llvm/DebugInfo/DWARF/DWARFUnitIndex.h"
#include "llvm/MC/MCTargetOptionsCommandFlags.h"
#include "llvm/ObjCopy/CommonConfig.h"
#include "llvm/ObjCopy/ConfigManager.h"
#include "llvm/ObjCopy/ObjCopy.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CRC.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/Endian.h"
#include <limits>

using namespace llvm;
using namespace object;

namespace {
enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

#define OPTTABLE_STR_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

using namespace llvm::opt;
static constexpr opt::OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

class DwarfutilOptTable : public opt::GenericOptTable {
public:
  DwarfutilOptTable()
      : opt::GenericOptTable(OptionStrTable, OptionPrefixesTable, InfoTable) {}
};
} // namespace

namespace llvm {
namespace dwarfutil {

std::string ToolName;

static mc::RegisterMCTargetOptionsFlags MOF;

static Error validateAndSetOptions(opt::InputArgList &Args, Options &Options) {
  auto UnknownArgs = Args.filtered(OPT_UNKNOWN);
  if (!UnknownArgs.empty())
    return createStringError(
        std::errc::invalid_argument,
        formatv("unknown option: {0}", (*UnknownArgs.begin())->getSpelling())
            .str()
            .c_str());

  std::vector<std::string> InputFiles = Args.getAllArgValues(OPT_INPUT);
  if (InputFiles.size() != 2)
    return createStringError(
        std::errc::invalid_argument,
        formatv("exactly two positional arguments expected, {0} provided",
                InputFiles.size())
            .str()
            .c_str());

  Options.InputFileName = InputFiles[0];
  Options.OutputFileName = InputFiles[1];

  Options.BuildSeparateDebugFile =
      Args.hasFlag(OPT_separate_debug_file, OPT_no_separate_debug_file, false);
  Options.DoODRDeduplication =
      Args.hasFlag(OPT_odr_deduplication, OPT_no_odr_deduplication, true);
  Options.DoGarbageCollection =
      Args.hasFlag(OPT_garbage_collection, OPT_no_garbage_collection, true);
  Options.Verbose = Args.hasArg(OPT_verbose);
  Options.Verify = Args.hasArg(OPT_verify);
  Options.ExperimentalDWPOutputImage =
      Args.hasArg(OPT_experimental_dwp_output_image);
  Options.ExperimentalDWPOutputBundle =
      Args.hasArg(OPT_experimental_dwp_output_bundle);

  if (opt::Arg *NumThreads = Args.getLastArg(OPT_threads))
    Options.NumThreads = atoi(NumThreads->getValue());
  else
    Options.NumThreads = 0; // Use all available hardware threads

  if (opt::Arg *Tombstone = Args.getLastArg(OPT_tombstone)) {
    StringRef S = Tombstone->getValue();
    if (S == "bfd")
      Options.Tombstone = TombstoneKind::BFD;
    else if (S == "maxpc")
      Options.Tombstone = TombstoneKind::MaxPC;
    else if (S == "universal")
      Options.Tombstone = TombstoneKind::Universal;
    else if (S == "exec")
      Options.Tombstone = TombstoneKind::Exec;
    else
      return createStringError(
          std::errc::invalid_argument,
          formatv("unknown tombstone value: '{0}'", S).str().c_str());
  }

  if (opt::Arg *LinkerKind = Args.getLastArg(OPT_linker)) {
    StringRef S = LinkerKind->getValue();
    if (S == "classic")
      Options.UseDWARFLinkerParallel = false;
    else if (S == "parallel")
      Options.UseDWARFLinkerParallel = true;
    else
      return createStringError(
          std::errc::invalid_argument,
          formatv("unknown linker kind value: '{0}'", S).str().c_str());
  }

  if (opt::Arg *BuildAccelerator = Args.getLastArg(OPT_build_accelerator)) {
    StringRef S = BuildAccelerator->getValue();

    if (S == "none")
      Options.AccelTableKind = DwarfUtilAccelKind::None;
    else if (S == "DWARF")
      Options.AccelTableKind = DwarfUtilAccelKind::DWARF;
    else
      return createStringError(
          std::errc::invalid_argument,
          formatv("unknown build-accelerator value: '{0}'", S).str().c_str());
  }

  if (opt::Arg *DWPFile = Args.getLastArg(OPT_dwp))
    Options.DWPFileName = DWPFile->getValue();

  if (Options.Verbose) {
    if (Options.NumThreads != 1 && Args.hasArg(OPT_threads))
      warning("--num-threads set to 1 because verbose mode is specified");

    Options.NumThreads = 1;
  }

  if (Options.DoODRDeduplication && Args.hasArg(OPT_odr_deduplication) &&
      !Options.DoGarbageCollection)
    return createStringError(
        std::errc::invalid_argument,
        "cannot use --odr-deduplication without --garbage-collection");

  if (Options.BuildSeparateDebugFile && Options.OutputFileName == "-")
    return createStringError(
        std::errc::invalid_argument,
        "unable to write to stdout when --separate-debug-file specified");

  if (Options.hasDWPInput() && Options.OutputFileName == "-")
    return createStringError(std::errc::invalid_argument,
                             "the current --dwp path cannot write to stdout");

  if (Options.hasDWPInput() && Options.InputFileName == "-")
    return createStringError(std::errc::invalid_argument,
                             "the current --dwp path cannot read the main "
                             "input from stdin");

  if (Options.DWPFileName == "-")
    return createStringError(std::errc::invalid_argument,
                             "the current --dwp path cannot read the DWP "
                             "input from stdin");

  if (Options.ExperimentalDWPOutputImage || Options.ExperimentalDWPOutputBundle) {
    unsigned ExperimentalOutputs =
        static_cast<unsigned>(Options.ExperimentalDWPOutputImage) +
        static_cast<unsigned>(Options.ExperimentalDWPOutputBundle);
    if (ExperimentalOutputs > 1)
      return createStringError(
          std::errc::invalid_argument,
          "cannot use multiple experimental DWP output modes together");
    if (!Options.hasDWPInput())
      return createStringError(
          std::errc::invalid_argument,
          "experimental DWP output requires --dwp");
    if (Options.DoGarbageCollection)
      return createStringError(
          std::errc::invalid_argument,
          "experimental DWP output requires --no-garbage-collection");
    if (Options.DoODRDeduplication)
      return createStringError(
          std::errc::invalid_argument,
          "experimental DWP output requires --no-odr-deduplication");
    if (Options.BuildSeparateDebugFile)
      return createStringError(
          std::errc::invalid_argument,
          "experimental DWP output cannot be used with --separate-debug-file");
    if (Options.Verify)
      return createStringError(
          std::errc::invalid_argument,
          "experimental DWP output cannot be used with --verify");
    if (Options.AccelTableKind != DwarfUtilAccelKind::None)
      return createStringError(
          std::errc::invalid_argument,
          "experimental DWP output requires --build-accelerator=none");
  }

  if (Options.hasDWPInput() && !Options.ExperimentalDWPOutputImage &&
      !Options.ExperimentalDWPOutputBundle) {
    if (!Options.DoGarbageCollection)
      return createStringError(
          std::errc::invalid_argument,
          "the current --dwp path requires --garbage-collection");
    if (Options.DoODRDeduplication && Args.hasArg(OPT_odr_deduplication))
      return createStringError(
          std::errc::invalid_argument,
          "the current --dwp path does not yet support "
          "--odr-deduplication");
    Options.DoODRDeduplication = false;
    if (Options.BuildSeparateDebugFile)
      return createStringError(
          std::errc::invalid_argument,
          "the current --dwp path cannot be used with "
          "--separate-debug-file");
    if (Options.Verify)
      return createStringError(
          std::errc::invalid_argument,
          "the current --dwp path cannot be used with --verify");
    if (Options.AccelTableKind != DwarfUtilAccelKind::None)
      return createStringError(
          std::errc::invalid_argument,
          "the current --dwp path requires --build-accelerator=none");
  }

  return Error::success();
}

static Error setConfigToAddNewDebugSections(objcopy::ConfigManager &Config,
                                            ObjectFile &ObjFile) {
  // Add new debug sections.
  for (SectionRef Sec : ObjFile.sections()) {
    Expected<StringRef> SecName = Sec.getName();
    if (!SecName)
      return SecName.takeError();

    if (isDebugSection(*SecName)) {
      Expected<StringRef> SecData = Sec.getContents();
      if (!SecData)
        return SecData.takeError();

      Config.Common.AddSection.emplace_back(objcopy::NewSectionInfo(
          *SecName, MemoryBuffer::getMemBuffer(*SecData, *SecName, false)));
    }
  }

  return Error::success();
}

static Error verifyOutput(const Options &Opts) {
  if (Opts.OutputFileName == "-") {
    warning("verification skipped because writing to stdout");
    return Error::success();
  }

  std::string FileName = Opts.BuildSeparateDebugFile
                             ? Opts.getSeparateDebugFileName()
                             : Opts.OutputFileName;
  Expected<OwningBinary<Binary>> BinOrErr = createBinary(FileName);
  if (!BinOrErr)
    return createFileError(FileName, BinOrErr.takeError());

  if (BinOrErr->getBinary()->isObject()) {
    if (ObjectFile *Obj = static_cast<ObjectFile *>(BinOrErr->getBinary())) {
      verbose("Verifying DWARF...", Opts.Verbose);
      std::unique_ptr<DWARFContext> DICtx = DWARFContext::create(*Obj);
      DIDumpOptions DumpOpts;
      if (!DICtx->verify(Opts.Verbose ? outs() : nulls(),
                         DumpOpts.noImplicitRecursion()))
        return createFileError(FileName,
                               createError("output verification failed"));

      return Error::success();
    }
  }

  // The file "FileName" was created by this utility in the previous steps
  // (i.e. it is already known that it should pass the isObject check).
  // If the createBinary() function does not return an error, the isObject
  // check should also be successful.
  llvm_unreachable(
      formatv("tool unexpectedly did not emit a supported object file: '{0}'",
              FileName)
          .str()
          .c_str());
}

class raw_crc_ostream : public raw_ostream {
public:
  explicit raw_crc_ostream(raw_ostream &O) : OS(O) { SetUnbuffered(); }

  void reserveExtraSpace(uint64_t ExtraSize) override {
    OS.reserveExtraSpace(ExtraSize);
  }

  uint32_t getCRC32() { return CRC32; }

protected:
  raw_ostream &OS;
  uint32_t CRC32 = 0;

  /// See raw_ostream::write_impl.
  void write_impl(const char *Ptr, size_t Size) override {
    CRC32 = crc32(
        CRC32, ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Ptr), Size));
    OS.write(Ptr, Size);
  }

  /// Return the current position within the stream, not counting the bytes
  /// currently in the buffer.
  uint64_t current_pos() const override { return OS.tell(); }
};

static Expected<uint32_t> saveSeparateDebugInfo(const Options &Opts,
                                                ObjectFile &InputFile) {
  objcopy::ConfigManager Config;
  std::string OutputFilename = Opts.getSeparateDebugFileName();
  Config.Common.InputFilename = Opts.InputFileName;
  Config.Common.OutputFilename = OutputFilename;
  Config.Common.OnlyKeepDebug = true;
  uint32_t WrittenFileCRC32 = 0;

  if (Error Err = writeToOutput(
          Config.Common.OutputFilename, [&](raw_ostream &OutFile) -> Error {
            raw_crc_ostream CRCBuffer(OutFile);
            if (Error Err = objcopy::executeObjcopyOnBinary(Config, InputFile,
                                                            CRCBuffer))
              return Err;

            WrittenFileCRC32 = CRCBuffer.getCRC32();
            return Error::success();
          }))
    return std::move(Err);

  return WrittenFileCRC32;
}

static Error saveNonDebugInfo(const Options &Opts, ObjectFile &InputFile,
                              uint32_t GnuDebugLinkCRC32) {
  objcopy::ConfigManager Config;
  Config.Common.InputFilename = Opts.InputFileName;
  Config.Common.OutputFilename = Opts.OutputFileName;
  Config.Common.StripDebug = true;
  std::string SeparateDebugFileName = Opts.getSeparateDebugFileName();
  Config.Common.AddGnuDebugLink = sys::path::filename(SeparateDebugFileName);
  Config.Common.GnuDebugLinkCRC32 = GnuDebugLinkCRC32;

  if (Error Err = writeToOutput(
          Config.Common.OutputFilename, [&](raw_ostream &OutFile) -> Error {
            if (Error Err =
                    objcopy::executeObjcopyOnBinary(Config, InputFile, OutFile))
              return Err;

            return Error::success();
          }))
    return Err;

  return Error::success();
}

static Error splitDebugIntoSeparateFile(const Options &Opts,
                                        ObjectFile &InputFile) {
  Expected<uint32_t> SeparateDebugFileCRC32OrErr =
      saveSeparateDebugInfo(Opts, InputFile);
  if (!SeparateDebugFileCRC32OrErr)
    return SeparateDebugFileCRC32OrErr.takeError();

  if (Error Err =
          saveNonDebugInfo(Opts, InputFile, *SeparateDebugFileCRC32OrErr))
    return Err;

  return Error::success();
}

using DebugInfoBits = SmallString<10000>;

static Error addSectionsFromLinkedData(objcopy::ConfigManager &Config,
                                       ObjectFile &InputFile,
                                       DebugInfoBits &LinkedDebugInfoBits) {
  if (isa<ELFObjectFile<ELF32LE>>(&InputFile)) {
    Expected<ELFObjectFile<ELF32LE>> MemFile = ELFObjectFile<ELF32LE>::create(
        MemoryBufferRef(LinkedDebugInfoBits, ""));
    if (!MemFile)
      return MemFile.takeError();

    if (Error Err = setConfigToAddNewDebugSections(Config, *MemFile))
      return Err;
  } else if (isa<ELFObjectFile<ELF64LE>>(&InputFile)) {
    Expected<ELFObjectFile<ELF64LE>> MemFile = ELFObjectFile<ELF64LE>::create(
        MemoryBufferRef(LinkedDebugInfoBits, ""));
    if (!MemFile)
      return MemFile.takeError();

    if (Error Err = setConfigToAddNewDebugSections(Config, *MemFile))
      return Err;
  } else if (isa<ELFObjectFile<ELF32BE>>(&InputFile)) {
    Expected<ELFObjectFile<ELF32BE>> MemFile = ELFObjectFile<ELF32BE>::create(
        MemoryBufferRef(LinkedDebugInfoBits, ""));
    if (!MemFile)
      return MemFile.takeError();

    if (Error Err = setConfigToAddNewDebugSections(Config, *MemFile))
      return Err;
  } else if (isa<ELFObjectFile<ELF64BE>>(&InputFile)) {
    Expected<ELFObjectFile<ELF64BE>> MemFile = ELFObjectFile<ELF64BE>::create(
        MemoryBufferRef(LinkedDebugInfoBits, ""));
    if (!MemFile)
      return MemFile.takeError();

    if (Error Err = setConfigToAddNewDebugSections(Config, *MemFile))
      return Err;
  } else
    return createStringError(std::errc::invalid_argument,
                             "unsupported file format");

  return Error::success();
}

static Expected<uint32_t>
saveSeparateLinkedDebugInfo(const Options &Opts, ObjectFile &InputFile,
                            DebugInfoBits LinkedDebugInfoBits) {
  objcopy::ConfigManager Config;
  std::string OutputFilename = Opts.getSeparateDebugFileName();
  Config.Common.InputFilename = Opts.InputFileName;
  Config.Common.OutputFilename = OutputFilename;
  Config.Common.StripDebug = true;
  Config.Common.OnlyKeepDebug = true;
  uint32_t WrittenFileCRC32 = 0;

  if (Error Err =
          addSectionsFromLinkedData(Config, InputFile, LinkedDebugInfoBits))
    return std::move(Err);

  if (Error Err = writeToOutput(
          Config.Common.OutputFilename, [&](raw_ostream &OutFile) -> Error {
            raw_crc_ostream CRCBuffer(OutFile);

            if (Error Err = objcopy::executeObjcopyOnBinary(Config, InputFile,
                                                            CRCBuffer))
              return Err;

            WrittenFileCRC32 = CRCBuffer.getCRC32();
            return Error::success();
          }))
    return std::move(Err);

  return WrittenFileCRC32;
}

static Error saveSingleLinkedDebugInfo(const Options &Opts,
                                       ObjectFile &InputFile,
                                       DebugInfoBits LinkedDebugInfoBits) {
  objcopy::ConfigManager Config;

  Config.Common.InputFilename = Opts.InputFileName;
  Config.Common.OutputFilename = Opts.OutputFileName;
  Config.Common.StripDebug = true;
  if (Error Err =
          addSectionsFromLinkedData(Config, InputFile, LinkedDebugInfoBits))
    return Err;

  if (Error Err = writeToOutput(
          Config.Common.OutputFilename, [&](raw_ostream &OutFile) -> Error {
            return objcopy::executeObjcopyOnBinary(Config, InputFile, OutFile);
          }))
    return Err;

  return Error::success();
}

static Error saveLinkedDebugInfo(const Options &Opts, ObjectFile &InputFile,
                                 DebugInfoBits LinkedDebugInfoBits) {
  if (Opts.BuildSeparateDebugFile) {
    Expected<uint32_t> SeparateDebugFileCRC32OrErr =
        saveSeparateLinkedDebugInfo(Opts, InputFile,
                                    std::move(LinkedDebugInfoBits));
    if (!SeparateDebugFileCRC32OrErr)
      return SeparateDebugFileCRC32OrErr.takeError();

    if (Error Err =
            saveNonDebugInfo(Opts, InputFile, *SeparateDebugFileCRC32OrErr))
      return Err;
  } else {
    if (Error Err = saveSingleLinkedDebugInfo(Opts, InputFile,
                                              std::move(LinkedDebugInfoBits)))
      return Err;
  }

  return Error::success();
}

static Error saveCopyOfFile(const Options &Opts, ObjectFile &InputFile) {
  objcopy::ConfigManager Config;

  Config.Common.InputFilename = Opts.InputFileName;
  Config.Common.OutputFilename = Opts.OutputFileName;

  if (Error Err = writeToOutput(
          Config.Common.OutputFilename, [&](raw_ostream &OutFile) -> Error {
            return objcopy::executeObjcopyOnBinary(Config, InputFile, OutFile);
          }))
    return Err;

  return Error::success();
}

static Error saveDWPOutputImage(const Options &Opts, const DWPLinkMap &LinkMap) {
  const RetainedDWPOutputManifest &Manifest = LinkMap.RewritePlan.OutputManifest;

  std::error_code EC;
  ToolOutputFile Output(Opts.OutputFileName, EC, sys::fs::OF_None);
  if (EC)
    return createFileError(Opts.OutputFileName, EC);

  StringRef Payload = Manifest.PayloadImage;
  Output.os().write(Payload.data(), Payload.size());
  Output.keep();
  verbose(formatv("Wrote retained DWP output image '{0}': size={1:x}",
                  Opts.OutputFileName, Payload.size()),
          Opts.Verbose);
  return Error::success();
}

struct DWPOutputBundleRecord {
  DWARFSectionKind Kind = DW_SECT_EXT_unknown;
  std::string Name;
  uint64_t PayloadOffset = 0;
  uint64_t PayloadSize = 0;
};

static void appendU32(std::string &Buffer, uint32_t Value, bool IsLittleEndian) {
  char Bytes[sizeof(uint32_t)];
  if (IsLittleEndian)
    support::endian::write32le(Bytes, Value);
  else
    support::endian::write32be(Bytes, Value);
  Buffer.append(Bytes, sizeof(Bytes));
}

static void appendU64(std::string &Buffer, uint64_t Value, bool IsLittleEndian) {
  char Bytes[sizeof(uint64_t)];
  if (IsLittleEndian)
    support::endian::write64le(Bytes, Value);
  else
    support::endian::write64be(Bytes, Value);
  Buffer.append(Bytes, sizeof(Bytes));
}

static Expected<uint32_t> readU32LE(StringRef Buffer, uint64_t &Offset) {
  if (Offset + sizeof(uint32_t) > Buffer.size())
    return createStringError(std::errc::invalid_argument,
                             "unexpected end of bundle while reading u32");
  uint32_t Value = support::endian::read32le(Buffer.data() + Offset);
  Offset += sizeof(uint32_t);
  return Value;
}

static Expected<uint64_t> readU64LE(StringRef Buffer, uint64_t &Offset) {
  if (Offset + sizeof(uint64_t) > Buffer.size())
    return createStringError(std::errc::invalid_argument,
                             "unexpected end of bundle while reading u64");
  uint64_t Value = support::endian::read64le(Buffer.data() + Offset);
  Offset += sizeof(uint64_t);
  return Value;
}

static Expected<std::string>
buildDWPOutputBundleImage(const RetainedDWPOutputManifest &Manifest) {
  constexpr StringLiteral Magic = "DWPBNDL1";
  const uint32_t Version = 1;
  const uint32_t RecordCount = Manifest.Records.size();
  const uint64_t HeaderSize = Magic.size() + sizeof(uint32_t) +
                              sizeof(uint32_t) + sizeof(uint64_t) +
                              sizeof(uint64_t);
  uint64_t RecordsSize = 0;
  for (const RetainedDWPOutputRecord &Record : Manifest.Records) {
    if (Record.Name.size() > std::numeric_limits<uint32_t>::max())
      return createStringError(
          std::errc::invalid_argument,
          formatv("bundle record name too large for section '{0}'", Record.Name)
              .str()
              .c_str());
    RecordsSize += sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) +
                   sizeof(uint64_t) + Record.Name.size();
  }
  const uint64_t PayloadOffset = HeaderSize + RecordsSize;
  const uint64_t PayloadSize = Manifest.PayloadImage.size();

  std::string BundleImage;
  BundleImage.reserve(PayloadOffset + PayloadSize);
  BundleImage.append(Magic.data(), Magic.size());
  appendU32(BundleImage, Version, /*IsLittleEndian=*/true);
  appendU32(BundleImage, RecordCount, /*IsLittleEndian=*/true);
  appendU64(BundleImage, PayloadOffset, /*IsLittleEndian=*/true);
  appendU64(BundleImage, PayloadSize, /*IsLittleEndian=*/true);

  for (const RetainedDWPOutputRecord &Record : Manifest.Records) {
    appendU32(BundleImage, static_cast<uint32_t>(Record.Kind),
              /*IsLittleEndian=*/true);
    appendU32(BundleImage, static_cast<uint32_t>(Record.Name.size()),
              /*IsLittleEndian=*/true);
    appendU64(BundleImage, Record.PayloadOffset, /*IsLittleEndian=*/true);
    appendU64(BundleImage, Record.PayloadSize, /*IsLittleEndian=*/true);
    BundleImage.append(Record.Name);
  }

  BundleImage.append(Manifest.PayloadImage);
  return BundleImage;
}

static Expected<SmallVector<DWPOutputBundleRecord, 8>>
parseDWPOutputBundleRecords(StringRef BundleImage, uint64_t &PayloadOffset,
                            uint64_t &PayloadSize) {
  constexpr StringLiteral Magic = "DWPBNDL1";
  if (!BundleImage.starts_with(Magic))
    return createStringError(std::errc::invalid_argument,
                             "invalid DWP bundle magic");

  uint64_t Offset = Magic.size();
  Expected<uint32_t> VersionOrErr = readU32LE(BundleImage, Offset);
  if (!VersionOrErr)
    return VersionOrErr.takeError();
  if (*VersionOrErr != 1)
    return createStringError(
        std::errc::invalid_argument,
        formatv("unsupported DWP bundle version: {0}", *VersionOrErr)
            .str()
            .c_str());

  Expected<uint32_t> RecordCountOrErr = readU32LE(BundleImage, Offset);
  if (!RecordCountOrErr)
    return RecordCountOrErr.takeError();
  Expected<uint64_t> PayloadOffsetOrErr = readU64LE(BundleImage, Offset);
  if (!PayloadOffsetOrErr)
    return PayloadOffsetOrErr.takeError();
  Expected<uint64_t> PayloadSizeOrErr = readU64LE(BundleImage, Offset);
  if (!PayloadSizeOrErr)
    return PayloadSizeOrErr.takeError();

  PayloadOffset = *PayloadOffsetOrErr;
  PayloadSize = *PayloadSizeOrErr;

  SmallVector<DWPOutputBundleRecord, 8> Records;
  Records.reserve(*RecordCountOrErr);
  for (uint32_t I = 0; I != *RecordCountOrErr; ++I) {
    Expected<uint32_t> KindOrErr = readU32LE(BundleImage, Offset);
    if (!KindOrErr)
      return KindOrErr.takeError();
    Expected<uint32_t> NameSizeOrErr = readU32LE(BundleImage, Offset);
    if (!NameSizeOrErr)
      return NameSizeOrErr.takeError();
    Expected<uint64_t> RecordPayloadOffsetOrErr = readU64LE(BundleImage, Offset);
    if (!RecordPayloadOffsetOrErr)
      return RecordPayloadOffsetOrErr.takeError();
    Expected<uint64_t> RecordPayloadSizeOrErr = readU64LE(BundleImage, Offset);
    if (!RecordPayloadSizeOrErr)
      return RecordPayloadSizeOrErr.takeError();
    if (Offset + *NameSizeOrErr > BundleImage.size())
      return createStringError(std::errc::invalid_argument,
                               "bundle record name exceeds image bounds");

    DWPOutputBundleRecord &Record = Records.emplace_back();
    Record.Kind = static_cast<DWARFSectionKind>(*KindOrErr);
    Record.Name = std::string(
        BundleImage.slice(Offset, Offset + *NameSizeOrErr));
    Record.PayloadOffset = *RecordPayloadOffsetOrErr;
    Record.PayloadSize = *RecordPayloadSizeOrErr;
    Offset += *NameSizeOrErr;
  }

  return Records;
}

static Error validateDWPOutputBundleImage(
    StringRef BundleImage, const RetainedDWPOutputManifest &Manifest) {
  uint64_t PayloadOffset = 0;
  uint64_t PayloadSize = 0;
  Expected<SmallVector<DWPOutputBundleRecord, 8>> RecordsOrErr =
      parseDWPOutputBundleRecords(BundleImage, PayloadOffset, PayloadSize);
  if (!RecordsOrErr)
    return RecordsOrErr.takeError();

  if (RecordsOrErr->size() != Manifest.Records.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("bundle record count mismatch: expected {0}, got {1}",
                Manifest.Records.size(), RecordsOrErr->size())
            .str()
            .c_str());
  if (PayloadOffset > BundleImage.size() ||
      PayloadOffset + PayloadSize > BundleImage.size())
    return createStringError(std::errc::invalid_argument,
                             "bundle payload header exceeds image bounds");

  for (size_t I = 0; I != Manifest.Records.size(); ++I) {
    const RetainedDWPOutputRecord &Expected = Manifest.Records[I];
    const DWPOutputBundleRecord &Parsed = (*RecordsOrErr)[I];
    if (Parsed.Kind != Expected.Kind || Parsed.Name != Expected.Name ||
        Parsed.PayloadOffset != Expected.PayloadOffset ||
        Parsed.PayloadSize != Expected.PayloadSize)
      return createStringError(
          std::errc::invalid_argument,
          formatv("bundle record mismatch for section '{0}'", Expected.Name)
              .str()
              .c_str());
  }

  StringRef ParsedPayload =
      BundleImage.slice(PayloadOffset, PayloadOffset + PayloadSize);
  if (ParsedPayload != Manifest.PayloadImage)
    return createStringError(std::errc::invalid_argument,
                             "bundle payload image mismatch");

  return Error::success();
}

static Expected<std::string>
buildRetainedDWPIndexSection(const DWPLinkMap &LinkMap, bool IsLittleEndian) {
  if (LinkMap.CUIndexVersion == 0)
    return createStringError(std::errc::invalid_argument,
                             "missing CU index version for retained DWP index");
  if (LinkMap.CUIndexColumnKinds.empty())
    return createStringError(std::errc::invalid_argument,
                             "missing CU index columns for retained DWP index");

  DenseMap<uint64_t, DenseMap<DWARFSectionKind,
                              DWARFUnitIndex::Entry::SectionContribution>>
      NewContributionsByDWOId;

  for (const RetainedDWPPackageUnitInfo &Unit : LinkMap.RetainedPackageUnits)
    for (const PackageUnitInfo::SectionContributionInfo &Contribution :
         Unit.Contributions)
      NewContributionsByDWOId[Unit.Unit.DWOId][Contribution.Kind] =
          DWARFUnitIndex::Entry::SectionContribution(Contribution.Offset,
                                                     Contribution.Length);

  for (const RetainedDWPSectionContributionInfo &Contribution :
       LinkMap.RewritePlan.Contributions)
    NewContributionsByDWOId[Contribution.DWOId][Contribution.Kind] =
        DWARFUnitIndex::Entry::SectionContribution(Contribution.OutputOffset,
                                                   Contribution.Length);

  const size_t NumUnits = LinkMap.RetainedPackageUnits.size();
  const size_t NumBuckets = std::max<size_t>(1, NextPowerOf2(3 * NumUnits / 2));
  SmallVector<uint32_t, 8> Buckets(NumBuckets, 0);
  const uint64_t Mask = NumBuckets - 1;

  for (size_t I = 0; I != NumUnits; ++I) {
    uint64_t Signature = LinkMap.RetainedPackageUnits[I].Unit.DWOId;
    uint64_t H = Signature & Mask;
    uint64_t HP = ((Signature >> 32) & Mask) | 1;
    while (Buckets[H] != 0)
      H = (H + HP) & Mask;
    Buckets[H] = static_cast<uint32_t>(I + 1);
  }

  std::string Index;
  Index.reserve(16 + NumBuckets * 12 +
                LinkMap.CUIndexColumnKinds.size() * 4 * (1 + 2 * NumUnits));
  appendU32(Index, LinkMap.CUIndexVersion, IsLittleEndian);
  appendU32(Index, static_cast<uint32_t>(LinkMap.CUIndexColumnKinds.size()),
            IsLittleEndian);
  appendU32(Index, static_cast<uint32_t>(NumUnits), IsLittleEndian);
  appendU32(Index, static_cast<uint32_t>(NumBuckets), IsLittleEndian);

  for (uint32_t Bucket : Buckets) {
    uint64_t Signature =
        Bucket ? LinkMap.RetainedPackageUnits[Bucket - 1].Unit.DWOId : 0;
    appendU64(Index, Signature, IsLittleEndian);
  }
  for (uint32_t Bucket : Buckets)
    appendU32(Index, Bucket, IsLittleEndian);

  for (DWARFSectionKind Kind : LinkMap.CUIndexColumnKinds)
    appendU32(Index, serializeSectionKind(Kind, LinkMap.CUIndexVersion),
              IsLittleEndian);

  for (const RetainedDWPPackageUnitInfo &Unit : LinkMap.RetainedPackageUnits) {
    auto It = NewContributionsByDWOId.find(Unit.Unit.DWOId);
    for (DWARFSectionKind Kind : LinkMap.CUIndexColumnKinds) {
      uint64_t Offset = 0;
      if (It != NewContributionsByDWOId.end()) {
        auto Found = It->second.find(Kind);
        if (Found != It->second.end())
          Offset = Found->second.getOffset();
      }
      appendU32(Index, static_cast<uint32_t>(Offset), IsLittleEndian);
    }
  }

  for (const RetainedDWPPackageUnitInfo &Unit : LinkMap.RetainedPackageUnits) {
    auto It = NewContributionsByDWOId.find(Unit.Unit.DWOId);
    for (DWARFSectionKind Kind : LinkMap.CUIndexColumnKinds) {
      uint64_t Length = 0;
      if (It != NewContributionsByDWOId.end()) {
        auto Found = It->second.find(Kind);
        if (Found != It->second.end())
          Length = Found->second.getLength();
      }
      appendU32(Index, static_cast<uint32_t>(Length), IsLittleEndian);
    }
  }

  return Index;
}

static Error validateDWPOutputContainerPrototype(
    StringRef OutputFileName, const DWPLinkMap &LinkMap,
    StringRef ExpectedCUIndexContents) {
  Expected<OwningBinary<Binary>> BinOrErr = object::createBinary(OutputFileName);
  if (!BinOrErr)
    return createFileError(OutputFileName, BinOrErr.takeError());
  if (!BinOrErr->getBinary()->isObject())
    return createFileError(OutputFileName,
                           createError("unsupported output DWP file"));

  auto *OutputObject = cast<ObjectFile>(BinOrErr->getBinary());
  DenseMap<StringRef, StringRef> SectionContents;
  for (const SectionRef &Section : OutputObject->sections()) {
    Expected<StringRef> NameOrErr = Section.getName();
    if (!NameOrErr)
      return NameOrErr.takeError();
    Expected<StringRef> ContentsOrErr = Section.getContents();
    if (!ContentsOrErr)
      return ContentsOrErr.takeError();
    SectionContents[*NameOrErr] = *ContentsOrErr;
  }

  for (const RetainedDWPOutputSection &Section :
       LinkMap.RewritePlan.OutputManifest.Sections) {
    auto It = SectionContents.find(Section.Name);
    if (It == SectionContents.end())
      return createStringError(
          std::errc::invalid_argument,
          formatv("missing retained container section '{0}'", Section.Name)
              .str()
              .c_str());
    if (It->second != Section.Contents)
      return createStringError(
          std::errc::invalid_argument,
          formatv("retained container section payload mismatch for '{0}'",
                  Section.Name)
              .str()
              .c_str());
  }

  auto CUIndexIt = SectionContents.find(".debug_cu_index");
  if (CUIndexIt == SectionContents.end())
    return createStringError(std::errc::invalid_argument,
                             "missing retained container .debug_cu_index");
  if (CUIndexIt->second != ExpectedCUIndexContents)
    return createStringError(std::errc::invalid_argument,
                             "retained container .debug_cu_index mismatch");

  return Error::success();
}

static Error validateDWPOutputContainerLinkage(const Options &Opts,
                                               const DWPLinkMap &LinkMap) {
  Expected<OwningBinary<Binary>> MainBinOrErr = createBinary(Opts.InputFileName);
  if (!MainBinOrErr)
    return createFileError(Opts.InputFileName, MainBinOrErr.takeError());
  if (!MainBinOrErr->getBinary()->isObject())
    return createFileError(Opts.InputFileName,
                           createError("unsupported main input file"));

  Options ValidationOpts = Opts;
  ValidationOpts.DWPFileName = Opts.OutputFileName;
  ValidationOpts.ExperimentalDWPOutputImage = false;
  ValidationOpts.ExperimentalDWPOutputBundle = false;

  const auto *MainObject = cast<ObjectFile>(MainBinOrErr->getBinary());
  Expected<DWPLinkMap> RevalidatedLinkMapOrErr =
      loadDWPLinkMap(*MainObject, ValidationOpts);
  if (!RevalidatedLinkMapOrErr)
    return RevalidatedLinkMapOrErr.takeError();

  const DWPLinkMap &RevalidatedLinkMap = *RevalidatedLinkMapOrErr;
  if (RevalidatedLinkMap.LinkedUnits.size() != LinkMap.LinkedUnits.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("revalidated DWP linked unit count mismatch: expected {0}, "
                "got {1}",
                LinkMap.LinkedUnits.size(),
                RevalidatedLinkMap.LinkedUnits.size())
            .str()
            .c_str());
  if (RevalidatedLinkMap.RetainedPackageUnits.size() !=
      LinkMap.RetainedPackageUnits.size())
    return createStringError(
        std::errc::invalid_argument,
        formatv("revalidated retained DWP package unit count mismatch: "
                "expected {0}, got {1}",
                LinkMap.RetainedPackageUnits.size(),
                RevalidatedLinkMap.RetainedPackageUnits.size())
            .str()
            .c_str());

  for (const LinkedSplitUnit &ExpectedUnit : LinkMap.LinkedUnits) {
    const LinkedSplitUnit *ActualUnit =
        RevalidatedLinkMap.findLinkedUnit(ExpectedUnit.Skeleton.DWOId);
    if (!ActualUnit)
      return createStringError(
          std::errc::invalid_argument,
          formatv("revalidated DWP output is missing linked unit for DWO_id "
                  "{0:x16}",
                  ExpectedUnit.Skeleton.DWOId)
              .str()
              .c_str());
    if (ActualUnit->Skeleton.getLiveSubprogramCount() !=
        ExpectedUnit.Skeleton.getLiveSubprogramCount())
      return createStringError(
          std::errc::invalid_argument,
          formatv("revalidated live subprogram count mismatch for DWO_id "
                  "{0:x16}: expected {1}, got {2}",
                  ExpectedUnit.Skeleton.DWOId,
                  ExpectedUnit.Skeleton.getLiveSubprogramCount(),
                  ActualUnit->Skeleton.getLiveSubprogramCount())
              .str()
              .c_str());
    if (!ExpectedUnit.Skeleton.LiveRootOffsets.empty() &&
        ActualUnit->Skeleton.LiveRootOffsets.empty())
      return createStringError(
          std::errc::invalid_argument,
          formatv("revalidated DWP output has no live roots for DWO_id "
                  "{0:x16}",
                  ExpectedUnit.Skeleton.DWOId)
              .str()
              .c_str());
    if (ActualUnit->Skeleton.RetainedDIEOffsets.empty())
      return createStringError(
          std::errc::invalid_argument,
          formatv("revalidated DWP output has no retained DIEs for DWO_id "
                  "{0:x16}",
                  ExpectedUnit.Skeleton.DWOId)
              .str()
              .c_str());
  }

  return Error::success();
}

static Error saveDWPOutputContainerPrototype(const Options &Opts,
                                            const DWPLinkMap &LinkMap) {
  Expected<OwningBinary<Binary>> DWPBinOrErr = createBinary(Opts.DWPFileName);
  if (!DWPBinOrErr)
    return createFileError(Opts.DWPFileName, DWPBinOrErr.takeError());
  if (!DWPBinOrErr->getBinary()->isObject())
    return createFileError(Opts.DWPFileName,
                           createError("unsupported DWP input file"));

  auto *DWPObject = cast<ObjectFile>(DWPBinOrErr->getBinary());
  Expected<std::string> CUIndexContentsOrErr =
      buildRetainedDWPIndexSection(LinkMap, DWPObject->isLittleEndian());
  if (!CUIndexContentsOrErr)
    return CUIndexContentsOrErr.takeError();

  DenseSet<StringRef> ExistingSections;
  for (const SectionRef &Section : DWPObject->sections()) {
    Expected<StringRef> NameOrErr = Section.getName();
    if (!NameOrErr)
      return NameOrErr.takeError();
    ExistingSections.insert(*NameOrErr);
  }

  objcopy::ConfigManager Config;
  Config.Common.InputFilename = Opts.DWPFileName;
  Config.Common.OutputFilename = Opts.OutputFileName;

  auto AddOrUpdateSection = [&](StringRef Name, StringRef Contents) {
    auto Buffer = MemoryBuffer::getMemBufferCopy(Contents, Name);
    if (ExistingSections.contains(Name))
      Config.Common.UpdateSection.emplace_back(Name, std::move(Buffer));
    else
      Config.Common.AddSection.emplace_back(Name, std::move(Buffer));
  };

  for (const RetainedDWPOutputSection &Section :
       LinkMap.RewritePlan.OutputManifest.Sections)
    AddOrUpdateSection(Section.Name, Section.Contents);
  AddOrUpdateSection(".debug_cu_index", *CUIndexContentsOrErr);

  if (Error Err = writeToOutput(
          Config.Common.OutputFilename, [&](raw_ostream &OutFile) -> Error {
            return objcopy::executeObjcopyOnBinary(Config, *DWPObject, OutFile);
          }))
    return Err;

  if (Error Err = validateDWPOutputContainerPrototype(
          Opts.OutputFileName, LinkMap, *CUIndexContentsOrErr))
    return Err;
  if (Error Err = validateDWPOutputContainerLinkage(Opts, LinkMap))
    return Err;

  verbose(formatv("Wrote retained DWP container prototype '{0}': sections={1}, "
                  "cu-index-units={2}",
                  Opts.OutputFileName,
                  LinkMap.RewritePlan.OutputManifest.Sections.size() + 1,
                  LinkMap.RetainedPackageUnits.size()),
          Opts.Verbose);
  verbose("Validated retained DWP container contents", Opts.Verbose);
  verbose("Validated retained DWP container linkage with main input",
          Opts.Verbose);
  return Error::success();
}

static Error saveDWPOutputBundle(const Options &Opts, const DWPLinkMap &LinkMap) {
  const RetainedDWPOutputManifest &Manifest = LinkMap.RewritePlan.OutputManifest;

  std::error_code EC;
  ToolOutputFile Output(Opts.OutputFileName, EC, sys::fs::OF_None);
  if (EC)
    return createFileError(Opts.OutputFileName, EC);

  Expected<std::string> BundleImageOrErr = buildDWPOutputBundleImage(Manifest);
  if (!BundleImageOrErr)
    return BundleImageOrErr.takeError();
  if (Error Err = validateDWPOutputBundleImage(*BundleImageOrErr, Manifest))
    return Err;

  Output.os().write(BundleImageOrErr->data(), BundleImageOrErr->size());
  Output.keep();
  verbose(
      formatv("Wrote retained DWP output bundle '{0}': records={1}, names={2}, "
              "size={3:x}",
              Opts.OutputFileName, Manifest.Records.size(),
              BundleImageOrErr->size() - Manifest.PayloadImage.size(),
              BundleImageOrErr->size()),
      Opts.Verbose);
  verbose("Validated retained DWP output bundle contents", Opts.Verbose);
  return Error::success();
}

static Error applyCLOptions(const struct Options &Opts, ObjectFile &InputFile) {
  if (Opts.DoGarbageCollection ||
      Opts.AccelTableKind != DwarfUtilAccelKind::None) {
    verbose("Do debug info linking...", Opts.Verbose);

    DebugInfoBits LinkedDebugInfo;
    raw_svector_ostream OutStream(LinkedDebugInfo);

    if (Error Err = linkDebugInfo(InputFile, Opts, OutStream))
      return Err;

    if (Error Err =
            saveLinkedDebugInfo(Opts, InputFile, std::move(LinkedDebugInfo)))
      return Err;

    return Error::success();
  } else if (Opts.BuildSeparateDebugFile) {
    if (Error Err = splitDebugIntoSeparateFile(Opts, InputFile))
      return Err;
  } else {
    if (Error Err = saveCopyOfFile(Opts, InputFile))
      return Err;
  }

  return Error::success();
}

} // end of namespace dwarfutil
} // end of namespace llvm

int main(int Argc, char const *Argv[]) {
  using namespace dwarfutil;

  InitLLVM X(Argc, Argv);
  ToolName = Argv[0];

  // Parse arguments.
  DwarfutilOptTable T;
  unsigned MAI;
  unsigned MAC;
  ArrayRef<const char *> ArgsArr = ArrayRef(Argv + 1, Argc - 1);
  opt::InputArgList Args = T.ParseArgs(ArgsArr, MAI, MAC);

  if (Args.hasArg(OPT_help) || Args.size() == 0) {
    T.printHelp(
        outs(), (ToolName + " [options] <input file> <output file>").c_str(),
        "llvm-dwarfutil is a tool to copy and manipulate debug info", false);
    return EXIT_SUCCESS;
  }

  if (Args.hasArg(OPT_version)) {
    cl::PrintVersionMessage();
    return EXIT_SUCCESS;
  }

  Options Opts;
  if (Error Err = validateAndSetOptions(Args, Opts))
    error(std::move(Err), dwarfutil::ToolName);

  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllTargetInfos();
  InitializeAllAsmPrinters();

  ErrorOr<std::unique_ptr<MemoryBuffer>> BuffOrErr =
      MemoryBuffer::getFileOrSTDIN(Opts.InputFileName);
  if (BuffOrErr.getError())
    error(createFileError(Opts.InputFileName, BuffOrErr.getError()));

  Expected<std::unique_ptr<Binary>> BinOrErr =
      object::createBinary(**BuffOrErr);
  if (!BinOrErr)
    error(createFileError(Opts.InputFileName, BinOrErr.takeError()));

  Expected<FilePermissionsApplier> PermsApplierOrErr =
      FilePermissionsApplier::create(Opts.InputFileName);
  if (!PermsApplierOrErr)
    error(createFileError(Opts.InputFileName, PermsApplierOrErr.takeError()));

  if (!(*BinOrErr)->isObject())
    error(createFileError(Opts.InputFileName,
                          createError("unsupported input file")));

  if (Opts.hasDWPInput()) {
    Expected<DWPLinkMap> LinkMap =
        loadDWPLinkMap(*static_cast<ObjectFile *>((*BinOrErr).get()), Opts);
    if (!LinkMap)
      error(std::move(LinkMap.takeError()), dwarfutil::ToolName);
    logDWPLinkMap(*LinkMap, Opts);

    if (Opts.ExperimentalDWPOutputImage) {
      if (Error Err = saveDWPOutputImage(Opts, *LinkMap))
        error(std::move(Err));
      if (Error Err = PermsApplierOrErr->apply(Opts.OutputFileName))
        error(std::move(Err));
      return EXIT_SUCCESS;
    }
    if (Opts.ExperimentalDWPOutputBundle) {
      if (Error Err = saveDWPOutputBundle(Opts, *LinkMap))
        error(std::move(Err));
      if (Error Err = PermsApplierOrErr->apply(Opts.OutputFileName))
        error(std::move(Err));
      return EXIT_SUCCESS;
    }
    if (Error Err = saveDWPOutputContainerPrototype(Opts, *LinkMap))
      error(std::move(Err));
    if (Error Err = PermsApplierOrErr->apply(Opts.OutputFileName))
      error(std::move(Err));
    return EXIT_SUCCESS;
  }

  if (Error Err =
          applyCLOptions(Opts, *static_cast<ObjectFile *>((*BinOrErr).get())))
    error(createFileError(Opts.InputFileName, std::move(Err)));

  BinOrErr->reset();
  BuffOrErr->reset();

  if (Error Err = PermsApplierOrErr->apply(Opts.OutputFileName))
    error(std::move(Err));

  if (Opts.BuildSeparateDebugFile)
    if (Error Err = PermsApplierOrErr->apply(Opts.getSeparateDebugFileName()))
      error(std::move(Err));

  if (Opts.Verify) {
    if (Error Err = verifyOutput(Opts))
      error(std::move(Err));
  }

  return EXIT_SUCCESS;
}
