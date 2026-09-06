// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/create.mlir 2>&1 | %FileCheck %s --check-prefix=CREATE
// RUN: %not %acir_opt %t/get.mlir 2>&1 | %FileCheck %s --check-prefix=GET
// RUN: %not %acir_opt %t/with.mlir 2>&1 | %FileCheck %s --check-prefix=WITH
// RUN: %not %acir_opt %t/serialize.mlir 2>&1 | %FileCheck %s --check-prefix=SERIALIZE
// RUN: %not %acir_opt %t/deserialize.mlir 2>&1 | %FileCheck %s --check-prefix=DESERIALIZE

// CREATE: error: unregistered operation 'ac.record.create'
// GET: error: unregistered operation 'ac.record.get'
// WITH: error: unregistered operation 'ac.record.with'
// SERIALIZE: error: unregistered operation 'ac.packet.serialize'
// DESERIALIZE: error: unregistered operation 'ac.packet.deserialize'

//--- create.mlir
module { "ac.record.create"() : () -> () }

//--- get.mlir
module { "ac.record.get"() : () -> () }

//--- with.mlir
module { "ac.record.with"() : () -> () }

//--- serialize.mlir
module { "ac.packet.serialize"() : () -> () }

//--- deserialize.mlir
module { "ac.packet.deserialize"() : () -> () }
