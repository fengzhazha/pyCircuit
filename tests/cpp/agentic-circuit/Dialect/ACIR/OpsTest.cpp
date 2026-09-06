#include "Dialect/ACIR/ACIROpsTestHooks.h"
#include "Dialect/ACIR/ACIRResourcesTestHooks.h"
#include "acir/Analysis/ModelAnalysis.h"
#include "acir/Dialect/ACIR/ACIRDialect.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "acir/Dialect/ACIR/ACIRResources.h"
#include "acir/Dialect/ACIR/GraphRegion.h"
#include "acir/InitAllDialects.h"
#include "acir/Transforms/Passes.h"

#include "mlir/AsmParser/AsmParser.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/DLTI/DLTI.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <vector>

namespace acir::ac {
namespace {

TEST(ACIROpsTest, PublicBuildersConstructEveryTaskFourOperation) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(module.getBody());

  auto scope = TypeScopeOp::create(builder, loc, "types");
  builder.setInsertionPointToStart(&scope.getBody().emplaceBlock());
  auto names = builder.getStrArrayAttr({"x"});
  auto field = builder.getDictionaryAttr({
      builder.getNamedAttr("name", builder.getStringAttr("x")),
      builder.getNamedAttr("type", mlir::TypeAttr::get(builder.getI8Type())),
  });
  auto fields = builder.getArrayAttr({field});

  EXPECT_TRUE(TypeAliasOp::create(builder, loc, "Byte", builder.getI8Type()));
  EXPECT_TRUE(StructOp::create(builder, loc, "S", fields));
  EXPECT_TRUE(
      EnumOp::create(builder, loc, "E", builder.getStrArrayAttr({"a"})));
  EXPECT_TRUE(UnionOp::create(builder, loc, "U", fields, "x"));
  EXPECT_TRUE(PacketOp::create(builder, loc, "P", fields));
  EXPECT_TRUE(TransactionOp::create(builder, loc, "T", fields));

  auto input = mlir::UnrealizedConversionCastOp::create(
                   builder, loc, mlir::TypeRange{builder.getI8Type()},
                   mlir::ValueRange{})
                   .getResult(0);
  auto structRef = mlir::SymbolRefAttr::get(
      &context, "types", {mlir::FlatSymbolRefAttr::get(&context, "S")});
  auto packetRef = mlir::SymbolRefAttr::get(
      &context, "types", {mlir::FlatSymbolRefAttr::get(&context, "P")});
  auto structType = StructType::get(&context, structRef);
  auto packetType = PacketType::get(&context, packetRef);
  auto bytesType = VectorType::get(&context, 1, builder.getI8Type());
  auto record = RecordCreateOp::create(builder, loc, structType,
                                       mlir::ValueRange{input}, names);
  EXPECT_TRUE(record);
  auto get = RecordGetOp::create(builder, loc, builder.getI8Type(),
                                 record.getResult(), "x");
  auto with = RecordWithOp::create(builder, loc, structType, record.getResult(),
                                   input, "x");
  EXPECT_TRUE(get);
  EXPECT_TRUE(with);
  auto packet =
      mlir::UnrealizedConversionCastOp::create(
          builder, loc, mlir::TypeRange{packetType}, mlir::ValueRange{})
          .getResult(0);
  auto bytes =
      PacketSerializeOp::create(builder, loc, bytesType, packet, packetRef);
  EXPECT_TRUE(bytes);
  auto deserialize = PacketDeserializeOp::create(builder, loc, packetType,
                                                 bytes.getResult(), packetRef);
  EXPECT_TRUE(deserialize);
  EXPECT_TRUE(mlir::isMemoryEffectFree(record.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(get.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(with.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(bytes.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(deserialize.getOperation()));
}

TEST(ACIROpsTest, QueueBoundariesOwnEffectsAndFiringBodyIsPure) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  auto module = mlir::parseSourceString<mlir::ModuleOp>(R"mlir(
    module attributes {ac.contract_epoch = "0.5"} {
      %input = ac.source depth 4 latency 1 : !ac.queue<i32>
      %output = ac.firing %input depths [4] latencies [1]
          stable_id "identity" domain "cycle" guard "true" checks []
          handshake "ready_valid_1x1" schedule "independent"
          effects ["input.consume", "output.produce"] {
      ^body(%value: !ac.var<i32>):
        ac.firing.yield %value : !ac.var<i32>
      } : (!ac.queue<i32>) -> !ac.queue<i32>
      ac.sink %output : !ac.queue<i32>
    }
  )mlir",
                                                        &context);
  ASSERT_TRUE(module);

  auto checkEffect = [&](mlir::Operation *operation, mlir::Value queue,
                         mlir::MemoryEffects::Effect *expected) {
    llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
    mlir::cast<mlir::MemoryEffectOpInterface>(operation).getEffects(effects);
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_EQ(effects.front().getEffect(), expected);
    EXPECT_EQ(effects.front().getValue(), queue);
    EXPECT_EQ(effects.front().getResource(), QueueStateResource::get());
  };

  module->walk([&](FiringOp operation) {
    EXPECT_TRUE(mlir::isMemoryEffectFree(operation));
  });
  module->walk([&](SourceOp operation) {
    checkEffect(operation, operation.getOutput(),
                mlir::MemoryEffects::Write::get());
  });
  module->walk([&](SinkOp operation) {
    checkEffect(operation, operation.getInput(),
                mlir::MemoryEffects::Write::get());
  });
}

TEST(ACIROpsTest, LayoutAndEffectsAreDeclaredThroughMLIRInterfaces) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(module.getBody());
  auto scope = TypeScopeOp::create(builder, loc, "types");
  EXPECT_TRUE(mlir::isa<mlir::DataLayoutOpInterface>(scope.getOperation()));
  mlir::DataLayout layout(scope);
  EXPECT_EQ(layout.getTypeSize(builder.getI32Type()).getFixedValue(), 4u);

  builder.setInsertionPointToStart(&scope.getBody().emplaceBlock());
  auto input = mlir::UnrealizedConversionCastOp::create(
      builder, loc, mlir::TypeRange{builder.getI8Type()}, mlir::ValueRange{});
  auto structRef = mlir::SymbolRefAttr::get(
      &context, "types", {mlir::FlatSymbolRefAttr::get(&context, "S")});
  auto packetRef = mlir::SymbolRefAttr::get(
      &context, "types", {mlir::FlatSymbolRefAttr::get(&context, "P")});
  auto structType = StructType::get(&context, structRef);
  auto record = RecordCreateOp::create(builder, loc, structType,
                                       mlir::ValueRange{input.getResult(0)},
                                       builder.getStrArrayAttr({"x"}));
  auto packetType = PacketType::get(&context, packetRef);
  auto packet = mlir::UnrealizedConversionCastOp::create(
      builder, loc, mlir::TypeRange{packetType}, mlir::ValueRange{});
  auto serialized = PacketSerializeOp::create(
      builder, loc, VectorType::get(&context, 1, builder.getI8Type()),
      packet.getResult(0), packetRef);
  EXPECT_TRUE(mlir::isMemoryEffectFree(record.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(serialized.getOperation()));
}

TEST(ACIROpsTest, ClosedRegistryDoesNotLoadUnrelatedDialects) {
  mlir::DialectRegistry registry;
  registry.insert<ACIRDialect, mlir::DLTIDialect>();
  mlir::MLIRContext context(registry);
  context.loadAllAvailableDialects();
  EXPECT_NE(context.getLoadedDialect("ac"), nullptr);
  EXPECT_NE(context.getLoadedDialect("dlti"), nullptr);
  EXPECT_EQ(context.getLoadedDialect("arith"), nullptr);
  EXPECT_EQ(context.getLoadedDialect("scf"), nullptr);
}

TEST(ACIROpsTest, QualifiedNamedTypesImplementDataLayout) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::Type type = mlir::parseType("!ac.struct<@types::@S>", &context);
  ASSERT_TRUE(type);
  EXPECT_TRUE(mlir::isa<mlir::DataLayoutTypeInterface>(type));
}

TEST(ACIROpsTest, NamedAggregateLayoutUsesExactDLTIEntry) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect, mlir::DLTIDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(module.getBody());
  auto scope = TypeScopeOp::create(builder, loc, "types");
  auto reference = mlir::SymbolRefAttr::get(
      &context, "types", {mlir::FlatSymbolRefAttr::get(&context, "S")});
  auto type = StructType::get(&context, reference);
  auto metadata = builder.getDictionaryAttr({
      builder.getNamedAttr("size", builder.getI64IntegerAttr(12)),
      builder.getNamedAttr("abi_alignment", builder.getI64IntegerAttr(4)),
      builder.getNamedAttr("preferred_alignment", builder.getI64IntegerAttr(8)),
      builder.getNamedAttr("endianness", builder.getStringAttr("big")),
  });
  auto entry = mlir::DataLayoutEntryAttr::get(type, metadata);
  auto spec =
      mlir::DataLayoutSpecAttr::get(&context, mlir::DataLayoutEntryList{entry});
  scope->setAttr(mlir::DLTIDialect::kDataLayoutAttrName, spec);

  mlir::DataLayout layout(scope);
  EXPECT_EQ(layout.getTypeSize(type).getFixedValue(), 12u);
  EXPECT_EQ(layout.getTypeABIAlignment(type), 4u);
  EXPECT_EQ(layout.getTypePreferredAlignment(type), 8u);
  auto queried = spec.query(mlir::DataLayoutEntryKey(type));
  ASSERT_TRUE(mlir::succeeded(queried));
  EXPECT_EQ(mlir::cast<mlir::DictionaryAttr>(*queried)
                .getAs<mlir::StringAttr>("endianness")
                .getValue(),
            "big");
}

TEST(ACIROpsTest, PublicRegistryIncludesOnlyRequiredDLTIDependency) {
  mlir::DialectRegistry registry;
  acir::registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  context.loadAllAvailableDialects();
  EXPECT_NE(context.getLoadedDialect("dlti"), nullptr);
}

TEST(ACIROpsTest, PublicBuildersConstructEveryTaskFiveOperation) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(module.getBody());

  auto protocol = ProtocolOp::create(builder, loc, "p");
  builder.setInsertionPointToStart(&protocol.getBody().emplaceBlock());
  auto roleA = RoleOp::create(builder, loc, "a", "b", "exclusive");
  EXPECT_TRUE(roleA);
  auto roleB = RoleOp::create(builder, loc, "b", "a", "exclusive");
  auto state = StateOp::create(builder, loc, "s", true, false);
  auto event = EventOp::create(builder, loc, "e", "a", "b", builder.getI8Type(),
                               "notify");
  auto transition =
      TransitionOp::create(builder, loc, "s", "s", "e", nullptr, false, false);
  transition.getGuard().emplaceBlock();
  EXPECT_TRUE(transition);
  auto guarantee = GuaranteeOp::create(builder, loc, "ordering",
                                       builder.getStringAttr("fifo"));

  builder.setInsertionPointAfter(protocol);
  auto interface = InterfaceOp::create(builder, loc, "I");
  builder.setInsertionPointToStart(&interface.getBody().emplaceBlock());
  EXPECT_TRUE(RoleOp::create(builder, loc, "a", "b", "exclusive"));
  EXPECT_TRUE(RoleOp::create(builder, loc, "b", "a", "exclusive"));
  auto channel = ChannelType::get(&context, builder.getI8Type(),
                                  mlir::FlatSymbolRefAttr::get(&context, "p"));
  auto port = PortOp::create(builder, loc, "data", channel, "a", "b", "a", "b");
  EXPECT_TRUE(port);
  EXPECT_EQ(port.getProtocolFromAttr().getValue(), "a");
  EXPECT_EQ(port.getProtocolToAttr().getValue(), "b");
  EXPECT_TRUE(mlir::isa<ProtocolContainerOpInterface>(protocol.getOperation()));
  EXPECT_TRUE(
      mlir::isa<InterfaceContainerOpInterface>(interface.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::RegionKindInterface>(protocol.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::RegionKindInterface>(interface.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::SymbolOpInterface>(protocol.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::SymbolOpInterface>(interface.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::SymbolOpInterface>(roleA.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::SymbolOpInterface>(roleB.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::SymbolOpInterface>(state.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::SymbolOpInterface>(event.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::SymbolOpInterface>(port.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(roleA.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(roleB.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(state.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(event.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(guarantee.getOperation()));
  EXPECT_TRUE(mlir::isMemoryEffectFree(port.getOperation()));
  EXPECT_TRUE(mlir::succeeded(mlir::verify(module)));
}

TEST(ACIROpsTest, RegistryContainsExactQueueVarOperations) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  const std::array<llvm::StringLiteral, 8> names = {
      "ac.interface", "ac.protocol",   "ac.role",      "ac.state",
      "ac.event",     "ac.transition", "ac.guarantee", "ac.port",
  };
  for (llvm::StringLiteral name : names)
    EXPECT_TRUE(mlir::OperationName(name, &context).isRegistered())
        << name.str();
  EXPECT_FALSE(mlir::OperationName("ac.connect", &context).isRegistered());
  EXPECT_FALSE(mlir::OperationName("ac.ready_valid", &context).isRegistered());

  std::vector<std::string> actual;
  for (mlir::RegisteredOperationName operation :
       context.getRegisteredOperationsByDialect("ac"))
    actual.push_back(operation.getStringRef().str());
  llvm::sort(actual);
  std::vector<std::string> expected = {
      "ac.array",
      "ac.address_map",
      "ac.address_space",
      "ac.assert",
      "ac.barrier",
      "ac.bitfield",
      "ac.broadcast",
      "ac.credit",
      "ac.credit.yield",
      "ac.dependency",
      "ac.dependency.yield",
      "ac.enum",
      "ac.ensure",
      "ac.event",
      "ac.event_queue",
      "ac.expect",
      "ac.expect.yield",
      "ac.feedback",
      "ac.feedback.yield",
      "ac.firing",
      "ac.firing.condition",
      "ac.firing.output",
      "ac.firing.yield",
      "ac.fork",
      "ac.guarantee",
      "ac.interface",
      "ac.instance",
      "ac.instances",
      "ac.instrumentation",
      "ac.module",
      "ac.module.extern",
      "ac.module.generated",
      "ac.merge",
      "ac.memory.instance",
      "ac.memory.request",
      "ac.memory.yield",
      "ac.marker.obligation",
      "ac.marker.type",
      "ac.marker.value",
      "ac.table",
      "ac.table.get",
      "ac.table.match",
      "ac.table.match.yield",
      "ac.table.choose",
      "ac.table.choose.yield",
      "ac.table.read",
      "ac.table.propose",
      "ac.table.write",
      "ac.table.masked_write",
      "ac.table.yield",
      "ac.slot",
      "ac.slot.get",
      "ac.slot.release",
      "ac.slot.yield",
      "ac.reorder",
      "ac.reorder.yield",
      "ac.observe",
      "ac.packet",
      "ac.packet.deserialize",
      "ac.packet.serialize",
      "ac.port",
      "ac.probe",
      "ac.process",
      "ac.protocol",
      "ac.queue",
      "ac.record.create",
      "ac.record.get",
      "ac.record.with",
      "ac.var.add",
      "ac.var.and",
      "ac.var.array",
      "ac.var.assign",
      "ac.var.assign_element",
      "ac.var.choose",
      "ac.var.choose.yield",
      "ac.var.concat",
      "ac.var.constant",
      "ac.var.count_zeros",
      "ac.var.cmp",
      "ac.var.decl",
      "ac.var.element",
      "ac.var.enum",
      "ac.var.extract",
      "ac.var.get",
      "ac.var.insert",
      "ac.var.match",
      "ac.var.match.yield",
      "ac.var.mul",
      "ac.var.not",
      "ac.var.or",
      "ac.var.popcount",
      "ac.var.priority_encode",
      "ac.var.read",
      "ac.var.read_element",
      "ac.var.select",
      "ac.var.shl",
      "ac.var.shr",
      "ac.var.matches",
      "ac.var.sub",
      "ac.var.tuple",
      "ac.var.xor",
      "ac.var.with",
      "ac.require",
      "ac.return",
      "ac.resource",
      "ac.rule",
      "ac.rule.condition",
      "ac.rule.output",
      "ac.rule.return",
      "ac.route",
      "ac.route.yield",
      "ac.role",
      "ac.scope",
      "ac.scope.yield",
      "ac.select",
      "ac.select.yield",
      "ac.sink",
      "ac.source",
      "ac.state",
      "ac.state.snapshot",
      "ac.state.snapshot_set",
      "ac.stat",
      "ac.stat.add",
      "ac.struct",
      "ac.system",
      "ac.time_domain",
      "ac.transaction",
      "ac.transition",
      "ac.transform",
      "ac.transform.yield",
      "ac.trace.decode",
      "ac.trace.eof",
      "ac.trace.next",
      "ac.trace.open",
      "ac.trace.position",
      "ac.try_recv",
      "ac.try_send",
      "ac.type_alias",
      "ac.type_scope",
      "ac.union",
      "ac.view",
      "ac.await_event",
      "ac.schedule",
      "ac.wait_for",
      "ac.wait_until",
      "ac.yield_sim",
  };
  llvm::sort(expected);
  EXPECT_EQ(actual, expected);
}

TEST(ACIROpsTest, PublicBuildersConstructEveryTaskSixOperation) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  getStructuralProviderRegistry(&context).registerExternal("Leaf");
  getStructuralProviderRegistry(&context).registerGenerator("Generated");
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto file = mlir::ModuleOp::create(loc);
  file->setAttr("ac.contract_epoch", builder.getStringAttr("0.5"));
  builder.setInsertionPointToStart(file.getBody());

  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto binding = builder.getDictionaryAttr({
      builder.getNamedAttr("registry", builder.getStringAttr("cpp")),
      builder.getNamedAttr("name", builder.getStringAttr("Leaf")),
  });
  auto generator = builder.getDictionaryAttr({
      builder.getNamedAttr("registry", builder.getStringAttr("ac")),
      builder.getNamedAttr("name", builder.getStringAttr("Generated")),
  });
  auto leaf =
      ModuleExternOp::create(builder, loc, "Leaf", emptyType,
                             mlir::StringAttr(), emptyDictionary, binding);
  auto generated =
      ModuleGeneratedOp::create(builder, loc, "Generated", emptyType,
                                mlir::StringAttr(), emptyDictionary, generator);
  EXPECT_TRUE(leaf);
  EXPECT_TRUE(generated);

  auto top = ModuleOp::create(builder, loc, "Top", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(top.addEntryBlock());
  auto instance =
      InstanceOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{},
                         "Leaf", "child", "child", "child", emptyDictionary);
  auto array = ArrayOp::create(
      builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "Leaf", "lanes",
      "lanes", "lanes", llvm::ArrayRef<int64_t>{2},
      builder.getArrayAttr({emptyDictionary, emptyDictionary}));
  auto definitions = builder.getArrayAttr({
      mlir::FlatSymbolRefAttr::get(&context, "Leaf"),
      mlir::FlatSymbolRefAttr::get(&context, "Generated"),
  });
  auto instances = InstancesOp::create(
      builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "mixed", "mixed",
      "mixed", definitions, builder.getStrArrayAttr({"a", "b"}),
      builder.getStrArrayAttr({"mix-a", "mix-b"}),
      builder.getStrArrayAttr({"mix_a", "mix_b"}), emptyType,
      builder.getArrayAttr({emptyDictionary, emptyDictionary}));
  auto view = ViewOp::create(
      builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "view",
      "permutation",
      builder.getArrayAttr({mlir::FlatSymbolRefAttr::get(&context, "child")}),
      builder.getArrayAttr({builder.getDenseI64ArrayAttr({0})}),
      mlir::IntegerAttr(), llvm::ArrayRef<int64_t>{},
      llvm::ArrayRef<int64_t>{0});
  auto returnOp = ReturnOp::create(builder, loc, mlir::ValueRange{});
  EXPECT_TRUE(instance);
  EXPECT_TRUE(array);
  EXPECT_TRUE(instances);
  EXPECT_TRUE(view);
  EXPECT_TRUE(returnOp);

  builder.setInsertionPointToStart(file.getBody());
  auto seedPolicy = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
  });
  auto resultSchema = builder.getDictionaryAttr({
      builder.getNamedAttr("id", builder.getStringAttr("default")),
      builder.getNamedAttr("format", builder.getStringAttr("json")),
  });
  auto system = SystemOp::create(builder, loc, "soc", "Top", "root", 0, "cycle",
                                 mlir::FlatSymbolRefAttr(), seedPolicy,
                                 builder.getArrayAttr({}), resultSchema, true);
  EXPECT_TRUE(system);
  EXPECT_TRUE(mlir::isa<mlir::FunctionOpInterface>(top.getOperation()));
  EXPECT_TRUE(mlir::isa<mlir::RegionKindInterface>(top.getOperation()));
  EXPECT_TRUE(mlir::succeeded(mlir::verify(file)));
  EXPECT_TRUE(mlir::succeeded(verifyGraphStructure(file)));
}

TEST(ACIROpsTest, TaskSixRegistryDeltaIsExactlyNineGraphOperations) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  const std::array<llvm::StringLiteral, 9> names = {
      "ac.system",           "ac.module",   "ac.module.extern",
      "ac.module.generated", "ac.instance", "ac.array",
      "ac.instances",        "ac.view",     "ac.return",
  };
  for (llvm::StringLiteral name : names)
    EXPECT_TRUE(mlir::OperationName(name, &context).isRegistered())
        << name.str();
  EXPECT_FALSE(mlir::OperationName("ac.connect", &context).isRegistered());
  EXPECT_FALSE(mlir::OperationName("ac.freeze", &context).isRegistered());
}

TEST(ACIROpsTest, PublicBuildersConstructEveryTaskEightOperation) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect, mlir::arith::ArithDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto file = mlir::parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.protocol @fifo {
        ac.role @sender dual @receiver cardinality "exclusive"
        ac.role @receiver dual @sender cardinality "exclusive"
        ac.state @idle initial true terminal false
        ac.state @done initial false terminal true
        ac.event @push from @sender to @receiver payload i32 action "offer"
        ac.transition from @idle to @done on @push transfer true retain false guard {}
      }
      ac.module @M(i32) parameters {} graph {
      ^bb0(%input : i32):
        ac.time_domain @clock period 1 phase 0 scale 1
        ac.queue @q payload i32 entries 4 ordering "fifo" protocol @fifo
            ownership "exclusive" id "q" path "q"
        ac.event_queue @events payload !ac.event<i32> capacity 4
            ordering "time_then_sequence" domain @clock id "events" path "events"
        ac.resource @resource capacity 1 issue_width 1 ii 1
            latency {kind = "fixed", ticks = 1 : i64}
            lifecycle {reservation = "propose_commit", release = "balanced", cancellation = "explicit"}
            ownership "exclusive" classes [] id "resource" path "resource"
        ac.address_space @memory width 32 unit "byte" id "memory" path "memory"
        ac.stat @count kind "counter"
        ac.process @worker kind "workload" captures(%input : i32) {
        ^bb0(%value : i32):
          ac.yield_sim
        }
        ac.return
      }
    }
  )mlir",
                                                      &context);
  ASSERT_TRUE(file);
  ModuleOp module = *file->getOps<ModuleOp>().begin();
  builder.setInsertionPoint(&module.getBody().front().back());
  StatOp stat = *module.getBody().front().getOps<StatOp>().begin();
  auto process =
      ProcessOp::create(builder, loc, "p", "control", mlir::ValueRange{});
  auto *body = &process.getBody().emplaceBlock();
  builder.setInsertionPointToStart(body);
  auto i1 =
      mlir::arith::ConstantOp::create(builder, loc, builder.getBoolAttr(true));
  auto i32 = mlir::arith::ConstantOp::create(builder, loc,
                                             builder.getI32IntegerAttr(7));
  auto i64 = mlir::arith::ConstantOp::create(builder, loc,
                                             builder.getI64IntegerAttr(1));
  auto send = TrySendOp::create(builder, loc, builder.getI1Type(), i32, "q");
  auto recv = TryRecvOp::create(builder, loc, builder.getI32Type(),
                                builder.getI1Type(), "q");
  EXPECT_TRUE(send && recv);
  auto schedule = ScheduleOp::create(builder, loc, i32, i64, "worker");
  auto waitUntil = WaitUntilOp::create(builder, loc, i1);
  auto waitFor = WaitForOp::create(builder, loc, "resource");
  auto awaitEvent = AwaitEventOp::create(builder, loc, "events");
  EXPECT_TRUE(schedule && waitUntil && waitFor && awaitEvent);
  auto cursor =
      TraceOpenOp::create(builder, loc, builder.getIndexType(), "pto");
  EXPECT_EQ(cursor.getSource(), "pto");
  auto next = TraceNextOp::create(builder, loc, builder.getIndexType(),
                                  builder.getI32Type(), builder.getI1Type(),
                                  cursor, "pto");
  auto decoded = TraceDecodeOp::create(builder, loc, builder.getI64Type(),
                                       next.getEntry());
  auto position = TracePositionOp::create(builder, loc, builder.getIndexType(),
                                          next.getCursor(), "pto");
  auto eof = TraceEofOp::create(builder, loc, builder.getI1Type(),
                                next.getCursor(), "pto");
  EXPECT_TRUE(decoded && eof);
  auto runtimeRequire = RequireOp::create(builder, loc, i1, "require");
  EXPECT_TRUE(runtimeRequire);
  auto runtimeEnsure = EnsureOp::create(builder, loc, i1, "ensure");
  auto runtimeAssert = AssertOp::create(builder, loc, i1, "assert");
  EXPECT_TRUE(runtimeEnsure && runtimeAssert);
  auto probe =
      ProbeOp::create(builder, loc, builder.getI32Type(), "q", "queue");
  auto storageProbe =
      ProbeOp::create(builder, loc, builder.getI64Type(), "memory", "storage");
  auto statAdd = StatAddOp::create(builder, loc, probe, "count");
  EXPECT_TRUE(statAdd && storageProbe);
  auto instrumentation = InstrumentationOp::create(builder, loc, "debug");
  instrumentation.getBody().emplaceBlock();
  auto yield = YieldSimOp::create(builder, loc);
  EXPECT_TRUE(yield);
  EXPECT_TRUE(mlir::succeeded(mlir::verify(*file)));
  std::string printed;
  llvm::raw_string_ostream(printed) << *file;
  auto reparsed = mlir::parseSourceString<mlir::ModuleOp>(printed, &context);
  ASSERT_TRUE(reparsed);
  EXPECT_TRUE(mlir::succeeded(mlir::verify(*reparsed)));

  auto effectsOf = [](mlir::Operation *operation) {
    llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
    mlir::cast<mlir::MemoryEffectOpInterface>(operation).getEffects(effects);
    return effects;
  };
  struct ExpectedEffect {
    bool write;
    mlir::SideEffects::Resource *resource;
    mlir::SymbolRefAttr symbol;
    mlir::DictionaryAttr parameters;
  };
  auto expectedEffect = [&](bool write, mlir::SideEffects::Resource *resource,
                            llvm::StringRef identity, llvm::StringRef kind,
                            llvm::StringRef contractPhase = "") {
    llvm::SmallVector<mlir::NamedAttribute> parameters = {
        builder.getNamedAttr("identity_phase",
                             builder.getStringAttr("definition_pre_freeze")),
        builder.getNamedAttr("owner_kind", builder.getStringAttr(kind)),
        builder.getNamedAttr("identity", builder.getStringAttr(identity)),
    };
    if (!contractPhase.empty())
      parameters.push_back(builder.getNamedAttr(
          "contract_phase", builder.getStringAttr(contractPhase)));
    return ExpectedEffect{
        write, resource,
        mlir::SymbolRefAttr::get(
            &context, "M", {mlir::FlatSymbolRefAttr::get(&context, identity)}),
        builder.getDictionaryAttr(parameters)};
  };
  auto expectExactEffects =
      [&](mlir::Operation *operation,
          std::initializer_list<ExpectedEffect> expected) {
        auto actual = effectsOf(operation);
        ASSERT_EQ(actual.size(), expected.size())
            << operation->getName().getStringRef().str();
        llvm::SmallVector<bool> matched(actual.size());
        for (const ExpectedEffect &want : expected) {
          bool found = false;
          for (auto [index, got] : llvm::enumerate(actual)) {
            bool effectMatches =
                want.write
                    ? mlir::isa<mlir::MemoryEffects::Write>(got.getEffect())
                    : mlir::isa<mlir::MemoryEffects::Read>(got.getEffect());
            if (!matched[index] && effectMatches &&
                got.getResource() == want.resource &&
                got.getSymbolRef() == want.symbol &&
                got.getParameters() == want.parameters) {
              matched[index] = true;
              found = true;
              break;
            }
          }
          EXPECT_TRUE(found)
              << operation->getName().getStringRef().str() << " missing exact "
              << (want.write ? "write" : "read") << " effect";
        }
      };
  auto read = [&](mlir::SideEffects::Resource *resource,
                  llvm::StringRef identity, llvm::StringRef kind,
                  llvm::StringRef contractPhase = "") {
    return expectedEffect(false, resource, identity, kind, contractPhase);
  };
  auto write = [&](mlir::SideEffects::Resource *resource,
                   llvm::StringRef identity, llvm::StringRef kind,
                   llvm::StringRef contractPhase = "") {
    return expectedEffect(true, resource, identity, kind, contractPhase);
  };

  for (mlir::Operation *operation : {send.getOperation(), recv.getOperation()})
    expectExactEffects(operation,
                       {read(QueueStateResource::get(), "q", "queue"),
                        write(QueueStateResource::get(), "q", "queue"),
                        read(ProtocolStateResource::get(), "q", "protocol"),
                        write(ProtocolStateResource::get(), "q", "protocol")});
  expectExactEffects(
      schedule,
      {write(ModuleStateResource::get(), "worker", "module"),
       write(EventQueueStateResource::get(), "worker", "event_queue")});
  expectExactEffects(waitUntil,
                     {read(EventQueueStateResource::get(), "p", "event_queue"),
                      write(ModuleStateResource::get(), "p", "module")});
  expectExactEffects(
      waitFor, {read(ReservationStateResource::get(), "resource", "resource"),
                write(ModuleStateResource::get(), "p", "module")});
  expectExactEffects(
      awaitEvent,
      {read(EventQueueStateResource::get(), "events", "event_queue"),
       write(ModuleStateResource::get(), "p", "module")});
  expectExactEffects(yield, {write(ModuleStateResource::get(), "p", "module")});
  expectExactEffects(cursor,
                     {read(ExternalIOResource::get(), "p/pto", "external_io"),
                      write(TracePositionResource::get(), "p/pto", "trace")});
  expectExactEffects(next,
                     {read(TracePositionResource::get(), "p/pto", "trace"),
                      write(TracePositionResource::get(), "p/pto", "trace"),
                      read(ExternalIOResource::get(), "p/pto", "external_io")});
  for (mlir::Operation *operation :
       {position.getOperation(), eof.getOperation()})
    expectExactEffects(operation,
                       {read(TracePositionResource::get(), "p/pto", "trace")});
  for (mlir::Operation *operation :
       {runtimeRequire.getOperation(), runtimeEnsure.getOperation(),
        runtimeAssert.getOperation()})
    expectExactEffects(operation, {write(ExternalIOResource::get(), "p",
                                         "contract", "runtime")});
  expectExactEffects(probe, {read(QueueStateResource::get(), "q", "queue")});
  expectExactEffects(storageProbe,
                     {read(StorageStateResource::get(), "memory", "storage")});
  expectExactEffects(stat,
                     {write(StatisticsResource::get(), "count", "statistics")});
  expectExactEffects(statAdd,
                     {read(StatisticsResource::get(), "count", "statistics"),
                      write(StatisticsResource::get(), "count", "statistics")});
  EXPECT_TRUE(mlir::isMemoryEffectFree(decoded));
  EXPECT_TRUE(mlir::isa<ObservationOpInterface>(*instrumentation));
  EXPECT_FALSE(mlir::isMemoryEffectFree(process));
  EXPECT_FALSE(mlir::isMemoryEffectFree(send));
  EXPECT_FALSE(mlir::isMemoryEffectFree(next));
  EXPECT_FALSE(mlir::isMemoryEffectFree(position));
  EXPECT_FALSE(mlir::isMemoryEffectFree(eof));
  EXPECT_FALSE(mlir::isMemoryEffectFree(statAdd));
}

TEST(ACIROpsTest, UnresolvedRuntimeReferencesDoNotInventEffects) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect, mlir::arith::ArithDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto file = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(file.getBody());
  auto module =
      ModuleOp::create(builder, loc, "M", builder.getFunctionType({}, {}),
                       builder.getDictionaryAttr({}));
  builder.setInsertionPointToStart(module.addEntryBlock());
  auto process =
      ProcessOp::create(builder, loc, "p", "control", mlir::ValueRange{});
  builder.setInsertionPointToStart(&process.getBody().emplaceBlock());
  auto value = mlir::arith::ConstantOp::create(builder, loc,
                                               builder.getI32IntegerAttr(1));
  auto send =
      TrySendOp::create(builder, loc, builder.getI1Type(), value, "missing");
  llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
  mlir::cast<mlir::MemoryEffectOpInterface>(*send).getEffects(effects);
  EXPECT_TRUE(effects.empty());
}

TEST(ACIROpsTest, RuntimeAndQueueVarRegistryIsExact) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  const std::array<llvm::StringLiteral, 20> names = {
      "ac.process",        "ac.try_send",        "ac.try_recv",
      "ac.schedule",       "ac.wait_until",      "ac.wait_for",
      "ac.await_event",    "ac.yield_sim",       "ac.trace.open",
      "ac.trace.next",     "ac.trace.decode",    "ac.trace.eof",
      "ac.trace.position", "ac.require",         "ac.ensure",
      "ac.assert",         "ac.probe",           "ac.stat",
      "ac.stat.add",       "ac.instrumentation",
  };
  for (llvm::StringLiteral name : names)
    EXPECT_TRUE(mlir::OperationName(name, &context).isRegistered())
        << name.str();
  EXPECT_FALSE(mlir::OperationName("ac.try_issue", &context).isRegistered());
  EXPECT_FALSE(mlir::OperationName("ac.connect", &context).isRegistered());
  const std::array<llvm::StringLiteral, 91> queueVarNames = {
      "ac.transform",
      "ac.transform.yield",
      "ac.rule",
      "ac.rule.condition",
      "ac.rule.output",
      "ac.rule.return",
      "ac.marker.type",
      "ac.marker.value",
      "ac.marker.obligation",
      "ac.firing",
      "ac.firing.condition",
      "ac.firing.output",
      "ac.firing.yield",
      "ac.bitfield",
      "ac.source",
      "ac.sink",
      "ac.var.constant",
      "ac.var.count_zeros",
      "ac.var.add",
      "ac.var.and",
      "ac.var.array",
      "ac.var.assign",
      "ac.var.assign_element",
      "ac.var.choose",
      "ac.var.choose.yield",
      "ac.var.concat",
      "ac.var.decl",
      "ac.var.element",
      "ac.var.enum",
      "ac.var.extract",
      "ac.var.insert",
      "ac.var.match",
      "ac.var.match.yield",
      "ac.var.read",
      "ac.var.read_element",
      "ac.var.select",
      "ac.var.sub",
      "ac.var.mul",
      "ac.var.not",
      "ac.var.or",
      "ac.var.popcount",
      "ac.var.priority_encode",
      "ac.var.shl",
      "ac.var.shr",
      "ac.var.matches",
      "ac.var.tuple",
      "ac.var.xor",
      "ac.var.get",
      "ac.var.with",
      "ac.scope",
      "ac.scope.yield",
      "ac.broadcast",
      "ac.route",
      "ac.route.yield",
      "ac.select",
      "ac.select.yield",
      "ac.fork",
      "ac.merge",
      "ac.barrier",
      "ac.dependency",
      "ac.dependency.yield",
      "ac.credit",
      "ac.credit.yield",
      "ac.memory.instance",
      "ac.memory.request",
      "ac.memory.yield",
      "ac.table",
      "ac.table.get",
      "ac.table.match",
      "ac.table.match.yield",
      "ac.table.choose",
      "ac.table.choose.yield",
      "ac.table.read",
      "ac.table.propose",
      "ac.table.write",
      "ac.table.masked_write",
      "ac.table.yield",
      "ac.slot",
      "ac.slot.get",
      "ac.slot.release",
      "ac.slot.yield",
      "ac.reorder",
      "ac.reorder.yield",
      "ac.feedback",
      "ac.feedback.yield",
      "ac.observe",
      "ac.expect",
      "ac.expect.yield",
      "ac.var.cmp",
      "ac.state.snapshot",
      "ac.state.snapshot_set",
  };
  for (llvm::StringLiteral name : queueVarNames)
    EXPECT_TRUE(mlir::OperationName(name, &context).isRegistered())
        << name.str();
  EXPECT_EQ(context.getRegisteredOperationsByDialect("ac").size(), 146u);
}

TEST(ACIROpsTest, ProcessLinearLivenessDoesNotRescanBlockPerValue) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect, mlir::arith::ArithDialect,
                      mlir::scf::SCFDialect>();

  auto buildProcess = [&](unsigned valueCount) {
    std::string source;
    llvm::raw_string_ostream os(source);
    os << "builtin.module attributes {ac.contract_epoch = \"0.5\"} {\n"
          "  ac.module @Scale() parameters {} graph {\n"
          "    ac.process @worker kind \"control\" {\n";
    for (unsigned index = 0; index != valueCount; ++index)
      os << "      %cursor" << index << " = ac.trace.open source \"source"
         << index << "\"\n"
         << "      %next" << index << ", %value" << index << ", %advanced"
         << index << " = ac.trace.next %cursor" << index
         << " from source \"source" << index
         << "\" : !ac.resource_token<@resource>\n";
    for (unsigned index = 0; index != valueCount; ++index)
      os << "      %decoded" << index << " = ac.trace.decode %value" << index
         << " : !ac.resource_token<@resource> to i64\n";
    os << "      ac.yield_sim\n"
          "    }\n"
          "    ac.return\n"
          "  }\n"
          "}\n";
    auto file = mlir::parseSourceString<mlir::ModuleOp>(source, &context);
    EXPECT_TRUE(file);
    return file;
  };
  auto measureWork = [&](unsigned valueCount) {
    auto file = buildProcess(valueCount);
    if (!file)
      return detail::ProcessLivenessWork{};
    ProcessOp process;
    file->walk([&](ProcessOp candidate) { process = candidate; });
    EXPECT_TRUE(process);
    detail::ProcessLivenessWork work;
    {
      detail::ScopedProcessLivenessWorkCollector collector(work);
      EXPECT_TRUE(mlir::succeeded(process.verify()));
    }
    return work;
  };

  auto expectSinglePassWork = [](const detail::ProcessLivenessWork &work,
                                 uint64_t valueCount) {
    // Each fixture value contributes trace.open, trace.next, and trace.decode.
    // The only fixed operation is the terminating ac.yield_sim.
    uint64_t operationCount = valueCount * 3 + 1;
    EXPECT_EQ(work.summaryOperationVisits, operationCount);
    EXPECT_EQ(work.epochOperationVisits, operationCount);
    EXPECT_EQ(work.livenessOperationVisits, operationCount);
    EXPECT_EQ(work.valueVisits, valueCount * 5);
    EXPECT_EQ(work.useVisits, valueCount);
    EXPECT_EQ(work.total(), valueCount * 15 + 3);
  };

  constexpr unsigned smallSize = 64;
  constexpr unsigned largeSize = 256;
  detail::ProcessLivenessWork smallWork = measureWork(smallSize);
  detail::ProcessLivenessWork largeWork = measureWork(largeSize);
  RecordProperty("small_values", smallSize);
  RecordProperty("large_values", largeSize);
  RecordProperty("small_work_units", smallWork.total());
  RecordProperty("large_work_units", largeWork.total());
  expectSinglePassWork(smallWork, smallSize);
  expectSinglePassWork(largeWork, largeSize);
  EXPECT_EQ(largeWork.total() - smallWork.total(),
            uint64_t{15} * (largeSize - smallSize));
}

TEST(ACIROpsTest, LargeArrayVerificationIsDeterministic) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  getStructuralProviderRegistry(&context).registerExternal("Leaf");
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto file = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(file.getBody());
  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto binding = builder.getDictionaryAttr({
      builder.getNamedAttr("registry", builder.getStringAttr("cpp")),
      builder.getNamedAttr("name", builder.getStringAttr("Leaf")),
  });
  ModuleExternOp::create(builder, loc, "Leaf", emptyType, mlir::StringAttr(),
                         emptyDictionary, binding);
  auto top = ModuleOp::create(builder, loc, "Top", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(top.addEntryBlock());
  constexpr int64_t elementCount = 4096;
  llvm::SmallVector<mlir::Attribute> arguments(elementCount, emptyDictionary);
  ArrayOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "Leaf",
                  "large", "large", "large", llvm::ArrayRef<int64_t>{64, 64},
                  builder.getArrayAttr(arguments));
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  EXPECT_TRUE(mlir::succeeded(mlir::verify(file)));
  EXPECT_TRUE(mlir::succeeded(mlir::verify(file)));
  EXPECT_TRUE(mlir::succeeded(verifyGraphStructure(file)));
  EXPECT_EQ(buildArrayElementPath("root.large", {1, 2, 3}),
            "root.large[1][2][3]");
}

TEST(ACIROpsTest, ModulePortMetadataPrintsAndReparsesCanonically) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  constexpr llvm::StringLiteral source = R"mlir(
    ac.module @M(%x : i32 {ac.port_name = "input"})
        -> (i32 {ac.port_name = "output"}) parameters {}
        attributes {ac.graph_label = "graph"} graph {
      ac.return %x : i32
    }
  )mlir";
  auto module = mlir::parseSourceString<mlir::ModuleOp>(source, &context);
  ASSERT_TRUE(module);
  auto operation = *module->getOps<ModuleOp>().begin();
  ASSERT_TRUE(operation.getArgAttrsAttr());
  ASSERT_TRUE(operation.getResAttrsAttr());
  std::string printed;
  llvm::raw_string_ostream(printed) << *module;
  auto reparsed = mlir::parseSourceString<mlir::ModuleOp>(printed, &context);
  ASSERT_TRUE(reparsed);
  auto reparsedOperation = *reparsed->getOps<ModuleOp>().begin();
  EXPECT_EQ(operation.getArgAttrsAttr(), reparsedOperation.getArgAttrsAttr());
  EXPECT_EQ(operation.getResAttrsAttr(), reparsedOperation.getResAttrsAttr());
  EXPECT_EQ(operation->getAttr("ac.graph_label"),
            reparsedOperation->getAttr("ac.graph_label"));
}

TEST(ACIROpsTest, StructuralProvidersAreContextOwnedAndExact) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  auto &providers = getStructuralProviderRegistry(&context);
  providers.registerExternal("test.external");
  providers.registerGenerator("test.generator");
  EXPECT_TRUE(providers.hasExternal("test.external"));
  EXPECT_TRUE(providers.hasGenerator("test.generator"));
  EXPECT_FALSE(providers.hasExternal("test.generator"));
  EXPECT_FALSE(providers.hasGenerator("test.external"));
}

TEST(ACIROpsTest, StaticUnitDictionaryUsesClosedACIRUnitSet) {
  mlir::MLIRContext context;
  mlir::Builder builder(&context);
  auto unitValue = [&](llvm::StringRef unit) {
    return builder.getDictionaryAttr({
        builder.getNamedAttr("value", builder.getI64IntegerAttr(1)),
        builder.getNamedAttr("unit", builder.getStringAttr(unit)),
    });
  };
  for (llvm::StringRef unit :
       {"ticks", "cycles", "seconds", "milliseconds", "microseconds",
        "nanoseconds", "picoseconds", "bytes", "bits", "entries", "packets",
        "transactions"})
    EXPECT_TRUE(isConcreteStaticValue(unitValue(unit))) << unit.str();
  EXPECT_FALSE(isConcreteStaticValue(unitValue("bananas")));
  EXPECT_FALSE(isConcreteStaticValue(unitValue("")));
}

TEST(ACIROpsTest, HierarchyDepthAndOwnerBudgetsRejectCompactGraphs) {
  auto buildGraph = [](mlir::MLIRContext &context, unsigned moduleCount,
                       unsigned fanout, llvm::StringRef prefix) {
    mlir::OpBuilder builder(&context);
    auto loc = builder.getUnknownLoc();
    auto file = mlir::ModuleOp::create(loc);
    auto emptyType = builder.getFunctionType({}, {});
    auto emptyDictionary = builder.getDictionaryAttr({});
    builder.setInsertionPointToStart(file.getBody());
    for (unsigned index = 0; index != moduleCount; ++index) {
      std::string name = (prefix + std::to_string(index)).str();
      auto module =
          ModuleOp::create(builder, loc, name, emptyType, emptyDictionary);
      builder.setInsertionPointToStart(module.addEntryBlock());
      if (index + 1 != moduleCount) {
        std::string target = (prefix + std::to_string(index + 1)).str();
        for (unsigned child = 0; child != fanout; ++child) {
          std::string segment = "child" + std::to_string(child);
          InstanceOp::create(builder, loc, mlir::TypeRange{},
                             mlir::ValueRange{}, target, segment, segment,
                             segment, emptyDictionary);
        }
      }
      ReturnOp::create(builder, loc, mlir::ValueRange{});
      builder.setInsertionPointToEnd(file.getBody());
    }
    auto seed = builder.getDictionaryAttr({
        builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
        builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
    });
    auto results = builder.getDictionaryAttr({
        builder.getNamedAttr("id", builder.getStringAttr("budget")),
        builder.getNamedAttr("format", builder.getStringAttr("json")),
    });
    std::string root = (prefix + "0").str();
    SystemOp::create(builder, loc, "budget", root, "root", 0, "cycle",
                     mlir::FlatSymbolRefAttr(), seed, builder.getArrayAttr({}),
                     results, true);
    return file;
  };

  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  llvm::SmallVector<std::string> diagnostics;
  mlir::ScopedDiagnosticHandler handler(
      &context, [&](mlir::Diagnostic &diagnostic) {
        std::string text;
        llvm::raw_string_ostream(text) << diagnostic;
        diagnostics.push_back(std::move(text));
        return mlir::success();
      });
  auto depthBoundary = buildGraph(context, 1025, 1, "Boundary");
  EXPECT_TRUE(mlir::succeeded(verifyGraphStructure(depthBoundary)));
  diagnostics.clear();
  auto tooDeep = buildGraph(context, 1026, 1, "Depth");
  EXPECT_TRUE(mlir::failed(verifyGraphStructure(tooDeep)));
  EXPECT_TRUE(llvm::any_of(diagnostics, [](llvm::StringRef diagnostic) {
    return diagnostic.contains("hierarchy depth exceeds bound 1024");
  }));
  diagnostics.clear();
  auto tooWide = buildGraph(context, 21, 2, "Wide");
  EXPECT_TRUE(mlir::failed(verifyGraphStructure(tooWide)));
  EXPECT_TRUE(llvm::any_of(diagnostics, [](llvm::StringRef diagnostic) {
    return diagnostic.contains("owner count exceeds bound 1048576");
  }));
  diagnostics.clear();
  auto veryDeep = buildGraph(context, 20001, 1, "VeryDeep");
  EXPECT_TRUE(mlir::failed(verifyGraphStructure(veryDeep)));
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_TRUE(llvm::StringRef(diagnostics.front())
                  .contains("hierarchy depth exceeds bound 1024"));
}

TEST(ACIROpsTest, NestedArraysCountTaskSevenOwnersBeforeElaboration) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto file = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(file.getBody());

  auto leaf =
      ModuleOp::create(builder, loc, "Leaf", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(leaf.addEntryBlock());
  auto latency = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("ticks", builder.getI64IntegerAttr(1)),
  });
  auto lifecycle = builder.getDictionaryAttr({
      builder.getNamedAttr("reservation",
                           builder.getStringAttr("propose_commit")),
      builder.getNamedAttr("release", builder.getStringAttr("balanced")),
      builder.getNamedAttr("cancellation", builder.getStringAttr("explicit")),
  });
  QueueOp::create(
      builder, loc, builder.getStringAttr("queue"),
      builder.getStringAttr("queue"), builder.getStringAttr("queue"),
      mlir::TypeAttr::get(builder.getI32Type()), builder.getI64IntegerAttr(1),
      mlir::IntegerAttr(), builder.getStringAttr("fifo"),
      mlir::FlatSymbolRefAttr::get(&context, "p"),
      builder.getStringAttr("exclusive"), mlir::DictionaryAttr(),
      builder.getI64IntegerAttr(1));
  EventQueueOp::create(
      builder, loc, builder.getStringAttr("events"),
      builder.getStringAttr("events"), builder.getStringAttr("events"),
      mlir::TypeAttr::get(EventType::get(&context, builder.getI32Type())),
      builder.getI64IntegerAttr(1), builder.getStringAttr("time_then_sequence"),
      mlir::FlatSymbolRefAttr::get(&context, "clock"),
      builder.getI64IntegerAttr(1));
  ResourceOp::create(builder, loc, "state", "state", "state", 1, 1, 1, latency,
                     lifecycle, "exclusive", mlir::FlatSymbolRefAttr(),
                     builder.getArrayAttr({}), 1);
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  llvm::SmallVector<mlir::Attribute> staticArgs(
      512, mlir::Attribute(emptyDictionary));
  builder.setInsertionPointToEnd(file.getBody());
  auto middle =
      ModuleOp::create(builder, loc, "Middle", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(middle.addEntryBlock());
  ArrayOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "Leaf",
                  "leaves", "leaves", "leaves",
                  builder.getDenseI64ArrayAttr({512}),
                  builder.getArrayAttr(staticArgs));
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  builder.setInsertionPointToEnd(file.getBody());
  auto top = ModuleOp::create(builder, loc, "Top", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(top.addEntryBlock());
  ArrayOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "Middle",
                  "middles", "middles", "middles",
                  builder.getDenseI64ArrayAttr({512}),
                  builder.getArrayAttr(staticArgs));
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  builder.setInsertionPointToEnd(file.getBody());
  auto seed = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
  });
  auto results = builder.getDictionaryAttr({
      builder.getNamedAttr("id", builder.getStringAttr("owners")),
      builder.getNamedAttr("format", builder.getStringAttr("json")),
  });
  SystemOp::create(builder, loc, "owners", "Top", "root", 0, "cycle",
                   mlir::FlatSymbolRefAttr(), seed, builder.getArrayAttr({}),
                   results, true);

  auto structureOnly = mlir::cast<mlir::ModuleOp>(file->clone());
  auto structureLeaf = *structureOnly.getOps<ModuleOp>().begin();
  for (mlir::Operation &operation :
       llvm::make_early_inc_range(structureLeaf.getBody().front()))
    if (mlir::isa<QueueOp, EventQueueOp, ResourceOp>(operation))
      operation.erase();
  EXPECT_TRUE(mlir::succeeded(verifyGraphStructure(structureOnly)));

  std::string diagnostic;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return mlir::success();
  });
  auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(mlir::failed(verifyGraphStructure(file)));
  EXPECT_NE(diagnostic.find("owner count exceeds bound 1048576"),
            std::string::npos);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST(ACIROpsTest, TaskSevenOwnersRegisterAtDistinctAbsoluteInstancePaths) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto file = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(file.getBody());
  auto leaf =
      ModuleOp::create(builder, loc, "Leaf", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(leaf.addEntryBlock());
  auto latency = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("ticks", builder.getI64IntegerAttr(1)),
  });
  auto lifecycle = builder.getDictionaryAttr({
      builder.getNamedAttr("reservation",
                           builder.getStringAttr("propose_commit")),
      builder.getNamedAttr("release", builder.getStringAttr("balanced")),
      builder.getNamedAttr("cancellation", builder.getStringAttr("explicit")),
  });
  QueueOp::create(
      builder, loc, builder.getStringAttr("queue"),
      builder.getStringAttr("queue"), builder.getStringAttr("queue"),
      mlir::TypeAttr::get(builder.getI32Type()), builder.getI64IntegerAttr(1),
      mlir::IntegerAttr(), builder.getStringAttr("fifo"),
      mlir::FlatSymbolRefAttr::get(&context, "p"),
      builder.getStringAttr("exclusive"), mlir::DictionaryAttr(),
      builder.getI64IntegerAttr(1));
  EventQueueOp::create(
      builder, loc, builder.getStringAttr("events"),
      builder.getStringAttr("events"), builder.getStringAttr("events"),
      mlir::TypeAttr::get(EventType::get(&context, builder.getI32Type())),
      builder.getI64IntegerAttr(1), builder.getStringAttr("time_then_sequence"),
      mlir::FlatSymbolRefAttr::get(&context, "clock"),
      builder.getI64IntegerAttr(1));
  ResourceOp::create(builder, loc, "state", "state", "state", 1, 1, 1, latency,
                     lifecycle, "exclusive", mlir::FlatSymbolRefAttr(),
                     builder.getArrayAttr({}), 1);
  ReturnOp::create(builder, loc, mlir::ValueRange{});
  builder.setInsertionPointToEnd(file.getBody());
  auto top = ModuleOp::create(builder, loc, "Top", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(top.addEntryBlock());
  InstanceOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{},
                     "Leaf", "left", "left", "left", emptyDictionary);
  InstanceOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{},
                     "Leaf", "right", "right", "right", emptyDictionary);
  ReturnOp::create(builder, loc, mlir::ValueRange{});
  builder.setInsertionPointToEnd(file.getBody());
  auto seed = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
  });
  auto results = builder.getDictionaryAttr({
      builder.getNamedAttr("id", builder.getStringAttr("owners")),
      builder.getNamedAttr("format", builder.getStringAttr("json")),
  });
  SystemOp::create(builder, loc, "owners", "Top", "root", 0, "cycle",
                   mlir::FlatSymbolRefAttr(), seed, builder.getArrayAttr({}),
                   results, true);
  llvm::SmallVector<ElaboratedStateOwner> owners;
  ASSERT_TRUE(mlir::succeeded(collectElaboratedStateOwners(file, owners)));
  ASSERT_EQ(owners.size(), 6u);
  EXPECT_EQ(owners[0].path, "root.left.queue");
  EXPECT_EQ(owners[0].stableId, "root/left/queue");
  EXPECT_EQ(owners[1].path, "root.left.events");
  EXPECT_EQ(owners[1].stableId, "root/left/events");
  EXPECT_EQ(owners[2].path, "root.left.state");
  EXPECT_EQ(owners[2].stableId, "root/left/state");
  EXPECT_EQ(owners[3].path, "root.right.queue");
  EXPECT_EQ(owners[3].stableId, "root/right/queue");
  EXPECT_EQ(owners[4].path, "root.right.events");
  EXPECT_EQ(owners[4].stableId, "root/right/events");
  EXPECT_EQ(owners[5].path, "root.right.state");
  EXPECT_EQ(owners[5].stableId, "root/right/state");
}

TEST(ACIROpsTest, TaskEightOwnersRegisterAtDistinctAbsoluteInstancePaths) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto file = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(file.getBody());
  auto leaf =
      ModuleOp::create(builder, loc, "Leaf", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(leaf.addEntryBlock());
  auto process =
      ProcessOp::create(builder, loc, "worker", "workload", mlir::ValueRange{});
  builder.setInsertionPointToStart(&process.getBody().emplaceBlock());
  YieldSimOp::create(builder, loc);
  builder.setInsertionPointToEnd(&leaf.getBody().front());
  StatOp::create(builder, loc, "requests", "counter");
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  builder.setInsertionPointToEnd(file.getBody());
  auto top = ModuleOp::create(builder, loc, "Top", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(top.addEntryBlock());
  InstanceOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{},
                     "Leaf", "left", "left", "left", emptyDictionary);
  InstanceOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{},
                     "Leaf", "right", "right", "right", emptyDictionary);
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  builder.setInsertionPointToEnd(file.getBody());
  auto seed = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
  });
  auto results = builder.getDictionaryAttr({
      builder.getNamedAttr("id", builder.getStringAttr("owners")),
      builder.getNamedAttr("format", builder.getStringAttr("json")),
  });
  SystemOp::create(builder, loc, "owners", "Top", "root", 0, "cycle",
                   mlir::FlatSymbolRefAttr(), seed, builder.getArrayAttr({}),
                   results, true);

  llvm::SmallVector<ElaboratedStateOwner> owners;
  ASSERT_TRUE(mlir::succeeded(collectElaboratedStateOwners(file, owners)));
  ASSERT_EQ(owners.size(), 4u);
  EXPECT_EQ(owners[0].path, "root.left.worker");
  EXPECT_EQ(owners[0].stableId, "root/left/worker");
  EXPECT_EQ(owners[1].path, "root.left.requests");
  EXPECT_EQ(owners[1].stableId, "root/left/requests");
  EXPECT_EQ(owners[2].path, "root.right.worker");
  EXPECT_EQ(owners[2].stableId, "root/right/worker");
  EXPECT_EQ(owners[3].path, "root.right.requests");
  EXPECT_EQ(owners[3].stableId, "root/right/requests");
}

TEST(ACIROpsTest, TaskEightOwnersParticipateInSaturatedArrayBudget) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto file = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(file.getBody());
  auto leaf =
      ModuleOp::create(builder, loc, "Leaf", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(leaf.addEntryBlock());
  auto process =
      ProcessOp::create(builder, loc, "worker", "workload", mlir::ValueRange{});
  builder.setInsertionPointToStart(&process.getBody().emplaceBlock());
  YieldSimOp::create(builder, loc);
  builder.setInsertionPointToEnd(&leaf.getBody().front());
  StatOp::create(builder, loc, "requests", "counter");
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  llvm::SmallVector<mlir::Attribute> staticArgs(
      512, mlir::Attribute(emptyDictionary));
  builder.setInsertionPointToEnd(file.getBody());
  auto middle =
      ModuleOp::create(builder, loc, "Middle", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(middle.addEntryBlock());
  ArrayOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "Leaf",
                  "leaves", "leaves", "leaves",
                  builder.getDenseI64ArrayAttr({512}),
                  builder.getArrayAttr(staticArgs));
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  builder.setInsertionPointToEnd(file.getBody());
  auto top = ModuleOp::create(builder, loc, "Top", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(top.addEntryBlock());
  ArrayOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "Middle",
                  "middles", "middles", "middles",
                  builder.getDenseI64ArrayAttr({512}),
                  builder.getArrayAttr(staticArgs));
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  builder.setInsertionPointToEnd(file.getBody());
  auto seed = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
  });
  auto results = builder.getDictionaryAttr({
      builder.getNamedAttr("id", builder.getStringAttr("owners")),
      builder.getNamedAttr("format", builder.getStringAttr("json")),
  });
  SystemOp::create(builder, loc, "owners", "Top", "root", 0, "cycle",
                   mlir::FlatSymbolRefAttr(), seed, builder.getArrayAttr({}),
                   results, true);
  EXPECT_TRUE(mlir::succeeded(verifyGraphStructure(file)));

  auto overBudget = mlir::cast<mlir::ModuleOp>(file->clone());
  auto overBudgetLeaf = *overBudget.getOps<ModuleOp>().begin();
  builder.setInsertionPoint(&overBudgetLeaf.getBody().front().back());
  StatOp::create(builder, loc, "latency", "counter");
  std::string diagnostic;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return mlir::success();
  });
  EXPECT_TRUE(mlir::failed(verifyGraphStructure(overBudget)));
  EXPECT_NE(diagnostic.find("owner count exceeds bound 1048576"),
            std::string::npos);
}

TEST(ACIROpsTest, TraceSourcesHaveOneOwnerAcrossElaboratedHierarchy) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  auto singleOwner = mlir::parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.module @Top() parameters {} graph {
        ac.process @workload kind "workload" {
          %cursor = ac.trace.open source "pto"
          ac.yield_sim
        }
        ac.return
      }
      ac.system @test root @Top as "root" tick 0 "cycle"
          seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "trace", format = "json"} selected true
    }
  )mlir",
                                                             &context);
  ASSERT_TRUE(singleOwner);
  llvm::SmallVector<ElaboratedStateOwner> owners;
  ASSERT_TRUE(mlir::succeeded(
      collectElaboratedStateOwners(singleOwner->getOperation(), owners)));
  ASSERT_EQ(owners.size(), 1u);
  EXPECT_EQ(owners.front().path, "root.workload");
  EXPECT_EQ(owners.front().stableId, "root/workload");
  ASSERT_EQ(owners.front().traceSources.size(), 1u);
  EXPECT_EQ(owners.front().traceSources.front(), "pto");

  auto expectDuplicate = [&](llvm::StringRef source) {
    auto file = mlir::parseSourceString<mlir::ModuleOp>(source, &context);
    ASSERT_TRUE(file);
    std::string diagnostic;
    mlir::ScopedDiagnosticHandler handler(
        &context, [&](mlir::Diagnostic &value) {
          llvm::raw_string_ostream(diagnostic) << value;
          return mlir::success();
        });
    EXPECT_TRUE(mlir::failed(verifyGraphStructure(file->getOperation())));
    EXPECT_NE(diagnostic.find("trace source 'pto' has multiple elaborated "
                              "cursor owners"),
              std::string::npos);
  };

  expectDuplicate(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.module @Left() parameters {} graph {
        ac.process @workload kind "workload" {
          %cursor = ac.trace.open source "pto"
          ac.yield_sim
        }
        ac.return
      }
      ac.module @Right() parameters {} graph {
        ac.process @workload kind "workload" {
          %cursor = ac.trace.open source "pto"
          ac.yield_sim
        }
        ac.return
      }
      ac.module @Top() parameters {} graph {
        ac.instance @left of @Left() static {} id "left" path "left" : () -> ()
        ac.instance @right of @Right() static {} id "right" path "right" : () -> ()
        ac.return
      }
      ac.system @test root @Top as "root" tick 0 "cycle"
          seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "trace", format = "json"} selected true
    }
  )mlir");

  expectDuplicate(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.module @Leaf() parameters {} graph {
        ac.process @workload kind "workload" {
          %cursor = ac.trace.open source "pto"
          ac.yield_sim
        }
        ac.return
      }
      ac.module @Top() parameters {} graph {
        ac.instance @left of @Leaf() static {} id "left" path "left" : () -> ()
        ac.instance @right of @Leaf() static {} id "right" path "right" : () -> ()
        ac.return
      }
      ac.system @test root @Top as "root" tick 0 "cycle"
          seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "trace", format = "json"} selected true
    }
  )mlir");
}

TEST(ACIROpsTest, TraceSourceArrayDuplicationFailsBeforeElaboration) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto file = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(file.getBody());
  auto leaf =
      ModuleOp::create(builder, loc, "Leaf", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(leaf.addEntryBlock());
  auto process = ProcessOp::create(builder, loc, "workload", "workload",
                                   mlir::ValueRange{});
  builder.setInsertionPointToStart(&process.getBody().emplaceBlock());
  TraceOpenOp::create(builder, loc, builder.getIndexType(), "pto");
  YieldSimOp::create(builder, loc);
  builder.setInsertionPointToEnd(&leaf.getBody().front());
  ReturnOp::create(builder, loc, mlir::ValueRange{});

  llvm::SmallVector<mlir::Attribute> staticArgs(
      512, mlir::Attribute(emptyDictionary));
  builder.setInsertionPointToEnd(file.getBody());
  auto middle =
      ModuleOp::create(builder, loc, "Middle", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(middle.addEntryBlock());
  ArrayOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "Leaf",
                  "leaves", "leaves", "leaves",
                  builder.getDenseI64ArrayAttr({512}),
                  builder.getArrayAttr(staticArgs));
  ReturnOp::create(builder, loc, mlir::ValueRange{});
  builder.setInsertionPointToEnd(file.getBody());
  auto top = ModuleOp::create(builder, loc, "Top", emptyType, emptyDictionary);
  builder.setInsertionPointToStart(top.addEntryBlock());
  ArrayOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "Middle",
                  "middles", "middles", "middles",
                  builder.getDenseI64ArrayAttr({512}),
                  builder.getArrayAttr(staticArgs));
  ReturnOp::create(builder, loc, mlir::ValueRange{});
  builder.setInsertionPointToEnd(file.getBody());
  auto seed = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", builder.getStringAttr("fixed")),
      builder.getNamedAttr("value", builder.getI64IntegerAttr(0)),
  });
  auto results = builder.getDictionaryAttr({
      builder.getNamedAttr("id", builder.getStringAttr("trace")),
      builder.getNamedAttr("format", builder.getStringAttr("json")),
  });
  SystemOp::create(builder, loc, "trace", "Top", "root", 0, "cycle",
                   mlir::FlatSymbolRefAttr(), seed, builder.getArrayAttr({}),
                   results, true);

  std::string diagnostic;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return mlir::success();
  });
  auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(mlir::failed(verifyGraphStructure(file)));
  EXPECT_NE(diagnostic.find("trace source 'pto' has multiple elaborated cursor "
                            "owners"),
            std::string::npos);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST(ACIROpsTest, StaticContractsUseFreezePhaseModuleEffects) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  auto file = mlir::parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.module @M(i1) parameters {} graph {
      ^bb0(%condition : i1):
        ac.require %condition, "capacity"
        ac.ensure %condition, "topology"
        ac.return
      }
    }
  )mlir",
                                                      &context);
  ASSERT_TRUE(file);
  ASSERT_TRUE(mlir::succeeded(mlir::verify(file->getOperation())));
  auto module = *file->getOps<ModuleOp>().begin();
  for (mlir::Operation &operation : module.getBody().front()) {
    if (!mlir::isa<RequireOp, EnsureOp>(operation))
      continue;
    llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
    mlir::cast<mlir::MemoryEffectOpInterface>(&operation).getEffects(effects);
    ASSERT_EQ(effects.size(), 1u);
    EXPECT_EQ(effects.front().getResource(), ModuleStateResource::get());
    EXPECT_TRUE(
        mlir::isa<mlir::MemoryEffects::Read>(effects.front().getEffect()));
    auto parameters =
        mlir::cast<mlir::DictionaryAttr>(effects.front().getParameters());
    EXPECT_EQ(parameters.getAs<mlir::StringAttr>("contract_phase").getValue(),
              "topology_freeze");
  }
}

TEST(ACIROpsTest, ExplicitViewProvenanceScalesNearLinearly) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto emptyType = builder.getFunctionType({}, {});
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto zeroShape = builder.getDenseI64ArrayAttr({0});

  auto buildChain = [&](unsigned viewCount, llvm::StringRef prefix) {
    auto file = mlir::ModuleOp::create(loc);
    builder.setInsertionPointToStart(file.getBody());
    auto leaf = ModuleOp::create(builder, loc, (prefix + "Leaf").str(),
                                 emptyType, emptyDictionary);
    builder.setInsertionPointToStart(leaf.addEntryBlock());
    ReturnOp::create(builder, loc, mlir::ValueRange{});
    builder.setInsertionPointToEnd(file.getBody());
    auto top = ModuleOp::create(builder, loc, (prefix + "Top").str(), emptyType,
                                emptyDictionary);
    builder.setInsertionPointToStart(top.addEntryBlock());
    std::string previous = "source";
    InstanceOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{},
                       (prefix + "Leaf").str(), previous, previous, previous,
                       emptyDictionary);
    for (unsigned index = 0; index != viewCount; ++index) {
      std::string name = "view" + std::to_string(index);
      ViewOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, name,
                     "permutation",
                     builder.getArrayAttr(
                         {mlir::FlatSymbolRefAttr::get(&context, previous)}),
                     builder.getArrayAttr({zeroShape}), mlir::IntegerAttr(),
                     llvm::ArrayRef<int64_t>{}, llvm::ArrayRef<int64_t>{0});
      previous = std::move(name);
    }
    ReturnOp::create(builder, loc, mlir::ValueRange{});
    return file;
  };

  auto buildWide = [&] {
    constexpr unsigned sourceCount = 1000;
    auto file = mlir::ModuleOp::create(loc);
    builder.setInsertionPointToStart(file.getBody());
    auto leaf =
        ModuleOp::create(builder, loc, "WideLeaf", emptyType, emptyDictionary);
    builder.setInsertionPointToStart(leaf.addEntryBlock());
    ReturnOp::create(builder, loc, mlir::ValueRange{});
    builder.setInsertionPointToEnd(file.getBody());
    auto top =
        ModuleOp::create(builder, loc, "WideTop", emptyType, emptyDictionary);
    builder.setInsertionPointToStart(top.addEntryBlock());
    llvm::SmallVector<mlir::Attribute> producerRefs;
    llvm::SmallVector<mlir::Attribute> sourceShapes;
    producerRefs.reserve(sourceCount);
    sourceShapes.reserve(sourceCount);
    for (unsigned index = 0; index != sourceCount; ++index) {
      std::string name = "source" + std::to_string(index);
      InstanceOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{},
                         "WideLeaf", name, name, name, emptyDictionary);
      producerRefs.push_back(mlir::FlatSymbolRefAttr::get(&context, name));
      sourceShapes.push_back(zeroShape);
    }
    ViewOp::create(builder, loc, mlir::TypeRange{}, mlir::ValueRange{}, "wide",
                   "concat", builder.getArrayAttr(producerRefs),
                   builder.getArrayAttr(sourceShapes),
                   builder.getI64IntegerAttr(0), llvm::ArrayRef<int64_t>{},
                   llvm::ArrayRef<int64_t>{0});
    ReturnOp::create(builder, loc, mlir::ValueRange{});
    return file;
  };

  auto wide = buildWide();
  auto small = buildChain(1000, "Small");
  auto large = buildChain(5000, "Large");
  auto verifyTimed = [](mlir::ModuleOp file) {
    auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(mlir::succeeded(mlir::verify(file)));
    return std::chrono::steady_clock::now() - start;
  };
  auto wideElapsed = verifyTimed(wide);
  auto smallElapsed = verifyTimed(small);
  auto largeElapsed = verifyTimed(large);
  EXPECT_LT(wideElapsed, std::chrono::seconds(5));
  EXPECT_LT(largeElapsed, std::chrono::seconds(5));
  EXPECT_LT(largeElapsed, smallElapsed * 8 + std::chrono::milliseconds(50));
}

TEST(ACIROpsTest, TopologyVerifierWalksTypeAttributesAndLocations) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::ScopedDiagnosticHandler handler(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });
  mlir::OpBuilder builder(&context);
  auto protocol = mlir::FlatSymbolRefAttr::get(&context, "missing");
  auto flow = FlowType::get(&context, builder.getI8Type(), protocol);
  auto nested = OptionalType::get(&context, flow);

  auto attributeModule = mlir::ModuleOp::create(builder.getUnknownLoc());
  attributeModule->setAttr("metadata", mlir::TypeAttr::get(nested));
  EXPECT_TRUE(mlir::failed(verifyTopologyTypeUses(attributeModule)));

  auto operandModule = mlir::ModuleOp::create(builder.getUnknownLoc());
  builder.setInsertionPointToStart(operandModule.getBody());
  auto source = mlir::UnrealizedConversionCastOp::create(
      builder, builder.getUnknownLoc(), mlir::TypeRange{nested},
      mlir::ValueRange{});
  auto consumer = mlir::UnrealizedConversionCastOp::create(
      builder, builder.getUnknownLoc(), mlir::TypeRange{builder.getI1Type()},
      source.getResults());
  EXPECT_TRUE(mlir::failed(verifyTopologyTypeUses(consumer)));

  auto location = mlir::FusedLoc::get(&context, {builder.getUnknownLoc()},
                                      mlir::TypeAttr::get(nested));
  auto locationModule = mlir::ModuleOp::create(location);
  EXPECT_TRUE(mlir::failed(verifyTopologyTypeUses(locationModule)));
}

TEST(ACIROpsTest, ReverseChainOwnershipAnalysisHasLinearScaling) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto loc = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(loc);
  builder.setInsertionPointToStart(module.getBody());
  auto protocol = ProtocolOp::create(builder, loc, "long_chain");
  builder.setInsertionPointToStart(&protocol.getBody().emplaceBlock());
  RoleOp::create(builder, loc, "a", "b", "exclusive");
  RoleOp::create(builder, loc, "b", "a", "exclusive");

  constexpr unsigned stateCount = 1201;
  std::vector<std::string> stateNames;
  stateNames.reserve(stateCount);
  for (unsigned index = 0; index < stateCount; ++index) {
    stateNames.push_back((llvm::Twine("s") + llvm::Twine(index)).str());
    StateOp::create(builder, loc, stateNames.back(), index == 0,
                    index + 1 == stateCount);
  }
  EventOp::create(builder, loc, "step", "a", "b", builder.getI8Type(),
                  "notify");
  for (unsigned index = stateCount - 1; index > 0; --index) {
    auto transition =
        TransitionOp::create(builder, loc, stateNames[index - 1],
                             stateNames[index], "step", nullptr, false, false);
    transition.getGuard().emplaceBlock();
  }

  auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(mlir::succeeded(mlir::verify(module)));
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(ACIROpsTest, TransitionTableRejectsAmbiguousRowsDeterministically) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  constexpr llvm::StringLiteral source = R"mlir(
    builtin.module {
      "ac.protocol"() <{sym_name = "p"}> ({
        "ac.role"() <{sym_name = "a", dual = @b, cardinality = "exclusive"}> : () -> ()
        "ac.role"() <{sym_name = "b", dual = @a, cardinality = "exclusive"}> : () -> ()
        "ac.state"() <{sym_name = "s", initial = true, terminal = false}> : () -> ()
        "ac.event"() <{sym_name = "e", from = @a, to = @b, payload = i8, action = "notify"}> : () -> ()
        "ac.transition"() <{source = @s, target = @s, event = @e}> ({}) : () -> ()
        "ac.transition"() <{source = @s, target = @s, event = @e}> ({}) : () -> ()
      }) : () -> ()
    }
  )mlir";
  std::string diagnostic;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &value) {
    llvm::raw_string_ostream(diagnostic) << value;
    return mlir::success();
  });
  EXPECT_FALSE(mlir::parseSourceString<mlir::ModuleOp>(source, &context));
  EXPECT_NE(
      diagnostic.find("overlapping transitions require explicit priority"),
      std::string::npos);
}

TEST(ACIROpsTest, TableEntryTypeRejectsNonStructRecordKindsAndNesting) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect, mlir::DLTIDialect>();
  constexpr std::array<llvm::StringLiteral, 3> sources = {
      R"mlir(
        builtin.module attributes {ac.contract_epoch = "0.5"} {
          "ac.type_scope"() <{sym_name = "types"}> ({
            "ac.packet"() <{sym_name = "Entry", fields = [{name = "value", type = i8}]}> : () -> ()
          }) {dlti.dl_spec = #dlti.dl_spec<!ac.packet<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, serialization_width = 1 : i64, size = 1 : i64}>} : () -> ()
          ac.table @bad entry !ac.packet<@types::@Entry> entries 4 init 0 owner "/" stable_id "table/bad"
        }
      )mlir",
      R"mlir(
        builtin.module attributes {ac.contract_epoch = "0.5"} {
          "ac.type_scope"() <{sym_name = "types"}> ({
            "ac.transaction"() <{sym_name = "Entry", fields = [{name = "value", type = i8}]}> : () -> ()
          }) : () -> ()
          ac.table @bad entry !ac.transaction<@types::@Entry> entries 4 init 0 owner "/" stable_id "table/bad"
        }
      )mlir",
      R"mlir(
        builtin.module attributes {ac.contract_epoch = "0.5"} {
          ac.type_scope @types {
            ac.struct @Inner fields [{name = "value", type = i8}]
            ac.struct @Entry fields [{name = "inner", type = !ac.struct<@types::@Inner>}]
          } {dlti.dl_spec = #dlti.dl_spec<
            !ac.struct<@types::@Inner> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64},
            !ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}
          >}
          ac.table @bad entry !ac.struct<@types::@Entry> entries 4 init 0 owner "/" stable_id "table/bad"
        }
      )mlir"};

  for (llvm::StringRef source : sources) {
    std::string diagnostic;
    mlir::ScopedDiagnosticHandler handler(
        &context, [&](mlir::Diagnostic &value) {
          llvm::raw_string_ostream(diagnostic) << value;
          return mlir::success();
        });
    EXPECT_FALSE(mlir::parseSourceString<mlir::ModuleOp>(source, &context));
    EXPECT_NE(diagnostic.find("entry type must be bool, a <=64-bit integer, "
                              "or a flat integer struct"),
              std::string::npos)
        << diagnostic;
  }
}

TEST(ACIRResourcesTest, CheckedArithmeticAndIntervalsRejectBoundaries) {
  uint64_t result = 0;
  EXPECT_TRUE(checkedAdd(4, 5, result));
  EXPECT_EQ(result, 9u);
  EXPECT_FALSE(checkedAdd(std::numeric_limits<uint64_t>::max(), 1, result));
  EXPECT_TRUE(checkedMultiply(7, 6, result));
  EXPECT_EQ(result, 42u);
  EXPECT_FALSE(
      checkedMultiply(std::numeric_limits<uint64_t>::max(), 2, result));
  EXPECT_TRUE(intervalsOverlap({0, 8}, {7, 9}));
  EXPECT_FALSE(intervalsOverlap({0, 8}, {8, 9}));
  EXPECT_TRUE(intervalsOverlap(
      {uint64_t{1} << 63, WideAddress{1} << 64},
      {std::numeric_limits<uint64_t>::max(), WideAddress{1} << 64}));
  EXPECT_EQ(compareAddressMapOrder({0, 8, true, 2}, {0, 4, true, 1}), -1);
}

TEST(ACIRResourcesTest, RationalNormalizationIsExactAndBounded) {
  uint64_t ticks = 0;
  EXPECT_TRUE(normalizeRationalToTicks(3, 2, 1, 2, ticks));
  EXPECT_EQ(ticks, 3u);
  EXPECT_FALSE(normalizeRationalToTicks(1, 3, 1, 2, ticks));
  EXPECT_FALSE(normalizeRationalToTicks(1, 0, 1, 1, ticks));
  EXPECT_FALSE(normalizeRationalToTicks(std::numeric_limits<uint64_t>::max(), 1,
                                        1, 2, ticks));
  EXPECT_TRUE(normalizeRationalToTicks(0, 1, 1, 1, ticks));
  EXPECT_EQ(ticks, 0u);
  EXPECT_TRUE(normalizeRationalToTicks(std::numeric_limits<int64_t>::max(), 1,
                                       1, 1, ticks));
  EXPECT_EQ(ticks, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
  EXPECT_FALSE(normalizeRationalToTicks(
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1, 1, 1, 1,
      ticks));
  EXPECT_TRUE(normalizeRationalToTicks(1, 1, 1, kMaxTickScale, ticks));
  EXPECT_EQ(ticks, kMaxTickScale);
  EXPECT_FALSE(normalizeRationalToTicks(1, 1, 1, kMaxTickScale + 1, ticks));
  EXPECT_FALSE(normalizeRationalToTicks(1, kMaxTickScale + 1, 1,
                                        kMaxTickScale + 1, ticks));
  EXPECT_TRUE(normalizeRationalToTicks(3, 2, 3, 4, ticks));
  EXPECT_EQ(ticks, 2u);
  EXPECT_FALSE(normalizeRationalToTicks(std::numeric_limits<uint64_t>::max(), 1,
                                        1, 2, ticks));
}

TEST(ACIRResourcesTest, DomainTickUsesNormativePhasePlusCycleTimesPeriod) {
  uint64_t tick = 0;
  EXPECT_TRUE(
      checkedDomainTick(std::numeric_limits<int64_t>::max(), 1, 0, tick));
  EXPECT_EQ(tick, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
  EXPECT_FALSE(
      checkedDomainTick(std::numeric_limits<int64_t>::max(), 1, 1, tick));
}

TEST(ACIRResourcesTest, TaskSevenRegistryDeltaIsExactlySixOperations) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  const std::array<llvm::StringLiteral, 6> names = {
      "ac.queue",         "ac.event_queue", "ac.resource",
      "ac.address_space", "ac.address_map", "ac.time_domain",
  };
  for (llvm::StringLiteral name : names)
    EXPECT_TRUE(mlir::OperationName(name, &context).isRegistered())
        << name.str();
  EXPECT_FALSE(mlir::OperationName("ac.freeze", &context).isRegistered());
  EXPECT_FALSE(mlir::OperationName("ac.try_issue", &context).isRegistered());
}

TEST(ACIRResourcesTest, PublicBuildersAndTypedEffectsCoverAllSixOperations) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto file = mlir::ModuleOp::create(location);
  builder.setInsertionPointToStart(file.getBody());
  auto module =
      ModuleOp::create(builder, location, "M", builder.getFunctionType({}, {}),
                       builder.getDictionaryAttr({}));
  builder.setInsertionPointToStart(module.addEntryBlock());

  auto i64 = [&](int64_t value) { return builder.getI64IntegerAttr(value); };
  auto string = [&](llvm::StringRef value) {
    return builder.getStringAttr(value);
  };
  auto symbol = [&](llvm::StringRef value) {
    return mlir::FlatSymbolRefAttr::get(&context, value);
  };
  auto queue =
      QueueOp::create(builder, location, string("q"), string("q"), string("q"),
                      mlir::TypeAttr::get(builder.getI32Type()), i64(8),
                      mlir::IntegerAttr(), string("fifo"), symbol("p"),
                      string("exclusive"), mlir::DictionaryAttr(), i64(1));
  auto domain = TimeDomainOp::create(builder, location, string("clock"), i64(1),
                                     i64(0), i64(1), mlir::FlatSymbolRefAttr(),
                                     mlir::DictionaryAttr());
  auto eventQueue = EventQueueOp::create(
      builder, location, string("events"), string("events"), string("events"),
      mlir::TypeAttr::get(EventType::get(&context, builder.getI32Type())),
      i64(8), string("time_then_sequence"), symbol("clock"), i64(1));
  auto latency = builder.getDictionaryAttr({
      builder.getNamedAttr("kind", string("fixed")),
      builder.getNamedAttr("ticks", i64(2)),
  });
  auto lifecycle = builder.getDictionaryAttr({
      builder.getNamedAttr("reservation", string("propose_commit")),
      builder.getNamedAttr("release", string("balanced")),
      builder.getNamedAttr("cancellation", string("explicit")),
  });
  auto resource = ResourceOp::create(
      builder, location, string("r"), string("r"), string("r"), i64(2), i64(1),
      i64(1), latency, lifecycle, string("exclusive"),
      mlir::FlatSymbolRefAttr(), builder.getArrayAttr({}), i64(1));
  auto address = AddressSpaceOp::create(
      builder, location, string("memory"), string("memory"), string("memory"),
      i64(32), string("byte"), mlir::Attribute(), mlir::FlatSymbolRefAttr(),
      mlir::DictionaryAttr());
  auto addressMap =
      AddressMapOp::create(builder, location, string("map"), symbol("memory"),
                           builder.getArrayAttr({}),
                           builder.getDictionaryAttr({
                               builder.getNamedAttr("kind", string("unmapped")),
                           }));
  ReturnOp::create(builder, location, mlir::ValueRange{});

  EXPECT_TRUE(queue && eventQueue && resource && address && addressMap &&
              domain);
  auto hasWriteOn = [](mlir::Operation *operation,
                       mlir::SideEffects::Resource *resourceKind) {
    llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
    mlir::cast<mlir::MemoryEffectOpInterface>(operation).getEffects(effects);
    return llvm::any_of(effects, [&](const auto &effect) {
      return mlir::isa<mlir::MemoryEffects::Write>(effect.getEffect()) &&
             effect.getResource() == resourceKind;
    });
  };
  EXPECT_TRUE(hasWriteOn(queue, QueueStateResource::get()));
  EXPECT_TRUE(hasWriteOn(eventQueue, EventQueueStateResource::get()));
  EXPECT_TRUE(hasWriteOn(resource, ReservationStateResource::get()));
  EXPECT_FALSE(mlir::isMemoryEffectFree(queue));
  EXPECT_FALSE(mlir::isMemoryEffectFree(eventQueue));
  EXPECT_FALSE(mlir::isMemoryEffectFree(resource));

  auto effectOf = [](mlir::Operation *operation) {
    llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
    mlir::cast<mlir::MemoryEffectOpInterface>(operation).getEffects(effects);
    EXPECT_EQ(effects.size(), 1u);
    return effects.front();
  };
  auto queueEffect = effectOf(queue);
  auto eventEffect = effectOf(eventQueue);
  auto queueEffectAgain = effectOf(queue);
  auto qualified = [&](llvm::StringRef local) {
    return mlir::SymbolRefAttr::get(
        &context, "M", {mlir::FlatSymbolRefAttr::get(&context, local)});
  };
  EXPECT_EQ(queueEffect.getSymbolRef(), qualified("q"));
  EXPECT_EQ(eventEffect.getSymbolRef(), qualified("events"));
  EXPECT_EQ(queueEffect.getSymbolRef(), queueEffectAgain.getSymbolRef());
  EXPECT_EQ(queueEffect.getParameters(), queueEffectAgain.getParameters());
  EXPECT_NE(queueEffect.getSymbolRef(), eventEffect.getSymbolRef());
  auto parameters =
      mlir::cast<mlir::DictionaryAttr>(queueEffect.getParameters());
  EXPECT_EQ(parameters.getAs<mlir::StringAttr>("stable_id").getValue(), "q");
  EXPECT_EQ(parameters.getAs<mlir::StringAttr>("path").getValue(), "q");
}

TEST(ACIRResourcesTest, EffectsUseDefinitionQualifiedPreFreezeIdentity) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto file = mlir::ModuleOp::create(location);
  auto buildQueue = [&](llvm::StringRef definition) {
    builder.setInsertionPointToEnd(file.getBody());
    auto module = ModuleOp::create(builder, location, definition,
                                   builder.getFunctionType({}, {}),
                                   builder.getDictionaryAttr({}));
    builder.setInsertionPointToStart(module.addEntryBlock());
    auto queue = QueueOp::create(
        builder, location, builder.getStringAttr("q"),
        builder.getStringAttr("q"), builder.getStringAttr("q"),
        mlir::TypeAttr::get(builder.getI32Type()), builder.getI64IntegerAttr(1),
        mlir::IntegerAttr(), builder.getStringAttr("fifo"),
        mlir::FlatSymbolRefAttr::get(&context, "p"),
        builder.getStringAttr("exclusive"), mlir::DictionaryAttr(),
        builder.getI64IntegerAttr(1));
    ReturnOp::create(builder, location, mlir::ValueRange{});
    return queue;
  };
  QueueOp left = buildQueue("Left");
  QueueOp right = buildQueue("Right");
  auto effectOf = [](QueueOp queue) {
    llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
    mlir::cast<mlir::MemoryEffectOpInterface>(*queue).getEffects(effects);
    EXPECT_EQ(effects.size(), 1u);
    return effects.front();
  };
  auto leftEffect = effectOf(left);
  auto leftAgain = effectOf(left);
  auto rightEffect = effectOf(right);
  auto qualified = [&](llvm::StringRef definition) {
    return mlir::SymbolRefAttr::get(
        &context, definition, {mlir::FlatSymbolRefAttr::get(&context, "q")});
  };
  EXPECT_EQ(leftEffect.getSymbolRef(), qualified("Left"));
  EXPECT_EQ(rightEffect.getSymbolRef(), qualified("Right"));
  EXPECT_NE(leftEffect.getSymbolRef(), rightEffect.getSymbolRef());
  EXPECT_EQ(leftEffect.getSymbolRef(), leftAgain.getSymbolRef());
  EXPECT_EQ(leftEffect.getParameters(), leftAgain.getParameters());
  auto parameters =
      mlir::cast<mlir::DictionaryAttr>(leftEffect.getParameters());
  auto identityPhase = parameters.getAs<mlir::StringAttr>("identity_phase");
  ASSERT_TRUE(identityPhase);
  EXPECT_EQ(identityPhase.getValue(), "definition_pre_freeze");
}

TEST(ACIRResourcesTest, LargeAddressMapAndParentGraphScaleNearLinearly) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto emptyDictionary = builder.getDictionaryAttr({});
  auto file = mlir::ModuleOp::create(location);
  builder.setInsertionPointToStart(file.getBody());
  auto module = ModuleOp::create(
      builder, location, "M", builder.getFunctionType({}, {}), emptyDictionary);
  builder.setInsertionPointToStart(module.addEntryBlock());
  AddressSpaceOp::create(
      builder, location, builder.getStringAttr("space"),
      builder.getStringAttr("space"), builder.getStringAttr("space"),
      builder.getI64IntegerAttr(32), builder.getStringAttr("byte"),
      mlir::Attribute(), mlir::FlatSymbolRefAttr(), mlir::DictionaryAttr());

  constexpr unsigned entryCount = 10000;
  llvm::SmallVector<mlir::Attribute> entries;
  entries.reserve(entryCount);
  for (unsigned index = 0; index != entryCount; ++index) {
    entries.push_back(builder.getDictionaryAttr({
        builder.getNamedAttr("base", builder.getI64IntegerAttr(0)),
        builder.getNamedAttr("size", builder.getI64IntegerAttr(entryCount)),
        builder.getNamedAttr("target",
                             mlir::FlatSymbolRefAttr::get(&context, "space")),
        builder.getNamedAttr("offset", builder.getI64IntegerAttr(0)),
        builder.getNamedAttr(
            "permissions",
            builder.getArrayAttr({builder.getStringAttr("read")})),
        builder.getNamedAttr("classes", builder.getArrayAttr({})),
        builder.getNamedAttr(
            "interleave",
            builder.getDictionaryAttr({
                builder.getNamedAttr("granularity",
                                     builder.getI64IntegerAttr(1)),
                builder.getNamedAttr("banks",
                                     builder.getI64IntegerAttr(entryCount)),
                builder.getNamedAttr("bank", builder.getI64IntegerAttr(index)),
            })),
    }));
  }
  AddressMapOp::create(
      builder, location, "map", "space", builder.getArrayAttr(entries),
      builder.getDictionaryAttr(
          {builder.getNamedAttr("kind", builder.getStringAttr("unmapped"))}));
  ReturnOp::create(builder, location, mlir::ValueRange{});

  auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(mlir::succeeded(mlir::verify(file)));
  auto verifyElapsed = std::chrono::steady_clock::now() - start;

  constexpr unsigned domainCount = 10000;
  auto graphFile = mlir::ModuleOp::create(location);
  builder.setInsertionPointToStart(graphFile.getBody());
  auto bridgeModule =
      ModuleOp::create(builder, location, "Bridge",
                       builder.getFunctionType({}, {}), emptyDictionary);
  builder.setInsertionPointToStart(bridgeModule.addEntryBlock());
  ReturnOp::create(builder, location, mlir::ValueRange{});
  builder.setInsertionPointToEnd(graphFile.getBody());
  auto graphModule =
      ModuleOp::create(builder, location, "Graph",
                       builder.getFunctionType({}, {}), emptyDictionary);
  builder.setInsertionPointToStart(graphModule.addEntryBlock());
  InstanceOp::create(builder, location, mlir::TypeRange{}, mlir::ValueRange{},
                     "Bridge", "bridge", "bridge", "bridge", emptyDictionary);
  for (unsigned index = 0; index != domainCount; ++index) {
    std::string name = "d" + std::to_string(index);
    mlir::FlatSymbolRefAttr parent;
    mlir::DictionaryAttr bridge;
    if (index) {
      parent = mlir::FlatSymbolRefAttr::get(&context,
                                            "d" + std::to_string(index - 1));
      bridge = builder.getDictionaryAttr({
          builder.getNamedAttr("kind", builder.getStringAttr("explicit")),
          builder.getNamedAttr(
              "owner", mlir::FlatSymbolRefAttr::get(&context, "bridge")),
      });
    }
    TimeDomainOp::create(builder, location, builder.getStringAttr(name),
                         builder.getI64IntegerAttr(1),
                         builder.getI64IntegerAttr(0),
                         builder.getI64IntegerAttr(1), parent, bridge);
  }
  ReturnOp::create(builder, location, mlir::ValueRange{});

  start = std::chrono::steady_clock::now();
  EXPECT_TRUE(mlir::succeeded(mlir::verify(graphFile)));
  auto graphElapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(verifyElapsed, std::chrono::seconds(5));
  EXPECT_LT(graphElapsed, std::chrono::seconds(5));
}

TEST(ACIRResourcesTest, MixedGeometryDistinctPrioritiesScaleNearLinearly) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto buildMap = [&](unsigned entryCount) {
    auto file = mlir::ModuleOp::create(location);
    builder.setInsertionPointToStart(file.getBody());
    auto module = ModuleOp::create(builder, location, "M",
                                   builder.getFunctionType({}, {}),
                                   builder.getDictionaryAttr({}));
    builder.setInsertionPointToStart(module.addEntryBlock());
    AddressSpaceOp::create(
        builder, location, builder.getStringAttr("space"),
        builder.getStringAttr("space"), builder.getStringAttr("space"),
        builder.getI64IntegerAttr(32), builder.getStringAttr("byte"),
        mlir::Attribute(), mlir::FlatSymbolRefAttr(), mlir::DictionaryAttr());
    llvm::SmallVector<mlir::Attribute> entries;
    entries.reserve(entryCount);
    for (uint64_t priority = 1; priority <= entryCount; ++priority) {
      entries.push_back(builder.getDictionaryAttr({
          builder.getNamedAttr("base", builder.getI64IntegerAttr(0)),
          builder.getNamedAttr("size", builder.getI64IntegerAttr(priority)),
          builder.getNamedAttr("target",
                               mlir::FlatSymbolRefAttr::get(&context, "space")),
          builder.getNamedAttr("offset", builder.getI64IntegerAttr(0)),
          builder.getNamedAttr(
              "permissions",
              builder.getArrayAttr({builder.getStringAttr("read")})),
          builder.getNamedAttr("classes", builder.getArrayAttr({})),
          builder.getNamedAttr("priority", builder.getI64IntegerAttr(priority)),
          builder.getNamedAttr(
              "interleave",
              builder.getDictionaryAttr({
                  builder.getNamedAttr("granularity",
                                       builder.getI64IntegerAttr(1)),
                  builder.getNamedAttr("banks",
                                       builder.getI64IntegerAttr(priority)),
                  builder.getNamedAttr("bank", builder.getI64IntegerAttr(0)),
              })),
      }));
    }
    AddressMapOp::create(
        builder, location, "map", "space", builder.getArrayAttr(entries),
        builder.getDictionaryAttr(
            {builder.getNamedAttr("kind", builder.getStringAttr("unmapped"))}));
    ReturnOp::create(builder, location, mlir::ValueRange{});
    return file;
  };
  auto verifyTimed = [&](unsigned entryCount) {
    mlir::ModuleOp file = buildMap(entryCount);
    auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(mlir::succeeded(mlir::verify(file)));
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
  };

  int64_t fiveThousandMs = verifyTimed(5000);
  int64_t tenThousandMs = verifyTimed(10000);
  RecordProperty("five_thousand_ms", fiveThousandMs);
  RecordProperty("ten_thousand_ms", tenThousandMs);
  EXPECT_LT(tenThousandMs, 5000);
  EXPECT_LT(tenThousandMs, std::max<int64_t>(100, fiveThousandMs * 3));
}

TEST(ACIRResourcesTest, SingleSelectedStripeMixedGeometriesScaleNearLinearly) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto measureWork = [&](unsigned entryCount) {
    auto file = mlir::ModuleOp::create(location);
    builder.setInsertionPointToStart(file.getBody());
    auto module = ModuleOp::create(builder, location, "M",
                                   builder.getFunctionType({}, {}),
                                   builder.getDictionaryAttr({}));
    builder.setInsertionPointToStart(module.addEntryBlock());
    AddressSpaceOp::create(
        builder, location, builder.getStringAttr("space"),
        builder.getStringAttr("space"), builder.getStringAttr("space"),
        builder.getI64IntegerAttr(32), builder.getStringAttr("byte"),
        mlir::Attribute(), mlir::FlatSymbolRefAttr(), mlir::DictionaryAttr());
    llvm::SmallVector<mlir::Attribute> entries;
    entries.reserve(entryCount);
    for (uint64_t index = 1; index <= entryCount; ++index) {
      uint64_t size = 16384 * index;
      entries.push_back(builder.getDictionaryAttr({
          builder.getNamedAttr("base", builder.getI64IntegerAttr(0)),
          builder.getNamedAttr("size", builder.getI64IntegerAttr(size)),
          builder.getNamedAttr("target",
                               mlir::FlatSymbolRefAttr::get(&context, "space")),
          builder.getNamedAttr("offset", builder.getI64IntegerAttr(0)),
          builder.getNamedAttr(
              "permissions",
              builder.getArrayAttr({builder.getStringAttr("read")})),
          builder.getNamedAttr("classes", builder.getArrayAttr({})),
          builder.getNamedAttr(
              "interleave",
              builder.getDictionaryAttr({
                  builder.getNamedAttr("granularity",
                                       builder.getI64IntegerAttr(1)),
                  builder.getNamedAttr("banks",
                                       builder.getI64IntegerAttr(size)),
                  builder.getNamedAttr("bank",
                                       builder.getI64IntegerAttr(index - 1)),
              })),
      }));
    }
    AddressMapOp::create(
        builder, location, "map", "space", builder.getArrayAttr(entries),
        builder.getDictionaryAttr(
            {builder.getNamedAttr("kind", builder.getStringAttr("unmapped"))}));
    ReturnOp::create(builder, location, mlir::ValueRange{});
    detail::AddressMapVerificationWork work;
    {
      detail::ScopedAddressMapVerificationWorkCollector collector(work);
      EXPECT_TRUE(mlir::succeeded(mlir::verify(file)));
    }
    return work;
  };

  auto expectSingleStripeWork =
      [](const detail::AddressMapVerificationWork &work, uint64_t entryCount) {
        // Each entry is parsed and swept once. Adjacent one-address selections
        // expire N-1 prior entries; the concrete and mixed sweeps make two
        // queries per entry and six key updates per entry minus the two absent
        // first-entry removals. This reaches no candidate or general relation.
        EXPECT_EQ(work.entryNormalizationVisits, entryCount);
        EXPECT_EQ(work.concreteEntryVisits, entryCount);
        EXPECT_EQ(work.concreteExpirationVisits, entryCount - 1);
        EXPECT_EQ(work.selectorQueryVisits, entryCount * 2);
        EXPECT_EQ(work.selectorUpdateVisits, entryCount * 6 - 2);
        EXPECT_EQ(work.candidateIntersectionChecks, 0u);
        EXPECT_EQ(work.generalRelationChecks, 0u);
        EXPECT_EQ(work.total(), entryCount * 11 - 3);
      };

  constexpr uint64_t smallSize = 5000;
  constexpr uint64_t largeSize = 10000;
  detail::AddressMapVerificationWork smallWork = measureWork(smallSize);
  detail::AddressMapVerificationWork largeWork = measureWork(largeSize);
  expectSingleStripeWork(smallWork, smallSize);
  expectSingleStripeWork(largeWork, largeSize);
  EXPECT_EQ(largeWork.total() - smallWork.total(),
            uint64_t{11} * (largeSize - smallSize));
}

TEST(ACIRResourcesTest,
     SingleSelectionHandlesPartialEmptyAndFullWidthIntervals) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto integer = [&](uint64_t value) {
    return builder.getIntegerAttr(builder.getI64Type(), llvm::APInt(64, value));
  };
  auto entry = [&](uint64_t base, uint64_t size, uint64_t granularity,
                   uint64_t banks, uint64_t bank) {
    return builder.getDictionaryAttr({
        builder.getNamedAttr("base", integer(base)),
        builder.getNamedAttr("size", integer(size)),
        builder.getNamedAttr("target",
                             mlir::FlatSymbolRefAttr::get(&context, "space")),
        builder.getNamedAttr("offset", integer(0)),
        builder.getNamedAttr(
            "permissions",
            builder.getArrayAttr({builder.getStringAttr("read")})),
        builder.getNamedAttr("classes", builder.getArrayAttr({})),
        builder.getNamedAttr(
            "interleave",
            builder.getDictionaryAttr({
                builder.getNamedAttr("granularity", integer(granularity)),
                builder.getNamedAttr("banks", integer(banks)),
                builder.getNamedAttr("bank", integer(bank)),
            })),
    });
  };
  auto buildMap = [&](llvm::ArrayRef<mlir::Attribute> entries) {
    auto file = mlir::ModuleOp::create(location);
    builder.setInsertionPointToStart(file.getBody());
    auto module = ModuleOp::create(builder, location, "M",
                                   builder.getFunctionType({}, {}),
                                   builder.getDictionaryAttr({}));
    builder.setInsertionPointToStart(module.addEntryBlock());
    AddressSpaceOp::create(
        builder, location, builder.getStringAttr("space"),
        builder.getStringAttr("space"), builder.getStringAttr("space"),
        builder.getI64IntegerAttr(64), builder.getStringAttr("byte"),
        mlir::Attribute(), mlir::FlatSymbolRefAttr(), mlir::DictionaryAttr());
    AddressMapOp::create(
        builder, location, "map", "space", builder.getArrayAttr(entries),
        builder.getDictionaryAttr(
            {builder.getNamedAttr("kind", builder.getStringAttr("unmapped"))}));
    ReturnOp::create(builder, location, mlir::ValueRange{});
    return file;
  };

  auto valid = buildMap(
      {entry(3, 2, 4, 8, 1), entry(3, 2, 1, 7, 3), entry(3, 1, 1, 8, 7),
       entry(std::numeric_limits<uint64_t>::max(), 1, 1, 2, 1)});
  std::string before;
  llvm::raw_string_ostream(before) << valid;
  EXPECT_TRUE(mlir::succeeded(mlir::verify(valid)));
  std::string after;
  llvm::raw_string_ostream(after) << valid;
  EXPECT_EQ(before, after);
  normalizeAddressMaps(valid);
  std::string normalizedOnce;
  llvm::raw_string_ostream(normalizedOnce) << valid;
  normalizeAddressMaps(valid);
  std::string normalizedTwice;
  llvm::raw_string_ostream(normalizedTwice) << valid;
  EXPECT_EQ(normalizedOnce, normalizedTwice);

  auto generalFastDisjoint =
      buildMap({entry(0, 16, 1, 2, 0), entry(3, 2, 1, 7, 3)});
  EXPECT_TRUE(mlir::succeeded(mlir::verify(generalFastDisjoint)));
  auto clampForward =
      buildMap({entry(24, 62, 2, 7, 0), entry(48, 98, 8, 8, 5)});
  auto clampReverse =
      buildMap({entry(48, 98, 8, 8, 5), entry(24, 62, 2, 7, 0)});
  EXPECT_TRUE(mlir::succeeded(mlir::verify(clampForward)));
  EXPECT_TRUE(mlir::succeeded(mlir::verify(clampReverse)));

  mlir::ScopedDiagnosticHandler suppress(
      &context, [](mlir::Diagnostic &) { return mlir::success(); });
  auto overlapping = buildMap({entry(3, 2, 4, 8, 1), entry(3, 2, 1, 7, 4)});
  EXPECT_TRUE(mlir::failed(mlir::verify(overlapping)));
  auto generalFastOverlap =
      buildMap({entry(0, 16, 1, 2, 0), entry(3, 2, 1, 7, 4)});
  EXPECT_TRUE(mlir::failed(mlir::verify(generalFastOverlap)));
}

TEST(ACIRResourcesTest,
     GeneralMixedIntersectionCapabilityIsExactAndPermutationInvariant) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto buildMap = [&](uint64_t relationCount, bool reverse) {
    auto file = mlir::ModuleOp::create(location);
    builder.setInsertionPointToStart(file.getBody());
    auto module = ModuleOp::create(builder, location, "M",
                                   builder.getFunctionType({}, {}),
                                   builder.getDictionaryAttr({}));
    builder.setInsertionPointToStart(module.addEntryBlock());
    AddressSpaceOp::create(
        builder, location, builder.getStringAttr("space"),
        builder.getStringAttr("space"), builder.getStringAttr("space"),
        builder.getI64IntegerAttr(32), builder.getStringAttr("byte"),
        mlir::Attribute(), mlir::FlatSymbolRefAttr(), mlir::DictionaryAttr());
    auto makeEntry = [&](uint64_t banks, uint64_t bank,
                         std::optional<uint64_t> priority) {
      llvm::SmallVector<mlir::NamedAttribute> attributes{
          builder.getNamedAttr("base", builder.getI64IntegerAttr(0)),
          builder.getNamedAttr("size", builder.getI64IntegerAttr(65536)),
          builder.getNamedAttr("target",
                               mlir::FlatSymbolRefAttr::get(&context, "space")),
          builder.getNamedAttr("offset", builder.getI64IntegerAttr(0)),
          builder.getNamedAttr(
              "permissions",
              builder.getArrayAttr({builder.getStringAttr("read")})),
          builder.getNamedAttr("classes", builder.getArrayAttr({})),
          builder.getNamedAttr(
              "interleave",
              builder.getDictionaryAttr({
                  builder.getNamedAttr("granularity",
                                       builder.getI64IntegerAttr(1)),
                  builder.getNamedAttr("banks",
                                       builder.getI64IntegerAttr(banks)),
                  builder.getNamedAttr("bank", builder.getI64IntegerAttr(bank)),
              })),
      };
      if (priority)
        attributes.push_back(builder.getNamedAttr(
            "priority", builder.getI64IntegerAttr(*priority)));
      return builder.getDictionaryAttr(attributes);
    };
    llvm::SmallVector<mlir::Attribute> entries;
    entries.push_back(makeEntry(2, 0, std::nullopt));
    for (uint64_t index = 0; index < relationCount; ++index)
      entries.push_back(makeEntry(2 * (index + 2), 1, index + 1));
    if (reverse)
      std::reverse(entries.begin(), entries.end());
    AddressMapOp::create(
        builder, location, "map", "space", builder.getArrayAttr(entries),
        builder.getDictionaryAttr(
            {builder.getNamedAttr("kind", builder.getStringAttr("unmapped"))}));
    ReturnOp::create(builder, location, mlir::ValueRange{});
    return file;
  };
  auto verify = [&](mlir::ModuleOp file) {
    std::string diagnostic;
    mlir::ScopedDiagnosticHandler handler(
        &context, [&](mlir::Diagnostic &value) {
          llvm::raw_string_ostream(diagnostic) << value;
          return mlir::success();
        });
    bool succeeded = mlir::succeeded(mlir::verify(file));
    return std::pair{succeeded, diagnostic};
  };

  EXPECT_TRUE(
      verify(buildMap(kMaxGeneralSelectorIntersectionQueries - 1, false))
          .first);
  EXPECT_TRUE(
      verify(buildMap(kMaxGeneralSelectorIntersectionQueries, false)).first);
  auto forward =
      verify(buildMap(kMaxGeneralSelectorIntersectionQueries + 1, false));
  auto reverse =
      verify(buildMap(kMaxGeneralSelectorIntersectionQueries + 1, true));
  EXPECT_FALSE(forward.first);
  EXPECT_FALSE(reverse.first);
  EXPECT_EQ(forward.second, reverse.second);
  EXPECT_NE(forward.second.find(
                "general mixed interleave analysis exceeds ACIR limit 256"),
            std::string::npos);
}

TEST(ACIRResourcesTest, SingleBankSelectionsDoNotConsumeGeneralMixedBudget) {
  mlir::MLIRContext context;
  context.loadDialect<ACIRDialect>();
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto file = mlir::ModuleOp::create(location);
  builder.setInsertionPointToStart(file.getBody());
  auto module =
      ModuleOp::create(builder, location, "M", builder.getFunctionType({}, {}),
                       builder.getDictionaryAttr({}));
  builder.setInsertionPointToStart(module.addEntryBlock());
  AddressSpaceOp::create(
      builder, location, builder.getStringAttr("space"),
      builder.getStringAttr("space"), builder.getStringAttr("space"),
      builder.getI64IntegerAttr(12), builder.getStringAttr("byte"),
      mlir::Attribute(), mlir::FlatSymbolRefAttr(), mlir::DictionaryAttr());
  auto makeEntry = [&](uint64_t base, uint64_t size, uint64_t banks,
                       std::optional<uint64_t> priority) {
    llvm::SmallVector<mlir::NamedAttribute> attributes{
        builder.getNamedAttr("base", builder.getI64IntegerAttr(base)),
        builder.getNamedAttr("size", builder.getI64IntegerAttr(size)),
        builder.getNamedAttr("target",
                             mlir::FlatSymbolRefAttr::get(&context, "space")),
        builder.getNamedAttr("offset", builder.getI64IntegerAttr(0)),
        builder.getNamedAttr(
            "permissions",
            builder.getArrayAttr({builder.getStringAttr("read")})),
        builder.getNamedAttr("classes", builder.getArrayAttr({})),
        builder.getNamedAttr(
            "interleave",
            builder.getDictionaryAttr({
                builder.getNamedAttr("granularity",
                                     builder.getI64IntegerAttr(1)),
                builder.getNamedAttr("banks", builder.getI64IntegerAttr(banks)),
                builder.getNamedAttr("bank", builder.getI64IntegerAttr(0)),
            })),
    };
    if (priority)
      attributes.push_back(builder.getNamedAttr(
          "priority", builder.getI64IntegerAttr(*priority)));
    return builder.getDictionaryAttr(attributes);
  };
  llvm::SmallVector<mlir::Attribute> entries;
  entries.push_back(makeEntry(10, 10, 1, std::nullopt));
  for (uint64_t index = 0; index < 257; ++index)
    entries.push_back(makeEntry(0, 2000, 1000 + index, index + 1));
  AddressMapOp::create(
      builder, location, "map", "space", builder.getArrayAttr(entries),
      builder.getDictionaryAttr(
          {builder.getNamedAttr("kind", builder.getStringAttr("unmapped"))}));
  ReturnOp::create(builder, location, mlir::ValueRange{});
  EXPECT_TRUE(mlir::succeeded(mlir::verify(file)));
}

struct SelectorOracleLane {
  uint64_t base;
  uint64_t size;
  uint64_t granularity;
  uint64_t banks;
  uint64_t bank;
};

enum class SelectorOracleKind {
  FastFast,
  GeneralFast,
  GeneralGeneralSame,
  GeneralGeneralMixed,
};

struct SelectorOracleCase {
  SelectorOracleLane left;
  SelectorOracleLane right;
  bool reverse;
  SelectorOracleKind kind;
};

bool oracleSelects(const SelectorOracleLane &lane, uint64_t address) {
  if (address < lane.base || address >= lane.base + lane.size)
    return false;
  uint64_t cycle = lane.granularity * lane.banks;
  uint64_t within = address % cycle;
  uint64_t begin = lane.granularity * lane.bank;
  return begin <= within && within < begin + lane.granularity;
}

std::vector<SelectorOracleCase> makeSelectorOracleCases() {
  std::vector<SelectorOracleCase> cases;
  cases.reserve(1800);
  for (uint64_t index = 0; index < 450; ++index) {
    uint64_t leftBanks = 8 + index % 5;
    uint64_t rightBanks = 9 + index % 7;
    cases.push_back(
        {{index % 60, 2, 1, leftBanks, (index * 3) % leftBanks},
         {(index * 7) % 60, 2, 1, rightBanks, (index * 5) % rightBanks},
         index % 2 != 0,
         SelectorOracleKind::FastFast});

    uint64_t fastBanks = 8 + index % 9;
    cases.push_back({{0, 64, 1, 2, index % 2},
                     {index % 60, 3, 1, fastBanks, (index * 7) % fastBanks},
                     index % 2 != 0,
                     SelectorOracleKind::GeneralFast});

    uint64_t granularity = 1 + index % 3;
    uint64_t banks = 2 + index % 4;
    cases.push_back({{0, 64, granularity, banks, index % banks},
                     {0, 64, granularity, banks, (index * 3 + 1) % banks},
                     index % 2 != 0,
                     SelectorOracleKind::GeneralGeneralSame});

    uint64_t mixedBanks = 3 + index % 4;
    cases.push_back({{0, 64, 1, 2, index % 2},
                     {0, 64, 2, mixedBanks, (index * 5) % mixedBanks},
                     index % 2 != 0,
                     SelectorOracleKind::GeneralGeneralMixed});
  }
  return cases;
}

class SelectorOracleTest : public testing::TestWithParam<SelectorOracleCase> {
protected:
  static mlir::MLIRContext &context() {
    static mlir::MLIRContext value;
    static bool loaded = [] {
      value.loadDialect<ACIRDialect>();
      return true;
    }();
    (void)loaded;
    return value;
  }
};

TEST_P(SelectorOracleTest, MatchesManualSmallDomainEnumeration) {
  const SelectorOracleCase &testCase = GetParam();
  SCOPED_TRACE(testing::Message()
               << "kind=" << static_cast<unsigned>(testCase.kind)
               << " reverse=" << testCase.reverse);
  bool overlaps = false;
  for (uint64_t address = 0; address < 64; ++address)
    overlaps |= oracleSelects(testCase.left, address) &&
                oracleSelects(testCase.right, address);

  mlir::MLIRContext &mlirContext = context();
  mlir::OpBuilder builder(&mlirContext);
  auto location = builder.getUnknownLoc();
  auto file = mlir::ModuleOp::create(location);
  builder.setInsertionPointToStart(file.getBody());
  auto module =
      ModuleOp::create(builder, location, "M", builder.getFunctionType({}, {}),
                       builder.getDictionaryAttr({}));
  builder.setInsertionPointToStart(module.addEntryBlock());
  AddressSpaceOp::create(
      builder, location, builder.getStringAttr("space"),
      builder.getStringAttr("space"), builder.getStringAttr("space"),
      builder.getI64IntegerAttr(8), builder.getStringAttr("byte"),
      mlir::Attribute(), mlir::FlatSymbolRefAttr(), mlir::DictionaryAttr());
  auto makeEntry = [&](const SelectorOracleLane &lane) {
    return builder.getDictionaryAttr({
        builder.getNamedAttr("base", builder.getI64IntegerAttr(lane.base)),
        builder.getNamedAttr("size", builder.getI64IntegerAttr(lane.size)),
        builder.getNamedAttr(
            "target", mlir::FlatSymbolRefAttr::get(&mlirContext, "space")),
        builder.getNamedAttr("offset", builder.getI64IntegerAttr(0)),
        builder.getNamedAttr(
            "permissions",
            builder.getArrayAttr({builder.getStringAttr("read")})),
        builder.getNamedAttr("classes", builder.getArrayAttr({})),
        builder.getNamedAttr(
            "interleave",
            builder.getDictionaryAttr({
                builder.getNamedAttr(
                    "granularity", builder.getI64IntegerAttr(lane.granularity)),
                builder.getNamedAttr("banks",
                                     builder.getI64IntegerAttr(lane.banks)),
                builder.getNamedAttr("bank",
                                     builder.getI64IntegerAttr(lane.bank)),
            })),
    });
  };
  llvm::SmallVector<mlir::Attribute> entries{makeEntry(testCase.left),
                                             makeEntry(testCase.right)};
  if (testCase.reverse)
    std::reverse(entries.begin(), entries.end());
  AddressMapOp::create(
      builder, location, "map", "space", builder.getArrayAttr(entries),
      builder.getDictionaryAttr(
          {builder.getNamedAttr("kind", builder.getStringAttr("unmapped"))}));
  ReturnOp::create(builder, location, mlir::ValueRange{});
  mlir::ScopedDiagnosticHandler suppress(
      &mlirContext, [](mlir::Diagnostic &) { return mlir::success(); });
  EXPECT_EQ(mlir::succeeded(mlir::verify(file)), !overlaps);
}

std::string
selectorOracleName(const testing::TestParamInfo<SelectorOracleCase> &info) {
  const char *prefix = nullptr;
  switch (info.param.kind) {
  case SelectorOracleKind::FastFast:
    prefix = "FF";
    break;
  case SelectorOracleKind::GeneralFast:
    prefix = "GF";
    break;
  case SelectorOracleKind::GeneralGeneralSame:
    prefix = "GGSame";
    break;
  case SelectorOracleKind::GeneralGeneralMixed:
    prefix = "GGMixed";
    break;
  }
  return std::string(prefix) + "_" + std::to_string(info.index);
}

INSTANTIATE_TEST_SUITE_P(SmallDomain, SelectorOracleTest,
                         testing::ValuesIn(makeSelectorOracleCases()),
                         selectorOracleName);

TEST(ACIRFreezeEffectsTest, FrozenEffectsUseElaboratedAbsoluteOwnerSets) {
  mlir::DialectRegistry registry;
  acir::registerAllDialects(registry);
  mlir::MLIRContext context(registry);
  auto file = mlir::parseSourceString<mlir::ModuleOp>(R"mlir(
    builtin.module attributes {ac.contract_epoch = "0.5"} {
      ac.system @soc root @Top as "root" tick 0 "cycle"
          workload @Top::@workload seed {kind = "fixed", value = 0 : i64}
          instrumentation [] results {id = "default", format = "json"}
          selected true
      ac.module @Top() parameters {} graph {
        ac.process @workload kind "workload" { ac.yield_sim }
        ac.stat @requests kind "counter"
        ac.return
      }
    }
  )mlir",
                                                      &context);
  ASSERT_TRUE(file);
  mlir::PassManager manager(&context);
  manager.addPass(acir::createFreezeTopologyPass());
  ASSERT_TRUE(mlir::succeeded(manager.run(*file)));

  ProcessOp process;
  StatOp stat;
  file->walk([&](ProcessOp candidate) { process = candidate; });
  file->walk([&](StatOp candidate) { stat = candidate; });
  ASSERT_TRUE(process && stat);
  auto checkAbsoluteOwners = [](mlir::Operation *operation,
                                llvm::StringRef expectedPath) {
    llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
    mlir::cast<mlir::MemoryEffectOpInterface>(operation).getEffects(effects);
    ASSERT_FALSE(effects.empty());
    for (const mlir::MemoryEffects::EffectInstance &effect : effects) {
      auto parameters =
          mlir::cast<mlir::DictionaryAttr>(effect.getParameters());
      EXPECT_EQ(parameters.getAs<mlir::StringAttr>("identity_phase").getValue(),
                "elaborated_absolute");
      auto owners = parameters.getAs<mlir::ArrayAttr>("owners");
      ASSERT_TRUE(owners);
      ASSERT_EQ(owners.size(), 1u);
      EXPECT_EQ(mlir::cast<mlir::DictionaryAttr>(owners[0])
                    .getAs<mlir::StringAttr>("path")
                    .getValue(),
                expectedPath);
    }
  };
  checkAbsoluteOwners(&process.getBody().front().back(), "root.workload");
  checkAbsoluteOwners(stat, "root.requests");

  std::string diagnostic;
  mlir::ScopedDiagnosticHandler handler(&context, [&](mlir::Diagnostic &value) {
    llvm::raw_string_ostream stream(diagnostic);
    stream << value;
    return mlir::success();
  });
  stat->removeAttr("ac.frozen_owners");
  EXPECT_TRUE(mlir::failed(acir::verifyModel(*file)));
  EXPECT_NE(diagnostic.find("frozen topology digest mismatch"),
            std::string::npos);
}

} // namespace
} // namespace acir::ac
