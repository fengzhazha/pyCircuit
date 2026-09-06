"""One input selects one of two lexical state updates through plain if/else."""

import agentic_circuit as ac


@ac.struct
class Command:
    select_right: bool
    value: ac.u8


@ac.rule
def route(left, right, command):
    if command.select_right:
        right = command.value
    else:
        left = command.value


@ac.system
def branch_local_state(command: Command) -> None:
    left: ac.u8 = 0
    right: ac.u8 = 0
    route(left, right, command)
