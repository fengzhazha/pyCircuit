#ifndef ACIR_TRANSFORMS_PASSES_H
#define ACIR_TRANSFORMS_PASSES_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LogicalResult.h"

#include <memory>

namespace mlir {
class OpPassManager;
}

namespace acir {

std::unique_ptr<mlir::Pass> createNormalizeACIRFilePass();
std::unique_ptr<mlir::Pass> createVerifyACIRFilePass();
std::unique_ptr<mlir::Pass> createLowerProcessStatePass();

#define GEN_PASS_DECL_VERIFYMODELPASS
#define GEN_PASS_DECL_VERIFYVALUECONSTRAINTSPASS
#define GEN_PASS_DECL_LOWERPROCESSSTATEPASS
#define GEN_PASS_DECL_LOWERVARIABLESTATEPASS
#define GEN_PASS_DECL_CANONICALIZEMODELPASS
#define GEN_PASS_DECL_FREEZETOPOLOGYPASS
#define GEN_PASS_DECL_INFERRULETYPESPASS
#define GEN_PASS_DECL_INFERRULEEFFECTSPASS
#define GEN_PASS_DECL_INFERRULEACTIVATIONPASS
#define GEN_PASS_DECL_MATERIALIZERULECHECKSPASS
#define GEN_PASS_DECL_MATERIALIZERULEHANDSHAKEPASS
#define GEN_PASS_DECL_DISCHARGERULEOBLIGATIONSPASS
#define GEN_PASS_DECL_RESOLVERULESCHEDULEPASS
#define GEN_PASS_DECL_LOWERRULESTOFIRINGPASS
#define GEN_PASS_DECL_CANONICALIZEPUREFIRINGSPASS
#define GEN_PASS_DECL_VERIFYRULECLOSUREPASS
#include "acir/Transforms/Passes.h.inc"

/// Shared implementation used by ac-canonicalize-model and the atomic freeze
/// pass. It is idempotent and never depends on host pointer order.
mlir::LogicalResult canonicalizeModel(mlir::ModuleOp model);

/// Reject every transient rule and typed marker before
/// freeze/hash/serialization.
mlir::LogicalResult verifyRuleClosure(mlir::ModuleOp model);

/// Prove that every dynamic state index is within its declared resource
/// extent. Unknown constraints fail closed.
mlir::LogicalResult verifyValueConstraints(mlir::ModuleOp model);

/// Add the canonical staged rule-to-marker-free-IR pipeline. Topology freeze
/// remains a separate stage so compiler drivers can preserve stage evidence.
void addRuleLoweringPipeline(mlir::OpPassManager &manager);
void registerRuleLoweringPipeline();

#define GEN_PASS_REGISTRATION
#include "acir/Transforms/Passes.h.inc"

} // namespace acir

#endif // ACIR_TRANSFORMS_PASSES_H
