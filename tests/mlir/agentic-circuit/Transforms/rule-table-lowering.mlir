// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s | %FileCheck %s --check-prefix=FROZEN
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o
// RUN: %not %acir_queue_pycgen %t.frozen.mlir 2>&1 | %FileCheck %s --check-prefix=PYC-ERR

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "rule_table"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i1}, {name = "value", type = i7}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 8 : i64}>}
  ac.table @rob entry !ac.struct<@types::@Entry> entries 2 init 0 owner "/" stable_id "table/rob"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Entry>>
  %output = ac.rule %input depths [1] latencies [1]
      name "install" stable_id "install_0" domain "cycle"
      type exact input_fact committed_input {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
    %index = ac.var.get %item field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
    %old = ac.table.get @rob [%index] : !ac.var<i1> -> !ac.var<!ac.struct<@types::@Entry>>
    ac.table.propose @rob [%index] = %item mode "replace"
        write_fields ["index", "value"] : !ac.var<i1>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %old state pending resolver handshake
        origin "install:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } {ac.name = "output"} : (!ac.queue<!ac.struct<@types::@Entry>>) -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %output {ac.name = "sink"} : !ac.queue<!ac.struct<@types::@Entry>>
}

// LOWERED-NOT: ac.rule
// LOWERED-NOT: ac.marker
// LOWERED: ac.firing
// LOWERED-SAME: handshake "ready_valid_1x1_table"
// LOWERED-SAME: schedule "table_lexical_priority"
// LOWERED-SAME: effects ["input.consume", "output.produce", "table.replace:rob"]
// LOWERED: ac.table.propose @rob
// LOWERED: ac.firing.yield
// LOWERED: ac.rule_footprints = [{access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "dynamic", resource = @rob}, {access = "replace", fields = ["index", "value"], guard_kind = #ac<rule_guard_kind always>, index_kind = "dynamic", resource = @rob}]
// LOWERED-SAME: ac.rule_priority = 0 : i64
// LOWERED-NOT: ac.transform

// FROZEN: ac.topology_frozen = true
// FROZEN: ac.firing
// FROZEN: ac.table.propose @rob

// GFSIM: struct block_0_policy
// GFSIM: std::optional<gfsim::TableTransitionPlan<Entry, Entry>>
// GFSIM: auto [proposal_index, proposal_value, proposal_present, output_value0, output_present0, condition]
// GFSIM-COUNT-1: table_rob->at
// GFSIM: gfsim::TableWriteMode::Replace
// GFSIM: gfsim::QueueTableTransition<block_0_policy, Entry

// PYC-ERR: unsupported provisional Table
