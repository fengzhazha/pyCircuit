// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt %s | %acir_opt | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.struct"() <{sym_name = "S", fields = [{name = "value", type = i32}]}> : () -> ()
    ac.enum @E enumerants ["a", "b"]
    "ac.union"() <{sym_name = "U", fields = [{name = "tag", type = !ac.enum<@types::@E>}, {name = "value", type = i32}], discriminator = "tag"}> : () -> ()
    "ac.packet"() <{sym_name = "P", fields = [{name = "items", type = !ac.list<i8>, max_length = 8 : i64}]}> : () -> ()
    "ac.transaction"() <{sym_name = "T", fields = [{name = "packet", type = !ac.packet<@types::@P>}]}> : () -> ()
  }) {dlti.dl_spec = #dlti.dl_spec<
    !ac.struct<@types::@S> = {abi_alignment = 4 : i64, endianness = "little", preferred_alignment = 4 : i64, size = 4 : i64},
    !ac.packet<@types::@P> = {abi_alignment = 8 : i64, endianness = "big", preferred_alignment = 8 : i64, serialization_width = 8 : i64, size = 16 : i64},
    !ac.enum<@types::@E> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64},
    !ac.union<@types::@U> = {abi_alignment = 4 : i64, endianness = "little", preferred_alignment = 4 : i64, size = 4 : i64}
  >} : () -> ()

  %i32 = "builtin.unrealized_conversion_cast"() : () -> i32
  %s = "ac.record.create"(%i32) <{field_names = ["value"]}> : (i32) -> !ac.struct<@types::@S>
  %value = "ac.record.get"(%s) <{field = "value"}> : (!ac.struct<@types::@S>) -> i32
  %s2 = "ac.record.with"(%s, %value) <{field = "value"}> : (!ac.struct<@types::@S>, i32) -> !ac.struct<@types::@S>
  %packet = "builtin.unrealized_conversion_cast"() : () -> !ac.packet<@types::@P>
  %bytes = "ac.packet.serialize"(%packet) <{packet = @types::@P}> : (!ac.packet<@types::@P>) -> !ac.vector<8 x i8>
  %copy = "ac.packet.deserialize"(%bytes) <{packet = @types::@P}> : (!ac.vector<8 x i8>) -> !ac.packet<@types::@P>
}

// CHECK: dlti.dl_spec
// CHECK: !ac.struct<@types::@S>
// CHECK: !ac.packet<@types::@P>
// CHECK: serialization_width = 8 : i64
// CHECK: "ac.packet.serialize"
// CHECK-SAME: packet = @types::@P
