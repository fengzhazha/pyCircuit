// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/count.mlir 2>&1 | %FileCheck %s --check-prefix=COUNT
// RUN: %not %acir_opt %t/resource.mlir 2>&1 | %FileCheck %s --check-prefix=EXACT
// RUN: %not %acir_opt %t/access.mlir 2>&1 | %FileCheck %s --check-prefix=EXACT
// RUN: %not %acir_opt %t/index.mlir 2>&1 | %FileCheck %s --check-prefix=EXACT
// RUN: %not %acir_opt %t/fields.mlir 2>&1 | %FileCheck %s --check-prefix=EXACT
// RUN: %not %acir_opt %t/duplicate.mlir 2>&1 | %FileCheck %s --check-prefix=DUPLICATE

//--- count.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad_count"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = ac.source depth 1 latency 1 : !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    ac.table.propose @state[%index] = %item mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.yield %item : !ac.var<i8>
  } {ac.rule_footprints = [], ac.rule_priority = 0 : i64} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output : !ac.queue<i8>
}
// COUNT: inferred footprint count must match state operations

//--- resource.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad_resource"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = ac.source depth 1 latency 1 : !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    ac.table.propose @state[%index] = %item mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.yield %item : !ac.var<i8>
  } {ac.rule_footprints = [{access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @other}], ac.rule_priority = 0 : i64} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output : !ac.queue<i8>
}
// EXACT: inferred footprint must exactly match its state operation

//--- access.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad_access"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = ac.source depth 1 latency 1 : !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    ac.table.propose @state[%index] = %item mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.yield %item : !ac.var<i8>
  } {ac.rule_footprints = [{access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @state}], ac.rule_priority = 0 : i64} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output : !ac.queue<i8>
}

//--- index.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad_index"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = ac.source depth 1 latency 1 : !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    ac.table.propose @state[%index] = %item mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.yield %item : !ac.var<i8>
  } {ac.rule_footprints = [{access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "dynamic", resource = @state}], ac.rule_priority = 0 : i64} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output : !ac.queue<i8>
}

//--- fields.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bad_fields"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = ac.source depth 1 latency 1 : !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    ac.table.propose @state[%index] = %item mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.yield %item : !ac.var<i8>
  } {ac.rule_footprints = [{access = "replace", fields = [], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @state}], ac.rule_priority = 0 : i64} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output : !ac.queue<i8>
}

//--- duplicate.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "duplicate"} {
  ac.table @state entry i8 entries 1 init 0 owner "/" stable_id "table/state"
  %input = ac.source depth 1 latency 1 : !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1] stable_id "bad" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    ac.table.propose @state[%index] = %item mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.table.propose @state[%index] = %item mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.firing.yield %item : !ac.var<i8>
  } {ac.rule_footprints = [{access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @state}, {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @state}], ac.rule_priority = 0 : i64} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output : !ac.queue<i8>
}
// DUPLICATE: first multi-state firing slice permits one proposal per owner
