// RUN: %acir_opt --pass-pipeline='builtin.module(ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_pycgen %t.frozen.mlir | %FileCheck %s --check-prefix=PYC
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "masked_matching"} {
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i64>
  %output = ac.transform %input depths [1] latencies [1] {
  ^body(%item: !ac.var<i64>):
    %matched = ac.var.matches %item mask 9223372036854775809 value 9223372036854775809 : !ac.var<i64> -> !ac.var<i1>
    ac.transform.yield %matched : !ac.var<i1>
  } {ac.output_names = ["output"]} : (!ac.queue<i64>) -> !ac.queue<i1>
  ac.sink %output {ac.name = "sink"} : !ac.queue<i1>
}

// PLAN: "kind":"masked_match"
// PLAN-SAME: "mask":"0x8000000000000001"
// PLAN-SAME: "value":"0x8000000000000001"

// PYC: %[[MASK:.*]] = pyc.constant 0x8000000000000001 : i64
// PYC: %[[MASKED:.*]] = pyc.and {{.*}}, %[[MASK]] : i64, i64 -> i64
// PYC: %[[EXPECTED:.*]] = pyc.constant 0x8000000000000001 : i64
// PYC: pyc.eq %[[MASKED]], %[[EXPECTED]] : i64, i64 -> i1

// GFSIM: & std::uint64_t{0x8000000000000001}
// GFSIM-SAME: == std::uint64_t{0x8000000000000001}
