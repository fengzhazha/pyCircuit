// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt %s | %acir_opt | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.type_alias"() <{sym_name = "Word", target = i32}> : () -> ()
    "ac.struct"() <{sym_name = "Header", fields = [{name = "opcode", type = i8}, {name = "tag", type = i16}]}> : () -> ()
    ac.enum @Opcode enumerants ["read", "write"]
    "ac.packet"() <{sym_name = "Request", fields = [{name = "opcode", type = i8}, {name = "payload", type = i32}]}> : () -> ()
    "ac.transaction"() <{sym_name = "Dma", fields = [{name = "request", type = !ac.packet<@types::@Request>}, {name = "tag", type = i16}]}> : () -> ()
  }) {dlti.dl_spec = #dlti.dl_spec<
    !ac.struct<@types::@Header> = {abi_alignment = 2 : i64, endianness = "little", preferred_alignment = 2 : i64, size = 4 : i64},
    !ac.enum<@types::@Opcode> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64},
    !ac.packet<@types::@Request> = {abi_alignment = 4 : i64, endianness = "little", preferred_alignment = 4 : i64, serialization_width = 8 : i64, size = 8 : i64}
  >} : () -> ()
}

// CHECK: ac.type_scope @types
// CHECK: "ac.type_alias"
// CHECK: ac.struct @Header
// CHECK: ac.enum @Opcode enumerants ["read", "write"]
// CHECK: "ac.packet"
// CHECK: ac.transaction @Dma
