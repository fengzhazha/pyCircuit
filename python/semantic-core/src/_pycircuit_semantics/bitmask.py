"""Deterministic ASL-style bit-mask pattern parsing shared by frontends."""

from __future__ import annotations

_CARE = {"0", "1"}
_DONT_CARE = {"x", "X", "-"}
_SEPARATORS = {" ", "\t", "_"}


def parse_bitmask(pattern: str, *, extended: bool = True) -> tuple[int, int, int]:
    """Compile an MSB-first pattern to ``(mask, value, width)``.

    Parenthesized bit characters are don't-care positions. Spaces, tabs, and
    underscores are cosmetic separators and do not contribute to width.
    """

    if not isinstance(pattern, str):
        raise TypeError(f"bit-mask pattern must be a str, got {type(pattern).__name__}")
    if not extended:
        invalid = next(
            (character for character in pattern if character not in {"0", "1", "x"}),
            None,
        )
        if invalid is not None:
            raise ValueError(
                f"invalid character {invalid!r} in basic bit-mask pattern "
                f"{pattern!r}; expected only '0', '1', or 'x'"
            )
        if not pattern:
            raise ValueError(f"bit-mask pattern {pattern!r} has no bits")
        mask = 0
        value = 0
        for character in pattern:
            mask = (mask << 1) | (character != "x")
            value = (value << 1) | (character == "1")
        return mask, value, len(pattern)
    bits: list[tuple[bool, int]] = []
    in_parentheses = False
    for character in pattern:
        if character in _SEPARATORS:
            continue
        if character == "(":
            if in_parentheses:
                raise ValueError(f"nested '(' in bit-mask pattern {pattern!r}")
            in_parentheses = True
            continue
        if character == ")":
            if not in_parentheses:
                raise ValueError(f"unmatched ')' in bit-mask pattern {pattern!r}")
            in_parentheses = False
            continue
        if in_parentheses:
            if character not in _CARE and character not in _DONT_CARE:
                raise ValueError(
                    f"invalid character {character!r} inside parentheses of "
                    f"pattern {pattern!r}"
                )
            bits.append((False, 0))
            continue
        if character in _CARE:
            bits.append((True, int(character)))
        elif character in _DONT_CARE:
            bits.append((False, 0))
        else:
            raise ValueError(
                f"invalid character {character!r} in bit-mask pattern {pattern!r}"
            )
    if in_parentheses:
        raise ValueError(f"unclosed '(' in bit-mask pattern {pattern!r}")
    if not bits:
        raise ValueError(f"bit-mask pattern {pattern!r} has no bits")

    mask = 0
    value = 0
    width = len(bits)
    for index, (is_care, value_bit) in enumerate(bits):
        position = width - index - 1
        if is_care:
            mask |= 1 << position
            value |= value_bit << position
    return mask, value, width


def parse_bitmask_checked(
    pattern: str, *, width: int, extended: bool = True
) -> tuple[int, int]:
    """Compile ``pattern`` and require its logical width to equal ``width``."""

    mask, value, actual_width = parse_bitmask(pattern, extended=extended)
    if actual_width != int(width):
        raise ValueError(
            f"bit-mask pattern {pattern!r} has width {actual_width}, "
            f"expected {int(width)}"
        )
    return mask, value


def normalize_patterns(patterns: tuple[object, ...]) -> list[str]:
    """Normalize non-empty varargs or one iterable of bit-mask patterns."""

    if len(patterns) == 1 and not isinstance(patterns[0], str):
        items = list(patterns[0])  # type: ignore[arg-type]
    else:
        items = list(patterns)
    if not items:
        raise ValueError("in_()/not_in_() requires at least one pattern")
    for pattern in items:
        if not isinstance(pattern, str):
            raise TypeError(
                "bit-mask pattern must be a str, " f"got {type(pattern).__name__}"
            )
    return items  # type: ignore[return-value]
