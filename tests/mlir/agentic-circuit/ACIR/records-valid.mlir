// RUN: %acir_opt %s | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  %opcode = ac.var.constant 0 : i8 as !ac.var<i8>
  %tag = ac.var.constant 0 : i16 as !ac.var<i16>
  %header = ac.var.tuple %opcode, %tag : !ac.var<i8>, !ac.var<i16> -> !ac.var<tuple<i8, i16>>
  %got = ac.var.element %header at 0 : !ac.var<tuple<i8, i16>> -> !ac.var<i8>
}

// CHECK: ac.var.tuple
// CHECK: ac.var.element
