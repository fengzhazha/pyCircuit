// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt %s | %acir_opt | %FileCheck %s
// RUN: %acir_opt --emit-bytecode -o %t.bc %s
// RUN: %acir_opt %t.bc | %FileCheck %s

// This phase-branch fixture uses the active file epoch while the
// replacement contract is built in verified slices. The final hard-break
// checkpoint updates every artifact and fixture to epoch 0.5 together.
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.var<i32>
  "builtin.unrealized_conversion_cast"() : () -> !ac.var<!ac.struct<@types::@Token>>
  "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i32>
  "builtin.unrealized_conversion_cast"() : () -> !ac.queue<!ac.struct<@types::@Token>>
  "builtin.unrealized_conversion_cast"() : () -> !ac.var<tuple<i3, i5>>
  "builtin.unrealized_conversion_cast"() : () -> !ac.queue<!ac.value_array<4 x i8>>
  "builtin.unrealized_conversion_cast"() : () -> !ac.array<4 x !ac.queue<i32>>
}

// CHECK: !ac.var<i32>
// CHECK: !ac.var<!ac.struct<@types::@Token>>
// CHECK: !ac.queue<i32>
// CHECK: !ac.queue<!ac.struct<@types::@Token>>
// CHECK: !ac.var<tuple<i3, i5>>
// CHECK: !ac.queue<!ac.value_array<4 x i8>>
// CHECK: !ac.array<4 x !ac.queue<i32>>
