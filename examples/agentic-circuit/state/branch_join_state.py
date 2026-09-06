"""Two Python branches join to one lexical state proposal."""

import agentic_circuit as ac


@ac.struct
class Command:
    direct: bool
    value: ac.u8


@ac.rule
def update(total, command):
    if command.direct:
        total = command.value
    else:
        total = command.value + 1


@ac.system
def branch_join_state(command: Command) -> None:
    total: ac.u8 = 0
    update(total, command)
