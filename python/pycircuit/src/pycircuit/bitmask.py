"""Bit-mask pattern parsing for ASL-style ``IN {'1xx0'}`` matching (TODO T2).

Pure, dependency-free helpers so both ``Wire`` (``hw.py``) and
``CycleAwareSignal`` (``v6.py``) can expose ``matches`` / ``in_`` / ``not_in_``
without import cycles. A pattern compiles to a ``(mask, value, width)`` triple;
``signal.matches(p)`` then expands to ``(signal & mask) == value``.

Pattern grammar (MSB-first, matching ASL bit literals):

- ``0`` / ``1``            : care bit (must equal 0/1).
- ``x`` / ``X`` / ``-``    : don't-care bit (ignored).
- ``(...)``                : every bit inside the parentheses is don't-care,
                             regardless of the 0/1 written (ASL ``'1(0)x0'``).
- spaces / ``_``           : cosmetic separators, ignored (ASL ``'11 11'``).

Each bit character (including those inside parentheses) contributes one bit of
width; separators and the parentheses markers do not.
"""

from _pycircuit_semantics.bitmask import (
    normalize_patterns,
    parse_bitmask,
    parse_bitmask_checked,
)

__all__ = ("normalize_patterns", "parse_bitmask", "parse_bitmask_checked")
