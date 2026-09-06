// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "optional_output"} {
  ac.type_scope @types {
    ac.struct @Event fields [{name = "emit", type = i1}, {name = "value", type = i8}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Event> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.table @count entry i8 entries 1 init 0 owner "/" stable_id "table/count"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Event>>
  %output = ac.rule %input depths [1] latencies [1] name "filter"
      stable_id "filter_0" domain "cycle" type exact
      input_fact committed_input {
  ^body(%item: !ac.var<!ac.struct<@types::@Event>>):
    %index = ac.var.constant false as !ac.var<i1>
    %old = ac.table.get @count[%index] : !ac.var<i1> -> !ac.var<i8>
    %emit = ac.var.get %item field "emit" : !ac.var<!ac.struct<@types::@Event>> -> !ac.var<i1>
    %candidate = ac.var.constant true as !ac.var<i1>
    %one = ac.var.constant 1 : i8 as !ac.var<i8>
    %next = ac.var.add %old, %one : !ac.var<i8>
    ac.rule.condition %candidate : !ac.var<i1>
    ac.table.propose @count[%index] = %next when %candidate : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    %ready = ac.marker.obligation %item state pending resolver handshake
        origin "filter:return" path "true" : !ac.var<!ac.struct<@types::@Event>>
    ac.rule.output %item when %emit ordinal 0 : !ac.var<!ac.struct<@types::@Event>>, !ac.var<i1>
    ac.rule.return %ready : !ac.var<!ac.struct<@types::@Event>>
  } {ac.name = "output"} : (!ac.queue<!ac.struct<@types::@Event>>) -> !ac.queue<!ac.struct<@types::@Event>>
  ac.sink %output {ac.name = "sink"} : !ac.queue<!ac.struct<@types::@Event>>
}

// LOWERED: ac.firing.condition %[[CANDIDATE:[^ ]+]] : !ac.var<i1>
// LOWERED: ac.table.propose @count[%{{.*}}] = %{{.*}} when %[[CANDIDATE]] : !ac.var<i1>
// LOWERED: ac.firing.output %{{.*}} when %[[OUTPUT:[^ ]+]] ordinal 0
// LOWERED: ac.checks_typed = [{{.*}}guard_kind = #ac<rule_guard_kind predicate>{{.*}}output_capacity
// LOWERED-SAME: ac.output_presence = [{ordinal = 0 : i64, presence_kind = #ac<rule_output_presence_kind predicate>}]

// PLAN: "output_presence":[{"ordinal":0,"present":"v{{[0-9]+}}","value":"item"}]
// PLAN: "state_writes":[{"fields":["$entry"],"index":"v{{[0-9]+}}","mode":"replace","present":"v{{[0-9]+}}","table":"count"

// GFSIM: output_present0 ? std::optional<Event>{output_value0} : std::optional<Event>{}
