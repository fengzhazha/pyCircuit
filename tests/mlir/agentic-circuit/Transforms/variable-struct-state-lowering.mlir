// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-variable-state)' %s | %FileCheck %s --check-prefix=STORAGE
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "variable_struct_state"} {
  ac.type_scope @types {
    ac.struct @State fields [{name = "value", type = i8}, {name = "valid", type = i1}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@State> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.var.decl @state type !ac.struct<@types::@State> init 0 : i64 owner "/" stable_id "var/state"
  %input = ac.source depth 2 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@State>>
  %output = ac.rule %input depths [1] latencies [1]
      name "update" stable_id "update_0" domain "cycle"
      type exact input_fact committed_input {
  ^body(%item: !ac.var<!ac.struct<@types::@State>>):
    %old = ac.var.read @state : !ac.var<!ac.struct<@types::@State>>
    %value = ac.var.get %old field "value" : !ac.var<!ac.struct<@types::@State>> -> !ac.var<i8>
    %incoming = ac.var.get %item field "value" : !ac.var<!ac.struct<@types::@State>> -> !ac.var<i8>
    %sum = ac.var.add %value, %incoming : !ac.var<i8>
    %updated = ac.var.with %old, %sum field "value" : !ac.var<!ac.struct<@types::@State>>, !ac.var<i8> -> !ac.var<!ac.struct<@types::@State>>
    ac.var.assign @state = %updated : !ac.var<!ac.struct<@types::@State>>
    %ready = ac.marker.obligation %updated state pending resolver handshake origin "update:return" path "true" : !ac.var<!ac.struct<@types::@State>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@State>>
  } {ac.name = "output"} : (!ac.queue<!ac.struct<@types::@State>>) -> !ac.queue<!ac.struct<@types::@State>>
  ac.sink %output {ac.name = "sink"} : !ac.queue<!ac.struct<@types::@State>>
}

// STORAGE: ac.table @state entry !ac.struct<@types::@State> entries 1 init 0
// STORAGE: ac.table.propose @state
// STORAGE-SAME: write_fields ["value", "valid"]
