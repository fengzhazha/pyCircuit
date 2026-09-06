// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/schema-fingerprint.mlir 2>&1 | %FileCheck %s --check-prefix=SCHEMA-FINGERPRINT
// RUN: %not %acir_opt %t/schema-range.mlir 2>&1 | %FileCheck %s --check-prefix=SCHEMA-RANGE
// RUN: %not %acir_opt %t/schema-order.mlir 2>&1 | %FileCheck %s --check-prefix=SCHEMA-ORDER
// RUN: %not %acir_opt %t/extract-stale.mlir 2>&1 | %FileCheck %s --check-prefix=EXTRACT-STALE
// RUN: %not %acir_opt %t/extract-range.mlir 2>&1 | %FileCheck %s --check-prefix=EXTRACT-RANGE
// RUN: %not %acir_opt %t/concat-width.mlir 2>&1 | %FileCheck %s --check-prefix=CONCAT-WIDTH

// SCHEMA-FINGERPRINT: error: 'ac.bitfield' op fingerprint does not match canonical schema
// SCHEMA-RANGE: error: 'ac.bitfield' op field 'bad' range must satisfy 0 <= lsb <= msb < width
// SCHEMA-ORDER: error: 'ac.bitfield' op fields must be sorted by UTF-8 name
// EXTRACT-STALE: error: 'ac.var.extract' op bitfield provenance fingerprint is stale
// EXTRACT-RANGE: error: 'ac.var.extract' op bitfield field range does not match its schema
// CONCAT-WIDTH: error: 'ac.var.concat' op bitfield concat input width does not match its field

//--- schema-fingerprint.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.bitfield @Instr width 8 fingerprint "sha256:0000000000000000000000000000000000000000000000000000000000000000" fields [{lsb = 4 : i64, msb = 7 : i64, name = "hi"}, {lsb = 0 : i64, msb = 3 : i64, name = "low"}, {lsb = 0 : i64, msb = 5 : i64, name = "low6"}]
  }
}

//--- schema-range.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.bitfield @Bad width 8 fingerprint "sha256:0000000000000000000000000000000000000000000000000000000000000000" fields [{lsb = 0 : i64, msb = 8 : i64, name = "bad"}]
  }
}

//--- schema-order.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.bitfield @Bad width 8 fingerprint "sha256:0000000000000000000000000000000000000000000000000000000000000000" fields [{lsb = 0 : i64, msb = 3 : i64, name = "low"}, {lsb = 4 : i64, msb = 7 : i64, name = "hi"}]
  }
}

//--- extract-stale.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.bitfield @Instr width 8 fingerprint "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652" fields [{lsb = 4 : i64, msb = 7 : i64, name = "hi"}, {lsb = 0 : i64, msb = 3 : i64, name = "low"}, {lsb = 0 : i64, msb = 5 : i64, name = "low6"}]
  }
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i8>
  %bad = ac.var.extract %value from 4 width 4 {ac.bitfield_field = "hi", ac.bitfield_fingerprint = "sha256:0000000000000000000000000000000000000000000000000000000000000000", ac.bitfield_schema = @types::@Instr} : !ac.var<i8> -> !ac.var<i4>
}

//--- extract-range.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.bitfield @Instr width 8 fingerprint "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652" fields [{lsb = 4 : i64, msb = 7 : i64, name = "hi"}, {lsb = 0 : i64, msb = 3 : i64, name = "low"}, {lsb = 0 : i64, msb = 5 : i64, name = "low6"}]
  }
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i8>
  %bad = ac.var.extract %value from 3 width 4 {ac.bitfield_field = "hi", ac.bitfield_fingerprint = "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652", ac.bitfield_schema = @types::@Instr} : !ac.var<i8> -> !ac.var<i4>
}

//--- concat-width.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.bitfield @Instr width 8 fingerprint "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652" fields [{lsb = 4 : i64, msb = 7 : i64, name = "hi"}, {lsb = 0 : i64, msb = 3 : i64, name = "low"}, {lsb = 0 : i64, msb = 5 : i64, name = "low6"}]
  }
  %value = "builtin.unrealized_conversion_cast"() : () -> !ac.var<i4>
  %bad = ac.var.concat %value {ac.bitfield_fields = ["low6"], ac.bitfield_fingerprint = "sha256:7e585b0bcdfac35c93e4c9dd0398c3df753968c61c7700b76373a37d81c80652", ac.bitfield_schema = @types::@Instr} : !ac.var<i4> -> !ac.var<i4>
}
