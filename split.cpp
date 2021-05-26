#include "llvm/ADT/EquivalenceClasses.h"
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
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <algorithm>
#include <cassert>
#include <iterator>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "split"

namespace {

using ClusterMapType = EquivalenceClasses<const GlobalValue *>;
using ComdatMembersType = DenseMap<const Comdat *, const GlobalValue *>;
using ClusterIDMapType = DenseMap<const GlobalValue *, unsigned>;

} // end anonymous namespace

static void addNonConstUser(ClusterMapType &GVtoClusterMap,
                            const GlobalValue *GV, const User *U) {
  assert((!isa<Constant>(U) || isa<GlobalValue>(U)) && "Bad user");

  if (const Instruction *I = dyn_cast<Instruction>(U)) {
    const GlobalValue *F = I->getParent()->getParent();
    GVtoClusterMap.unionSets(GV, F);
  } else if (isa<GlobalIndirectSymbol>(U) || isa<Function>(U) ||
             isa<GlobalVariable>(U)) {
    GVtoClusterMap.unionSets(GV, cast<GlobalValue>(U));
  } else {
    llvm_unreachable("Underimplemented use case");
  }
}

// Adds all GlobalValue users of V to the same cluster as GV.
static void addAllGlobalValueUsers(ClusterMapType &GVtoClusterMap,
                                   const GlobalValue *GV, const Value *V) {
  for (auto *U : V->users()) {
    SmallVector<const User *, 4> Worklist;
    Worklist.push_back(U);
    while (!Worklist.empty()) {
      const User *UU = Worklist.pop_back_val();
      // For each constant that is not a GV (a pure const) recurse.
      if (isa<Constant>(UU) && !isa<GlobalValue>(UU)) {
        Worklist.append(UU->user_begin(), UU->user_end());
        continue;
      }
      addNonConstUser(GVtoClusterMap, GV, UU);
    }
  }
}

// Find partitions for module in the way that no locals need to be
// globalized.
// Try to balance pack those partitions into N files since this roughly equals
// thread balancing for the backend codegen step.
static void findPartitions(Module *M, ClusterIDMapType &ClusterIDMap,
                           unsigned N) {
  // At this point module should have the proper mix of globals and locals.
  // As we attempt to partition this module, we must not change any
  // locals to globals.
  LLVM_DEBUG(dbgs() << "Partition module with (" << M->size()
                    << ")functions\n");
  ClusterMapType GVtoClusterMap;
  ComdatMembersType ComdatMembers;

  auto recordGVSet = [&GVtoClusterMap, &ComdatMembers](GlobalValue &GV) {
    if (GV.isDeclaration())
      return;

    if (!GV.hasName())
      GV.setName("__llvmsplit_unnamed");

    // Comdat groups must not be partitioned. For comdat groups that contain
    // locals, record all their members here so we can keep them together.
    // Comdat groups that only contain external globals are already handled by
    // the MD5-based partitioning.
    if (const Comdat *C = GV.getComdat()) {
      auto &Member = ComdatMembers[C];
      if (Member)
        GVtoClusterMap.unionSets(Member, &GV);
      else
        Member = &GV;
    }

    // For aliases we should not separate them from their aliasees regardless
    // of linkage.
    if (auto *GIS = dyn_cast<GlobalIndirectSymbol>(&GV)) {
      if (const GlobalObject *Base = GIS->getBaseObject())
        GVtoClusterMap.unionSets(&GV, Base);
    }

    if (const Function *F = dyn_cast<Function>(&GV)) {
      for (const BasicBlock &BB : *F) {
        BlockAddress *BA = BlockAddress::lookup(&BB);
        if (!BA || !BA->isConstantUsed())
          continue;
        addAllGlobalValueUsers(GVtoClusterMap, F, BA);
      }
    }

    if (GV.hasLocalLinkage())
      addAllGlobalValueUsers(GVtoClusterMap, &GV, &GV);
  };

  llvm::for_each(M->functions(), recordGVSet);
  llvm::for_each(M->globals(), recordGVSet);
  llvm::for_each(M->aliases(), recordGVSet);

  // Assigned all GVs to merged clusters while balancing number of objects in
  // each.
  auto CompareClusters = [](const std::pair<unsigned, unsigned> &a,
                            const std::pair<unsigned, unsigned> &b) {
    if (a.second || b.second)
      return a.second > b.second;
    else
      return a.first > b.first;
  };

  std::priority_queue<std::pair<unsigned, unsigned>,
                      std::vector<std::pair<unsigned, unsigned>>,
                      decltype(CompareClusters)>
      BalancinQueue(CompareClusters);
  // Pre-populate priority queue with N slot blanks.
  for (unsigned i = 0; i < N; ++i)
    BalancinQueue.push(std::make_pair(i, 0));

  using SortType = std::pair<unsigned, ClusterMapType::iterator>;

  SmallVector<SortType, 64> Sets;
  SmallPtrSet<const GlobalValue *, 32> Visited;

  // To guarantee determinism, we have to sort SCC according to size.
  // When size is the same, use leader's name.
  for (ClusterMapType::iterator I = GVtoClusterMap.begin(),
                                E = GVtoClusterMap.end();
       I != E; ++I)
    if (I->isLeader())
      Sets.push_back(
          std::make_pair(std::distance(GVtoClusterMap.member_begin(I),
                                       GVtoClusterMap.member_end()),
                         I));

  llvm::sort(Sets, [](const SortType &a, const SortType &b) {
    if (a.first == b.first)
      return a.second->getData()->getName() > b.second->getData()->getName();
    else
      return a.first > b.first;
  });

  for (auto &I : Sets) {
    unsigned CurrentClusterID = BalancinQueue.top().first;
    unsigned CurrentClusterSize = BalancinQueue.top().second;
    BalancinQueue.pop();

    LLVM_DEBUG(dbgs() << "Root[" << CurrentClusterID << "] cluster_size("
                      << I.first << ") ----> " << I.second->getData()->getName()
                      << "\n");

    for (ClusterMapType::member_iterator MI =
             GVtoClusterMap.findLeader(I.second);
         MI != GVtoClusterMap.member_end(); ++MI) {
      if (!Visited.insert(*MI).second)
        continue;
      LLVM_DEBUG(dbgs() << "----> " << (*MI)->getName()
                        << ((*MI)->hasLocalLinkage() ? " l " : " e ") << "\n");
      Visited.insert(*MI);
      ClusterIDMap[*MI] = CurrentClusterID;
      CurrentClusterSize++;
    }
    // Add this set size to the number of entries in this cluster.
    BalancinQueue.push(std::make_pair(CurrentClusterID, CurrentClusterSize));
  }
}

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

// Returns whether GV should be in partition (0-based) I of N.
static bool isInPartition(const GlobalValue *GV, unsigned I, unsigned N) {
  if (auto *GIS = dyn_cast<GlobalIndirectSymbol>(GV))
    if (const GlobalObject *Base = GIS->getBaseObject())
      GV = Base;

  StringRef Name;
  if (const Comdat *C = GV->getComdat())
    Name = C->getName();
  else
    Name = GV->getName();

  // Partition by MD5 hash. We only need a few bits for evenness as the number
  // of partitions will generally be in the 1-2 figure range; the low 16 bits
  // are enough.
  MD5 H;
  MD5::MD5Result R;
  H.update(Name);
  H.final(R);
  return (R[0] | (R[1] << 8)) % N == I;
}

static void
split(std::unique_ptr<Module> M, unsigned N,
      function_ref<void(std::unique_ptr<Module> MPart)> ModuleCallback,
      bool PreserveLocals) {
  if (!PreserveLocals) {
    for (Function &F : *M)
      externalize(&F);
    for (GlobalVariable &GV : M->globals())
      externalize(&GV);
    for (GlobalAlias &GA : M->aliases())
      externalize(&GA);
    for (GlobalIFunc &GIF : M->ifuncs())
      externalize(&GIF);
  }

  // This performs splitting without a need for externalization, which might not
  // always be possible.
  ClusterIDMapType ClusterIDMap;
  findPartitions(M.get(), ClusterIDMap, N);

  // FIXME: We should be able to reuse M as the last partition instead of
  // cloning it.
  for (unsigned I = 0; I < N; ++I) {
    ValueToValueMapTy VMap;
    std::unique_ptr<Module> MPart(
        CloneModule(*M, VMap, [&](const GlobalValue *GV) {
          if (GV->isDiscardableIfUnused())
            return true;
          if (ClusterIDMap.count(GV))
            return (ClusterIDMap[GV] == I);
          else
            return isInPartition(GV, I, N);
        }));
    if (I != 0)
      MPart->setModuleInlineAsm("");
    ModuleCallback(std::move(MPart));
  }
}

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Override output filename"),
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

  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  unsigned I = 0;
  split(
      std::move(M), NumOutputs,
      [&](std::unique_ptr<Module> MPart) {
        std::error_code EC;
        std::unique_ptr<ToolOutputFile> Out(new ToolOutputFile(
            OutputFilename + utostr(I++), EC, sys::fs::OF_None));
        if (EC) {
          errs() << EC.message() << '\n';
          exit(1);
        }

        verifyModule(*MPart);
        WriteBitcodeToFile(*MPart, Out->os());

        // Declare success.
        Out->keep();
      },
      PreserveLocals);

  return 0;
}
