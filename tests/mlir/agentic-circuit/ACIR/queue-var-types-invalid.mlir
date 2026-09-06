// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/queue-of-var.mlir 2>&1 | %FileCheck %s --check-prefix=QUEUE-OF-VAR
// RUN: %not %acir_opt %t/var-of-queue.mlir 2>&1 | %FileCheck %s --check-prefix=VAR-OF-QUEUE
// RUN: %not %acir_opt %t/queue-of-function.mlir 2>&1 | %FileCheck %s --check-prefix=QUEUE-OF-FUNCTION
// RUN: %not %acir_opt %t/var-of-function.mlir 2>&1 | %FileCheck %s --check-prefix=VAR-OF-FUNCTION
// RUN: %not %acir_opt %t/queue-of-list.mlir 2>&1 | %FileCheck %s --check-prefix=QUEUE-OF-LIST
// RUN: %not %acir_opt %t/var-of-list.mlir 2>&1 | %FileCheck %s --check-prefix=VAR-OF-LIST
// RUN: %not %acir_opt %t/array-zero.mlir 2>&1 | %FileCheck %s --check-prefix=ARRAY-ZERO
// RUN: %not %acir_opt %t/array-payload.mlir 2>&1 | %FileCheck %s --check-prefix=ARRAY-PAYLOAD
// RUN: %not %acir_opt %t/map-duplicate.mlir 2>&1 | %FileCheck %s --check-prefix=MAP-DUPLICATE
// RUN: %not %acir_opt %t/map-order.mlir 2>&1 | %FileCheck %s --check-prefix=MAP-ORDER
// RUN: %not %acir_opt %t/set-zero.mlir 2>&1 | %FileCheck %s --check-prefix=SET-ZERO
// RUN: %not %acir_opt %t/set-payload.mlir 2>&1 | %FileCheck %s --check-prefix=SET-PAYLOAD
// RUN: %not %acir_opt %t/value-array-zero.mlir 2>&1 | %FileCheck %s --check-prefix=VALUE-ARRAY-ZERO
// RUN: %not %acir_opt %t/value-array-runtime.mlir 2>&1 | %FileCheck %s --check-prefix=VALUE-ARRAY-RUNTIME
// RUN: %not %acir_opt %t/tuple-runtime.mlir 2>&1 | %FileCheck %s --check-prefix=TUPLE-RUNTIME

// QUEUE-OF-VAR: error: queue payload must be an immutable ACIR value type
// VAR-OF-QUEUE: error: var payload must be an immutable ACIR value type
// QUEUE-OF-FUNCTION: error: queue payload must be an immutable ACIR value type
// VAR-OF-FUNCTION: error: var payload must be an immutable ACIR value type
// QUEUE-OF-LIST: error: queue payload must be an immutable ACIR value type
// VAR-OF-LIST: error: var payload must be an immutable ACIR value type
// ARRAY-ZERO: error: array length must be positive
// ARRAY-PAYLOAD: error: array element must be a queue, var, or static collection
// MAP-DUPLICATE: error: map keys must be non-empty and strictly lexicographic
// MAP-ORDER: error: map keys must be non-empty and strictly lexicographic
// SET-ZERO: error: set length must be positive
// SET-PAYLOAD: error: set element must be a queue, var, or static collection
// VALUE-ARRAY-ZERO: error: value_array length must be positive
// VALUE-ARRAY-RUNTIME: error: value_array element must be an immutable ACIR value type
// TUPLE-RUNTIME: error: var payload must be an immutable ACIR value type

//--- queue-of-var.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.queue<!ac.var<i32>>
}

//--- var-of-queue.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.var<!ac.queue<i32>>
}

//--- queue-of-function.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.queue<(i32) -> i32>
}

//--- var-of-function.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.var<(i32) -> i32>
}

//--- queue-of-list.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.queue<!ac.list<i32>>
}

//--- var-of-list.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.var<!ac.list<i32>>
}

//--- array-zero.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.array<0 x !ac.queue<i32>>
}

//--- array-payload.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.array<2 x i32>
}

//--- map-duplicate.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.map<["lane", "lane"], !ac.queue<i32>>
}

//--- map-order.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.map<["right", "left"], !ac.queue<i32>>
}

//--- set-zero.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.set<0 x !ac.var<i1>>
}

//--- set-payload.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.set<2 x i32>
}

//--- value-array-zero.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.value_array<0 x i8>
}

//--- value-array-runtime.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.value_array<2 x !ac.queue<i8>>
}

//--- tuple-runtime.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.var<tuple<i8, !ac.queue<i8>>>
}
