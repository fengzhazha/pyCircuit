// RUN: %acir_opt %s | %FileCheck %s
// RUN: %acir_opt %s | %acir_opt | %FileCheck %s

builtin.module attributes {ac.contract_epoch = "0.5"} {
  "ac.type_scope"() <{sym_name = "types"}> ({
    "ac.transaction"() <{sym_name = "Message", fields = [{name = "tag", type = i16}]}> : () -> ()
    "ac.transaction"() <{sym_name = "Req", fields = [{name = "id", type = i16}, {name = "data", type = i8}]}> : () -> ()
    "ac.transaction"() <{sym_name = "Resp", fields = [{name = "id", type = i16}, {name = "status", type = i1}]}> : () -> ()
  }) : () -> ()
  "ac.protocol"() <{sym_name = "handshake"}> ({
    "ac.role"() <{sym_name = "producer", dual = @consumer, cardinality = "exclusive"}> : () -> ()
    "ac.role"() <{sym_name = "consumer", dual = @producer, cardinality = "exclusive"}> : () -> ()
    "ac.state"() <{sym_name = "idle", initial = true, terminal = false}> : () -> ()
    "ac.state"() <{sym_name = "pending", initial = false, terminal = false}> : () -> ()
    "ac.state"() <{sym_name = "done", initial = false, terminal = true}> : () -> ()
    "ac.event"() <{sym_name = "offer", from = @producer, to = @consumer, payload = i32, action = "offer"}> : () -> ()
    "ac.event"() <{sym_name = "accept", from = @consumer, to = @producer, payload = i32, action = "accept"}> : () -> ()
    "ac.event"() <{sym_name = "cancel", from = @producer, to = @consumer, payload = i32, action = "cancel"}> : () -> ()
    "ac.event"() <{sym_name = "reject", from = @consumer, to = @producer, payload = i32, action = "reject"}> : () -> ()
    "ac.event"() <{sym_name = "retry", from = @producer, to = @consumer, payload = i32, action = "retry"}> : () -> ()
    "ac.transition"() <{source = @idle, target = @pending, event = @offer, retain = true}> ({}) : () -> ()
    "ac.transition"() <{source = @pending, target = @done, event = @accept, transfer = true}> ({}) : () -> ()
    "ac.transition"() <{source = @pending, target = @done, event = @cancel}> ({}) : () -> ()
    "ac.transition"() <{source = @pending, target = @done, event = @reject}> ({}) : () -> ()
    "ac.transition"() <{source = @pending, target = @pending, event = @retry, retain = true}> ({}) : () -> ()
    "ac.guarantee"() <{kind = "backpressure", value = "accept"}> : () -> ()
    "ac.guarantee"() <{kind = "ordering", value = "fifo"}> : () -> ()
    "ac.guarantee"() <{kind = "delivery", value = "exactly_once"}> : () -> ()
    "ac.guarantee"() <{kind = "completion", value = "on_accept"}> : () -> ()
    "ac.guarantee"() <{kind = "stable_pending", value = true}> : () -> ()
    "ac.guarantee"() <{kind = "max_inflight", value = 1 : i64}> : () -> ()
  }) : () -> ()

  "ac.protocol"() <{sym_name = "correlated"}> ({
    "ac.role"() <{sym_name = "requester", dual = @responder, cardinality = "exclusive"}> : () -> ()
    "ac.role"() <{sym_name = "responder", dual = @requester, cardinality = "exclusive"}> : () -> ()
    "ac.state"() <{sym_name = "active", initial = true, terminal = false}> : () -> ()
    "ac.event"() <{sym_name = "request", from = @requester, to = @responder, payload = !ac.transaction<@types::@Req>, action = "offer"}> : () -> ()
    "ac.event"() <{sym_name = "response", from = @responder, to = @requester, payload = !ac.transaction<@types::@Resp>, action = "response"}> : () -> ()
    "ac.transition"() <{source = @active, target = @active, event = @request, transfer = true}> ({}) : () -> ()
    "ac.transition"() <{source = @active, target = @active, event = @response}> ({
      %zero = "arith.constant"() <{value = 0 : i16}> : () -> i16
      %idx = "index.constant"() <{value = 0 : index}> : () -> index
    }) : () -> ()
    "ac.guarantee"() <{kind = "backpressure", value = "credit"}> : () -> ()
    "ac.guarantee"() <{kind = "ordering", value = "per_key"}> : () -> ()
    "ac.guarantee"() <{kind = "delivery", value = "exactly_once"}> : () -> ()
    "ac.guarantee"() <{kind = "completion", value = "on_response"}> : () -> ()
    "ac.guarantee"() <{kind = "max_inflight", value = 4 : i64}> : () -> ()
    "ac.guarantee"() <{kind = "correlation", value = "id"}> : () -> ()
  }) : () -> ()

  "ac.protocol"() <{sym_name = "terminal_completion"}> ({
    "ac.state"() <{sym_name = "start", initial = true, terminal = false}> : () -> ()
    "ac.state"() <{sym_name = "done", initial = false, terminal = true}> : () -> ()
    "ac.event"() <{sym_name = "finish", from = @producer, to = @consumer, payload = i1, action = "notify"}> : () -> ()
    "ac.role"() <{sym_name = "producer", dual = @consumer, cardinality = "exclusive"}> : () -> ()
    "ac.role"() <{sym_name = "consumer", dual = @producer, cardinality = "exclusive"}> : () -> ()
    "ac.transition"() <{source = @start, target = @done, event = @finish}> ({}) : () -> ()
    "ac.guarantee"() <{kind = "completion", value = "on_terminal_phase"}> : () -> ()
  }) : () -> ()

  %flow = "builtin.unrealized_conversion_cast"() : () -> !ac.flow<i32, @handshake>
}

// CHECK: ac.protocol
// CHECK: ac.role
// CHECK: ac.state
// CHECK: ac.event
// CHECK: ac.transition
// CHECK: ac.guarantee
