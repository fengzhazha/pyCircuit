#ifndef ACIR_ANALYSIS_MODELANALYSIS_H
#define ACIR_ANALYSIS_MODELANALYSIS_H

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"

#include <cstdint>
namespace acir {

/// Fixed capability limits keep hostile or malformed models diagnosable and
/// make the whole-model analyses independent of allocator failure behavior.
inline constexpr uint64_t kMaxModelAnalysisNodes = 1U << 20;
inline constexpr uint64_t kMaxModelAnalysisEdges = 1U << 22;
/// The top-level builtin.module has depth 0; each contained operation adds 1.
inline constexpr uint64_t kMaxModelRegionNesting = 512;
inline constexpr uint64_t kMaxPureCallFunctions = 1U << 16;
inline constexpr uint64_t kMaxPureCallEdges = 1U << 18;
inline constexpr uint64_t kMaxPureCallDepth = 1024;

class ModelAnalysis {
public:
  explicit ModelAnalysis(mlir::ModuleOp model) : model(model) {}

  /// Runs the complete IR-stage semantic closure. Existing operation
  /// verifiers remain authoritative for local contracts; this analysis adds
  /// deterministic whole-model selection, purity, dependency, and frozen
  /// integrity checks.
  mlir::LogicalResult verify();

  /// Verifies module-level require/ensure conditions at topology freeze.
  mlir::LogicalResult verifyFreezeContracts();

private:
  mlir::LogicalResult verifyPureProcessCalls();
  mlir::LogicalResult verifyZeroDelayDependencies();
  mlir::LogicalResult verifyFrozenIntegrity();

  mlir::ModuleOp model;
};

mlir::LogicalResult verifyModel(mlir::ModuleOp model);
/// Verify the sealed evidence and restricted top-level shape of the flat
/// QueueGraph representation used by the gfsim/PYC generators.
mlir::LogicalResult verifyFrozenFlatQueueGraph(mlir::ModuleOp model);
/// Verify a hierarchy-preserving QueueGraph whose reusable module
/// specializations are sealed before backend planning.
mlir::LogicalResult verifyFrozenStructuredQueueGraph(mlir::ModuleOp model);
bool isTopologyFrozen(mlir::ModuleOp model);

} // namespace acir

#endif // ACIR_ANALYSIS_MODELANALYSIS_H
