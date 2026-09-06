// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/duplicate-symbol.mlir 2>&1 | %FileCheck %s --check-prefix=DUP-SYMBOL
// RUN: %not %acir_opt %t/duplicate-field.mlir 2>&1 | %FileCheck %s --check-prefix=DUP-FIELD
// RUN: %not %acir_opt %t/duplicate-enumerant.mlir 2>&1 | %FileCheck %s --check-prefix=DUP-ENUM
// RUN: %not %acir_opt %t/unresolved-field.mlir 2>&1 | %FileCheck %s --check-prefix=UNRESOLVED
// RUN: %not %acir_opt %t/wrong-kind.mlir 2>&1 | %FileCheck %s --check-prefix=WRONG-KIND
// RUN: %not %acir_opt %t/recursive.mlir 2>&1 | %FileCheck %s --check-prefix=RECURSIVE
// RUN: %not %acir_opt %t/alias-target.mlir 2>&1 | %FileCheck %s --check-prefix=ALIAS

// DUP-SYMBOL: error: {{.*}}redefinition of symbol named 'Item'
// DUP-FIELD: error: {{.*}}duplicate field 'x'
// DUP-ENUM: error: {{.*}}duplicate enumerant 'read'
// UNRESOLVED: error: {{.*}}unresolved named data type '@types::@Missing'
// WRONG-KIND: error: {{.*}}named type '@types::@Mode' requires ac.struct but resolves to ac.enum
// RECURSIVE: error: {{.*}}unbounded value recursion through '@Node'
// ALIAS: error: {{.*}}unresolved named data type '@types::@Missing'

//--- duplicate-symbol.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "Item", fields = []}> : () -> ()
    "ac.transaction"() <{sym_name = "Item", fields = []}> : () -> ()
  }) : () -> ()
}

//--- duplicate-field.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "Pair", fields = [{name = "x", type = i8}, {name = "x", type = i16}]}> : () -> ()
  }) : () -> ()
}

//--- duplicate-enumerant.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    ac.enum @Mode enumerants ["read", "read"]
  }) : () -> ()
}

//--- unresolved-field.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "Holder", fields = [{name = "item", type = !ac.struct<@types::@Missing>}]}> : () -> ()
  }) : () -> ()
}

//--- wrong-kind.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    ac.enum @Mode enumerants ["read"]
    "ac.transaction"() <{sym_name = "Holder", fields = [{name = "mode", type = !ac.struct<@types::@Mode>}]}> : () -> ()
  }) {dlti.dl_spec = #dlti.dl_spec<!ac.enum<@types::@Mode> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}>} : () -> ()
}

//--- recursive.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "Node", fields = [{name = "next", type = !ac.transaction<@types::@Node>}]}> : () -> ()
  }) : () -> ()
}

//--- alias-target.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.type_alias"() <{sym_name = "MissingAlias", target = !ac.struct<@types::@Missing>}> : () -> ()
  }) : () -> ()
}
