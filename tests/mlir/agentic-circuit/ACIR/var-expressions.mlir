// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt %s | %acir_opt | %FileCheck %s
// RUN: %acir_opt --emit-bytecode -o %t.bc %s
// RUN: %acir_opt %t.bc | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.struct"() <{sym_name = "WorkItem", fields = [{name = "value", type = i64}, {name = "remaining", type = i16}]}> : () -> ()
  }) {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@WorkItem> = {abi_alignment = 8 : i64, endianness = "little", preferred_alignment = 8 : i64, size = 16 : i64}>} : () -> ()
  %input = ac.source depth 4 latency 1 : !ac.queue<!ac.struct<@types::@WorkItem>>
  %output = ac.transform %input depths [8] latencies [1] {
  ^body(%item: !ac.var<!ac.struct<@types::@WorkItem>>):
    %one64 = ac.var.constant 1 : i64 as !ac.var<i64>
    %one16 = ac.var.constant 1 : i16 as !ac.var<i16>
    %bits3 = ac.var.constant 5 : i3 as !ac.var<i3>
    %shift3 = ac.var.constant 1 : i3 as !ac.var<i3>
    %value = ac.var.get %item field "value" : !ac.var<!ac.struct<@types::@WorkItem>> -> !ac.var<i64>
    %remaining = ac.var.get %item field "remaining" : !ac.var<!ac.struct<@types::@WorkItem>> -> !ac.var<i16>
    %sum = ac.var.add %value, %one64 : !ac.var<i64>
    %difference = ac.var.sub %remaining, %one16 : !ac.var<i16>
    %product = ac.var.mul %sum, %one64 : !ac.var<i64>
    %positive = ac.var.cmp "sgt" %product, %one64 : !ac.var<i64> -> !ac.var<i1>
    %anded = ac.var.and %bits3, %shift3 : !ac.var<i3>
    %ored = ac.var.or %bits3, %shift3 : !ac.var<i3>
    %xored = ac.var.xor %bits3, %shift3 : !ac.var<i3>
    %inverted = ac.var.not %bits3 : !ac.var<i3> -> !ac.var<i3>
    %shifted_left = ac.var.shl %bits3, %shift3 : !ac.var<i3>
    %shifted_right = ac.var.shr %bits3, %shift3 : !ac.var<i3>
    %priority_index, %priority_valid = ac.var.priority_encode %bits3 order "high" : !ac.var<i3> -> !ac.var<i2>, !ac.var<i1>
    %leading = ac.var.count_zeros %bits3 direction "leading" : !ac.var<i3> -> !ac.var<i2>
    %trailing = ac.var.count_zeros %bits3 direction "trailing" : !ac.var<i3> -> !ac.var<i2>
    %extracted = ac.var.extract %bits3 from 1 width 2 : !ac.var<i3> -> !ac.var<i2>
    %concatenated = ac.var.concat %extracted, %shift3 : !ac.var<i2>, !ac.var<i3> -> !ac.var<i5>
    %inserted = ac.var.insert %bits3, %extracted at 1 : !ac.var<i3>, !ac.var<i2> -> !ac.var<i3>
    %updated_value = ac.var.with %item, %product field "value" : !ac.var<!ac.struct<@types::@WorkItem>>, !ac.var<i64> -> !ac.var<!ac.struct<@types::@WorkItem>>
    %updated = ac.var.with %updated_value, %difference field "remaining" : !ac.var<!ac.struct<@types::@WorkItem>>, !ac.var<i16> -> !ac.var<!ac.struct<@types::@WorkItem>>
    %selected = ac.var.select %positive, %updated, %item : !ac.var<i1>, !ac.var<!ac.struct<@types::@WorkItem>> -> !ac.var<!ac.struct<@types::@WorkItem>>
    ac.transform.yield %selected : !ac.var<!ac.struct<@types::@WorkItem>>
  } : (!ac.queue<!ac.struct<@types::@WorkItem>>) -> !ac.queue<!ac.struct<@types::@WorkItem>>
  ac.sink %output : !ac.queue<!ac.struct<@types::@WorkItem>>
}

// CHECK: ac.var.constant 1 : i64 as !ac.var<i64>
// CHECK: ac.var.get {{.*}} field "value"
// CHECK: ac.var.add
// CHECK: ac.var.sub
// CHECK: ac.var.mul
// CHECK: ac.var.cmp "sgt"
// CHECK: ac.var.and
// CHECK: ac.var.or
// CHECK: ac.var.xor
// CHECK: ac.var.not
// CHECK: ac.var.shl
// CHECK: ac.var.shr
// CHECK: ac.var.priority_encode
// CHECK: ac.var.count_zeros {{.*}} direction "leading"
// CHECK: ac.var.count_zeros {{.*}} direction "trailing"
// CHECK: ac.var.extract
// CHECK: ac.var.concat
// CHECK: ac.var.insert
// CHECK: ac.var.with
// CHECK: ac.var.select
