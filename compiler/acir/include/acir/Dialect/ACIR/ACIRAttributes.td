#ifndef ACIR_DIALECT_ACIR_ACIRATTRIBUTES_TD
#define ACIR_DIALECT_ACIR_ACIRATTRIBUTES_TD

include "acir/Dialect/ACIR/ACIRDialect.td"
include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/EnumAttr.td"

def ACIR_Unit_Ticks : I32EnumAttrCase<"Ticks", 0, "ticks">;
def ACIR_Unit_Cycles : I32EnumAttrCase<"Cycles", 1, "cycles">;
def ACIR_Unit_Seconds : I32EnumAttrCase<"Seconds", 2, "seconds">;
def ACIR_Unit_Milliseconds : I32EnumAttrCase<"Milliseconds", 3, "milliseconds">;
def ACIR_Unit_Microseconds : I32EnumAttrCase<"Microseconds", 4, "microseconds">;
def ACIR_Unit_Nanoseconds : I32EnumAttrCase<"Nanoseconds", 5, "nanoseconds">;
def ACIR_Unit_Picoseconds : I32EnumAttrCase<"Picoseconds", 6, "picoseconds">;
def ACIR_Unit_Bytes : I32EnumAttrCase<"Bytes", 7, "bytes">;
def ACIR_Unit_Bits : I32EnumAttrCase<"Bits", 8, "bits">;
def ACIR_Unit_Entries : I32EnumAttrCase<"Entries", 9, "entries">;
def ACIR_Unit_Packets : I32EnumAttrCase<"Packets", 10, "packets">;
def ACIR_Unit_Transactions
    : I32EnumAttrCase<"Transactions", 11, "transactions">;

def ACIR_Unit
    : I32EnumAttr<"Unit", "ACIR exact measurement unit",
                  [ACIR_Unit_Ticks, ACIR_Unit_Cycles, ACIR_Unit_Seconds,
                   ACIR_Unit_Milliseconds, ACIR_Unit_Microseconds,
                   ACIR_Unit_Nanoseconds, ACIR_Unit_Picoseconds,
                   ACIR_Unit_Bytes, ACIR_Unit_Bits, ACIR_Unit_Entries,
                   ACIR_Unit_Packets, ACIR_Unit_Transactions]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}

def ACIR_TypeConstraintState_Unknown : I32EnumAttrCase<"Unknown", 0, "unknown">;
def ACIR_TypeConstraintState_Constrained
    : I32EnumAttrCase<"Constrained", 1, "constrained">;
def ACIR_TypeConstraintState_Exact : I32EnumAttrCase<"Exact", 2, "exact">;
def ACIR_TypeConstraintState
    : I32EnumAttr<"TypeConstraintState", "rule type-constraint lattice",
                  [ACIR_TypeConstraintState_Unknown,
                   ACIR_TypeConstraintState_Constrained,
                   ACIR_TypeConstraintState_Exact]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_TypeConstraintStateAttr
    : EnumAttr<ACIR_Dialect, ACIR_TypeConstraintState, "type_constraint_state">;

def ACIR_TypeConstraintKind_QueuePayload
    : I32EnumAttrCase<"QueuePayload", 0, "queue_payload">;
def ACIR_TypeConstraintKind
    : I32EnumAttr<
          "TypeConstraintKind",
          "rule type-constraint kind", [ACIR_TypeConstraintKind_QueuePayload]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_TypeConstraintKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_TypeConstraintKind, "type_constraint_kind">;

def ACIR_ValueFactKind_CommittedInput
    : I32EnumAttrCase<"CommittedInput", 0, "committed_input">;
def ACIR_ValueFactKind
    : I32EnumAttr<"ValueFactKind",
                  "rule value-fact kind", [ACIR_ValueFactKind_CommittedInput]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_ValueFactKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_ValueFactKind, "value_fact_kind">;

def ACIR_ObligationState_Pending : I32EnumAttrCase<"Pending", 0, "pending">;
def ACIR_ObligationState_Materialized
    : I32EnumAttrCase<"Materialized", 1, "materialized">;
def ACIR_ObligationState_Discharged
    : I32EnumAttrCase<"Discharged", 2, "discharged">;
def ACIR_ObligationState
    : I32EnumAttr<"ObligationState", "rule obligation lifecycle",
                  [ACIR_ObligationState_Pending,
                   ACIR_ObligationState_Materialized,
                   ACIR_ObligationState_Discharged]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_ObligationStateAttr
    : EnumAttr<ACIR_Dialect, ACIR_ObligationState, "obligation_state">;

def ACIR_ObligationResolver_Handshake
    : I32EnumAttrCase<"Handshake", 0, "handshake">;
def ACIR_ObligationResolver_Checks : I32EnumAttrCase<"Checks", 1, "checks">;
def ACIR_ObligationResolver_Schedule
    : I32EnumAttrCase<"Schedule", 2, "schedule">;
def ACIR_ObligationResolver
    : I32EnumAttr<"ObligationResolver", "named rule obligation resolver",
                  [ACIR_ObligationResolver_Handshake,
                   ACIR_ObligationResolver_Checks,
                   ACIR_ObligationResolver_Schedule]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_ObligationResolverAttr
    : EnumAttr<ACIR_Dialect, ACIR_ObligationResolver, "obligation_resolver">;

def ACIR_ActivationResourceKind_InputQueue
    : I32EnumAttrCase<"InputQueue", 0, "input_queue">;
def ACIR_ActivationResourceKind_OutputQueue
    : I32EnumAttrCase<"OutputQueue", 1, "output_queue">;
def ACIR_ActivationResourceKind_State : I32EnumAttrCase<"State", 2, "state">;
def ACIR_ActivationResourceKind
    : I32EnumAttr<"ActivationResourceKind",
                  "typed rule activation/transaction resource kind",
                  [ACIR_ActivationResourceKind_InputQueue,
                   ACIR_ActivationResourceKind_OutputQueue,
                   ACIR_ActivationResourceKind_State]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_ActivationResourceKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_ActivationResourceKind,
               "activation_resource_kind">;

def ACIR_RuleGuardKind_Always : I32EnumAttrCase<"Always", 0, "always">;
def ACIR_RuleGuardKind_Predicate
    : I32EnumAttrCase<"Predicate", 1, "predicate">;
def ACIR_RuleGuardKind
    : I32EnumAttr<"RuleGuardKind", "typed rule guard kind",
                  [ACIR_RuleGuardKind_Always,
                   ACIR_RuleGuardKind_Predicate]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_RuleGuardKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_RuleGuardKind, "rule_guard_kind">;

def ACIR_RuleScheduleKind_Independent
    : I32EnumAttrCase<"Independent", 0, "independent">;
def ACIR_RuleScheduleKind_LexicalPriority
    : I32EnumAttrCase<"LexicalPriority", 1, "lexical_priority">;
def ACIR_RuleScheduleKind
    : I32EnumAttr<"RuleScheduleKind", "typed rule schedule kind",
                  [ACIR_RuleScheduleKind_Independent,
                   ACIR_RuleScheduleKind_LexicalPriority]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_RuleScheduleKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_RuleScheduleKind, "rule_schedule_kind">;

def ACIR_RuleCheckKind_InputAvailable
    : I32EnumAttrCase<"InputAvailable", 0, "input_available">;
def ACIR_RuleCheckKind_OutputCapacity
    : I32EnumAttrCase<"OutputCapacity", 1, "output_capacity">;
def ACIR_RuleCheckKind
    : I32EnumAttr<"RuleCheckKind", "typed inferred rule check kind",
                  [ACIR_RuleCheckKind_InputAvailable,
                   ACIR_RuleCheckKind_OutputCapacity]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_RuleCheckKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_RuleCheckKind, "rule_check_kind">;

def ACIR_RuleEffectKind_InputConsume
    : I32EnumAttrCase<"InputConsume", 0, "input_consume">;
def ACIR_RuleEffectKind_OutputProduce
    : I32EnumAttrCase<"OutputProduce", 1, "output_produce">;
def ACIR_RuleEffectKind_StateRead
    : I32EnumAttrCase<"StateRead", 2, "state_read">;
def ACIR_RuleEffectKind_StateWrite
    : I32EnumAttrCase<"StateWrite", 3, "state_write">;
def ACIR_RuleEffectKind
    : I32EnumAttr<"RuleEffectKind", "typed inferred rule effect kind",
                  [ACIR_RuleEffectKind_InputConsume,
                   ACIR_RuleEffectKind_OutputProduce,
                   ACIR_RuleEffectKind_StateRead,
                   ACIR_RuleEffectKind_StateWrite]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_RuleEffectKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_RuleEffectKind, "rule_effect_kind">;

def ACIR_RuleOutputPresenceKind_Always
    : I32EnumAttrCase<"Always", 0, "always">;
def ACIR_RuleOutputPresenceKind_Predicate
    : I32EnumAttrCase<"Predicate", 1, "predicate">;
def ACIR_RuleOutputPresenceKind
    : I32EnumAttr<"RuleOutputPresenceKind", "typed selected output presence",
                  [ACIR_RuleOutputPresenceKind_Always,
                   ACIR_RuleOutputPresenceKind_Predicate]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_RuleOutputPresenceKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_RuleOutputPresenceKind,
               "rule_output_presence_kind">;

def ACIR_RuleStateAccessKind_Read
    : I32EnumAttrCase<"Read", 0, "read">;
def ACIR_RuleStateAccessKind_Replace
    : I32EnumAttrCase<"Replace", 1, "replace">;
def ACIR_RuleStateAccessKind_FieldWrite
    : I32EnumAttrCase<"FieldWrite", 2, "field_write">;
def ACIR_RuleStateAccessKind
    : I32EnumAttr<"RuleStateAccessKind", "typed rule state access kind",
                  [ACIR_RuleStateAccessKind_Read,
                   ACIR_RuleStateAccessKind_Replace,
                   ACIR_RuleStateAccessKind_FieldWrite]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_RuleStateAccessKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_RuleStateAccessKind,
               "rule_state_access_kind">;

def ACIR_RuleIndexKind_Static
    : I32EnumAttrCase<"Static", 0, "static">;
def ACIR_RuleIndexKind_Dynamic
    : I32EnumAttrCase<"Dynamic", 1, "dynamic">;
def ACIR_RuleIndexKind_All : I32EnumAttrCase<"All", 2, "all">;
def ACIR_RuleIndexKind
    : I32EnumAttr<"RuleIndexKind", "typed state footprint index kind",
                  [ACIR_RuleIndexKind_Static,
                   ACIR_RuleIndexKind_Dynamic,
                   ACIR_RuleIndexKind_All]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_RuleIndexKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_RuleIndexKind, "rule_index_kind">;

def ACIR_RuleArbitrationKind_LexicalPriority
    : I32EnumAttrCase<"LexicalPriority", 0, "lexical_priority">;
def ACIR_RuleArbitrationKind
    : I32EnumAttr<"RuleArbitrationKind", "typed rule arbitration kind",
                  [ACIR_RuleArbitrationKind_LexicalPriority]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::acir::ac";
}
def ACIR_RuleArbitrationKindAttr
    : EnumAttr<ACIR_Dialect, ACIR_RuleArbitrationKind,
               "rule_arbitration_kind">;

#endif // ACIR_DIALECT_ACIR_ACIRATTRIBUTES_TD
