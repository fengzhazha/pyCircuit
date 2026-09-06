// RUN: %acir_opt --pass-pipeline='builtin.module(ac-infer-rule-types,ac-infer-rule-effects,ac-materialize-rule-checks,ac-materialize-rule-handshake)' %s | %FileCheck %s --check-prefix=MATERIALIZED
// RUN: %acir_opt --pass-pipeline='builtin.module(ac-lower-rules)' %s | %FileCheck %s --check-prefix=LOWERED
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-lower-rules,canonicalize,cse,ac-verify-rule-closure,ac-freeze-topology)' %s -o %t.frozen.mlir
// RUN: %acir_queue_cxxgen %t.frozen.mlir > %t.cpp
// RUN: %FileCheck %s --check-prefix=GFSIM < %t.cpp
// RUN: %cxx -std=c++20 -I%source_root/simulator/gfsim/include -c %t.cpp -o %t.o

module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "rule_atomic_sum"} {
  %left = ac.source depth 2 latency 1 {ac.name = "left"} : !ac.queue<i64>
  %right = ac.source depth 2 latency 1 {ac.name = "right"} : !ac.queue<i64>
  %sum = ac.rule %left, %right depths [2] latencies [1]
      name "add" stable_id "sum" domain "cycle"
      type exact input_fact committed_input {
  ^body(%lhs: !ac.var<i64>, %rhs: !ac.var<i64>):
    %value = ac.var.add %lhs, %rhs : !ac.var<i64>
    %ready = ac.marker.obligation %value state pending resolver handshake
        origin "add:return" path "true" : !ac.var<i64>
    ac.rule.return %ready : !ac.var<i64>
  } {ac.name = "sum"} : (!ac.queue<i64>, !ac.queue<i64>) -> !ac.queue<i64>
  ac.sink %sum {ac.name = "sink"} : !ac.queue<i64>
}

// MATERIALIZED: ac.rule %{{.*}}, %{{.*}}
// MATERIALIZED: ac.rule.handshake = "ready_valid_2x1"

// LOWERED-NOT: ac.rule
// LOWERED-NOT: ac.marker
// LOWERED: ac.transform %{{.*}}, %{{.*}}
// LOWERED: ac.rule_effects = ["input.consume", "output.produce"]
// LOWERED-SAME: ac.rule_handshake = "ready_valid_2x1"

// GFSIM: std::tuple<gfsim::UInt<64>> operator()(const gfsim::UInt<64> &item, const gfsim::UInt<64> &item1)
// GFSIM: gfsim::QueueAtomicTransform<block_0_policy, std::tuple<gfsim::UInt<64>, gfsim::UInt<64>>, std::tuple<gfsim::UInt<64>>> block_0_;
