// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt --emit-bytecode -o %t.bc %s
// RUN: %acir_opt %t.bc | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  %a = ac.var.constant 5 : i3 as !ac.var<i3>
  %b = ac.var.constant 17 : i5 as !ac.var<i5>
  %tuple = ac.var.tuple %a, %b : !ac.var<i3>, !ac.var<i5> -> !ac.var<tuple<i3, i5>>
  %second = ac.var.element %tuple at 1 : !ac.var<tuple<i3, i5>> -> !ac.var<i5>
  %array = ac.var.array %a, %a, %a, %a : !ac.var<i3>, !ac.var<i3>, !ac.var<i3>, !ac.var<i3> -> !ac.var<!ac.value_array<4 x i3>>
  %third = ac.var.element %array at 2 : !ac.var<!ac.value_array<4 x i3>> -> !ac.var<i3>
}

// CHECK: ac.var.tuple
// CHECK: ac.var.element {{.*}} at 1
// CHECK: ac.var.array
// CHECK: ac.var.element {{.*}} at 2
