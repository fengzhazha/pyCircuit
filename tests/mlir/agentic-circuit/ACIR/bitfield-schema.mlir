// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt --emit-bytecode -o %t.bc %s
// RUN: %acir_opt %t.bc | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.bitfield @Instr width 8 fingerprint "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652" fields [{lsb = 4 : i64, msb = 7 : i64, name = "hi"}, {lsb = 0 : i64, msb = 3 : i64, name = "low"}, {lsb = 0 : i64, msb = 5 : i64, name = "low6"}]
  }
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i8>
  %hi = ac.var.extract %value from 4 width 4 {ac.bitfield_field = "hi", ac.bitfield_fingerprint = "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652", ac.bitfield_schema = @types::@Instr} : !ac.var<i8> -> !ac.var<i4>
  %low = ac.var.extract %value from 0 width 4 {ac.bitfield_field = "low", ac.bitfield_fingerprint = "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652", ac.bitfield_schema = @types::@Instr} : !ac.var<i8> -> !ac.var<i4>
  %joined = ac.var.concat %hi, %low {ac.bitfield_fields = ["hi", "low"], ac.bitfield_fingerprint = "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652", ac.bitfield_schema = @types::@Instr} : !ac.var<i4>, !ac.var<i4> -> !ac.var<i8>
  %updated = ac.var.insert %value, %low at 0 {ac.bitfield_field = "low", ac.bitfield_fingerprint = "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652", ac.bitfield_schema = @types::@Instr} : !ac.var<i8>, !ac.var<i4> -> !ac.var<i8>
}

// CHECK: ac.bitfield @Instr width 8 fingerprint "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652"
// CHECK: ac.var.extract
// CHECK-SAME: ac.bitfield_field = "hi"
// CHECK-SAME: ac.bitfield_schema = @types::@Instr
// CHECK: ac.var.concat
// CHECK-SAME: ac.bitfield_fields = ["hi", "low"]
// CHECK: ac.var.insert
// CHECK-SAME: ac.bitfield_field = "low"
