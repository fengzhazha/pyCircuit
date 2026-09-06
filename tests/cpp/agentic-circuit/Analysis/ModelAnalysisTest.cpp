#include "acir/Analysis/ModelAnalysis.h"
#include "Analysis/ModelAnalysisInternal.h"
#include "Analysis/ModelAnalysisTestHooks.h"
#include "acir/Analysis/VariableAnalysis.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "acir/Dialect/ACIR/GraphRegion.h"
#include "acir/InitAllDialects.h"
#include "acir/Transforms/Passes.h"

#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/StringSet.h"
#include "gtest/gtest.h"

#include <functional>
#include <string>

namespace acir {
namespace {

using namespace mlir;
using namespace acir::ac;

template <typename Analysis>
concept ExposesTopologyDigest =
    requires(Analysis &analysis) { analysis.computeTopologyDigest(); };

template <typename Analysis>
concept ExposesOwnerManifest =
    requires(Analysis &analysis) { analysis.buildFrozenOwnerManifest(); };

template <typename Analysis>
concept ExposesOwnerWork =
    requires(Analysis &analysis) { analysis.getLastOwnerManifestWork(); };

template <typename Process>
concept ExposesProcessSkeleton =
    requires(Process process) { buildFrozenProcessSkeleton(process); };

static_assert(!ExposesTopologyDigest<ModelAnalysis>);
static_assert(!ExposesOwnerManifest<ModelAnalysis>);
static_assert(!ExposesOwnerWork<ModelAnalysis>);
static_assert(!ExposesProcessSkeleton<ProcessOp>);

constexpr llvm::StringLiteral kProcessModel = R"mlir(
  builtin.module attributes {ac.contract_epoch = "0.5"} {
    ac.protocol @p32 {
      ac.role @sender dual @receiver cardinality "exclusive"
      ac.role @receiver dual @sender cardinality "exclusive"
      ac.state @idle initial true terminal false
      ac.event @push from @sender to @receiver payload i32 action "notify"
      ac.transition from @idle to @idle on @push transfer false retain false guard {}
    }
    ac.protocol @p64 {
      ac.role @sender dual @receiver cardinality "exclusive"
      ac.role @receiver dual @sender cardinality "exclusive"
      ac.state @idle initial true terminal false
      ac.event @push from @sender to @receiver payload i64 action "notify"
      ac.transition from @idle to @idle on @push transfer false retain false guard {}
    }
    ac.system @soc root @Top as "root" tick 0 "cycle"
        workload @Top::@workload seed {kind = "fixed", value = 0 : i64}
        instrumentation [] results {id = "default", format = "json"}
        selected true
    ac.module @Top() parameters {} graph {
      %graph_i32 = arith.constant 7 : i32
      %graph_i64 = arith.constant 9 : i64
      ac.time_domain @clock period 1 phase 0 scale 1
      ac.queue @q0 payload i32 entries 4 ordering "fifo" protocol @p32
          ownership "exclusive" id "q0" path "q0"
      ac.queue @q1 payload i32 entries 4 ordering "fifo" protocol @p32
          ownership "exclusive" id "q1" path "q1"
      ac.queue @q64 payload i64 entries 4 ordering "fifo" protocol @p64
          ownership "exclusive" id "q64" path "q64"
      ac.event_queue @e0 payload !ac.event<i32> capacity 4
          ordering "time_then_sequence" domain @clock id "e0" path "e0"
      ac.event_queue @e1 payload !ac.event<i32> capacity 4
          ordering "time_then_sequence" domain @clock id "e1" path "e1"
      ac.resource @r0 capacity 1 issue_width 1 ii 1
          latency {kind = "fixed", ticks = 1 : i64}
          lifecycle {reservation = "propose_commit", release = "balanced", cancellation = "explicit"}
          ownership "exclusive" classes [] id "r0" path "r0"
      ac.resource @r1 capacity 1 issue_width 1 ii 1
          latency {kind = "fixed", ticks = 1 : i64}
          lifecycle {reservation = "propose_commit", release = "balanced", cancellation = "explicit"}
          ownership "exclusive" classes [] id "r1" path "r1"
      ac.stat @s0 kind "counter"
      ac.stat @s1 kind "counter"
      ac.process @worker0 kind "control" captures(%graph_i32 : i32) {
      ^bb0(%captured : i32):
        ac.yield_sim
      }
      ac.process @worker1 kind "control" captures(%graph_i32 : i32) {
      ^bb0(%captured : i32):
        ac.yield_sim
      }
      ac.process @workload kind "workload" captures(%graph_i32 : i32) {
      ^bb0(%captured : i32):
        %one = arith.constant 1 : i64
        %other = arith.constant 11 : i64
        %true = arith.constant true
        %unused = arith.constant 101 : i64
        %accepted = ac.try_send @q0 %captured : i32
        %value, %received = ac.try_recv @q0 : i32
        ac.schedule @worker0 %value after %one : i32
        ac.wait_until %true
        ac.wait_for @r0
        ac.await_event @e0
        %cursor = ac.trace.open source "pto"
        %observed = ac.probe @q0 kind "queue" : i32
        ac.stat.add @s0 %captured : i32
        ac.assert %true, "runtime"
        ac.yield_sim
      }
      ac.return
    }
  }
)mlir";

OwningOpRef<mlir::ModuleOp> parseAndFreeze(MLIRContext &context,
                                           StringRef source) {
  auto model = parseSourceString<mlir::ModuleOp>(source, &context);
  if (!model)
    return {};
  PassManager manager(&context);
  manager.addPass(createFreezeTopologyPass());
  if (failed(manager.run(*model)))
    return {};
  return model;
}

std::string runFreeze(MLIRContext &context, mlir::ModuleOp model) {
  std::string diagnostic;
  ScopedDiagnosticHandler handler(&context, [&](Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return success();
  });
  PassManager manager(&context);
  manager.addPass(createFreezeTopologyPass());
  if (succeeded(manager.run(model)))
    return "freeze unexpectedly succeeded";
  return diagnostic;
}

std::string moduleText(mlir::ModuleOp model) {
  std::string storage;
  llvm::raw_string_ostream stream(storage);
  model.print(stream);
  return storage;
}

std::string moduleBytecode(mlir::ModuleOp model) {
  std::string storage;
  llvm::raw_string_ostream stream(storage);
  if (failed(writeBytecodeToFile(model, stream)))
    return {};
  return storage;
}

std::string runCanonicalize(MLIRContext &context, mlir::ModuleOp model) {
  std::string diagnostic;
  ScopedDiagnosticHandler handler(&context, [&](Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return success();
  });
  if (succeeded(canonicalizeModel(model)))
    return "canonicalization unexpectedly succeeded";
  return diagnostic;
}

void makeTopLevelNoncanonical(mlir::ModuleOp model) {
  ac::ModuleOp graph = *model.getOps<ac::ModuleOp>().begin();
  graph->moveBefore(&model.getBody()->front());
}

std::string
verifyAfterMutation(MLIRContext &context,
                    const std::function<void(mlir::ModuleOp)> &mutate) {
  OwningOpRef<mlir::ModuleOp> model = parseAndFreeze(context, kProcessModel);
  if (!model)
    return "setup failed";
  mutate(*model);
  std::string diagnostic;
  ScopedDiagnosticHandler handler(&context, [&](Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return success();
  });
  if (succeeded(verifyModel(*model)))
    return "verification unexpectedly succeeded";
  return diagnostic;
}

template <typename OpTy> OpTy one(mlir::ModuleOp model) {
  OpTy result;
  model.walk([&](OpTy candidate) {
    if (!result)
      result = candidate;
  });
  return result;
}

template <typename OpTy> OpTy named(mlir::ModuleOp model, StringRef name) {
  OpTy result;
  model.walk([&](OpTy candidate) {
    if (candidate->template getAttrOfType<StringAttr>(
            SymbolTable::getSymbolAttrName()) ==
        StringAttr::get(model.getContext(), name))
      result = candidate;
  });
  return result;
}

OwningOpRef<mlir::ModuleOp> makeFlatAddressModel(MLIRContext &context,
                                                 uint64_t ownerCount) {
  OpBuilder builder(&context);
  Location loc = builder.getUnknownLoc();
  auto model = mlir::ModuleOp::create(loc);
  model->setAttr("ac.contract_epoch", builder.getStringAttr("0.5"));
  builder.setInsertionPointToStart(model.getBody());
  auto top =
      ac::ModuleOp::create(builder, loc, "Top", builder.getFunctionType({}, {}),
                           builder.getDictionaryAttr({}));
  builder.setInsertionPointToStart(top.addEntryBlock());
  for (uint64_t index = 0; index < ownerCount; ++index) {
    std::string name = ("mem" + Twine(index)).str();
    AddressSpaceOp::create(builder, loc, name, name, name, 32, "byte",
                           Attribute(), FlatSymbolRefAttr(), DictionaryAttr());
  }
  auto workload =
      ProcessOp::create(builder, loc, "workload", "workload", ValueRange{});
  builder.setInsertionPointToStart(&workload.getBody().emplaceBlock());
  auto instrumentation = InstrumentationOp::create(builder, loc, "trace");
  instrumentation.getBody().emplaceBlock();
  TraceOpenOp::create(builder, loc, builder.getIndexType(), "pto");
  YieldSimOp::create(builder, loc);
  builder.setInsertionPointToEnd(&top.getBody().front());
  ReturnOp::create(builder, loc, ValueRange{});
  builder.setInsertionPointToEnd(model.getBody());
  auto workloadRef = SymbolRefAttr::get(
      &context, "Top", {FlatSymbolRefAttr::get(&context, "workload")});
  auto instrumentationRef =
      SymbolRefAttr::get(&context, "Top",
                         {FlatSymbolRefAttr::get(&context, "workload"),
                          FlatSymbolRefAttr::get(&context, "trace")});
  SystemOp::create(
      builder, loc, "owners", "Top", "root", 0, "cycle", workloadRef,
      builder.getDictionaryAttr({
          builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
          builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
      }),
      builder.getArrayAttr({instrumentationRef}),
      builder.getDictionaryAttr({
          builder.getNamedAttr("id", builder.getStringAttr("owners")),
          builder.getNamedAttr("format", builder.getStringAttr("json")),
      }),
      true);
  return OwningOpRef<mlir::ModuleOp>(model);
}

OwningOpRef<mlir::ModuleOp> makeDeepProcessModel(MLIRContext &context,
                                                 uint64_t depth) {
  context.loadDialect<arith::ArithDialect>();
  auto model = parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.system @soc root @Top as "root" tick 0 "cycle"
          workload @Top::@workload seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "default", format = "json"}
          selected true
      ac.module @Top() parameters {} graph {
        ac.process @workload kind "workload" { ac.yield_sim }
        ac.return
      }
    }
  )mlir",
                                                 &context);
  if (!model)
    return {};
  ProcessOp process = one<ProcessOp>(*model);
  Operation *yield = &process.getBody().front().front();
  OpBuilder builder(yield);
  Location loc = builder.getUnknownLoc();
  Value constant =
      arith::ConstantOp::create(builder, loc, builder.getBoolAttr(true));
  Value current = constant;
  for (uint64_t index = 0; index < depth; ++index)
    current = arith::XOrIOp::create(builder, loc, current, constant);
  WaitUntilOp::create(builder, loc, current);
  return model;
}

OwningOpRef<mlir::ModuleOp> makeNestedScfModel(MLIRContext &context,
                                               uint64_t scfDepth) {
  context.loadDialect<arith::ArithDialect, scf::SCFDialect>();
  auto model = parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.system @soc root @Top as "root" tick 0 "cycle"
          workload @Top::@workload seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "default", format = "json"}
          selected true
      ac.module @Top() parameters {} graph {
        ac.process @workload kind "workload" { ac.yield_sim }
        ac.return
      }
    }
  )mlir",
                                                 &context);
  if (!model)
    return {};
  ProcessOp process = one<ProcessOp>(*model);
  Operation *processYield = &process.getBody().front().front();
  OpBuilder builder(processYield);
  Location loc = builder.getUnknownLoc();
  Value condition =
      arith::ConstantOp::create(builder, loc, builder.getBoolAttr(true));
  for (uint64_t index = 0; index < scfDepth; ++index) {
    auto branch = scf::IfOp::create(builder, loc, condition,
                                    /*withElseRegion=*/false);
    builder.setInsertionPoint(branch.getThenRegion().front().getTerminator());
  }
  AssertOp::create(builder, loc, condition, "nested effect");
  return model;
}

class RawNestedRegionModel {
public:
  RawNestedRegionModel(MLIRContext &context, uint64_t depth)
      : model(mlir::ModuleOp::create(UnknownLoc::get(&context))) {
    context.allowUnregisteredDialects();
    OpBuilder builder(&context);
    (*model)->setAttr("ac.contract_epoch", builder.getStringAttr("0.5"));
    Block *block = model->getBody();
    for (uint64_t index = 0; index < depth; ++index) {
      OperationState state(UnknownLoc::get(&context), "test.nested");
      state.addRegion();
      Operation *operation = Operation::create(state);
      block->push_back(operation);
      operations.push_back(operation);
      block = &operation->getRegion(0).emplaceBlock();
    }
  }

  ~RawNestedRegionModel() {
    for (Operation *operation : llvm::reverse(operations)) {
      operation->remove();
      operation->destroy();
    }
  }

  RawNestedRegionModel(const RawNestedRegionModel &) = delete;
  RawNestedRegionModel &operator=(const RawNestedRegionModel &) = delete;

  mlir::ModuleOp get() const { return *model; }

private:
  OwningOpRef<mlir::ModuleOp> model;
  SmallVector<Operation *> operations;
};

OwningOpRef<mlir::ModuleOp> makeRawEmptyRegionModel(MLIRContext &context,
                                                    bool addEmptyBlock) {
  context.allowUnregisteredDialects();
  auto model = mlir::ModuleOp::create(UnknownLoc::get(&context));
  OperationState state(UnknownLoc::get(&context), "test.malformed_region");
  state.addRegion();
  Operation *operation = Operation::create(state);
  model.getBody()->push_back(operation);
  if (addEmptyBlock)
    operation->getRegion(0).emplaceBlock();
  return OwningOpRef<mlir::ModuleOp>(model);
}

enum class ModelEntryPath { Verify, Canonicalize, Freeze };

const char *entryPathName(ModelEntryPath path) {
  switch (path) {
  case ModelEntryPath::Verify:
    return "verify";
  case ModelEntryPath::Canonicalize:
    return "canonicalize";
  case ModelEntryPath::Freeze:
    return "freeze";
  }
  llvm_unreachable("unknown model entry path");
}

LogicalResult invokeModelEntry(MLIRContext &context, mlir::ModuleOp model,
                               ModelEntryPath path) {
  switch (path) {
  case ModelEntryPath::Verify:
    return verifyModel(model);
  case ModelEntryPath::Canonicalize:
    return canonicalizeModel(model);
  case ModelEntryPath::Freeze: {
    PassManager manager(&context);
    manager.addPass(createFreezeTopologyPass());
    return manager.run(model);
  }
  }
  llvm_unreachable("unknown model entry path");
}

std::string runModelEntry(MLIRContext &context, mlir::ModuleOp model,
                          ModelEntryPath path) {
  std::string diagnostic;
  ScopedDiagnosticHandler handler(&context, [&](Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return success();
  });
  if (succeeded(invokeModelEntry(context, model, path)))
    return "model entry unexpectedly succeeded";
  return diagnostic;
}

TEST(ModelAnalysisTest, FrozenProcessSkeletonRejectsEffectSemanticMutation) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  auto symbol = [&](StringRef value) {
    return FlatSymbolRefAttr::get(&context, value);
  };
  struct Mutation {
    const char *name;
    std::function<void(mlir::ModuleOp)> apply;
  };
  const Mutation mutations[] = {
      {"queue target",
       [&](mlir::ModuleOp model) {
         one<TrySendOp>(model).setQueueAttr(symbol("q1"));
       }},
      {"resource target",
       [&](mlir::ModuleOp model) {
         one<WaitForOp>(model).setResourceAttr(symbol("r1"));
       }},
      {"stat target",
       [&](mlir::ModuleOp model) {
         one<StatAddOp>(model).setStatAttr(symbol("s1"));
       }},
      {"process target",
       [&](mlir::ModuleOp model) {
         one<ScheduleOp>(model).setTargetAttr(symbol("worker1"));
       }},
      {"event target",
       [&](mlir::ModuleOp model) {
         one<AwaitEventOp>(model).setEventQueueAttr(symbol("e1"));
       }},
      {"trace source",
       [&](mlir::ModuleOp model) {
         one<TraceOpenOp>(model).setSource("pto_other");
       }},
      {"trace frozen owner",
       [&](mlir::ModuleOp model) {
         auto trace = one<TraceOpenOp>(model);
         auto owner = trace->getAttrOfType<DictionaryAttr>("ac.frozen_owner");
         NamedAttrList fields(owner);
         fields.set("path", StringAttr::get(&context, "wrong.path"));
         trace->setAttr("ac.frozen_owner", fields.getDictionary(&context));
       }},
      {"probe target",
       [&](mlir::ModuleOp model) {
         one<ProbeOp>(model).setTargetAttr(symbol("q1"));
       }},
      {"contract attribute",
       [&](mlir::ModuleOp model) {
         one<AssertOp>(model).setMessage("changed runtime contract");
       }},
      {"control dependency",
       [&](mlir::ModuleOp model) {
         arith::ConstantOp condition;
         model.walk([&](arith::ConstantOp candidate) {
           if (candidate.getType().isInteger(1))
             condition = candidate;
         });
         condition.setValueAttr(IntegerAttr::get(condition.getType(), 0));
       }},
      {"effect value type",
       [&](mlir::ModuleOp model) {
         TrySendOp send = one<TrySendOp>(model);
         arith::ConstantOp replacement;
         model.walk([&](arith::ConstantOp candidate) {
           if (candidate.getType().isInteger(64) &&
               cast<IntegerAttr>(candidate.getValue()).getInt() == 11)
             replacement = candidate;
         });
         send.setQueueAttr(symbol("q64"));
         send->setOperand(0, replacement.getResult());
       }},
  };
  for (const Mutation &mutation : mutations) {
    std::string diagnostic = verifyAfterMutation(context, mutation.apply);
    EXPECT_NE(diagnostic.find("frozen process skeleton mismatch"),
              std::string::npos)
        << mutation.name << ": " << diagnostic;
  }
}

TEST(ModelAnalysisTest,
     FrozenProcessSkeletonPermitsPureContinuationSSARewrite) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model = parseAndFreeze(context, kProcessModel);
  ASSERT_TRUE(model);
  arith::ConstantOp unused;
  model->walk([&](arith::ConstantOp candidate) {
    if (candidate.getType().isInteger(64) &&
        cast<IntegerAttr>(candidate.getValue()).getInt() == 101)
      unused = candidate;
  });
  ASSERT_TRUE(unused);
  unused.setValueAttr(IntegerAttr::get(unused.getType(), 202));
  EXPECT_TRUE(succeeded(verifyModel(*model)));
}

TEST(ModelAnalysisTest, FrozenMutationCannotBeResealedThroughPublicRoutes) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> frozen = parseAndFreeze(context, kProcessModel);
  ASSERT_TRUE(frozen);

  auto retarget = [&](mlir::ModuleOp model) {
    one<TrySendOp>(model).setQueueAttr(FlatSymbolRefAttr::get(&context, "q1"));
  };
  auto cloneFrozen = [&]() {
    return OwningOpRef<mlir::ModuleOp>(cast<mlir::ModuleOp>(frozen->clone()));
  };

  OwningOpRef<mlir::ModuleOp> direct = cloneFrozen();
  retarget(*direct);
  {
    ScopedDiagnosticHandler handler(&context,
                                    [](Diagnostic &) { return success(); });
    EXPECT_TRUE(failed(verifyModel(*direct)));
    ModelAnalysis analysis(*direct);
    EXPECT_TRUE(failed(analysis.verify()));
  }
  EXPECT_NE(
      runFreeze(context, *direct).find("frozen process skeleton mismatch"),
      std::string::npos);

  OwningOpRef<mlir::ModuleOp> missingMarker = cloneFrozen();
  retarget(*missingMarker);
  (*missingMarker)->removeAttr("ac.topology_frozen");
  EXPECT_NE(runFreeze(context, *missingMarker)
                .find("malformed topology freeze marker"),
            std::string::npos);

  OwningOpRef<mlir::ModuleOp> nestedEvidenceOnly = cloneFrozen();
  retarget(*nestedEvidenceOnly);
  for (StringRef name :
       {"ac.freeze_epoch", "ac.frozen_system", "ac.frozen_owners",
        "ac.frozen_primary_workload", "ac.frozen_instrumentation",
        "ac.topology_frozen", "ac.topology_digest"})
    (*nestedEvidenceOnly)->removeAttr(name);
  EXPECT_NE(runFreeze(context, *nestedEvidenceOnly)
                .find("malformed topology freeze marker"),
            std::string::npos);

  auto partial = parseSourceString<mlir::ModuleOp>(kProcessModel, &context);
  ASSERT_TRUE(partial);
  (*partial)->setAttr("ac.freeze_epoch", StringAttr::get(&context, "0.5"));
  EXPECT_NE(
      runFreeze(context, *partial).find("malformed topology freeze marker"),
      std::string::npos);
}

TEST(ModelAnalysisTest,
     CanonicalizationVerifiesEveryFreezeEvidenceFormBeforeWriting) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> frozen = parseAndFreeze(context, kProcessModel);
  ASSERT_TRUE(frozen);

  std::string validBefore = moduleText(*frozen);
  std::string validBytecodeBefore = moduleBytecode(*frozen);
  ASSERT_FALSE(validBytecodeBefore.empty());
  EXPECT_TRUE(succeeded(canonicalizeModel(*frozen)));
  EXPECT_EQ(moduleText(*frozen), validBefore);
  EXPECT_EQ(moduleBytecode(*frozen), validBytecodeBefore);

  enum class Evidence {
    Full,
    MissingMarker,
    NestedOnly,
    PartialEpoch,
    PartialDigest,
  };
  struct Case {
    const char *name;
    Evidence evidence;
    bool retarget;
    const char *diagnostic;
  };
  const Case cases[] = {
      {"full retarget", Evidence::Full, true,
       "frozen process skeleton mismatch"},
      {"marker removed", Evidence::MissingMarker, false,
       "malformed topology freeze marker"},
      {"marker removed retarget", Evidence::MissingMarker, true,
       "malformed topology freeze marker"},
      {"nested evidence only", Evidence::NestedOnly, false,
       "malformed topology freeze marker"},
      {"nested evidence only retarget", Evidence::NestedOnly, true,
       "malformed topology freeze marker"},
      {"partial epoch", Evidence::PartialEpoch, false,
       "malformed topology freeze marker"},
      {"partial epoch retarget", Evidence::PartialEpoch, true,
       "malformed topology freeze marker"},
      {"partial digest", Evidence::PartialDigest, false,
       "malformed topology freeze marker"},
      {"partial digest retarget", Evidence::PartialDigest, true,
       "malformed topology freeze marker"},
  };

  auto buildCase = [&](const Case &testCase) {
    OwningOpRef<mlir::ModuleOp> model;
    if (testCase.evidence == Evidence::PartialEpoch ||
        testCase.evidence == Evidence::PartialDigest) {
      model = parseSourceString<mlir::ModuleOp>(kProcessModel, &context);
      if (!model)
        return model;
      if (testCase.evidence == Evidence::PartialEpoch)
        (*model)->setAttr("ac.freeze_epoch", StringAttr::get(&context, "0.5"));
      else
        (*model)->setAttr("ac.topology_digest",
                          StringAttr::get(&context, std::string(64, '0')));
    } else {
      model =
          OwningOpRef<mlir::ModuleOp>(cast<mlir::ModuleOp>(frozen->clone()));
      if (testCase.evidence == Evidence::MissingMarker)
        (*model)->removeAttr("ac.topology_frozen");
      if (testCase.evidence == Evidence::NestedOnly)
        for (StringRef name :
             {"ac.freeze_epoch", "ac.frozen_system", "ac.frozen_owners",
              "ac.frozen_primary_workload", "ac.frozen_instrumentation",
              "ac.topology_frozen", "ac.topology_digest"})
          (*model)->removeAttr(name);
    }
    if (testCase.retarget)
      one<TrySendOp>(*model).setQueueAttr(
          FlatSymbolRefAttr::get(&context, "q1"));
    makeTopLevelNoncanonical(*model);
    return model;
  };

  for (const Case &testCase : cases) {
    OwningOpRef<mlir::ModuleOp> model = buildCase(testCase);
    ASSERT_TRUE(model) << testCase.name;
    std::string before = moduleText(*model);
    std::string bytecodeBefore = moduleBytecode(*model);
    ASSERT_FALSE(bytecodeBefore.empty()) << testCase.name;
    std::string diagnostic = runCanonicalize(context, *model);
    EXPECT_NE(diagnostic.find(testCase.diagnostic), std::string::npos)
        << testCase.name << ": " << diagnostic;
    EXPECT_EQ(moduleText(*model), before) << testCase.name;
    EXPECT_EQ(moduleBytecode(*model), bytecodeBefore) << testCase.name;
  }
}

TEST(ModelAnalysisTest, UnfrozenCanonicalizationRemainsDeterministic) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model =
      parseSourceString<mlir::ModuleOp>(kProcessModel, &context);
  ASSERT_TRUE(model);
  EXPECT_TRUE(succeeded(canonicalizeModel(*model)));
  std::string first = moduleText(*model);
  EXPECT_TRUE(succeeded(canonicalizeModel(*model)));
  EXPECT_EQ(moduleText(*model), first);
}

TEST(ModelAnalysisTest, FrozenDigestCommitsNestedGuardParentage) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model = parseAndFreeze(context, R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.protocol @p {
        ac.role @a dual @b cardinality "exclusive"
        ac.role @b dual @a cardinality "exclusive"
        ac.state @idle initial true terminal false
        ac.event @x from @a to @b payload i1 action "notify"
        ac.event @y from @a to @b payload i1 action "notify"
        ac.transition from @idle to @idle on @x transfer false retain false guard {
          %x = arith.constant true
        }
        ac.transition from @idle to @idle on @y transfer false retain false guard {
          %y = arith.constant false
        }
      }
      ac.system @soc root @Top as "root" tick 0 "cycle"
          workload @Top::@workload seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "default", format = "json"}
          selected true
      ac.module @Top() parameters {} graph {
        ac.process @workload kind "workload" { ac.yield_sim }
        ac.return
      }
    }
  )mlir");
  ASSERT_TRUE(model);
  SmallVector<TransitionOp> transitions;
  model->walk(
      [&](TransitionOp transition) { transitions.push_back(transition); });
  ASSERT_EQ(transitions.size(), 2u);
  Operation *first = &transitions[0].getGuard().front().front();
  Operation *second = &transitions[1].getGuard().front().front();
  first->moveBefore(second);
  second->moveBefore(&transitions[0].getGuard().front(),
                     transitions[0].getGuard().front().end());
  std::string diagnostic;
  ScopedDiagnosticHandler handler(&context, [&](Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return success();
  });
  EXPECT_TRUE(failed(verifyModel(*model)));
  EXPECT_NE(diagnostic.find("frozen topology digest mismatch"),
            std::string::npos);
}

TEST(ModelAnalysisTest, AddressSpacesFreezeAsAbsoluteStateOwners) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model = parseAndFreeze(context, R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.system @soc root @Top as "root" tick 0 "cycle"
          workload @Top::@workload seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "default", format = "json"}
          selected true
      ac.module @Leaf() parameters {} graph {
        ac.address_space @mem width 32 unit "byte" id "mem" path "mem"
        ac.address_space @other width 32 unit "byte" id "other" path "other"
        ac.process @observer kind "monitor" {
          %value = ac.probe @mem kind "storage" : i64
          ac.yield_sim
        }
        ac.return
      }
      "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({
        "ac.instance"() <{definition = @Leaf, sym_name = "left", stable_id = "left", path = "left", static_args = {}}> : () -> ()
        "ac.array"() <{definition = @Leaf, sym_name = "banks", stable_id = "banks", path = "banks", shape = array<i64: 2>, static_args = [{}, {}]}> : () -> ()
        "ac.instances"() <{sym_name = "mix", stable_id = "mix", path = "mix", definitions = [@Leaf, @Leaf], names = ["x", "y"], stable_ids = ["x", "y"], paths = ["x", "y"], interface = () -> (), static_args = [{}, {}]}> : () -> ()
        ac.address_space @root_mem width 32 unit "byte" id "root_mem" path "root_mem"
        ac.process @workload kind "workload" { ac.yield_sim }
        "ac.return"() : () -> ()
      }) : () -> ()
    }
  )mlir");
  ASSERT_TRUE(model);
  AddressSpaceOp memory = named<AddressSpaceOp>(*model, "mem");
  ASSERT_TRUE(memory);
  ArrayAttr owners = memory->getAttrOfType<ArrayAttr>("ac.frozen_owners");
  ASSERT_TRUE(owners);
  ASSERT_EQ(owners.size(), 5u);
  const char *expectedPaths[] = {"root.banks[0].mem", "root.banks[1].mem",
                                 "root.left.mem", "root.mix.x.mem",
                                 "root.mix.y.mem"};
  for (auto [owner, expected] : llvm::zip(owners, expectedPaths))
    EXPECT_EQ(cast<DictionaryAttr>(owner).getAs<StringAttr>("path").getValue(),
              expected);

  AddressSpaceOp rootMemory = named<AddressSpaceOp>(*model, "root_mem");
  ASSERT_TRUE(rootMemory);
  ArrayAttr rootOwners =
      rootMemory->getAttrOfType<ArrayAttr>("ac.frozen_owners");
  ASSERT_TRUE(rootOwners);
  ASSERT_EQ(rootOwners.size(), 1u);
  EXPECT_EQ(
      cast<DictionaryAttr>(rootOwners[0]).getAs<StringAttr>("path").getValue(),
      "root.root_mem");

  ProbeOp probe = one<ProbeOp>(*model);
  SmallVector<MemoryEffects::EffectInstance> effects;
  cast<MemoryEffectOpInterface>(probe.getOperation()).getEffects(effects);
  ASSERT_FALSE(effects.empty());
  for (const MemoryEffects::EffectInstance &effect : effects) {
    auto parameters = cast<DictionaryAttr>(effect.getParameters());
    EXPECT_EQ(parameters.getAs<StringAttr>("identity_phase").getValue(),
              "elaborated_absolute");
    EXPECT_EQ(parameters.getAs<ArrayAttr>("owners"), owners);
  }

  memory->removeAttr("ac.frozen_owners");
  ScopedDiagnosticHandler handler(&context,
                                  [&](Diagnostic &) { return success(); });
  EXPECT_TRUE(failed(verifyModel(*model)));
}

TEST(ModelAnalysisTest, AddressSpacesParticipateInSaturatedOwnerBudget) {
  MLIRContext context;
  context.loadDialect<ACIRDialect>();
  OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto model = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(model.getBody());
  auto leaf =
      ac::ModuleOp::create(builder, loc, "Leaf", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(leaf.addEntryBlock());
  auto addAddress = [&](StringRef name) {
    return AddressSpaceOp::create(builder, loc, name, name, name, 32, "byte",
                                  Attribute(), FlatSymbolRefAttr(),
                                  DictionaryAttr());
  };
  addAddress("mem0");
  addAddress("mem1");
  ReturnOp::create(builder, loc, ValueRange{});

  SmallVector<Attribute> staticArgs(512, Attribute(emptyDictionary));
  builder.setInsertionPointToEnd(model.getBody());
  auto middle =
      ac::ModuleOp::create(builder, loc, "Middle", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(middle.addEntryBlock());
  ArrayOp::create(builder, loc, TypeRange{}, ValueRange{}, "Leaf", "leaves",
                  "leaves", "leaves", builder.getDenseI64ArrayAttr({512}),
                  builder.getArrayAttr(staticArgs));
  ReturnOp::create(builder, loc, ValueRange{});

  builder.setInsertionPointToEnd(model.getBody());
  auto top =
      ac::ModuleOp::create(builder, loc, "Top", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(top.addEntryBlock());
  ArrayOp::create(builder, loc, TypeRange{}, ValueRange{}, "Middle", "middles",
                  "middles", "middles", builder.getDenseI64ArrayAttr({512}),
                  builder.getArrayAttr(staticArgs));
  ReturnOp::create(builder, loc, ValueRange{});

  builder.setInsertionPointToEnd(model.getBody());
  auto seed = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
  });
  auto results = builder.getDictionaryAttr({
      builder.getNamedAttr("id", builder.getStringAttr("owners")),
      builder.getNamedAttr("format", builder.getStringAttr("json")),
  });
  SystemOp::create(builder, loc, "owners", "Top", "root", 0, "cycle",
                   FlatSymbolRefAttr(), seed, builder.getArrayAttr({}), results,
                   true);
  EXPECT_TRUE(succeeded(verifyGraphStructure(model)));

  auto overBudget = cast<mlir::ModuleOp>(model->clone());
  auto overBudgetLeaf = *overBudget.getOps<ac::ModuleOp>().begin();
  builder.setInsertionPoint(&overBudgetLeaf.getBody().front().back());
  addAddress("mem2");
  std::string diagnostic;
  ScopedDiagnosticHandler handler(&context, [&](Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return success();
  });
  EXPECT_TRUE(failed(verifyGraphStructure(overBudget)));
  EXPECT_NE(diagnostic.find("owner count exceeds bound 1048576"),
            std::string::npos);
}

TEST(ModelAnalysisTest, FullFreezePathHasExactLinearIndexedWork) {
  MLIRContext context;
  context.loadDialect<ACIRDialect>();
  auto measure = [&](uint64_t ownerCount) {
    OwningOpRef<mlir::ModuleOp> model =
        makeFlatAddressModel(context, ownerCount);
    acir::detail::FreezeWork work;
    acir::detail::ScopedFreezeWorkRecorder recorder(work);
    PassManager manager(&context);
    manager.addPass(createFreezeTopologyPass());
    EXPECT_TRUE(succeeded(manager.run(*model)));
    // The complete path constructs the seal, then independently reconstructs
    // the manifest during final frozen verification.
    EXPECT_EQ(work.stateIndexInsertions, 2 * (ownerCount + 1));
    EXPECT_EQ(work.topologyIndexLookups, 2 * (ownerCount + 1));
    EXPECT_EQ(work.manifestIndexInsertions, ownerCount + 2);
    EXPECT_EQ(work.manifestOwnerLookups, 2u);
    EXPECT_EQ(work.declarationIndexInsertions, ownerCount + 1);
    EXPECT_EQ(work.declarationLookups, ownerCount + 1);
    return work.total();
  };
  uint64_t work1000 = measure(1000);
  uint64_t work4000 = measure(4000);
  EXPECT_EQ(work1000, 7010u);
  EXPECT_EQ(work4000, 28010u);
  EXPECT_EQ(work4000 - 10, 4 * (work1000 - 10));
}

TEST(ModelAnalysisTest, DeepProcessDependencyChainsFreezeWithoutStackGrowth) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  for (uint64_t depth : {30000u, 200000u}) {
    OwningOpRef<mlir::ModuleOp> model = makeDeepProcessModel(context, depth);
    ASSERT_TRUE(model) << depth;
    PassManager manager(&context);
    manager.addPass(createFreezeTopologyPass());
    EXPECT_TRUE(succeeded(manager.run(*model))) << depth;
  }
}

TEST(ModelAnalysisTest, ModelEntryPathsAcceptExactRegionNestingLimit) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  constexpr uint64_t scfDepthAtLimit = 509;
  for (ModelEntryPath path :
       {ModelEntryPath::Verify, ModelEntryPath::Canonicalize,
        ModelEntryPath::Freeze}) {
    OwningOpRef<mlir::ModuleOp> model =
        makeNestedScfModel(context, scfDepthAtLimit);
    ASSERT_TRUE(model) << entryPathName(path);
    std::string diagnostic;
    ScopedDiagnosticHandler handler(&context, [&](Diagnostic &value) {
      llvm::raw_string_ostream(diagnostic) << value;
      return success();
    });
    EXPECT_TRUE(succeeded(invokeModelEntry(context, *model, path)))
        << entryPathName(path) << ": " << diagnostic;
  }
}

TEST(ModelAnalysisTest,
     ModelEntryPathsRejectRegionNestingLimitPlusOneDeterministically) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  constexpr uint64_t scfDepthOverLimit = 510;
  constexpr StringLiteral expected =
      "whole-model region nesting exceeds ACIR capability limit 512";
  std::string firstDiagnostic;
  for (ModelEntryPath path :
       {ModelEntryPath::Verify, ModelEntryPath::Canonicalize,
        ModelEntryPath::Freeze}) {
    OwningOpRef<mlir::ModuleOp> model =
        makeNestedScfModel(context, scfDepthOverLimit);
    ASSERT_TRUE(model) << entryPathName(path);
    std::string diagnostic = runModelEntry(context, *model, path);
    EXPECT_EQ(diagnostic, expected.str()) << entryPathName(path);
    if (firstDiagnostic.empty())
      firstDiagnostic = diagnostic;
    else
      EXPECT_EQ(diagnostic, firstDiagnostic) << entryPathName(path);
  }
}

TEST(ModelAnalysisTest,
     HostileRawRegionNestingRejectsWithoutStackGrowthOnEveryEntryPath) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  constexpr StringLiteral expected =
      "whole-model region nesting exceeds ACIR capability limit 512";
  std::string firstDiagnostic;
  for (ModelEntryPath path :
       {ModelEntryPath::Verify, ModelEntryPath::Canonicalize,
        ModelEntryPath::Freeze}) {
    RawNestedRegionModel model(context, 10000);
    std::string diagnostic = runModelEntry(context, model.get(), path);
    EXPECT_EQ(diagnostic, expected.str()) << entryPathName(path);
    if (firstDiagnostic.empty())
      firstDiagnostic = diagnostic;
    else
      EXPECT_EQ(diagnostic, firstDiagnostic) << entryPathName(path);
  }
}

TEST(ModelAnalysisTest, StructuralPreflightHandlesEmptyRegionsAndBlocks) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  for (bool addEmptyBlock : {false, true}) {
    OwningOpRef<mlir::ModuleOp> model =
        makeRawEmptyRegionModel(context, addEmptyBlock);
    ASSERT_TRUE(model);
    EXPECT_TRUE(succeeded(::acir::detail::preflightModelStructure(*model)))
        << "add_empty_block=" << addEmptyBlock;
  }
}

TEST(ModelAnalysisTest, ProcessDependencyCapabilityIsCheckedBeforeGrowth) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model = makeDeepProcessModel(context, 1025);
  ASSERT_TRUE(model);
  acir::detail::ScopedProcessSkeletonLimits limits(/*nodes=*/1024,
                                                   /*edges=*/4096);
  std::string diagnostic = runFreeze(context, *model);
  EXPECT_NE(diagnostic.find(
                "process skeleton dependency node count exceeds bound 1024"),
            std::string::npos)
      << diagnostic;
}

TEST(ModelAnalysisTest, ProcessDependencyEdgeCapabilityIsCheckedBeforeGrowth) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model = makeDeepProcessModel(context, 16);
  ASSERT_TRUE(model);
  acir::detail::ScopedProcessSkeletonLimits limits(/*nodes=*/64, /*edges=*/8);
  std::string diagnostic = runFreeze(context, *model);
  EXPECT_NE(
      diagnostic.find("process skeleton dependency edge count exceeds bound 8"),
      std::string::npos)
      << diagnostic;
}

TEST(ModelAnalysisTest, ProcessSkeletonIncludesNestedControlParents) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model = parseAndFreeze(context, R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.system @soc root @Top as "root" tick 0 "cycle"
          workload @Top::@workload seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "default", format = "json"}
          selected true
      ac.module @Top() parameters {} graph {
        ac.stat @count kind "counter"
        ac.process @workload kind "workload" {
          %condition = arith.constant true
          scf.if %condition {
            %one = arith.constant 1 : i32
            ac.stat.add @count %one : i32
          }
          ac.yield_sim
        }
        ac.return
      }
    }
  )mlir");
  ASSERT_TRUE(model);
  arith::ConstantOp condition;
  model->walk([&](arith::ConstantOp candidate) {
    if (candidate.getType().isInteger(1))
      condition = candidate;
  });
  ASSERT_TRUE(condition);
  condition.setValueAttr(IntegerAttr::get(condition.getType(), 0));
  std::string diagnostic;
  ScopedDiagnosticHandler handler(&context, [&](Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return success();
  });
  EXPECT_TRUE(failed(verifyModel(*model)));
  EXPECT_NE(diagnostic.find("frozen process skeleton mismatch"),
            std::string::npos);
}

TEST(ACDataFlowAnalyzerTest, InfersValueLifetimeAndLexicalStateOwnership) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model =
      parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "variables"} {
      ac.var.decl @counter type i8 init 0 : i8 owner "/" stable_id "var/counter"
      ac.table @state entry i32 entries 2 init 0 owner "/" stable_id "table/state"
      %state_view = ac.table.read @state depth 1 latency 1 address {
      ^address:
        %state_index = ac.var.constant 0 : i64 as !ac.var<i64>
        ac.table.yield %state_index : !ac.var<i64>
      } when {
      ^when:
        %enabled = ac.var.constant true as !ac.var<i1>
        ac.table.yield %enabled : !ac.var<i1>
      } -> !ac.queue<i32>
      ac.sink %state_view : !ac.queue<i32>
      %input = ac.source depth 1 latency 1 : !ac.queue<i32>
      %output = ac.scope @worker(%input) {
      ^body(%inner: !ac.queue<i32>):
        %result = ac.transform %inner depths [1] latencies [1] {
        ^transform(%item: !ac.var<i32>):
          %one = ac.var.constant 1 : i32 as !ac.var<i32>
          %sum = ac.var.add %item, %one : !ac.var<i32>
          ac.transform.yield %sum : !ac.var<i32>
        } : (!ac.queue<i32>) -> !ac.queue<i32>
        ac.scope.yield %result : !ac.queue<i32>
      } : (!ac.queue<i32>) -> !ac.queue<i32>
      ac.sink %output : !ac.queue<i32>
    }
  )mlir",
                                        &context);
  ASSERT_TRUE(model);

  ACDataFlowAnalyzer analysis(model->getOperation());
  ASSERT_TRUE(succeeded(analysis.run()));

  ac::VarConstantOp constant;
  ac::VarAddOp sum;
  ac::TransformOp transform;
  ac::VarDeclOp counter;
  ac::TableOp state;
  model->walk([&](ac::VarConstantOp operation) {
    if (cast<ac::VarType>(operation.getResult().getType())
            .getElementType()
            .isInteger(32))
      constant = operation;
  });
  model->walk([&](ac::VarAddOp operation) { sum = operation; });
  model->walk([&](ac::TransformOp operation) { transform = operation; });
  model->walk([&](ac::VarDeclOp operation) { counter = operation; });
  model->walk([&](ac::TableOp operation) { state = operation; });
  ASSERT_TRUE(constant && sum && transform && counter && state);

  VariableProperties constantInfo = analysis.lookup(constant.getResult());
  EXPECT_EQ(VariableLifetime::Static, constantInfo.lifetime);
  EXPECT_EQ(VariableUpdate::Immutable, constantInfo.update);
  EXPECT_EQ("/worker", constantInfo.ownerPath);

  VariableProperties argumentInfo =
      analysis.lookup(transform.getBody().front().getArgument(0));
  EXPECT_EQ(VariableLifetime::Temporary, argumentInfo.lifetime);
  EXPECT_EQ(VariableUpdate::Immutable, argumentInfo.update);
  EXPECT_EQ("/worker", argumentInfo.ownerPath);

  VariableProperties sumInfo = analysis.lookup(sum.getResult());
  EXPECT_EQ(VariableLifetime::Temporary, sumInfo.lifetime);
  EXPECT_EQ(VariableUpdate::Immutable, sumInfo.update);
  EXPECT_EQ("/worker", sumInfo.ownerPath);

  VariableProperties stateInfo = analysis.lookupOwnedState(state);
  EXPECT_EQ(VariableLifetime::Persistent, stateInfo.lifetime);
  EXPECT_EQ(VariableUpdate::Assignable, stateInfo.update);
  EXPECT_EQ("/", stateInfo.ownerPath);

  VariableProperties counterInfo = analysis.lookupOwnedState(counter);
  EXPECT_EQ(VariableLifetime::Persistent, counterInfo.lifetime);
  EXPECT_EQ(VariableUpdate::Assignable, counterInfo.update);
  EXPECT_EQ("/", counterInfo.ownerPath);
}

TEST(ValueConstraintTest, JoinIsIdempotentAndAbsorbingAcrossRepresentations) {
  ValueConstraint interval = ValueConstraint::closedInterval(0, 7);
  ValueConstraint constant = ValueConstraint::constant(3);
  ValueConstraint finite = ValueConstraint::finiteSet({1, 4});
  ValueConstraint unknown = ValueConstraint::unknown();

  EXPECT_EQ(interval, ValueConstraint::join(interval, interval));
  EXPECT_EQ(interval, ValueConstraint::join(interval, constant));
  EXPECT_EQ(interval, ValueConstraint::join(constant, interval));
  EXPECT_EQ(unknown, ValueConstraint::join(interval, unknown));
  EXPECT_EQ(unknown, ValueConstraint::join(unknown, finite));
  EXPECT_EQ(ValueConstraint::finiteSet({1, 3, 4}),
            ValueConstraint::join(finite, constant));

  ValueConstraint widened = ValueConstraint::join(finite, interval);
  EXPECT_EQ(widened, ValueConstraint::join(widened, finite));
  EXPECT_EQ(widened, ValueConstraint::join(widened, interval));
}

TEST(ACDataFlowAnalyzerTest, InfersBoundedUnsignedValueConstraints) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model =
      parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "constraints"} {
      ac.type_scope @types {
        ac.enum @Mode enumerants ["idle", "run", "wait"]
      } {dlti.dl_spec = #dlti.dl_spec<!ac.enum<@types::@Mode> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}>}
      %input = ac.source depth 1 latency 1 : !ac.queue<i3>
      %output = ac.transform %input depths [1] latencies [1] {
      ^body(%item: !ac.var<i3>):
        %three = ac.var.constant 3 : i3 as !ac.var<i3>
        %four = ac.var.constant 4 : i3 as !ac.var<i3>
        %seven = ac.var.constant 7 : i3 as !ac.var<i3>
        %zero = ac.var.constant 0 : i3 as !ac.var<i3>
        %one = ac.var.constant 1 : i3 as !ac.var<i3>
        %wrapped_add = ac.var.add %seven, %one : !ac.var<i3>
        %wrapped_sub = ac.var.sub %zero, %one : !ac.var<i3>
        %overshifted = ac.var.shl %one, %three : !ac.var<i3>
        %wide = ac.var.constant 7 : i64 as !ac.var<i64>
        %wide_concat = ac.var.concat %wide : !ac.var<i64> -> !ac.var<i64>
        %constant_match = ac.var.matches %seven mask 3 value 3 : !ac.var<i3> -> !ac.var<i1>
        %constant_miss = ac.var.matches %seven mask 3 value 2 : !ac.var<i3> -> !ac.var<i1>
        %wildcard_match = ac.var.matches %item mask 0 value 0 : !ac.var<i3> -> !ac.var<i1>
        %dynamic_match = ac.var.matches %item mask 3 value 2 : !ac.var<i3> -> !ac.var<i1>
        %idle = ac.var.enum @types::@Mode "idle" : !ac.var<!ac.enum<@types::@Mode>>
        %run = ac.var.enum @types::@Mode "run" : !ac.var<!ac.enum<@types::@Mode>>
        %enum_different = ac.var.cmp "ne" %idle, %run : !ac.var<!ac.enum<@types::@Mode>> -> !ac.var<i1>
        %masked = ac.var.and %item, %three : !ac.var<i3>
        %condition = ac.var.cmp "ult" %item, %four : !ac.var<i3> -> !ac.var<i1>
        %selected = ac.var.select %condition, %three, %four : !ac.var<i1>, !ac.var<i3> -> !ac.var<i3>
        %priority, %valid = ac.var.priority_encode %item order "low" : !ac.var<i3> -> !ac.var<i2>, !ac.var<i1>
        ac.transform.yield %masked : !ac.var<i3>
      } : (!ac.queue<i3>) -> !ac.queue<i3>
      ac.sink %output : !ac.queue<i3>
    }
  )mlir",
                                        &context);
  ASSERT_TRUE(model);

  ACDataFlowAnalyzer analysis(model->getOperation());
  ASSERT_TRUE(succeeded(analysis.run()));
  ac::TransformOp transform;
  ac::VarAndOp masked;
  ac::VarSelectOp selected;
  ac::VarPriorityEncodeOp priority;
  SmallVector<ac::VarAddOp> additions;
  ac::VarSubOp subtraction;
  ac::VarShlOp overshift;
  ac::VarConcatOp wideConcat;
  ac::VarCmpOp enumCompare;
  SmallVector<ac::VarMatchesOp> matches;
  model->walk([&](ac::TransformOp operation) { transform = operation; });
  model->walk([&](ac::VarAndOp operation) { masked = operation; });
  model->walk([&](ac::VarSelectOp operation) { selected = operation; });
  model->walk(
      [&](ac::VarPriorityEncodeOp operation) { priority = operation; });
  model->walk([&](ac::VarAddOp operation) { additions.push_back(operation); });
  model->walk([&](ac::VarSubOp operation) { subtraction = operation; });
  model->walk([&](ac::VarShlOp operation) { overshift = operation; });
  model->walk([&](ac::VarConcatOp operation) { wideConcat = operation; });
  model->walk([&](ac::VarCmpOp operation) {
    if (mlir::isa<ac::EnumType>(
            cast<ac::VarType>(operation.getLhs().getType()).getElementType()))
      enumCompare = operation;
  });
  model->walk([&](ac::VarMatchesOp operation) { matches.push_back(operation); });
  ASSERT_TRUE(transform && masked && selected && priority);
  ASSERT_EQ(1u, additions.size());

  ValueConstraint argument =
      analysis.lookupConstraint(transform.getBody().front().getArgument(0));
  EXPECT_EQ(ValueConstraintKind::ClosedInterval, argument.kind);
  EXPECT_EQ(0u, argument.lower);
  EXPECT_EQ(7u, argument.upper);
  EXPECT_TRUE(analysis.provesWithin(masked.getResult(), 0, 3));

  ValueConstraint selection = analysis.lookupConstraint(selected.getResult());
  EXPECT_EQ(ValueConstraintKind::FiniteSet, selection.kind);
  EXPECT_EQ((llvm::SmallVector<uint64_t, 8>{3, 4}), selection.values);
  EXPECT_TRUE(analysis.provesWithin(priority.getIndex(), 0, 2));
  EXPECT_TRUE(analysis.provesWithin(priority.getValid(), 0, 1));
  EXPECT_EQ(ValueConstraint::constant(0),
            analysis.lookupConstraint(additions.front().getResult()));
  EXPECT_EQ(ValueConstraint::constant(7),
            analysis.lookupConstraint(subtraction.getResult()));
  EXPECT_EQ(ValueConstraint::constant(0),
            analysis.lookupConstraint(overshift.getResult()));
  EXPECT_EQ(ValueConstraint::constant(7),
            analysis.lookupConstraint(wideConcat.getResult()));
  EXPECT_EQ(ValueConstraint::constant(1),
            analysis.lookupConstraint(enumCompare.getResult()));
  ASSERT_EQ(4u, matches.size());
  EXPECT_EQ(ValueConstraint::constant(1),
            analysis.lookupConstraint(matches[0].getResult()));
  EXPECT_EQ(ValueConstraint::constant(0),
            analysis.lookupConstraint(matches[1].getResult()));
  EXPECT_EQ(ValueConstraint::constant(1),
            analysis.lookupConstraint(matches[2].getResult()));
  EXPECT_EQ(ValueConstraint::closedInterval(0, 1),
            analysis.lookupConstraint(matches[3].getResult()));
}

TEST(ACDataFlowAnalyzerTest, InfersOrderedStateAccessFootprints) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model =
      parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "footprints"} {
      ac.type_scope @types {
        ac.struct @Entry fields [{name = "index", type = i1}, {name = "value", type = i7}]
      } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}>}
      ac.table @entries entry !ac.struct<@types::@Entry> entries 2 init 0 owner "/" stable_id "table/entries"
      %input = ac.source depth 1 latency 1 : !ac.queue<!ac.struct<@types::@Entry>>
      %output = ac.rule %input depths [1] latencies [1]
          name "replace" stable_id "replace" domain "cycle"
          type exact input_fact committed_input {
      ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
        %index = ac.var.get %item field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
        %old = ac.table.get @entries[%index] : !ac.var<i1> -> !ac.var<!ac.struct<@types::@Entry>>
        ac.table.propose @entries[%index] = %item mode "replace"
            write_fields ["index", "value"] : !ac.var<i1>, !ac.var<!ac.struct<@types::@Entry>>
        ac.rule.return %old : !ac.var<!ac.struct<@types::@Entry>>
      } : (!ac.queue<!ac.struct<@types::@Entry>>) -> !ac.queue<!ac.struct<@types::@Entry>>
      ac.sink %output : !ac.queue<!ac.struct<@types::@Entry>>
    }
  )mlir",
                                        &context);
  ASSERT_TRUE(model);

  ACDataFlowAnalyzer analysis(model->getOperation());
  ASSERT_TRUE(succeeded(analysis.run()));
  ac::RuleOp rule;
  model->walk([&](ac::RuleOp operation) { rule = operation; });
  ASSERT_TRUE(rule);

  llvm::SmallVector<StateAccessFootprint> footprints =
      analysis.stateFootprints(rule.getOperation());
  ASSERT_EQ(2u, footprints.size());
  EXPECT_EQ("entries", footprints[0].resource);
  EXPECT_EQ("read", footprints[0].access);
  EXPECT_EQ("dynamic", footprints[0].indexKind);
  EXPECT_TRUE(footprints[0].fields.empty());
  EXPECT_EQ("entries", footprints[1].resource);
  EXPECT_EQ("replace", footprints[1].access);
  EXPECT_EQ("dynamic", footprints[1].indexKind);
  EXPECT_EQ((std::vector<std::string>{"index", "value"}), footprints[1].fields);
}

TEST(ACDataFlowAnalyzerTest, InfersConditionalEffectSnapshotReadSet) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model =
      parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.table @epoch entry i8 entries 1 init 0 owner "/" stable_id "table/epoch"
      ac.table @entries entry i8 entries 2 init 0 owner "/" stable_id "table/entries"
      %input = ac.source depth 1 latency 1 : !ac.queue<i8>
      ac.rule %input depths [] latencies [] name "complete" stable_id "complete"
          domain "cycle" type exact input_fact committed_input {
      ^body(%item: !ac.var<i8>):
        %zero = ac.var.constant false as !ac.var<i1>
        %index = ac.var.constant false as !ac.var<i1>
        %old_epoch = ac.table.get @epoch[%zero] : !ac.var<i1> -> !ac.var<i8>
        %old_entry = ac.table.get @entries[%index] : !ac.var<i1> -> !ac.var<i8>
        %epoch_matches = ac.var.cmp "eq" %old_epoch, %item : !ac.var<i8> -> !ac.var<i1>
        %entry_matches = ac.var.cmp "eq" %old_entry, %item : !ac.var<i8> -> !ac.var<i1>
        %fresh = ac.var.and %epoch_matches, %entry_matches : !ac.var<i1>
        %candidate = ac.var.constant true as !ac.var<i1>
        ac.rule.condition %candidate : !ac.var<i1>
        ac.table.propose @entries[%index] = %item when %fresh : !ac.var<i1>
            mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
        ac.rule.return
      } : (!ac.queue<i8>) -> ()
    }
  )mlir",
                                        &context);
  ASSERT_TRUE(model);

  ACDataFlowAnalyzer analysis(model->getOperation());
  ASSERT_TRUE(succeeded(analysis.run()));
  ac::RuleOp rule;
  model->walk([&](ac::RuleOp operation) { rule = operation; });
  ASSERT_TRUE(rule);
  llvm::SmallVector<StateSnapshotFootprint> snapshots =
      analysis.stateSnapshots(rule.getOperation());
  ASSERT_EQ(2u, snapshots.size());
  llvm::StringSet<> resources;
  for (const StateSnapshotFootprint &snapshot : snapshots) {
    resources.insert(snapshot.resource);
    EXPECT_EQ("static", snapshot.indexKind);
    EXPECT_TRUE(snapshot.index);
    EXPECT_TRUE(snapshot.predicate);
    EXPECT_EQ((std::vector<std::string>{"$entry"}), snapshot.fields);
  }
  EXPECT_TRUE(resources.contains("epoch"));
  EXPECT_TRUE(resources.contains("entries"));
}

TEST(ACDataFlowAnalyzerTest, InfersMatchAndChooseSnapshotSets) {
  DialectRegistry registry;
  registerAllDialects(registry);
  MLIRContext context(registry);
  OwningOpRef<mlir::ModuleOp> model =
      parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.table @entries entry i2 entries 4 init 0 owner "/" stable_id "table/entries"
      ac.table @ready entry i1 entries 4 init 0 owner "/" stable_id "table/ready"
      ac.table @priority entry i2 entries 4 init 0 owner "/" stable_id "table/priority"
      %output = ac.rule depths [1] latencies [1] name "issue"
          stable_id "issue" domain "cycle" type exact
          input_fact committed_input {
      ^body:
        %mask = ac.table.match @entries predicate {
        ^bb0(%entry: !ac.var<i2>):
          %is_ready = ac.table.get @ready[%entry] : !ac.var<i2> -> !ac.var<i1>
          ac.table.match.yield %is_ready : !ac.var<i1>
        } -> !ac.var<i4>
        %index, %present = ac.table.choose @entries %mask : !ac.var<i4>
            count 1 policy "min" key {
        ^bb0(%entry: !ac.var<i2>):
          %priority = ac.table.get @priority[%entry] : !ac.var<i2> -> !ac.var<i2>
          ac.table.choose.yield %priority : !ac.var<i2>
        } -> !ac.var<i2>, !ac.var<i1>
        %old = ac.table.get @entries[%index] : !ac.var<i2> -> !ac.var<i2>
        ac.rule.condition %present : !ac.var<i1>
        ac.table.propose @entries[%index] = %old mode "replace"
            write_fields ["$entry"] : !ac.var<i2>, !ac.var<i2>
        %ready_output = ac.marker.obligation %old state pending
            resolver handshake origin "issue:return" path "true" : !ac.var<i2>
        ac.rule.return %ready_output : !ac.var<i2>
      } : () -> !ac.queue<i2>
      ac.sink %output : !ac.queue<i2>
    }
  )mlir",
                                        &context);
  ASSERT_TRUE(model);

  ACDataFlowAnalyzer analysis(model->getOperation());
  ASSERT_TRUE(succeeded(analysis.run()));
  ac::RuleOp rule;
  ac::TableMatchOp match;
  ac::TableChooseOp choose;
  model->walk([&](ac::RuleOp operation) { rule = operation; });
  model->walk([&](ac::TableMatchOp operation) { match = operation; });
  model->walk([&](ac::TableChooseOp operation) { choose = operation; });
  ASSERT_TRUE(rule);
  ASSERT_TRUE(match);
  ASSERT_TRUE(choose);

  llvm::SmallVector<StateSnapshotFootprint> snapshots =
      analysis.stateSnapshots(rule.getOperation());
  ASSERT_EQ(3u, snapshots.size());
  EXPECT_EQ("entries", snapshots[0].resource);
  EXPECT_EQ("all", snapshots[0].indexKind);
  EXPECT_FALSE(snapshots[0].source);
  EXPECT_EQ((std::vector<std::string>{"$entry"}), snapshots[0].fields);
  EXPECT_EQ("ready", snapshots[1].resource);
  EXPECT_EQ("set", snapshots[1].indexKind);
  EXPECT_EQ(match.getMask(), snapshots[1].source);
  EXPECT_EQ((std::vector<std::string>{"$entry"}), snapshots[1].fields);
  EXPECT_EQ("priority", snapshots[2].resource);
  EXPECT_EQ("set", snapshots[2].indexKind);
  EXPECT_EQ(choose.getIndex(), snapshots[2].source);
  EXPECT_EQ((std::vector<std::string>{"$entry"}), snapshots[2].fields);
}

} // namespace
} // namespace acir
