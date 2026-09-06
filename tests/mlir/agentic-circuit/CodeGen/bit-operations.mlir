// RUN: %acir_opt --pass-pipeline='builtin.module(ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_plan %t.frozen.mlir | %FileCheck %s --check-prefix=PLAN
// RUN: %acir_queue_pycgen %t.frozen.mlir | %FileCheck %s --check-prefix=PYC
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "bit_operations"} {
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i17>
  %output = ac.transform %input depths [1] latencies [1] {
  ^transform(%item: !ac.var<i17>):
    %low = ac.var.extract %item from 0 width 5 : !ac.var<i17> -> !ac.var<i5>
    %middle = ac.var.extract %item from 5 width 3 : !ac.var<i17> -> !ac.var<i3>
    %joined = ac.var.concat %middle, %low : !ac.var<i3>, !ac.var<i5> -> !ac.var<i8>
    %updated = ac.var.insert %item, %joined at 9 : !ac.var<i17>, !ac.var<i8> -> !ac.var<i17>
    ac.transform.yield %updated : !ac.var<i17>
  } {ac.output_names = ["output"]} : (!ac.queue<i17>) -> !ac.queue<i17>
  ac.sink %output {ac.name = "sink"} : !ac.queue<i17>
}

// PLAN: "kind":"bit_extract","literal":"","lsb":0
// PLAN-SAME: "width":5
// PLAN: "kind":"bit_concat"
// PLAN: "kind":"bit_insert","literal":"","lsb":9

// PYC: pyc.extract {{.*}} {lsb = 0} : i17 -> i5
// PYC: pyc.extract {{.*}} {lsb = 5} : i17 -> i3
// PYC: pyc.concat({{.*}}) : (i3, i5) -> i8
// PYC: pyc.extract {{.*}} {lsb = 0} : i17 -> i9
// PYC: pyc.concat({{.*}}) : (i8, i9) -> i17

// GFSIM: gfsim::bitExtract<5>(item, 0)
// GFSIM: gfsim::bitExtract<3>(item, 5)
// GFSIM: gfsim::bitConcat(
// GFSIM: gfsim::bitInsert(
