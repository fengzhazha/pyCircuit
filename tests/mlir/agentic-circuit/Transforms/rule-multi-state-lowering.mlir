// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "multi_state"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i2}, {name = "value", type = i8}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.table @tail entry i2 entries 1 init 0 owner "/" stable_id "table/tail"
  ac.table @entries entry !ac.struct<@types::@Entry> entries 4 init 0 owner "/" stable_id "table/entries"
  %input = ac.source depth 2 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Entry>>
  %output = ac.rule %input depths [1] latencies [1]
      name "allocate" stable_id "allocate_0" domain "cycle"
      type exact input_fact committed_input {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
    %zero = ac.var.constant 0 : i1 as !ac.var<i1>
    %tail = ac.table.get @tail[%zero] : !ac.var<i1> -> !ac.var<i2>
    %one = ac.var.constant 1 : i2 as !ac.var<i2>
    %next = ac.var.add %tail, %one : !ac.var<i2>
    ac.table.propose @tail[%zero] = %next mode "replace"
        write_fields ["$entry"] : !ac.var<i1>, !ac.var<i2>
    ac.table.propose @entries[%tail] = %item mode "replace"
        write_fields ["index", "value"] : !ac.var<i2>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "allocate:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } {ac.name = "output"} : (!ac.queue<!ac.struct<@types::@Entry>>) -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %output {ac.name = "sink"} : !ac.queue<!ac.struct<@types::@Entry>>
}

// LOWERED: ac.firing
// LOWERED-SAME: effects ["input.consume", "output.produce", "table.replace:tail", "table.replace:entries"]
// LOWERED: ac.table.propose @tail
// LOWERED: ac.table.propose @entries
// LOWERED: ac.activation_sources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}
// LOWERED-SAME: ac.initially_active = false
// LOWERED-SAME: ac.rule_footprints = [
// LOWERED-SAME: ac.transaction_resources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}

// PLAN: "activation_sources":[{"kind":"input_queue"
// PLAN-SAME: "has_activation_evidence":true
// PLAN-SAME: "initially_active":false
// PLAN: "state_writes":[{"fields":["$entry"],"index":"v{{[0-9]+}}","mode":"replace","present":"v{{[0-9]+}}","table":"tail"
// PLAN-SAME: {"fields":["index","value"],"index":"v{{[0-9]+}}","mode":"replace","present":"v{{[0-9]+}}","table":"entries"

// GFSIM: gfsim::StateTransitionPlan<std::tuple<gfsim::UInt<2>, Entry>, std::tuple<Entry>>
// GFSIM: gfsim::QueueStateTransition<
// GFSIM-SAME: std::tuple<gfsim::UInt<2>, Entry>
