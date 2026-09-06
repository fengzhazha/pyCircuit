import agentic_circuit as ac


@ac.struct
class Entry:
    index: ac.u1
    value: ac.u7


@ac.struct
class Delta:
    amount: ac.u7


@ac.rule
def install(rob, entry, delta):
    old = rob[entry.index]
    rob[entry.index] = entry.with_fields(value=entry.value + delta.amount)
    return old


@ac.system
def table_multi_input_rule() -> None:
    rob = ac.table[2, Entry](init=0)
    incoming = ac.source(Entry, depth=2)
    deltas = ac.source(Delta, depth=2)
    outgoing = install(rob, incoming, deltas)
    ac.sink(outgoing)
