// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s > %t.lowered.mlir
// RUN: sed 's/ac.initially_active = true/ac.initially_active = false/' %t.lowered.mlir | %not %acir_opt 2>&1 | %FileCheck %s --check-prefix=INVALID
// RUN: sed 's/ac.guard_kind = #ac<rule_guard_kind predicate>/ac.guard_kind = #ac<rule_guard_kind always>/' %t.lowered.mlir | %not %acir_opt 2>&1 | %FileCheck %s --check-prefix=INVALID-TYPED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "state_driven"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i1}, {name = "value", type = i7}, {name = "valid", type = i1}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.table @entries entry !ac.struct<@types::@Entry> entries 2 init 0 owner "/" stable_id "table/entries"
  %retired = ac.rule depths [1] latencies [1]
      name "retire" stable_id "retire_0" domain "cycle"
      type exact {
  ^body:
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    %old = ac.table.get @entries[%index] : !ac.var<i1> -> !ac.var<!ac.struct<@types::@Entry>>
    %valid = ac.var.get %old field "valid" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
    ac.rule.condition %valid : !ac.var<i1>
    %false = ac.var.constant false as !ac.var<i1>
    %cleared = ac.var.with %old, %false field "valid" : !ac.var<!ac.struct<@types::@Entry>>, !ac.var<i1> -> !ac.var<!ac.struct<@types::@Entry>>
    ac.table.propose @entries[%index] = %cleared mode "replace"
        write_fields ["index", "value", "valid"] : !ac.var<i1>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %old state pending resolver handshake
        origin "retire:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } {ac.name = "retired"} : () -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %retired {ac.name = "sink"} : !ac.queue<!ac.struct<@types::@Entry>>
}

// LOWERED: %{{.*}} = ac.firing depths [1] latencies [1]
// LOWERED: ac.firing.condition %{{.*}} : !ac.var<i1>
// LOWERED: ac.table.propose @entries[%{{[0-9]+}}] = %{{[0-9]+}} when %[[PRESENT:[0-9]+]] : !ac.var<i1>
// LOWERED: ac.firing.output %{{.*}} when %[[PRESENT]] ordinal 0
// LOWERED: ac.activation_sources = [{kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind state>, resource = @entries}]
// LOWERED-SAME: ac.arbitration_membership = [{priority = 0 : i64, resource = @entries}]
// LOWERED-SAME: ac.checks_typed = [{guard_kind = #ac<rule_guard_kind predicate>, kind = #ac<rule_check_kind output_capacity>, ordinal = 0 : i64}]
// LOWERED-SAME: ac.guard_kind = #ac<rule_guard_kind predicate>
// LOWERED-SAME: ac.initially_active = true
// LOWERED-SAME: ac.output_presence = [{ordinal = 0 : i64, presence_kind = #ac<rule_output_presence_kind predicate>}]
// LOWERED-SAME: ac.schedule_kind = #ac<rule_schedule_kind lexical_priority>
// LOWERED-SAME: ac.transaction_resources = [{kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind state>, resource = @entries}]

// PLAN: "activation_sources":[{"kind":"output_queue"
// PLAN-SAME: "guard":"v{{[0-9]+}}"
// PLAN-SAME: "has_activation_evidence":true
// PLAN-SAME: "initially_active":true
// PLAN: "inputs":[]
// PLAN: "output_presence":[{"ordinal":0,"present":"v{{[0-9]+}}","value":"v{{[0-9]+}}"}]
// PLAN-SAME: "outputs":["retired"]
// PLAN: "state_writes":[{"fields":["index","value","valid"],"index":"v{{[0-9]+}}","mode":"replace","present":"v{{[0-9]+}}"

// GFSIM: if (!condition)
// GFSIM-NEXT: return std::nullopt;
// GFSIM: std::tuple<>, std::tuple<Entry>

// INVALID: error: 'ac.firing' op activation/transaction evidence must exactly match typed resources
// INVALID-TYPED: error: 'ac.firing' op typed guard/schedule evidence does not match the body
