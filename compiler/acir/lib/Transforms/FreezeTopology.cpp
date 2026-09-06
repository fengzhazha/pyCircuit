#include "acir/Transforms/Passes.h"

#include "Analysis/ModelAnalysisInternal.h"
#include "Analysis/ModelAnalysisTestHooks.h"
#include "acir/Analysis/ModelAnalysis.h"
#include "acir/Dialect/ACIR/ACIROps.h"
#include "acir/Dialect/ACIR/GraphRegion.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

using namespace mlir;

namespace acir {
namespace {

std::string manifestKey(SymbolRefAttr owner, StringRef kind) {
  std::string key;
  llvm::raw_string_ostream stream(key);
  stream << owner << '\0' << kind;
  return key;
}

class ManifestOwnerIndex {
public:
  explicit ManifestOwnerIndex(ArrayAttr manifest) {
    for (Attribute attribute : manifest) {
      if (detail::activeFreezeWork)
        ++detail::activeFreezeWork->manifestIndexInsertions;
      auto record = cast<DictionaryAttr>(attribute);
      index[manifestKey(record.getAs<SymbolRefAttr>("owner"),
                        record.getAs<StringAttr>("kind").getValue())]
          .push_back(record);
    }
  }

  FailureOr<DictionaryAttr> lookup(SymbolRefAttr owner, StringRef kind) const {
    if (detail::activeFreezeWork)
      ++detail::activeFreezeWork->manifestOwnerLookups;
    auto found = index.find(manifestKey(owner, kind));
    if (found == index.end() || found->second.size() != 1)
      return failure();
    return found->second.front();
  }

private:
  llvm::StringMap<SmallVector<DictionaryAttr>> index;
};

std::string declarationKey(SymbolRefAttr reference) {
  std::string key;
  llvm::raw_string_ostream(key) << reference;
  return key;
}

class DeclarationIndex {
public:
  LogicalResult build(ModuleOp model) {
    for (ac::ModuleOp definition : model.getOps<ac::ModuleOp>()) {
      if (definition.getBody().empty())
        continue;
      for (Operation &operation : definition.getBody().front()) {
        auto name = operation.getAttrOfType<StringAttr>(
            SymbolTable::getSymbolAttrName());
        if (!name)
          continue;
        SymbolRefAttr reference = SymbolRefAttr::get(
            model.getContext(), definition.getSymName(),
            {FlatSymbolRefAttr::get(model.getContext(), name.getValue())});
        if (detail::activeFreezeWork)
          ++detail::activeFreezeWork->declarationIndexInsertions;
        if (!index.try_emplace(declarationKey(reference), &operation).second)
          return operation.emitOpError()
                 << "duplicate freeze declaration index key '" << reference
                 << "'";
      }
    }
    return success();
  }

  Operation *lookup(SymbolRefAttr reference) const {
    if (detail::activeFreezeWork)
      ++detail::activeFreezeWork->declarationLookups;
    auto found = index.find(declarationKey(reference));
    return found == index.end() ? nullptr : found->second;
  }

private:
  llvm::StringMap<Operation *> index;
};

LogicalResult freezeFlatQueueGraph(ModuleOp model) {
  auto contractEpoch = model->getAttrOfType<StringAttr>("ac.contract_epoch");
  if (!contractEpoch || contractEpoch.getValue() != "0.5")
    return model.emitError(
        "flat QueueGraph freeze requires ac.contract_epoch = \"0.5\"");
  auto modelKind = model->getAttrOfType<StringAttr>("ac.model_kind");
  if (!modelKind || modelKind.getValue() != "queue_graph")
    return model.emitError(
        "flat QueueGraph freeze requires ac.model_kind = \"queue_graph\"");
  for (Operation &operation : model.getBody()->getOperations()) {
    if (isa<ac::SystemOp, ac::ModuleOp, ac::ModuleExternOp,
            ac::ModuleGeneratedOp>(operation))
      return operation.emitOpError(
          "is not legal at the top level of a flat QueueGraph model");
  }
  auto system = model->getAttrOfType<StringAttr>("ac.system");
  if (!system || system.getValue().empty())
    return model.emitError(
        "flat QueueGraph freeze requires non-empty ac.system");
  auto domain = model->getAttrOfType<StringAttr>("ac.queue_graph_domain");
  if (!domain || domain.getValue() != "cycle")
    return model.emitError(
        "flat QueueGraph freeze requires exact ac.queue_graph_domain = "
        "\"cycle\"");
  if (failed(verifyRuleClosure(model)))
    return failure();
  if (detail::hasTopologyFreezeEvidence(model))
    return verifyFrozenFlatQueueGraph(model);
  Builder builder(model.getContext());
  model->setAttr("ac.freeze_epoch", builder.getStringAttr("0.5"));
  model->setAttr("ac.frozen_owners", builder.getArrayAttr({}));
  model->setAttr("ac.topology_frozen", builder.getBoolAttr(true));
  model->setAttr("ac.topology_digest",
                 builder.getStringAttr(detail::computeTopologyDigest(model)));
  return success();
}

LogicalResult freezeStructuredQueueGraph(ModuleOp model) {
  auto contractEpoch = model->getAttrOfType<StringAttr>("ac.contract_epoch");
  if (!contractEpoch || contractEpoch.getValue() != "0.5")
    return model.emitError(
        "structured QueueGraph freeze requires ac.contract_epoch = \"0.5\"");
  auto domain = model->getAttrOfType<StringAttr>("ac.queue_graph_domain");
  if (!domain || domain.getValue() != "cycle")
    return model.emitError("structured QueueGraph freeze requires exact "
                           "ac.queue_graph_domain = \"cycle\"");
  if (model.getOps<ac::SystemOp>().empty() ||
      model.getOps<ac::ModuleOp>().empty())
    return model.emitError(
        "structured QueueGraph freeze requires ac.system and ac.module");
  if (failed(mlir::verify(model)) || failed(ac::verifyGraphStructure(model)))
    return failure();
  if (detail::hasTopologyFreezeEvidence(model))
    return verifyFrozenStructuredQueueGraph(model);

  ac::SystemOp selected;
  unsigned selectedCount = 0;
  for (ac::SystemOp system : model.getOps<ac::SystemOp>())
    if (system.getSelected()) {
      selected = system;
      ++selectedCount;
    }
  if (selectedCount != 1)
    return model.emitError(
        "structured QueueGraph freeze requires exactly one selected ac.system");

  SymbolTable symbols(model);
  auto root = dyn_cast_or_null<ac::ModuleOp>(
      symbols.lookup(selected.getRootAttr().getValue()));
  if (!root)
    return selected.emitOpError(
        "structured QueueGraph root must resolve to a materialized module");

  Builder builder(model.getContext());
  for (ac::ModuleOp definition : model.getOps<ac::ModuleOp>()) {
    definition->removeAttr("ac.specialization");
    definition->setAttr(
        "ac.definition_fingerprint",
        builder.getStringAttr(
            detail::computeQueueGraphDefinitionFingerprint(definition)));
  }
  for (ac::ModuleOp definition : model.getOps<ac::ModuleOp>())
    for (ac::InstanceOp instance :
         definition.getBody().front().getOps<ac::InstanceOp>()) {
      auto target = dyn_cast_or_null<ac::ModuleOp>(
          symbols.lookup(instance.getDefinitionAttr().getValue()));
      if (!target)
        return instance.emitOpError(
            "QueueGraph instance requires a materialized module definition");
      instance->setAttr("ac.specialization",
                        builder.getStringAttr(
                            detail::computeQueueGraphSpecializationFingerprint(
                                target, instance.getStaticArgs())));
    }
  root->setAttr(
      "ac.specialization",
      builder.getStringAttr(detail::computeQueueGraphSpecializationFingerprint(
          root, builder.getDictionaryAttr({}))));

  FailureOr<ArrayAttr> ownerManifest = detail::buildFrozenOwnerManifest(model);
  if (failed(ownerManifest))
    return failure();
  model->setAttr("ac.freeze_epoch", builder.getStringAttr("0.5"));
  model->setAttr(
      "ac.frozen_system",
      FlatSymbolRefAttr::get(model.getContext(), selected.getSymName()));
  model->setAttr("ac.frozen_owners", *ownerManifest);
  model->setAttr("ac.topology_frozen", builder.getBoolAttr(true));
  model->setAttr("ac.topology_digest",
                 builder.getStringAttr(detail::computeTopologyDigest(model)));
  return verifyFrozenStructuredQueueGraph(model);
}

LogicalResult freezeTopology(ModuleOp model) {
  // Frozen models and direct hand-written ACIR take the same fail-closed
  // dynamic-index proof path as rule lowering.
  if (failed(verifyValueConstraints(model)))
    return failure();
  // Closure precedes every canonicalization/hash mutation so unresolved
  // obligations cannot be dropped or hidden by a generic transform.
  if (failed(verifyRuleClosure(model)))
    return failure();
  if (auto modelKind = model->getAttrOfType<StringAttr>("ac.model_kind")) {
    if (modelKind.getValue() != "queue_graph")
      return model.emitError("unknown ac.model_kind '")
             << modelKind.getValue() << "'";
    if (!model.getOps<ac::SystemOp>().empty())
      return freezeStructuredQueueGraph(model);
    return freezeFlatQueueGraph(model);
  }
  if (model->getAttrOfType<StringAttr>("ac.system") &&
      model.getOps<ac::SystemOp>().empty())
    return model.emitError(
        "flat QueueGraph model requires ac.model_kind = \"queue_graph\"");
  if (failed(detail::preflightModelStructure(model)))
    return failure();
  if (detail::hasTopologyFreezeEvidence(model))
    return verifyModel(model);
  if (failed(canonicalizeModel(model)))
    return failure();

  ModelAnalysis analysis(model);
  if (failed(analysis.verify()) || failed(analysis.verifyFreezeContracts()))
    return failure();

  ac::SystemOp selected;
  for (ac::SystemOp system : model.getOps<ac::SystemOp>())
    if (system.getSelected()) {
      selected = system;
      break;
    }
  if (!selected)
    return model.emitError(
        "topology freeze requires exactly one selected ac.system");
  if (!selected.getPrimaryWorkloadAttr())
    return selected.emitOpError(
        "selected system requires one canonical primary workload at topology "
        "freeze");

  FailureOr<ArrayAttr> ownerManifest = detail::buildFrozenOwnerManifest(model);
  if (failed(ownerManifest))
    return failure();
  Builder builder(model.getContext());
  model->setAttr("ac.freeze_epoch", builder.getStringAttr("0.5"));
  model->setAttr(
      "ac.frozen_system",
      FlatSymbolRefAttr::get(model.getContext(), selected.getSymName()));
  model->setAttr("ac.frozen_owners", *ownerManifest);
  ManifestOwnerIndex ownerIndex(*ownerManifest);
  DeclarationIndex declarationIndex;
  if (failed(declarationIndex.build(model)))
    return failure();

  SymbolRefAttr workload = selected.getPrimaryWorkloadAttr();
  FailureOr<DictionaryAttr> workloadOwner =
      ownerIndex.lookup(workload, "ac.process");
  if (failed(workloadOwner))
    return selected.emitOpError()
           << "primary workload '" << workload
           << "' has no unique elaborated absolute owner";
  model->setAttr(
      "ac.frozen_primary_workload",
      builder.getDictionaryAttr({
          builder.getNamedAttr("reference", workload),
          builder.getNamedAttr("path", (*workloadOwner).get("path")),
          builder.getNamedAttr("stable_id", (*workloadOwner).get("stable_id")),
      }));

  SmallVector<Attribute> frozenInstrumentation;
  for (Attribute attribute : selected.getInstrumentation()) {
    auto reference = cast<SymbolRefAttr>(attribute);
    ArrayRef<FlatSymbolRefAttr> nested = reference.getNestedReferences();
    SymbolRefAttr processReference = SymbolRefAttr::get(
        model.getContext(), reference.getRootReference(), {nested.front()});
    FailureOr<DictionaryAttr> processOwner =
        ownerIndex.lookup(processReference, "ac.process");
    if (failed(processOwner))
      return selected.emitOpError()
             << "instrumentation '" << reference
             << "' has no unique elaborated process owner";
    StringRef local = nested.back().getValue();
    frozenInstrumentation.push_back(builder.getDictionaryAttr({
        builder.getNamedAttr("reference", reference),
        builder.getNamedAttr(
            "path", builder.getStringAttr(
                        ((*processOwner).getAs<StringAttr>("path").getValue() +
                         "." + local)
                            .str())),
        builder.getNamedAttr(
            "stable_id",
            builder.getStringAttr(
                ((*processOwner).getAs<StringAttr>("stable_id").getValue() +
                 "/" + local)
                    .str())),
    }));
  }
  model->setAttr("ac.frozen_instrumentation",
                 builder.getArrayAttr(frozenInstrumentation));

  // Persist the relevant absolute owner set on each declaration. Reused
  // definitions intentionally receive multiple records rather than a lossy
  // definition-local identity.
  llvm::DenseMap<Operation *, SmallVector<Attribute>> byDeclaration;
  for (Attribute attribute : *ownerManifest) {
    auto record = cast<DictionaryAttr>(attribute);
    if (record.getAs<StringAttr>("kind").getValue() == "ac.system_root")
      continue;
    Operation *declaration =
        declarationIndex.lookup(record.getAs<SymbolRefAttr>("owner"));
    if (!declaration)
      return model.emitError()
             << "frozen owner manifest references an unindexed declaration '"
             << record.getAs<SymbolRefAttr>("owner") << "'";
    byDeclaration[declaration].push_back(record);

    if (record.getAs<StringAttr>("kind").getValue() != "ac.process")
      continue;
    ArrayAttr traces = record.getAs<ArrayAttr>("trace_sources");
    if (!traces)
      continue;
    auto process = dyn_cast<ac::ProcessOp>(declaration);
    if (!process)
      return declaration->emitOpError(
          "process owner record does not resolve to ac.process");
    process.getBody().walk([&](ac::TraceOpenOp trace) {
      if (!llvm::is_contained(traces, builder.getStringAttr(trace.getSource())))
        return;
      trace->setAttr(
          "ac.frozen_owner",
          builder.getDictionaryAttr({
              builder.getNamedAttr("path", record.get("path")),
              builder.getNamedAttr("stable_id", record.get("stable_id")),
              builder.getNamedAttr("source", trace.getSourceAttr()),
          }));
    });
  }
  for (auto &[declaration, records] : byDeclaration) {
    llvm::sort(records, [](Attribute left, Attribute right) {
      auto leftRecord = cast<DictionaryAttr>(left);
      auto rightRecord = cast<DictionaryAttr>(right);
      return leftRecord.getAs<StringAttr>("path").getValue() <
             rightRecord.getAs<StringAttr>("path").getValue();
    });
    declaration->setAttr("ac.frozen_owners", builder.getArrayAttr(records));
  }

  for (ac::ModuleOp module : model.getOps<ac::ModuleOp>()) {
    if (module.getBody().empty())
      continue;
    for (Operation &operation : module.getBody().front())
      if (isa<ac::RequireOp, ac::EnsureOp>(operation))
        operation.setAttr("ac.freeze_proven", builder.getBoolAttr(true));
  }

  LogicalResult skeletonResult = success();
  model.walk([&](ac::ProcessOp process) {
    if (failed(skeletonResult))
      return;
    FailureOr<ArrayAttr> skeleton = detail::buildFrozenProcessSkeleton(process);
    if (failed(skeleton)) {
      skeletonResult = failure();
      return;
    }
    process->setAttr("ac.frozen_process_skeleton", *skeleton);
  });
  if (failed(skeletonResult))
    return failure();

  model->setAttr("ac.topology_frozen", builder.getBoolAttr(true));
  model->setAttr("ac.topology_digest",
                 builder.getStringAttr(detail::computeTopologyDigest(model)));
  return verifyModel(model);
}

#define GEN_PASS_DEF_FREEZETOPOLOGYPASS
#include "acir/Transforms/Passes.h.inc"

struct FreezeTopologyPass : impl::FreezeTopologyPassBase<FreezeTopologyPass> {
  void runOnOperation() override {
    if (failed(freezeTopology(getOperation())))
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<Pass> createFreezeTopologyPass() {
  return std::make_unique<FreezeTopologyPass>();
}

} // namespace acir
