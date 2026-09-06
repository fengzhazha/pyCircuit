// RUN: %split_file %s %t
// RUN: %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/valid.mlir | %FileCheck %s --check-prefix=VALID
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/fanout.mlir 2>&1 | %FileCheck %s --check-prefix=FANOUT
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/duplicate-owner.mlir 2>&1 | %FileCheck %s --check-prefix=DUPLICATE
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/arbitration.mlir 2>&1 | %FileCheck %s --check-prefix=ARBITRATION
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/unresolved.mlir 2>&1 | %FileCheck %s --check-prefix=UNRESOLVED
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/bad-provider.mlir 2>&1 | %FileCheck %s --check-prefix=PROVIDER
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/bad-process.mlir 2>&1 | %FileCheck %s --check-prefix=PROCESS
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/bad-probe.mlir 2>&1 | %FileCheck %s --check-prefix=PROBE
// RUN: %not %acir_opt --verify-each=false --pass-pipeline='builtin.module(ac-verify-model)' %t/bad-contract.mlir 2>&1 | %FileCheck %s --check-prefix=CONTRACT

//--- valid.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  func.func private @identity(%arg0 : i32) -> i32 {
    return %arg0 : i32
  }
  ac.system @soc root @Top as "root" tick 0 "cycle"
      workload @Top::@workload seed {kind = "fixed", value = 7 : i64}
      instrumentation [] results {id = "default", format = "json"} selected true
  ac.module @Top() parameters {} graph {
    ac.process @workload kind "workload" {
      %c0 = arith.constant 0 : i32
      %v = func.call @identity(%c0) : (i32) -> i32
      ac.yield_sim
    }
    ac.return
  }
}
// VALID: func.func private @identity
// VALID: ac.process @workload
// VALID: func.call @identity

//--- fanout.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.protocol @p {
    ac.role @sender dual @receiver cardinality "exclusive"
    ac.role @receiver dual @sender cardinality "exclusive"
    ac.state @idle initial true terminal false
    ac.state @done initial false terminal true
    ac.event @push from @sender to @receiver payload i32 action "offer"
    ac.transition from @idle to @done on @push transfer true retain false guard {}
  }
  ac.module @Leaf(!ac.flow<i32, @p>) -> !ac.flow<i32, @p> parameters {} graph {
  ^bb0(%arg0 : !ac.flow<i32, @p>):
    ac.return %arg0 : !ac.flow<i32, @p>
  }
  ac.module @Top(!ac.flow<i32, @p>) parameters {} graph {
  ^bb0(%arg0 : !ac.flow<i32, @p>):
    %a = ac.instance @a of @Leaf(%arg0) static {} id "a" path "a" : (!ac.flow<i32, @p>) -> !ac.flow<i32, @p>
    %b = ac.instance @b of @Leaf(%arg0) static {} id "b" path "b" : (!ac.flow<i32, @p>) -> !ac.flow<i32, @p>
    ac.return
  }
}
// FANOUT: flow value has more than one functional use

//--- duplicate-owner.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.module @Top() parameters {} graph {
    ac.stat @same kind "counter"
    ac.process @same kind "control" { ac.yield_sim }
    ac.return
  }
}
// DUPLICATE: duplicate local structural name 'same'

//--- arbitration.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.module @Top() parameters {} graph {
    ac.resource @r capacity 1 issue_width 1 ii 1
        latency {kind = "fixed", ticks = 1 : i64}
        lifecycle {reservation = "propose_commit", release = "balanced", cancellation = "explicit"}
        ownership "contested" classes [] id "r" path "r"
    ac.return
  }
}
// ARBITRATION: shared or contested resource requires one arbitration owner

//--- unresolved.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.system @soc root @Missing as "root" tick 0 "cycle"
      seed {kind = "fixed", value = 0 : i64} instrumentation []
      results {id = "default", format = "json"} selected true
}
// UNRESOLVED: selected root must resolve to a materialized ac.module

//--- bad-provider.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.module.extern @Missing : () -> () parameters {}
      implementation {registry = "cpp", name = "NotRegistered"}
}
// PROVIDER: structural provider 'cpp:NotRegistered' is not registered

//--- bad-process.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.module @Top() parameters {} graph {
    ac.process @p kind "control" { ac.wait_for @missing ac.yield_sim }
    ac.return
  }
}
// PROCESS: unresolved runtime target '@missing'

//--- bad-probe.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.module @Top() parameters {} graph {
    ac.process @p kind "monitor" {
      %v = ac.probe @missing kind "queue" : i32
      ac.yield_sim
    }
    ac.return
  }
}
// PROBE: unresolved runtime target '@missing'

//--- bad-contract.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  ac.module @Top() parameters {} graph {
    %true = arith.constant true
    ac.assert %true, "not static"
    ac.return
  }
}
// CONTRACT: operation is not legal in an ac.module structural Graph region
