#include "acir/CodeGen/QueueGraphPlan.h"
#include "acir/CodeGen/QueueGraphGenerator.h"
#include "acir/CodeGen/QueueGraphPyc.h"
#include "acir/Transforms/Passes.h"

#include "acir/Dialect/ACIR/ACIRDialect.h"
#include "mlir/Dialect/DLTI/DLTI.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <array>
#include <optional>
#include <system_error>

namespace acir::codegen {
namespace {

bool freezeQueueGraph(mlir::ModuleOp module) {
  mlir::PassManager manager(module.getContext());
  manager.addPass(acir::createFreezeTopologyPass());
  return mlir::succeeded(manager.run(module));
}

void expectCppCompiles(llvm::StringRef source) {
  llvm::SmallString<256> directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("acir-queue-graph", directory));
  struct Cleanup {
    llvm::SmallString<256> path;
    ~Cleanup() { llvm::sys::fs::remove_directories(path); }
  } cleanup{directory};

  llvm::SmallString<256> input(directory);
  llvm::sys::path::append(input, "model.cpp");
  std::error_code error;
  llvm::raw_fd_ostream output(input, error);
  ASSERT_FALSE(error);
  output << source;
  output.close();

  llvm::SmallString<256> log(directory);
  llvm::sys::path::append(log, "compile.log");
  const std::array<std::string, 5> ownedArguments = {
      ACIR_TEST_CXX_COMPILER,
      "-std=c++20",
      "-I" ACIR_TEST_SOURCE_DIR "/simulator/gfsim/include",
      "-fsyntax-only",
      input.str().str(),
  };
  llvm::SmallVector<llvm::StringRef> arguments;
  for (const std::string &argument : ownedArguments)
    arguments.push_back(argument);
  const std::array<std::optional<llvm::StringRef>, 3> redirects = {
      std::nullopt, log.str(), log.str()};
  const int status = llvm::sys::ExecuteAndWait(
      ACIR_TEST_CXX_COMPILER, arguments, std::nullopt, redirects);
  auto logBuffer = llvm::MemoryBuffer::getFile(log);
  ASSERT_EQ(status, 0) << (logBuffer ? logBuffer.get()->getBuffer().str()
                                     : std::string{});
}

void expectCppRuns(llvm::StringRef source) {
  llvm::SmallString<256> directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("acir-queue-module", directory));
  struct Cleanup {
    llvm::SmallString<256> path;
    ~Cleanup() { llvm::sys::fs::remove_directories(path); }
  } cleanup{directory};

  llvm::SmallString<256> input(directory);
  llvm::sys::path::append(input, "model.cpp");
  std::error_code error;
  llvm::raw_fd_ostream output(input, error);
  ASSERT_FALSE(error);
  output << source;
  output.close();

  llvm::SmallString<256> executable(directory);
  llvm::sys::path::append(executable, "model");
  llvm::SmallString<256> log(directory);
  llvm::sys::path::append(log, "run.log");
  const std::array<std::string, 6> ownedArguments = {
      ACIR_TEST_CXX_COMPILER,
      "-std=c++20",
      "-I" ACIR_TEST_SOURCE_DIR "/simulator/gfsim/include",
      input.str().str(),
      "-o",
      executable.str().str(),
  };
  llvm::SmallVector<llvm::StringRef> arguments;
  for (const std::string &argument : ownedArguments)
    arguments.push_back(argument);
  const std::array<std::optional<llvm::StringRef>, 3> redirects = {
      std::nullopt, log.str(), log.str()};
  int status = llvm::sys::ExecuteAndWait(ACIR_TEST_CXX_COMPILER, arguments,
                                         std::nullopt, redirects);
  auto logBuffer = llvm::MemoryBuffer::getFile(log);
  ASSERT_EQ(status, 0) << (logBuffer ? logBuffer.get()->getBuffer().str()
                                     : std::string{});
  const std::array<llvm::StringRef, 1> runArguments = {executable.str()};
  status = llvm::sys::ExecuteAndWait(executable, runArguments, std::nullopt,
                                     redirects);
  logBuffer = llvm::MemoryBuffer::getFile(log);
  EXPECT_EQ(status, 0) << (logBuffer ? logBuffer.get()->getBuffer().str()
                                     : std::string{});
}

constexpr llvm::StringLiteral kQueueGraph = R"mlir(
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "pipeline"} {
  %input = ac.source depth 4 latency 1 {ac.name = "input"} : !ac.queue<i64>
  %left, %right = ac.route %input depths [2, 2] latencies [1, 1] {
  ^selector(%item: !ac.var<i64>):
    %zero = ac.var.constant 0 : i64 as !ac.var<i64>
    %selected = ac.var.cmp "eq" %item, %zero : !ac.var<i64> -> !ac.var<i1>
    ac.route.yield %selected : !ac.var<i1>
  } {ac.output_names = ["left", "right"]} : !ac.queue<i64> -> (!ac.queue<i64>, !ac.queue<i64>)
  %merged = ac.merge %left, %right policy "round_robin" depth 3 latency 1 {ac.name = "merged"} : (!ac.queue<i64>, !ac.queue<i64>) -> !ac.queue<i64>
  ac.sink %merged {ac.name = "sink_0"} : !ac.queue<i64>
}
)mlir";

constexpr llvm::StringLiteral kStructuredTransform = R"mlir(
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "structured"} {
  ac.type_scope @types {
    ac.struct @Item fields [{name = "value", type = i64}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Item> = {abi_alignment = 8 : i64, endianness = "little", preferred_alignment = 8 : i64, size = 8 : i64}>}
  %input = ac.source depth 2 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Item>>
  %output = ac.transform %input depths [2] latencies [1] {
  ^body(%item: !ac.var<!ac.struct<@types::@Item>>):
    %value = ac.var.get %item field "value" : !ac.var<!ac.struct<@types::@Item>> -> !ac.var<i64>
    %one = ac.var.constant 1 : i64 as !ac.var<i64>
    %sum = ac.var.add %value, %one : !ac.var<i64>
    %updated = ac.var.with %item, %sum field "value" : !ac.var<!ac.struct<@types::@Item>>, !ac.var<i64> -> !ac.var<!ac.struct<@types::@Item>>
    ac.transform.yield %updated : !ac.var<!ac.struct<@types::@Item>>
  } {ac.name = "output"} : (!ac.queue<!ac.struct<@types::@Item>>) -> !ac.queue<!ac.struct<@types::@Item>>
  ac.sink %output {ac.name = "sink_0"} : !ac.queue<!ac.struct<@types::@Item>>
}
)mlir";

constexpr llvm::StringLiteral kMultipleConsumers = R"mlir(
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad"} {
  %input = ac.source depth 2 latency 1 {ac.name = "input"} : !ac.queue<i64>
  ac.sink %input {ac.name = "left"} : !ac.queue<i64>
  ac.sink %input {ac.name = "right"} : !ac.queue<i64>
}
)mlir";

constexpr llvm::StringLiteral kObservationUse = R"mlir(
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "observed"} {
  %input = ac.source depth 2 latency 1 {ac.name = "input"} : !ac.queue<i64>
  ac.observe %input name "head" : !ac.queue<i64>
  ac.sink %input {ac.name = "sink_0"} : !ac.queue<i64>
}
)mlir";

constexpr llvm::StringLiteral kStatefulFiring = R"mlir(
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "stateful"} {
  ac.table @table entry i8 entries 2 init 0 owner "/" stable_id "table/table"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1]
      stable_id "install" domain "cycle" guard "true" checks []
      handshake "ready_valid_1x1_table" schedule "table_lexical_priority"
      effects ["input.consume", "output.produce", "table.replace:table"] {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant 1 : i2 as !ac.var<i2>
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.table.propose @table [%index] = %item when %enabled : !ac.var<i1>
        mode "replace"
        write_fields ["$entry"] : !ac.var<i2>, !ac.var<i8>
    ac.firing.output %item when %enabled ordinal 0 : !ac.var<i8>, !ac.var<i1>
    ac.firing.yield %item : !ac.var<i8>
  } {ac.name = "output", ac.rule_definition = "install", ac.rule_footprints = [{access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @table}], ac.rule_priority = 0 : i64} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output {ac.name = "sink"} : !ac.queue<i8>
}
)mlir";

QueueGraphPlan sharedReferencePlan() {
  QueueGraphPlan plan;
  plan.system = "shared_reference";
  plan.queues = {{"input", "i8", "/", 1, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
  QueueBlockPlan slot{"slot", "release", "/", {"input"}, {}};
  slot.slot = "pending";
  slot.expressions = {
      {"m", "table_match_ref", "i4", {}, "match", "", "", "issue"},
      {"i",
       "table_selection_index_ref",
       "i2",
       {},
       "selection",
       "",
       "",
       "issue"},
      {"v",
       "table_selection_valid_ref",
       "i1",
       {},
       "selection",
       "",
       "",
       "issue"},
  };
  plan.blocks.push_back(std::move(slot));
  plan.tables = {{"issue", "i8", 4, 0, "table-id", "/"}};
  plan.tableMatches = {{"match", "issue", "/", "i4", {}, "predicate"}};
  plan.tableSelections = {
      {"selection", "issue", "/", "match", "first", "i2", {}, ""}};
  plan.tableReads = {{"issue", "read", "/", "", "unused", 1, 1}};
  plan.slots = {{"pending", "i8", "input", "/", "slot-id", "/"}};
  return plan;
}

QueueGraphPlan aggregateMetadataPlan() {
  QueueGraphPlan plan = sharedReferencePlan();
  plan.payloads = {{"Packet",
                    {{"pair", "tuple<i3, i5>", 8},
                     {"lanes", "!ac.value_array<4 x i4>", 16}}}};
  plan.aggregates = {
      {"tuple<i3, i5>", "tuple", {"i3", "i5"}, 2, 8},
      {"!ac.value_array<4 x i4>", "array", {"i4"}, 4, 16},
  };
  return plan;
}

QueueGraphPlan aggregateExpressionPlan() {
  QueueGraphPlan plan;
  plan.system = "aggregate_expression";
  plan.aggregates = {
      {"tuple<i3, i5>", "tuple", {"i3", "i5"}, 2, 8},
  };
  plan.queues = {{"input", "tuple<i3, i5>", "/", 1, 1},
                 {"output", "tuple<i3, i5>", "/", 1, 1}};

  QueueBlockPlan source;
  source.kind = "source";
  source.name = "input";
  source.scope = "/";
  source.outputs = {"input"};
  source.depths = {1};
  source.latencies = {1};
  plan.blocks.push_back(std::move(source));

  QueueBlockPlan transform;
  transform.kind = "transform";
  transform.name = "output";
  transform.scope = "/";
  transform.inputs = {"input"};
  transform.outputs = {"output"};
  transform.depths = {1};
  transform.latencies = {1};
  QueueExpressionPlan first;
  first.result = "first";
  first.kind = "aggregate_get";
  first.type = "i3";
  first.operands = {"item"};
  first.lsb = 5;
  first.width = 3;
  transform.expressions.push_back(std::move(first));
  QueueExpressionPlan second;
  second.result = "second";
  second.kind = "aggregate_get";
  second.type = "i5";
  second.operands = {"item"};
  second.lsb = 0;
  second.width = 5;
  transform.expressions.push_back(std::move(second));
  QueueExpressionPlan packed;
  packed.result = "packed";
  packed.kind = "tuple_create";
  packed.type = "tuple<i3, i5>";
  packed.operands = {"first", "second"};
  packed.width = 8;
  transform.expressions.push_back(std::move(packed));
  transform.yields = {"packed"};
  plan.blocks.push_back(std::move(transform));

  QueueBlockPlan sink;
  sink.kind = "sink";
  sink.name = "sink";
  sink.scope = "/";
  sink.inputs = {"output"};
  plan.blocks.push_back(std::move(sink));
  return plan;
}

TEST(QueueGraphPlanTest, ExtractsFrozenQueueIdentitiesAndTopology) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(kQueueGraph, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  EXPECT_EQ(plan->system, "pipeline");
  ASSERT_EQ(plan->queues.size(), 4u);
  EXPECT_EQ(plan->queues[0].name, "input");
  EXPECT_EQ(plan->queues[1].name, "left");
  EXPECT_EQ(plan->queues[2].name, "right");
  EXPECT_EQ(plan->queues[3].name, "merged");
  ASSERT_EQ(plan->blocks.size(), 4u);
  EXPECT_EQ(plan->blocks[0].kind, "source");
  EXPECT_EQ(plan->blocks[1].kind, "route");
  EXPECT_EQ(plan->blocks[2].kind, "merge");
  EXPECT_EQ(plan->blocks[3].kind, "sink");
  EXPECT_EQ(plan->blocks[2].policy, "round_robin");
}

TEST(QueueGraphPlanTest, RejectsRawUnfrozenQueueGraph) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(kQueueGraph, &context);
  ASSERT_TRUE(module);
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_FALSE(bool(plan));
  EXPECT_NE(llvm::toString(plan.takeError())
                .find("QueueGraph requires verified epoch 0.5 topology freeze"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, CanonicalJsonIsByteIdenticalAndClosed) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(kQueueGraph, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  auto first = plan->canonicalJson();
  ASSERT_TRUE(bool(first)) << llvm::toString(first.takeError());
  auto second = plan->canonicalJson();
  ASSERT_TRUE(bool(second)) << llvm::toString(second.takeError());
  EXPECT_EQ(*first, *second);
  EXPECT_NE(first->find("\"contract_epoch\":\"0.5\""), std::string::npos);
  EXPECT_NE(first->find("\"schema\":\"agentic-circuit-queue-graph-plan\""),
            std::string::npos);
  EXPECT_NE(first->find("\"version\":\"0.5\""), std::string::npos);
  EXPECT_NE(first->find("\"name\":\"merged\""), std::string::npos);
}

TEST(QueueGraphPlanTest, PreservesJitSpecializationIdentity) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  constexpr llvm::StringLiteral fingerprint =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  std::string specialized = kQueueGraph.str();
  size_t system = specialized.find("ac.system = \"pipeline\"");
  ASSERT_NE(system, std::string::npos);
  specialized.insert(system,
                     ("ac.specialization = \"" + fingerprint + "\", ").str());
  auto module = mlir::parseSourceString<mlir::ModuleOp>(specialized, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  EXPECT_EQ(plan->specializationFingerprint, fingerprint);
  auto json = plan->canonicalJson();
  ASSERT_TRUE(bool(json)) << llvm::toString(json.takeError());
  EXPECT_NE(json->find(fingerprint), std::string::npos);
  auto cpp = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find(("// Specialization: " + fingerprint).str()),
            std::string::npos);

  specialized.replace(specialized.find(fingerprint), fingerprint.size(),
                      "sha256:bad");
  module = mlir::parseSourceString<mlir::ModuleOp>(specialized, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  plan = buildQueueGraphPlan(*module);
  ASSERT_FALSE(bool(plan));
  EXPECT_NE(llvm::toString(plan.takeError()).find("fingerprint is invalid"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsRecursiveNestedPayloadDefinitions) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kStructuredTransform, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  plan->payloads = {
      QueuePayloadPlan{"Left", {{"right", "!ac.struct<@types::@Right>"}}},
      QueuePayloadPlan{"Right", {{"left", "!ac.struct<@types::@Left>"}}},
  };

  auto error = verifyQueueGraphPlan(*plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error)).find("contain a cycle"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsMalformedNominalEnumMetadata) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kStructuredTransform, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  plan->enums = {QueueEnumPlan{"Mode", {"IDLE", "RUN", "WAIT"}, 3}};

  auto widthError = verifyQueueGraphPlan(*plan);
  ASSERT_TRUE(bool(widthError));
  EXPECT_NE(llvm::toString(std::move(widthError)).find("width is inconsistent"),
            std::string::npos);

  plan->enums.front().width = 2;
  plan->enums.front().enumerants = {"IDLE", "IDLE"};
  auto memberError = verifyQueueGraphPlan(*plan);
  ASSERT_TRUE(bool(memberError));
  EXPECT_NE(llvm::toString(std::move(memberError)).find("must be non-empty"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsTupleAggregateWidthMismatch) {
  QueueGraphPlan plan = aggregateMetadataPlan();
  plan.aggregates[0].width = 9;

  auto error = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error)).find("tuple aggregate width"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsMalformedValueArrayMetadata) {
  QueueGraphPlan malformedElements = aggregateMetadataPlan();
  malformedElements.aggregates[1].elements.push_back("i4");
  auto elementError = verifyQueueGraphPlan(malformedElements);
  ASSERT_TRUE(bool(elementError));
  EXPECT_NE(
      llvm::toString(std::move(elementError)).find("value-array metadata"),
      std::string::npos);

  QueueGraphPlan malformedLength = aggregateMetadataPlan();
  malformedLength.aggregates[1].length = 0;
  auto lengthError = verifyQueueGraphPlan(malformedLength);
  ASSERT_TRUE(bool(lengthError));
  EXPECT_NE(llvm::toString(std::move(lengthError)).find("value-array metadata"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsAggregateMetadataWiderThanSixtyFourBits) {
  QueueGraphPlan plan = aggregateMetadataPlan();
  plan.aggregates[0].width = 65;

  auto error = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error)).find("aggregate type metadata"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsAggregateBitWidthArithmeticOverflow) {
  QueueGraphPlan plan = aggregateMetadataPlan();
  plan.payloads[0].fields[1].type = "!ac.value_array<huge x i8>";
  plan.payloads[0].fields[1].width = 8;
  plan.aggregates[1] = {"!ac.value_array<huge x i8>",
                        "array",
                        {"i8"},
                        (uint64_t{1} << 61) + 1,
                        8};

  auto error = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error)).find("overflows uint64_t"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsPayloadFieldWidthMismatch) {
  QueueGraphPlan plan = aggregateMetadataPlan();
  plan.payloads[0].fields[0].width = 7;

  auto error = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error)).find("payload field width"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsAggregateCreateOperandTypeOrArityForgery) {
  QueueGraphPlan swapped = aggregateExpressionPlan();
  swapped.blocks[1].expressions[2].operands = {"second", "first"};
  auto typeError = verifyQueueGraphPlan(swapped);
  ASSERT_TRUE(bool(typeError));
  EXPECT_NE(llvm::toString(std::move(typeError)).find("operand type"),
            std::string::npos);

  QueueGraphPlan shortTuple = aggregateExpressionPlan();
  shortTuple.blocks[1].expressions[2].operands.pop_back();
  auto arityError = verifyQueueGraphPlan(shortTuple);
  ASSERT_TRUE(bool(arityError));
  EXPECT_NE(llvm::toString(std::move(arityError)).find("operand arity"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsAggregateGetCrossElementSliceForgery) {
  QueueGraphPlan plan = aggregateExpressionPlan();
  plan.blocks[1].expressions[0].lsb = 4;

  auto error = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error)).find("exact declared element"),
            std::string::npos);
}

TEST(QueueGraphPlanTest,
     PreservesExactU64MaskedMatchAndRejectsForgedMetadata) {
  QueueGraphPlan plan;
  plan.system = "masked_match";
  plan.queues = {{"input", "i64", "/", 1, 1},
                 {"output", "i1", "/", 1, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
  QueueBlockPlan transform{"transform", "output", "/", {"input"},
                           {"output"},  {1},      {1}};
  QueueExpressionPlan match;
  match.result = "matched";
  match.kind = "masked_match";
  match.type = "i1";
  match.operands = {"item"};
  match.mask = "0x8000000000000001";
  match.value = "0x8000000000000001";
  transform.expressions.push_back(match);
  transform.yields = {"matched"};
  plan.blocks.push_back(std::move(transform));
  plan.blocks.push_back({"sink", "sink", "/", {"output"}, {}});

  auto error = verifyQueueGraphPlan(plan);
  ASSERT_FALSE(bool(error)) << llvm::toString(std::move(error));
  auto json = plan.canonicalJson();
  ASSERT_TRUE(bool(json)) << llvm::toString(json.takeError());
  EXPECT_NE(json->find("\"mask\":\"0x8000000000000001\""),
            std::string::npos);
  EXPECT_NE(json->find("\"value\":\"0x8000000000000001\""),
            std::string::npos);
  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  expectCppCompiles(*cpp);
  std::string executable = *cpp;
  executable.append(R"cpp(
int main() {
  ac_generated::block_0_policy match;
  return match(gfsim::UInt<64>{0x8000000000000001ULL}) == 1 &&
                 match(gfsim::UInt<64>{0x8000000000000000ULL}) == 0 &&
                 match(gfsim::UInt<64>{0xffffffffffffffffULL}) == 1
             ? 0
             : 1;
}
)cpp");
  expectCppRuns(executable);
  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_NE(pyc->find("pyc.and"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.eq"), std::string::npos);

  for (const std::pair<std::string, std::string> &forgery : {
           std::pair<std::string, std::string>{"9223372036854775809",
                                               match.value},
           {"0x08000000000000001", match.value},
           {"0x8000000000000000", "0x0000000000000001"},
       }) {
    QueueGraphPlan malformed = plan;
    malformed.blocks[1].expressions[0].mask = forgery.first;
    malformed.blocks[1].expressions[0].value = forgery.second;
    auto malformedError = verifyQueueGraphPlan(malformed);
    ASSERT_TRUE(bool(malformedError));
    EXPECT_NE(llvm::toString(std::move(malformedError))
                  .find("masked_match mask/value metadata"),
              std::string::npos);
  }
}

TEST(QueueGraphPlanTest,
     PreservesReusableModuleSpecializationAndRunsIndependentInstances) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(
      ACIR_TEST_SOURCE_DIR
      "/tests/mlir/agentic-circuit/Transforms/queue-module-freeze.mlir",
      &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  EXPECT_EQ(plan->definition, "Top");
  ASSERT_EQ(plan->moduleSpecializations.size(), 1u);
  ASSERT_EQ(plan->moduleInstances.size(), 2u);
  EXPECT_EQ(plan->moduleSpecializations.front()->definition, "Increment");
  EXPECT_EQ(plan->moduleInstances[0].specializationFingerprint,
            plan->moduleInstances[1].specializationFingerprint);
  EXPECT_EQ(plan->moduleInstances[0].specializationFingerprint,
            plan->moduleSpecializations.front()->specializationFingerprint);
  EXPECT_EQ(plan->moduleInstances[0].lexicalOrder, 2u);
  EXPECT_EQ(plan->moduleInstances[1].lexicalOrder, 3u);
  QueueGraphPlan malformedOrder = *plan;
  malformedOrder.moduleInstances[1].lexicalOrder =
      malformedOrder.moduleInstances[0].lexicalOrder;
  auto orderError = verifyQueueGraphPlan(malformedOrder);
  ASSERT_TRUE(bool(orderError));
  EXPECT_NE(llvm::toString(std::move(orderError))
                .find("lexical orders must be unique"),
            std::string::npos);
  auto pyc = generateQueueGraphPyc(*plan);
  ASSERT_FALSE(bool(pyc));
  EXPECT_NE(llvm::toString(pyc.takeError())
                .find("module-preserving QueueGraph PYC lowering is not "
                      "implemented"),
            std::string::npos);

  auto generated = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(generated)) << llvm::toString(generated.takeError());
  llvm::StringRef source(*generated);
  size_t classBegin = source.find("class Increment_");
  ASSERT_NE(classBegin, llvm::StringRef::npos);
  size_t classEnd = source.find(" final", classBegin);
  ASSERT_NE(classEnd, llvm::StringRef::npos);
  llvm::StringRef implementation =
      source.slice(classBegin + std::string("class ").size(), classEnd);
  EXPECT_EQ(source.count(("class " + implementation + " final").str()), 1u);
  EXPECT_EQ(source.count(("  " + implementation + " instance_").str()), 2u);
  const size_t dispatch = source.find("dispatch_rows()");
  ASSERT_NE(dispatch, llvm::StringRef::npos);
  const size_t broadcastRow =
      source.find("makeDispatchRow(&block_0_)", dispatch);
  const size_t leftRow = source.find("instance_0_.dispatch_row(0)", dispatch);
  const size_t rightRow = source.find("instance_1_.dispatch_row(0)", dispatch);
  const size_t firstSinkRow =
      source.find("makeDispatchRow(&block_1_)", dispatch);
  EXPECT_LT(broadcastRow, leftRow);
  EXPECT_LT(leftRow, rightRow);
  EXPECT_LT(rightRow, firstSinkRow);

  std::string executableSource = *generated;
  executableSource.append(R"cpp(
int main() {
  ac_generated::ReusedPipeline model;
  if (!model.input().proposePush(gfsim::UInt<8>{5}))
    return 1;
  model.input().doXfer({0, 0});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick != 8; ++tick) {
    const gfsim::Epoch epoch{tick, 0};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }
  const auto &left = model.sink_0_values();
  const auto &right = model.sink_1_values();
  return left.size() == 1 && right.size() == 1 && left[0] == 6 &&
                 right[0] == 6
             ? 0
             : 2;
}
)cpp");
  expectCppRuns(executableSource);
}

TEST(QueueGraphPlanTest,
     ReusesStatefulModuleImplementationWithIndependentPersistentState) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(
      ACIR_TEST_SOURCE_DIR "/tests/mlir/agentic-circuit/Transforms/"
                           "queue-stateful-module-freeze.mlir",
      &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_EQ(plan->moduleSpecializations.size(), 1u);
  ASSERT_EQ(plan->moduleInstances.size(), 2u);
  const QueueGraphPlan &specialization = *plan->moduleSpecializations.front();
  ASSERT_EQ(specialization.blocks.size(), 1u);
  ASSERT_EQ(specialization.tables.size(), 1u);
  EXPECT_FALSE(plan->activationEdges.empty());
  EXPECT_FALSE(plan->workClosureEdges.empty());
  EXPECT_FALSE(specialization.activationEdges.empty());
  EXPECT_FALSE(specialization.workClosureEdges.empty());
  EXPECT_EQ(specialization.blocks.front().kind, "firing");
  EXPECT_EQ(specialization.tables.front().name, "sum");

  auto generated = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(generated)) << llvm::toString(generated.takeError());
  llvm::StringRef source(*generated);
  size_t classBegin = source.find("class Accumulator_");
  ASSERT_NE(classBegin, llvm::StringRef::npos);
  size_t classEnd = source.find(" final", classBegin);
  ASSERT_NE(classEnd, llvm::StringRef::npos);
  llvm::StringRef implementation =
      source.slice(classBegin + std::string("class ").size(), classEnd);
  EXPECT_EQ(source.count(("class " + implementation + " final").str()), 1u);
  EXPECT_EQ(source.count(("  " + implementation + " instance_").str()), 2u);
  EXPECT_EQ(source.count("gfsim::SimTable<gfsim::UInt<8>> table_0_;"), 1u);
  EXPECT_NE(source.find("activation_offsets()"), llvm::StringRef::npos);
  EXPECT_NE(source.find("activation_complete() { return true; }"),
            llvm::StringRef::npos);
  EXPECT_NE(source.find("activation_targets()"), llvm::StringRef::npos);
  EXPECT_NE(source.find("initial_work_ids()"), llvm::StringRef::npos);
  EXPECT_NE(source.find("work_closure_offsets()"), llvm::StringRef::npos);
  EXPECT_NE(source.find("work_closure_targets()"), llvm::StringRef::npos);
  EXPECT_NE(source.find("offer_left_input"), llvm::StringRef::npos);
  EXPECT_NE(source.find("schedule_initial_work"), llvm::StringRef::npos);

  QueueGraphPlan forgedActivation = *plan;
  forgedActivation.activationEdges.pop_back();
  auto activationError = verifyQueueGraphPlan(forgedActivation);
  ASSERT_TRUE(bool(activationError));
  EXPECT_NE(llvm::toString(std::move(activationError))
                .find("activation, Work closure, and initial frontier"),
            std::string::npos);
  QueueGraphPlan forgedClosure = *plan;
  forgedClosure.workClosureEdges.pop_back();
  auto closureError = verifyQueueGraphPlan(forgedClosure);
  ASSERT_TRUE(bool(closureError));
  EXPECT_NE(llvm::toString(std::move(closureError))
                .find("activation, Work closure, and initial frontier"),
            std::string::npos);

  std::string executableSource = *generated;
  executableSource.append(R"cpp(
namespace {
void runTicks(ac_generated::StatefulReuse &model, unsigned first,
              unsigned limit) {
  auto rows = model.dispatch_rows();
  for (unsigned tick = first; tick != limit; ++tick) {
    const gfsim::Epoch epoch{tick, 0};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }
}
} // namespace

int main() {
  ac_generated::StatefulReuse model;
  if (!model.left_input().proposePush(gfsim::UInt<8>{1}) ||
      !model.right_input().proposePush(gfsim::UInt<8>{10}))
    return 1;
  model.left_input().doXfer({0, 0});
  model.right_input().doXfer({0, 0});
  runTicks(model, 1, 8);
  if (!model.left_input().proposePush(gfsim::UInt<8>{2}))
    return 2;
  model.left_input().doXfer({8, 0});
  runTicks(model, 9, 16);
  const auto &left = model.sink_0_values();
  const auto &right = model.sink_1_values();
  return left.size() == 2 && left[0] == 1 && left[1] == 3 &&
                 right.size() == 1 && right[0] == 10
             ? 0
             : 3;
}
)cpp");
  expectCppRuns(executableSource);
}

TEST(QueueGraphPlanTest,
     ReusesMultiRuleModuleAndPreservesOwnerLocalLexicalArbitration) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(
      ACIR_TEST_SOURCE_DIR "/tests/mlir/agentic-circuit/Transforms/"
                           "queue-multi-rule-module-freeze.mlir",
      &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_EQ(plan->moduleSpecializations.size(), 1u);
  ASSERT_EQ(plan->moduleInstances.size(), 2u);
  const QueueGraphPlan &specialization = *plan->moduleSpecializations.front();
  ASSERT_EQ(specialization.interfaceInputs.size(), 2u);
  ASSERT_EQ(specialization.interfaceOutputs.size(), 2u);
  ASSERT_EQ(specialization.blocks.size(), 2u);
  EXPECT_EQ(specialization.blocks[0].priority, 0u);
  EXPECT_EQ(specialization.blocks[1].priority, 1u);

  auto generated = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(generated)) << llvm::toString(generated.takeError());
  llvm::StringRef source(*generated);
  size_t classBegin = source.find("class DualAccumulator_");
  ASSERT_NE(classBegin, llvm::StringRef::npos);
  size_t classEnd = source.find(" final", classBegin);
  ASSERT_NE(classEnd, llvm::StringRef::npos);
  llvm::StringRef implementation =
      source.slice(classBegin + std::string("class ").size(), classEnd);
  EXPECT_EQ(source.count(("class " + implementation + " final").str()), 1u);
  EXPECT_EQ(source.count(("  " + implementation + " instance_").str()), 2u);
  EXPECT_NE(source.find((implementation + "_block_0_policy").str()),
            llvm::StringRef::npos);
  EXPECT_NE(source.find((implementation + "_block_1_policy").str()),
            llvm::StringRef::npos);

  std::string executableSource = *generated;
  executableSource.append(R"cpp(
int main() {
  ac_generated::MultiRuleReuse model;
  if (!model.left_a().proposePush(gfsim::UInt<8>{1}) ||
      !model.left_b().proposePush(gfsim::UInt<8>{2}) ||
      !model.right_b().proposePush(gfsim::UInt<8>{10}))
    return 1;
  model.left_a().doXfer({0, 0});
  model.left_b().doXfer({0, 0});
  model.right_b().doXfer({0, 0});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick != 12; ++tick) {
    const gfsim::Epoch epoch{tick, 0};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }
  const auto &leftA = model.sink_0_values();
  const auto &leftB = model.sink_1_values();
  const auto &rightA = model.sink_2_values();
  const auto &rightB = model.sink_3_values();
  return leftA.size() == 1 && leftA[0] == 1 && leftB.size() == 1 &&
                 leftB[0] == 3 && rightA.empty() && rightB.size() == 1 &&
                 rightB[0] == 10
             ? 0
             : 2;
}
)cpp");
  expectCppRuns(executableSource);
}

TEST(QueueGraphPlanTest,
     ReusesMultiOwnerModuleWithAtomicIndependentInstanceState) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(
      ACIR_TEST_SOURCE_DIR "/tests/mlir/agentic-circuit/Transforms/"
                           "queue-multi-owner-module-freeze.mlir",
      &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_EQ(plan->moduleSpecializations.size(), 1u);
  ASSERT_EQ(plan->moduleInstances.size(), 2u);
  const QueueGraphPlan &specialization = *plan->moduleSpecializations.front();
  ASSERT_EQ(specialization.blocks.size(), 1u);
  ASSERT_EQ(specialization.tables.size(), 2u);
  ASSERT_EQ(specialization.blocks.front().stateWrites.size(), 2u);
  EXPECT_EQ(specialization.blocks.front().stateWrites[0].table, "cursor");
  EXPECT_EQ(specialization.blocks.front().stateWrites[1].table, "total");

  auto generated = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(generated)) << llvm::toString(generated.takeError());
  llvm::StringRef source(*generated);
  size_t classBegin = source.find("class StatePair_");
  ASSERT_NE(classBegin, llvm::StringRef::npos);
  size_t classEnd = source.find(" final", classBegin);
  ASSERT_NE(classEnd, llvm::StringRef::npos);
  llvm::StringRef implementation =
      source.slice(classBegin + std::string("class ").size(), classEnd);
  EXPECT_EQ(source.count(("class " + implementation + " final").str()), 1u);
  EXPECT_EQ(source.count(("  " + implementation + " instance_").str()), 2u);
  EXPECT_NE(source.find("gfsim::QueueStateTransition<"), llvm::StringRef::npos);

  std::string executableSource = *generated;
  executableSource.append(R"cpp(
namespace {
void runTicks(ac_generated::MultiOwnerReuse &model, unsigned first,
              unsigned limit) {
  auto rows = model.dispatch_rows();
  for (unsigned tick = first; tick != limit; ++tick) {
    const gfsim::Epoch epoch{tick, 0};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }
}
} // namespace

int main() {
  ac_generated::MultiOwnerReuse model;
  if (!model.left_input().proposePush(gfsim::UInt<8>{3}) ||
      !model.right_input().proposePush(gfsim::UInt<8>{10}))
    return 1;
  model.left_input().doXfer({0, 0});
  model.right_input().doXfer({0, 0});
  runTicks(model, 1, 8);
  if (!model.left_input().proposePush(gfsim::UInt<8>{5}))
    return 2;
  model.left_input().doXfer({8, 0});
  runTicks(model, 9, 16);
  const auto &left = model.sink_0_values();
  const auto &right = model.sink_1_values();
  return left.size() == 2 && left[0] == 4 && left[1] == 10 &&
                 right.size() == 1 && right[0] == 11
             ? 0
             : 3;
}
)cpp");
  expectCppRuns(executableSource);
}

TEST(QueueGraphPlanTest,
     ReusesCombinedMultiRuleMultiOwnerModuleWithAtomicArbitration) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(
      ACIR_TEST_SOURCE_DIR "/tests/mlir/agentic-circuit/Transforms/"
                           "queue-multi-rule-multi-owner-module.mlir",
      &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_EQ(plan->moduleSpecializations.size(), 1u);
  ASSERT_EQ(plan->moduleInstances.size(), 2u);
  const QueueGraphPlan &specialization = *plan->moduleSpecializations.front();
  ASSERT_EQ(specialization.blocks.size(), 2u);
  ASSERT_EQ(specialization.tables.size(), 2u);
  ASSERT_EQ(specialization.blocks[0].stateWrites.size(), 2u);
  ASSERT_EQ(specialization.blocks[1].stateWrites.size(), 2u);
  EXPECT_EQ(specialization.blocks[0].priority, 0u);
  EXPECT_EQ(specialization.blocks[1].priority, 1u);

  auto generated = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(generated)) << llvm::toString(generated.takeError());
  llvm::StringRef source(*generated);
  size_t classBegin = source.find("class DualState_");
  ASSERT_NE(classBegin, llvm::StringRef::npos);
  size_t classEnd = source.find(" final", classBegin);
  ASSERT_NE(classEnd, llvm::StringRef::npos);
  llvm::StringRef implementation =
      source.slice(classBegin + std::string("class ").size(), classEnd);
  EXPECT_EQ(source.count(("class " + implementation + " final").str()), 1u);
  EXPECT_EQ(source.count(("  " + implementation + " instance_").str()), 2u);
  EXPECT_EQ(source.count("gfsim::QueueStateTransition<"), 2u);

  std::string executableSource = *generated;
  executableSource.append(R"cpp(
int main() {
  ac_generated::CombinedReuse model;
  if (!model.left_a().proposePush(gfsim::UInt<8>{1}) ||
      !model.left_b().proposePush(gfsim::UInt<8>{2}) ||
      !model.right_b().proposePush(gfsim::UInt<8>{10}))
    return 1;
  model.left_a().doXfer({0, 0});
  model.left_b().doXfer({0, 0});
  model.right_b().doXfer({0, 0});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick != 12; ++tick) {
    const gfsim::Epoch epoch{tick, 0};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }
  const auto &leftA = model.sink_0_values();
  const auto &leftB = model.sink_1_values();
  const auto &rightA = model.sink_2_values();
  const auto &rightB = model.sink_3_values();
  return leftA.size() == 1 && leftA[0] == 2 && leftB.size() == 1 &&
                 leftB[0] == 5 && rightA.empty() && rightB.size() == 1 &&
                 rightB[0] == 11
             ? 0
             : 2;
}
)cpp");
  expectCppRuns(executableSource);
}

TEST(QueueGraphPlanTest,
     PreservesNestedSpecializationReuseWithoutFlatteningChildBodies) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(
      ACIR_TEST_SOURCE_DIR "/tests/mlir/agentic-circuit/Transforms/"
                           "queue-nested-module-freeze.mlir",
      &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_EQ(plan->moduleSpecializations.size(), 1u);
  ASSERT_EQ(plan->moduleInstances.size(), 2u);
  const QueueGraphPlan &wrapper = *plan->moduleSpecializations.front();
  EXPECT_EQ(wrapper.definition, "Wrapper");
  ASSERT_EQ(wrapper.moduleSpecializations.size(), 1u);
  ASSERT_EQ(wrapper.moduleInstances.size(), 1u);
  EXPECT_EQ(wrapper.moduleSpecializations.front()->definition, "Increment");
  EXPECT_EQ(wrapper.moduleInstances.front().specializationFingerprint,
            wrapper.moduleSpecializations.front()->specializationFingerprint);

  auto generated = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(generated)) << llvm::toString(generated.takeError());
  llvm::StringRef source(*generated);
  EXPECT_EQ(source.count("class Increment_"), 1u);
  EXPECT_EQ(source.count("class Wrapper_"), 1u);
  EXPECT_EQ(source.count(" child_0_;"), 1u);

  std::string executableSource = *generated;
  executableSource.append(R"cpp(
int main() {
  ac_generated::NestedReuse model;
  if (!model.left_input().proposePush(gfsim::UInt<8>{5}) ||
      !model.right_input().proposePush(gfsim::UInt<8>{10}))
    return 1;
  model.left_input().doXfer({0, 0});
  model.right_input().doXfer({0, 0});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick != 8; ++tick) {
    const gfsim::Epoch epoch{tick, 0};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }
  const auto &left = model.sink_0_values();
  const auto &right = model.sink_1_values();
  return left.size() == 1 && left[0] == 6 && right.size() == 1 &&
                 right[0] == 11
             ? 0
             : 2;
}
)cpp");
  expectCppRuns(executableSource);
}

TEST(QueueGraphPlanTest,
     OwnsInternalQueuesPerMixedNestedSpecializationInstance) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(
      ACIR_TEST_SOURCE_DIR "/tests/mlir/agentic-circuit/Transforms/"
                           "queue-mixed-nested-module.mlir",
      &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_EQ(plan->moduleSpecializations.size(), 1u);
  const QueueGraphPlan &parent = *plan->moduleSpecializations.front();
  EXPECT_EQ(parent.definition, "PrepareAndIncrement");
  ASSERT_EQ(parent.blocks.size(), 1u);
  ASSERT_EQ(parent.moduleInstances.size(), 1u);
  ASSERT_EQ(parent.moduleSpecializations.size(), 1u);
  ASSERT_EQ(parent.queues.size(), 2u);
  EXPECT_EQ(parent.blocks.front().outputs.front(), "prepared");
  EXPECT_EQ(parent.moduleInstances.front().inputs.front(), "prepared");

  auto generated = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(generated)) << llvm::toString(generated.takeError());
  llvm::StringRef source(*generated);
  EXPECT_EQ(source.count("class Increment_"), 1u);
  EXPECT_EQ(source.count("class PrepareAndIncrement_"), 1u);
  EXPECT_EQ(source.count("gfsim::SimQueue<gfsim::UInt<8>> queue_0_;"), 1u);
  EXPECT_NE(source.find("activation_complete() { return true; }"),
            llvm::StringRef::npos);

  std::string executableSource = *generated;
  executableSource.append(R"cpp(
int main() {
  ac_generated::MixedNestedReuse model;
  if (!model.left_input().proposePush(gfsim::UInt<8>{5}) ||
      !model.right_input().proposePush(gfsim::UInt<8>{10}))
    return 1;
  model.left_input().doXfer({0, 0});
  model.right_input().doXfer({0, 0});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick != 10; ++tick) {
    const gfsim::Epoch epoch{tick, 0};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }
  const auto &left = model.sink_0_values();
  const auto &right = model.sink_1_values();
  return left.size() == 1 && left[0] == 7 && right.size() == 1 &&
                 right[0] == 12
             ? 0
             : 2;
}
)cpp");
  expectCppRuns(executableSource);
}

TEST(QueueGraphPlanTest, PreservesQueueRateAndRejectsUnspecializedPycLanes) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  std::string rated = kQueueGraph.str();
  size_t attributes = rated.find("{ac.name = \"input\"}");
  ASSERT_NE(attributes, std::string::npos);
  rated.replace(attributes, std::string("{ac.name = \"input\"}").size(),
                "{ac.name = \"input\", "
                "ac.output_rates = array<i64: 2>}");
  auto module = mlir::parseSourceString<mlir::ModuleOp>(rated, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_FALSE(plan->queues.empty());
  EXPECT_EQ(plan->queues.front().rate, 2u);
  auto json = plan->canonicalJson();
  ASSERT_TRUE(bool(json)) << llvm::toString(json.takeError());
  EXPECT_NE(json->find("\"rate\":2"), std::string::npos);
  auto cpp = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find(", nullptr, 1, 2)"), std::string::npos);
  auto pyc = generateQueueGraphPyc(*plan);
  ASSERT_FALSE(bool(pyc));
  EXPECT_NE(llvm::toString(pyc.takeError())
                .find("rate greater than one requires explicit lane lowering"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsLegacyContractEpochBeforePlanning) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  std::string legacy = kQueueGraph.str();
  size_t epoch = legacy.find("ac.contract_epoch = \"0.5\"");
  ASSERT_NE(epoch, std::string::npos);
  legacy.replace(epoch, std::string("ac.contract_epoch = \"0.5\"").size(),
                 "ac.contract_epoch = \"0.4\"");
  auto module = mlir::parseSourceString<mlir::ModuleOp>(legacy, &context);
  ASSERT_TRUE(module);
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_FALSE(bool(plan));
  EXPECT_NE(llvm::toString(plan.takeError())
                .find("module requires ac.contract_epoch exactly '0.5'"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, ExtractsPayloadAndImmutableVarDag) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kStructuredTransform, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_EQ(plan->payloads.size(), 1u);
  EXPECT_EQ(plan->payloads[0].name, "Item");
  ASSERT_EQ(plan->payloads[0].fields.size(), 1u);
  EXPECT_EQ(plan->payloads[0].fields[0].name, "value");
  ASSERT_EQ(plan->blocks.size(), 3u);
  const QueueBlockPlan &transform = plan->blocks[1];
  ASSERT_EQ(transform.expressions.size(), 4u);
  EXPECT_EQ(transform.expressions[0].kind, "get");
  EXPECT_EQ(transform.expressions[1].kind, "constant");
  EXPECT_EQ(transform.expressions[2].kind, "add");
  EXPECT_EQ(transform.expressions[3].kind, "with");
  ASSERT_EQ(transform.yields.size(), 1u);
  EXPECT_EQ(transform.yields[0], "v3");
}

TEST(QueueGraphPlanTest, NativeGeneratorConsumesOnlyExtractedPlan) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(kQueueGraph, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  auto source = generateQueueGraphCpp(*plan);
  ASSERT_TRUE(bool(source)) << llvm::toString(source.takeError());
  EXPECT_NE(source->find("gfsim::QueueRoute<gfsim::UInt<64>, 2"),
            std::string::npos);
  EXPECT_NE(source->find("gfsim::QueueMerge<gfsim::UInt<64>, 2>"),
            std::string::npos);
  EXPECT_NE(source->find("gfsim::QueueSink<gfsim::UInt<64>>"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, EmitsCanonicalScalarQueuePyc) {
  QueueGraphPlan plan;
  plan.system = "scalar_pipeline";
  plan.queues = {{"input", "i64", "/", 2, 1}, {"output", "i64", "/", 2, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {2}, {1}});
  QueueBlockPlan transform{"transform", "output", "/", {"input"},
                           {"output"},  {2},      {1}};
  transform.expressions = {{"v0", "constant", "i64", {}, "", "", "1 : i64"},
                           {"v1", "add", "i64", {"item", "v0"}, "", "", ""}};
  transform.yields = {"v1"};
  plan.blocks.push_back(std::move(transform));
  plan.blocks.push_back({"sink", "sink_0", "/", {"output"}, {}});
  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_EQ(std::count(pyc->begin(), pyc->end(), '\n') > 5, true);
  EXPECT_NE(pyc->find("pyc.fifo"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.add"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.frontend.contract = \"pycircuit\""),
            std::string::npos);
}

TEST(QueueGraphPlanTest, EmitsAtomicTransformWithIndependentArity) {
  QueueGraphPlan plan;
  plan.system = "atomic_sum";
  plan.queues = {{"left", "i64", "/", 2, 1},
                 {"right", "i64", "/", 2, 1},
                 {"sum", "i64", "/", 2, 1}};
  plan.blocks.push_back({"source", "left", "/", {}, {"left"}, {2}, {1}});
  plan.blocks.push_back({"source", "right", "/", {}, {"right"}, {2}, {1}});
  QueueBlockPlan transform{"transform", "sum", "/", {"left", "right"},
                           {"sum"},     {2},   {1}};
  transform.expressions = {{"v0", "add", "i64", {"item", "item1"}, "", "", ""}};
  transform.yields = {"v0"};
  plan.blocks.push_back(std::move(transform));
  plan.blocks.push_back({"sink", "sink_0", "/", {"sum"}, {}});

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find("std::tuple<gfsim::UInt<64>> operator()(const "
                      "gfsim::UInt<64> &item, const gfsim::UInt<64> &item1)"),
            std::string::npos);
  EXPECT_NE(cpp->find("QueueAtomicTransform<block_0_policy, "
                      "std::tuple<gfsim::UInt<64>, gfsim::UInt<64>>, "
                      "std::tuple<gfsim::UInt<64>>>"),
            std::string::npos);

  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_NE(pyc->find("pyc.add"), std::string::npos);
  EXPECT_NE(pyc->find("= pyc.wire : i1"), std::string::npos);
}

TEST(QueueGraphPlanTest, EmitsHeterogeneousBarrierForBothBackends) {
  QueueGraphPlan plan;
  plan.system = "barrier";
  plan.queues = {{"left", "i8", "/", 2, 1},
                 {"right", "i16", "/", 2, 1},
                 {"left_ready", "i8", "/", 2, 1},
                 {"right_ready", "i16", "/", 2, 1}};
  plan.blocks.push_back({"source", "left", "/", {}, {"left"}, {2}, {1}});
  plan.blocks.push_back({"source", "right", "/", {}, {"right"}, {2}, {1}});
  plan.blocks.push_back({"barrier",
                         "left_ready",
                         "/",
                         {"left", "right"},
                         {"left_ready", "right_ready"},
                         {2, 2},
                         {1, 1}});
  plan.blocks.push_back({"sink", "sink_0", "/", {"left_ready"}, {}});
  plan.blocks.push_back({"sink", "sink_1", "/", {"right_ready"}, {}});

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find("gfsim::QueueBarrier<std::tuple<gfsim::UInt<8>, "
                      "gfsim::UInt<16>>>"),
            std::string::npos);

  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_NE(pyc->find("%in0_valid"), std::string::npos);
  EXPECT_NE(pyc->find("%out1_ready"), std::string::npos);
}

TEST(QueueGraphPlanTest, EmitsStaticQueueCollectionSelectForBothBackends) {
  QueueGraphPlan plan;
  plan.system = "select";
  plan.queues = {{"control", "i8", "/", 1, 1},
                 {"left", "i16", "/", 1, 1},
                 {"right", "i16", "/", 1, 1},
                 {"selected", "i16", "/", 2, 1}};
  plan.blocks.push_back({"source", "control", "/", {}, {"control"}, {1}, {1}});
  plan.blocks.push_back({"source", "left", "/", {}, {"left"}, {1}, {1}});
  plan.blocks.push_back({"source", "right", "/", {}, {"right"}, {1}, {1}});
  QueueBlockPlan select{
      "select",     "selected", "/", {"control", "left", "right"},
      {"selected"}, {2},        {1}};
  select.yields = {"item"};
  plan.blocks.push_back(std::move(select));
  plan.blocks.push_back({"sink", "sink_0", "/", {"selected"}, {}});

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find("gfsim::QueueSelect<gfsim::UInt<8>, gfsim::UInt<16>, 2"),
            std::string::npos);

  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_NE(pyc->find("select_selector_out_of_range"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.mux"), std::string::npos);
}

TEST(QueueGraphPlanTest, EmitsTypedReorderForBothBackends) {
  QueueGraphPlan plan;
  plan.system = "ordered";
  plan.queues = {{"input", "i64", "/", 4, 1}, {"output", "i64", "/", 4, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {4}, {1}});
  QueueBlockPlan reorder{"reorder",  "output", "/", {"input"},
                         {"output"}, {4},      {1}};
  reorder.yields = {"item"};
  reorder.capacity = 4;
  reorder.start = 0;
  plan.blocks.push_back(std::move(reorder));
  plan.blocks.push_back({"sink", "sink_0", "/", {"output"}, {}});

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find("gfsim::QueueReorder<gfsim::UInt<64>, block_0_policy>"),
            std::string::npos);
  EXPECT_NE(cpp->find(", input_, output_, 4, 0)"), std::string::npos);
  EXPECT_NE(cpp->find("size_t reorder_0_active() const"), std::string::npos);

  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_NE(pyc->find("pyc.reg"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.ult"), std::string::npos);
  EXPECT_GE(std::count(pyc->begin(), pyc->end(), '\n'), 40);
}

TEST(QueueGraphPlanTest, EmitsTypedDependencyForBothBackends) {
  QueueGraphPlan plan;
  plan.system = "dependent";
  plan.queues = {{"input", "i8", "/", 4, 1}, {"output", "i8", "/", 4, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {4}, {1}});
  QueueBlockPlan dependency{"dependency", "output", "/", {"input"},
                            {"output"},   {4},      {1}};
  dependency.expressions = {
      {"v0", "constant", "i8", {}, "", "", "255 : i8"},
      {"v1", "constant", "i1", {}, "", "", "0 : i1"},
      {"v2", "constant", "i8", {}, "", "", "1 : i8"},
  };
  dependency.yields = {"item", "v0", "v1", "v2"};
  dependency.capacity = 4;
  dependency.resources = 2;
  dependency.noDependency = 255;
  plan.blocks.push_back(std::move(dependency));
  plan.blocks.push_back({"sink", "sink_0", "/", {"output"}, {}});

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find("gfsim::QueueDependency<gfsim::UInt<8>"),
            std::string::npos);
  EXPECT_NE(cpp->find(", input_, output_, 4, 2, 255)"), std::string::npos);
  EXPECT_NE(cpp->find("size_t dependency_0_active() const"), std::string::npos);
  EXPECT_NE(cpp->find("dependency_0_resource_active"), std::string::npos);

  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_NE(pyc->find("pyc.reg"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.sub"), std::string::npos);
}

TEST(QueueGraphPlanTest, EmitsTypedCreditWindowForBothBackends) {
  QueueGraphPlan plan;
  plan.system = "credited";
  plan.queues = {{"input", "i8", "/", 4, 1}, {"output", "i8", "/", 4, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {4}, {1}});
  QueueBlockPlan credit{"credit",   "output", "/", {"input"},
                        {"output"}, {4},      {1}};
  credit.yields = {"item"};
  credit.credits = 2;
  plan.blocks.push_back(std::move(credit));
  plan.blocks.push_back({"sink", "sink_0", "/", {"output"}, {}});

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find("gfsim::QueueCredit<gfsim::UInt<8>, block_0_policy>"),
            std::string::npos);
  EXPECT_NE(cpp->find(", input_, output_, 2)"), std::string::npos);

  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_NE(pyc->find("pyc.reg"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.sub"), std::string::npos);
}

TEST(QueueGraphPlanTest, EmitsOldDataMemoryForBothBackends) {
  QueueGraphPlan plan;
  plan.system = "memory_pipeline";
  plan.payloads = {{"MemoryRequest",
                    {{"address", "i4", 4},
                     {"write", "i1", 1},
                     {"data", "i16", 16},
                     {"tag", "i8", 8}}}};
  constexpr llvm::StringLiteral requestType =
      "!ac.struct<@types::@MemoryRequest>";
  plan.queues = {{"input0", requestType.str(), "/", 4, 1},
                 {"input1", requestType.str(), "/", 4, 1},
                 {"output0", requestType.str(), "/", 4, 1},
                 {"output1", requestType.str(), "/", 4, 1}};
  plan.blocks.push_back({"source", "input0", "/", {}, {"input0"}, {4}, {1}});
  plan.blocks.push_back({"source", "input1", "/", {}, {"input1"}, {4}, {1}});
  plan.memoryInstances.push_back({"sram", "i16", 15, 0, 3, "memory/sram", "/"});
  QueueBlockPlan memory{"memory_request", "output0", "/", {"input0"},
                        {"output0"},      {4},       {1}};
  memory.expressions = {
      {"v0", "get", "i4", {"item"}, "address", "", ""},
      {"v1", "get", "i1", {"item"}, "write", "", ""},
      {"v2", "get", "i16", {"item"}, "data", "", ""},
  };
  memory.yields = {"v0", "v1", "v2"};
  memory.resultField = "data";
  memory.memoryInstance = "sram";
  memory.endpointOrdinal = 0;
  plan.memoryRequests.push_back(
      {"sram", "output0", "/", "input0", "output0", 0, 4, "data"});
  plan.blocks.push_back(memory);
  memory.name = "output1";
  memory.inputs = {"input1"};
  memory.outputs = {"output1"};
  memory.endpointOrdinal = 1;
  plan.memoryRequests.push_back(
      {"sram", "output1", "/", "input1", "output1", 1, 4, "data"});
  plan.blocks.push_back(std::move(memory));
  plan.blocks.push_back({"sink", "sink_0", "/", {"output0"}, {}});
  plan.blocks.push_back({"sink", "sink_1", "/", {"output1"}, {}});

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(
      cpp->find("gfsim::QueueMemoryArbiter<MemoryRequest, gfsim::UInt<16>"),
      std::string::npos);
  EXPECT_NE(cpp->find("result.data = old_data"), std::string::npos);
  EXPECT_NE(cpp->find("std::array<gfsim::SimQueue<MemoryRequest> *, "
                      "2>{&input0_, &input1_}"),
            std::string::npos);
  expectCppCompiles(*cpp);

  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  EXPECT_NE(pyc->find("pyc.sub"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.sync_mem"), std::string::npos);
  EXPECT_EQ(pyc->find("pyc.sync_mem", pyc->find("pyc.sync_mem") + 1),
            std::string::npos);
  EXPECT_NE(pyc->find("{depth = 15, name = \"sram\"}"), std::string::npos);
  EXPECT_NE(pyc->find("pyc.concat"), std::string::npos);
  EXPECT_NE(pyc->find("memory_address_out_of_range"), std::string::npos);
}

TEST(QueueGraphPlanTest, NativeTableKeyConvertsExactWidthValue) {
  QueueGraphPlan plan = sharedReferencePlan();
  plan.blocks.clear();
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
  plan.queues.push_back({"output", "i13", "/", 1, 1});
  QueueBlockPlan read{"table_read", "read", "/", {"input"},
                      {"output"},   {1},    {1}};
  read.table = "issue";
  read.expressions = {
      {"address", "constant", "i2", {}, "", "", "0 : i2"},
      {"enabled", "constant", "i1", {}, "", "", "true"},
  };
  read.yields = {"address", "enabled"};
  plan.blocks.push_back(std::move(read));
  plan.blocks.push_back({"sink", "sink_0", "/", {"output"}, {}});
  plan.slots.clear();
  plan.tableReads = {{"issue", "read", "/", "input", "output", 1, 1}};
  plan.tables[0].entryType = "i13";
  plan.tableMatches = {{"match",
                        "issue",
                        "/",
                        "i4",
                        {{"v0", "constant", "i1", {}, "", "", "true"}},
                        "v0"}};
  plan.tableSelections[0].policy = "min";
  plan.tableSelections[0].keyYield = "item";

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find("return static_cast<std::uint64_t>([&]()"),
            std::string::npos);
  expectCppCompiles(*cpp);
}

TEST(QueueGraphPlanTest, EmitsQueuePredicateAsPycComparison) {
  struct Case {
    llvm::StringLiteral predicate;
    llvm::StringLiteral opcode;
    bool negated;
  };
  constexpr Case cases[] = {
      {"eq", "pyc.eq", false},   {"ne", "pyc.eq", true},
      {"slt", "pyc.slt", false}, {"sle", "pyc.slt", true},
      {"sgt", "pyc.slt", false}, {"sge", "pyc.slt", true},
  };
  for (const Case &testCase : cases) {
    SCOPED_TRACE(testCase.predicate.str());
    mlir::MLIRContext context;
    context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
    std::string source = kQueueGraph.str();
    const std::string original = "ac.var.cmp \"eq\"";
    size_t predicate = source.find(original);
    ASSERT_NE(predicate, std::string::npos);
    source.replace(predicate, original.size(),
                   "ac.var.cmp \"" + testCase.predicate.str() + "\"");
    auto module = mlir::parseSourceString<mlir::ModuleOp>(source, &context);
    ASSERT_TRUE(module);
    ASSERT_TRUE(freezeQueueGraph(*module));
    auto plan = buildQueueGraphPlan(*module);
    ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
    auto pyc = generateQueueGraphPyc(*plan);
    ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
    const size_t comparison = pyc->find(testCase.opcode.str());
    ASSERT_NE(comparison, std::string::npos);
    EXPECT_EQ(pyc->find("pyc.rr_arbiter"), std::string::npos);
    EXPECT_NE(pyc->find("primitive_id = \"control.rr_arbiter.v1\""),
              std::string::npos);
    const size_t comparisonEnd = pyc->find('\n', comparison);
    ASSERT_NE(comparisonEnd, std::string::npos);
    const size_t nextEnd = pyc->find('\n', comparisonEnd + 1);
    ASSERT_NE(nextEnd, std::string::npos);
    const llvm::StringRef nextLine(pyc->data() + comparisonEnd + 1,
                                   nextEnd - comparisonEnd - 1);
    EXPECT_EQ(nextLine.contains("pyc.not"), testCase.negated);
  }
}

TEST(QueueGraphPlanTest, EmitsOneReadyValidStagePerQueueLatency) {
  QueueGraphPlan plan;
  plan.system = "latency_pipeline";
  plan.queues = {{"input", "i64", "/", 2, 1}, {"output", "i64", "/", 4, 3}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {2}, {1}});
  QueueBlockPlan transform{"transform", "output", "/", {"input"},
                           {"output"},  {4},      {3}};
  transform.yields = {"item"};
  plan.blocks.push_back(std::move(transform));
  plan.blocks.push_back({"sink", "sink_0", "/", {"output"}, {}});
  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_TRUE(bool(pyc)) << llvm::toString(pyc.takeError());
  size_t count = 0;
  for (size_t offset = 0;
       (offset = pyc->find("pyc.fifo", offset)) != std::string::npos;
       offset += 8)
    ++count;
  EXPECT_EQ(count, 4u);
  EXPECT_NE(pyc->find("{depth = 4}"), std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsImplicitMultipleConsumers) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kMultipleConsumers, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_FALSE(bool(plan));
  EXPECT_NE(llvm::toString(plan.takeError()).find("insert ac.broadcast"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsOutOfRangeConstantTableFiringPlan) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kStatefulFiring, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  auto firing = llvm::find_if(plan->blocks, [](const QueueBlockPlan &block) {
    return block.kind == "firing";
  });
  ASSERT_NE(firing, plan->blocks.end());
  auto constant = llvm::find_if(firing->expressions,
                                [](const QueueExpressionPlan &expression) {
                                  return expression.kind == "constant";
                                });
  ASSERT_NE(constant, firing->expressions.end());
  constant->literal = "3 : i2";

  auto error = verifyQueueGraphPlan(*plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error)).find("statically safe"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RecomputesBoundedFiringIndexConstraints) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kStatefulFiring, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto extracted = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(extracted)) << llvm::toString(extracted.takeError());

  auto configure = [](QueueGraphPlan plan, llvm::StringRef payload,
                      llvm::StringRef index) {
    for (QueuePlan &queue : plan.queues)
      queue.payloadType = payload.str();
    plan.tables.front().entryType = payload.str();
    plan.tables.front().entries = 5;
    QueueBlockPlan &firing = *llvm::find_if(
        plan.blocks,
        [](const QueueBlockPlan &block) { return block.kind == "firing"; });
    firing.stateWrites.front().index = index.str();
    return plan;
  };
  auto firing = [](QueueGraphPlan &plan) -> QueueBlockPlan & {
    return *llvm::find_if(
        plan.blocks,
        [](const QueueBlockPlan &block) { return block.kind == "firing"; });
  };
  auto expectAccepted = [&](QueueGraphPlan plan) {
    auto error = verifyQueueGraphPlan(plan);
    ASSERT_FALSE(bool(error)) << llvm::toString(std::move(error));
  };
  auto expectRejected = [&](QueueGraphPlan plan) {
    auto error = verifyQueueGraphPlan(plan);
    ASSERT_TRUE(bool(error));
    EXPECT_NE(llvm::toString(std::move(error)).find("statically safe"),
              std::string::npos);
  };

  // A u2 root domain is [0,3], which is a safe subset of a five-entry Table.
  QueueGraphPlan u2 = configure(*extracted, "i2", "item");
  expectAccepted(u2);
  auto cpp = generateQueueGraphCpp(u2);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  expectCppCompiles(*cpp);

  // A raw u3 root is [0,7], so the same access must fail closed.
  expectRejected(configure(*extracted, "i3", "item"));

  QueueGraphPlan arithmetic = configure(*extracted, "i3", "sum");
  firing(arithmetic).expressions.push_back(
      {"two_a", "constant", "i3", {}, "", "", "2 : i3"});
  firing(arithmetic).expressions.push_back(
      {"two_b", "constant", "i3", {}, "", "", "2 : i3"});
  firing(arithmetic).expressions.push_back(
      {"sum", "add", "i3", {"two_a", "two_b"}});
  expectAccepted(arithmetic);

  QueueGraphPlan masked = configure(*extracted, "i3", "masked");
  firing(masked).expressions.push_back(
      {"mask", "constant", "i3", {}, "", "", "-4 : i3"});
  firing(masked).expressions.push_back(
      {"masked", "and", "i3", {"item", "mask"}});
  expectAccepted(masked);

  QueueGraphPlan selected = configure(*extracted, "i3", "selected");
  firing(selected).expressions.push_back(
      {"one", "constant", "i3", {}, "", "", "1 : i3"});
  firing(selected).expressions.push_back(
      {"four", "constant", "i3", {}, "", "", "-4 : i3"});
  firing(selected).expressions.push_back(
      {"less", "cmp", "i1", {"item", "four"}, "", "ult"});
  firing(selected).expressions.push_back(
      {"selected", "value_select", "i3", {"less", "one", "four"}});
  expectAccepted(selected);

  QueueGraphPlan priority = configure(*extracted, "i5", "priority");
  firing(priority).expressions.push_back(
      {"priority", "priority_index", "i3", {"item"}, "", "low"});
  expectAccepted(priority);
}

TEST(QueueGraphPlanTest, RecomputesTableEndpointAndObservationConstraints) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module = mlir::parseSourceFile<mlir::ModuleOp>(
      ACIR_TEST_SOURCE_DIR
      "/tests/mlir/agentic-circuit/CodeGen/table-endpoint-value-constraints.mlir",
      &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto extracted = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(extracted)) << llvm::toString(extracted.takeError());
  auto baseError = verifyQueueGraphPlan(*extracted);
  ASSERT_FALSE(bool(baseError)) << llvm::toString(std::move(baseError));

  auto widenInput = [](QueueGraphPlan &plan, llvm::StringRef name) {
    auto queue = llvm::find_if(plan.queues, [&](const QueuePlan &candidate) {
      return candidate.name == name;
    });
    ASSERT_NE(queue, plan.queues.end());
    queue->payloadType = "i3";
  };
  auto expectRejected = [&](QueueGraphPlan plan, llvm::StringRef queue,
                            llvm::StringRef diagnostic) {
    widenInput(plan, queue);
    auto error = verifyQueueGraphPlan(plan);
    ASSERT_TRUE(bool(error));
    EXPECT_NE(llvm::toString(std::move(error)).find(diagnostic),
              std::string::npos);
  };

  expectRejected(*extracted, "read_input",
                 "Table endpoint address is not statically safe");
  expectRejected(*extracted, "write_input",
                 "Table endpoint address is not statically safe");
  expectRejected(*extracted, "get_input",
                 "Table observation index is not statically safe");
}

TEST(QueueGraphPlanTest, RejectsTableFiringPlanTypeAndOwnershipBypasses) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kStatefulFiring, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());

  QueueGraphPlan mismatched = *plan;
  auto output = llvm::find_if(mismatched.queues, [](const QueuePlan &queue) {
    return queue.name == "output";
  });
  ASSERT_NE(output, mismatched.queues.end());
  output->payloadType = "i16";
  auto typeError = verifyQueueGraphPlan(mismatched);
  ASSERT_TRUE(bool(typeError));
  EXPECT_NE(llvm::toString(std::move(typeError)).find("must match"),
            std::string::npos);

  QueueGraphPlan conflicting = *plan;
  conflicting.tableWrites.push_back(
      {"table", "extra", "/", "", "field", {"$entry"}});
  auto ownershipError = verifyQueueGraphPlan(conflicting);
  ASSERT_TRUE(bool(ownershipError));
  EXPECT_NE(
      llvm::toString(std::move(ownershipError)).find("state firing write"),
      std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsForgedFrozenFiringBeforePlanExtraction) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kStatefulFiring, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  ac::FiringOp firing;
  module->walk([&](ac::FiringOp candidate) { firing = candidate; });
  ASSERT_TRUE(firing);
  firing.setHandshakeAttr(mlir::StringAttr::get(&context, "ready_valid_1x1"));

  auto plan = buildQueueGraphPlan(*module);
  ASSERT_FALSE(bool(plan));
  EXPECT_NE(
      llvm::toString(plan.takeError()).find("failed operation verification"),
      std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsInvalidSharedTableReferenceTargets) {
  for (size_t index = 0; index < 3; ++index) {
    QueueGraphPlan plan = sharedReferencePlan();
    plan.blocks[1].expressions[index].field = "missing";
    auto error = verifyQueueGraphPlan(plan);
    ASSERT_TRUE(bool(error));
    EXPECT_NE(llvm::toString(std::move(error)).find("unknown"),
              std::string::npos);
  }
}

TEST(QueueGraphPlanTest, RejectsInvalidSharedTableReferenceProvenance) {
  for (size_t index = 0; index < 3; ++index) {
    QueueGraphPlan plan = sharedReferencePlan();
    plan.blocks[1].expressions[index].table = "other";
    auto error = verifyQueueGraphPlan(plan);
    ASSERT_TRUE(bool(error));
    EXPECT_NE(llvm::toString(std::move(error)).find("provenance"),
              std::string::npos);
  }
}

TEST(QueueGraphPlanTest, RejectsInvalidSharedTableReferenceFieldTypes) {
  for (size_t index = 0; index < 3; ++index) {
    QueueGraphPlan plan = sharedReferencePlan();
    plan.blocks[1].expressions[index].type = "i8";
    auto error = verifyQueueGraphPlan(plan);
    ASSERT_TRUE(bool(error));
    EXPECT_NE(llvm::toString(std::move(error)).find("field type"),
              std::string::npos);
  }
}

TEST(QueueGraphPlanTest, RejectsInvalidSharedTableWidths) {
  QueueGraphPlan plan = sharedReferencePlan();
  plan.tableMatches[0].resultType = "i3";
  auto matchError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(matchError));
  EXPECT_NE(llvm::toString(std::move(matchError)).find("table match metadata"),
            std::string::npos);

  plan = sharedReferencePlan();
  plan.tableSelections[0].indexType = "i3";
  auto selectionError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(selectionError));
  EXPECT_NE(llvm::toString(std::move(selectionError))
                .find("table selection metadata"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsMalformedPriorityExpressionPlan) {
  auto makePlan = [] {
    QueueGraphPlan plan;
    plan.system = "priority";
    plan.queues = {{"input", "i3", "/", 1, 1}, {"output", "i3", "/", 1, 1}};
    plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
    QueueBlockPlan transform{"transform", "output", "/", {"input"}, {"output"}};
    transform.expressions = {
        {"v0", "priority_index", "i2", {"item"}, "", "low", ""},
        {"v1", "priority_valid", "i1", {"item"}, "", "low", ""},
    };
    transform.yields = {"item"};
    plan.blocks.push_back(std::move(transform));
    plan.blocks.push_back({"sink", "sink", "/", {"output"}, {}});
    return plan;
  };

  QueueGraphPlan plan = makePlan();
  EXPECT_FALSE(bool(verifyQueueGraphPlan(plan)));

  plan = makePlan();
  plan.blocks[1].expressions[0].predicate = "middle";
  auto predicateError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(predicateError));
  EXPECT_NE(
      llvm::toString(std::move(predicateError)).find("priority expression"),
      std::string::npos);

  plan = makePlan();
  plan.blocks[1].expressions[0].operands.push_back("item");
  auto arityError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(arityError));
  EXPECT_NE(llvm::toString(std::move(arityError)).find("priority expression"),
            std::string::npos);

  plan = makePlan();
  plan.blocks[1].expressions[0].type = "i1";
  auto typeError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(typeError));
  EXPECT_NE(llvm::toString(std::move(typeError)).find("result type"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsMalformedPopcountExpressionPlan) {
  auto makePlan = [] {
    QueueGraphPlan plan;
    plan.system = "popcount";
    plan.queues = {{"input", "i13", "/", 1, 1}, {"output", "i13", "/", 1, 1}};
    plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
    QueueBlockPlan transform{"transform", "output", "/", {"input"}, {"output"}};
    transform.expressions = {{"v0", "popcount", "i4", {"item"}}};
    transform.yields = {"item"};
    plan.blocks.push_back(std::move(transform));
    plan.blocks.push_back({"sink", "sink", "/", {"output"}, {}});
    return plan;
  };

  QueueGraphPlan plan = makePlan();
  EXPECT_FALSE(bool(verifyQueueGraphPlan(plan)));

  plan = makePlan();
  plan.blocks[1].expressions[0].operands.push_back("item");
  auto arityError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(arityError));
  EXPECT_NE(llvm::toString(std::move(arityError)).find("popcount expression"),
            std::string::npos);

  plan = makePlan();
  plan.blocks[1].expressions[0].type = "i3";
  auto typeError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(typeError));
  EXPECT_NE(llvm::toString(std::move(typeError)).find("result type"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsMalformedCountZerosExpressionPlan) {
  auto makePlan = [] {
    QueueGraphPlan plan;
    plan.system = "count_leading_zeros";
    plan.queues = {{"input", "i13", "/", 1, 1}, {"output", "i13", "/", 1, 1}};
    plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
    QueueBlockPlan transform{"transform", "output", "/", {"input"}, {"output"}};
    transform.expressions = {{"v0", "count_zeros", "i4", {"item"}}};
    transform.expressions[0].predicate = "leading";
    transform.yields = {"item"};
    plan.blocks.push_back(std::move(transform));
    plan.blocks.push_back({"sink", "sink", "/", {"output"}, {}});
    return plan;
  };

  QueueGraphPlan plan = makePlan();
  EXPECT_FALSE(bool(verifyQueueGraphPlan(plan)));

  plan = makePlan();
  plan.blocks[1].expressions[0].operands.push_back("item");
  auto arityError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(arityError));
  EXPECT_NE(
      llvm::toString(std::move(arityError)).find("count_zeros expression"),
      std::string::npos);

  plan = makePlan();
  plan.blocks[1].expressions[0].predicate = "middle";
  auto directionError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(directionError));
  EXPECT_NE(llvm::toString(std::move(directionError)).find("direction"),
            std::string::npos);

  plan = makePlan();
  plan.blocks[1].expressions[0].type = "i3";
  auto typeError = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(typeError));
  EXPECT_NE(llvm::toString(std::move(typeError)).find("result type"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsUnconsumedQueueAsStaticDeadlockRisk) {
  QueueGraphPlan plan;
  plan.system = "unconsumed";
  plan.queues = {{"input", "i8", "/", 1, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
  auto error = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error)).find("has no consuming block"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsRawQueueCycleOutsideFeedbackBlock) {
  QueueGraphPlan plan;
  plan.system = "cycle";
  plan.queues = {{"a", "i8", "/", 1, 1}, {"b", "i8", "/", 1, 1}};
  plan.blocks.push_back({"transform", "a", "/", {"b"}, {"a"}, {1}, {1}});
  plan.blocks.push_back({"transform", "b", "/", {"a"}, {"b"}, {1}, {1}});
  auto error = verifyQueueGraphPlan(plan);
  ASSERT_TRUE(bool(error));
  EXPECT_NE(llvm::toString(std::move(error))
                .find("represent stateful loops with ac.feedback"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, ObservationDoesNotConsumeQueue) {
  mlir::MLIRContext context;
  context.loadDialect<ac::ACIRDialect, mlir::DLTIDialect>();
  auto module =
      mlir::parseSourceString<mlir::ModuleOp>(kObservationUse, &context);
  ASSERT_TRUE(module);
  ASSERT_TRUE(freezeQueueGraph(*module));
  auto plan = buildQueueGraphPlan(*module);
  ASSERT_TRUE(bool(plan)) << llvm::toString(plan.takeError());
  ASSERT_EQ(plan->blocks.size(), 3u);
  EXPECT_EQ(plan->blocks[1].kind, "observe");
}

TEST(QueueGraphPlanTest, VerificationLeafRunsInGfsimAndRejectsPycDesign) {
  QueueGraphPlan plan;
  plan.system = "verified";
  plan.queues = {{"input", "i8", "/", 1, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
  QueueBlockPlan expect{"expect", "expect_1", "/", {"input"}, {}};
  expect.expressions = {
      {"v0", "constant", "i8", {}, "", "", "0 : i8"},
      {"v1", "cmp", "i1", {"item", "v0"}, "", "sgt", ""},
  };
  expect.yields = {"v1"};
  expect.message = "positive";
  plan.blocks.push_back(std::move(expect));
  plan.blocks.push_back({"sink", "sink_0", "/", {"input"}, {}});

  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_TRUE(bool(cpp)) << llvm::toString(cpp.takeError());
  EXPECT_NE(cpp->find("gfsim::QueueExpect<gfsim::UInt<8>, block_0_policy>"),
            std::string::npos);

  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_FALSE(bool(pyc));
  EXPECT_NE(llvm::toString(pyc.takeError())
                .find("cannot appear in a design hierarchy"),
            std::string::npos);
}

TEST(QueueGraphPlanTest, RejectsMalformedPycFeedbackContract) {
  QueueGraphPlan plan;
  plan.system = "bad_feedback";
  plan.queues = {{"input", "i64", "/", 1, 1}, {"output", "i64", "/", 1, 1}};
  plan.blocks.push_back({"source", "input", "/", {}, {"input"}, {1}, {1}});
  QueueBlockPlan feedback{"feedback", "output", "/", {"input"}, {"output"}};
  feedback.yields = {"item", "condition"};
  feedback.maxIterations = 0;
  plan.blocks.push_back(std::move(feedback));
  plan.blocks.push_back({"sink", "sink_0", "/", {"output"}, {}});
  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_FALSE(bool(pyc));
  EXPECT_NE(
      llvm::toString(pyc.takeError()).find("feedback contract is unsupported"),
      std::string::npos);
}

TEST(QueueGraphPlanTest, BackendsRejectUncatalogedApplicationBlock) {
  QueueGraphPlan plan;
  plan.system = "bad_dispatch";
  plan.queues = {{"input", "i64", "/", 1, 1}};
  plan.blocks.push_back({"dispatch", "dispatch", "/", {"input"}, {}});
  auto cpp = generateQueueGraphCpp(plan);
  ASSERT_FALSE(bool(cpp));
  EXPECT_NE(llvm::toString(cpp.takeError())
                .find("official opcode has no gfsim lowering: 'dispatch'"),
            std::string::npos);
  auto pyc = generateQueueGraphPyc(plan);
  ASSERT_FALSE(bool(pyc));
  EXPECT_NE(llvm::toString(pyc.takeError())
                .find("official opcode has no PYC lowering: 'dispatch'"),
            std::string::npos);
}

} // namespace
} // namespace acir::codegen
