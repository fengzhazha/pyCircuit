// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/tuple-type.mlir 2>&1 | %FileCheck %s --check-prefix=TUPLE-TYPE
// RUN: %not %acir_opt %t/array-length.mlir 2>&1 | %FileCheck %s --check-prefix=ARRAY-LENGTH
// RUN: %not %acir_opt %t/array-type.mlir 2>&1 | %FileCheck %s --check-prefix=ARRAY-TYPE
// RUN: %not %acir_opt %t/element-index.mlir 2>&1 | %FileCheck %s --check-prefix=ELEMENT-INDEX
// RUN: %not %acir_opt %t/element-result.mlir 2>&1 | %FileCheck %s --check-prefix=ELEMENT-RESULT

// TUPLE-TYPE: error: 'ac.var.tuple' op tuple operand types must match result elements
// ARRAY-LENGTH: error: 'ac.var.array' op result value_array length must match operands
// ARRAY-TYPE: error: 'ac.var.array' op value_array operands must match its element type
// ELEMENT-INDEX: error: 'ac.var.element' op aggregate index is out of range
// ELEMENT-RESULT: error: 'ac.var.element' op result must match the selected aggregate element

//--- tuple-type.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %a = ac.var.constant 5 : i3 as !ac.var<i3>
  %bad = ac.var.tuple %a : !ac.var<i3> -> !ac.var<tuple<i5>>
}

//--- array-length.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %a = ac.var.constant 5 : i3 as !ac.var<i3>
  %bad = ac.var.array %a : !ac.var<i3> -> !ac.var<!ac.value_array<2 x i3>>
}

//--- array-type.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %a = ac.var.constant 5 : i3 as !ac.var<i3>
  %bad = ac.var.array %a, %a : !ac.var<i3>, !ac.var<i3> -> !ac.var<!ac.value_array<2 x i5>>
}

//--- element-index.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<tuple<i3, i5>>
  %bad = ac.var.element %value at 2 : !ac.var<tuple<i3, i5>> -> !ac.var<i3>
}

//--- element-result.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<!ac.value_array<2 x i3>>
  %bad = ac.var.element %value at 0 : !ac.var<!ac.value_array<2 x i3>> -> !ac.var<i5>
}
