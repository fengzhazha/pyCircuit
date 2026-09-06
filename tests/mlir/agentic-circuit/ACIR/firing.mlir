// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt --emit-bytecode -o %t.bc %s
// RUN: %acir_opt %t.bc | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  %output = ac.firing %input depths [2] latencies [1]
      stable_id "increment" domain "cycle" {
  ^firing(%item: !ac.var<i32>):
    %one = ac.var.constant 1 : i32 as !ac.var<i32>
    %value = ac.var.add %item, %one : !ac.var<i32>
    ac.firing.yield %value : !ac.var<i32>
  } {ac.activation_sources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}], ac.arbitration_membership = [], ac.checks_typed = [{guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_check_kind input_available>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_check_kind output_capacity>, ordinal = 0 : i64}], ac.effects_typed = [{guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind input_consume>, ordinal = 0 : i64}, {guard_kind = #ac<rule_guard_kind always>, kind = #ac<rule_effect_kind output_produce>, ordinal = 0 : i64}], ac.guard_kind = #ac<rule_guard_kind always>, ac.initially_active = false, ac.output_presence = [{ordinal = 0 : i64, presence_kind = #ac<rule_output_presence_kind always>}], ac.rule_footprints = [], ac.rule_priority = 0 : i64, ac.schedule_kind = #ac<rule_schedule_kind independent>, ac.state_accesses = [], ac.transaction_resources = [{kind = #ac<activation_resource_kind input_queue>, ordinal = 0 : i64}, {kind = #ac<activation_resource_kind output_queue>, ordinal = 0 : i64}]} : (!ac.queue<i32>) -> !ac.queue<i32>
}

// CHECK: %[[OUTPUT:.*]] = ac.firing %[[INPUT:.*]] depths [2] latencies [1]
// CHECK-SAME: stable_id "increment" domain "cycle"
// CHECK: ^bb0(%[[ITEM:.*]]: !ac.var<i32>):
// CHECK: ac.firing.yield %{{.*}} : !ac.var<i32>
// CHECK: ac.checks_typed = [
// CHECK-SAME: #ac<rule_check_kind input_available>
// CHECK-SAME: #ac<rule_check_kind output_capacity>
// CHECK: ac.effects_typed = [
// CHECK-SAME: #ac<rule_effect_kind input_consume>
// CHECK-SAME: #ac<rule_effect_kind output_produce>
