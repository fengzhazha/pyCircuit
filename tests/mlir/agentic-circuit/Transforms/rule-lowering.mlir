// RUN: %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types)' %s | %FileCheck %s --check-prefix=TYPE
// RUN: %acir_opt --pass-pipeline='builtin.module(canonicalize,cse)' %s | %FileCheck %s --check-prefix=MARKERS
// RUN: %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types,ac-infer-rule-effects,ac-materialize-rule-checks,ac-materialize-rule-handshake)' %s | %FileCheck %s --check-prefix=MATERIALIZED
// RUN: %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types,ac-infer-rule-effects,ac-infer-rule-activation,ac-materialize-rule-checks,ac-materialize-rule-handshake,ac-discharge-rule-obligations,ac-resolve-rule-schedule,ac-lower-rules-to-firing)' %s | %FileCheck %s --check-prefix=FIRING
// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen
// RUN: %FileCheck %s --check-prefix=FROZEN < %t.frozen
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-freeze-topology)' %t.frozen | %FileCheck %s --check-prefix=FROZEN
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-freeze-topology)' %s 2>&1 | %FileCheck %s --check-prefix=UNRESOLVED

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "rule_test"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  ac.observe %input name "input_probe" : !ac.queue<i32>
  %output = ac.rule %input depths [2] latencies [1]
      name "increment" stable_id "top/increment_0" domain "cycle"
      type exact {
  ^rule(%item: !ac.var<i32>):
    %typed = ac.marker.type %item state unknown
        : !ac.var<i32>
    %fact = ac.marker.value %typed identity "input"
        path "true" : !ac.var<i32>
    %one = ac.var.constant 1 : i32 as !ac.var<i32>
    %sum = ac.var.add %fact, %one : !ac.var<i32>
    %ready = ac.marker.obligation %sum state pending resolver handshake
        origin "increment:return" path "true" : !ac.var<i32>
    ac.rule.return %ready : !ac.var<i32>
  } {ac.name = "output"} : (!ac.queue<i32>) -> !ac.queue<i32>
  ac.sink %output : !ac.queue<i32>
}

// TYPE-NOT: ac.marker.type
// TYPE: ac.marker.value %{{.*}} identity "input" path "true"

// MARKERS: ac.marker.type
// MARKERS: ac.marker.value
// MARKERS: ac.marker.obligation

// MATERIALIZED: ac.marker.obligation %{{.*}} state materialized resolver handshake

// FIRING-NOT: ac.rule
// FIRING-NOT: ac.marker
// FIRING-NOT: functional_guard
// FIRING-NOT: handshake
// FIRING-NOT: schedule =
// FIRING-NOT: effects =
// FIRING: ac.firing %{{.*}} depths [2] latencies [1]
// FIRING: ac.firing.yield

// LOWERED-NOT: ac.rule
// LOWERED-NOT: ac.firing
// LOWERED-NOT: ac.marker
// LOWERED-NOT: ac.rule_guard =
// LOWERED-NOT: ac.rule_checks =
// LOWERED-NOT: ac.rule_handshake =
// LOWERED-NOT: ac.rule_schedule =
// LOWERED-NOT: ac.rule_effects =
// LOWERED: ac.transform
// LOWERED: ac.rule_definition = "increment"
// LOWERED-SAME: ac.rule_stable_id = "top/increment_0"
// LOWERED-SAME: ac.rule_time_domain = "cycle"

// FROZEN: module attributes {
// FROZEN-SAME: ac.freeze_epoch = "0.5"
// FROZEN-SAME: ac.frozen_owners = []
// FROZEN-SAME: ac.topology_digest = "{{[0-9a-f]+}}"
// FROZEN-SAME: ac.topology_frozen = true
// FROZEN: ac.rule_stable_id = "top/increment_0"

// UNRESOLVED: unresolved transient rule or typed marker before Frozen ACIR
