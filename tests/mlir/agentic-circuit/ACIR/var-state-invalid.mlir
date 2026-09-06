// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/init.mlir 2>&1 | %FileCheck %s --check-prefix=INIT
// RUN: %not %acir_opt %t/read.mlir 2>&1 | %FileCheck %s --check-prefix=READ
// RUN: %not %acir_opt %t/assign.mlir 2>&1 | %FileCheck %s --check-prefix=ASSIGN
// RUN: %not %acir_opt %t/assign-when.mlir 2>&1 | %FileCheck %s --check-prefix=ASSIGN-WHEN
// RUN: %not %acir_opt %t/assign-element-parent.mlir 2>&1 | %FileCheck %s --check-prefix=ASSIGN-ELEMENT-PARENT
// RUN: %not %acir_opt %t/shape.mlir 2>&1 | %FileCheck %s --check-prefix=SHAPE
// RUN: %not %acir_opt %t/scalar-shaped-read.mlir 2>&1 | %FileCheck %s --check-prefix=SHAPED-READ
// RUN: %not %acir_opt %t/index-domain.mlir -ac-verify-value-constraints 2>&1 | %FileCheck %s --check-prefix=INDEX-DOMAIN
// RUN: %not %acir_opt %t/match-scalar.mlir 2>&1 | %FileCheck %s --check-prefix=MATCH-SCALAR
// RUN: %not %acir_opt %t/match-domain.mlir 2>&1 | %FileCheck %s --check-prefix=MATCH-DOMAIN
// RUN: %not %acir_opt %t/match-result.mlir 2>&1 | %FileCheck %s --check-prefix=MATCH-RESULT
// RUN: %not %acir_opt %t/match-predicate.mlir 2>&1 | %FileCheck %s --check-prefix=MATCH-PREDICATE
// RUN: %not %acir_opt %t/match-predicate-yield.mlir 2>&1 | %FileCheck %s --check-prefix=MATCH-PREDICATE-YIELD
// RUN: %not %acir_opt %t/choose-mask.mlir 2>&1 | %FileCheck %s --check-prefix=CHOOSE-MASK
// RUN: %not %acir_opt %t/choose-variable.mlir 2>&1 | %FileCheck %s --check-prefix=CHOOSE-VARIABLE
// RUN: %not %acir_opt %t/choose-result.mlir 2>&1 | %FileCheck %s --check-prefix=CHOOSE-RESULT
// RUN: %not %acir_opt %t/choose-valid.mlir 2>&1 | %FileCheck %s --check-prefix=CHOOSE-VALID
// RUN: %not %acir_opt %t/choose-key.mlir 2>&1 | %FileCheck %s --check-prefix=CHOOSE-KEY
// RUN: %not %acir_opt %t/choose-key-yield.mlir 2>&1 | %FileCheck %s --check-prefix=CHOOSE-KEY-YIELD

//--- init.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i16 owner "/" stable_id "var/state"
}
// INIT: 'ac.var.decl' op init must match value type or be the zero image for a struct

//--- read.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state"
  %value = ac.var.read @state : !ac.var<i16>
}
// READ: 'ac.var.read' op result must match declared ac.var value type

//--- assign.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state"
  %value = ac.var.constant 1 : i8 as !ac.var<i8>
  ac.var.assign @state = %value : !ac.var<i8>
}
// ASSIGN: 'ac.var.assign' op must be nested directly in ac.rule or ac.firing

//--- assign-when.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state"
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i8>
  ac.rule %input depths [] latencies [] name "bad" stable_id "bad"
      domain "cycle" type exact input_fact committed_input {
  ^body(%item: !ac.var<i8>):
    %bad = ac.var.constant 0 : i2 as !ac.var<i2>
    ac.var.assign @state = %item when %bad : !ac.var<i2> : !ac.var<i8>
    ac.rule.return
  } : (!ac.queue<i8>) -> ()
}
// ASSIGN-WHEN: 'ac.var.assign' op condition must be !ac.var<i1>

//--- assign-element-parent.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %index = ac.var.constant 0 : i2 as !ac.var<i2>
  %value = ac.var.constant 1 : i8 as !ac.var<i8>
  ac.var.assign_element @state[%index] = %value : !ac.var<i2>, !ac.var<i8>
}
// ASSIGN-ELEMENT-PARENT: 'ac.var.assign_element' op must be nested directly in ac.rule or ac.firing

//--- shape.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [2, 2]
}
// SHAPE: 'ac.var.decl' op persistent ac.var shape must be one positive dimension

//--- scalar-shaped-read.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %value = ac.var.read @state : !ac.var<i8>
}
// SHAPED-READ: 'ac.var.read' op shaped ac.var requires ac.var.read_element

//--- index-domain.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [3]
  %three = ac.var.constant 3 : i2 as !ac.var<i2>
  %zero = ac.var.constant 0 : i2 as !ac.var<i2>
  %index = ac.var.add %three, %zero : !ac.var<i2>
  %value = ac.var.read_element @state[%index] : !ac.var<i2> -> !ac.var<i8>
}
// INDEX-DOMAIN: 'ac.var.read_element' op cannot prove shaped ac.var index is within [0, 2]; inferred constant(3)

//--- match-scalar.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state"
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i8>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i1>
}
// MATCH-SCALAR: 'ac.var.match' op requires a one-dimensional shaped ac.var

//--- match-domain.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [65]
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i8>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i65>
}
// MATCH-DOMAIN: 'ac.var.match' op match domain must contain 1..64 elements

//--- match-result.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i8>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i3>
}
// MATCH-RESULT: 'ac.var.match' op mask width must equal the ac.var domain

//--- match-predicate.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i16>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i4>
}
// MATCH-PREDICATE: 'ac.var.match' op predicate argument must match the ac.var element

//--- match-predicate-yield.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i8>):
    %bad = ac.var.constant 1 : i2 as !ac.var<i2>
    ac.var.match.yield %bad : !ac.var<i2>
  } -> !ac.var<i4>
}
// MATCH-PREDICATE-YIELD: 'ac.var.match' op predicate must yield !ac.var<i1>

//--- choose-mask.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %mask = ac.var.constant 15 : i4 as !ac.var<i4>
  %index, %valid = ac.var.choose @state %mask : !ac.var<i4> count 1 policy "first" key {} -> !ac.var<i2>, !ac.var<i1>
}
// CHOOSE-MASK: 'ac.var.choose' op candidate mask must be produced directly by ac.var.match

//--- choose-variable.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @left type i8 init 0 : i8 owner "/" stable_id "var/left" shape [4]
  ac.var.decl @right type i8 init 0 : i8 owner "/" stable_id "var/right" shape [4]
  %mask = ac.var.match @left predicate {
  ^bb0(%entry: !ac.var<i8>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i4>
  %index, %valid = ac.var.choose @right %mask : !ac.var<i4> count 1 policy "first" key {} -> !ac.var<i2>, !ac.var<i1>
}
// CHOOSE-VARIABLE: 'ac.var.choose' op candidate mask must come from the same ac.var

//--- choose-result.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i8>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i4>
  %index, %valid = ac.var.choose @state %mask : !ac.var<i4> count 1 policy "first" key {} -> !ac.var<i3>, !ac.var<i1>
}
// CHOOSE-RESULT: 'ac.var.choose' op index result width must address the ac.var domain

//--- choose-valid.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i8>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i4>
  %index, %valid = ac.var.choose @state %mask : !ac.var<i4> count 1 policy "first" key {} -> !ac.var<i2>, !ac.var<i2>
}
// CHOOSE-VALID: 'ac.var.choose' op valid result must be !ac.var<i1>

//--- choose-key.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i8>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i4>
  %index, %valid = ac.var.choose @state %mask : !ac.var<i4> count 1 policy "min" key {
  ^bb0(%entry: !ac.var<i16>):
    ac.var.choose.yield %entry : !ac.var<i16>
  } -> !ac.var<i2>, !ac.var<i1>
}
// CHOOSE-KEY: 'ac.var.choose' op key argument must match the ac.var element

//--- choose-key-yield.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.var.decl @state type i8 init 0 : i8 owner "/" stable_id "var/state" shape [4]
  %mask = ac.var.match @state predicate {
  ^bb0(%entry: !ac.var<i8>):
    %true = ac.var.constant 1 : i1 as !ac.var<i1>
    ac.var.match.yield %true : !ac.var<i1>
  } -> !ac.var<i4>
  %index, %valid = ac.var.choose @state %mask : !ac.var<i4> count 1 policy "min" key {
  ^bb0(%entry: !ac.var<i8>):
    %bad = ac.var.constant 0.000000e+00 : f32 as !ac.var<f32>
    ac.var.choose.yield %bad : !ac.var<f32>
  } -> !ac.var<i2>, !ac.var<i1>
}
// CHOOSE-KEY-YIELD: 'ac.var.choose' op min/max key must yield an unsigned fixed-width integer
