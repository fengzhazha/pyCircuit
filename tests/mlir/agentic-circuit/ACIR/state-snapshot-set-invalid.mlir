// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/choose-valid-source.mlir 2>&1 | %FileCheck %s --check-prefix=CHOOSE-VALID
// RUN: %not %acir_opt %t/choose-missing-read.mlir 2>&1 | %FileCheck %s --check-prefix=MISSING-READ
// RUN: %not %acir_opt %t/shared-choose-read.mlir 2>&1 | %FileCheck %s --check-prefix=SHARED-READ
// RUN: %not %acir_opt %t/field-relation-capacity.mlir 2>&1 | %FileCheck %s --check-prefix=FIELD-CAPACITY

//--- choose-valid-source.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @entries entry i2 entries 4 init 0 owner "/" stable_id "table/entries"
  ac.table @priority entry i2 entries 4 init 0 owner "/" stable_id "table/priority"
  %output = ac.rule depths [1] latencies [1] name "issue" stable_id "issue"
      domain "cycle" type exact {
  ^body:
    %mask = ac.table.match @entries predicate {
    ^bb0(%entry: !ac.var<i2>):
      %true = ac.var.constant true as !ac.var<i1>
      ac.table.match.yield %true : !ac.var<i1>
    } -> !ac.var<i4>
    %index, %present = ac.table.choose @entries %mask : !ac.var<i4>
        count 1 policy "min" key {
    ^bb0(%entry: !ac.var<i2>):
      %priority = ac.table.get @priority[%entry] : !ac.var<i2> -> !ac.var<i2>
      ac.table.choose.yield %priority : !ac.var<i2>
    } -> !ac.var<i2>, !ac.var<i1>
    ac.rule.condition %present : !ac.var<i1>
    ac.state.snapshot_set @priority from %present : !ac.var<i1>
        for %present : !ac.var<i1> read_fields ["$entry"]
    %ready = ac.marker.obligation %index state pending resolver handshake
        origin "issue:return" path "true" : !ac.var<i2>
    ac.rule.return %ready : !ac.var<i2>
  } : () -> !ac.queue<i2>
  ac.sink %output : !ac.queue<i2>
}

// CHOOSE-VALID: source table.choose must use the owning rule/firing's index result

//--- choose-missing-read.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @entries entry i2 entries 4 init 0 owner "/" stable_id "table/entries"
  ac.table @priority entry i2 entries 4 init 0 owner "/" stable_id "table/priority"
  ac.table @other entry i2 entries 4 init 0 owner "/" stable_id "table/other"
  %output = ac.rule depths [1] latencies [1] name "issue" stable_id "issue"
      domain "cycle" type exact {
  ^body:
    %mask = ac.table.match @entries predicate {
    ^bb0(%entry: !ac.var<i2>):
      %true = ac.var.constant true as !ac.var<i1>
      ac.table.match.yield %true : !ac.var<i1>
    } -> !ac.var<i4>
    %index, %present = ac.table.choose @entries %mask : !ac.var<i4>
        count 1 policy "min" key {
    ^bb0(%entry: !ac.var<i2>):
      %priority = ac.table.get @priority[%entry] : !ac.var<i2> -> !ac.var<i2>
      ac.table.choose.yield %priority : !ac.var<i2>
    } -> !ac.var<i2>, !ac.var<i1>
    ac.rule.condition %present : !ac.var<i1>
    %unused = ac.table.get @other[%index] : !ac.var<i2> -> !ac.var<i2>
    ac.table.propose @entries[%index] = %index mode "replace"
        write_fields ["$entry"] : !ac.var<i2>, !ac.var<i2>
    ac.state.snapshot_set @other from %index : !ac.var<i2>
        for %present : !ac.var<i1> read_fields ["$entry"]
    %ready = ac.marker.obligation %index state pending resolver handshake
        origin "issue:return" path "true" : !ac.var<i2>
    ac.rule.return %ready : !ac.var<i2>
  } : () -> !ac.queue<i2>
  ac.sink %output : !ac.queue<i2>
}

// MISSING-READ: source evaluation must contain a region-local read of the target table

//--- shared-choose-read.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.table @entries entry i2 entries 4 init 0 owner "/" stable_id "table/entries"
  ac.table @priority entry i2 entries 4 init 0 owner "/" stable_id "table/priority"
  %mask = ac.table.match @entries predicate {
  ^bb0(%entry: !ac.var<i2>):
    %true = ac.var.constant true as !ac.var<i1>
    ac.table.match.yield %true : !ac.var<i1>
  } -> !ac.var<i4>
  %index, %present = ac.table.choose @entries %mask : !ac.var<i4>
      count 1 policy "min" key {
  ^bb0(%entry: !ac.var<i2>):
    %priority = ac.table.get @priority[%entry] : !ac.var<i2> -> !ac.var<i2>
    ac.table.choose.yield %priority : !ac.var<i2>
  } -> !ac.var<i2>, !ac.var<i1>
}

// SHARED-READ: key Table reads require transactional rule/firing ownership

//--- field-relation-capacity.mlir
module attributes {ac.contract_epoch = "0.5"} {
  ac.type_scope @types {
    ac.struct @Pair fields [{name = "a", type = i8}, {name = "b", type = i8}]
  } {dlti.dl_spec = #dlti.dl_spec<!ac.struct<@types::@Pair> = {abi_alignment = 1 : i64, endianness = "little", preferred_alignment = 1 : i64, size = 2 : i64}>}
  ac.table @wide entry !ac.struct<@types::@Pair> entries 64 init 0 owner "/"
      stable_id "table/wide"
  %input = "builtin.unrealized_conversion_cast"() : () -> !ac.queue<i6>
  ac.rule %input depths [] latencies [] name "read" stable_id "read"
      domain "cycle" type exact {
  ^body(%item: !ac.var<i6>):
    %old = ac.table.get @wide[%item] : !ac.var<i6> -> !ac.var<!ac.struct<@types::@Pair>>
    %true = ac.var.constant true as !ac.var<i1>
    ac.rule.condition %true : !ac.var<i1>
    ac.table.propose @wide[%item] = %old mode "replace" write_fields ["a", "b"]
        : !ac.var<i6>, !ac.var<!ac.struct<@types::@Pair>>
    ac.state.snapshot @wide[%item : !ac.var<i6>] for %true : !ac.var<i1>
        kind #ac<rule_index_kind dynamic> read_fields ["a"]
    ac.rule.return
  } : (!ac.queue<i6>) -> ()
}

// FIELD-CAPACITY: field-qualified snapshot exceeds the 64-bit entry/field relation
