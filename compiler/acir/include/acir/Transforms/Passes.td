#ifndef ACIR_TRANSFORMS_PASSES
#define ACIR_TRANSFORMS_PASSES

include "mlir/Pass/PassBase.td"

def VerifyModelPass : Pass<"ac-verify-model", "::mlir::ModuleOp"> {
  let summary = "Verify complete Agentic Circuit model semantics";
  let description = [{
    Runs local ACIR verification together with deterministic whole-model
    hierarchy, ownership, purity, zero-delay dependency, and frozen-integrity
    checks.
  }];
}

def VerifyValueConstraintsPass
    : Pass<"ac-verify-value-constraints", "::mlir::ModuleOp"> {
  let summary = "Prove dynamic Agentic Circuit state indices are in range";
  let description = [{
    Runs ACDataFlowAnalyzer's bounded unsigned value-domain analysis and
    rejects shaped ac.var and rule/firing Table accesses whose dynamic index
    cannot be proven within the declared extent.
  }];
}

def CanonicalizeModelPass : Pass<"ac-canonicalize-model", "::mlir::ModuleOp"> {
  let summary = "Canonicalize Agentic Circuit model topology";
  let description = [{
    Sorts symbol and Graph declarations by stable semantic keys and removes
    source-location variance before deterministic topology freeze.
  }];
}

def FreezeTopologyPass : Pass<"ac-freeze-topology", "::mlir::ModuleOp"> {
  let summary = "Freeze deterministic Agentic Circuit topology";
  let description = [{
    Verifies the selected static architecture, proves freeze-phase contracts,
    binds elaborated owner paths and stable IDs, and persists an integrity
    digest that makes later topology mutation a hard error.
  }];
}

def LowerProcessStatePass : Pass<"ac-lower-process-state", "::mlir::ModuleOp"> {
  let summary = "Plan ACSim process state from frozen ACIR";
  let description = [{
    Analyzes a selected topology-frozen model without mutation and produces
    an immutable ProcessStatePlanSet.
  }];
}

def LowerVariableStatePass
    : Pass<"ac-lower-variable-state", "::mlir::ModuleOp"> {
  let summary = "Select committed storage for persistent ac.var declarations";
}

def InferRuleTypesPass : Pass<"ac-infer-rule-types", "::mlir::ModuleOp"> {
  let summary = "Refine transient rule type-constraint markers";
}

def InferRuleEffectsPass : Pass<"ac-infer-rule-effects", "::mlir::ModuleOp"> {
  let summary = "Infer rule provenance and effect summaries";
}

def InferRuleActivationPass
    : Pass<"ac-infer-rule-activation", "::mlir::ModuleOp"> {
  let summary = "Infer typed rule activation and transaction resources";
}

def MaterializeRuleChecksPass
    : Pass<"ac-materialize-rule-checks", "::mlir::ModuleOp"> {
  let summary = "Materialize path-qualified rule checks";
}

def MaterializeRuleHandshakePass
    : Pass<"ac-materialize-rule-handshake", "::mlir::ModuleOp"> {
  let summary = "Materialize rule Queue handshake obligations";
}

def DischargeRuleObligationsPass
    : Pass<"ac-discharge-rule-obligations", "::mlir::ModuleOp"> {
  let summary =
      "Discharge materialized rule obligations with explicit evidence";
}

def ResolveRuleSchedulePass
    : Pass<"ac-resolve-rule-schedule", "::mlir::ModuleOp"> {
  let summary = "Resolve rule selection and scheduling contracts";
}

def LowerRulesToFiringPass
    : Pass<"ac-lower-rules-to-firing", "::mlir::ModuleOp"> {
  let summary = "Lower closed transient rules to internal firing IR";
}

def CanonicalizePureFiringsPass
    : Pass<"ac-canonicalize-pure-firings", "::mlir::ModuleOp"> {
  let summary = "Prove and canonicalize pure firings as Queue transforms";
}

def VerifyRuleClosurePass : Pass<"ac-verify-rule-closure", "::mlir::ModuleOp"> {
  let summary = "Reject transient rules and unresolved markers";
}

#endif // ACIR_TRANSFORMS_PASSES
