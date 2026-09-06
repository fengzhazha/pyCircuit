// RUN: %acir_opt %s -ac-verify-value-constraints | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "constraints"} {
  ac.type_scope @types {
    ac.enum @Mode enumerants ["idle", "run"]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.enum<@types::@Mode> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}>}
  %idle = ac.var.enum @types::@Mode "idle" : !ac.var<!ac.enum<@types::@Mode>>
  %run = ac.var.enum @types::@Mode "run" : !ac.var<!ac.enum<@types::@Mode>>
  %mode_changed = ac.var.cmp "ne" %idle, %run : !ac.var<!ac.enum<@types::@Mode>> -> !ac.var<i1>
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [5]

  // The type domain of u2 is [0,3], so it safely addresses five entries.
  %u2 = ac.source depth 1 latency 1 : !ac.queue<i2>
  %from_u2 = ac.transform %u2 depths [1] latencies [1] {
  ^body(%index: !ac.var<i2>):
    %value = ac.var.read_element @state[%index] : !ac.var<i2> -> !ac.var<i8>
    ac.transform.yield %value : !ac.var<i8>
  } : (!ac.queue<i2>) -> !ac.queue<i8>
  ac.sink %from_u2 : !ac.queue<i8>

  // Constant, select, mask, and priority-encode constraints are propagated.
  %u3 = ac.source depth 1 latency 1 : !ac.queue<i3>
  %from_mask = ac.transform %u3 depths [1] latencies [1] {
  ^body(%raw: !ac.var<i3>):
    %three = ac.var.constant 3 : i3 as !ac.var<i3>
    %four = ac.var.constant 4 : i3 as !ac.var<i3>
    %masked = ac.var.and %raw, %three : !ac.var<i3>
    %condition = ac.var.cmp "ult" %raw, %four : !ac.var<i3> -> !ac.var<i1>
    %selected = ac.var.select %condition, %three, %four : !ac.var<i1>, !ac.var<i3> -> !ac.var<i3>
    %selected_value = ac.var.read_element @state[%selected] : !ac.var<i3> -> !ac.var<i8>
    %masked_value = ac.var.read_element @state[%masked] : !ac.var<i3> -> !ac.var<i8>
    ac.transform.yield %masked_value : !ac.var<i8>
  } : (!ac.queue<i3>) -> !ac.queue<i8>
  ac.sink %from_mask : !ac.queue<i8>

  %bits = ac.source depth 1 latency 1 : !ac.queue<i5>
  %from_priority = ac.transform %bits depths [1] latencies [1] {
  ^body(%mask: !ac.var<i5>):
    %index, %valid = ac.var.priority_encode %mask order "low" : !ac.var<i5> -> !ac.var<i3>, !ac.var<i1>
    %value = ac.var.read_element @state[%index] : !ac.var<i3> -> !ac.var<i8>
    ac.transform.yield %value : !ac.var<i8>
  } : (!ac.queue<i5>) -> !ac.queue<i8>
  ac.sink %from_priority : !ac.queue<i8>

  ac.table @entries entry i3 entries 5 init 0 owner "/" stable_id "table/entries"
  %requests = ac.source depth 1 latency 1 : !ac.queue<i3>
  %responses = ac.rule %requests depths [1] latencies [1]
      name "bounded_table" stable_id "bounded_table" domain "cycle"
      type exact {
  ^body(%raw_index: !ac.var<i3>):
    %four = ac.var.constant 4 : i3 as !ac.var<i3>
    %index = ac.var.and %raw_index, %four : !ac.var<i3>
    %old = ac.table.get @entries[%index] : !ac.var<i3> -> !ac.var<i3>
    ac.table.propose @entries[%index] = %raw_index mode "replace"
        write_fields ["$entry"] : !ac.var<i3>, !ac.var<i3>
    %ready = ac.marker.obligation %old state pending resolver handshake
        origin "bounded_table:return" path "true" : !ac.var<i3>
    ac.rule.return %ready : !ac.var<i3>
  } : (!ac.queue<i3>) -> !ac.queue<i3>
  ac.sink %responses : !ac.queue<i3>
}

// CHECK: ac.var.cmp "ne" %{{.*}}, %{{.*}} : !ac.var<!ac.enum<@types::@Mode>>
// CHECK: ac.var.read_element @state
// CHECK: ac.var.priority_encode
// CHECK: ac.table.propose @entries
