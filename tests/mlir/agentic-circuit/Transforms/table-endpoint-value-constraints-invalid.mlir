// RUN: %split_file %s %t
// RUN: %acir_opt %t/read.mlir -ac-verify-value-constraints -verify-diagnostics
// RUN: %acir_opt %t/read.mlir -ac-freeze-topology -verify-diagnostics
// RUN: %acir_opt %t/write.mlir -ac-verify-value-constraints -verify-diagnostics
// RUN: %acir_opt %t/write.mlir -ac-freeze-topology -verify-diagnostics
// RUN: %acir_opt %t/get.mlir -ac-verify-value-constraints -verify-diagnostics
// RUN: %acir_opt %t/get.mlir -ac-freeze-topology -verify-diagnostics

//--- read.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "unsafe_read"} {
  ac.table @entries entry i2 entries 5 init 0 owner "/" stable_id "table/entries"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i3>
  // expected-error @+1 {{cannot prove Table read address index is within [0, 4]; inferred interval[0,7]}}
  %output = ac.table.read @entries, %input : !ac.queue<i3> depth 1 latency 1 address {
  ^address(%index: !ac.var<i3>):
    ac.table.yield %index : !ac.var<i3>
  } when {
  ^when(%index: !ac.var<i3>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.table.yield %true : !ac.var<i1>
  } {ac.endpoint_path = "/read", ac.name = "read"} -> !ac.queue<i2>
  ac.sink %output {ac.name = "sink"} : !ac.queue<i2>
}

//--- write.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "unsafe_write"} {
  ac.table @entries entry i2 entries 5 init 0 owner "/" stable_id "table/entries"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i3>
  // expected-error @+1 {{cannot prove Table write address index is within [0, 4]; inferred interval[0,7]}}
  ac.table.write @entries, %input : !ac.queue<i3> mode "replace" write_fields ["$entry"] address {
  ^address(%index: !ac.var<i3>):
    ac.table.yield %index : !ac.var<i3>
  } enable {
  ^enable(%item: !ac.var<i3>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.table.yield %true : !ac.var<i1>
  } value {
  ^value(%item: !ac.var<i3>):
    %zero = ac.var.constant 0 : i2 as !ac.var<i2>
    ac.table.yield %zero : !ac.var<i2>
  } {ac.endpoint_path = "/write", ac.name = "write"}
}

//--- get.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "unsafe_get"} {
  ac.table @entries entry i1 entries 5 init 0 owner "/" stable_id "table/entries"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i3>
  %output = ac.table.read @entries, %input : !ac.queue<i3> depth 1 latency 1 address {
  ^address(%index: !ac.var<i3>):
    %zero = ac.var.constant 0 : i3 as !ac.var<i3>
    ac.table.yield %zero : !ac.var<i3>
  } when {
  ^when(%index: !ac.var<i3>):
    // expected-error @+1 {{cannot prove Table index is within [0, 4]; inferred interval[0,7]}}
    %present = ac.table.get @entries[%index] : !ac.var<i3> -> !ac.var<i1>
    ac.table.yield %present : !ac.var<i1>
  } {ac.endpoint_path = "/read", ac.name = "read"} -> !ac.queue<i1>
  ac.sink %output {ac.name = "sink"} : !ac.queue<i1>
}
