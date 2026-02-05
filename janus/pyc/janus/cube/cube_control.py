from __future__ import annotations

from pycircuit import Circuit, Wire

from janus.cube.cube_consts import ST_COMPUTE, ST_DONE, ST_DRAIN, ST_IDLE, ST_LOAD_WEIGHTS
from janus.cube.cube_types import CubeState


def build_control(
    m: Circuit,
    *,
    start: Wire,  # Start signal from CPU
    reset_cube: Wire,  # Reset signal from CPU
    state: CubeState,
) -> Wire:
    """
    Build control FSM.

    Returns:
        compute: Compute enable signal (load_weight is derived from state)
    """
    with m.scope("CONTROL"):
        current_state = state.state.out()
        cycle_count = state.cycle_count.out()

        # State transitions
        state_is_idle = current_state == ST_IDLE
        state_is_load = current_state == ST_LOAD_WEIGHTS
        state_is_compute = current_state == ST_COMPUTE
        state_is_drain = current_state == ST_DRAIN
        state_is_done = current_state == ST_DONE

        # Next state logic
        next_state = current_state

        if state_is_idle:
            if start:
                next_state = m.const_wire(ST_LOAD_WEIGHTS, width=3)

        if state_is_load:
            # Load takes 1 cycle
            if cycle_count == 0:
                next_state = m.const_wire(ST_COMPUTE, width=3)

        if state_is_compute:
            # Compute takes 4 cycles (streaming 4 rows)
            if cycle_count == 3:
                next_state = m.const_wire(ST_DRAIN, width=3)

        if state_is_drain:
            # Drain takes 3 cycles (pipeline depth)
            if cycle_count == 2:
                next_state = m.const_wire(ST_DONE, width=3)

        if state_is_done:
            if reset_cube:
                next_state = m.const_wire(ST_IDLE, width=3)

        # Update state
        state.state.set(next_state)

        # Cycle counter
        counter_reset = (
            state_is_idle
            | (state_is_load & (cycle_count == 0))
            | (state_is_compute & (cycle_count == 3))
            | (state_is_drain & (cycle_count == 2))
        )

        next_count = counter_reset.select(m.const_wire(0, width=8), cycle_count + m.const_wire(1, width=8))
        state.cycle_count.set(next_count)

        # Control signals
        load_weight = state_is_load
        compute = state_is_compute | state_is_drain
        done = state_is_done

        # Status flags
        state.done.set(done)
        state.busy.set(~state_is_idle & ~state_is_done)

        return compute
