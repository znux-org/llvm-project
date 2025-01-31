//===- dsymutil.cpp - Debug info dumping utility for llvm -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This program is a utility that aims to be a dropin replacement for Darwin's
// dsymutil.
//===----------------------------------------------------------------------===//

#include "dsymutil.h"
#include "BinaryHolder.h"
#include "CFBundle.h"
#include "DebugMap.h"
#include "LinkUtils.h"
#include "MachOUtils.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFVerifier.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/MachO.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/thread.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>

using namespace llvm;
using namespace llvm::dsymutil;
using namespace object;

namespace {
enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  OPT_##ID,
#include "Options.inc"
#undef OPTION
};

#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "Options.inc"
#undef PREFIX

const opt::OptTable::Info InfoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  {                                                                            \
      PREFIX,      NAME,      HELPTEXT,                                        \
      METAVAR,     OPT_##ID,  opt::Option::KIND##Class,                        \
      PARAM,       FLAGS,     OPT_##GROUP,                                     \
      OPT_##ALIAS, ALIASARGS, VALUES},
#include "Options.inc"
#undef OPTION
};

class DsymutilOptTable : public opt::OptTable {
public:
  DsymutilOptTable() : OptTable(InfoTable) {}
};
} // namespace

struct DsymutilOptions {
  bool DumpDebugMap = false;
  bool DumpStab = false;
  bool Flat = false;
  bool InputIsYAMLDebugMap = false;
  bool PaperTrailWarnings = false;
  bool Verify = false;
  std::string SymbolMap;
  std::string OutputFile;
  std::string Toolchain;
  std::vector<std::string> Archs;
  std::vector<std::string> InputFiles;
  unsigned NumThreads;
  dsymutil::LinkOptions LinkOpts;
};

/// Return a list of input files. This function has logic for dealing with the
/// special case where we might have dSYM bundles as input. The function
/// returns an error when the directory structure doesn't match that of a dSYM
/// bundle.
static Expected<std::vector<std::string>> getInputs(opt::InputArgList &Args,
                                                    bool DsymAsInput) {
  std::vector<std::string> InputFiles;
  for (auto *File : Args.filtered(OPT_INPUT))
    InputFiles.push_back(File->getValue());

  if (!DsymAsInput)
    return InputFiles;

  // If we are updating, we might get dSYM bundles as input.
  std::vector<std::string> Inputs;
  for (const auto &Input : InputFiles) {
    if (!sys::fs::is_directory(Input)) {
      Inputs.push_back(Input);
      continue;
    }

    // Make sure that we're dealing with a dSYM bundle.
    SmallString<256> BundlePath(Input);
    sys::path::append(BundlePath, "Contents", "Resources", "DWARF");
    if (!sys::fs::is_directory(BundlePath))
      return make_error<StringError>(
          Input + " is a directory, but doesn't look like a dSYM bundle.",
          inconvertibleErrorCode());

    // Create a directory iterator to iterate over all the entries in the
    // bundle.
    std::error_code EC;
    sys::fs::directory_iterator DirIt(BundlePath, EC);
    sys::fs::directory_iterator DirEnd;
    if (EC)
      return errorCodeToError(EC);

    // Add each entry to the list of inputs.
    while (DirIt != DirEnd) {
      Inputs.push_back(DirIt->path());
      DirIt.increment(EC);
      if (EC)
        return errorCodeToError(EC);
    }
  }
  return Inputs;
}

// Verify that the given combination of options makes sense.
static Error verifyOptions(const DsymutilOptions &Options) {
  if (Options.InputFiles.empty()) {
    return make_error<StringError>("no input files specified",
                                   errc::invalid_argument);
  }

  if (Options.LinkOpts.Update &&
      std::find(Options.InputFiles.begin(), Options.InputFiles.end(), "-") !=
          Options.InputFiles.end()) {
    // FIXME: We cannot use stdin for an update because stdin will be
    // consumed by the BinaryHolder during the debugmap parsing, and
    // then we will want to consume it again in DwarfLinker. If we
    // used a unique BinaryHolder object that could cache multiple
    // binaries this restriction would go away.
    return make_error<StringError>(
        "standard input cannot be used as input for a dSYM update.",
        errc::invalid_argument);
  }

  if (!Options.Flat && Options.OutputFile == "-")
    return make_error<StringError>(
        "cannot emit to standard output without --flat.",
        errc::invalid_argument);

  if (Options.InputFiles.size() > 1 && Options.Flat &&
      !Options.OutputFile.empty())
    return make_error<StringError>(
        "cannot use -o with multiple inputs in flat mode.",
        errc::invalid_argument);

  if (Options.PaperTrailWarnings && Options.InputIsYAMLDebugMap)
    return make_error<StringError>(
        "paper trail warnings are not supported for YAML input.",
        errc::invalid_argument);

  return Error::success();
}

static Expected<AccelTableKind> getAccelTableKind(opt::InputArgList &Args) {
  if (opt::Arg *Accelerator = Args.getLastArg(OPT_accelerator)) {
    StringRef S = Accelerator->getValue();
    if (S == "Apple")
      return AccelTableKind::Apple;
    if (S == "Dwarf")
      return AccelTableKind::Dwarf;
    if (S == "Default")
      return AccelTableKind::Default;
    return make_error<StringError>(
        "invalid accelerator type specified: '" + S +
            "'. Support values are 'Apple', 'Dwarf' and 'Default'.",
        inconvertibleErrorCode());
  }
  return AccelTableKind::Default;
}

/// Parses the command line options into the LinkOptions struct and performs
/// some sanity checking. Returns an error in case the latter fails.
static Expected<DsymutilOptions> getOptions(opt::InputArgList &Args) {
  DsymutilOptions Options;

  Options.DumpDebugMap = Args.hasArg(OPT_dump_debug_map);
  Options.DumpStab = Args.hasArg(OPT_symtab);
  Options.Flat = Args.hasArg(OPT_flat);
  Options.InputIsYAMLDebugMap = Args.hasArg(OPT_yaml_input);
  Options.PaperTrailWarnings = Args.hasArg(OPT_papertrail);
  Options.Verify = Args.hasArg(OPT_verify);

  Options.LinkOpts.Minimize = Args.hasArg(OPT_minimize);
  Options.LinkOpts.NoODR = Args.hasArg(OPT_no_odr);
  Options.LinkOpts.NoOutput = Args.hasArg(OPT_no_output);
  Options.LinkOpts.NoTimestamp = Args.hasArg(OPT_no_swiftmodule_timestamp);
  Options.LinkOpts.Update = Args.hasArg(OPT_update);
  Options.LinkOpts.Verbose = Args.hasArg(OPT_verbose);

  if (Expected<AccelTableKind> AccelKind = getAccelTableKind(Args)) {
    Options.LinkOpts.TheAccelTableKind = *AccelKind;
  } else {
    return AccelKind.takeError();
  }

  if (opt::Arg *SymbolMap = Args.getLastArg(OPT_symbolmap))
    Options.SymbolMap = SymbolMap->getValue();

  if (Args.hasArg(OPT_symbolmap))
    Options.LinkOpts.Update = true;

  if (Expected<std::vector<std::string>> InputFiles =
          getInputs(Args, Options.LinkOpts.Update)) {
    Options.InputFiles = std::move(*InputFiles);
  } else {
    return InputFiles.takeError();
  }

  for (auto *Arch : Args.filtered(OPT_arch))
    Options.Archs.push_back(Arch->getValue());

  if (opt::Arg *OsoPrependPath = Args.getLastArg(OPT_oso_prepend_path))
    Options.LinkOpts.PrependPath = OsoPrependPath->getValue();

  if (opt::Arg *OutputFile = Args.getLastArg(OPT_output))
    Options.OutputFile = OutputFile->getValue();

  if (opt::Arg *Toolchain = Args.getLastArg(OPT_toolchain))
    Options.Toolchain = Toolchain->getValue();

  if (Args.hasArg(OPT_assembly))
    Options.LinkOpts.FileType = OutputFileType::Assembly;

  if (opt::Arg *NumThreads = Args.getLastArg(OPT_threads))
    Options.LinkOpts.Threads = atoi(NumThreads->getValue());
  else
    Options.LinkOpts.Threads = thread::hardware_concurrency();

  if (Options.DumpDebugMap || Options.LinkOpts.Verbose)
    Options.LinkOpts.Threads = 1;

  if (getenv("RC_DEBUG_OPTIONS"))
    Options.PaperTrailWarnings = true;

  if (Error E = verifyOptions(Options))
    return std::move(E);
  return Options;
}

static Error createPlistFile(StringRef Bin, StringRef BundleRoot,
                             StringRef Toolchain) {
  // Create plist file to write to.
  SmallString<128> InfoPlist(BundleRoot);
  sys::path::append(InfoPlist, "Contents/Info.plist");
  std::error_code EC;
  raw_fd_ostream PL(InfoPlist, EC, sys::fs::F_Text);
  if (EC)
    return make_error<StringError>(
        "cannot create Plist: " + toString(errorCodeToError(EC)), EC);

  CFBundleInfo BI = getBundleInfo(Bin);

  if (BI.IDStr.empty()) {
    StringRef BundleID = *sys::path::rbegin(BundleRoot);
    if (sys::path::extension(BundleRoot) == ".dSYM")
      BI.IDStr = sys::path::stem(BundleID);
    else
      BI.IDStr = BundleID;
  }

  // Print out information to the plist file.
  PL << "<?xml version=\"1.0\" encoding=\"UTF-8\"\?>\n"
     << "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" "
     << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
     << "<plist version=\"1.0\">\n"
     << "\t<dict>\n"
     << "\t\t<key>CFBundleDevelopmentRegion</key>\n"
     << "\t\t<string>English</string>\n"
     << "\t\t<key>CFBundleIdentifier</key>\n"
     << "\t\t<string>com.apple.xcode.dsym." << BI.IDStr << "</string>\n"
     << "\t\t<key>CFBundleInfoDictionaryVersion</key>\n"
     << "\t\t<string>6.0</string>\n"
     << "\t\t<key>CFBundlePackageType</key>\n"
     << "\t\t<string>dSYM</string>\n"
     << "\t\t<key>CFBundleSignature</key>\n"
     << "\t\t<string>\?\?\?\?</string>\n";

  if (!BI.OmitShortVersion()) {
    PL << "\t\t<key>CFBundleShortVersionString</key>\n";
    PL << "\t\t<string>";
    printHTMLEscaped(BI.ShortVersionStr, PL);
    PL << "</string>\n";
  }

  PL << "\t\t<key>CFBundleVersion</key>\n";
  PL << "\t\t<string>";
  printHTMLEscaped(BI.VersionStr, PL);
  PL << "</string>\n";

  if (!Toolchain.empty()) {
    PL << "\t\t<key>Toolchain</key>\n";
    PL << "\t\t<string>";
    printHTMLEscaped(Toolchain, PL);
    PL << "</string>\n";
  }

  PL << "\t</dict>\n"
     << "</plist>\n";

  PL.close();
  return Error::success();
}

static Error createBundleDir(StringRef BundleBase) {
  SmallString<128> Bundle(BundleBase);
  sys::path::append(Bundle, "Contents", "Resources", "DWARF");
  if (std::error_code EC =
          create_directories(Bundle.str(), true, sys::fs::perms::all_all))
    return make_error<StringError>(
        "cannot create bundle: " + toString(errorCodeToError(EC)), EC);

  return Error::success();
}

static bool verify(StringRef OutputFile, StringRef Arch, bool Verbose) {
  if (OutputFile == "-") {
    WithColor::warning() << "verification skipped for " << Arch
                         << "because writing to stdout.\n";
    return true;
  }

  Expected<OwningBinary<Binary>> BinOrErr = createBinary(OutputFile);
  if (!BinOrErr) {
    WithColor::error() << OutputFile << ": " << toString(BinOrErr.takeError());
    return false;
  }

  Binary &Binary = *BinOrErr.get().getBinary();
  if (auto *Obj = dyn_cast<MachOObjectFile>(&Binary)) {
    raw_ostream &os = Verbose ? errs() : nulls();
    os << "Verifying DWARF for architecture: " << Arch << "\n";
    std::unique_ptr<DWARFContext> DICtx = DWARFContext::create(*Obj);
    DIDumpOptions DumpOpts;
    bool success = DICtx->verify(os, DumpOpts.noImplicitRecursion());
    if (!success)
      WithColor::error() << "verification failed for " << Arch << '\n';
    return success;
  }

  return false;
}

namespace {
struct OutputLocation {
  OutputLocation(std::string DWARFFile, Optional<std::string> ResourceDir = {})
      : DWARFFile(DWARFFile), ResourceDir(ResourceDir) {}
  /// This method is a workaround for older compilers.
  Optional<std::string> getResourceDir() const { return ResourceDir; }
  std::string DWARFFile;
  Optional<std::string> ResourceDir;
};
} // namespace

static Expected<OutputLocation>
getOutputFileName(StringRef InputFile, const DsymutilOptions &Options) {
  if (Options.OutputFile == "-")
    return OutputLocation(Options.OutputFile);

  // When updating, do in place replacement.
  if (Options.OutputFile.empty() &&
      (Options.LinkOpts.Update || !Options.SymbolMap.empty()))
    return OutputLocation(InputFile);

  // If a flat dSYM has been requested, things are pretty simple.
  if (Options.Flat) {
    if (Options.OutputFile.empty()) {
      if (InputFile == "-")
        return OutputLocation{"a.out.dwarf", {}};
      return OutputLocation((InputFile + ".dwarf").str());
    }

    return OutputLocation(Options.OutputFile);
  }

  // We need to create/update a dSYM bundle.
  // A bundle hierarchy looks like this:
  //   <bundle name>.dSYM/
  //       Contents/
  //          Info.plist
  //          Resources/
  //             DWARF/
  //                <DWARF file(s)>
  std::string DwarfFile = InputFile == "-" ? StringRef("a.out") : InputFile;
  SmallString<128> Path(Options.OutputFile);
  if (Path.empty())
    Path = DwarfFile + ".dSYM";
  if (!Options.LinkOpts.NoOutput) {
    if (auto E = createBundleDir(Path))
      return std::move(E);
    if (auto E = createPlistFile(DwarfFile, Path, Options.Toolchain))
      return std::move(E);
  }

  sys::path::append(Path, "Contents", "Resources");
  std::string ResourceDir = Path.str();
  sys::path::append(Path, "DWARF", sys::path::filename(DwarfFile));
  return OutputLocation(Path.str(), ResourceDir);
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  // Parse arguments.
  DsymutilOptTable T;
  unsigned MAI;
  unsigned MAC;
  ArrayRef<const char *> ArgsArr = makeArrayRef(argv + 1, argc - 1);
  opt::InputArgList Args = T.ParseArgs(ArgsArr, MAI, MAC);

  void *P = (void *)(intptr_t)getOutputFileName;
  std::string SDKPath = sys::fs::getMainExecutable(argv[0], P);
  SDKPath = sys::path::parent_path(SDKPath);

  for (auto *Arg : Args.filtered(OPT_UNKNOWN)) {
    WithColor::warning() << "ignoring unknown option: " << Arg->getSpelling()
                         << '\n';
  }

  if (Args.hasArg(OPT_help)) {
    T.PrintHelp(
        outs(), (std::string(argv[0]) + " [options] <input files>").c_str(),
        "manipulate archived DWARF debug symbol files.\n\n"
        "dsymutil links the DWARF debug information found in the object files\n"
        "for the executable <input file> by using debug symbols information\n"
        "contained in its symbol table.\n",
        false);
    return 0;
  }

  if (Args.hasArg(OPT_version)) {
    cl::PrintVersionMessage();
    return 0;
  }

  auto OptionsOrErr = getOptions(Args);
  if (!OptionsOrErr) {
    WithColor::error() << toString(OptionsOrErr.takeError());
    return 1;
  }

  auto &Options = *OptionsOrErr;

  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllTargets();
  InitializeAllAsmPrinters();

  for (const auto &Arch : Options.Archs)
    if (Arch != "*" && Arch != "all" &&
        !object::MachOObjectFile::isValidArch(Arch)) {
      WithColor::error() << "unsupported cpu architecture: '" << Arch << "'\n";
      return 1;
    }

  SymbolMapLoader SymMapLoader(Options.SymbolMap);

  for (auto &InputFile : Options.InputFiles) {
    // Dump the symbol table for each input file and requested arch
    if (Options.DumpStab) {
      if (!dumpStab(InputFile, Options.Archs, Options.LinkOpts.PrependPath))
        return 1;
      continue;
    }

    auto DebugMapPtrsOrErr =
        parseDebugMap(InputFile, Options.Archs, Options.LinkOpts.PrependPath,
                      Options.PaperTrailWarnings, Options.LinkOpts.Verbose,
                      Options.InputIsYAMLDebugMap);

    if (auto EC = DebugMapPtrsOrErr.getError()) {
      WithColor::error() << "cannot parse the debug map for '" << InputFile
                         << "': " << EC.message() << '\n';
      return 1;
    }

    if (Options.LinkOpts.Update) {
      // The debug map should be empty. Add one object file corresponding to
      // the input file.
      for (auto &Map : *DebugMapPtrsOrErr)
        Map->addDebugMapObject(InputFile,
                               sys::TimePoint<std::chrono::seconds>());
    }

    // Ensure that the debug map is not empty (anymore).
    if (DebugMapPtrsOrErr->empty()) {
      WithColor::error() << "no architecture to link\n";
      return 1;
    }

    // Shared a single binary holder for all the link steps.
    BinaryHolder BinHolder;

    unsigned ThreadCount =
        std::min<unsigned>(Options.LinkOpts.Threads, DebugMapPtrsOrErr->size());
    ThreadPool Threads(ThreadCount);

    // If there is more than one link to execute, we need to generate
    // temporary files.
    const bool NeedsTempFiles =
        !Options.DumpDebugMap && (Options.OutputFile != "-") &&
        (DebugMapPtrsOrErr->size() != 1 || Options.LinkOpts.Update);
    const bool Verify = Options.Verify && !Options.LinkOpts.NoOutput;

    SmallVector<MachOUtils::ArchAndFile, 4> TempFiles;
    std::atomic_char AllOK(1);
    for (auto &Map : *DebugMapPtrsOrErr) {
      if (Options.LinkOpts.Verbose || Options.DumpDebugMap)
        Map->print(outs());

      if (Options.DumpDebugMap)
        continue;

      if (!Options.SymbolMap.empty())
        Options.LinkOpts.Translator = SymMapLoader.Load(InputFile, *Map);

      if (Map->begin() == Map->end())
        WithColor::warning()
            << "no debug symbols in executable (-arch "
            << MachOUtils::getArchName(Map->getTriple().getArchName()) << ")\n";

      // Using a std::shared_ptr rather than std::unique_ptr because move-only
      // types don't work with std::bind in the ThreadPool implementation.
      std::shared_ptr<raw_fd_ostream> OS;

      Expected<OutputLocation> OutputLocationOrErr =
          getOutputFileName(InputFile, Options);
      if (!OutputLocationOrErr) {
        WithColor::error() << toString(OutputLocationOrErr.takeError());
        return 1;
      }
      Options.LinkOpts.ResourceDir = OutputLocationOrErr->getResourceDir();

      std::string OutputFile = OutputLocationOrErr->DWARFFile;
      if (NeedsTempFiles) {
        TempFiles.emplace_back(Map->getTriple().getArchName().str());

        auto E = TempFiles.back().createTempFile();
        if (E) {
          WithColor::error() << toString(std::move(E));
          return 1;
        }

        auto &TempFile = *(TempFiles.back().File);
        OS = std::make_shared<raw_fd_ostream>(TempFile.FD,
                                              /*shouldClose*/ false);
        OutputFile = TempFile.TmpName;
      } else {
        std::error_code EC;
        OS = std::make_shared<raw_fd_ostream>(
            Options.LinkOpts.NoOutput ? "-" : OutputFile, EC, sys::fs::F_None);
        if (EC) {
          WithColor::error() << OutputFile << ": " << EC.message();
          return 1;
        }
      }

      auto LinkLambda = [&, OutputFile](std::shared_ptr<raw_fd_ostream> Stream,
                                        LinkOptions Options) {
        AllOK.fetch_and(
            linkDwarf(*Stream, BinHolder, *Map, std::move(Options)));
        Stream->flush();
        if (Verify)
          AllOK.fetch_and(verify(OutputFile, Map->getTriple().getArchName(),
                                 Options.Verbose));
      };

      // FIXME: The DwarfLinker can have some very deep recursion that can max
      // out the (significantly smaller) stack when using threads. We don't
      // want this limitation when we only have a single thread.
      if (ThreadCount == 1)
        LinkLambda(OS, Options.LinkOpts);
      else
        Threads.async(LinkLambda, OS, Options.LinkOpts);
    }

    Threads.wait();

    if (!AllOK)
      return 1;

    if (NeedsTempFiles) {
      Expected<OutputLocation> OutputLocationOrErr =
          getOutputFileName(InputFile, Options);
      if (!OutputLocationOrErr) {
        WithColor::error() << toString(OutputLocationOrErr.takeError());
        return 1;
      }
      if (!MachOUtils::generateUniversalBinary(TempFiles,
                                               OutputLocationOrErr->DWARFFile,
                                               Options.LinkOpts, SDKPath))
        return 1;
    }
  }

  return 0;
}
