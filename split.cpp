#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/SplitModule.h"

using namespace llvm;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
    cl::init("-"), cl::value_desc("filename"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Override output filename"),
               cl::value_desc("filename"));

static cl::opt<unsigned> NumOutputs("j", cl::Prefix, cl::init(2),
                                    cl::desc("Number of output files"));

static cl::opt<bool>
    PreserveLocals("preserve-locals", cl::Prefix, cl::init(false),
                   cl::desc("Split without externalizing locals"));

int main(int argc, char **argv) {
  LLVMContext Context;
  SMDiagnostic Err;
  cl::ParseCommandLineOptions(argc, argv, "LLVM module splitter\n");

  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);

  for (auto &F : *M)
    errs() << "Found " << F.getName() << '\n';

  return 0;
}
