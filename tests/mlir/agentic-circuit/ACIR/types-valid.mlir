// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt %s | %acir_opt | %FileCheck %s
// RUN: %acir_opt --emit-bytecode -o %t.bc %s
// RUN: %acir_opt %t.bc | %FileCheck %s

// This file covers the SSA-legal ACIR public value/topology types.
builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.protocol"() <{sym_name = "test_protocol"}> ({
    "ac.role"() <{sym_name = "producer", dual = @consumer, cardinality = "exclusive"}> : () -> ()
    "ac.role"() <{sym_name = "consumer", dual = @producer, cardinality = "exclusive"}> : () -> ()
    "ac.state"() <{sym_name = "idle", initial = true, terminal = true}> : () -> ()
    "ac.event"() <{sym_name = "send", from = @producer, to = @consumer, payload = i8, action = "offer"}> : () -> ()
    "ac.transition"() <{source = @idle, target = @idle, event = @send, transfer = true}> ({}) : () -> ()
  }) : () -> ()
  "ac.interface"() <{sym_name = "MemoryPort"}> ({
    "ac.role"() <{sym_name = "initiator", dual = @target, cardinality = "exclusive"}> : () -> ()
    "ac.role"() <{sym_name = "target", dual = @initiator, cardinality = "exclusive"}> : () -> ()
  }) : () -> ()
  "builtin.unrealized_conversion_cast"() : () -> !ac.struct<@types::@Header>
  "builtin.unrealized_conversion_cast"() : () -> !ac.packet<@types::@Request>
  "builtin.unrealized_conversion_cast"() : () -> !ac.transaction<@types::@Dma>
  "builtin.unrealized_conversion_cast"() : () -> !ac.enum<@types::@Opcode>
  "builtin.unrealized_conversion_cast"() : () -> !ac.flow<i8, @test_protocol>
  "builtin.unrealized_conversion_cast"() : () -> !ac.endpoint<@MemoryPort, @target>
  "builtin.unrealized_conversion_cast"() : () -> !ac.resource_ref<@Memory, @reader>
  "builtin.unrealized_conversion_cast"() : () -> !ac.event<!ac.transaction<@types::@Dma>>
  "builtin.unrealized_conversion_cast"() : () -> !ac.resource_token<@Memory>
}

// CHECK: !ac.struct<@types::@Header>
// CHECK: !ac.packet<@types::@Request>
// CHECK: !ac.transaction<@types::@Dma>
// CHECK: !ac.enum<@types::@Opcode>
// CHECK: !ac.flow<i8, @test_protocol>
// CHECK: !ac.endpoint<@MemoryPort, @target>
// CHECK: !ac.resource_ref<@Memory, @reader>
// CHECK: !ac.event<!ac.transaction<@types::@Dma>>
// CHECK: !ac.resource_token<@Memory>
