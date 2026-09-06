// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/input.mlir 2>&1 | %FileCheck %s --check-prefix=INPUT
// RUN: %not %acir_opt %t/result.mlir 2>&1 | %FileCheck %s --check-prefix=RESULT
// RUN: %not %acir_opt %t/mask-width.mlir 2>&1 | %FileCheck %s --check-prefix=MASK-WIDTH
// RUN: %not %acir_opt %t/value-width.mlir 2>&1 | %FileCheck %s --check-prefix=VALUE-WIDTH
// RUN: %not %acir_opt %t/value-mask.mlir 2>&1 | %FileCheck %s --check-prefix=VALUE-MASK

//--- input.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.var<f32>
  %matched = ac.var.matches %input mask 0 value 0 : !ac.var<f32> -> !ac.var<i1>
}
// INPUT: 'ac.var.matches' op input must be an ac.var carrying a signless i1..i64 integer

//--- result.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = ac.var.constant 0 : i8 as !ac.var<i8>
  %matched = ac.var.matches %input mask 0 value 0 : !ac.var<i8> -> !ac.var<i2>
}
// RESULT: 'ac.var.matches' op result must be !ac.var<i1>

//--- mask-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = ac.var.constant 0 : i4 as !ac.var<i4>
  %matched = ac.var.matches %input mask 16 value 0 : !ac.var<i4> -> !ac.var<i1>
}
// MASK-WIDTH: 'ac.var.matches' op mask and value must fit the input width

//--- value-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = ac.var.constant 0 : i4 as !ac.var<i4>
  %matched = ac.var.matches %input mask 15 value 16 : !ac.var<i4> -> !ac.var<i1>
}
// VALUE-WIDTH: 'ac.var.matches' op mask and value must fit the input width

//--- value-mask.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %input = ac.var.constant 0 : i4 as !ac.var<i4>
  %matched = ac.var.matches %input mask 10 value 5 : !ac.var<i4> -> !ac.var<i1>
}
// VALUE-MASK: 'ac.var.matches' op value may set only bits selected by mask
