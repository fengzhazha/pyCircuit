#ifndef ACIR_ANALYSIS_VARIABLEANALYSIS_H
#define ACIR_ANALYSIS_VARIABLEANALYSIS_H

#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <memory>
#include <string>
#include <vector>

namespace acir {

enum class VariableLifetime { Unknown, Static, Temporary, Persistent };
enum class VariableUpdate { Unknown, Immutable, Assignable };

enum class ValueConstraintKind {
  Unknown,
  Constant,
  FiniteSet,
  ClosedInterval,
};

/// A conservative unsigned bit-vector value domain. Finite sets are kept only
/// while they contain at most 64 values; wider joins are represented by their
/// closed interval hull. Unknown is the fail-closed top value.
struct ValueConstraint {
  ValueConstraintKind kind = ValueConstraintKind::Unknown;
  llvm::SmallVector<uint64_t, 8> values;
  uint64_t lower = 0;
  uint64_t upper = 0;

  static ValueConstraint unknown();
  static ValueConstraint constant(uint64_t value);
  static ValueConstraint finiteSet(llvm::ArrayRef<uint64_t> values);
  static ValueConstraint closedInterval(uint64_t lower, uint64_t upper);
  static ValueConstraint join(const ValueConstraint &left,
                              const ValueConstraint &right);

  bool provesWithin(uint64_t requestedLower, uint64_t requestedUpper) const;
  bool operator==(const ValueConstraint &) const = default;
  void print(llvm::raw_ostream &os) const;
};

struct VariableProperties {
  VariableLifetime lifetime = VariableLifetime::Unknown;
  VariableUpdate update = VariableUpdate::Unknown;
  std::string ownerPath;

  static VariableProperties join(const VariableProperties &left,
                                 const VariableProperties &right);
  bool operator==(const VariableProperties &) const = default;
  void print(llvm::raw_ostream &os) const;
};

struct StateAccessFootprint {
  std::string resource;
  std::string access;
  std::string indexKind;
  std::vector<std::string> fields;
  mlir::Value present;
};

struct StateSnapshotFootprint {
  std::string resource;
  mlir::Value index;
  mlir::Value source;
  std::string indexKind;
  mlir::Value predicate;
  std::vector<std::string> fields;
};

class ACDataFlowAnalyzer {
public:
  explicit ACDataFlowAnalyzer(mlir::Operation *root);
  ~ACDataFlowAnalyzer();

  mlir::LogicalResult run();
  VariableProperties lookup(mlir::Value value) const;
  ValueConstraint lookupConstraint(mlir::Value value) const;
  bool provesWithin(mlir::Value value, uint64_t lower, uint64_t upper) const;
  VariableProperties lookupOwnedState(mlir::Operation *operation) const;
  llvm::SmallVector<StateAccessFootprint>
  stateFootprints(mlir::Operation *scope) const;
  llvm::SmallVector<StateSnapshotFootprint>
  stateSnapshots(mlir::Operation *scope) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace acir

#endif // ACIR_ANALYSIS_VARIABLEANALYSIS_H
