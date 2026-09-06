"""One state update always commits while an output is conditionally selected."""

import agentic_circuit as ac


@ac.struct
class Event:
    emit: bool
    value: ac.u8


@ac.rule
def filter_event(count, event):
    count = count + 1
    if event.emit:
        return event
    return


@ac.system
def optional_output_state(event: Event) -> Event:
    count: ac.u8 = 0
    filtered = filter_event(count, event)
    return filtered
