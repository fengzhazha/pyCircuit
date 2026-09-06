// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/channel-standalone.mlir 2>&1 | %FileCheck %s --check-prefix=CHANNEL-STANDALONE
// RUN: %not %acir_opt %t/channel-tuple.mlir 2>&1 | %FileCheck %s --check-prefix=CHANNEL-TUPLE
// RUN: %not %acir_opt %t/channel-function.mlir 2>&1 | %FileCheck %s --check-prefix=CHANNEL-FUNCTION
// RUN: %not %acir_opt %t/channel-type-attr.mlir 2>&1 | %FileCheck %s --check-prefix=CHANNEL-TYPE-ATTR
// RUN: %not %acir_opt %t/channel-composite-attr.mlir 2>&1 | %FileCheck %s --check-prefix=CHANNEL-COMPOSITE-ATTR
// RUN: %not %acir_opt %t/malformed-named.mlir 2>&1 | %FileCheck %s --check-prefix=MALFORMED-NAMED
// RUN: %not %acir_opt %t/malformed-topology.mlir 2>&1 | %FileCheck %s --check-prefix=MALFORMED-TOPOLOGY

// CHANNEL-STANDALONE: error: channel type is only permitted in an ac.interface channel declaration
// CHANNEL-TUPLE: topology type '!ac.channel<i8, @ready_valid>' cannot be nested inside 'tuple<i8, tuple<!ac.channel<i8, @ready_valid>>>'
// CHANNEL-FUNCTION: channel type is only permitted in an ac.interface channel declaration
// CHANNEL-TYPE-ATTR: error: channel type is only permitted in an ac.interface channel declaration
// CHANNEL-COMPOSITE-ATTR: topology type '!ac.channel<i8, @ready_valid>' cannot be nested inside 'tuple<i8, !ac.channel<i8, @ready_valid>>'
// MALFORMED-NAMED: error: failed to parse ACIR_StructType parameter 'name'
// MALFORMED-TOPOLOGY: error: expected ','

//--- channel-standalone.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.channel<i8, @ready_valid>
}

//--- channel-tuple.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> tuple<i8, tuple<!ac.channel<i8, @ready_valid>>>
}

//--- channel-function.mlir
builtin.module attributes {
  ac.contract_epoch = "0.5",
  test.signature = (i8) -> !ac.channel<i8, @ready_valid>
} {
}

//--- channel-type-attr.mlir
builtin.module attributes {
  ac.contract_epoch = "0.5",
  test.type = !ac.channel<i8, @ready_valid>
} {
}

//--- channel-composite-attr.mlir
builtin.module attributes {
  ac.contract_epoch = "0.5",
  test.types = [tuple<i8, !ac.channel<i8, @ready_valid>>]
} {
}

//--- malformed-named.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.struct<i8>
}

//--- malformed-topology.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "builtin.unrealized_conversion_cast"() : () -> !ac.flow<i8>
}\n
