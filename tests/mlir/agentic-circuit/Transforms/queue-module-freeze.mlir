// RUN: %acir_opt --ac-freeze-topology %s | %FileCheck %s
// RUN: %acir_opt --ac-freeze-topology %s | %acir_opt --ac-freeze-topology | %FileCheck %s
// RUN: %acir_opt --ac-freeze-topology %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=CXX < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -fsyntax-only %t.cpp
// RUN: %acir_opt --ac-freeze-topology %s | %python -c "import sys; text=sys.stdin.read(); start=text.index('ac.instance @left'); pos=text.index('sha256:', start); sys.stdout.write(text[:pos] + 'sha256:' + '0' * 64 + text[pos + 71:])" > %t.tampered.mlir
// RUN: %not %acir_opt --ac-freeze-topology %t.tampered.mlir 2>&1 | %FileCheck %s --check-prefix=TAMPERED

builtin.module attributes {
  ac.contract_epoch = "0.5",
  ac.model_kind = "queue_graph",
  ac.queue_graph_domain = "cycle"
} {
  "ac.system"() <{
    sym_name = "reused_pipeline",
    root = @Top,
    root_name = "root",
    tick_epoch = 0 : i64,
    tick_unit = "cycle",
    seed_policy = {kind = "fixed", value = 0 : i64},
    instrumentation = [],
    result_schema = {id = "default", format = "json"},
    selected = true
  }> : () -> ()

  ac.module @Increment(%input: !ac.queue<i8>) -> (!ac.queue<i8>)
      parameters {} graph {
    %output = ac.scope @logic(%input) {
    ^bb0(%borrowed: !ac.queue<i8>):
      %incremented = ac.transform %borrowed depths [2] latencies [1] {
      ^bb0(%item: !ac.var<i8>):
        %one = ac.var.constant 1 : i8 as !ac.var<i8>
        %next = ac.var.add %item, %one : !ac.var<i8>
        ac.transform.yield %next : !ac.var<i8>
      } {ac.name = "module_output"} : (!ac.queue<i8>) -> !ac.queue<i8>
      ac.scope.yield %incremented : !ac.queue<i8>
    } : (!ac.queue<i8>) -> !ac.queue<i8>
    ac.return %output : !ac.queue<i8>
  }

  ac.module @Top() parameters {} graph {
    %left_input, %right_input = ac.scope @inputs() {
      %input = ac.source depth 2 latency 1 {ac.name = "input"}
          : !ac.queue<i8>
      %left, %right = ac.broadcast %input depths [2, 2] latencies [1, 1]
          {ac.output_names = ["left_input", "right_input"]}
          : !ac.queue<i8> -> (!ac.queue<i8>, !ac.queue<i8>)
      ac.scope.yield %left, %right : !ac.queue<i8>, !ac.queue<i8>
    } : () -> (!ac.queue<i8>, !ac.queue<i8>)
    %left_output = ac.instance @left of @Increment(%left_input) static {}
        id "left" path "left" : (!ac.queue<i8>) -> !ac.queue<i8>
    %right_output = ac.instance @right of @Increment(%right_input) static {}
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

// CHECK: module attributes
// CHECK-SAME: ac.frozen_system = @reused_pipeline
// CHECK-SAME: ac.topology_frozen = true
// CHECK: ac.module @Increment
// CHECK-SAME: ac.definition_fingerprint = "[[DEFINITION:sha256:[0-9a-f]{64}]]"
// CHECK-NOT: ac.specialization
// CHECK: ac.module @Top
// CHECK-SAME: ac.definition_fingerprint = "[[ROOT_DEFINITION:sha256:[0-9a-f]{64}]]"
// CHECK-SAME: ac.specialization = "[[ROOT_SPECIALIZATION:sha256:[0-9a-f]{64}]]"
// CHECK: ac.instance @left of @Increment
// CHECK-SAME: ac.specialization = "[[LEAF_SPECIALIZATION:sha256:[0-9a-f]{64}]]"
// CHECK: ac.instance @right of @Increment
// CHECK-SAME: ac.specialization = "[[LEAF_SPECIALIZATION]]"

// TAMPERED: QueueGraph specialization fingerprint is missing or stale

// PLAN: "definition":"Top"
// PLAN-SAME: "module_instances":[{"definition":"Increment"
// PLAN-SAME: "name":"left"
// PLAN-SAME: "specialization":"[[PLAN_SPECIALIZATION:sha256:[0-9a-f]{64}]]"
// PLAN-SAME: {"definition":"Increment"
// PLAN-SAME: "name":"right"
// PLAN-SAME: "specialization":"[[PLAN_SPECIALIZATION]]"
// PLAN-SAME: "module_specializations":[{
// PLAN-SAME: "definition":"Increment"
// PLAN-SAME: "interface_inputs":[{"name":"input_0","payload_type":"i8"}]
// PLAN-SAME: "interface_outputs":[{"name":"module_output","payload_type":"i8"}]

// CXX-COUNT-1: class [[IMPLEMENTATION:Increment_[0-9a-f]+]] final : public gfsim::Module
// CXX: class ReusedPipeline final : public gfsim::Module
// CXX-COUNT-2: [[IMPLEMENTATION]] instance_
