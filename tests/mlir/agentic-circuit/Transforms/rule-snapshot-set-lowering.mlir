// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "snapshot_set"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "tag", type = i2}, {name = "valid", type = i1}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}>}
  ac.table @entries entry !ac.struct<@types::@Entry> entries 4 init 0 owner "/" stable_id "table/entries"
  ac.table @ready entry i1 entries 4 init 0 owner "/" stable_id "table/ready"
  ac.table @priority entry i2 entries 4 init 0 owner "/" stable_id "table/priority"
  %output = ac.rule depths [1] latencies [1] name "issue" stable_id "issue_0"
      domain "cycle" type exact {
  ^body:
    %mask = ac.table.match @entries predicate {
    ^bb0(%entry: !ac.var<!ac.struct<@types::@Entry>>):
      %valid = ac.var.get %entry field "valid" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
      %tag = ac.var.get %entry field "tag" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i2>
      %is_ready = ac.table.get @ready[%tag] : !ac.var<i2> -> !ac.var<i1>
      %selected = ac.var.and %valid, %is_ready : !ac.var<i1>
      ac.table.match.yield %selected : !ac.var<i1>
    } -> !ac.var<i4>
    %index, %present = ac.table.choose @entries %mask : !ac.var<i4>
        count 1 policy "min" key {
    ^bb0(%entry: !ac.var<!ac.struct<@types::@Entry>>):
      %tag = ac.var.get %entry field "tag" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i2>
      %priority = ac.table.get @priority[%tag] : !ac.var<i2> -> !ac.var<i2>
      ac.table.choose.yield %priority : !ac.var<i2>
    } -> !ac.var<i2>, !ac.var<i1>
    %old = ac.table.get @entries[%index] : !ac.var<i2> -> !ac.var<!ac.struct<@types::@Entry>>
    %false = ac.var.constant false as !ac.var<i1>
    %cleared = ac.var.with %old, %false field "valid" : !ac.var<!ac.struct<@types::@Entry>>, !ac.var<i1> -> !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.condition %present : !ac.var<i1>
    ac.table.propose @entries[%index] = %cleared mode "replace"
        write_fields ["tag", "valid"] : !ac.var<i2>, !ac.var<!ac.struct<@types::@Entry>>
    %ready_output = ac.marker.obligation %old state pending resolver handshake
        origin "issue:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready_output : !ac.var<!ac.struct<@types::@Entry>>
  } {ac.name = "output"} : () -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %output {ac.name = "sink"} : !ac.queue<!ac.struct<@types::@Entry>>
}

// LOWERED: %[[MASK:[^ ]+]] = ac.table.match @entries predicate
// LOWERED: %[[INDEX:[^, ]+]], %[[PRESENT:[^ ]+]] = ac.table.choose @entries %[[MASK]]
// LOWERED: ac.firing.condition %[[PRESENT]] : !ac.var<i1>
// LOWERED: ac.state.snapshot @entries for %[[PRESENT]] : !ac.var<i1> kind all read_fields ["tag", "valid"]
// LOWERED: ac.state.snapshot_set @ready from %[[MASK]] : !ac.var<i4> for %[[PRESENT]] : !ac.var<i1> read_fields ["$entry"]
// LOWERED: ac.state.snapshot_set @priority from %[[INDEX]] : !ac.var<i2> for %[[PRESENT]] : !ac.var<i1> read_fields ["$entry"]

// PLAN: "state_reservations":[{"fields":["tag","valid"],"index":"","index_kind":"all","predicate":"v{{[0-9]+}}","source":"","table":"entries"},{"fields":["$entry"],"index":"","index_kind":"set","predicate":"v{{[0-9]+}}","source":"v{{[0-9]+}}","table":"ready"},{"fields":["$entry"],"index":"","index_kind":"set","predicate":"v{{[0-9]+}}","source":"v{{[0-9]+}}","table":"priority"}]

// GFSIM: std::uint64_t snapshot_set_1_0 = 0;
// GFSIM-COUNT-1: snapshot_set_1_0 |=
// GFSIM: std::uint64_t snapshot_set_2_0 = 0;
// GFSIM-COUNT-1: snapshot_set_2_0 |=
// GFSIM-NOT: snapshot_entry
// GFSIM: gfsim::StateReservation(((std::uint64_t{1} << 4) - 1))
// GFSIM-SAME: gfsim::StateReservation(snapshot_set_1_0)
// GFSIM-SAME: gfsim::StateReservation(snapshot_set_2_0)
