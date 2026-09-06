// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/constant.mlir 2>&1 | %FileCheck %s --check-prefix=CONSTANT
// RUN: %not %acir_opt %t/binary.mlir 2>&1 | %FileCheck %s --check-prefix=BINARY
// RUN: %not %acir_opt %t/get-field.mlir 2>&1 | %FileCheck %s --check-prefix=GET-FIELD
// RUN: %not %acir_opt %t/get-result.mlir 2>&1 | %FileCheck %s --check-prefix=GET-RESULT
// RUN: %not %acir_opt %t/with-result.mlir 2>&1 | %FileCheck %s --check-prefix=WITH-RESULT
// RUN: %not %acir_opt %t/with-value.mlir 2>&1 | %FileCheck %s --check-prefix=WITH-VALUE
// RUN: %not %acir_opt %t/bit-width.mlir 2>&1 | %FileCheck %s --check-prefix=BIT-WIDTH
// RUN: %not %acir_opt %t/priority-index.mlir 2>&1 | %FileCheck %s --check-prefix=PRIORITY-INDEX
// RUN: %not %acir_opt %t/priority-order.mlir 2>&1 | %FileCheck %s --check-prefix=PRIORITY-ORDER
// RUN: %not %acir_opt %t/cmp-predicate.mlir 2>&1 | %FileCheck %s --check-prefix=CMP-PREDICATE
// RUN: %not %acir_opt %t/popcount-width.mlir 2>&1 | %FileCheck %s --check-prefix=POPCOUNT-WIDTH
// RUN: %not %acir_opt %t/popcount-input.mlir 2>&1 | %FileCheck %s --check-prefix=POPCOUNT-INPUT
// RUN: %not %acir_opt %t/count-leading-zeros-width.mlir 2>&1 | %FileCheck %s --check-prefix=CLZ-WIDTH
// RUN: %not %acir_opt %t/count-leading-zeros-input.mlir 2>&1 | %FileCheck %s --check-prefix=CLZ-INPUT
// RUN: %not %acir_opt %t/count-zeros-direction.mlir 2>&1 | %FileCheck %s --check-prefix=ZERO-DIRECTION
// RUN: %not %acir_opt %t/select-condition.mlir 2>&1 | %FileCheck %s --check-prefix=SELECT-CONDITION
// RUN: %not %acir_opt %t/extract-range.mlir 2>&1 | %FileCheck %s --check-prefix=EXTRACT-RANGE
// RUN: %not %acir_opt %t/concat-result.mlir 2>&1 | %FileCheck %s --check-prefix=CONCAT-RESULT
// RUN: %not %acir_opt %t/insert-range.mlir 2>&1 | %FileCheck %s --check-prefix=INSERT-RANGE
// RUN: %not %acir_opt %t/sub-nonnumeric.mlir 2>&1 | %FileCheck %s --check-prefix=SUB-NONNUMERIC
// RUN: %not %acir_opt %t/mul-nonnumeric.mlir 2>&1 | %FileCheck %s --check-prefix=MUL-NONNUMERIC
// RUN: %not %acir_opt %t/or-width.mlir 2>&1 | %FileCheck %s --check-prefix=OR-WIDTH
// RUN: %not %acir_opt %t/xor-width.mlir 2>&1 | %FileCheck %s --check-prefix=XOR-WIDTH
// RUN: %not %acir_opt %t/not-width.mlir 2>&1 | %FileCheck %s --check-prefix=NOT-WIDTH
// RUN: %not %acir_opt %t/shl-width.mlir 2>&1 | %FileCheck %s --check-prefix=SHL-WIDTH
// RUN: %not %acir_opt %t/shr-width.mlir 2>&1 | %FileCheck %s --check-prefix=SHR-WIDTH

// CONSTANT: error: 'ac.var.constant' op attribute type must match Var element type
// BINARY: error: use of value '%right' expects different type than prior uses
// GET-FIELD: error: 'ac.var.get' op unknown field 'missing'
// GET-RESULT: error: 'ac.var.get' op field 'value' result must be '!ac.var<i64>'
// WITH-RESULT: error: 'ac.var.with' op must preserve record Var identity
// WITH-VALUE: error: 'ac.var.with' op field 'value' expects '!ac.var<i64>'
// BIT-WIDTH: error: 'ac.var.and' op bit operation Var element must be a signless integer with width in [1, 64]
// PRIORITY-INDEX: error: 'ac.var.priority_encode' op index width must be max(1, ceil(log2(input_width))) = 4
// PRIORITY-ORDER: error: 'ac.var.priority_encode' op order must be low or high
// CMP-PREDICATE: error: 'ac.var.cmp' op predicate must be eq, ne, slt, sle, sgt, sge, ult, ule, ugt, or uge
// POPCOUNT-WIDTH: error: 'ac.var.popcount' op result width must be ceil(log2(input_width + 1)) = 4
// POPCOUNT-INPUT: error: 'ac.var.popcount' op input width must be in [1, 64]
// CLZ-WIDTH: error: 'ac.var.count_zeros' op result width must be ceil(log2(input_width + 1)) = 4
// CLZ-INPUT: error: 'ac.var.count_zeros' op input width must be in [1, 64]
// ZERO-DIRECTION: error: 'ac.var.count_zeros' op direction must be leading or trailing
// SELECT-CONDITION: error: 'ac.var.select' op condition must be !ac.var<i1>
// EXTRACT-RANGE: error: 'ac.var.extract' op slice must be non-empty and within the input width
// CONCAT-RESULT: error: 'ac.var.concat' op result width must equal the sum of input widths
// INSERT-RANGE: error: 'ac.var.insert' op inserted range must be within the base width
// SUB-NONNUMERIC: error: 'ac.var.sub' op arithmetic Var element must be an integer or float
// MUL-NONNUMERIC: error: 'ac.var.mul' op arithmetic Var element must be an integer or float
// OR-WIDTH: error: 'ac.var.or' op bit operation Var element must be a signless integer with width in [1, 64]
// XOR-WIDTH: error: 'ac.var.xor' op bit operation Var element must be a signless integer with width in [1, 64]
// NOT-WIDTH: error: 'ac.var.not' op bit operation Var element must be a signless integer with width in [1, 64]
// SHL-WIDTH: error: 'ac.var.shl' op bit operation Var element must be a signless integer with width in [1, 64]
// SHR-WIDTH: error: 'ac.var.shr' op bit operation Var element must be a signless integer with width in [1, 64]

//--- constant.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %bad = ac.var.constant 1 : i16 as !ac.var<i32>
}

//--- popcount-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i8>
  %bad = ac.var.popcount %value : !ac.var<i8> -> !ac.var<i3>
}

//--- bit-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i128>
  %bad = ac.var.and %value, %value : !ac.var<i128>
}

//--- priority-index.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i13>
  %index, %valid = ac.var.priority_encode %value order "low" : !ac.var<i13> -> !ac.var<i3>, !ac.var<i1>
}

//--- priority-order.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i4>
  %index, %valid = ac.var.priority_encode %value order "middle" : !ac.var<i4> -> !ac.var<i2>, !ac.var<i1>
}

//--- popcount-input.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i128>
  %bad = ac.var.popcount %value : !ac.var<i128> -> !ac.var<i8>
}

//--- count-leading-zeros-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i8>
  %bad = ac.var.count_zeros %value direction "leading" : !ac.var<i8> -> !ac.var<i3>
}

//--- count-leading-zeros-input.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i128>
  %bad = ac.var.count_zeros %value direction "trailing" : !ac.var<i128> -> !ac.var<i8>
}

//--- count-zeros-direction.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i8>
  %bad = ac.var.count_zeros %value direction "middle" : !ac.var<i8> -> !ac.var<i4>
}

//--- cmp-predicate.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %left = ac.var.constant 1 : i64 as !ac.var<i64>
  %right = ac.var.constant 2 : i64 as !ac.var<i64>
  %bad = ac.var.cmp "random" %left, %right : !ac.var<i64> -> !ac.var<i1>
}

//--- select-condition.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %condition = ac.var.constant 1 : i8 as !ac.var<i8>
  %left = ac.var.constant 2 : i8 as !ac.var<i8>
  %right = ac.var.constant 3 : i8 as !ac.var<i8>
  %bad = ac.var.select %condition, %left, %right : !ac.var<i8>, !ac.var<i8> -> !ac.var<i8>
}

//--- extract-range.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = ac.var.constant 5 : i3 as !ac.var<i3>
  %bad = ac.var.extract %value from 2 width 2 : !ac.var<i3> -> !ac.var<i2>
}

//--- concat-result.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %left = ac.var.constant 5 : i3 as !ac.var<i3>
  %right = ac.var.constant 17 : i5 as !ac.var<i5>
  %bad = ac.var.concat %left, %right : !ac.var<i3>, !ac.var<i5> -> !ac.var<i7>
}

//--- insert-range.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %base = ac.var.constant 5 : i3 as !ac.var<i3>
  %value = ac.var.constant 3 : i2 as !ac.var<i2>
  %bad = ac.var.insert %base, %value at 2 : !ac.var<i3>, !ac.var<i2> -> !ac.var<i3>
}

//--- binary.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %left = ac.var.constant 1 : i32 as !ac.var<i32>
  %right = ac.var.constant 1 : i16 as !ac.var<i16>
  %bad = ac.var.add %left, %right : !ac.var<i32>
}

//--- sub-nonnumeric.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<tuple<i8>>
  %bad = ac.var.sub %value, %value : !ac.var<tuple<i8>>
}

//--- mul-nonnumeric.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<tuple<i8>>
  %bad = ac.var.mul %value, %value : !ac.var<tuple<i8>>
}

//--- or-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i128>
  %bad = ac.var.or %value, %value : !ac.var<i128>
}

//--- xor-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i128>
  %bad = ac.var.xor %value, %value : !ac.var<i128>
}

//--- not-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i128>
  %bad = ac.var.not %value : !ac.var<i128> -> !ac.var<i128>
}

//--- shl-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i128>
  %bad = ac.var.shl %value, %value : !ac.var<i128>
}

//--- shr-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i128>
  %bad = ac.var.shr %value, %value : !ac.var<i128>
}

//--- get-field.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "Item", fields = [{name = "value", type = i64}]}> : () -> ()
  }) : () -> ()
  %item = "builtin.unrealized_conversion_cast"() : () -> !ac.var<!ac.transaction<@types::@Item>>
  %bad = ac.var.get %item field "missing" : !ac.var<!ac.transaction<@types::@Item>> -> !ac.var<i64>
}

//--- get-result.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "Item", fields = [{name = "value", type = i64}]}> : () -> ()
  }) : () -> ()
  %item = "builtin.unrealized_conversion_cast"() : () -> !ac.var<!ac.transaction<@types::@Item>>
  %bad = ac.var.get %item field "value" : !ac.var<!ac.transaction<@types::@Item>> -> !ac.var<i16>
}

//--- with-result.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "A", fields = [{name = "value", type = i64}]}> : () -> ()
    "ac.transaction"() <{sym_name = "B", fields = [{name = "value", type = i64}]}> : () -> ()
  }) : () -> ()
  %item = "builtin.unrealized_conversion_cast"() : () -> !ac.var<!ac.transaction<@types::@A>>
  %value = ac.var.constant 1 : i64 as !ac.var<i64>
  %bad = ac.var.with %item, %value field "value" : !ac.var<!ac.transaction<@types::@A>>, !ac.var<i64> -> !ac.var<!ac.transaction<@types::@B>>
}

//--- with-value.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "Item", fields = [{name = "value", type = i64}]}> : () -> ()
  }) : () -> ()
  %item = "builtin.unrealized_conversion_cast"() : () -> !ac.var<!ac.transaction<@types::@Item>>
  %value = ac.var.constant 1 : i16 as !ac.var<i16>
  %bad = ac.var.with %item, %value field "value" : !ac.var<!ac.transaction<@types::@Item>>, !ac.var<i16> -> !ac.var<!ac.transaction<@types::@Item>>
}
