// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "branch_local"} {
  ac.type_scope @types {
    ac.struct @Command fields [{name = "select_right", type = i1}, {name = "value", type = i8}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Command> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.table @left entry i8 entries 1 init 0 owner "/" stable_id "table/left"
  ac.table @right entry i8 entries 1 init 0 owner "/" stable_id "table/right"
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<!ac.struct<@types::@Command>>
  ac.rule %input depths [] latencies [] name "route" stable_id "route_0"
      domain "cycle" type exact input_fact committed_input {
  ^body(%item: !ac.var<!ac.struct<@types::@Command>>):
    %candidate = ac.var.constant true as !ac.var<i1>
    %select_right = ac.var.get %item field "select_right" : !ac.var<!ac.struct<@types::@Command>> -> !ac.var<i1>
    %value = ac.var.get %item field "value" : !ac.var<!ac.struct<@types::@Command>> -> !ac.var<i8>
    %false = ac.var.constant false as !ac.var<i1>
    %select_left = ac.var.cmp "eq" %select_right, %false : !ac.var<i1> -> !ac.var<i1>
    ac.rule.condition %candidate : !ac.var<i1>
    ac.table.propose @right[%false] = %value when %select_right : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.table.propose @left[%false] = %value when %select_left : !ac.var<i1>
        mode "replace" write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
    ac.rule.return
  } : (!ac.queue<!ac.struct<@types::@Command>>) -> ()
}

// LOWERED: ac.firing %{{.*}} depths [] latencies []
// LOWERED: ac.firing.condition %[[CANDIDATE:[^ ]+]] : !ac.var<i1>
// LOWERED: ac.table.propose @right[%{{.*}}] = %{{.*}} when %[[RIGHT:[^ ]+]] : !ac.var<i1>
// LOWERED: ac.table.propose @left[%{{.*}}] = %{{.*}} when %[[LEFT:[^ ]+]] : !ac.var<i1>
// LOWERED: ac.firing.yield

// PLAN: "state_writes":[{"fields":["$entry"],"index":"v{{[0-9]+}}","mode":"replace","present":"v{{[0-9]+}}","table":"right"
// PLAN-SAME: {"fields":["$entry"],"index":"v{{[0-9]+}}","mode":"replace","present":"v{{[0-9]+}}","table":"left"

// GFSIM: proposal_present1 ? std::optional<std::pair<size_t, gfsim::UInt<8>>>
// GFSIM-SAME: proposal_present0 ? std::optional<std::pair<size_t, gfsim::UInt<8>>>
