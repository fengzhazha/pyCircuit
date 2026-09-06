// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt %s | %acir_opt | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.struct"() <{sym_name = "S", fields = [{name = "value", type = i32}]}> : () -> ()
    ac.enum @E enumerants ["a", "b"]
    "ac.packet"() <{sym_name = "P", fields = [{name = "value", type = i8}]}> : () -> ()
  }) {dlti.dl_spec = #dlti.dl_spec<
    !ac.struct<@types::@S> = {abi_alignment = 4 : i64, endianness = "little", preferred_alignment = 4 : i64, size = 4 : i64},
    !ac.packet<@types::@P> = {abi_alignment = 1 : i64, endianness = "big", preferred_alignment = 1 : i64, serialization_width = 1 : i64, size = 1 : i64},
    !ac.enum<@types::@E> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}
  >} : () -> ()
}

// CHECK: dlti.dl_spec
// CHECK: !ac.struct<@types::@S>
// CHECK: !ac.packet<@types::@P>
// CHECK: !ac.enum<@types::@E>
