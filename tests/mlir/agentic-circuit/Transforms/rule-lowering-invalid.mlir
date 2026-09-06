// RUN: %split_file %s %t
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-materialize-rule-handshake)' %t/order.mlir 2>&1 | %FileCheck %s --check-prefix=ORDER
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types,ac-infer-rule-effects,ac-materialize-rule-checks,ac-materialize-rule-handshake)' %t/dead-handshake.mlir 2>&1 | %FileCheck %s --check-prefix=DEAD
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-discharge-rule-obligations)' %t/pending.mlir 2>&1 | %FileCheck %s --check-prefix=PENDING
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types,ac-infer-rule-effects,ac-materialize-rule-checks,ac-materialize-rule-handshake,ac-discharge-rule-obligations,ac-resolve-rule-schedule)' %t/duplicate.mlir 2>&1 | %FileCheck %s --check-prefix=DUPLICATE
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types)' %t/unrelated-type-marker.mlir 2>&1 | %FileCheck %s --check-prefix=TYPE-ORIGIN
// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-effects)' %t/bad-value-identity.mlir 2>&1 | %FileCheck %s --check-prefix=VALUE-IDENTITY
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-rule-closure)' %t/forged-proof.mlir 2>&1 | %FileCheck %s --check-prefix=FORGED-PROOF

//--- order.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad_order"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.rule %input depths [1] latencies [1]
      name "identity" stable_id "identity_0" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i32>):
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "identity:return" path "true" : !ac.var<i32>
    ac.rule.return %ready : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}
// ORDER: requires 'ac.rule.checks_typed' before handshake materialization

//--- dead-handshake.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "dead_handshake"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.rule %input depths [1] latencies [1]
      name "identity" stable_id "identity_0" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i32>):
    %unused = ac.marker.obligation %item state pending resolver handshake
        origin "identity:return" path "true" : !ac.var<i32>
    ac.rule.return %item : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}
// DEAD: handshake obligation must wrap the value returned by ac.rule.return

//--- pending.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "pending"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.rule %input depths [1] latencies [1]
      name "identity" stable_id "identity_0" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i32>):
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "identity:return" path "true" : !ac.var<i32>
    ac.rule.return %ready : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}
// PENDING: must be materialized by its named resolver before discharge

//--- duplicate.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "duplicate"} {
  %left = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %right = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %left_out = ac.rule %left depths [1] latencies [1]
      name "left" stable_id "same" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i32>):
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "left:return" path "true" : !ac.var<i32>
    ac.rule.return %ready : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
  %right_out = ac.rule %right depths [1] latencies [1]
      name "right" stable_id "same" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i32>):
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "right:return" path "true" : !ac.var<i32>
    ac.rule.return %ready : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %left_out : !ac.queue<i32>
  ac.sink %right_out : !ac.queue<i32>
}
// DUPLICATE: duplicate stable rule identity 'same'

//--- unrelated-type-marker.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad_type_marker"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.rule %input depths [1] latencies [1]
      name "identity" stable_id "identity_0" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i32>):
    %zero = ac.var.constant 0 : i32 as !ac.var<i32>
    %typed = ac.marker.type %zero state unknown
        : !ac.var<i32>
    %ready = ac.marker.obligation %typed state pending resolver handshake
        origin "identity:return" path "true" : !ac.var<i32>
    ac.rule.return %ready : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}
// TYPE-ORIGIN: phase-one Queue payload inference must refine a rule input

//--- bad-value-identity.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad_value_identity"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.rule %input depths [1] latencies [1]
      name "identity" stable_id "identity_0" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i32>):
    %fact = ac.marker.value %item identity "other"
        path "true" : !ac.var<i32>
    %ready = ac.marker.obligation %fact state pending resolver handshake
        origin "identity:return" path "true" : !ac.var<i32>
    ac.rule.return %ready : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}
// VALUE-IDENTITY: committed input 0 requires identity 'input'

//--- forged-proof.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "forged"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.transform %input depths [1] latencies [1] {
  ^body(%item: !ac.var<i32>):
    ac.transform.yield %item : !ac.var<i32>
  } {ac.rule_stable_id = "forged/rule"} : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}
// FORGED-PROOF: requires non-empty lowered-rule proof 'ac.rule_definition'
