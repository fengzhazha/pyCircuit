// RUN: %acir_opt %s -ac-verify-value-constraints -verify-diagnostics
// RUN: %acir_opt %s -ac-lower-rules -verify-diagnostics
// RUN: %acir_opt %s -ac-freeze-topology -verify-diagnostics

builtin.module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "unsafe"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [5]
  %input = ac.source depth 1 latency 1 : !ac.queue<i3>
  %output = ac.transform %input depths [1] latencies [1] {
  ^body(%index: !ac.var<i3>):
    // expected-error @+1 {{cannot prove shaped ac.var index is within [0, 4]; inferred interval[0,7]}}
    %value = ac.var.read_element @state[%index] : !ac.var<i3> -> !ac.var<i8>
    ac.transform.yield %value : !ac.var<i8>
  } : (!ac.queue<i3>) -> !ac.queue<i8>
  ac.sink %output : !ac.queue<i8>
}
