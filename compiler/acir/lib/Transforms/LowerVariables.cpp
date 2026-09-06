#include "acir/Transforms/Passes.h"

#include "acir/Dialect/ACIR/ACIROps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/SymbolTable.h"

using namespace mlir;

namespace acir {
namespace {

ac::VarConstantOp createZeroIndex(OpBuilder &builder, Location location) {
  Type indexType = IntegerType::get(builder.getContext(), 1);
  OperationState state(location, ac::VarConstantOp::getOperationName());
  state.addTypes(ac::VarType::get(builder.getContext(), indexType));
  state.addAttribute("value", builder.getIntegerAttr(indexType, 0));
  return cast<ac::VarConstantOp>(builder.create(state));
}

ac::VarDeclOp resolveVariable(Operation *operation,
                              FlatSymbolRefAttr reference) {
  for (Operation *ancestor = operation->getParentOp(); ancestor;
       ancestor = ancestor->getParentOp()) {
    if (ancestor->getNumRegions() != 1 || !ancestor->getRegion(0).hasOneBlock())
      continue;
    for (ac::VarDeclOp variable :
         ancestor->getRegion(0).front().getOps<ac::VarDeclOp>())
      if (variable.getSymName() == reference.getValue())
        return variable;
  }
  return {};
}

FailureOr<ArrayAttr> completeWriteFields(OpBuilder &builder,
                                         ac::VarDeclOp variable) {
  auto structure = dyn_cast<ac::StructType>(variable.getValueType());
  if (!structure)
    return builder.getStrArrayAttr({"$entry"});
  Operation *declaration =
      SymbolTable::lookupNearestSymbolFrom(variable, structure.getName());
  auto fields = declaration ? declaration->getAttrOfType<ArrayAttr>("fields")
                            : ArrayAttr();
  if (!fields)
    return failure();
  SmallVector<StringRef> names;
  for (Attribute rawField : fields) {
    auto field = dyn_cast<DictionaryAttr>(rawField);
    auto name = field ? field.getAs<StringAttr>("name") : StringAttr();
    if (!name)
      return failure();
    names.push_back(name.getValue());
  }
  return builder.getStrArrayAttr(names);
}

void replaceYield(Operation *yield, StringRef operationName) {
  OpBuilder builder(yield);
  OperationState state(yield->getLoc(), operationName);
  state.addOperands(yield->getOperands());
  state.addAttributes(yield->getAttrs());
  builder.create(state);
  yield->erase();
}

LogicalResult lowerVariableState(ModuleOp model) {
  SmallVector<ac::VarReadOp> reads;
  SmallVector<ac::VarReadElementOp> elementReads;
  SmallVector<ac::VarAssignOp> assignments;
  SmallVector<ac::VarAssignElementOp> elementAssignments;
  SmallVector<ac::VarMatchOp> matches;
  SmallVector<ac::VarChooseOp> choices;
  SmallVector<ac::VarDeclOp> declarations;
  model.walk([&](ac::VarReadOp operation) { reads.push_back(operation); });
  model.walk([&](ac::VarReadElementOp operation) {
    elementReads.push_back(operation);
  });
  model.walk(
      [&](ac::VarAssignOp operation) { assignments.push_back(operation); });
  model.walk([&](ac::VarAssignElementOp operation) {
    elementAssignments.push_back(operation);
  });
  model.walk([&](ac::VarMatchOp operation) { matches.push_back(operation); });
  model.walk([&](ac::VarChooseOp operation) { choices.push_back(operation); });
  model.walk(
      [&](ac::VarDeclOp operation) { declarations.push_back(operation); });

  for (ac::VarMatchOp match : matches) {
    OpBuilder builder(match);
    OperationState state(match.getLoc(), ac::TableMatchOp::getOperationName());
    state.addTypes(match.getMask().getType());
    NamedAttrList attributes(match->getAttrs());
    attributes.erase("variable");
    attributes.set("table", match.getVariableAttr());
    state.addAttributes(attributes);
    state.addRegion();
    Operation *replacement = builder.create(state);
    replacement->getRegion(0).takeBody(match.getPredicate());
    replaceYield(replacement->getRegion(0).front().getTerminator(),
                 ac::TableMatchYieldOp::getOperationName());
    match.getMask().replaceAllUsesWith(replacement->getResult(0));
    match.erase();
  }

  for (ac::VarChooseOp choice : choices) {
    OpBuilder builder(choice);
    OperationState state(choice.getLoc(),
                         ac::TableChooseOp::getOperationName());
    state.addOperands(choice.getMask());
    state.addTypes({choice.getIndex().getType(), choice.getValid().getType()});
    NamedAttrList attributes(choice->getAttrs());
    attributes.erase("variable");
    attributes.set("table", choice.getVariableAttr());
    state.addAttributes(attributes);
    state.addRegion();
    Operation *replacement = builder.create(state);
    replacement->getRegion(0).takeBody(choice.getKey());
    if (!replacement->getRegion(0).empty())
      replaceYield(replacement->getRegion(0).front().getTerminator(),
                   ac::TableChooseYieldOp::getOperationName());
    choice.getIndex().replaceAllUsesWith(replacement->getResult(0));
    choice.getValid().replaceAllUsesWith(replacement->getResult(1));
    choice.erase();
  }

  for (ac::VarReadOp read : reads) {
    OpBuilder builder(read);
    ac::VarConstantOp index = createZeroIndex(builder, read.getLoc());
    OperationState state(read.getLoc(), ac::TableGetOp::getOperationName());
    state.addOperands(index.getResult());
    state.addTypes(read.getResult().getType());
    state.addAttribute("table", read.getVariableAttr());
    Operation *replacement = builder.create(state);
    read.getResult().replaceAllUsesWith(replacement->getResult(0));
    read.erase();
  }

  for (ac::VarReadElementOp read : elementReads) {
    OpBuilder builder(read);
    OperationState state(read.getLoc(), ac::TableGetOp::getOperationName());
    state.addOperands(read.getIndex());
    state.addTypes(read.getResult().getType());
    state.addAttribute("table", read.getVariableAttr());
    Operation *replacement = builder.create(state);
    read.getResult().replaceAllUsesWith(replacement->getResult(0));
    read.erase();
  }

  for (ac::VarAssignOp assignment : assignments) {
    OpBuilder builder(assignment);
    ac::VarDeclOp variable =
        resolveVariable(assignment, assignment.getVariableAttr());
    if (!variable)
      return assignment.emitOpError("persistent ac.var declaration is missing");
    FailureOr<ArrayAttr> writeFields = completeWriteFields(builder, variable);
    if (failed(writeFields))
      return assignment.emitOpError(
          "persistent struct field schema is unresolved");
    ac::VarConstantOp index = createZeroIndex(builder, assignment.getLoc());
    OperationState state(assignment.getLoc(),
                         ac::TableProposeOp::getOperationName());
    state.addOperands({index.getResult(), assignment.getValue()});
    if (assignment.getWhen())
      state.addOperands(assignment.getWhen());
    state.addAttribute("table", assignment.getVariableAttr());
    state.addAttribute("mode", builder.getStringAttr("replace"));
    state.addAttribute("write_fields", *writeFields);
    builder.create(state);
    assignment.erase();
  }

  for (ac::VarAssignElementOp assignment : elementAssignments) {
    OpBuilder builder(assignment);
    ac::VarDeclOp variable =
        resolveVariable(assignment, assignment.getVariableAttr());
    if (!variable)
      return assignment.emitOpError("persistent ac.var declaration is missing");
    FailureOr<ArrayAttr> writeFields = completeWriteFields(builder, variable);
    if (failed(writeFields))
      return assignment.emitOpError(
          "persistent struct field schema is unresolved");
    OperationState state(assignment.getLoc(),
                         ac::TableProposeOp::getOperationName());
    state.addOperands({assignment.getIndex(), assignment.getValue()});
    if (assignment.getWhen())
      state.addOperands(assignment.getWhen());
    state.addAttribute("table", assignment.getVariableAttr());
    state.addAttribute("mode", builder.getStringAttr("replace"));
    state.addAttribute("write_fields", *writeFields);
    builder.create(state);
    assignment.erase();
  }

  for (ac::VarDeclOp declaration : declarations) {
    auto integer = dyn_cast<IntegerAttr>(declaration.getInit());
    if (!integer || !integer.getValue().isZero())
      return declaration.emitOpError(
          "first ac.var storage-selection slice requires integer zero init");
    if (!isa<IntegerType, ac::StructType>(declaration.getValueType()))
      return declaration.emitOpError("first ac.var storage-selection slice "
                                     "requires scalar or flat struct");
    int64_t entries = 1;
    if (auto shape = declaration.getShapeAttr()) {
      if (shape.asArrayRef().size() != 1)
        return declaration.emitOpError(
            "storage selection requires one-dimensional ac.var shape");
      entries = shape.asArrayRef().front();
    }
    OpBuilder builder(declaration);
    OperationState state(declaration.getLoc(), ac::TableOp::getOperationName());
    state.addAttribute(SymbolTable::getSymbolAttrName(),
                       declaration.getSymNameAttr());
    state.addAttribute("entry_type", TypeAttr::get(declaration.getValueType()));
    state.addAttribute("entries", builder.getI64IntegerAttr(entries));
    state.addAttribute("init", builder.getI64IntegerAttr(0));
    state.addAttribute("owner", declaration.getOwnerAttr());
    std::string stableId = "table/";
    if (declaration.getOwner() != "/") {
      stableId.append(declaration.getOwner().drop_front());
      stableId.push_back('/');
    }
    stableId.append(declaration.getSymName());
    state.addAttribute("stable_id", builder.getStringAttr(stableId));
    builder.create(state);
    declaration.erase();
  }
  return success();
}

#define GEN_PASS_DEF_LOWERVARIABLESTATEPASS
#include "acir/Transforms/Passes.h.inc"

struct LowerVariableStatePass
    : impl::LowerVariableStatePassBase<LowerVariableStatePass> {
  void runOnOperation() override {
    if (failed(lowerVariableState(getOperation())))
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<Pass> createLowerVariableStatePass() {
  return std::make_unique<LowerVariableStatePass>();
}

} // namespace acir
