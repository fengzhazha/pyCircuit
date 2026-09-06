// RUN: %not %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types,ac-infer-rule-effects,ac-materialize-rule-checks)' %s 2>&1 | %FileCheck %s

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "checked_rule"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.rule %input depths [1] latencies [1]
      name "checked" stable_id "checked_0" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i32>):
    %checked = ac.marker.obligation %item state pending resolver checks
        origin "checked:input" path "true" : !ac.var<i32>
    %ready = ac.marker.obligation %checked state pending resolver handshake
        origin "checked:return" path "true" : !ac.var<i32>
    ac.rule.return %ready : !ac.var<i32>
  } : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}

// CHECK: dynamic checks are not executable in the phase-one pure rule subset
