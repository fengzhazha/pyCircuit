// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/malformed-field.mlir 2>&1 | %FileCheck %s --check-prefix=MALFORMED
// RUN: %not %acir_opt %t/missing-name.mlir 2>&1 | %FileCheck %s --check-prefix=MISSING-NAME
// RUN: %not %acir_opt %t/missing-type.mlir 2>&1 | %FileCheck %s --check-prefix=MISSING-TYPE
// RUN: %not %acir_opt %t/wrong-typed-non-list-bound.mlir 2>&1 | %FileCheck %s --check-prefix=WRONG-BOUND

// MALFORMED: error: {{.*}}field metadata requires string 'name' and type 'type'
// MISSING-NAME: error: {{.*}}field metadata requires string 'name' and type 'type'
// MISSING-TYPE: error: {{.*}}field metadata requires string 'name' and type 'type'
// WRONG-BOUND: error: {{.*}}field 'value' cannot declare removed max_length

//--- malformed-field.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "T", fields = ["oops"]}> : () -> ()
  }) : () -> ()
}

//--- missing-name.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "T", fields = [{type = i8}]}> : () -> ()
  }) : () -> ()
}

//--- missing-type.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "T", fields = [{name = "value"}]}> : () -> ()
  }) : () -> ()
}

//--- wrong-typed-non-list-bound.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "T", fields = [{name = "value", type = i8, max_length = "oops"}]}> : () -> ()
  }) : () -> ()
}
