// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/outside.mlir 2>&1 | %FileCheck %s --check-prefix=OUTSIDE
// RUN: %not %acir_opt %t/dynamic-bounds.mlir -ac-verify-value-constraints 2>&1 | %FileCheck %s --check-prefix=BOUNDS
// RUN: %not %acir_opt %t/field-mode.mlir 2>&1 | %FileCheck %s --check-prefix=MODE
// RUN: %acir_opt %t/type-mismatch.mlir | %FileCheck %s --check-prefix=HETERO
// RUN: %not %acir_opt %t/unsafe-read.mlir -ac-verify-value-constraints 2>&1 | %FileCheck %s --check-prefix=READ-BOUNDS
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-canonicalize-pure-firings,ac-verify-rule-closure)' %t/forged-pure-firing.mlir 2>&1 | %FileCheck %s --check-prefix=FORGED
// RUN: %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types,ac-infer-rule-effects,ac-materialize-rule-checks,ac-materialize-rule-handshake,ac-discharge-rule-obligations,ac-resolve-rule-schedule)' %t/write-conflict.mlir | %FileCheck %s --check-prefix=SHARED-SCHEDULE

//--- outside.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @table entry i8 entries 1 init 0 owner "/" stable_id "table/table"
  %zero = ac.var.constant 0 : i1 as !ac.var<i1>
  %value = ac.var.constant 0 : i8 as !ac.var<i8>
  ac.table.propose @table [%zero] = %value mode "replace"
      write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
}
// OUTSIDE: 'ac.table.propose' op must be nested directly in ac.rule or ac.firing

//--- dynamic-bounds.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "dynamic_bounds"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i2}, {name = "value", type = i7}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 8 : i64}>}
  ac.table @table entry !ac.struct<@types::@Entry> entries 3 init 0 owner "/" stable_id "table/table"
  %input = ac.source depth 1 latency 1 : !ac.queue<!ac.struct<@types::@Entry>>
  %output = ac.rule %input depths [1] latencies [1]
      name "install" stable_id "install" domain "cycle"
      type exact {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
    %index = ac.var.get %item field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i2>
    ac.table.propose @table [%index] = %item mode "replace"
        write_fields ["index", "value"] : !ac.var<i2>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "install:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } : (!ac.queue<!ac.struct<@types::@Entry>>) -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %output : !ac.queue<!ac.struct<@types::@Entry>>
}
// BOUNDS: 'ac.table.propose' op cannot prove Table index is within [0, 2]; inferred interval[0,3]

//--- field-mode.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "field_mode"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i1}, {name = "value", type = i7}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 8 : i64}>}
  ac.table @table entry !ac.struct<@types::@Entry> entries 2 init 0 owner "/" stable_id "table/table"
  %input = ac.source depth 1 latency 1 : !ac.queue<!ac.struct<@types::@Entry>>
  %output = ac.rule %input depths [1] latencies [1]
      name "patch" stable_id "patch" domain "cycle"
      type exact {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
    %index = ac.var.get %item field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
    ac.table.propose @table [%index] = %item mode "field"
        write_fields ["value"] : !ac.var<i1>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "patch:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } : (!ac.queue<!ac.struct<@types::@Entry>>) -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %output : !ac.queue<!ac.struct<@types::@Entry>>
}
// MODE: 'ac.table.propose' op stateful rule phase one supports replace mode only

//--- write-conflict.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "write_conflict"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "index", type = i1}, {name = "value", type = i7}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 8 : i64}>}
  ac.table @table entry !ac.struct<@types::@Entry> entries 2 init 0 owner "/" stable_id "table/table"
  %input = ac.source depth 1 latency 1 : !ac.queue<!ac.struct<@types::@Entry>>
  %output = ac.rule %input depths [1] latencies [1]
      name "install" stable_id "install" domain "cycle"
      type exact {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
    %index = ac.var.get %item field "index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
    ac.table.propose @table [%index] = %item mode "replace"
        write_fields ["index", "value"] : !ac.var<i1>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "install:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } : (!ac.queue<!ac.struct<@types::@Entry>>) -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.table.write @table mode "field" write_fields ["value"] address {
    %zero = ac.var.constant 0 : i1 as !ac.var<i1>
    ac.table.yield %zero : !ac.var<i1>
  } enable {
    %true = ac.var.constant true as !ac.var<i1>
    ac.table.yield %true : !ac.var<i1>
  } value {
    %zero = ac.var.constant 0 : i1 as !ac.var<i1>
    %old = ac.table.get @table [%zero] : !ac.var<i1> -> !ac.var<!ac.struct<@types::@Entry>>
    %one = ac.var.constant 1 : i7 as !ac.var<i7>
    %next = ac.var.with %old, %one field "value" : !ac.var<!ac.struct<@types::@Entry>>, !ac.var<i7> -> !ac.var<!ac.struct<@types::@Entry>>
    ac.table.yield %next : !ac.var<!ac.struct<@types::@Entry>>
  }
  ac.sink %output : !ac.queue<!ac.struct<@types::@Entry>>
}
// SHARED-SCHEDULE: ac.rule
// SHARED-SCHEDULE: ac.rule.arbitration_membership = [{priority = 0 : i64, resource = @table}]
// SHARED-SCHEDULE: ac.rule.footprints = [{access = "replace"
// SHARED-SCHEDULE-SAME: resource = @table
// SHARED-SCHEDULE: ac.rule.priority = 0 : i64
// SHARED-SCHEDULE-SAME: ac.rule.schedule_kind = #ac<rule_schedule_kind lexical_priority>

//--- type-mismatch.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "type_mismatch"} {
  ac.table @table entry i8 entries 1 init 0 owner "/" stable_id "table/table"
  %input = ac.source depth 1 latency 1 : !ac.queue<i16>
  %output = ac.rule %input depths [1] latencies [1]
      name "bad" stable_id "bad" domain "cycle"
      type exact {
  ^body(%item: !ac.var<i16>):
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    %value = ac.var.constant 0 : i8 as !ac.var<i8>
    ac.table.propose @table [%index] = %value mode "replace"
        write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "bad:return" path "true" : !ac.var<i16>
    ac.rule.return %ready : !ac.var<i16>
  } : (!ac.queue<i16>) -> !ac.queue<i16>
  ac.sink %output : !ac.queue<i16>
}
// HETERO: ac.table.propose @table
// HETERO: ac.rule.return %{{.*}} : !ac.var<i16>

//--- unsafe-read.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "unsafe_read"} {
  ac.type_scope @types {
    ac.struct @Entry fields [{name = "write_index", type = i1}, {name = "read_index", type = i2}, {name = "value", type = i7}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Entry> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 8 : i64}>}
  ac.table @table entry !ac.struct<@types::@Entry> entries 2 init 0 owner "/" stable_id "table/table"
  %input = ac.source depth 1 latency 1 : !ac.queue<!ac.struct<@types::@Entry>>
  %output = ac.rule %input depths [1] latencies [1]
      name "bad_read" stable_id "bad_read" domain "cycle"
      type exact {
  ^body(%item: !ac.var<!ac.struct<@types::@Entry>>):
    %write_index = ac.var.get %item field "write_index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i1>
    %read_index = ac.var.get %item field "read_index" : !ac.var<!ac.struct<@types::@Entry>> -> !ac.var<i2>
    %old = ac.table.get @table [%read_index] : !ac.var<i2> -> !ac.var<!ac.struct<@types::@Entry>>
    ac.table.propose @table [%write_index] = %item mode "replace"
        write_fields ["write_index", "read_index", "value"] : !ac.var<i1>, !ac.var<!ac.struct<@types::@Entry>>
    %ready = ac.marker.obligation %old state pending resolver handshake
        origin "bad_read:return" path "true" : !ac.var<!ac.struct<@types::@Entry>>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Entry>>
  } : (!ac.queue<!ac.struct<@types::@Entry>>) -> !ac.queue<!ac.struct<@types::@Entry>>
  ac.sink %output : !ac.queue<!ac.struct<@types::@Entry>>
}
// READ-BOUNDS: 'ac.table.get' op cannot prove Table index is within [0, 1]; inferred interval[0,3]

//--- forged-pure-firing.mlir
module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "forged_pure"} {
  ac.table @table entry i8 entries 1 init 0 owner "/" stable_id "table/table"
  %input = ac.source depth 1 latency 1 : !ac.queue<i8>
  %output = ac.firing %input depths [1] latencies [1]
      stable_id "forged" domain "cycle" {
  ^body(%item: !ac.var<i8>):
    %index = ac.var.constant 0 : i1 as !ac.var<i1>
    %enabled = ac.var.constant true as !ac.var<i1>
    ac.firing.condition %enabled : !ac.var<i1>
    ac.table.propose @table [%index] = %item mode "replace"
        write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.firing.yield %item : !ac.var<i8>
  } {ac.activation_sources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind state>, resource = @table}], ac.arbitration_membership = [{priority = 0 : i64, resource = @table}], ac.checks_typed = [{guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_check_kind input_available>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_check_kind output_capacity>, ordinal = 0 : i64}], ac.effects_typed = [{guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind input_consume>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind output_produce>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind state_write>, resource = @table}], ac.guard_kind = #ac<rule_guard_kind always>, ac.initially_active = false, ac.output_presence = [{ordinal = 0 : i64, presence_kind = #ac<rule_output_presence_kind always>}], ac.rule_footprints = [{access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @table}], ac.rule_priority = 0 : i64, ac.schedule_kind = #ac<rule_schedule_kind independent>, ac.state_accesses = [{fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = #ac<rule_index_kind static>, kind = #ac<rule_state_access_kind replace>, resource = @table}], ac.transaction_resources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind state>, resource = @table}]} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output : !ac.queue<i8>
}
// FORGED: typed guard/schedule evidence does not match the body
