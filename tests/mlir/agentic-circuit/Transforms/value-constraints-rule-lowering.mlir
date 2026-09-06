// RUN: %acir_opt %s -ac-lower-rules | %FileCheck %s
// RUN: %acir_opt %s --pass-pipeline='builtin.module(ac-lower-rules,ac-freeze-topology)' -o /dev/null

builtin.module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bounded_table"} {
  ac.table @entries entry i2 entries 5 init 0 owner "/" stable_id "table/entries"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i2>
  %output = ac.rule %input depths [1] latencies [1]
      name "bounded_table" stable_id "bounded_table" domain "cycle"
      type exact input_fact committed_input {
  ^body(%index: !ac.var<i2>):
    %old = ac.table.get @entries[%index] : !ac.var<i2> -> !ac.var<i2>
    ac.table.propose @entries[%index] = %index mode "replace"
        write_fields ["$entry"] : !ac.var<i2>, !ac.var<i2>
    %ready = ac.marker.obligation %old state pending resolver handshake
        origin "bounded_table:return" path "true" : !ac.var<i2>
    ac.rule.return %ready : !ac.var<i2>
  } {ac.name = "output"} : (!ac.queue<i2>) -> !ac.queue<i2>
  ac.sink %output {ac.name = "sink"} : !ac.queue<i2>
}

// CHECK-NOT: ac.rule
// CHECK: ac.firing
// CHECK: ac.table.get @entries
// CHECK: ac.table.propose @entries
