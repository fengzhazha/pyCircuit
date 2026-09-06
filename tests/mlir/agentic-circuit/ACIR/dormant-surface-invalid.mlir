// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/module-generated.mlir 2>&1 | %FileCheck %s --check-prefix=MODULE
// RUN: %not %acir_opt %t/union-op.mlir 2>&1 | %FileCheck %s --check-prefix=UNION-OP
// RUN: %not %acir_opt %t/address.mlir 2>&1 | %FileCheck %s --check-prefix=ADDRESS
// RUN: %not %acir_opt %t/duration.mlir 2>&1 | %FileCheck %s --check-prefix=DURATION
// RUN: %not %acir_opt %t/rate.mlir 2>&1 | %FileCheck %s --check-prefix=RATE
// RUN: %not %acir_opt %t/map.mlir 2>&1 | %FileCheck %s --check-prefix=MAP
// RUN: %not %acir_opt %t/set.mlir 2>&1 | %FileCheck %s --check-prefix=SET
// RUN: %not %acir_opt %t/union-type.mlir 2>&1 | %FileCheck %s --check-prefix=UNION-TYPE
// RUN: %not %acir_opt %t/optional.mlir 2>&1 | %FileCheck %s --check-prefix=OPTIONAL
// RUN: %not %acir_opt %t/list.mlir 2>&1 | %FileCheck %s --check-prefix=LIST
// RUN: %not %acir_opt %t/vector.mlir 2>&1 | %FileCheck %s --check-prefix=VECTOR

// MODULE: error: unregistered operation 'ac.module.generated'
// UNION-OP: error: unregistered operation 'ac.union'
// ADDRESS: error: unknown type `address` in dialect `ac`
// DURATION: error: unknown type `duration` in dialect `ac`
// RATE: error: unknown type `rate` in dialect `ac`
// MAP: error: unknown type `map` in dialect `ac`
// SET: error: unknown type `set` in dialect `ac`
// UNION-TYPE: error: unknown type `union` in dialect `ac`
// OPTIONAL: error: unknown type `optional` in dialect `ac`
// LIST: error: unknown type `list` in dialect `ac`
// VECTOR: error: unknown type `vector` in dialect `ac`

//--- module-generated.mlir
module { "ac.module.generated"() : () -> () }
//--- union-op.mlir
module { "ac.union"() : () -> () }
//--- address.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.address<@x> }
//--- duration.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.duration<cycles> }
//--- rate.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.rate<bytes, cycles> }
//--- map.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.map<["x"], !ac.queue<i8>> }
//--- set.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.set<1 x !ac.var<i8>> }
//--- union-type.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.union<@types::@U> }
//--- optional.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.optional<i8> }
//--- list.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.list<i8> }
//--- vector.mlir
module { "builtin.unrealized_conversion_cast"() : () -> !ac.vector<4 x i8> }
