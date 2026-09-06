// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt --emit-bytecode -o %t.bc %s
// RUN: %acir_opt %t.bc | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = ac.var.constant 165 : i8 as !ac.var<i8>
  %matched = ac.var.matches %input mask 240 value 160 : !ac.var<i8> -> !ac.var<i1>
  %wildcard = ac.var.matches %input mask 0 value 0 : !ac.var<i8> -> !ac.var<i1>
  %wide = ac.var.constant -1 : i64 as !ac.var<i64>
  %wide_match = ac.var.matches %wide mask 18446744073709551615 value 18446744073709551615 : !ac.var<i64> -> !ac.var<i1>
}

// CHECK: ac.var.matches %{{.*}} mask 240 value 160 : !ac.var<i8> -> !ac.var<i1>
// CHECK: ac.var.matches %{{.*}} mask 0 value 0 : !ac.var<i8> -> !ac.var<i1>
// CHECK: ac.var.matches %{{.*}} mask 18446744073709551615 value 18446744073709551615 : !ac.var<i64> -> !ac.var<i1>
