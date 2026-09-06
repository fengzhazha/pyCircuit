// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/no-selected.mlir 2>&1 | %FileCheck %s --check-prefix=NO-SELECTED
// RUN: %not %acir_opt %t/bad-ref.mlir 2>&1 | %FileCheck %s --check-prefix=BAD-REF
// RUN: %not %acir_opt %t/bad-call.mlir 2>&1 | %FileCheck %s --check-prefix=BAD-CALL
// RUN: %not %acir_opt %t/bad-return.mlir 2>&1 | %FileCheck %s --check-prefix=BAD-RETURN
// RUN: %not %acir_opt %t/duplicate-path.mlir 2>&1 | %FileCheck %s --check-prefix=DUP-PATH
// RUN: %not %acir_opt %t/dynamic-param.mlir 2>&1 | %FileCheck %s --check-prefix=DYNAMIC
// RUN: %not %acir_opt %t/private-export.mlir 2>&1 | %FileCheck %s --check-prefix=PRIVATE
// RUN: %not %acir_opt %t/missing-static-arg.mlir 2>&1 | %FileCheck %s --check-prefix=STATIC-ARG
// RUN: %not %acir_opt %t/unresolved-static-symbol.mlir 2>&1 | %FileCheck %s --check-prefix=STATIC-SYMBOL
// RUN: %not %acir_opt %t/nonzero-epoch.mlir 2>&1 | %FileCheck %s --check-prefix=EPOCH
// RUN: %not %acir_opt %t/unresolved-workload.mlir 2>&1 | %FileCheck %s --check-prefix=WORKLOAD
// RUN: %not %acir_opt %t/direct-recursion.mlir 2>&1 | %FileCheck %s --check-prefix=DIRECT-RECURSION
// RUN: %not %acir_opt %t/mutual-recursion.mlir 2>&1 | %FileCheck %s --check-prefix=MUTUAL-RECURSION
// RUN: %not %acir_opt %t/absolute-local-path.mlir 2>&1 | %FileCheck %s --check-prefix=LOCAL-PATH
// RUN: %not %acir_opt %t/duplicate-id.mlir 2>&1 | %FileCheck %s --check-prefix=DUP-ID
// RUN: %not %acir_opt %t/mutual-recursion.mlir > /dev/null 2> %t/mutual.first
// RUN: %not %acir_opt %t/mutual-recursion.mlir > /dev/null 2> %t/mutual.second
// RUN: diff %t/mutual.first %t/mutual.second
// RUN: %not %acir_opt %t/static-arg-type.mlir 2>&1 | %FileCheck %s --check-prefix=STATIC-TYPE
// RUN: %not %acir_opt %t/orphan-instance.mlir 2>&1 | %FileCheck %s --check-prefix=ORPHAN
// RUN: %not %acir_opt %t/nested-module.mlir 2>&1 | %FileCheck %s --check-prefix=NESTED-MODULE
// RUN: %not %acir_opt %t/two-selected.mlir 2>&1 | %FileCheck %s --check-prefix=TWO-SELECTED
// RUN: %not %acir_opt %t/unknown-provider.mlir 2>&1 | %FileCheck %s --check-prefix=PROVIDER
// RUN: %not %acir_opt %t/extern-root.mlir 2>&1 | %FileCheck %s --check-prefix=EXTERN-ROOT
// RUN: %not %acir_opt %t/negative-seed.mlir 2>&1 | %FileCheck %s --check-prefix=SEED
// RUN: %not %acir_opt %t/bad-result-schema.mlir 2>&1 | %FileCheck %s --check-prefix=RESULT-SCHEMA
// RUN: %not %acir_opt %t/bad-instrumentation.mlir 2>&1 | %FileCheck %s --check-prefix=INSTRUMENTATION
// RUN: %not %acir_opt %t/static-array.mlir 2>&1 | %FileCheck %s --check-prefix=STATIC-ARRAY
// RUN: %not %acir_opt %t/bad-unit.mlir 2>&1 | %FileCheck %s --check-prefix=BAD-UNIT
// RUN: %not %acir_opt %t/bad-seed-type.mlir 2>&1 | %FileCheck %s --check-prefix=SEED-TYPE
// RUN: %not %acir_opt %t/unresolved-instrumentation.mlir 2>&1 | %FileCheck %s --check-prefix=INSTRUMENTATION-REF

//--- no-selected.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "default", format = "json"}, selected = false}> : () -> ()
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
}
// NO-SELECTED: ACIR file requires exactly one selected ac.system, found 0

//--- bad-ref.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.system"() <{sym_name = "s", root = @Missing, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "default", format = "json"}, selected = true}> : () -> ()
}
// BAD-REF: selected root must resolve to a materialized ac.module

//--- bad-call.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Leaf", function_type = (i32) -> i32, static_params = {}}> ({
  ^bb0(%x : i32):
    "ac.return"(%x) : (i32) -> ()
  }) : () -> ()
  "ac.module"() <{sym_name = "Top", function_type = (i64) -> i64, static_params = {}}> ({
  ^bb0(%x : i64):
    %v = "ac.instance"(%x) <{definition = @Leaf, sym_name = "x", stable_id = "x", path = "x", static_args = {}}> : (i64) -> i64
    "ac.return"(%v) : (i64) -> ()
  }) : () -> ()
}
// BAD-CALL: operand types do not match module signature

//--- bad-return.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = (i32) -> i64, static_params = {}}> ({
  ^bb0(%x : i32):
    "ac.return"(%x) : (i32) -> ()
  }) : () -> ()
}
// BAD-RETURN: operand types and count must exactly match module results

//--- duplicate-path.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "default", format = "json"}, selected = true}> : () -> ()
  "ac.module.extern"() <{sym_name = "Leaf", function_type = () -> (), static_params = {}, implementation = {registry = "cpp", name = "Leaf"}}> : () -> ()
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @Leaf, sym_name = "a", stable_id = "a", path = "same", static_args = {}}> : () -> ()
    "ac.instance"() <{definition = @Leaf, sym_name = "b", stable_id = "b", path = "same", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
}
// DUP-PATH: duplicate local structural path 'same'

//--- dynamic-param.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module.extern"() <{sym_name = "Leaf", function_type = () -> (), static_params = {gain = 1.0 : f32}, implementation = {registry = "cpp", name = "Leaf"}}> : () -> ()
}
// DYNAMIC: static parameters must contain only concrete builtin static values

//--- private-export.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = (!ac.resource_token<@owned>) -> !ac.resource_token<@owned>, static_params = {}}> ({
  ^bb0(%token : !ac.resource_token<@owned>):
    "ac.return"(%token) : (!ac.resource_token<@owned>) -> ()
  }) : () -> ()
}
// PRIVATE: private ownership handle cannot be exported from ac.module

//--- missing-static-arg.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module.extern"() <{sym_name = "Leaf", function_type = () -> (), static_params = {width = 8 : i64}, implementation = {registry = "cpp", name = "Leaf"}}> : () -> ()
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @Leaf, sym_name = "leaf", stable_id = "leaf", path = "leaf", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
}
// STATIC-ARG: static argument names must exactly match definition parameters

//--- unresolved-static-symbol.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module.extern"() <{sym_name = "Leaf", function_type = () -> (), static_params = {target = @Missing}, implementation = {registry = "cpp", name = "Leaf"}}> : () -> ()
}
// STATIC-SYMBOL: unresolved static symbol reference '@Missing'

//--- nonzero-epoch.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module.extern"() <{sym_name = "Top", function_type = () -> (), static_params = {}, implementation = {registry = "cpp", name = "Top"}}> : () -> ()
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 1 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "default", format = "json"}, selected = true}> : () -> ()
}
// EPOCH: global tick epoch must be exactly 0

//--- unresolved-workload.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", primary_workload = @Top::@missing, seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "default", format = "json"}, selected = true}> : () -> ()
}
// WORKLOAD: primary workload '@Top::@missing' is unresolved

//--- direct-recursion.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Loop", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @Loop, sym_name = "self", stable_id = "self", path = "self", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
}
// DIRECT-RECURSION: recursive module instantiation cycle: @Loop -> @Loop

//--- mutual-recursion.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "A", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @B, sym_name = "b", stable_id = "b", path = "b", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
  "ac.module"() <{sym_name = "B", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @A, sym_name = "a", stable_id = "a", path = "a", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
}
// MUTUAL-RECURSION: recursive module instantiation cycle: @A -> @B -> @A

//--- absolute-local-path.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module.extern"() <{sym_name = "Leaf", function_type = () -> (), static_params = {}, implementation = {registry = "cpp", name = "Leaf"}}> : () -> ()
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @Leaf, sym_name = "leaf", stable_id = "leaf", path = "root.leaf", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
}
// LOCAL-PATH: path must be stable local segments

//--- duplicate-id.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "default", format = "json"}, selected = true}> : () -> ()
  "ac.module.extern"() <{sym_name = "Leaf", function_type = () -> (), static_params = {}, implementation = {registry = "cpp", name = "Leaf"}}> : () -> ()
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @Leaf, sym_name = "a", stable_id = "same", path = "a", static_args = {}}> : () -> ()
    "ac.instance"() <{definition = @Leaf, sym_name = "b", stable_id = "same", path = "b", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
}
// DUP-ID: duplicate local structural stable id 'same'

//--- static-arg-type.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module.extern"() <{sym_name = "Leaf", function_type = () -> (), static_params = {width = 8 : i64}, implementation = {registry = "cpp", name = "Leaf"}}> : () -> ()
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @Leaf, sym_name = "leaf", stable_id = "leaf", path = "leaf", static_args = {width = 8 : i32}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
}
// STATIC-TYPE: static argument 'width' must match parameter attribute type 'i64'

//--- orphan-instance.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Leaf", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  "ac.instance"() <{definition = @Leaf, sym_name = "x", stable_id = "x", path = "x", static_args = {}}> : () -> ()
}
// ORPHAN: must be a direct child of the unique ac.module Graph block

//--- nested-module.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.module"() <{sym_name = "Nested", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  }) : () -> ()
}
// NESTED-MODULE: must be a direct child of the outer builtin.module

//--- two-selected.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  "ac.system"() <{sym_name = "a", root = @Top, root_name = "a", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "a", format = "json"}, selected = true}> : () -> ()
  "ac.system"() <{sym_name = "b", root = @Top, root_name = "b", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "b", format = "json"}, selected = true}> : () -> ()
}
// TWO-SELECTED: exactly one selected ac.system, found 2

//--- unknown-provider.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module.extern"() <{sym_name = "Ext", function_type = () -> (), static_params = {}, implementation = {registry = "cpp", name = "not_registered"}}> : () -> ()
}
// PROVIDER: structural provider 'cpp:not_registered' is not registered

//--- extern-root.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module.extern"() <{sym_name = "Ext", function_type = () -> (), static_params = {}, implementation = {registry = "cpp", name = "Ext"}}> : () -> ()
  "ac.system"() <{sym_name = "s", root = @Ext, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "x", format = "json"}, selected = true}> : () -> ()
}
// EXTERN-ROOT: selected root must resolve to a materialized ac.module

//--- negative-seed.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = -1 : i64}, instrumentation = [], result_schema = {id = "x", format = "json"}, selected = true}> : () -> ()
}
// SEED: fixed seed value must be a non-negative signless i64

//--- bad-result-schema.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [], result_schema = {id = "", format = "text"}, selected = true}> : () -> ()
}
// RESULT-SCHEMA: result schema requires exact {id = non-empty string, format = "json"}

//--- bad-instrumentation.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = ["trace"], result_schema = {id = "x", format = "json"}, selected = true}> : () -> ()
}
// INSTRUMENTATION: instrumentation entries must be symbol references

//--- static-array.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {bad = [1 : i64]}}> ({ "ac.return"() : () -> () }) : () -> ()
}
// STATIC-ARRAY: static parameters must contain only concrete builtin static values

//--- bad-unit.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {latency = {value = 1 : i64, unit = "ns", extra = true}}}> ({ "ac.return"() : () -> () }) : () -> ()
}
// BAD-UNIT: static parameters must contain only concrete builtin static values

//--- bad-seed-type.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : ui32}, instrumentation = [], result_schema = {id = "x", format = "json"}, selected = true}> : () -> ()
}
// SEED-TYPE: seed policy requires exact {kind = "fixed", value = signless i64} schema

//--- unresolved-instrumentation.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.module"() <{sym_name = "Top", function_type = () -> (), static_params = {}}> ({ "ac.return"() : () -> () }) : () -> ()
  "ac.system"() <{sym_name = "s", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 0 : i64}, instrumentation = [@Top::@missing::@trace], result_schema = {id = "x", format = "json"}, selected = true}> : () -> ()
}
// INSTRUMENTATION-REF: instrumentation reference '@Top::@missing::@trace' does not resolve to ac.instrumentation
