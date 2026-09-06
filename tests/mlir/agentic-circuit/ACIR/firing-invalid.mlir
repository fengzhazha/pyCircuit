// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/arity.mlir 2>&1 | %FileCheck %s --check-prefix=ARITY
// RUN: %not %acir_opt %t/effectless.mlir 2>&1 | %FileCheck %s --check-prefix=EFFECTLESS
// RUN: %not %acir_opt %t/payload.mlir 2>&1 | %FileCheck %s --check-prefix=PAYLOAD
// RUN: %not %acir_opt %t/domain.mlir 2>&1 | %FileCheck %s --check-prefix=DOMAIN
// RUN: %not %acir_opt %t/output-presence.mlir 2>&1 | %FileCheck %s --check-prefix=OUTPUT-PRESENCE
// RUN: %not %acir_opt %t/optional-output-candidate.mlir 2>&1 | %FileCheck %s --check-prefix=OPTIONAL-OUTPUT
// RUN: %not %acir_opt %t/presence-does-not-imply.mlir 2>&1 | %FileCheck %s --check-prefix=IMPLIES
// RUN: %not %acir_opt %t/zero-input-divergence.mlir 2>&1 | %FileCheck %s --check-prefix=DIVERGENCE
// RUN: %not %acir_opt %t/multiple-effect-predicates.mlir 2>&1 | %FileCheck %s --check-prefix=DIVERGENCE
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-verify-rule-closure)' %t/missing-snapshot.mlir 2>&1 | %FileCheck %s --check-prefix=SNAPSHOT
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-verify-rule-closure)' %t/extra-snapshot.mlir 2>&1 | %FileCheck %s --check-prefix=SNAPSHOT
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-rule-closure,ac-freeze-topology)' %t/forged-contract.mlir 2>&1 | %FileCheck %s --check-prefix=FORGED

//--- arity.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %a, %b = ac.firing %input depths [1] latencies [1]
      stable_id "bad" domain "cycle" guard "true" checks []
      handshake "ready_valid_1x1" schedule "independent"
      effects ["input.consume", "output.produce"] {
  ^body(%item: !ac.var<i32>):
    ac.firing.yield %item, %item : !ac.var<i32>, !ac.var<i32>
  } : (!ac.queue<i32>) -> (!ac.queue<i32>, !ac.queue<i32>)
}
// ARITY: currently supports at most one output Queue

//--- effectless.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.firing %input depths [1] latencies [1]
      stable_id "bad" domain "cycle" guard "true" checks []
      handshake "ready_valid_1x1" schedule "independent" effects [] {
  ^body(%item: !ac.var<i32>):
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.yield %item : !ac.var<i32>
  } {ac.rule_footprints = [], ac.rule_priority = 0 : i64} : (!ac.queue<i32>) -> !ac.queue<i32>
}
// EFFECTLESS: requires explicit identity, guard, handshake, schedule, and effects

//--- payload.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.firing %input depths [1] latencies [1]
      stable_id "bad" domain "cycle" guard "true" checks []
      handshake "ready_valid_1x1" schedule "independent"
      effects ["input.consume", "output.produce"] {
  ^body(%item: !ac.var<i32>):
    %small = ac.var.constant 1 : i16 as !ac.var<i16>
    ac.firing.yield %small : !ac.var<i16>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
}
// PAYLOAD: yielded values must match output Queue payloads

//--- domain.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.firing %input depths [1] latencies [1]
      stable_id "bad" domain "bogus" guard "true" checks []
      handshake "ready_valid_1x1" schedule "independent"
      effects ["input.consume", "output.produce"] {
  ^body(%item: !ac.var<i32>):
    ac.firing.yield %item : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
}
// DOMAIN: phase-one firing requires exact time domain 'cycle'

//--- output-presence.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.firing %input depths [1] latencies [1]
      stable_id "bad" domain "cycle" guard "true" checks []
      handshake "ready_valid_1x1" schedule "independent"
      effects ["input.consume", "output.produce"] {
  ^body(%item: !ac.var<i32>):
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.output %item when %enabled ordinal 1 : !ac.var<i32>, !ac.var<i1>
    ac.firing.yield %item : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
}
// OUTPUT-PRESENCE: 'ac.firing.output' op ordinal must name one firing output

//--- optional-output-candidate.mlir
module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.firing %input depths [1] latencies [1]
      stable_id "bad" domain "cycle" guard "dynamic" checks []
      handshake "ready_valid_1x1" schedule "independent"
      effects ["input.consume", "output.produce"] {
  ^body(%item: !ac.var<i32>):
    %candidate = ac.var.constant false as !ac.var<i1>
    %present = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %candidate : !ac.var<i1>
    ac.firing.output %item when %present ordinal 0 : !ac.var<i32>, !ac.var<i1>
    ac.firing.yield %item : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
}

// OPTIONAL-OUTPUT: optional output presence requires one input and a true candidate

//--- presence-does-not-imply.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  ac.firing %input depths [] latencies [] stable_id "bad" domain "cycle"
      guard "dynamic" checks [] handshake "ready_valid_1x0_table"
      schedule "table_lexical_priority"
      effects ["input.consume", "table.replace:state"] {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant false as !ac.var<i1>
    %candidate = ac.var.constant false as !ac.var<i1>
    %present = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %candidate : !ac.var<i1>
    ac.table.propose @state[%index] = %item when %present : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.firing.yield
  } : (!ac.queue<i8>) -> ()
}
// IMPLIES: state proposal presence must imply the firing condition

//--- zero-input-divergence.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  ac.firing depths [] latencies [] stable_id "bad" domain "cycle"
      guard "true" checks [] handshake "ready_valid_0x0_table"
      schedule "table_lexical_priority" effects ["table.replace:state"] {
  ^body:
    %index = ac.var.constant false as !ac.var<i1>
    %value = ac.var.constant 1 : i8 as !ac.var<i8>
    %candidate = ac.var.constant true as !ac.var<i1>
    %present = ac.var.constant false as !ac.var<i1>
    ac.firing.condition %candidate : !ac.var<i1>
    ac.table.propose @state[%index] = %value when %present : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.firing.yield
  } : () -> ()
}
// DIVERGENCE: conditional-effect presence

//--- multiple-effect-predicates.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @left entry i8 entries 1 init 0 owner "/" stable_id "table/left"
  ac.table @right entry i8 entries 1 init 0 owner "/" stable_id "table/right"
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  ac.firing %input depths [] latencies [] stable_id "bad" domain "cycle"
      guard "true" checks [] handshake "ready_valid_1x0_table"
      schedule "table_lexical_priority"
      effects ["input.consume", "table.replace:left", "table.replace:right"] {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant false as !ac.var<i1>
    %candidate = ac.var.constant true as !ac.var<i1>
    %left_present = ac.var.constant false as !ac.var<i1>
    %right_present = ac.var.constant false as !ac.var<i1>
    ac.firing.condition %candidate : !ac.var<i1>
    ac.table.propose @left[%index] = %item when %left_present : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.table.propose @right[%index] = %item when %right_present : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.firing.yield
  } : (!ac.queue<i8>) -> ()
}

//--- forged-contract.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "forged"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.firing %input depths [1] latencies [1]
      stable_id "forged" domain "cycle" guard "not-a-guard"
      checks ["not-a-check"] handshake "bogus" schedule "implicit-priority"
      effects ["unknown.effect"] {
  ^body(%item: !ac.var<i32>):
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.yield %item : !ac.var<i32>
  } {ac.rule_footprints = [], ac.rule_priority = 0 : i64} : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}
// FORGED: has invalid phase-one guard/checks/handshake/schedule/effects contract

//--- missing-snapshot.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  ac.firing %input depths [] latencies [] stable_id "missing_snapshot"
      domain "cycle" guard "true" checks [] handshake "ready_valid_1x0_table"
      schedule "table_lexical_priority"
      effects ["input.consume", "table.replace:state"] {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant false as !ac.var<i1>
    %old = ac.table.get @state[%index] : !ac.var<i1> -> !ac.var<i8>
    %fresh = ac.var.cmp "eq" %old, %item : !ac.var<i8> -> !ac.var<i1>
    %candidate = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %candidate : !ac.var<i1>
    ac.table.propose @state[%index] = %item when %fresh : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.firing.yield
  } : (!ac.queue<i8>) -> ()
}
// SNAPSHOT: state snapshot evidence must exactly match predicate reads

//--- extra-snapshot.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  ac.firing %input depths [] latencies [] stable_id "extra_snapshot"
      domain "cycle" guard "true" checks [] handshake "ready_valid_1x0_table"
      schedule "table_lexical_priority"
      effects ["input.consume", "table.replace:state"] {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant false as !ac.var<i1>
    %candidate = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %candidate : !ac.var<i1>
    ac.table.propose @state[%index] = %item when %candidate : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.state.snapshot @state[%index : !ac.var<i1>] for %candidate : !ac.var<i1>
        kind #ac<rule_index_kind static> read_fields ["$entry"]
    ac.firing.yield
  } : (!ac.queue<i8>) -> ()
}
