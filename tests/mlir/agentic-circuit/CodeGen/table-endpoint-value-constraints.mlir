// RUN: %acir_opt %s -ac-verify-value-constraints -o /dev/null
// RUN: %acir_opt %s -ac-freeze-topology -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bounded_endpoints"} {
  ac.table @entries entry i2 entries 5 init 0 owner "/" stable_id "table/entries"

  %read_input = ac.source depth 1 latency 1 {ac.name = "read_input"} : !ac.queue<i2>
  %read_output = ac.table.read @entries, %read_input : !ac.queue<i2> depth 1 latency 1 address {
  ^address(%index: !ac.var<i2>):
    ac.table.yield %index : !ac.var<i2>
  } when {
  ^when(%index: !ac.var<i2>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.table.yield %true : !ac.var<i1>
  } {ac.endpoint_path = "/read", ac.name = "read"} -> !ac.queue<i2>
  ac.sink %read_output {ac.name = "read_sink"} : !ac.queue<i2>

  %write_input = ac.source depth 1 latency 1 {ac.name = "write_input"} : !ac.queue<i2>
  ac.table.write @entries, %write_input : !ac.queue<i2> mode "replace" write_fields ["$entry"] address {
  ^address(%index: !ac.var<i2>):
    ac.table.yield %index : !ac.var<i2>
  } enable {
  ^enable(%item: !ac.var<i2>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.table.yield %true : !ac.var<i1>
  } value {
  ^value(%item: !ac.var<i2>):
    ac.table.yield %item : !ac.var<i2>
  } {ac.endpoint_path = "/write", ac.name = "write"}

  ac.table @flags entry i1 entries 5 init 0 owner "/" stable_id "table/flags"
  %get_input = ac.source depth 1 latency 1 {ac.name = "get_input"} : !ac.queue<i2>
  %get_output = ac.table.read @flags, %get_input : !ac.queue<i2> depth 1 latency 1 address {
  ^address(%index: !ac.var<i2>):
    %zero = ac.var.constant 0 : i2 as !ac.var<i2>
    ac.table.yield %zero : !ac.var<i2>
  } when {
  ^when(%index: !ac.var<i2>):
    %present = ac.table.get @flags[%index] : !ac.var<i2> -> !ac.var<i1>
    ac.table.yield %present : !ac.var<i1>
  } {ac.endpoint_path = "/get", ac.name = "get"} -> !ac.queue<i1>
  ac.sink %get_output {ac.name = "get_sink"} : !ac.queue<i1>
}

// PLAN: "kind":"table_read"
// PLAN-SAME: "yields":["item"
// PLAN: "kind":"table_write"
// PLAN-SAME: "yields":["item"
// PLAN: "kind":"table_get"
