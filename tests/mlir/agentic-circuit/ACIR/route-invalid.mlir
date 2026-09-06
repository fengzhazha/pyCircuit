// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/outputs.mlir 2>&1 | %FileCheck %s --check-prefix=OUTPUTS
// RUN: %not %acir_opt %t/payload.mlir 2>&1 | %FileCheck %s --check-prefix=PAYLOAD

// OUTPUTS: error: 'ac.route' op requires at least two output queues
// PAYLOAD: error: 'ac.route' op output queue 1 must match input queue type

//--- outputs.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = ac.source depth 4 latency 1 : !ac.queue<i64>
  %only = ac.route %input depths [1] latencies [1] {
  ^selector(%item: !ac.var<i64>):
    ac.route.yield %item : !ac.var<i64>
  } : !ac.queue<i64> -> (!ac.queue<i64>)
}

//--- payload.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = ac.source depth 4 latency 1 : !ac.queue<i64>
  %left, %right = ac.route %input depths [1, 1] latencies [1, 1] {
  ^selector(%item: !ac.var<i64>):
    ac.route.yield %item : !ac.var<i64>
  } : !ac.queue<i64> -> (!ac.queue<i64>, !ac.queue<i32>)
}
