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
    sym_name = "nested_reuse",
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
      } {ac.name = "incremented"} : (!ac.queue<i8>) -> !ac.queue<i8>
      ac.scope.yield %incremented : !ac.queue<i8>
    } : (!ac.queue<i8>) -> !ac.queue<i8>
    ac.return %output : !ac.queue<i8>
  }

  ac.module @Wrapper(%input: !ac.queue<i8>) -> (!ac.queue<i8>)
      parameters {} graph {
    %output = ac.instance @child of @Increment(%input) static {}
        id "child" path "child" : (!ac.queue<i8>) -> !ac.queue<i8>
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
    %left_output = ac.instance @left of @Wrapper(%left_input) static {}
        id "left" path "left" : (!ac.queue<i8>) -> !ac.queue<i8>
    %right_output = ac.instance @right of @Wrapper(%right_input) static {}
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
// PLAN-SAME: "definition":"Wrapper"
// PLAN-SAME: "module_instances":[{"definition":"Increment"
// PLAN-SAME: "name":"child"
// PLAN-SAME: "module_specializations":[{
// PLAN-SAME: "kind":"transform"
// PLAN-SAME: "definition":"Increment"

// CXX-COUNT-1: class [[LEAF:Increment_[0-9a-f]+]] final : public gfsim::Module
// CXX-COUNT-1: class [[WRAPPER:Wrapper_[0-9a-f]+]] final : public gfsim::Module
// CXX: [[LEAF]] child_0_;
// CXX: class NestedReuse final : public gfsim::Module
// CXX-COUNT-2: [[WRAPPER]] instance_
