// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt %s | %acir_opt | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.system"() <{sym_name = "soc", root = @Top, root_name = "root", tick_epoch = 0 : i64, tick_unit = "cycle", primary_workload = @Top::@workload, seed_policy = {kind = "fixed", value = 7 : i64}, instrumentation = [@Top::@workload::@trace], result_schema = {id = "default", format = "json"}, selected = true}> : () -> ()
  "ac.system"() <{sym_name = "leaf_harness", root = @Leaf, root_name = "leaf", tick_epoch = 0 : i64, tick_unit = "cycle", seed_policy = {kind = "fixed", value = 9 : i64}, instrumentation = [], result_schema = {id = "default", format = "json"}, selected = false}> : () -> ()

  "ac.module"() <{sym_name = "Top", function_type = (i32) -> i32, static_params = {}}> ({
  ^bb0(%arg0 : i32):
    %0 = "ac.instance"(%arg0) <{definition = @Leaf, sym_name = "child", stable_id = "child", path = "child", static_args = {}}> : (i32) -> i32
    "ac.instance"() <{definition = @Reusable, sym_name = "left", stable_id = "left", path = "left", static_args = {}}> : () -> ()
    "ac.instance"() <{definition = @Reusable, sym_name = "right", stable_id = "right", path = "right", static_args = {}}> : () -> ()
    ac.process @workload kind "workload" captures(%arg0 : i32) {
    ^bb0(%capture : i32):
      ac.instrumentation @trace {}
      ac.yield_sim
    }
    "ac.return"(%0) : (i32) -> ()
  }) : () -> ()

  "ac.module"() <{sym_name = "Leaf", function_type = (i32) -> i32, static_params = {}}> ({
  ^bb0(%arg0 : i32):
    ac.process @state kind "control" { ac.yield_sim }
    "ac.return"(%arg0) : (i32) -> ()
  }) : () -> ()

  // Static symbol references inside modules and structural arguments resolve
  // in the enclosing builtin.module symbol table.
  "ac.module"() <{sym_name = "Parameterized", function_type = () -> (), static_params = {target = @Leaf}}> ({
    "ac.return"() : () -> ()
  }) : () -> ()
  "ac.module"() <{sym_name = "StaticRefs", function_type = () -> (), static_params = {target = @Leaf}}> ({
    "ac.instance"() <{definition = @Parameterized, sym_name = "one", stable_id = "one", path = "one", static_args = {target = @Leaf}}> : () -> ()
    "ac.array"() <{definition = @Parameterized, sym_name = "array", stable_id = "array", path = "array", shape = array<i64: 1>, static_args = [{target = @Leaf}]}> : () -> ()
    "ac.instances"() <{sym_name = "many", stable_id = "many", path = "many", definitions = [@Parameterized], names = ["item"], stable_ids = ["item"], paths = ["item"], interface = () -> (), static_args = [{target = @Leaf}]}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()

  "ac.module.extern"() <{sym_name = "Ext", function_type = (i32) -> i32, static_params = {}, implementation = {registry = "cpp", name = "Ext"}}> : () -> ()

  // One reusable definition may be instantiated by multiple parents. Its
  // relative child segment expands independently below each ownership path.
  "ac.module"() <{sym_name = "Reusable", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @Empty, sym_name = "leaf", stable_id = "leaf", path = "leaf", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
  "ac.module"() <{sym_name = "ReuseHarness", function_type = () -> (), static_params = {}}> ({
    "ac.instance"() <{definition = @Reusable, sym_name = "left", stable_id = "left", path = "left", static_args = {}}> : () -> ()
    "ac.instance"() <{definition = @Reusable, sym_name = "right", stable_id = "right", path = "right", static_args = {}}> : () -> ()
    "ac.return"() : () -> ()
  }) : () -> ()
  "ac.module.extern"() <{sym_name = "Empty", function_type = () -> (), static_params = {}, implementation = {registry = "cpp", name = "Empty"}}> : () -> ()

  // Graph-region SSA may use a value before its textual definition and cycle
  // when the instantiated module contributes an explicit state boundary.
  "ac.module"() <{sym_name = "DataCycle", function_type = () -> i32, static_params = {}}> ({
    %a = "ac.instance"(%b) <{definition = @Leaf, sym_name = "left", stable_id = "data-left", path = "left", static_args = {}}> : (i32) -> i32
    %b = "ac.instance"(%a) <{definition = @Leaf, sym_name = "right", stable_id = "data-right", path = "right", static_args = {}}> : (i32) -> i32
    "ac.return"(%a) : (i32) -> ()
  }) : () -> ()
}

// CHECK: ac.system
// CHECK-SAME: workload @Top::@workload
// CHECK-SAME: instrumentation [@Top::@workload::@trace]
// CHECK: ac.module
// CHECK: ac.instance
// CHECK: ac.return
// CHECK: ac.module.extern
