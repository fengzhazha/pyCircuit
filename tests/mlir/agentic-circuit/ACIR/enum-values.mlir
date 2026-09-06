// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt --emit-bytecode -o %t.bc %s
// RUN: %acir_opt %t.bc | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.enum @Mode enumerants ["idle", "run", "wait"]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.enum<@types::@Mode> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 1 : i64}>}
  %run = ac.var.enum @types::@Mode "run" : !ac.var<!ac.enum<@types::@Mode>>
  %wait = ac.var.enum @types::@Mode "wait" : !ac.var<!ac.enum<@types::@Mode>>
  %different = ac.var.cmp "ne" %run, %wait : !ac.var<!ac.enum<@types::@Mode>> -> !ac.var<i1>
}

// CHECK: ac.var.enum @types::@Mode "run"
// CHECK: ac.var.enum @types::@Mode "wait"
// CHECK: ac.var.cmp "ne"
