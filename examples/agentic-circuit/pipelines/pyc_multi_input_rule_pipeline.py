import agentic_circuit as ac


@ac.rule
def add(left, right):
    return left + right


@ac.system
def pyc_multi_input_rule_pipeline() -> None:
    left = ac.source(int, depth=2, latency=1)
    right = ac.source(int, depth=2, latency=1)
    summed = add(left, right)
    ac.sink(summed)
