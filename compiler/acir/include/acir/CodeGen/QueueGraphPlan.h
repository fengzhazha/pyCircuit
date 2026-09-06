#ifndef ACIR_CODEGEN_QUEUEGRAPHPLAN_H
#define ACIR_CODEGEN_QUEUEGRAPHPLAN_H

#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acir::codegen {

struct QueuePayloadFieldPlan {
  std::string name;
  std::string type;
  uint64_t width = 0;
};

struct QueuePayloadPlan {
  std::string name;
  std::vector<QueuePayloadFieldPlan> fields;
};

struct QueueEnumPlan {
  std::string name;
  std::vector<std::string> enumerants;
  uint64_t width = 0;
};

struct QueueAggregatePlan {
  std::string type;
  std::string kind;
  std::vector<std::string> elements;
  uint64_t length = 0;
  uint64_t width = 0;
};

struct QueueExpressionPlan {
  std::string result;
  std::string kind;
  std::string type;
  std::vector<std::string> operands;
  std::string field;
  std::string predicate;
  std::string literal;
  std::string table;
  std::string slot;
  std::vector<QueueExpressionPlan> nestedExpressions;
  std::vector<std::string> nestedYields;
  uint64_t lsb = 0;
  uint64_t width = 0;
  std::string mask;
  std::string value;
};

struct StateWritePlan {
  std::string table;
  std::string index;
  std::string value;
  std::string present;
  std::string mode;
  std::vector<std::string> fields;
};

struct OutputPresencePlan {
  uint64_t ordinal = 0;
  std::string value;
  std::string present;
};

struct StateReservationPlan {
  std::string table;
  std::string index;
  std::string source;
  std::string predicate;
  std::string indexKind;
  std::vector<std::string> fields;
};

struct QueuePlan {
  std::string name;
  std::string payloadType;
  std::string scope;
  uint64_t depth = 1;
  uint64_t latency = 1;
  uint64_t rate = 1;
};

struct QueueRuleResourcePlan {
  std::string kind;
  uint64_t ordinal = 0;
  std::string resource;

  bool operator==(const QueueRuleResourcePlan &) const = default;
};

struct QueueBlockPlan {
  std::string kind;
  std::string name;
  std::string scope;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::vector<uint64_t> depths;
  std::vector<uint64_t> latencies;
  std::string policy;
  uint64_t maxIterations = 0;
  std::string region;
  std::vector<QueueExpressionPlan> expressions;
  std::vector<std::string> yields;
  uint64_t capacity = 0;
  uint64_t start = 0;
  uint64_t noDependency = 0;
  uint64_t resources = 0;
  uint64_t credits = 0;
  uint64_t entries = 0;
  uint64_t init = 0;
  std::string resultField;
  std::string memoryInstance;
  std::string table;
  std::string tableIndex;
  std::string tableValue;
  std::string slot;
  std::string writeMode;
  uint64_t endpointOrdinal = 0;
  std::string message;
  std::vector<std::string> writeFields;
  uint64_t priority = 0;
  std::string guard;
  std::vector<StateWritePlan> stateWrites;
  std::vector<StateReservationPlan> stateReservations;
  std::vector<OutputPresencePlan> outputPresence;
  std::vector<QueueRuleResourcePlan> activationSources;
  std::vector<QueueRuleResourcePlan> transactionResources;
  bool hasActivationEvidence = false;
  bool initiallyActive = false;
  uint64_t lexicalOrder = 0;
};

struct MemoryInstancePlan {
  std::string name;
  std::string dataType;
  uint64_t entries = 0;
  uint64_t init = 0;
  uint64_t latency = 1;
  std::string stableId;
  std::string ownerPath;
};

struct MemoryRequestPlan {
  std::string instance;
  std::string name;
  std::string scope;
  std::string input;
  std::string output;
  uint64_t ordinal = 0;
  uint64_t depth = 1;
  std::string resultField;
};

struct TablePlan {
  std::string name;
  std::string entryType;
  uint64_t entries = 0;
  uint64_t init = 0;
  std::string stableId;
  std::string ownerPath;
};

struct TableMatchPlan {
  std::string name;
  std::string table;
  std::string scope;
  std::string resultType;
  std::vector<QueueExpressionPlan> expressions;
  std::string yield;
};

struct TableSelectionPlan {
  std::string name;
  std::string table;
  std::string scope;
  std::string match;
  std::string policy;
  std::string indexType;
  std::vector<QueueExpressionPlan> keyExpressions;
  std::string keyYield;
};

struct TableReadPlan {
  std::string table;
  std::string name;
  std::string scope;
  std::string input;
  std::string output;
  uint64_t depth = 1;
  uint64_t latency = 1;
};

struct TableWritePlan {
  std::string table;
  std::string name;
  std::string scope;
  std::string input;
  std::string mode;
  std::vector<std::string> writeFields;
};

struct TableMaskedWritePlan {
  std::string table;
  std::string name;
  std::string scope;
  std::string mode;
  std::vector<std::string> writeFields;
};

struct SlotPlan {
  std::string name;
  std::string payloadType;
  std::string input;
  std::string scope;
  std::string stableId;
  std::string ownerPath;
};

struct QueueInterfacePlan {
  std::string name;
  std::string payloadType;
};

struct QueueModuleInstancePlan {
  std::string name;
  std::string definition;
  std::string specializationFingerprint;
  std::string scope;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  uint64_t lexicalOrder = 0;
};

enum class QueueActivationNodeKind {
  InterfaceInput,
  InterfaceOutput,
  Queue,
  Block,
  Table,
};

struct QueueActivationNodePlan {
  QueueActivationNodeKind kind = QueueActivationNodeKind::Queue;
  uint64_t index = 0;

  bool operator==(const QueueActivationNodePlan &) const = default;
};

struct QueueActivationEdgePlan {
  QueueActivationNodePlan source;
  QueueActivationNodePlan target;

  bool operator==(const QueueActivationEdgePlan &) const = default;
};

struct QueueGraphPlan {
  std::string system;
  std::string definition;
  std::string definitionFingerprint;
  std::string specializationFingerprint;
  std::vector<QueueInterfacePlan> interfaceInputs;
  std::vector<QueueInterfacePlan> interfaceOutputs;
  std::vector<QueueModuleInstancePlan> moduleInstances;
  std::vector<std::shared_ptr<QueueGraphPlan>> moduleSpecializations;
  std::vector<QueueActivationEdgePlan> activationEdges;
  std::vector<QueueActivationEdgePlan> workClosureEdges;
  std::vector<QueueActivationNodePlan> initialActivation;
  std::vector<QueuePayloadPlan> payloads;
  std::vector<QueueEnumPlan> enums;
  std::vector<QueueAggregatePlan> aggregates;
  std::vector<std::string> scopes;
  std::vector<QueuePlan> queues;
  std::vector<QueueBlockPlan> blocks;
  std::vector<MemoryInstancePlan> memoryInstances;
  std::vector<MemoryRequestPlan> memoryRequests;
  std::vector<TablePlan> tables;
  std::vector<TableMatchPlan> tableMatches;
  std::vector<TableSelectionPlan> tableSelections;
  std::vector<TableReadPlan> tableReads;
  std::vector<TableWritePlan> tableWrites;
  std::vector<TableMaskedWritePlan> tableMaskedWrites;
  std::vector<SlotPlan> slots;

  llvm::Expected<std::string> canonicalJson() const;
};

llvm::Expected<QueueGraphPlan> buildQueueGraphPlan(mlir::ModuleOp module);
llvm::Error verifyQueueGraphPlan(const QueueGraphPlan &plan);

} // namespace acir::codegen

#endif // ACIR_CODEGEN_QUEUEGRAPHPLAN_H
