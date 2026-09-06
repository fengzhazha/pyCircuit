"""Two Python branches join both index and value for one persistent list."""

import agentic_circuit as ac


@ac.struct
class Command:
    select_right: bool
    left_index: ac.u2
    right_index: ac.u2
    value: ac.u8


@ac.rule
def update(entries, command):
    if command.select_right:
        entries[command.right_index] = command.value
    else:
        entries[command.left_index] = command.value + 1


@ac.system
def indexed_branch_join(command: Command) -> None:
    entries: list[ac.u8] = [0] * 4
    update(entries, command)
