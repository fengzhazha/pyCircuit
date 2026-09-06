#include "acir/Transforms/Passes.h"

#include "acir/Analysis/VariableAnalysis.h"
#include "acir/Dialect/ACIR/ACIROps.h"

#include "mlir/IR/SymbolTable.h"

using namespace mlir;

namespace acir {
namespace {

template <typename Declaration>
Declaration resolveFlatDeclaration(Operation *operation,
                                   FlatSymbolRefAttr reference) {
  for (Operation *ancestor = operation->getParentOp(); ancestor;
       ancestor = ancestor->getParentOp()) {
    for (Region &region : ancestor->getRegions())
      for (Block &block : region)
        for (Declaration declaration : block.getOps<Declaration>())
          if (declaration.getSymName() == reference.getValue())
            return declaration;
  }
  return {};
}

std::string constraintText(const ValueConstraint &constraint) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  constraint.print(stream);
  return text;
}

LogicalResult verifyIndex(ACDataFlowAnalyzer &analysis, Operation *operation,
                          Value index, uint64_t extent, StringRef resource) {
  if (extent == 0)
    return operation->emitOpError() << resource << " extent must be positive";
  if (analysis.provesWithin(index, 0, extent - 1))
    return success();
  ValueConstraint constraint = analysis.lookupConstraint(index);
  return operation->emitOpError()
         << "cannot prove " << resource << " index is within [0, "
         << extent - 1 << "]; inferred " << constraintText(constraint);
}

#define GEN_PASS_DEF_VERIFYVALUECONSTRAINTSPASS
#include "acir/Transforms/Passes.h.inc"

struct VerifyValueConstraintsPass
    : impl::VerifyValueConstraintsPassBase<VerifyValueConstraintsPass> {
  void runOnOperation() override {
    if (failed(verifyValueConstraints(getOperation())))
      signalPassFailure();
  }
};

} // namespace

LogicalResult verifyValueConstraints(ModuleOp model) {
  ACDataFlowAnalyzer analysis(model.getOperation());
  if (failed(analysis.run()))
    return model.emitError("AC value constraint analysis failed");

  LogicalResult result = success();
  model.walk([&](Operation *operation) {
    if (failed(result))
      return WalkResult::interrupt();
    if (auto read = dyn_cast<ac::VarReadElementOp>(operation)) {
      auto variable = resolveFlatDeclaration<ac::VarDeclOp>(
          read, read.getVariableAttr());
      if (!variable || !variable.getShapeAttr())
        return WalkResult::advance();
      result = verifyIndex(analysis, read, read.getIndex(),
                           variable.getShapeAttr().asArrayRef().front(),
                           "shaped ac.var");
    } else if (auto assign = dyn_cast<ac::VarAssignElementOp>(operation)) {
      auto variable = resolveFlatDeclaration<ac::VarDeclOp>(
          assign, assign.getVariableAttr());
      if (!variable || !variable.getShapeAttr())
        return WalkResult::advance();
      result = verifyIndex(analysis, assign, assign.getIndex(),
                           variable.getShapeAttr().asArrayRef().front(),
                           "shaped ac.var");
    } else if (auto read = dyn_cast<ac::TableGetOp>(operation)) {
      auto table = resolveFlatDeclaration<ac::TableOp>(read,
                                                       read.getTableAttr());
      if (table)
        result = verifyIndex(analysis, read, read.getIndex(),
                             table.getEntries(), "Table");
    } else if (auto read = dyn_cast<ac::TableReadOp>(operation)) {
      auto table = resolveFlatDeclaration<ac::TableOp>(read,
                                                       read.getTableAttr());
      if (table && read.getAddress().hasOneBlock())
        result = verifyIndex(
            analysis, read,
            cast<ac::TableYieldOp>(read.getAddress().front().getTerminator())
                .getValue(),
            table.getEntries(), "Table read address");
    } else if (auto write = dyn_cast<ac::TableWriteOp>(operation)) {
      auto table = resolveFlatDeclaration<ac::TableOp>(write,
                                                       write.getTableAttr());
      if (table && write.getAddress().hasOneBlock())
        result = verifyIndex(
            analysis, write,
            cast<ac::TableYieldOp>(write.getAddress().front().getTerminator())
                .getValue(),
            table.getEntries(), "Table write address");
    } else if (auto proposal = dyn_cast<ac::TableProposeOp>(operation)) {
      auto table = resolveFlatDeclaration<ac::TableOp>(
          proposal, proposal.getTableAttr());
      if (table)
        result = verifyIndex(analysis, proposal, proposal.getIndex(),
                             table.getEntries(), "Table");
    } else if (auto snapshot = dyn_cast<ac::StateSnapshotOp>(operation)) {
      if (!snapshot.getIndex())
        return WalkResult::advance();
      auto table = resolveFlatDeclaration<ac::TableOp>(
          snapshot, snapshot.getTableAttr());
      if (table)
        result = verifyIndex(analysis, snapshot, snapshot.getIndex(),
                             table.getEntries(), "Table snapshot");
    }
    return succeeded(result) ? WalkResult::advance()
                             : WalkResult::interrupt();
  });
  return result;
}

std::unique_ptr<Pass> createVerifyValueConstraintsPass() {
  return std::make_unique<VerifyValueConstraintsPass>();
}

} // namespace acir
