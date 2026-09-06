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
    sym_name = "multi_rule_reuse",
    root = @Top,
    root_name = "root",
    tick_epoch = 0 : i64,
    tick_unit = "cycle",
    seed_policy = {kind = "fixed", value = 0 : i64},
    instrumentation = [],
    result_schema = {id = "default", format = "json"},
    selected = true
  }> : () -> ()

  ac.module @DualAccumulator(
      %input_a: !ac.queue<i8>, %input_b: !ac.queue<i8>)
      -> (!ac.queue<i8>, !ac.queue<i8>) parameters {} graph {
    %output_a, %output_b = ac.scope @logic(%input_a, %input_b) {
    ^bb0(%a: !ac.queue<i8>, %b: !ac.queue<i8>):
      ac.table @sum entry i8 entries 1 init 0 owner "/logic"
          stable_id "table/logic/sum"
      %a_result = ac.firing %a depths [2] latencies [1]
          stable_id "accumulate_a" domain "cycle" {
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
        ac.activation_sources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind state>, resource = @sum}],
        ac.arbitration_membership = [{priority = 0 : i64, resource = @sum}],
        ac.checks_typed = [{guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_check_kind input_available>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_check_kind output_capacity>, ordinal = 0 : i64}],
        ac.effects_typed = [{guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind input_consume>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind output_produce>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind state_read>, resource = @sum}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind state_write>, resource = @sum}],
        ac.guard_kind = #ac<rule_guard_kind always>,
        ac.initially_active = false,
        ac.name = "output_a",
        ac.output_presence = [{ordinal = 0 : i64, presence_kind = #ac<rule_output_presence_kind always>}],
        ac.rule_definition = "accumulate_a",
        ac.rule_footprints = [
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @sum},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @sum}
        ],
        ac.rule_priority = 0 : i64,
        ac.schedule_kind = #ac<rule_schedule_kind lexical_priority>,
        ac.state_accesses = [{guard_kind = #ac<rule_guard_kind always>, index_kind = #ac<rule_index_kind static>, kind = #ac<rule_state_access_kind read>, resource = @sum}, {fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = #ac<rule_index_kind static>, kind = #ac<rule_state_access_kind replace>, resource = @sum}],
        ac.transaction_resources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind state>, resource = @sum}]
      } : (!ac.queue<i8>) -> !ac.queue<i8>
      %b_result = ac.firing %b depths [2] latencies [1]
          stable_id "accumulate_b" domain "cycle" {
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
        ac.activation_sources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind state>, resource = @sum}],
        ac.arbitration_membership = [{priority = 1 : i64, resource = @sum}],
        ac.checks_typed = [{guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_check_kind input_available>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_check_kind output_capacity>, ordinal = 0 : i64}],
        ac.effects_typed = [{guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind input_consume>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind output_produce>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind state_read>, resource = @sum}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind state_write>, resource = @sum}],
        ac.guard_kind = #ac<rule_guard_kind always>,
        ac.initially_active = false,
        ac.name = "output_b",
        ac.output_presence = [{ordinal = 0 : i64, presence_kind = #ac<rule_output_presence_kind always>}],
        ac.rule_definition = "accumulate_b",
        ac.rule_footprints = [
          {access = "read", guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @sum},
          {access = "replace", fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = "static", resource = @sum}
        ],
        ac.rule_priority = 1 : i64,
        ac.schedule_kind = #ac<rule_schedule_kind lexical_priority>,
        ac.state_accesses = [{guard_kind = #ac<rule_guard_kind always>, index_kind = #ac<rule_index_kind static>, kind = #ac<rule_state_access_kind read>, resource = @sum}, {fields = ["$entry"], guard_kind = #ac<rule_guard_kind always>, index_kind = #ac<rule_index_kind static>, kind = #ac<rule_state_access_kind replace>, resource = @sum}],
        ac.transaction_resources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind state>, resource = @sum}]
      } : (!ac.queue<i8>) -> !ac.queue<i8>
      ac.scope.yield %a_result, %b_result : !ac.queue<i8>, !ac.queue<i8>
    } : (!ac.queue<i8>, !ac.queue<i8>) -> (!ac.queue<i8>, !ac.queue<i8>)
    ac.return %output_a, %output_b : !ac.queue<i8>, !ac.queue<i8>
  }

  ac.module @Top() parameters {} graph {
    %left_a, %left_b, %right_a, %right_b = ac.scope @inputs() {
      %q0 = ac.source depth 2 latency 1 {ac.name = "left_a"} : !ac.queue<i8>
      %q1 = ac.source depth 2 latency 1 {ac.name = "left_b"} : !ac.queue<i8>
      %q2 = ac.source depth 2 latency 1 {ac.name = "right_a"} : !ac.queue<i8>
      %q3 = ac.source depth 2 latency 1 {ac.name = "right_b"} : !ac.queue<i8>
      ac.scope.yield %q0, %q1, %q2, %q3
          : !ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>
    } : () -> (!ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>, !ac.queue<i8>)
    %left:2 = ac.instance @left of @DualAccumulator(%left_a, %left_b) static {}
        id "left" path "left"
        : (!ac.queue<i8>, !ac.queue<i8>) -> (!ac.queue<i8>, !ac.queue<i8>)
    %right:2 = ac.instance @right of @DualAccumulator(%right_a, %right_b) static {}
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

// PLAN: "module_instances":[{"definition":"DualAccumulator"
// PLAN-SAME: "inputs":["left_a","left_b"]
// PLAN-SAME: "outputs":["left_0","left_1"]
// PLAN-SAME: {"definition":"DualAccumulator"
// PLAN-SAME: "inputs":["right_a","right_b"]
// PLAN-SAME: "outputs":["right_0","right_1"]
// PLAN-SAME: "module_specializations":[{
// PLAN-SAME: "name":"output_a"
// PLAN-SAME: "priority":0
// PLAN-SAME: "name":"output_b"
// PLAN-SAME: "priority":1
// PLAN-SAME: "definition":"DualAccumulator"
// PLAN-SAME: "interface_inputs":[{"name":"input_0","payload_type":"i8"},{"name":"input_1","payload_type":"i8"}]
// PLAN-SAME: "interface_outputs":[{"name":"output_a","payload_type":"i8"},{"name":"output_b","payload_type":"i8"}]

// CXX-COUNT-1: class [[IMPLEMENTATION:DualAccumulator_[0-9a-f]+]] final : public gfsim::Module
// CXX: gfsim::QueueTableTransition<[[IMPLEMENTATION]]_block_0_policy
// CXX: gfsim::QueueTableTransition<[[IMPLEMENTATION]]_block_1_policy
// CXX: class MultiRuleReuse final : public gfsim::Module
// CXX-COUNT-2: [[IMPLEMENTATION]] instance_
