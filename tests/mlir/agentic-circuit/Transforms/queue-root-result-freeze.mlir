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
  ac.system @host_result root @Top as "root" tick 0 "cycle"
      seed {kind = "fixed", value = 0 : i64} instrumentation []
      results {id = "default", format = "json"} selected true

  ac.module @Increment(%input: !ac.queue<i8>) -> !ac.queue<i8>
      parameters {} graph {
    %output = ac.scope @body(%input) {
    ^bb0(%borrowed: !ac.queue<i8>):
      %next = ac.transform %borrowed depths [1] latencies [1] {
      ^bb0(%item: !ac.var<i8>):
        %one = ac.var.constant 1 : i8 as !ac.var<i8>
        %value = ac.var.add %item, %one : !ac.var<i8>
        ac.transform.yield %value : !ac.var<i8>
      } {ac.name = "result"} : (!ac.queue<i8>) -> !ac.queue<i8>
      ac.scope.yield %next : !ac.queue<i8>
    } : (!ac.queue<i8>) -> !ac.queue<i8>
    ac.return %output : !ac.queue<i8>
  }

  ac.module @Top() -> !ac.queue<i8> parameters {} graph {
    %input = ac.scope @inputs() {
      %source = ac.source depth 1 latency 1 {ac.name = "input"}
          : !ac.queue<i8>
      ac.scope.yield %source : !ac.queue<i8>
    } : () -> !ac.queue<i8>
    %output = ac.instance @increment of @Increment(%input) static {}
        id "increment" path "increment"
        : (!ac.queue<i8>) -> !ac.queue<i8>
    ac.return %output : !ac.queue<i8>
  }
}

// PLAN: "definition":"Top"
// PLAN-SAME: "interface_outputs":[{"name":"increment"
// PLAN-SAME: "module_instances":[{"definition":"Increment"

// CXX: const gfsim::SimQueue<gfsim::UInt<8>> &result_0() const
// CXX: std::optional<gfsim::UInt<8>> try_take_result_0
// CXX: scheduleExternalXfer
