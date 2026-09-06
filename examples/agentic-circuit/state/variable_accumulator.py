import agentic_circuit as ac


@ac.rule
def accumulate(count, value):
    count = count + value
    return count


@ac.system
def variable_accumulator() -> None:
    count: ac.u8 = 0
    incoming = ac.source(ac.u8, depth=2)
    outgoing = accumulate(count, incoming)
    ac.sink(outgoing)
