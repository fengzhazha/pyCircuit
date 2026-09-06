// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "branch_join"} {
  ac.type_scope @types {
    ac.struct @Command fields [{name = "direct", type = i1}, {name = "left_index", type = i2}, {name = "right_index", type = i2}, {name = "value", type = i8}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Command> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.table @entries entry i8 entries 4 init 0 owner "/" stable_id "table/entries"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Command>>
  ac.rule %input depths [] latencies [] name "update" stable_id "update_0"
      domain "cycle" type exact input_fact committed_input {
  ^body(%item: !ac.var<!ac.struct<@types::@Command>>):
    %candidate = ac.var.constant true as !ac.var<i1>
    %direct = ac.var.get %item field "direct" : !ac.var<!ac.struct<@types::@Command>> -> !ac.var<i1>
    %left_index = ac.var.get %item field "left_index" : !ac.var<!ac.struct<@types::@Command>> -> !ac.var<i2>
    %right_index = ac.var.get %item field "right_index" : !ac.var<!ac.struct<@types::@Command>> -> !ac.var<i2>
    %value = ac.var.get %item field "value" : !ac.var<!ac.struct<@types::@Command>> -> !ac.var<i8>
    %one = ac.var.constant 1 : i8 as !ac.var<i8>
    %incremented = ac.var.add %value, %one : !ac.var<i8>
    %joined_value = ac.var.select %direct, %value, %incremented : !ac.var<i1>, !ac.var<i8> -> !ac.var<i8>
    %joined_index = ac.var.select %direct, %right_index, %left_index : !ac.var<i1>, !ac.var<i2> -> !ac.var<i2>
    ac.rule.condition %candidate : !ac.var<i1>
    ac.table.propose @entries[%joined_index] = %joined_value mode "replace"
        write_fields ["$entry"] : !ac.var<i2>, !ac.var<i8>
    ac.rule.return
  } : (!ac.queue<!ac.struct<@types::@Command>>) -> ()
}

// LOWERED: %[[VALUE:[^ ]+]] = ac.var.select %{{.*}}, %{{.*}}, %{{.*}} : !ac.var<i1>, !ac.var<i8> -> !ac.var<i8>
// LOWERED: %[[INDEX:[^ ]+]] = ac.var.select %{{.*}}, %{{.*}}, %{{.*}} : !ac.var<i1>, !ac.var<i2> -> !ac.var<i2>
// LOWERED: ac.table.propose @entries[%[[INDEX]]] = %[[VALUE]] when %[[PRESENT:[^ ]+]] : !ac.var<i1>

// PLAN-COUNT-2: "kind":"value_select"
// PLAN: "state_writes":[{"fields":["$entry"],"index":"v{{[0-9]+}}","mode":"replace","present":"v{{[0-9]+}}","table":"entries","value":"v{{[0-9]+}}"}]

// GFSIM-COUNT-2: auto v{{[0-9]+}} = v{{[0-9]+}} ? v{{[0-9]+}} : v{{[0-9]+}};
