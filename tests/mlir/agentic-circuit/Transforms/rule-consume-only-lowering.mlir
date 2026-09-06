// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "consume_only"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i1}, {name = "value", type = i7}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}>}
  ac.table @entries entry !ac.struct<@types::@Entry> entries 2 init 0 owner "/" stable_id "table/entries"
  ac.table @epoch entry i7 entries 1 init 0 owner "/" stable_id "table/epoch"
  %input = ac.source depth 2 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Entry>>
  ac.rule %input depths [] latencies []
      name "complete" stable_id "complete_0" domain "cycle"
      type exact {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
    %candidate = ac.var.constant true as !ac.var<i1>
    ac.rule.condition %candidate : !ac.var<i1>
    %index = ac.var.get %item field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
    %old = ac.table.get @entries[%index] : !ac.var<i1> -> !ac.var<!ac.struct<@types::@Entry>>
    %old_value = ac.var.get %old field "value" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i7>
    %zero = ac.var.constant 0 : i7 as !ac.var<i7>
    %zero_index = ac.var.constant false as !ac.var<i1>
    %old_epoch = ac.table.get @epoch[%zero_index] : !ac.var<i1> -> !ac.var<i7>
    %nonzero = ac.var.cmp "ne" %old_value, %zero : !ac.var<i7> -> !ac.var<i1>
    %epoch_matches = ac.var.cmp "eq" %old_value, %old_epoch : !ac.var<i7> -> !ac.var<i1>
    %fresh = ac.var.and %nonzero, %epoch_matches : !ac.var<i1>
    ac.table.propose @entries[%index] = %item when %fresh : !ac.var<i1> mode "replace"
        write_fields ["index", "value"] : !ac.var<i1>, !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return
  } : (!ac.queue<!ac.struct<@types::@Entry>>) -> ()
}

// LOWERED: ac.firing %{{.*}} depths [] latencies []
// LOWERED: ac.firing.condition %[[CANDIDATE:[0-9]+]] : !ac.var<i1>
// LOWERED: ac.table.propose @entries[%[[INDEX:[0-9]+]]] = %{{.*}} when %[[PRESENT:[0-9]+]] : !ac.var<i1>
// LOWERED: ac.state.snapshot @entries[%[[INDEX]] : !ac.var<i1>] for %[[PRESENT]] : !ac.var<i1> kind dynamic read_fields ["value"]
// LOWERED: ac.state.snapshot @epoch[%{{[0-9]+}} : !ac.var<i1>] for %[[PRESENT]] : !ac.var<i1> kind static read_fields ["$entry"]
// LOWERED: ac.firing.yield

// PLAN: "guard":"v{{[0-9]+}}"
// PLAN-SAME: "inputs":["input"]
// PLAN-SAME: "kind":"firing"
// PLAN: "outputs":[]
// PLAN: "state_reservations":[{"fields":["value"],"index":"v{{[0-9]+}}","index_kind":"dynamic","predicate":"v{{[0-9]+}}","source":"","table":"entries"},{"fields":["$entry"],"index":"v{{[0-9]+}}","index_kind":"static","predicate":"v{{[0-9]+}}","source":"","table":"epoch"}]
// PLAN: "state_writes":[{"fields":["index","value"],"index":"v{{[0-9]+}}","mode":"replace","present":"v{{[0-9]+}}"

// GFSIM: std::optional<gfsim::StateTransitionPlan<std::tuple<Entry, gfsim::UInt<7>>, std::tuple<>>>
// GFSIM: proposal_present
// GFSIM: std::optional<std::pair<size_t, gfsim::UInt<7>>>{}
// GFSIM: gfsim::StateReservation::forFields({{.*}}, std::uint64_t{2}, 2)
// GFSIM: gfsim::StateReservation({{.*}})
