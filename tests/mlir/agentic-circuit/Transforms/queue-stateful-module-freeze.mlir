// RUN: %acir_opt --ac-freeze-topology %s -o %t.frozen.mlir
// RUN: %acir_opt --ac-freeze-topology %t.frozen.mlir | %FileCheck %s --check-prefix=FROZEN
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
    sym_name = "stateful_reuse",
    root = @Top,
    root_name = "root",
    tick_epoch = 0 : i64,
    tick_unit = "cycle",
    seed_policy = {kind = "fixed", value = 0 : i64},
    instrumentation = [],
    result_schema = {id = "default", format = "json"},
    selected = true
  }> : () -> ()

  ac.module @Accumulator(%input: !ac.queue<i8>) -> (!ac.queue<i8>)
      parameters {} graph {
    %output = ac.scope @logic(%input) {
    ^bb0(%borrowed: !ac.queue<i8>):
      ac.table @sum entry i8 entries 1 init 0 owner "/logic"
          stable_id "table/logic/sum"
      %next = ac.firing %borrowed depths [2] latencies [1]
          stable_id "accumulate" domain "cycle" guard "true" checks []
          handshake "ready_valid_1x1_table"
          schedule "table_lexical_priority"
          effects ["input.consume", "output.produce", "table.replace:sum"] {
      ^bb0(%item: !ac.var<i8>):
        %index = ac.var.constant 0 : i1 as !ac.var<i1>
        %old = ac.table.get @sum[%index] : !ac.var<i1> -> !ac.var<i8>
        %value = ac.var.add %old, %item : !ac.var<i8>
        %enabled = ac.var.constant true as !ac.var<i1>
        ac.firing.condition %enabled : !ac.var<i1>
        ac.table.propose @sum[%index] = %value when %enabled : !ac.var<i1> mode "replace"
            write_fields ["$entry"] : !ac.var<i1>, !ac.var<i8>
        ac.firing.output %value when %enabled ordinal 0 : !ac.var<i8>, !ac.var<i1>
        ac.firing.yield %value : !ac.var<i8>
      } {
        ac.name = "module_output",
        ac.rule_definition = "accumulate",
        ac.rule_footprints = [
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @sum},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @sum}
        ],
        ac.rule_priority = 0 : i64
      } : (!ac.queue<i8>) -> !ac.queue<i8>
      ac.scope.yield %next : !ac.queue<i8>
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
    %left_output = ac.instance @left of @Accumulator(%left_input) static {}
        id "left" path "left" : (!ac.queue<i8>) -> !ac.queue<i8>
    %right_output = ac.instance @right of @Accumulator(%right_input) static {}
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

// FROZEN: ac.module @Accumulator
// FROZEN-SAME: ac.definition_fingerprint = "[[DEFINITION:sha256:[0-9a-f]{64}]]"
// FROZEN: ac.instance @left of @Accumulator
// FROZEN-SAME: ac.specialization = "[[SPECIALIZATION:sha256:[0-9a-f]{64}]]"
// FROZEN: ac.instance @right of @Accumulator
// FROZEN-SAME: ac.specialization = "[[SPECIALIZATION]]"

// PLAN: "activation_edges":[
// PLAN-SAME: "definition":"Top"
// PLAN-SAME: "module_instances":[{"definition":"Accumulator"
// PLAN-SAME: "specialization":"[[PLAN_SPECIALIZATION:sha256:[0-9a-f]{64}]]"
// PLAN-SAME: {"definition":"Accumulator"
// PLAN-SAME: "specialization":"[[PLAN_SPECIALIZATION]]"
// PLAN-SAME: "module_specializations":[{
// PLAN-SAME: "activation_edges":[
// PLAN-SAME: "kind":"firing"
// PLAN-SAME: "definition":"Accumulator"
// PLAN-SAME: "tables":[{"entries":1
// PLAN-SAME: "name":"sum"
// PLAN: "work_closure_edges":[

// CXX-COUNT-1: class [[IMPLEMENTATION:Accumulator_[0-9a-f]+]] final : public gfsim::Module
// CXX: gfsim::SimTable<gfsim::UInt<8>> table_0_;
// CXX: gfsim::QueueTableTransition<
// CXX: class StatefulReuse final : public gfsim::Module
// CXX: activation_offsets()
// CXX: activation_complete() { return true; }
// CXX: activation_targets()
// CXX: work_closure_offsets()
// CXX: work_closure_targets()
// CXX: initial_work_ids()
// CXX: schedule_initial_work
// CXX-COUNT-2: [[IMPLEMENTATION]] instance_
