// RUN: %acir_opt --pass-pipeline='builtin.module(ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_pycgen %t.frozen.mlir | %FileCheck %s --check-prefix=PYC
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "value_select"} {
  %input = ac.source depth 1 latency 1 {ac.name = "input"} : !ac.queue<i8>
  %output = ac.transform %input depths [1] latencies [1] {
  ^transform(%item: !ac.var<i8>):
    %zero = ac.var.constant 0 : i8 as !ac.var<i8>
    %positive = ac.var.cmp "ugt" %item, %zero : !ac.var<i8> -> !ac.var<i1>
    %selected = ac.var.select %positive, %item, %zero : !ac.var<i1>, !ac.var<i8> -> !ac.var<i8>
    ac.transform.yield %selected : !ac.var<i8>
  } {ac.output_names = ["output"]} : (!ac.queue<i8>) -> !ac.queue<i8>
  ac.sink %output {ac.name = "sink"} : !ac.queue<i8>
}

// PYC: pyc.select {{.*}} : i1, i8, i8 -> i8
// GFSIM: auto v{{[0-9]+}} = v{{[0-9]+}} ? item : v{{[0-9]+}};
