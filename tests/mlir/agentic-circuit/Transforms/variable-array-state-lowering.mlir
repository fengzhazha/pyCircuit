// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-variable-state)' %s | %FileCheck %s --check-prefix=STORAGE
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-variable-state,ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "variable_array_state"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i2}, {name = "value", type = i8}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.var.decl @entries type !ac.struct<@types::@Entry> init 0 : i64 owner "/" stable_id "var/entries" shape [4]
  %ready_mask = ac.var.match @entries predicate {
  ^bb0(%entry: !ac.var<!ac.struct<@types::@Entry>>):
    %value = ac.var.get %entry field "value" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i8>
    %zero = ac.var.constant 0 : i8 as !ac.var<i8>
    %ready = ac.var.cmp "ne" %value, %zero : !ac.var<i8> -> !ac.var<i1>
    ac.var.match.yield %ready : !ac.var<i1>
  } {ac.query = "ready"} -> !ac.var<i4>
  %oldest, %oldest_valid = ac.var.choose @entries %ready_mask : !ac.var<i4> count 1 policy "min" key {
  ^bb0(%entry: !ac.var<!ac.struct<@types::@Entry>>):
    %index = ac.var.get %entry field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i2>
    ac.var.choose.yield %index : !ac.var<i2>
  } {ac.query = "oldest"} -> !ac.var<i2>, !ac.var<i1>
  %input = ac.source depth 2 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Entry>>
  %output = ac.rule %input depths [1] latencies [1]
      name "replace" stable_id "replace_0" domain "cycle"
      type exact {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
    %index = ac.var.get %item field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i2>
    %old = ac.var.read_element @entries[%index] : !ac.var<i2> -> !ac.var<!ac.struct<@types::@Entry>>
    ac.var.assign_element @entries[%index] = %item : !ac.var<i2>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %old state pending resolver handshake origin "replace:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } {ac.name = "output"} : (!ac.queue<!ac.struct<@types::@Entry>>) -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %output {ac.name = "sink"} : !ac.queue<!ac.struct<@types::@Entry>>
}

// STORAGE-NOT: ac.var.decl
// STORAGE-NOT: ac.var.read_element
// STORAGE-NOT: ac.var.assign_element
// STORAGE-NOT: ac.var.match
// STORAGE-NOT: ac.var.choose
// STORAGE: ac.table @entries entry !ac.struct<@types::@Entry> entries 4 init 0
// STORAGE: %[[MASK:.*]] = ac.table.match @entries predicate
// STORAGE: ac.table.match.yield
// STORAGE: } {ac.query = "ready"} -> !ac.var<i4>
// STORAGE: ac.table.choose @entries %[[MASK]] : !ac.var<i4> count 1 policy "min" key
// STORAGE: ac.table.choose.yield
// STORAGE: } {ac.query = "oldest"} -> !ac.var<i2>, !ac.var<i1>
// STORAGE: ac.table.get @entries[%{{.*}}]
// STORAGE: ac.table.propose @entries
// STORAGE-SAME: mode "replace" write_fields ["index", "value"]

// GFSIM: gfsim::SimTable<Entry>
// GFSIM: gfsim::QueueTableTransition<
