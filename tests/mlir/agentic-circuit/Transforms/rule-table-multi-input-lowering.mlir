// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "rule_table_multi_input"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i1}, {name = "value", type = i7}]
    ac.struct @Delta fields [{name = "amount", type = i7}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 8 : i64}, !ac.struct<@types::@Delta> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}>}
  ac.table @rob entry !ac.struct<@types::@Entry> entries 2 init 0 owner "/" stable_id "table/rob"
  %entry = ac.source depth 2 latency 1 {ac.name = "entry"} : !ac.queue<!ac.struct<@types::@Entry>>
  %delta = ac.source depth 2 latency 1 {ac.name = "delta"} : !ac.queue<!ac.struct<@types::@Delta>>
  %output = ac.rule %entry, %delta depths [1] latencies [1]
      name "install" stable_id "install_0" domain "cycle"
      type exact input_fact committed_input {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>, %item1: !ac.var<!ac.struct<@types::@Delta>>):
    %index = ac.var.get %item field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
    %old = ac.table.get @rob [%index] : !ac.var<i1> -> !ac.var<!ac.struct<@types::@Entry>>
    %value = ac.var.get %item field "value" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i7>
    %amount = ac.var.get %item1 field "amount" : !ac.var<!ac.struct<@types::@Delta>> -> !ac.var<i7>
    %sum = ac.var.add %value, %amount : !ac.var<i7>
    %updated = ac.var.with %item, %sum field "value" : !ac.var<!ac.struct<@types::@Entry>>, !ac.var<i7> -> !ac.var<!ac.struct<@types::@Entry>>
    ac.table.propose @rob [%index] = %updated mode "replace"
        write_fields ["index", "value"] : !ac.var<i1>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %old state pending resolver handshake
        origin "install:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } {ac.name = "output"} : (!ac.queue<!ac.struct<@types::@Entry>>, !ac.queue<!ac.struct<@types::@Delta>>) -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %output {ac.name = "sink"} : !ac.queue<!ac.struct<@types::@Entry>>
}

// LOWERED-NOT: ac.rule
// LOWERED: = ac.firing
// LOWERED-SAME: handshake "ready_valid_2x1_table"
// LOWERED-SAME: schedule "table_lexical_priority"
// LOWERED: ac.table.propose @rob

// GFSIM: std::optional<gfsim::TableTransitionPlan<Entry, Entry>> operator()(gfsim::Epoch epoch, const gfsim::SimTable<Entry> &table_ref, const Entry &item, const Delta &item1)
// GFSIM: gfsim::QueueTableTransition<block_0_policy, Entry, std::tuple<Entry, Delta>, std::tuple<Entry>, block_0_merge_policy> block_0_;
