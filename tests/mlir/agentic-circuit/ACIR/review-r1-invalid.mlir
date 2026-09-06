// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/declaration-placement.mlir 2>&1 | %FileCheck %s --check-prefix=PLACEMENT
// RUN: %not %acir_opt %t/function-field.mlir 2>&1 | %FileCheck %s --check-prefix=FUNCTION
// RUN: %not %acir_opt %t/channel-field.mlir 2>&1 | %FileCheck %s --check-prefix=CHANNEL
// RUN: %not %acir_opt %t/capability-field.mlir 2>&1 | %FileCheck %s --check-prefix=CAPABILITY
// RUN: %not %acir_opt %t/none-field.mlir 2>&1 | %FileCheck %s --check-prefix=NONE
// RUN: %not %acir_opt %t/list-bound-inconsistent.mlir 2>&1 | %FileCheck %s --check-prefix=BOUND-INCONSISTENT
// RUN: %not %acir_opt %t/layout-missing.mlir 2>&1 | %FileCheck %s --check-prefix=LAYOUT-MISSING
// RUN: %not %acir_opt %t/layout-invalid.mlir 2>&1 | %FileCheck %s --check-prefix=LAYOUT-INVALID
// RUN: %not %acir_opt %t/packet-width-missing.mlir 2>&1 | %FileCheck %s --check-prefix=PACKET-WIDTH-MISSING

// PLACEMENT: error: {{.*}}named data declarations must be direct children of ac.type_scope
// FUNCTION: error: {{.*}}field 'bad' has non-value type
// CHANNEL: error: {{.*}}field 'bad' has non-value type
// CAPABILITY: error: {{.*}}field 'bad' has non-value type
// NONE: error: {{.*}}field 'bad' has non-value type
// BOUND-INCONSISTENT: error: {{.*}}field 'value' cannot declare removed max_length
// LAYOUT-MISSING: error: {{.*}}missing DLTI layout entry for '!ac.struct<@types::@S>'
// LAYOUT-INVALID: error: {{.*}}layout entry requires positive size/alignment and explicit endianness
// PACKET-WIDTH-MISSING: error: packet layout entry requires positive serialization_width

//--- declaration-placement.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.struct"() <{sym_name = "S", fields = []}> : () -> ()
}

//--- function-field.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.struct"() <{sym_name = "S", fields = [{name = "bad", type = (i8) -> i8}]}> : () -> ()
  }) : () -> ()
}

//--- channel-field.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.struct"() <{sym_name = "S", fields = [{name = "bad", type = !ac.channel<i8, @p>}]}> : () -> ()
  }) : () -> ()
}

//--- capability-field.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "T", fields = [{name = "bad", type = !ac.resource_token<@r>}]}> : () -> ()
  }) : () -> ()
}

//--- none-field.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "T", fields = [{name = "bad", type = none}]}> : () -> ()
  }) : () -> ()
}

//--- list-bound-inconsistent.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.struct"() <{sym_name = "S", fields = [{name = "value", type = i8, max_length = 4 : i64}]}> : () -> ()
  }) : () -> ()
}

//--- layout-missing.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.struct"() <{sym_name = "S", fields = []}> : () -> ()
  }) {dlti.dl_spec = #dlti.dl_spec<>} : () -> ()
}

//--- layout-invalid.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.struct"() <{sym_name = "S", fields = []}> : () -> ()
  }) {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@S> = {abi_alignment = 0 : i64, endianness = "middle", preferred_alignment = 0 : i64, size = 0 : i64}>} : () -> ()
}

//--- packet-width-missing.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.packet"() <{sym_name = "P", fields = []}> : () -> ()
  }) {dlti.dl_spec = #dlti.dl_spec<!ac.packet<@types::@P> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 8 : i64}>} : () -> ()
}
