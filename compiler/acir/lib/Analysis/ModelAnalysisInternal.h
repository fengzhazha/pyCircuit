#ifndef ACIR_ANALYSIS_MODELANALYSISINTERNAL_H
#define ACIR_ANALYSIS_MODELANALYSISINTERNAL_H

#include "Dialect/ACIR/ProcessLowerability.h"
#include "acir/Analysis/ModelAnalysis.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"

#include <string>
#include <vector>

namespace acir::detail {

struct PureCallGraphLimits {
  uint64_t maxFunctions = kMaxPureCallFunctions;
  uint64_t maxEdges = kMaxPureCallEdges;
  uint64_t maxDepth = kMaxPureCallDepth;
};

struct ValidatedPureFunction {
  mlir::func::FuncOp function;
  std::vector<mlir::func::CallOp> calls;
};

struct ValidatedPureCallGraph {
  std::vector<ValidatedPureFunction> functions;

  const ValidatedPureFunction *lookup(llvm::StringRef name,
                                      uint64_t *probes = nullptr) const;
};

/// The single lowerability and purity authority for process-reachable calls.
mlir::FailureOr<ValidatedPureCallGraph> validatePureProcessCallGraph(
    mlir::ModuleOp model,
    const ac::RawModelStructureLimits &structureLimits =
        ac::RawModelStructureLimits(),
    const PureCallGraphLimits &callLimits = PureCallGraphLimits());

/// Checks whole-file structural budgets without recursive or typed IR access.
mlir::LogicalResult preflightModelStructure(mlir::ModuleOp model);

/// Returns whether any top-level or nested reserved freeze attribute exists.
bool hasTopologyFreezeEvidence(mlir::ModuleOp model);

/// Internal seal construction primitives. These declarations are deliberately
/// unavailable from the installed public include tree; only the trusted
/// first-freeze writer and frozen-integrity verifier consume them.
mlir::FailureOr<mlir::ArrayAttr> buildFrozenOwnerManifest(mlir::ModuleOp model);
mlir::FailureOr<mlir::ArrayAttr>
buildFrozenProcessSkeleton(ac::ProcessOp process);
std::string computeTopologyDigest(mlir::ModuleOp model);
std::string computeQueueGraphDefinitionFingerprint(ac::ModuleOp definition);
std::string computeQueueGraphSpecializationFingerprint(
    ac::ModuleOp definition, mlir::DictionaryAttr staticArguments);

} // namespace acir::detail

#endif // ACIR_ANALYSIS_MODELANALYSISINTERNAL_H
