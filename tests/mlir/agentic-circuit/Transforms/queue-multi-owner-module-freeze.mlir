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
    sym_name = "multi_owner_reuse",
    root = @Top,
    root_name = "root",
    tick_epoch = 0 : i64,
    tick_unit = "cycle",
    seed_policy = {kind = "fixed", value = 0 : i64},
    instrumentation = [],
    result_schema = {id = "default", format = "json"},
    selected = true
  }> : () -> ()

  ac.module @StatePair(%input: !ac.queue<i8>) -> (!ac.queue<i8>)
      parameters {} graph {
    %output = ac.scope @logic(%input) {
    ^bb0(%borrowed: !ac.queue<i8>):
      ac.table @cursor entry i8 entries 1 init 0 owner "/logic"
          stable_id "table/logic/cursor"
      ac.table @total entry i8 entries 1 init 0 owner "/logic"
          stable_id "table/logic/total"
      %result = ac.firing %borrowed depths [2] latencies [1]
          stable_id "update" domain "cycle" guard "true" checks []
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
        ac.name = "output",
        ac.rule_definition = "update",
        ac.rule_footprints = [
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @cursor},
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @total},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @cursor},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @total}
        ],
        ac.rule_priority = 0 : i64
      } : (!ac.queue<i8>) -> !ac.queue<i8>
      ac.scope.yield %result : !ac.queue<i8>
    } : (!ac.queue<i8>) -> !ac.queue<i8>
    ac.return %output : !ac.queue<i8>
  }

  ac.module @Top() parameters {} graph {
    %left_input, %right_input = ac.scope @inputs() {
      %left = ac.source depth 2 latency 1 {ac.name = "left_input"}
          : !ac.queue<i8>
      %right = ac.source depth 2 latency 1 {ac.name = "right_input"}
          : !ac.queue<i8>
      ac.scope.yield %left, %right : !ac.queue<i8>, !ac.queue<i8>
    } : () -> (!ac.queue<i8>, !ac.queue<i8>)
    %left_output = ac.instance @left of @StatePair(%left_input) static {}
        id "left" path "left" : (!ac.queue<i8>) -> !ac.queue<i8>
    %right_output = ac.instance @right of @StatePair(%right_input) static {}
        id "right" path "right" : (!ac.queue<i8>) -> !ac.queue<i8>
    ac.scope @outputs(%left_output, %right_output) {
    ^bb0(%left: !ac.queue<i8>, %right: !ac.queue<i8>):
      ac.sink %left {ac.name = "left_sink"} : !ac.queue<i8>
      ac.sink %right {ac.name = "right_sink"} : !ac.queue<i8>
      ac.scope.yield
    } : (!ac.queue<i8>, !ac.queue<i8>) -> ()
    ac.return
  }
}

// PLAN: "module_specializations":[{
// PLAN-SAME: "state_writes":[{"fields":["$entry"]
// PLAN-SAME: "table":"cursor"
// PLAN-SAME: {"fields":["$entry"]
// PLAN-SAME: "table":"total"
// PLAN-SAME: "definition":"StatePair"
// PLAN-SAME: "tables":[{"entries":1
// PLAN-SAME: "name":"cursor"
// PLAN-SAME: {"entries":1
// PLAN-SAME: "name":"total"

// CXX: gfsim::StateTransitionPlan<std::tuple<gfsim::UInt<8>, gfsim::UInt<8>>
// CXX-COUNT-1: class [[IMPLEMENTATION:StatePair_[0-9a-f]+]] final : public gfsim::Module
// CXX: gfsim::SimTable<gfsim::UInt<8>> table_0_;
// CXX: gfsim::SimTable<gfsim::UInt<8>> table_1_;
// CXX: gfsim::QueueStateTransition<[[IMPLEMENTATION]]_block_0_policy
// CXX: class MultiOwnerReuse final : public gfsim::Module
// CXX-COUNT-2: [[IMPLEMENTATION]] instance_
