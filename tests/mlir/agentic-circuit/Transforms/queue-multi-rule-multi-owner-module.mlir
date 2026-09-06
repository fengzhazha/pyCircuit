// RUN: %acir_opt --ac-freeze-topology %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=CXX < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -fsyntax-only %t.cpp

builtin.module attributes {
  ac.contract_epoch = "0.5",
  ac.model_kind = "queue_graph",
  ac.queue_graph_domain = "cycle"
} {
  "ac.system"() <{
    sym_name = "combined_reuse",
    root = @Top,
    root_name = "root",
    tick_epoch = 0 : i64,
    tick_unit = "cycle",
    seed_policy = {kind = "fixed", value = 0 : i64},
    instrumentation = [],
    result_schema = {id = "default", format = "json"},
    selected = true
  }> : () -> ()

  ac.module @DualState(%a: !ac.queue<i8>, %b: !ac.queue<i8>)
      -> (!ac.queue<i8>, !ac.queue<i8>) parameters {} graph {
    %out_a, %out_b = ac.scope @logic(%a, %b) {
    ^bb0(%input_a: !ac.queue<i8>, %input_b: !ac.queue<i8>):
      ac.table @cursor entry i8 entries 1 init 0 owner "/logic"
          stable_id "table/logic/cursor"
      ac.table @total entry i8 entries 1 init 0 owner "/logic"
          stable_id "table/logic/total"
      %result_a = ac.firing %input_a depths [2] latencies [1]
          stable_id "update_a" domain "cycle" guard "true" checks []
          handshake "ready_valid_1x1_table"
          schedule "table_lexical_priority"
          effects ["input.consume", "output.produce",
                   "table.replace:cursor", "table.replace:total"] {
      ^bb0(%item: !ac.var<i8>):
        %index = ac.var.constant 0 : i1 as !ac.var<i1>
        %cursor = ac.table.get @cursor[%index] : !ac.var<i1> -> !ac.var<i8>
        %total = ac.table.get @total[%index] : !ac.var<i1> -> !ac.var<i8>
        %one = ac.var.constant 1 : i8 as !ac.var<i8>
        %next_cursor = ac.var.add %cursor, %one : !ac.var<i8>
        %next_total = ac.var.add %total, %item : !ac.var<i8>
        %reported = ac.var.add %next_total, %next_cursor : !ac.var<i8>
        %enabled = ac.var.constant true as !ac.var<i1>
        ac.firing.condition %enabled : !ac.var<i1>
        ac.table.propose @cursor[%index] = %next_cursor when %enabled : !ac.var<i1> mode "replace"
            write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
        ac.table.propose @total[%index] = %next_total when %enabled : !ac.var<i1> mode "replace"
            write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
        ac.firing.output %reported when %enabled ordinal 0 : !ac.var<i8>, !ac.var<i1>
        ac.firing.yield %reported : !ac.var<i8>
      } {
        ac.name = "output_a",
        ac.rule_definition = "update_a",
        ac.rule_footprints = [
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @cursor},
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @total},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @cursor},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @total}
        ],
        ac.rule_priority = 0 : i64
      } : (!ac.queue<i8>) -> !ac.queue<i8>
      %result_b = ac.firing %input_b depths [2] latencies [1]
          stable_id "update_b" domain "cycle" guard "true" checks []
          handshake "ready_valid_1x1_table"
          schedule "table_lexical_priority"
          effects ["input.consume", "output.produce",
                   "table.replace:cursor", "table.replace:total"] {
      ^bb0(%item: !ac.var<i8>):
        %index = ac.var.constant 0 : i1 as !ac.var<i1>
        %cursor = ac.table.get @cursor[%index] : !ac.var<i1> -> !ac.var<i8>
        %total = ac.table.get @total[%index] : !ac.var<i1> -> !ac.var<i8>
        %one = ac.var.constant 1 : i8 as !ac.var<i8>
        %next_cursor = ac.var.add %cursor, %one : !ac.var<i8>
        %next_total = ac.var.add %total, %item : !ac.var<i8>
        %reported = ac.var.add %next_total, %next_cursor : !ac.var<i8>
        %enabled = ac.var.constant true as !ac.var<i1>
        ac.firing.condition %enabled : !ac.var<i1>
        ac.table.propose @cursor[%index] = %next_cursor when %enabled : !ac.var<i1> mode "replace"
            write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
        ac.table.propose @total[%index] = %next_total when %enabled : !ac.var<i1> mode "replace"
            write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
        ac.firing.output %reported when %enabled ordinal 0 : !ac.var<i8>, !ac.var<i1>
        ac.firing.yield %reported : !ac.var<i8>
      } {
        ac.name = "output_b",
        ac.rule_definition = "update_b",
        ac.rule_footprints = [
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @cursor},
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @total},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @cursor},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @total}
        ],
        ac.rule_priority = 1 : i64
      } : (!ac.queue<i8>) -> !ac.queue<i8>
      ac.scope.yield %result_a, %result_b : !ac.queue<i8>, !ac.queue<i8>
    } : (!ac.queue<i8>, !ac.queue<i8>) -> (!ac.queue<i8>, !ac.queue<i8>)
    ac.return %out_a, %out_b : !ac.queue<i8>, !ac.queue<i8>
  }

  ac.module @Top() parameters {} graph {
    %la, %lb, %ra, %rb = ac.scope @inputs() {
      %q0 = ac.source depth 2 latency 1 {ac.name = "left_a"} : !ac.queue<i8>
      %q1 = ac.source depth 2 latency 1 {ac.name = "left_b"} : !ac.queue<i8>
      %q2 = ac.source depth 2 latency 1 {ac.name = "right_a"} : !ac.queue<i8>
      %q3 = ac.source depth 2 latency 1 {ac.name = "right_b"} : !ac.queue<i8>
      ac.scope.yield %q0, %q1, %q2, %q3
          : !ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>
    } : () -> (!ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>)
    %left:2 = ac.instance @left of @DualState(%la, %lb) static {}
        id "left" path "left"
        : (!ac.queue<i8>, !ac.queue<i8>) -> (!ac.queue<i8>, !ac.queue<i8>)
    %right:2 = ac.instance @right of @DualState(%ra, %rb) static {}
        id "right" path "right"
        : (!ac.queue<i8>, !ac.queue<i8>) -> (!ac.queue<i8>, !ac.queue<i8>)
    ac.scope @outputs(%left#0, %left#1, %right#0, %right#1) {
    ^bb0(%o0: !ac.queue<i8>, %o1: !ac.queue<i8>,
         %o2: !ac.queue<i8>, %o3: !ac.queue<i8>):
      ac.sink %o0 {ac.name = "left_a_sink"} : !ac.queue<i8>
      ac.sink %o1 {ac.name = "left_b_sink"} : !ac.queue<i8>
      ac.sink %o2 {ac.name = "right_a_sink"} : !ac.queue<i8>
      ac.sink %o3 {ac.name = "right_b_sink"} : !ac.queue<i8>
      ac.scope.yield
    } : (!ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>) -> ()
    ac.return
  }
}

// PLAN: "module_specializations":[{
// PLAN-SAME: "name":"output_a"
// PLAN-SAME: "priority":0
// PLAN-SAME: "state_writes":[{"fields":["$entry"]
// PLAN-SAME: "table":"cursor"
// PLAN-SAME: "table":"total"
// PLAN-SAME: "name":"output_b"
// PLAN-SAME: "priority":1
// PLAN-SAME: "state_writes":[{"fields":["$entry"]
// PLAN-SAME: "table":"cursor"
// PLAN-SAME: "table":"total"

// CXX-COUNT-1: class [[IMPLEMENTATION:DualState_[0-9a-f]+]] final : public gfsim::Module
// CXX: gfsim::QueueStateTransition<[[IMPLEMENTATION]]_block_0_policy
// CXX: gfsim::QueueStateTransition<[[IMPLEMENTATION]]_block_1_policy
// CXX: class CombinedReuse final : public gfsim::Module
// CXX-COUNT-2: [[IMPLEMENTATION]] instance_
