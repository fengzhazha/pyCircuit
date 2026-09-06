"""serial-Python to Queue/Var ACIR construction."""

from __future__ import annotations

import ast
import copy
import json
import re
from collections.abc import Mapping
from dataclasses import dataclass

from _pycircuit_semantics import (
    ArrayType,
    BitfieldLayout,
    BitsType,
    BoolType,
    Constant,
    Constraint,
    EnumType,
    StructType,
    TupleType,
    Unknown,
    ValueType,
    constraint_for_type,
    is_exhaustive,
    parse_bitmask_checked,
    prove_within,
    transfer_bits,
)

from ._acpy import AcpyDocument, EntityAllocator, Property, SourceFile
from ._canonical_json import sha256_bytes
from ._diagnostics import SourceSpan
from ._static_eval import (
    MAX_STATIC_EXPANSION,
    StaticEnvironment,
    StaticValue,
    evaluate_static,
)

RULE_LOWERING_PIPELINE = (
    "builtin.module("
    "ac-lower-rules,"
    "canonicalize,cse,"
    "ac-verify-rule-closure,"
    "ac-freeze-topology)"
)


def _render_type(value_type: ValueType) -> str:
    """Render one semantic value type only at the ACIR text boundary."""

    return value_type.mlir()


def _static_constraint(
    node: ast.expr, values: Mapping[str, StaticValue] | None = None
) -> Constraint:
    """Return an exact frontend fact when closed static evaluation succeeds."""

    if isinstance(node, ast.Constant) and type(node.value) in {bool, int, str}:
        return Constant(node.value)
    try:
        value = evaluate_static(node, StaticEnvironment(values or {}))
    except ValueError:
        return Unknown()
    if type(value) in {bool, int, str}:
        return Constant(value)
    return Unknown()


def _constant_integer(
    node: ast.expr, values: Mapping[str, StaticValue] | None = None
) -> int | None:
    fact = _static_constraint(node, values)
    if isinstance(fact, Constant) and type(fact.value) is int:
        return fact.value
    return None


def _proven_integer_in(value: int, lower: int, upper: int) -> bool:
    """Use the shared bounded domain for concrete shape/bound checks."""

    return prove_within(Constant(value), lower, upper)


def _is_epoch_05_bool_compatible(value_type: ValueType) -> bool:
    """Preserve the accepted epoch-0.5 i1 condition boundary.

    Bool and u1 retain distinct descriptor identities; this predicate exists
    only where the current ACIR contract historically accepts either i1 view.
    """

    return isinstance(value_type, BoolType) or (
        isinstance(value_type, BitsType) and value_type.bit_width() == 1
    )


def _types_equal_in_epoch_05(left: ValueType, right: ValueType) -> bool:
    """Compare semantic types at the epoch-0.5 ACIR rendering boundary."""

    return left == right or (
        _is_epoch_05_bool_compatible(left) and _is_epoch_05_bool_compatible(right)
    )


def _epoch_05_integer_width(value_type: ValueType) -> int | None:
    """Return the width of a value accepted by the epoch-0.5 integer boundary."""

    if isinstance(value_type, BitsType):
        return value_type.width
    if isinstance(value_type, BoolType):
        return 1
    return None


class QueueFrontendError(ValueError):
    """A stable rejection from the queue frontend."""


@dataclass(frozen=True, slots=True)
class Payload:
    descriptor: StructType

    @property
    def name(self) -> str:
        return self.descriptor.name

    @property
    def field_descriptors(self) -> tuple[tuple[str, ValueType], ...]:
        return tuple((field.name, field.type) for field in self.descriptor.fields)

    @property
    def fields(self) -> tuple[tuple[str, str], ...]:
        return tuple(
            (name, _render_type(descriptor))
            for name, descriptor in self.field_descriptors
        )

    @property
    def acir_type(self) -> str:
        return _render_type(self.descriptor)


@dataclass(frozen=True, slots=True)
class BitfieldBinding:
    name: str
    layout: BitfieldLayout


@dataclass(frozen=True, slots=True)
class EnumBinding:
    name: str
    descriptor: ValueType


@dataclass(frozen=True, slots=True)
class RuleStateWriteDefinition:
    argument: str
    index: ast.expr | None
    value: ast.expr
    guard: ast.expr | None = None
    guard_negated: bool = False


@dataclass(frozen=True, slots=True)
class RuleStateReadDefinition:
    name: str
    argument: str
    index: ast.expr | None


@dataclass(frozen=True, slots=True)
class RuleLocalDefinition:
    name: str
    value: ast.expr


@dataclass(frozen=True, slots=True)
class RuleFindDefinition:
    name: str
    argument: str
    predicate_argument: str
    predicate: ast.expr
    key_argument: str | None
    key: ast.expr | None


@dataclass(frozen=True, slots=True)
class RuleStateWriteBinding:
    variable: str
    argument: str
    value_type: ValueType
    entries: int
    index: ast.expr | None
    value: ast.expr
    guard: ast.expr | None = None
    guard_negated: bool = False


@dataclass(frozen=True, slots=True)
class RuleStateReadBinding:
    name: str
    variable: str
    argument: str
    value_type: ValueType
    entries: int
    index: ast.expr | None


@dataclass(frozen=True, slots=True)
class RuleLocalBinding:
    name: str
    value: ast.expr


@dataclass(frozen=True, slots=True)
class RuleFindBinding:
    name: str
    variable: str
    argument: str
    value_type: ValueType
    entries: int
    predicate_argument: str
    predicate: ast.expr
    key_argument: str | None
    key: ast.expr | None


@dataclass(frozen=True, slots=True)
class RuleStateOwnerBinding:
    variable: str
    argument: str
    value_type: ValueType
    entries: int


@dataclass(frozen=True, slots=True)
class QueueBinding:
    name: str
    payload: ValueType
    depth: int
    latency: int
    input_name: str | None
    argument: str | None = None
    expression: ast.expr | None = None
    scope: tuple[str, ...] = ()
    order: int = 0
    route_output: bool = False
    feedback_output: bool = False
    merge_output: bool = False
    reorder_output: bool = False
    dependency_output: bool = False
    credit_output: bool = False
    memory_output: bool = False
    table_read_output: bool = False
    barrier_output: bool = False
    select_output: bool = False
    provider: str = "transform"
    rate: int = 1
    rule_name: str | None = None
    rule_source_line: int | None = None
    rule_source_column: int | None = None
    rule_table: str | None = None
    rule_table_index: ast.expr | None = None
    rule_table_value: ast.expr | None = None
    rule_write_fields: tuple[str, ...] = ()
    rule_table_read_name: str | None = None
    rule_table_read_index: ast.expr | None = None
    rule_input_names: tuple[str, ...] = ()
    rule_arguments: tuple[str, ...] = ()
    rule_payloads: tuple[ValueType, ...] = ()
    rule_var: str | None = None
    rule_var_argument: str | None = None
    rule_var_value: ast.expr | None = None
    rule_var_index: ast.expr | None = None
    rule_var_read_name: str | None = None
    rule_var_read_index: ast.expr | None = None
    rule_has_output: bool = True
    rule_guard: ast.expr | None = None
    rule_effect_guard: ast.expr | None = None
    rule_output_guard: ast.expr | None = None
    rule_state_writes: tuple[RuleStateWriteBinding, ...] = ()
    rule_state_reads: tuple[RuleStateReadBinding, ...] = ()
    rule_locals: tuple[RuleLocalBinding, ...] = ()
    rule_finds: tuple[RuleFindBinding, ...] = ()
    rule_state_owners: tuple[RuleStateOwnerBinding, ...] = ()


@dataclass(frozen=True, slots=True)
class ScopeBinding:
    name: str
    path: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class SinkBinding:
    queue: str
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class ObservationBinding:
    queue: str
    name: str
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class ExpectBinding:
    queue: str
    argument: str
    predicate: ast.expr
    message: str
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class RouteBinding:
    input_name: str
    outputs: tuple[str, ...]
    argument: str
    selector: ast.expr
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int
    boolean_selector: bool = False


@dataclass(frozen=True, slots=True)
class ForkBinding:
    input_name: str
    outputs: tuple[str, ...]
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class FeedbackBinding:
    input_name: str
    output_name: str
    argument: str
    condition: ast.expr
    update: ast.expr
    depth: int
    latency: int
    max_iterations: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class MergeBinding:
    inputs: tuple[str, ...]
    output: str
    policy: str
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class ReorderBinding:
    input_name: str
    output_name: str
    argument: str
    key: ast.expr
    capacity: int
    start: int
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class DependencyBinding:
    input_name: str
    output_name: str
    argument: str
    key: ast.expr
    waits_for: ast.expr
    resource: ast.expr
    cost: ast.expr
    capacity: int
    resources: int
    no_dependency: int
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int
    provider: str = "dependency"


@dataclass(frozen=True, slots=True)
class CreditBinding:
    input_name: str
    output_name: str
    argument: str
    cost: ast.expr
    credits: int
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int
    provider: str = "credit"


@dataclass(frozen=True, slots=True)
class BarrierBinding:
    inputs: tuple[str, ...]
    outputs: tuple[str, ...]
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class SelectBinding:
    control: str
    inputs: tuple[str, ...]
    output: str
    argument: str
    selector: ast.expr
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class MemoryInstanceBinding:
    name: str
    data_type: ValueType
    entries: int
    init: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class MemoryRequestBinding:
    instance: str
    input_name: str
    output_name: str
    argument: str
    address: ast.expr
    write: ast.expr
    data: ast.expr
    result_field: str
    depth: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class MemoryBinding:
    input_name: str
    output_name: str
    argument: str
    address: ast.expr
    write: ast.expr
    data: ast.expr
    data_type: ValueType
    entries: int
    init: int
    result_field: str
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class TableBinding:
    name: str
    entry_type: ValueType
    entries: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class VarStateBinding:
    name: str
    value_type: ValueType
    init: int | bool
    scope: tuple[str, ...]
    order: int
    entries: int = 1


@dataclass(frozen=True, slots=True)
class EntryViewBinding:
    name: str
    table: str
    argument: str | None
    address: ast.expr
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class MaskedEntryViewBinding:
    name: str
    table: str
    candidates: str
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class TableReadBinding:
    table: str
    input_name: str | None
    output_name: str
    argument: str | None
    address: ast.expr
    when: ast.expr
    view_alias: str | None
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class TableWriteBinding:
    table: str
    input_name: str | None
    argument: str | None
    address: ast.expr
    enable: ast.expr
    value: ast.expr | None
    patch_fields: tuple[tuple[str, ast.expr], ...]
    write_fields: tuple[str, ...]
    write_mode: str
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class MaskedTableWriteBinding:
    table: str
    candidates: str
    enable: ast.expr
    value: ast.expr | None
    patch_fields: tuple[tuple[str, ast.expr], ...]
    write_fields: tuple[str, ...]
    write_mode: str
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class SlotBinding:
    name: str
    input_name: str
    payload: ValueType
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class SlotReleaseBinding:
    slot: str
    when: ast.expr
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class CandidateSetBinding:
    name: str
    table: str
    argument: str
    predicate: ast.expr
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class SelectionBinding:
    name: str
    table: str
    candidates: str
    policy: str
    argument: str | None
    key: ast.expr | None
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class StaticMemoryArrayBinding:
    name: str
    members: tuple[str, ...]
    data_type: ValueType
    entries: int
    init: int
    latency: int
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class SelectedMemoryBinding:
    name: str
    array: str
    input_name: str
    routed_inputs: tuple[str, ...]
    argument: str
    selector: ast.expr
    depth: int
    latency: int
    scope: tuple[str, ...]
    order: int
    provider: str = "memory"


@dataclass(frozen=True, slots=True)
class StaticQueueCollection:
    kind: str
    members: tuple[tuple[str | int | bool, str | StaticQueueCollection], ...]


@dataclass(frozen=True, slots=True)
class RecursiveQueueHelper:
    queue_parameter: str
    count_parameter: str
    argument: str
    expression: ast.expr
    apply_call: ast.Call


@dataclass(frozen=True, slots=True)
class RuleDefinition:
    name: str
    arguments: tuple[str, ...]
    expression: ast.expr | None
    source_line: int
    source_column: int
    table_argument: str | None = None
    table_index: ast.expr | None = None
    table_value: ast.expr | None = None
    table_read_name: str | None = None
    table_read_index: ast.expr | None = None
    var_argument: str | None = None
    var_value: ast.expr | None = None
    guard: ast.expr | None = None
    effect_guard: ast.expr | None = None
    output_guard: ast.expr | None = None
    state_arguments: tuple[str, ...] = ()
    state_writes: tuple[RuleStateWriteDefinition, ...] = ()
    state_reads: tuple[RuleStateReadDefinition, ...] = ()
    locals: tuple[RuleLocalDefinition, ...] = ()
    finds: tuple[RuleFindDefinition, ...] = ()


@dataclass(frozen=True, slots=True)
class CollectionBinding:
    name: str
    value: StaticQueueCollection
    scope: tuple[str, ...]
    order: int


@dataclass(frozen=True, slots=True)
class QueueProgram:
    system: str
    payloads: tuple[Payload, ...]
    enums: tuple[EnumBinding, ...]
    bitfields: tuple[BitfieldBinding, ...]
    queues: tuple[QueueBinding, ...]
    effect_rules: tuple[QueueBinding, ...]
    scopes: tuple[ScopeBinding, ...]
    routes: tuple[RouteBinding, ...]
    forks: tuple[ForkBinding, ...]
    feedbacks: tuple[FeedbackBinding, ...]
    merges: tuple[MergeBinding, ...]
    reorders: tuple[ReorderBinding, ...]
    dependencies: tuple[DependencyBinding, ...]
    credits: tuple[CreditBinding, ...]
    barriers: tuple[BarrierBinding, ...]
    selects: tuple[SelectBinding, ...]
    memory_instances: tuple[MemoryInstanceBinding, ...]
    memory_requests: tuple[MemoryRequestBinding, ...]
    memories: tuple[MemoryBinding, ...]
    variables: tuple[VarStateBinding, ...]
    tables: tuple[TableBinding, ...]
    table_reads: tuple[TableReadBinding, ...]
    table_writes: tuple[TableWriteBinding, ...]
    masked_table_writes: tuple[MaskedTableWriteBinding, ...]
    slots: tuple[SlotBinding, ...]
    slot_releases: tuple[SlotReleaseBinding, ...]
    candidates: tuple[CandidateSetBinding, ...]
    selections: tuple[SelectionBinding, ...]
    collections: tuple[CollectionBinding, ...]
    observations: tuple[ObservationBinding, ...]
    expectations: tuple[ExpectBinding, ...]
    sinks: tuple[SinkBinding, ...]
    specialization_fingerprint: str | None = None


@dataclass(frozen=True, slots=True)
class _ModuleRenderSpec:
    name: str
    inputs: tuple[tuple[str, ValueType], ...]
    outputs: tuple[tuple[str, ValueType], ...]


def _decorator_name(node: ast.expr) -> str:
    if isinstance(node, ast.Call):
        return _decorator_name(node.func)
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        prefix = _decorator_name(node.value)
        return f"{prefix}.{node.attr}" if prefix else node.attr
    return ""


def _scalar_type_descriptor(node: ast.expr) -> ValueType:
    from _pycircuit_semantics import BitsType, BoolType

    if (
        isinstance(node, ast.Subscript)
        and _decorator_name(node.value).rsplit(".", 1)[-1] == "bits"
    ):
        width_node = node.slice
        width = _constant_integer(width_node)
        if width is None:
            raise QueueFrontendError(
                "ACPY-TYPE-003: bits width must be a static integer"
            )
        if not _proven_integer_in(width, 1, 64):
            raise QueueFrontendError("ACPY-TYPE-003: bits width must be in [1, 64]")
        return BitsType(width)
    name = _decorator_name(node).rsplit(".", 1)[-1]
    if name == "int":
        return BitsType(64)
    if name == "bool":
        return BoolType()
    unsigned = re.fullmatch(r"u([0-9]+)", name)
    if unsigned is not None:
        width = int(unsigned.group(1))
        if 1 <= width <= 64:
            return BitsType(width)
        raise QueueFrontendError("ACPY-QUEUE-002: bit width must be in [1, 64]")
    widths = {
        "s8": 8,
        "s16": 16,
        "s32": 32,
        "s64": 64,
    }
    if name in widths:
        return BitsType(widths[name])
    raise QueueFrontendError("ACPY-QUEUE-002: unsupported field type")


def _enums(tree: ast.Module) -> tuple[EnumBinding, ...]:
    from _pycircuit_semantics import EnumType

    result: list[EnumBinding] = []
    names: set[str] = set()
    for node in tree.body:
        if not isinstance(node, ast.ClassDef) or not any(
            _decorator_name(base).rsplit(".", 1)[-1] == "Enum" for base in node.bases
        ):
            continue
        if node.name in names:
            raise QueueFrontendError(
                f"ACPY-TYPE-005: enum {node.name!r} is defined more than once"
            )
        enumerants: list[str] = []
        values: list[int] = []
        for statement in node.body:
            if (
                isinstance(statement, ast.Expr)
                and isinstance(statement.value, ast.Constant)
                and type(statement.value.value) is str
            ):
                continue
            if (
                not isinstance(statement, ast.Assign)
                or len(statement.targets) != 1
                or not isinstance(statement.targets[0], ast.Name)
                or not isinstance(statement.value, ast.Constant)
                or type(statement.value.value) is not int
            ):
                raise QueueFrontendError(
                    "ACPY-TYPE-005: enum body requires integer member assignments"
                )
            enumerants.append(statement.targets[0].id)
            values.append(statement.value.value)
        if values != list(range(len(values))):
            raise QueueFrontendError(
                "ACPY-TYPE-005: enum values must be contiguous from zero in declaration order"
            )
        descriptor = EnumType(node.name, tuple(enumerants))
        if not is_exhaustive(constraint_for_type(descriptor), set(enumerants)):
            raise QueueFrontendError(
                "ACPY-TYPE-005: enum declaration does not cover its finite domain"
            )
        names.add(node.name)
        result.append(EnumBinding(node.name, descriptor))
    return tuple(result)


def _payloads(
    tree: ast.Module, enums: tuple[EnumBinding, ...] = ()
) -> tuple[Payload, ...]:
    from _pycircuit_semantics import ArrayType, StructType, TupleType, ValueField

    declarations = [
        node
        for node in tree.body
        if isinstance(node, ast.ClassDef)
        and any(
            _decorator_name(item).rsplit(".", 1)[-1] == "struct"
            for item in node.decorator_list
        )
    ]
    by_name = {node.name: node for node in declarations}
    if len(by_name) != len(declarations):
        raise QueueFrontendError("ACPY-TYPE-004: struct names must be unique")
    resolved: dict[str, StructType] = {}
    active: list[str] = []
    enum_types = {binding.name: binding.descriptor for binding in enums}

    def annotation_type(node: ast.expr) -> ValueType:
        try:
            return _scalar_type_descriptor(node)
        except QueueFrontendError as scalar_error:
            name = _decorator_name(
                node.value if isinstance(node, ast.Subscript) else node
            ).rsplit(".", 1)[-1]
            if isinstance(node, ast.Subscript) and name in {"tuple", "Tuple"}:
                elements = (
                    tuple(node.slice.elts)
                    if isinstance(node.slice, ast.Tuple)
                    else (node.slice,)
                )
                return TupleType(
                    tuple(annotation_type(element) for element in elements)
                )
            if isinstance(node, ast.Subscript) and name == "array":
                if not isinstance(node.slice, ast.Tuple) or len(node.slice.elts) != 2:
                    raise QueueFrontendError(
                        "ACPY-TYPE-006: value array requires static [length, element]"
                    ) from scalar_error
                length = _constant_integer(node.slice.elts[0])
                if length is None:
                    raise QueueFrontendError(
                        "ACPY-TYPE-006: value array requires static [length, element]"
                    ) from scalar_error
                if not _proven_integer_in(length, 1, (1 << 63) - 1):
                    raise QueueFrontendError(
                        "ACPY-TYPE-006: value array length must be positive"
                    ) from scalar_error
                return ArrayType(
                    length,
                    annotation_type(node.slice.elts[1]),
                )
            if name in enum_types:
                return enum_types[name]
            if name in by_name:
                return resolve(name)
            raise scalar_error

    def resolve(name: str) -> StructType:
        cached = resolved.get(name)
        if cached is not None:
            return cached
        if name in active:
            cycle = " -> ".join((*active[active.index(name) :], name))
            raise QueueFrontendError(
                f"ACPY-TYPE-004: recursive struct cycle is unsupported: {cycle}"
            )
        active.append(name)
        node = by_name[name]
        fields: list[ValueField] = []
        for statement in node.body:
            if not isinstance(statement, ast.AnnAssign) or not isinstance(
                statement.target, ast.Name
            ):
                raise QueueFrontendError(
                    "ACPY-QUEUE-002: struct body requires annotated fields"
                )
            field_type = annotation_type(statement.annotation)
            if isinstance(field_type, (TupleType, ArrayType)) and (
                field_type.bit_width() > 64
            ):
                raise QueueFrontendError(
                    "ACPY-TYPE-006: aggregate field width must be in [1, 64]"
                )
            fields.append(ValueField(statement.target.id, field_type))
        if not fields or len({field.name for field in fields}) != len(fields):
            raise QueueFrontendError(
                "ACPY-QUEUE-002: struct requires unique compile-time fields"
            )
        descriptor = StructType(node.name, tuple(fields))
        active.pop()
        resolved[name] = descriptor
        return descriptor

    return tuple(Payload(resolve(node.name)) for node in declarations)


def _bitfields(tree: ast.Module) -> tuple[BitfieldBinding, ...]:
    result: list[BitfieldBinding] = []
    names: set[str] = set()
    for statement in tree.body:
        value: ast.expr | None = None
        target: ast.expr | None = None
        if isinstance(statement, ast.Assign) and len(statement.targets) == 1:
            target = statement.targets[0]
            value = statement.value
        elif isinstance(statement, ast.AnnAssign):
            target = statement.target
            value = statement.value
        if (
            not isinstance(target, ast.Name)
            or not isinstance(value, ast.Call)
            or _decorator_name(value.func).rsplit(".", 1)[-1] != "BitfieldSpec"
        ):
            continue
        if target.id in names:
            raise QueueFrontendError(
                f"ACPY-BITFIELD-001: BitfieldSpec {target.id!r} is duplicated"
            )
        if any(keyword.arg is None for keyword in value.keywords):
            raise QueueFrontendError(
                "ACPY-BITFIELD-001: BitfieldSpec does not accept keyword unpacking"
            )
        keyword_values = {keyword.arg: keyword.value for keyword in value.keywords}
        if len(keyword_values) != len(value.keywords) or set(keyword_values) - {
            "width",
            "fields",
        }:
            raise QueueFrontendError(
                "ACPY-BITFIELD-001: BitfieldSpec accepts only width and fields"
            )
        if len(value.args) > 2:
            raise QueueFrontendError(
                "ACPY-BITFIELD-001: BitfieldSpec requires width and fields"
            )
        width_node = value.args[0] if value.args else keyword_values.get("width")
        fields_node = (
            value.args[1] if len(value.args) == 2 else keyword_values.get("fields")
        )
        if (
            width_node is None
            or fields_node is None
            or (value.args and "width" in keyword_values)
            or (len(value.args) == 2 and "fields" in keyword_values)
        ):
            raise QueueFrontendError(
                "ACPY-BITFIELD-001: BitfieldSpec requires width and fields once"
            )
        try:
            width = ast.literal_eval(width_node)
            fields = ast.literal_eval(fields_node)
        except (ValueError, TypeError, SyntaxError) as exc:
            raise QueueFrontendError(
                "ACPY-BITFIELD-001: BitfieldSpec width and fields must be static literals"
            ) from exc
        if not isinstance(fields, Mapping):
            raise QueueFrontendError(
                "ACPY-BITFIELD-001: BitfieldSpec fields must be a static mapping"
            )
        from _pycircuit_semantics import BitfieldLayout, BitfieldLayoutError

        try:
            layout = BitfieldLayout(width, fields)
        except BitfieldLayoutError as exc:
            raise QueueFrontendError(f"ACPY-BITFIELD-001: {exc}") from exc
        if layout.width > 64:
            raise QueueFrontendError(
                "ACPY-BITFIELD-001: BitfieldSpec width must be in [1, 64]"
            )
        names.add(target.id)
        result.append(BitfieldBinding(target.id, layout))
    return tuple(result)


def _render_bitfield(binding: BitfieldBinding, indent: str) -> str:
    fields = ", ".join(
        f"{{lsb = {lsb} : i64, msb = {msb} : i64, name = {json.dumps(name)}}}"
        for name, (msb, lsb) in binding.layout.fields.items()
    )
    return (
        f"{indent}ac.bitfield @{binding.name} width {binding.layout.width} "
        f"fingerprint {json.dumps(binding.layout.fingerprint)} fields [{fields}]"
    )


def _align(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def _abi_layout(descriptor: ValueType) -> tuple[int, int]:
    from _pycircuit_semantics import ArrayType, StructType, TupleType

    if isinstance(descriptor, StructType):
        members = tuple(field.type for field in descriptor.fields)
    elif isinstance(descriptor, TupleType):
        members = descriptor.elements
    elif isinstance(descriptor, ArrayType):
        element_size, element_alignment = _abi_layout(descriptor.element)
        stride = _align(element_size, element_alignment)
        return stride * descriptor.length, element_alignment
    else:
        size = max(1, (descriptor.bit_width() + 7) // 8)
        return size, size

    offset = 0
    alignment = 1
    for member in members:
        member_size, member_alignment = _abi_layout(member)
        offset = _align(offset, member_alignment) + member_size
        alignment = max(alignment, member_alignment)
    return _align(offset, alignment), alignment


def _payload_layout_entry(payload: Payload) -> str:
    size, alignment = _abi_layout(payload.descriptor)
    return (
        f"{payload.acir_type} = "
        f'{{abi_alignment = {alignment} : i64, endianness = "little", '
        f"preferred_alignment = {alignment} : i64, size = {size} : i64}}"
    )


def _enum_layout_entry(binding: EnumBinding) -> str:
    size, alignment = _abi_layout(binding.descriptor)
    return (
        f"{_render_type(binding.descriptor)} = "
        f'{{abi_alignment = {alignment} : i64, endianness = "little", '
        f"preferred_alignment = {alignment} : i64, size = {size} : i64}}"
    )


def _render_enum(binding: EnumBinding, indent: str) -> str:
    enumerants = json.dumps(list(binding.descriptor.enumerants))
    return f"{indent}ac.enum @{binding.name} enumerants {enumerants}"


def _static_int_value(node: ast.expr, values: Mapping[str, StaticValue]) -> int | None:
    return _constant_integer(node, values)


def _positive_int_value(
    call: ast.Call,
    name: str,
    default: int,
    static_values: Mapping[str, StaticValue] | None = None,
) -> int:
    matches = [keyword for keyword in call.keywords if keyword.arg == name]
    if len(matches) > 1:
        raise QueueFrontendError(f"ACPY-QUEUE-001: repeated {name!r}")
    if not matches:
        return default
    value = _static_int_value(matches[0].value, static_values or {})
    if value is None:
        raise QueueFrontendError(
            f"ACPY-QUEUE-001: {name} must be a compile-time integer"
        )
    if value <= 0:
        raise QueueFrontendError(f"ACPY-QUEUE-001: {name} must be positive")
    return value


def _nonnegative_int_value(
    call: ast.Call,
    name: str,
    default: int,
    static_values: Mapping[str, StaticValue] | None = None,
) -> int:
    matches = [keyword for keyword in call.keywords if keyword.arg == name]
    if len(matches) > 1:
        raise QueueFrontendError(f"ACPY-QUEUE-001: repeated {name!r}")
    if not matches:
        return default
    value = _static_int_value(matches[0].value, static_values or {})
    if value is None:
        raise QueueFrontendError(
            f"ACPY-QUEUE-001: {name} must be a compile-time integer"
        )
    if value < 0:
        raise QueueFrontendError(f"ACPY-QUEUE-001: {name} must be non-negative")
    return value


def _payload(node: ast.expr, payloads: dict[str, Payload]) -> ValueType:
    try:
        return _scalar_type_descriptor(node)
    except QueueFrontendError:
        pass
    if isinstance(node, ast.Name) and node.id in payloads:
        return payloads[node.id].descriptor
    raise QueueFrontendError(
        "ACPY-QUEUE-002: source payload must be a compile-time supported type"
    )


def _lambda_value(node: ast.expr) -> tuple[str, ast.expr]:
    if not isinstance(node, ast.Lambda) or len(node.args.args) != 1:
        raise QueueFrontendError("ACPY-QUEUE-003: apply requires a one-argument lambda")
    return node.args.args[0].arg, node.body


def _constantize_expression(
    node: ast.expr,
    argument: str,
    values: Mapping[str, StaticValue],
) -> ast.expr:
    class Constantizer(ast.NodeTransformer):
        def _constant(self, candidate: ast.expr) -> ast.expr | None:
            try:
                value = evaluate_static(candidate, StaticEnvironment(values))
            except ValueError:
                return None
            if value is None or type(value) in {bool, int, float, str}:
                return ast.copy_location(ast.Constant(value=value), candidate)
            return None

        def visit_Name(self, candidate: ast.Name) -> ast.expr:
            if candidate.id == argument:
                return candidate
            return self._constant(candidate) or candidate

        def visit_Attribute(self, candidate: ast.Attribute) -> ast.expr:
            return self._constant(candidate) or self.generic_visit(candidate)

    result = Constantizer().visit(copy.deepcopy(node))
    assert isinstance(result, ast.expr)
    return ast.fix_missing_locations(result)


def _is_none_return(statement: ast.stmt) -> bool:
    return isinstance(statement, ast.Return) and (
        statement.value is None
        or (isinstance(statement.value, ast.Constant) and statement.value.value is None)
    )


def _extract_conditional_effect_guard(
    body: list[ast.stmt],
    parameter_names: tuple[str, ...],
    has_value_return: bool,
) -> tuple[list[ast.stmt], ast.expr | None]:
    early_returns = [
        (index, statement)
        for index, statement in enumerate(body)
        if isinstance(statement, ast.If)
        and not statement.orelse
        and len(statement.body) == 1
        and _is_none_return(statement.body[0])
    ]
    if not early_returns:
        return body, None
    if has_value_return:
        raise QueueFrontendError(
            "ACPY-RULE-010: conditional-effect early return is currently outputless"
        )
    indices = [index for index, _ in early_returns]
    if indices != list(range(indices[0], indices[-1] + 1)):
        raise QueueFrontendError(
            "ACPY-RULE-010: early returns must form one contiguous serial guard chain"
        )
    for statement in body[: indices[0]]:
        if not isinstance(statement, ast.Assign) or len(statement.targets) != 1:
            continue
        target = statement.targets[0]
        if isinstance(target, ast.Subscript) or (
            isinstance(target, ast.Name) and target.id in parameter_names
        ):
            raise QueueFrontendError(
                "ACPY-RULE-010: early-return guards must precede state effects"
            )
    conditions = [
        ast.UnaryOp(op=ast.Not(), operand=copy.deepcopy(statement.test))
        for _, statement in early_returns
        if isinstance(statement, ast.If)
    ]
    guard = (
        conditions[0]
        if len(conditions) == 1
        else ast.BoolOp(op=ast.And(), values=conditions)
    )
    for index in reversed(indices):
        body.pop(index)
    return body, ast.fix_missing_locations(guard)


def parse_queue_program(
    text: str,
    system: str,
    static_arguments: Mapping[str, StaticValue] | None = None,
    specialization_fingerprint: str | None = None,
    *,
    entry_kind: str = "system",
) -> QueueProgram:
    tree = ast.parse(text, filename="<queue-model>", type_comments=True)
    for node in tree.body:
        decorators = getattr(node, "decorator_list", ())
        if any(
            _decorator_name(decorator).rsplit(".", 1)[-1]
            in {"opcode", "provider", "backend"}
            for decorator in decorators
        ):
            raise QueueFrontendError(
                "ACPY-QUEUE-010: user opcode or backend providers are forbidden"
            )
    enums = _enums(tree)
    payloads = _payloads(tree, enums)
    payload_map = {item.name: item for item in payloads}
    bitfields = _bitfields(tree)
    bitfield_map = {binding.name: binding.layout for binding in bitfields}
    rule_definitions: dict[str, RuleDefinition] = {}
    for node in tree.body:
        if not isinstance(node, ast.FunctionDef) or not any(
            _decorator_name(decorator).rsplit(".", 1)[-1] == "rule"
            for decorator in node.decorator_list
        ):
            continue
        if node.name in rule_definitions:
            raise QueueFrontendError(
                f"ACPY-RULE-001: rule {node.name!r} is defined more than once"
            )
        if any(isinstance(decorator, ast.Call) for decorator in node.decorator_list):
            raise QueueFrontendError(
                "ACPY-RULE-001: rule decorators do not accept options"
            )
        if (
            not node.args.args
            or node.args.posonlyargs
            or node.args.kwonlyargs
            or node.args.vararg is not None
            or node.args.kwarg is not None
            or node.args.defaults
            or node.args.kw_defaults
        ):
            raise QueueFrontendError(
                "ACPY-RULE-001: rules require one or more positional parameters"
            )
        body = list(node.body)
        if (
            body
            and isinstance(body[0], ast.Expr)
            and isinstance(body[0].value, ast.Constant)
            and isinstance(body[0].value.value, str)
        ):
            body.pop(0)
        if (
            len(body) == 1
            and isinstance(body[0], ast.Return)
            and body[0].value is not None
        ):
            rule_definitions[node.name] = RuleDefinition(
                node.name,
                tuple(argument.arg for argument in node.args.args),
                copy.deepcopy(body[0].value),
                node.lineno,
                node.col_offset + 1,
            )
            continue

        parameter_names = tuple(argument.arg for argument in node.args.args)
        multi_body = list(body)
        multi_guard: ast.expr | None = None
        multi_effect_guard: ast.expr | None = None
        multi_output_guard: ast.expr | None = None
        multi_return: ast.expr | None = None
        if multi_body and isinstance(multi_body[-1], ast.Return):
            returned = multi_body.pop().value
            if not (
                returned is None
                or (isinstance(returned, ast.Constant) and returned.value is None)
            ):
                multi_return = copy.deepcopy(returned)
        multi_body, multi_effect_guard = _extract_conditional_effect_guard(
            multi_body, parameter_names, multi_return is not None
        )
        if (
            multi_body
            and isinstance(multi_body[-1], ast.If)
            and not multi_body[-1].orelse
            and len(multi_body[-1].body) == 1
            and isinstance(multi_body[-1].body[0], ast.Return)
            and multi_body[-1].body[0].value is not None
        ):
            optional_output = multi_body.pop()
            assert isinstance(optional_output, ast.If)
            returned = optional_output.body[0]
            assert isinstance(returned, ast.Return)
            multi_return = copy.deepcopy(returned.value)
            multi_output_guard = copy.deepcopy(optional_output.test)
        if (
            multi_body
            and isinstance(multi_body[-1], ast.If)
            and not multi_body[-1].orelse
        ):
            if multi_effect_guard is not None:
                raise QueueFrontendError(
                    "ACPY-RULE-010: conditional effects cannot also use a "
                    "blocking rule guard"
                )
            guarded = multi_body.pop()
            if guarded.orelse or not guarded.body:
                raise QueueFrontendError(
                    "ACPY-RULE-007: guarded state rule requires one if body "
                    "without else"
                )
            guarded_body = list(guarded.body)
            if guarded_body and isinstance(guarded_body[-1], ast.Return):
                returned = guarded_body.pop().value
                if not (
                    returned is None
                    or (isinstance(returned, ast.Constant) and returned.value is None)
                ):
                    multi_return = copy.deepcopy(returned)
            multi_guard = copy.deepcopy(guarded.test)
            multi_body.extend(guarded_body)
        guarded_statements: list[tuple[ast.stmt, ast.expr | None, bool]] = []
        has_branch_effects = False
        for statement in multi_body:
            if not isinstance(statement, ast.If) or not statement.orelse:
                guarded_statements.append((statement, None, False))
                continue
            if has_branch_effects:
                raise QueueFrontendError(
                    "ACPY-RULE-011: branch-local effects permit one if/else"
                )
            if multi_guard is not None or multi_effect_guard is not None:
                raise QueueFrontendError(
                    "ACPY-RULE-011: branch-local effects cannot combine with "
                    "blocking or early-return guards"
                )
            if multi_return is not None:
                raise QueueFrontendError(
                    "ACPY-RULE-011: branch-local effects are currently outputless"
                )
            if (
                not statement.body
                or not statement.orelse
                or any(
                    not isinstance(candidate, ast.Assign)
                    for candidate in (*statement.body, *statement.orelse)
                )
            ):
                raise QueueFrontendError(
                    "ACPY-RULE-011: each if/else branch requires state assignments"
                )
            has_branch_effects = True
            for candidate in statement.body:
                guarded_statements.append(
                    (candidate, copy.deepcopy(statement.test), False)
                )
            for candidate in statement.orelse:
                guarded_statements.append(
                    (candidate, copy.deepcopy(statement.test), True)
                )
        state_reads: list[RuleStateReadDefinition] = []
        state_writes: list[RuleStateWriteDefinition] = []
        rule_locals: list[RuleLocalDefinition] = []
        rule_finds: list[RuleFindDefinition] = []
        local_names: set[str] = set()
        valid_multi_state = bool(guarded_statements)
        for statement, branch_guard, branch_negated in guarded_statements:
            if not isinstance(statement, ast.Assign) or len(statement.targets) != 1:
                valid_multi_state = False
                break
            target = statement.targets[0]
            if (
                isinstance(target, ast.Name)
                and isinstance(statement.value, ast.Call)
                and _decorator_name(statement.value.func).rsplit(".", 1)[-1] == "find"
            ):
                call = statement.value
                if (
                    len(call.args) != 1
                    or not isinstance(call.args[0], ast.Name)
                    or call.args[0].id not in parameter_names
                    or any(
                        keyword.arg not in {"where", "key"} for keyword in call.keywords
                    )
                ):
                    raise QueueFrontendError(
                        "ACPY-RULE-009: find requires one persistent list and "
                        "where/key lambdas"
                    )
                where = [
                    keyword.value for keyword in call.keywords if keyword.arg == "where"
                ]
                keys = [
                    keyword.value for keyword in call.keywords if keyword.arg == "key"
                ]
                if len(where) != 1 or len(keys) > 1:
                    raise QueueFrontendError(
                        "ACPY-RULE-009: find requires one where and at most one key"
                    )
                predicate_argument, predicate = _lambda_value(where[0])
                key_argument: str | None = None
                key: ast.expr | None = None
                if keys:
                    key_argument, key = _lambda_value(keys[0])
                if target.id in parameter_names or target.id in local_names:
                    raise QueueFrontendError(
                        "ACPY-RULE-009: find result requires a fresh local name"
                    )
                local_names.add(target.id)
                rule_finds.append(
                    RuleFindDefinition(
                        target.id,
                        call.args[0].id,
                        predicate_argument,
                        copy.deepcopy(predicate),
                        key_argument,
                        copy.deepcopy(key),
                    )
                )
                continue
            if isinstance(target, ast.Name) and target.id in parameter_names:
                state_writes.append(
                    RuleStateWriteDefinition(
                        target.id,
                        None,
                        copy.deepcopy(statement.value),
                        copy.deepcopy(branch_guard),
                        branch_negated,
                    )
                )
            elif (
                isinstance(target, ast.Subscript)
                and isinstance(target.value, ast.Name)
                and target.value.id in parameter_names
            ):
                state_writes.append(
                    RuleStateWriteDefinition(
                        target.value.id,
                        copy.deepcopy(target.slice),
                        copy.deepcopy(statement.value),
                        copy.deepcopy(branch_guard),
                        branch_negated,
                    )
                )
            elif isinstance(target, ast.Name):
                if branch_guard is not None:
                    raise QueueFrontendError(
                        "ACPY-RULE-011: branch bodies may only assign persistent state"
                    )
                if target.id in local_names:
                    raise QueueFrontendError(
                        "ACPY-RULE-009: rule local names must be unique"
                    )
                local_names.add(target.id)
                if isinstance(statement.value, ast.Subscript):
                    source = statement.value
                    if (
                        not isinstance(source.value, ast.Name)
                        or source.value.id not in parameter_names
                    ):
                        valid_multi_state = False
                        break
                    state_reads.append(
                        RuleStateReadDefinition(
                            target.id,
                            source.value.id,
                            copy.deepcopy(source.slice),
                        )
                    )
                else:
                    rule_locals.append(
                        RuleLocalDefinition(target.id, copy.deepcopy(statement.value))
                    )
            else:
                valid_multi_state = False
                break
        state_names = {
            *(write.argument for write in state_writes),
            *(read.argument for read in state_reads),
            *(find.argument for find in rule_finds),
        }
        if has_branch_effects:
            writes_by_owner: dict[str, list[RuleStateWriteDefinition]] = {}
            for write in state_writes:
                writes_by_owner.setdefault(write.argument, []).append(write)
            for owner_writes in writes_by_owner.values():
                if len(owner_writes) == 1:
                    continue
                if (
                    len(owner_writes) != 2
                    or (owner_writes[0].index is None)
                    != (owner_writes[1].index is None)
                    or any(write.guard is None for write in owner_writes)
                    or {write.guard_negated for write in owner_writes} != {False, True}
                    or ast.dump(owner_writes[0].guard, include_attributes=False)
                    != ast.dump(owner_writes[1].guard, include_attributes=False)
                ):
                    raise QueueFrontendError(
                        "ACPY-RULE-011: same-owner branches require one matching "
                        "scalar/list assignment per complementary arm"
                    )
        if has_branch_effects:
            written_owners = {write.argument for write in state_writes}
            for write in state_writes:
                expressions = [write.value]
                if write.index is not None:
                    expressions.append(write.index)
                referenced = {
                    candidate.id
                    for expression in expressions
                    for candidate in ast.walk(expression)
                    if isinstance(candidate, ast.Name)
                }
                if (written_owners - {write.argument}) & referenced:
                    raise QueueFrontendError(
                        "ACPY-RULE-011: branch-local values cannot depend on "
                        "another branch-written owner"
                    )
        for find in rule_finds:
            for expression in (find.predicate, find.key):
                if expression is None:
                    continue
                for candidate in ast.walk(expression):
                    if (
                        isinstance(candidate, ast.Subscript)
                        and isinstance(candidate.value, ast.Name)
                        and candidate.value.id in parameter_names
                    ):
                        state_names.add(candidate.value.id)
        ordered_state: tuple[str, ...] = ()
        if state_names:
            last_state = max(parameter_names.index(name) for name in state_names)
            ordered_state = parameter_names[: last_state + 1]
        if (
            valid_multi_state
            and state_writes
            and (
                len(ordered_state) >= 2
                or bool(rule_finds)
                or has_branch_effects
                or multi_output_guard is not None
            )
        ):
            if parameter_names[: len(ordered_state)] != ordered_state:
                raise QueueFrontendError(
                    "ACPY-RULE-008: persistent rule parameters must precede "
                    "payload parameters"
                )
            payload_parameters = parameter_names[len(ordered_state) :]
            if has_branch_effects and len(payload_parameters) != 1:
                raise QueueFrontendError(
                    "ACPY-RULE-011: branch-local effects require exactly one "
                    "payload parameter"
                )
            if multi_output_guard is not None and len(payload_parameters) != 1:
                raise QueueFrontendError(
                    "ACPY-RULE-012: optional output requires exactly one "
                    "payload parameter"
                )
            if multi_effect_guard is not None and len(payload_parameters) != 1:
                raise QueueFrontendError(
                    "ACPY-RULE-010: conditional-effect early return requires "
                    "exactly one payload parameter"
                )
            rule_definitions[node.name] = RuleDefinition(
                node.name,
                payload_parameters,
                multi_return,
                node.lineno,
                node.col_offset + 1,
                guard=multi_guard,
                effect_guard=multi_effect_guard,
                output_guard=multi_output_guard,
                state_arguments=ordered_state,
                state_writes=tuple(state_writes),
                state_reads=tuple(state_reads),
                locals=tuple(rule_locals),
                finds=tuple(rule_finds),
            )
            continue

        if (
            len(node.args.args) >= 2
            and len(body) == 2
            and isinstance(body[0], ast.Assign)
            and len(body[0].targets) == 1
            and isinstance(body[0].targets[0], ast.Name)
            and body[0].targets[0].id == node.args.args[0].arg
            and isinstance(body[1], ast.Return)
            and body[1].value is not None
        ):
            rule_definitions[node.name] = RuleDefinition(
                node.name,
                tuple(argument.arg for argument in node.args.args[1:]),
                copy.deepcopy(body[1].value),
                node.lineno,
                node.col_offset + 1,
                var_argument=node.args.args[0].arg,
                var_value=copy.deepcopy(body[0].value),
            )
            continue

        table_argument = node.args.args[0].arg
        payload_arguments = tuple(argument.arg for argument in node.args.args[1:])
        payload_argument = payload_arguments[0] if payload_arguments else None
        return_expression: ast.expr | None = None
        if body and isinstance(body[-1], ast.Return):
            returned = body[-1].value
            if not (isinstance(returned, ast.Constant) and returned.value is None):
                return_expression = copy.deepcopy(returned)
            body = body[:-1]
        effect_guard_expression: ast.expr | None = None
        body, effect_guard_expression = _extract_conditional_effect_guard(
            body,
            tuple(argument.arg for argument in node.args.args),
            return_expression is not None,
        )
        guard_expression: ast.expr | None = None
        if body and isinstance(body[-1], ast.If):
            if effect_guard_expression is not None:
                raise QueueFrontendError(
                    "ACPY-RULE-010: conditional effects cannot also use a "
                    "blocking rule guard"
                )
            guarded = body[-1]
            if guarded.orelse or not guarded.body:
                raise QueueFrontendError(
                    "ACPY-RULE-007: guarded state rule requires one if body "
                    "without else"
                )
            guarded_body = list(guarded.body)
            if guarded_body and isinstance(guarded_body[-1], ast.Return):
                returned = guarded_body[-1].value
                if not (isinstance(returned, ast.Constant) and returned.value is None):
                    return_expression = copy.deepcopy(returned)
                guarded_body = guarded_body[:-1]
            guard_expression = copy.deepcopy(guarded.test)
            body = [*body[:-1], *guarded_body]
        read_statement: ast.Assign | None = None
        if len(body) == 2 and isinstance(body[0], ast.Assign):
            read_statement = body[0]
            body = body[1:]
        if (
            len(body) != 1
            or not isinstance(body[0], ast.Assign)
            or len(body[0].targets) != 1
            or not isinstance(body[0].targets[0], ast.Subscript)
            or not isinstance(body[0].targets[0].value, ast.Name)
            or body[0].targets[0].value.id != table_argument
        ):
            raise QueueFrontendError(
                "ACPY-RULE-002: pure rules require one value-returning path; "
                "stateful rules require one indexed state assignment and an "
                "optional value return"
            )
        if read_statement is not None and (
            len(read_statement.targets) != 1
            or not isinstance(read_statement.targets[0], ast.Name)
            or read_statement.targets[0].id
            in ({table_argument, payload_argument} - {None})
            or not isinstance(read_statement.value, ast.Subscript)
            or not isinstance(read_statement.value.value, ast.Name)
            or read_statement.value.value.id != table_argument
        ):
            raise QueueFrontendError(
                "ACPY-RULE-002: stateful rule observation must bind one "
                "Entry from the same Table"
            )
        assignment = body[0]
        assert isinstance(assignment.targets[0], ast.Subscript)
        read_name: str | None = None
        read_index: ast.expr | None = None
        if read_statement is not None:
            assert isinstance(read_statement.targets[0], ast.Name)
            assert isinstance(read_statement.value, ast.Subscript)
            read_name = read_statement.targets[0].id
            read_index = copy.deepcopy(read_statement.value.slice)
        if effect_guard_expression is not None and len(payload_arguments) != 1:
            raise QueueFrontendError(
                "ACPY-RULE-010: conditional-effect early return requires "
                "exactly one payload parameter"
            )
        rule_definitions[node.name] = RuleDefinition(
            node.name,
            payload_arguments,
            return_expression,
            node.lineno,
            node.col_offset + 1,
            table_argument,
            copy.deepcopy(assignment.targets[0].slice),
            copy.deepcopy(assignment.value),
            read_name,
            read_index,
            guard=guard_expression,
            effect_guard=effect_guard_expression,
        )
    candidates = [
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == system
        and any(
            _decorator_name(d).rsplit(".", 1)[-1] == entry_kind
            for d in node.decorator_list
        )
    ]
    if len(candidates) != 1:
        raise QueueFrontendError(
            f"ACPY-QUEUE-001: system {system!r} is missing or ambiguous"
        )
    function = candidates[0]
    if specialization_fingerprint is not None:
        prefix = "sha256:"
        digest = specialization_fingerprint.removeprefix(prefix)
        if (
            not specialization_fingerprint.startswith(prefix)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise QueueFrontendError(
                "ACPY-QUEUE-022: specialization fingerprint is invalid"
            )
    if function.args.vararg is not None or function.args.kwarg is not None:
        raise QueueFrontendError(
            "ACPY-QUEUE-001: a queue system cannot use variadic parameters"
        )
    parameters = [
        *function.args.posonlyargs,
        *function.args.args,
        *function.args.kwonlyargs,
    ]
    supplied = dict(static_arguments or {})
    positional_defaults: dict[str, ast.expr] = {}
    positional = [*function.args.posonlyargs, *function.args.args]
    if function.args.defaults:
        for parameter, default in zip(
            positional[-len(function.args.defaults) :],
            function.args.defaults,
            strict=True,
        ):
            positional_defaults[parameter.arg] = default
    keyword_defaults = {
        parameter.arg: default
        for parameter, default in zip(
            function.args.kwonlyargs,
            function.args.kw_defaults,
            strict=True,
        )
        if default is not None
    }
    static_parameter_names: set[str] = set()
    external_parameters: list[tuple[str, ValueType]] = []
    for parameter in parameters:
        annotation_name = (
            _decorator_name(parameter.annotation.value).rsplit(".", 1)[-1]
            if isinstance(parameter.annotation, ast.Subscript)
            else ""
        )
        if annotation_name != "const":
            if parameter.arg in supplied:
                raise QueueFrontendError(
                    "ACPY-QUEUE-022: supplied static arguments must use ac.const"
                )
            if (
                parameter.arg in positional_defaults
                or parameter.arg in keyword_defaults
            ):
                raise QueueFrontendError(
                    "ACPY-QUEUE-022: external system values cannot have defaults"
                )
            external_parameters.append(
                (parameter.arg, _payload(parameter.annotation, payload_map))
            )
            continue
        static_parameter_names.add(parameter.arg)
        if parameter.arg in supplied:
            continue
        default = positional_defaults.get(parameter.arg) or keyword_defaults.get(
            parameter.arg
        )
        if default is None:
            raise QueueFrontendError(
                f"ACPY-QUEUE-022: system requires static argument {parameter.arg!r}"
            )
        try:
            supplied[parameter.arg] = evaluate_static(
                default, StaticEnvironment(supplied)
            )
        except ValueError as error:
            raise QueueFrontendError(
                f"ACPY-QUEUE-022: default for {parameter.arg!r} is not static"
            ) from error
    extras = sorted(set(supplied) - static_parameter_names)
    if extras:
        raise QueueFrontendError(
            f"ACPY-QUEUE-001: unknown static argument {extras[0]!r}"
        )
    system_static_values: Mapping[str, StaticValue] = supplied

    def system_result_payloads(
        annotation: ast.expr | None,
    ) -> tuple[ValueType, ...] | None:
        if annotation is None:
            return None
        if (isinstance(annotation, ast.Constant) and annotation.value is None) or (
            isinstance(annotation, ast.Name) and annotation.id == "None"
        ):
            return ()
        if isinstance(annotation, ast.Subscript) and _decorator_name(
            annotation.value
        ).rsplit(".", 1)[-1] in {"tuple", "Tuple"}:
            elements = (
                annotation.slice.elts
                if isinstance(annotation.slice, ast.Tuple)
                else (annotation.slice,)
            )
            if not elements:
                raise QueueFrontendError(
                    "ACPY-QUEUE-026: system result tuple cannot be empty"
                )
            return tuple(_payload(element, payload_map) for element in elements)
        return (_payload(annotation, payload_map),)

    result_payloads = system_result_payloads(function.returns)

    def _static_int(
        node: ast.expr,
        values: Mapping[str, StaticValue] | None = None,
    ) -> int | None:
        return _static_int_value(
            node, system_static_values if values is None else values
        )

    def _positive_int(
        call: ast.Call,
        name: str,
        default: int,
        values: Mapping[str, StaticValue] | None = None,
    ) -> int:
        return _positive_int_value(
            call,
            name,
            default,
            system_static_values if values is None else values,
        )

    def _nonnegative_int(
        call: ast.Call,
        name: str,
        default: int,
        values: Mapping[str, StaticValue] | None = None,
    ) -> int:
        return _nonnegative_int_value(
            call,
            name,
            default,
            system_static_values if values is None else values,
        )

    def _lambda(node: ast.expr) -> tuple[str, ast.expr]:
        argument, expression = _lambda_value(node)
        return argument, _constantize_expression(
            expression, argument, system_static_values
        )

    recursive_helpers: dict[str, RecursiveQueueHelper] = {}
    for helper in tree.body:
        if (
            not isinstance(helper, ast.FunctionDef)
            or helper is function
            or helper.decorator_list
            or len(helper.args.args) != 2
            or helper.args.posonlyargs
            or helper.args.kwonlyargs
            or len(helper.body) != 2
            or not isinstance(helper.body[0], ast.If)
            or not isinstance(helper.body[1], ast.Return)
        ):
            continue
        queue_parameter = helper.args.args[0].arg
        count_parameter = helper.args.args[1].arg
        base = helper.body[0]
        recursive_return = helper.body[1]
        if (
            not isinstance(base.test, ast.Compare)
            or len(base.test.ops) != 1
            or not isinstance(base.test.ops[0], ast.Eq)
            or len(base.test.comparators) != 1
            or not isinstance(base.test.left, ast.Name)
            or base.test.left.id != count_parameter
            or not isinstance(base.test.comparators[0], ast.Constant)
            or base.test.comparators[0].value != 0
            or len(base.body) != 1
            or not isinstance(base.body[0], ast.Return)
            or not isinstance(base.body[0].value, ast.Name)
            or base.body[0].value.id != queue_parameter
            or base.orelse
            or not isinstance(recursive_return.value, ast.Call)
        ):
            continue
        recursive_call = recursive_return.value
        if (
            not isinstance(recursive_call.func, ast.Name)
            or recursive_call.func.id != helper.name
            or len(recursive_call.args) != 2
            or recursive_call.keywords
            or not isinstance(recursive_call.args[0], ast.Call)
            or not isinstance(recursive_call.args[1], ast.BinOp)
            or not isinstance(recursive_call.args[1].op, ast.Sub)
            or not isinstance(recursive_call.args[1].left, ast.Name)
            or recursive_call.args[1].left.id != count_parameter
            or not isinstance(recursive_call.args[1].right, ast.Constant)
            or recursive_call.args[1].right.value != 1
        ):
            continue
        apply_call = recursive_call.args[0]
        if (
            not isinstance(apply_call.func, ast.Attribute)
            or apply_call.func.attr != "apply"
            or not isinstance(apply_call.func.value, ast.Name)
            or apply_call.func.value.id != queue_parameter
            or len(apply_call.args) != 1
        ):
            continue
        argument, expression = _lambda(apply_call.args[0])
        recursive_helpers[helper.name] = RecursiveQueueHelper(
            queue_parameter,
            count_parameter,
            argument,
            expression,
            apply_call,
        )
    queues: list[QueueBinding] = []
    effect_rules: list[QueueBinding] = []
    scopes: list[ScopeBinding] = []
    routes: list[RouteBinding] = []
    forks: list[ForkBinding] = []
    feedbacks: list[FeedbackBinding] = []
    merges: list[MergeBinding] = []
    reorders: list[ReorderBinding] = []
    dependencies: list[DependencyBinding] = []
    credits: list[CreditBinding] = []
    barriers: list[BarrierBinding] = []
    selects: list[SelectBinding] = []
    memory_instances: list[MemoryInstanceBinding] = []
    memory_requests: list[MemoryRequestBinding] = []
    memories: list[MemoryBinding] = []
    variables: list[VarStateBinding] = []
    tables: list[TableBinding] = []
    table_reads: list[TableReadBinding] = []
    table_writes: list[TableWriteBinding] = []
    masked_table_writes: list[MaskedTableWriteBinding] = []
    slots: list[SlotBinding] = []
    slot_releases: list[SlotReleaseBinding] = []
    candidates: list[CandidateSetBinding] = []
    selections: list[SelectionBinding] = []
    table_by_name: dict[str, TableBinding] = {}
    variable_by_name: dict[str, VarStateBinding] = {}
    entry_views: dict[str, EntryViewBinding | MaskedEntryViewBinding] = {}
    slot_by_name: dict[str, SlotBinding] = {}
    candidate_by_name: dict[str, CandidateSetBinding] = {}
    selection_by_name: dict[str, SelectionBinding] = {}
    memory_by_name: dict[str, MemoryInstanceBinding] = {}
    memory_arrays: dict[str, StaticMemoryArrayBinding] = {}
    selected_memories: dict[str, SelectedMemoryBinding] = {}
    consumed_selected_memories: set[str] = set()
    sinks: list[SinkBinding] = []
    observations: list[ObservationBinding] = []
    expectations: list[ExpectBinding] = []
    by_name: dict[str, QueueBinding] = {}
    collections: dict[str, StaticQueueCollection] = {}
    collection_bindings: list[CollectionBinding] = []
    order = 0

    for name, payload in external_parameters:
        if name in by_name:
            raise QueueFrontendError(
                "ACPY-QUEUE-026: external system values require unique names"
            )
        binding = QueueBinding(
            name,
            payload,
            1,
            1,
            None,
            scope=(),
            order=order,
            provider="boundary",
        )
        queues.append(binding)
        by_name[name] = binding
        order += 1

    def call_name(call: ast.Call) -> str:
        return _decorator_name(call.func).rsplit(".", 1)[-1]

    def normalized_write_fields(
        table: TableBinding,
        value: ast.expr | None,
        patch_fields: tuple[tuple[str, ast.expr], ...],
    ) -> tuple[str, ...]:
        if not isinstance(table.entry_type, StructType):
            return ("$entry",)
        declared = tuple(field.name for field in table.entry_type.fields)
        if value is not None:
            return declared
        requested = {name for name, _ in patch_fields}
        return tuple(name for name in declared if name in requested)

    def complete_value_fields(value_type: ValueType) -> tuple[str, ...]:
        if not isinstance(value_type, StructType):
            return ("$entry",)
        return tuple(field.name for field in value_type.fields)

    def reject_overlapping_table_writer(
        table: str, write_fields: tuple[str, ...], write_mode: str
    ) -> None:
        requested = set(write_fields)
        for write in (*table_writes, *masked_table_writes):
            if write.table != table:
                continue
            if write_mode == "replace" or write.write_mode == "replace":
                if write_mode == write.write_mode == "replace":
                    raise QueueFrontendError(
                        "ACPY-TABLE-009: table permits one allocation endpoint"
                    )
                continue
            overlap = requested.intersection(write.write_fields)
            if overlap:
                field = min(overlap)
                raise QueueFrontendError(
                    "ACPY-TABLE-004: table write field "
                    f"'{field}' has multiple endpoints"
                )

    def table_declaration(call: ast.Call) -> tuple[int, ValueType] | None:
        if not isinstance(call.func, ast.Subscript):
            return None
        if _decorator_name(call.func.value).rsplit(".", 1)[-1] != "table":
            return None
        parameters = call.func.slice
        if not isinstance(parameters, ast.Tuple) or len(parameters.elts) != 2:
            raise QueueFrontendError(
                "ACPY-TABLE-001: table requires ac.table[entries, Entry]"
            )
        entries = _static_int(parameters.elts[0])
        if entries is None or entries <= 0:
            raise QueueFrontendError(
                "ACPY-TABLE-001: table entries must be a positive static integer"
            )
        entry_type = _payload(parameters.elts[1], payload_map)
        if call.args or any(
            keyword.arg is None or keyword.arg != "init" for keyword in call.keywords
        ):
            raise QueueFrontendError(
                "ACPY-TABLE-001: table accepts only keyword init=0"
            )
        init_values = [keyword.value for keyword in call.keywords]
        init = 0 if not init_values else _static_int(init_values[0])
        if len(init_values) > 1 or init != 0:
            raise QueueFrontendError("ACPY-TABLE-001: table init must be exactly zero")
        return entries, entry_type

    def parse_view(
        node: ast.expr,
        alias: str,
        scope_path: tuple[str, ...],
        current_order: int,
    ) -> EntryViewBinding | MaskedEntryViewBinding | None:
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            return None
        if node.func.attr != "view" or not isinstance(node.func.value, ast.Name):
            return None
        table_name = node.func.value.id
        if table_name not in table_by_name:
            return None
        if len(node.args) != 1 or node.keywords:
            raise QueueFrontendError(
                "ACPY-TABLE-002: table.view requires one index or selector lambda"
            )
        selector = node.args[0]
        if isinstance(selector, ast.Name) and selector.id in candidate_by_name:
            candidate = candidate_by_name[selector.id]
            if candidate.table != table_name:
                raise QueueFrontendError(
                    "ACPY-TABLE-008: CandidateSet belongs to a different Table"
                )
            return MaskedEntryViewBinding(
                alias, table_name, candidate.name, scope_path, current_order
            )
        if (
            isinstance(selector, ast.Attribute)
            and isinstance(selector.value, ast.Name)
            and selector.value.id in selection_by_name
            and selection_by_name[selector.value.id].table != table_name
        ):
            raise QueueFrontendError(
                "ACPY-TABLE-007: Selection belongs to a different Table"
            )
        if isinstance(selector, ast.Lambda):
            argument, address = _lambda(selector)
        else:
            argument, address = (
                None,
                _constantize_expression(selector, "", system_static_values),
            )
        return EntryViewBinding(
            alias, table_name, argument, address, scope_path, current_order
        )

    def resolve_view(
        node: ast.expr,
        scope_path: tuple[str, ...],
        current_order: int,
    ) -> EntryViewBinding | MaskedEntryViewBinding | None:
        if isinstance(node, ast.Name):
            view = entry_views.get(node.id)
            if view and view.scope == scope_path:
                return view
            return None
        return parse_view(node, "", scope_path, current_order)

    def lambda_or_constant(node: ast.expr, argument: str, diagnostic: str) -> ast.expr:
        if isinstance(node, ast.Lambda):
            candidate_argument, expression = _lambda(node)
            if candidate_argument != argument:
                raise QueueFrontendError(diagnostic)
            return expression
        return _constantize_expression(node, argument, system_static_values)

    def keyword_value(call: ast.Call, name: str) -> ast.expr:
        matches = [keyword.value for keyword in call.keywords if keyword.arg == name]
        if len(matches) != 1:
            raise QueueFrontendError(
                f"ACPY-QUEUE-024: high-level block requires one {name!r} parameter"
            )
        return matches[0]

    def field_expression(
        node: ast.expr,
        queue: QueueBinding,
        argument: str = "item",
    ) -> ast.expr:
        if not isinstance(node, ast.Attribute) or not isinstance(node.value, ast.Name):
            raise QueueFrontendError(
                "ACPY-QUEUE-024: high-level block requires a typed field descriptor"
            )
        payload = next(
            (item for item in payloads if item.descriptor == queue.payload), None
        )
        if payload is None or node.value.id != payload.name:
            raise QueueFrontendError(
                "ACPY-QUEUE-024: field descriptor payload does not match Queue"
            )
        if node.attr not in {field.name for field in payload.descriptor.fields}:
            raise QueueFrontendError(
                f"ACPY-QUEUE-024: payload has no field {node.attr!r}"
            )
        return ast.copy_location(
            ast.Attribute(
                value=ast.Name(id=argument, ctx=ast.Load()),
                attr=node.attr,
                ctx=ast.Load(),
            ),
            node,
        )

    def policy_value(call: ast.Call) -> str:
        matches = [
            keyword.value for keyword in call.keywords if keyword.arg == "policy"
        ]
        if not matches:
            return "priority"
        if len(matches) != 1:
            raise QueueFrontendError("ACPY-QUEUE-024: repeated merge policy")
        node = matches[0]
        if isinstance(node, ast.Constant) and type(node.value) is str:
            policy = node.value
        else:
            policy = _decorator_name(node).rsplit(".", 1)[-1]
        if policy not in {"priority", "round_robin"}:
            raise QueueFrontendError(
                "ACPY-QUEUE-024: merge policy must be priority or round_robin"
            )
        return policy

    def static_reference(
        node: ast.expr,
        aliases: dict[str, str | StaticQueueCollection],
    ) -> str | StaticQueueCollection:
        if isinstance(node, ast.Name):
            if node.id in aliases:
                return aliases[node.id]
            if node.id in by_name:
                return by_name[node.id].name
            if node.id in collections:
                return collections[node.id]
        if (
            isinstance(node, ast.Subscript)
            and isinstance(node.slice, ast.Constant)
            and type(node.slice.value) in {str, int, bool}
        ):
            collection = static_reference(node.value, aliases)
            if not isinstance(collection, StaticQueueCollection):
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: static indexing requires a collection"
                )
            for key, value in collection.members:
                if type(key) is type(node.slice.value) and key == node.slice.value:
                    return value
            raise QueueFrontendError(
                f"ACPY-QUEUE-005: collection has no key {node.slice.value!r}"
            )
        raise QueueFrontendError(
            "ACPY-QUEUE-005: collection reference must be statically resolvable"
        )

    def queue_reference(
        node: ast.expr,
        aliases: dict[str, str | StaticQueueCollection],
    ) -> str:
        value = static_reference(node, aliases)
        if isinstance(value, str):
            return value
        raise QueueFrontendError(
            "ACPY-QUEUE-005: a collection cannot be used as one Queue"
        )

    def collection_signature(
        value: str | StaticQueueCollection,
    ) -> tuple[object, ...]:
        if isinstance(value, str):
            return ("queue", by_name[value].payload)
        keys = tuple(key for key, _ in value.members)
        members = tuple(collection_signature(member) for _, member in value.members)
        return (value.kind, keys, members)

    def stable_collection_identity(value: str | StaticQueueCollection) -> str:
        if isinstance(value, str):
            return value
        return (
            value.kind
            + "("
            + ",".join(
                f"{key}:{stable_collection_identity(member)}"
                for key, member in value.members
            )
            + ")"
        )

    def source_binding(
        name: str,
        call: ast.Call,
        scope_path: tuple[str, ...],
        current_order: int,
        static_values: Mapping[str, StaticValue] | None = None,
    ) -> QueueBinding:
        if call_name(call) != "source" or len(call.args) != 1:
            raise QueueFrontendError(
                "ACPY-QUEUE-005: collection elements must be Queue sources"
            )
        depth = _positive_int(call, "depth", 1, static_values)
        rate = _positive_int(call, "rate", 1, static_values)
        if rate > depth:
            raise QueueFrontendError("ACPY-QUEUE-025: Queue rate must not exceed depth")
        return QueueBinding(
            name,
            _payload(call.args[0], payload_map),
            depth,
            _positive_int(call, "latency", 1, static_values),
            None,
            scope=scope_path,
            order=current_order,
            rate=rate,
        )

    def memory_instance_binding(
        name: str,
        call: ast.Call,
        scope_path: tuple[str, ...],
        current_order: int,
        static_values: dict[str, int] | None = None,
    ) -> MemoryInstanceBinding:
        if call_name(call) != "memory" or len(call.args) != 1:
            raise QueueFrontendError("ACPY-QUEUE-015: memory requires one data type")
        if any(
            keyword.arg is None or keyword.arg not in {"entries", "init", "latency"}
            for keyword in call.keywords
        ):
            raise QueueFrontendError(
                "ACPY-QUEUE-015: memory instance has an unsupported keyword"
            )
        data_type = _payload(call.args[0], payload_map)
        if _epoch_05_integer_width(data_type) is None:
            raise QueueFrontendError(
                "ACPY-QUEUE-015: memory data type must be an integer"
            )
        entries = _positive_int(call, "entries", 16, static_values)
        init = _nonnegative_int(call, "init", 0, static_values)
        latency = _positive_int(call, "latency", 1, static_values)
        if init != 0:
            raise QueueFrontendError("ACPY-QUEUE-015: memory init must be zero")
        return MemoryInstanceBinding(
            name, data_type, entries, init, latency, scope_path, current_order
        )

    def memory_request_parameters(
        call: ast.Call,
        incoming: QueueBinding,
        data_type: ValueType,
        extra_keywords: set[str] | None = None,
    ) -> tuple[str, ast.expr, ast.expr, ast.expr, str, int]:
        allowed_keywords = {
            "address",
            "write",
            "data",
            "result_field",
            "depth",
            *(extra_keywords or set()),
        }
        if any(
            keyword.arg is None or keyword.arg not in allowed_keywords
            for keyword in call.keywords
        ):
            raise QueueFrontendError(
                "ACPY-QUEUE-015: memory request has an unsupported keyword"
            )
        policies: dict[str, ast.expr] = {}
        for policy in ("address", "write", "data"):
            values = [
                keyword.value for keyword in call.keywords if keyword.arg == policy
            ]
            if len(values) != 1:
                raise QueueFrontendError(
                    f"ACPY-QUEUE-015: memory request requires one {policy} lambda"
                )
            policies[policy] = values[0]
        arguments_and_values = [_lambda(policies[item]) for item in policies]
        if len({argument for argument, _ in arguments_and_values}) != 1:
            raise QueueFrontendError(
                "ACPY-QUEUE-015: memory request lambdas require one argument name"
            )
        result_fields = [
            keyword.value for keyword in call.keywords if keyword.arg == "result_field"
        ]
        if (
            len(result_fields) != 1
            or not isinstance(result_fields[0], ast.Constant)
            or type(result_fields[0].value) is not str
            or not result_fields[0].value
        ):
            raise QueueFrontendError(
                "ACPY-QUEUE-015: memory request requires one static result_field"
            )
        payload = next(
            (
                declaration
                for declaration in payloads
                if declaration.descriptor == incoming.payload
            ),
            None,
        )
        result_field = result_fields[0].value
        field_types = dict(payload.field_descriptors) if payload is not None else {}
        if result_field not in field_types:
            raise QueueFrontendError("ACPY-QUEUE-015: memory result_field is unknown")
        if not _types_equal_in_epoch_05(field_types[result_field], data_type):
            raise QueueFrontendError(
                "ACPY-QUEUE-015: memory result_field must match instance data type"
            )
        return (
            arguments_and_values[0][0],
            arguments_and_values[0][1],
            arguments_and_values[1][1],
            arguments_and_values[2][1],
            result_field,
            _positive_int(call, "depth", 1),
        )

    def collection_binding(
        name: str,
        call: ast.Call,
        scope_path: tuple[str, ...],
        current_order: int,
        aliases: dict[str, str | StaticQueueCollection],
        static_values: Mapping[str, StaticValue] | None = None,
    ) -> StaticQueueCollection | None:
        static_values = system_static_values if static_values is None else static_values
        kind = call_name(call)
        if kind == "array":
            extent = (
                _static_int(call.args[0], static_values)
                if len(call.args) == 2
                else None
            )
            if len(call.args) != 2 or extent is None or extent <= 0:
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: array requires a positive compile-time extent"
                )
            argument, body = _lambda(call.args[1])
            members: list[tuple[str | int, str | StaticQueueCollection]] = []
            for index in range(extent):
                if not isinstance(body, ast.Call):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-005: array generator must produce a Queue"
                    )
                leaf = f"{name}__{index}"
                values = {**static_values, argument: index}
                if call_name(body) == "source":
                    binding = source_binding(
                        leaf, body, scope_path, current_order, values
                    )
                    queues.append(binding)
                    by_name[leaf] = binding
                    member: str | StaticQueueCollection = leaf
                else:
                    nested = collection_binding(
                        leaf,
                        body,
                        scope_path,
                        current_order,
                        aliases,
                        values,
                    )
                    if nested is None:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-005: array generator must produce a Queue "
                            "or static collection"
                        )
                    member = nested
                members.append((index, member))
            signatures = {collection_signature(member) for _, member in members}
            if len(signatures) != 1:
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: array elements must have one static shape"
                )
            return StaticQueueCollection("array", tuple(members))
        if kind == "map":
            if len(call.args) != 1 or not isinstance(call.args[0], ast.Dict):
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: map requires one compile-time dictionary"
                )
            entries: list[tuple[str | int | bool, str | StaticQueueCollection]] = []
            for key, value in zip(call.args[0].keys, call.args[0].values, strict=True):
                if (
                    not isinstance(key, ast.Constant)
                    or type(key.value) not in {str, int, bool}
                    or (type(key.value) is str and not key.value)
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-005: map keys must be compile-time bool/int/str"
                    )
                entries.append((key.value, static_reference(value, aliases)))
            rank = {bool: 0, int: 1, str: 2}
            entries.sort(key=lambda item: (rank[type(item[0])], item[0]))
            if not entries or len({(type(key), key) for key, _ in entries}) != len(
                entries
            ):
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: map keys must be unique and non-empty"
                )
            if len({collection_signature(value) for _, value in entries}) != 1:
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: map values must have one static shape"
                )
            return StaticQueueCollection("map", tuple(entries))
        if kind == "set":
            if len(call.args) != 1 or not isinstance(
                call.args[0], (ast.Set, ast.List, ast.Tuple)
            ):
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: set requires one compile-time collection"
                )
            members = [static_reference(item, aliases) for item in call.args[0].elts]
            identities = [stable_collection_identity(member) for member in members]
            if not members or len(set(identities)) != len(members):
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: set members must be unique and non-empty"
                )
            members.sort(key=stable_collection_identity)
            if len({collection_signature(member) for member in members}) != 1:
                raise QueueFrontendError(
                    "ACPY-QUEUE-005: set members must have one static shape"
                )
            return StaticQueueCollection(
                "set", tuple((index, member) for index, member in enumerate(members))
            )
        return None

    def visit(
        statements: list[ast.stmt],
        scope_path: tuple[str, ...],
        aliases: dict[str, str | StaticQueueCollection] | None = None,
    ) -> None:
        nonlocal order
        aliases = {} if aliases is None else aliases
        for statement in statements:
            current_order = order
            order += 1
            if (
                isinstance(statement, ast.AnnAssign)
                and isinstance(statement.target, ast.Name)
                and statement.value is not None
            ):
                name = statement.target.id
                if name in variable_by_name or name in by_name:
                    raise QueueFrontendError(
                        "ACPY-VAR-001: persistent variable requires a fresh name"
                    )
                entries = 1
                annotation = statement.annotation
                if isinstance(annotation, ast.Subscript) and _decorator_name(
                    annotation.value
                ).rsplit(".", 1)[-1] in {"list", "List"}:
                    value_type = _payload(annotation.slice, payload_map)
                    initializer = statement.value
                    repeated: ast.expr | None = None
                    count: int | None = None
                    if isinstance(initializer, ast.BinOp) and isinstance(
                        initializer.op, ast.Mult
                    ):
                        if isinstance(initializer.left, ast.List):
                            repeated = initializer.left
                            count = _static_int(initializer.right)
                        elif isinstance(initializer.right, ast.List):
                            repeated = initializer.right
                            count = _static_int(initializer.left)
                    if repeated is not None:
                        if (
                            not isinstance(repeated, ast.List)
                            or len(repeated.elts) != 1
                            or not isinstance(repeated.elts[0], ast.Constant)
                            or repeated.elts[0].value != 0
                            or count is None
                            or count <= 0
                        ):
                            raise QueueFrontendError(
                                "ACPY-VAR-002: persistent list requires [0] * N "
                                "with positive static N"
                            )
                        entries = count
                    elif isinstance(initializer, ast.List):
                        if not initializer.elts or any(
                            not isinstance(element, ast.Constant) or element.value != 0
                            for element in initializer.elts
                        ):
                            raise QueueFrontendError(
                                "ACPY-VAR-002: persistent list initializer must "
                                "be a non-empty zero image"
                            )
                        entries = len(initializer.elts)
                    else:
                        raise QueueFrontendError(
                            "ACPY-VAR-002: persistent list requires a static zero "
                            "initializer"
                        )
                    init: int | bool = False if isinstance(value_type, BoolType) else 0
                else:
                    value_type = _payload(annotation, payload_map)
                    if not isinstance(statement.value, ast.Constant) or type(
                        statement.value.value
                    ) not in {bool, int}:
                        raise QueueFrontendError(
                            "ACPY-VAR-001: persistent scalar init must be constant"
                        )
                    init = statement.value.value
                if isinstance(
                    value_type, (StructType, TupleType, ArrayType, EnumType)
                ) and (type(init) is not int or init != 0):
                    raise QueueFrontendError(
                        "ACPY-VAR-001: persistent struct init must be zero"
                    )
                if isinstance(value_type, BoolType) and type(init) is not bool:
                    raise QueueFrontendError(
                        "ACPY-VAR-001: bool variable requires bool init"
                    )
                if not isinstance(value_type, BoolType) and type(init) is not int:
                    raise QueueFrontendError(
                        "ACPY-VAR-001: integer variable requires integer init"
                    )
                binding = VarStateBinding(
                    name, value_type, init, scope_path, current_order, entries
                )
                variables.append(binding)
                variable_by_name[name] = binding
                continue
            assigned_names: tuple[str, ...] = ()
            if isinstance(statement, ast.Assign) and len(statement.targets) == 1:
                target = statement.targets[0]
                if isinstance(target, ast.Name):
                    assigned_names = (target.id,)
                elif isinstance(target, (ast.Tuple, ast.List)) and all(
                    isinstance(item, ast.Name) for item in target.elts
                ):
                    assigned_names = tuple(item.id for item in target.elts)
            if any(
                name in memory_by_name
                or name in memory_arrays
                or name in selected_memories
                or name in table_by_name
                or name in entry_views
                or name in slot_by_name
                or name in candidate_by_name
                or name in selection_by_name
                for name in assigned_names
            ):
                raise QueueFrontendError(
                    "ACPY-QUEUE-015: state binding cannot be rebound"
                )
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
            ):
                call = statement.value
                declaration = table_declaration(statement.value)
                if declaration is not None:
                    name = statement.targets[0].id
                    if name in by_name or name in collections or name in table_by_name:
                        raise QueueFrontendError(
                            "ACPY-TABLE-001: table declaration requires a fresh name"
                        )
                    entries, entry_type = declaration
                    binding = TableBinding(
                        name, entry_type, entries, scope_path, current_order
                    )
                    tables.append(binding)
                    table_by_name[name] = binding
                    continue
                if call_name(call) == "slot":
                    if len(call.args) != 1 or call.keywords:
                        raise QueueFrontendError(
                            "ACPY-SLOT-001: ac.slot requires exactly one Queue"
                        )
                    name = statement.targets[0].id
                    if name in by_name or name in collections or name in slot_by_name:
                        raise QueueFrontendError(
                            "ACPY-SLOT-001: slot declaration requires a fresh name"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                    binding = SlotBinding(
                        name,
                        input_name,
                        by_name[input_name].payload,
                        scope_path,
                        current_order,
                    )
                    slots.append(binding)
                    slot_by_name[name] = binding
                    continue
                if (
                    isinstance(call.func, ast.Attribute)
                    and call.func.attr == "match"
                    and isinstance(call.func.value, ast.Name)
                    and call.func.value.id in table_by_name
                ):
                    name = statement.targets[0].id
                    table_name = call.func.value.id
                    table = table_by_name[table_name]
                    if table.entries > 64:
                        raise QueueFrontendError(
                            "ACPY-TABLE-006: table.match domain must contain 1..64 entries"
                        )
                    if len(call.args) != 1 or call.keywords:
                        raise QueueFrontendError(
                            "ACPY-TABLE-006: table.match requires one predicate lambda"
                        )
                    argument, predicate = _lambda(call.args[0])
                    binding = CandidateSetBinding(
                        name, table_name, argument, predicate, scope_path, current_order
                    )
                    candidates.append(binding)
                    candidate_by_name[name] = binding
                    continue
                if (
                    isinstance(call.func, ast.Attribute)
                    and call.func.attr == "choose"
                    and isinstance(call.func.value, ast.Name)
                    and call.func.value.id in table_by_name
                ):
                    name = statement.targets[0].id
                    table_name = call.func.value.id
                    if len(call.args) != 1 or not isinstance(call.args[0], ast.Name):
                        raise QueueFrontendError(
                            "ACPY-TABLE-007: table.choose requires one CandidateSet"
                        )
                    candidate = candidate_by_name.get(call.args[0].id)
                    if candidate is None or candidate.table != table_name:
                        raise QueueFrontendError(
                            "ACPY-TABLE-007: CandidateSet belongs to a different Table"
                        )
                    keywords = {keyword.arg: keyword.value for keyword in call.keywords}
                    if None in keywords or set(keywords) - {"count", "policy", "key"}:
                        raise QueueFrontendError(
                            "ACPY-TABLE-007: table.choose parameters are invalid"
                        )
                    count = _static_int(keywords.get("count", ast.Constant(1)))
                    if count != 1:
                        raise QueueFrontendError(
                            "ACPY-TABLE-007: table.choose supports count=1 only"
                        )
                    policy_node = keywords.get("policy", ast.Constant("first"))
                    policy = (
                        policy_node.value
                        if isinstance(policy_node, ast.Constant)
                        and isinstance(policy_node.value, str)
                        else None
                    )
                    if policy not in {"first", "min", "max"}:
                        raise QueueFrontendError(
                            "ACPY-TABLE-007: choose policy must be first, min, or max"
                        )
                    key_node = keywords.get("key")
                    key_argument: str | None = None
                    key: ast.expr | None = None
                    if policy == "first":
                        if key_node is not None:
                            raise QueueFrontendError(
                                "ACPY-TABLE-007: first policy does not accept key"
                            )
                    else:
                        if key_node is None:
                            raise QueueFrontendError(
                                "ACPY-TABLE-007: min/max policy requires key lambda"
                            )
                        key_argument, key = _lambda(key_node)
                    binding = SelectionBinding(
                        name,
                        table_name,
                        candidate.name,
                        str(policy),
                        key_argument,
                        key,
                        scope_path,
                        current_order,
                    )
                    selections.append(binding)
                    selection_by_name[name] = binding
                    continue
                view = parse_view(
                    statement.value,
                    statement.targets[0].id,
                    scope_path,
                    current_order,
                )
                if view is not None:
                    if view.name in by_name or view.name in collections:
                        raise QueueFrontendError(
                            "ACPY-TABLE-002: EntryView alias requires a fresh name"
                        )
                    entry_views[view.name] = view
                    continue
            if (
                isinstance(statement, ast.Expr)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "release"
                and isinstance(statement.value.func.value, ast.Name)
                and statement.value.func.value.id in slot_by_name
            ):
                call = statement.value
                slot_name = call.func.value.id
                if call.args or any(
                    keyword.arg is None or keyword.arg != "when"
                    for keyword in call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-SLOT-002: slot.release accepts only when=expression"
                    )
                values = [
                    keyword.value for keyword in call.keywords if keyword.arg == "when"
                ]
                if len(values) != 1 or isinstance(values[0], ast.Lambda):
                    raise QueueFrontendError(
                        "ACPY-SLOT-002: slot.release requires one state expression"
                    )
                if any(release.slot == slot_name for release in slot_releases):
                    raise QueueFrontendError(
                        "ACPY-SLOT-002: slot permits exactly one release endpoint"
                    )
                slot_releases.append(
                    SlotReleaseBinding(
                        slot_name,
                        _constantize_expression(values[0], "", system_static_values),
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "read"
            ):
                call = statement.value
                view = resolve_view(call.func.value, scope_path, current_order)
                if view is not None:
                    if isinstance(view, MaskedEntryViewBinding):
                        raise QueueFrontendError(
                            "ACPY-TABLE-008: masked Table view does not support read"
                        )
                    name = statement.targets[0].id
                    if name in by_name or name in collections or name in table_by_name:
                        raise QueueFrontendError(
                            "ACPY-TABLE-003: table read output requires a fresh name"
                        )
                    if len(call.args) > 1 or any(
                        keyword.arg is None
                        or keyword.arg not in {"when", "depth", "latency"}
                        for keyword in call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-TABLE-003: table read parameters are invalid"
                        )
                    input_name: str | None = None
                    argument = view.argument
                    if call.args:
                        if argument is None:
                            raise QueueFrontendError(
                                "ACPY-TABLE-003: Queue-driven read requires a "
                                "selector lambda"
                            )
                        input_name = queue_reference(call.args[0], aliases)
                    elif argument is not None:
                        raise QueueFrontendError(
                            "ACPY-TABLE-003: state-driven read requires a bound index"
                        )
                    when_values = [
                        keyword.value
                        for keyword in call.keywords
                        if keyword.arg == "when"
                    ]
                    if len(when_values) > 1:
                        raise QueueFrontendError(
                            "ACPY-TABLE-003: table read has repeated when"
                        )
                    when_node = when_values[0] if when_values else ast.Constant(True)
                    if argument is not None:
                        when = lambda_or_constant(
                            when_node,
                            argument,
                            "ACPY-TABLE-003: selector and when lambdas require "
                            "one argument name",
                        )
                    else:
                        if isinstance(when_node, ast.Lambda):
                            raise QueueFrontendError(
                                "ACPY-TABLE-003: state-driven when is an "
                                "EntryView expression"
                            )
                        when = _constantize_expression(
                            when_node, "", system_static_values
                        )
                    depth = _positive_int(call, "depth", 1)
                    latency = _positive_int(call, "latency", 1)
                    table = table_by_name[view.table]
                    queue = QueueBinding(
                        name,
                        table.entry_type,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        table_read_output=True,
                    )
                    queues.append(queue)
                    by_name[name] = queue
                    table_reads.append(
                        TableReadBinding(
                            view.table,
                            input_name,
                            name,
                            argument,
                            view.address,
                            when,
                            view.name or None,
                            depth,
                            latency,
                            scope_path,
                            current_order,
                        )
                    )
                    continue
            if (
                isinstance(statement, ast.Expr)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr in {"write", "patch", "allocate"}
            ):
                call = statement.value
                view = resolve_view(call.func.value, scope_path, current_order)
                if view is not None:
                    method = call.func.attr
                    if isinstance(view, MaskedEntryViewBinding):
                        if method == "allocate":
                            raise QueueFrontendError(
                                "ACPY-TABLE-009: allocation requires a scalar view"
                            )
                        if call.args:
                            raise QueueFrontendError(
                                "ACPY-TABLE-008: masked write/patch is state-driven "
                                "and takes no Queue"
                            )
                        enable_values = [
                            keyword.value
                            for keyword in call.keywords
                            if keyword.arg == "enable"
                        ]
                        if len(enable_values) > 1:
                            raise QueueFrontendError(
                                "ACPY-TABLE-008: repeated masked write enable"
                            )
                        enable_node = (
                            enable_values[0] if enable_values else ast.Constant(True)
                        )
                        if isinstance(enable_node, ast.Lambda):
                            raise QueueFrontendError(
                                "ACPY-TABLE-008: masked enable must be an expression"
                            )
                        enable = _constantize_expression(
                            enable_node, "", system_static_values
                        )
                        table = table_by_name[view.table]
                        value: ast.expr | None = None
                        patch_fields: tuple[tuple[str, ast.expr], ...] = ()
                        if method == "write":
                            if any(
                                keyword.arg is None
                                or keyword.arg not in {"value", "enable"}
                                for keyword in call.keywords
                            ):
                                raise QueueFrontendError(
                                    "ACPY-TABLE-008: masked write accepts only "
                                    "value and enable"
                                )
                            values = [
                                keyword.value
                                for keyword in call.keywords
                                if keyword.arg == "value"
                            ]
                            if len(values) != 1:
                                raise QueueFrontendError(
                                    "ACPY-TABLE-008: masked write requires one value"
                                )
                            if isinstance(values[0], ast.Lambda):
                                raise QueueFrontendError(
                                    "ACPY-TABLE-008: masked write value must be a "
                                    "uniform expression, not a lambda"
                                )
                            value = _constantize_expression(
                                values[0], "", system_static_values
                            )
                        else:
                            if not isinstance(table.entry_type, StructType):
                                raise QueueFrontendError(
                                    "ACPY-TABLE-008: masked patch requires a struct "
                                    "Table Entry"
                                )
                            field_types = {
                                field.name: field.type
                                for field in table.entry_type.fields
                            }
                            patches: list[tuple[str, ast.expr]] = []
                            for keyword in call.keywords:
                                if keyword.arg == "enable":
                                    continue
                                if (
                                    keyword.arg is None
                                    or keyword.arg not in field_types
                                ):
                                    raise QueueFrontendError(
                                        "ACPY-TABLE-008: masked patch field is unknown"
                                    )
                                expression = keyword.value
                                if isinstance(expression, ast.Lambda):
                                    old_name, expression = _lambda(expression)

                                    class OldEntryName(ast.NodeTransformer):
                                        def visit_Name(
                                            self, node: ast.Name
                                        ) -> ast.expr:
                                            if node.id == old_name:
                                                return ast.copy_location(
                                                    ast.Name(
                                                        id="__old",
                                                        ctx=node.ctx,
                                                    ),
                                                    node,
                                                )
                                            return node

                                    expression = OldEntryName().visit(
                                        copy.deepcopy(expression)
                                    )
                                else:
                                    expression = _constantize_expression(
                                        expression, "", system_static_values
                                    )
                                patches.append((keyword.arg, expression))
                            if not patches:
                                raise QueueFrontendError(
                                    "ACPY-TABLE-008: masked patch requires at least "
                                    "one field"
                                )
                            if len({name for name, _ in patches}) != len(patches):
                                raise QueueFrontendError(
                                    "ACPY-TABLE-008: masked patch field is repeated"
                                )
                            patch_fields = tuple(patches)
                        write_fields = normalized_write_fields(
                            table, value, patch_fields
                        )
                        reject_overlapping_table_writer(
                            view.table, write_fields, "field"
                        )
                        masked_table_writes.append(
                            MaskedTableWriteBinding(
                                view.table,
                                view.candidates,
                                enable,
                                value,
                                patch_fields,
                                write_fields,
                                "field",
                                scope_path,
                                current_order,
                            )
                        )
                        continue
                    queue_driven = view.argument is not None
                    if method == "allocate" and queue_driven:
                        raise QueueFrontendError(
                            "ACPY-TABLE-009: allocation must be state-driven"
                        )
                    if len(call.args) != (1 if queue_driven else 0):
                        raise QueueFrontendError(
                            "ACPY-TABLE-004: Queue-driven table write/patch requires "
                            "one Queue; state-driven write/patch takes no Queue"
                        )
                    input_name = (
                        queue_reference(call.args[0], aliases) if queue_driven else None
                    )
                    argument = view.argument
                    enable_values = [
                        keyword.value
                        for keyword in call.keywords
                        if keyword.arg == "enable"
                    ]
                    if len(enable_values) > 1:
                        raise QueueFrontendError(
                            "ACPY-TABLE-004: repeated write enable"
                        )
                    enable_node = (
                        enable_values[0] if enable_values else ast.Constant(True)
                    )
                    if queue_driven:
                        assert argument is not None
                        enable = lambda_or_constant(
                            enable_node,
                            argument,
                            "ACPY-TABLE-004: selector and enable lambdas require "
                            "one argument name",
                        )
                    else:
                        if isinstance(enable_node, ast.Lambda):
                            raise QueueFrontendError(
                                "ACPY-TABLE-004: state-driven enable must be an "
                                "expression, not a lambda"
                            )
                        enable = _constantize_expression(
                            enable_node, "", system_static_values
                        )
                    value: ast.expr | None = None
                    patch_fields: tuple[tuple[str, ast.expr], ...] = ()
                    table = table_by_name[view.table]
                    if method in {"write", "allocate"}:
                        if any(
                            keyword.arg is None
                            or keyword.arg not in {"value", "enable"}
                            for keyword in call.keywords
                        ):
                            raise QueueFrontendError(
                                "ACPY-TABLE-004: write/allocation accepts only "
                                "value and enable"
                            )
                        values = [
                            keyword.value
                            for keyword in call.keywords
                            if keyword.arg == "value"
                        ]
                        if len(values) != 1:
                            raise QueueFrontendError(
                                "ACPY-TABLE-004: write/allocation requires one value"
                            )
                        if queue_driven:
                            assert argument is not None
                            value = lambda_or_constant(
                                values[0],
                                argument,
                                "ACPY-TABLE-004: selector and value lambdas require "
                                "one argument name",
                            )
                        else:
                            if isinstance(values[0], ast.Lambda):
                                raise QueueFrontendError(
                                    "ACPY-TABLE-004: state-driven value must be an "
                                    "expression, not a lambda"
                                )
                            value = _constantize_expression(
                                values[0], "", system_static_values
                            )
                    else:
                        if not isinstance(table.entry_type, StructType):
                            raise QueueFrontendError(
                                "ACPY-TABLE-004: patch requires a struct Table Entry"
                            )
                        field_types = {
                            field.name: field.type for field in table.entry_type.fields
                        }
                        patches: list[tuple[str, ast.expr]] = []
                        for keyword in call.keywords:
                            if keyword.arg == "enable":
                                continue
                            if keyword.arg is None or keyword.arg not in field_types:
                                raise QueueFrontendError(
                                    "ACPY-TABLE-004: patch field is unknown"
                                )
                            patches.append(
                                (
                                    keyword.arg,
                                    (
                                        lambda_or_constant(
                                            keyword.value,
                                            argument or "",
                                            "ACPY-TABLE-004: patch lambdas require "
                                            "one argument name",
                                        )
                                        if queue_driven
                                        else _constantize_expression(
                                            keyword.value, "", system_static_values
                                        )
                                    ),
                                )
                            )
                        if not patches:
                            raise QueueFrontendError(
                                "ACPY-TABLE-004: patch requires at least one field"
                            )
                        if len({name for name, _ in patches}) != len(patches):
                            raise QueueFrontendError(
                                "ACPY-TABLE-004: patch field is repeated"
                            )
                        patch_fields = tuple(patches)
                    write_fields = normalized_write_fields(table, value, patch_fields)
                    write_mode = "replace" if method == "allocate" else "field"
                    reject_overlapping_table_writer(
                        view.table, write_fields, write_mode
                    )
                    table_writes.append(
                        TableWriteBinding(
                            view.table,
                            input_name,
                            argument,
                            view.address,
                            enable,
                            value,
                            patch_fields,
                            write_fields,
                            write_mode,
                            scope_path,
                            current_order,
                        )
                    )
                    continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) == "memory"
                and len(statement.value.args) == 1
            ):
                name = statement.targets[0].id
                call = statement.value
                if (
                    name in by_name
                    or name in collections
                    or name in memory_by_name
                    or name in memory_arrays
                    or name in selected_memories
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory instance requires one fresh name"
                    )
                instance = memory_instance_binding(
                    name, call, scope_path, current_order
                )
                memory_instances.append(instance)
                memory_by_name[name] = instance
                continue
            if isinstance(statement, ast.If):
                if (
                    isinstance(statement.test, ast.Constant)
                    and type(statement.test.value) is bool
                ):
                    selected = (
                        statement.body if statement.test.value else statement.orelse
                    )
                    visit(selected, scope_path, aliases)
                    continue

                def parse_arm(
                    body: list[ast.stmt],
                ) -> tuple[str, str, ast.Call, str, ast.expr]:
                    if (
                        len(body) != 1
                        or not isinstance(body[0], ast.Assign)
                        or len(body[0].targets) != 1
                        or not isinstance(body[0].targets[0], ast.Name)
                        or not isinstance(body[0].value, ast.Call)
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-011: runtime if requires one apply "
                            "assignment in each branch"
                        )
                    target = body[0].targets[0].id
                    call = body[0].value
                    if (
                        not isinstance(call.func, ast.Attribute)
                        or call.func.attr != "apply"
                        or len(call.args) != 1
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-011: runtime if requires one apply "
                            "assignment in each branch"
                        )
                    input_name = queue_reference(call.func.value, aliases)
                    argument, expression = _lambda(call.args[0])
                    return target, input_name, call, argument, expression

                false_arm = parse_arm(statement.orelse)
                true_arm = parse_arm(statement.body)
                if false_arm[0] != true_arm[0]:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-011: runtime if branches require one result name"
                    )
                if false_arm[1] != true_arm[1]:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-011: runtime if branches must consume one Queue"
                    )
                name = true_arm[0]
                input_name = true_arm[1]
                if name in by_name or name in collections:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-011: runtime if result requires one fresh name"
                    )
                incoming = by_name[input_name]

                condition_names: dict[str, str] = {}
                for node in ast.walk(statement.test):
                    if not isinstance(node, ast.Name):
                        continue
                    try:
                        referenced = queue_reference(node, aliases)
                    except QueueFrontendError:
                        continue
                    condition_names[node.id] = referenced
                if set(condition_names.values()) != {input_name}:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-011: runtime if condition must read its branch Queue"
                    )

                argument = "item"

                class QueueCondition(ast.NodeTransformer):
                    def visit_Name(self, node: ast.Name) -> ast.expr:
                        if condition_names.get(node.id) == input_name:
                            return ast.copy_location(ast.Name(id=argument), node)
                        return node

                condition = QueueCondition().visit(copy.deepcopy(statement.test))
                assert isinstance(condition, ast.expr)
                _, condition_type = _ExpressionEmitter(
                    payload_map,
                    argument,
                    incoming.payload,
                    bitfields=bitfield_map,
                ).emit(condition)
                if not _is_epoch_05_bool_compatible(condition_type):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-011: runtime if condition must lower to bool"
                    )
                conditional = len([route for route in routes if route.boolean_selector])
                false_input = f"{name}__if_false{conditional}_in"
                true_input = f"{name}__if_true{conditional}_in"
                false_output = f"{name}__if_false{conditional}"
                true_output = f"{name}__if_true{conditional}"
                for route_name in (false_input, true_input):
                    binding = QueueBinding(
                        route_name,
                        incoming.payload,
                        1,
                        1,
                        None,
                        scope=scope_path,
                        order=current_order,
                        route_output=True,
                    )
                    queues.append(binding)
                    by_name[route_name] = binding
                routes.append(
                    RouteBinding(
                        input_name,
                        (false_input, true_input),
                        argument,
                        condition,
                        1,
                        1,
                        scope_path,
                        current_order,
                        True,
                    )
                )

                for arm, arm_input, arm_output in (
                    (false_arm, false_input, false_output),
                    (true_arm, true_input, true_output),
                ):
                    branch_order = order
                    order += 1
                    binding = QueueBinding(
                        arm_output,
                        incoming.payload,
                        _positive_int(arm[2], "depth", 1),
                        _positive_int(arm[2], "latency", 1),
                        arm_input,
                        arm[3],
                        arm[4],
                        scope_path,
                        branch_order,
                    )
                    queues.append(binding)
                    by_name[arm_output] = binding

                merge_order = order
                order += 1
                output = QueueBinding(
                    name,
                    incoming.payload,
                    1,
                    1,
                    None,
                    scope=scope_path,
                    order=merge_order,
                    merge_output=True,
                )
                queues.append(output)
                by_name[name] = output
                merges.append(
                    MergeBinding(
                        (false_output, true_output),
                        name,
                        "priority",
                        1,
                        1,
                        scope_path,
                        merge_order,
                    )
                )
                continue
            if isinstance(statement, ast.With) and len(statement.items) == 1:
                item = statement.items[0]
                call = item.context_expr
                if (
                    item.optional_vars is None
                    and isinstance(call, ast.Call)
                    and call_name(call) == "scope"
                    and len(call.args) == 1
                    and isinstance(call.args[0], ast.Constant)
                    and type(call.args[0].value) is str
                    and call.args[0].value
                ):
                    path = (*scope_path, call.args[0].value)
                    if any(existing.path == path for existing in scopes):
                        raise QueueFrontendError("ACPY-QUEUE-004: duplicate scope path")
                    scopes.append(ScopeBinding(call.args[0].value, path, current_order))
                    visit(statement.body, path, aliases)
                    continue
            if isinstance(statement, ast.With) and len(statement.items) == 1:
                item = statement.items[0]
                call = item.context_expr
                if (
                    item.optional_vars is None
                    and isinstance(call, ast.Call)
                    and call_name(call) == "atomic"
                    and not call.args
                    and not call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-RULE-005: ac.atomic() was removed in contract epoch "
                        "0.5; express the transaction as @ac.rule"
                    )
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) in {"array", "map", "set"}
            ):
                name = statement.targets[0].id
                if (
                    name in by_name
                    or name in collections
                    or name in memory_by_name
                    or name in memory_arrays
                    or name in selected_memories
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-005: collection assignment requires one fresh name"
                    )
                call = statement.value
                is_memory_array = False
                if call_name(call) == "array" and len(call.args) == 2:
                    _, generator = _lambda(call.args[1])
                    is_memory_array = (
                        isinstance(generator, ast.Call)
                        and call_name(generator) == "memory"
                    )
                if is_memory_array:
                    extent = _static_int(call.args[0], {})
                    if extent is None or extent <= 0:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-015: memory array requires a positive "
                            "compile-time extent"
                        )
                    argument, generator = _lambda(call.args[1])
                    assert isinstance(generator, ast.Call)
                    pending: list[MemoryInstanceBinding] = []
                    for index in range(extent):
                        member_name = f"{name}__{index}"
                        if (
                            member_name in by_name
                            or member_name in collections
                            or member_name in memory_by_name
                            or member_name in memory_arrays
                            or member_name in selected_memories
                        ):
                            raise QueueFrontendError(
                                "ACPY-QUEUE-015: memory array element name "
                                "collides with an existing binding"
                            )
                        pending.append(
                            memory_instance_binding(
                                member_name,
                                generator,
                                scope_path,
                                current_order,
                                {argument: index},
                            )
                        )
                    configurations = {
                        (item.data_type, item.entries, item.init, item.latency)
                        for item in pending
                    }
                    if len(configurations) != 1:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-015: memory array elements must be homogeneous"
                        )
                    for instance in pending:
                        memory_instances.append(instance)
                        memory_by_name[instance.name] = instance
                    first = pending[0]
                    memory_arrays[name] = StaticMemoryArrayBinding(
                        name,
                        tuple(instance.name for instance in pending),
                        first.data_type,
                        first.entries,
                        first.init,
                        first.latency,
                        scope_path,
                        current_order,
                    )
                    continue
                collection = collection_binding(
                    name,
                    call,
                    scope_path,
                    current_order,
                    aliases,
                )
                assert collection is not None
                collections[name] = collection
                collection_bindings.append(
                    CollectionBinding(name, collection, scope_path, current_order)
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "select"
                and isinstance(statement.value.func.value, ast.Name)
                and statement.value.func.value.id in memory_arrays
            ):
                name = statement.targets[0].id
                if (
                    name in by_name
                    or name in collections
                    or name in memory_by_name
                    or name in memory_arrays
                    or name in selected_memories
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: selected memory requires one fresh name"
                    )
                call = statement.value
                if len(call.args) != 1 or any(
                    keyword.arg is None
                    or keyword.arg not in {"key", "depth", "latency"}
                    for keyword in call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory array select requires one request Queue"
                    )
                keys = [
                    keyword.value for keyword in call.keywords if keyword.arg == "key"
                ]
                if len(keys) != 1:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory array select requires one key lambda"
                    )
                array = memory_arrays[call.func.value.id]
                if len(scope_path) < len(array.scope) or (
                    scope_path[: len(array.scope)] != array.scope
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory array is only visible in its "
                        "declaration scope and descendants"
                    )
                input_name = queue_reference(call.args[0], aliases)
                incoming = by_name[input_name]
                argument, selector = _lambda(keys[0])
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                routed_inputs = tuple(
                    f"{name}__bank{index}_request"
                    for index in range(len(array.members))
                )
                for routed in routed_inputs:
                    if routed in by_name:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-015: selected memory synthetic Queue "
                            "name collides with an existing binding"
                        )
                    output = QueueBinding(
                        routed,
                        incoming.payload,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        route_output=True,
                    )
                    queues.append(output)
                    by_name[routed] = output
                routes.append(
                    RouteBinding(
                        input_name,
                        routed_inputs,
                        argument,
                        selector,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                selected_memories[name] = SelectedMemoryBinding(
                    name,
                    array.name,
                    input_name,
                    routed_inputs,
                    argument,
                    selector,
                    depth,
                    latency,
                    scope_path,
                    current_order,
                )
                continue
            if (
                isinstance(statement, ast.For)
                and isinstance(statement.target, ast.Name)
                and isinstance(statement.iter, ast.Call)
                and call_name(statement.iter) == "range"
                and len(statement.iter.args) == 1
                and not statement.iter.keywords
                and not statement.orelse
            ):
                extent = _static_int(statement.iter.args[0])
                if extent is None or not prove_within(
                    Constant(extent), 0, MAX_STATIC_EXPANSION
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-005: range extent must be a compile-time "
                        f"integer in [0, {MAX_STATIC_EXPANSION}]"
                    )

                class StaticIndex(ast.NodeTransformer):
                    def visit_Name(self, node: ast.Name) -> ast.expr:
                        if node.id == statement.target.id:
                            return ast.copy_location(ast.Constant(index), node)
                        return node

                for index in range(extent):
                    expanded = [
                        StaticIndex().visit(copy.deepcopy(body))
                        for body in statement.body
                    ]
                    visit(expanded, scope_path, aliases)
                continue
            if (
                isinstance(statement, ast.For)
                and isinstance(statement.target, ast.Name)
                and not statement.orelse
            ):
                collection = static_reference(statement.iter, aliases)
                if not isinstance(collection, StaticQueueCollection):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-005: compile-time for requires a static collection"
                    )
                for _, member in collection.members:
                    visit(
                        statement.body,
                        scope_path,
                        {**aliases, statement.target.id: member},
                    )
                continue
            if isinstance(statement, ast.While) and not statement.orelse:
                body = list(statement.body)
                break_test: ast.expr | None = None
                continue_test: ast.expr | None = None
                if (
                    body
                    and isinstance(body[0], ast.If)
                    and len(body[0].body) == 1
                    and isinstance(body[0].body[0], ast.Break)
                    and not body[0].orelse
                ):
                    break_test = body.pop(0).test
                if (
                    body
                    and isinstance(body[-1], ast.If)
                    and len(body[-1].body) == 1
                    and isinstance(body[-1].body[0], ast.Continue)
                    and not body[-1].orelse
                ):
                    continue_test = body.pop().test
                if (
                    len(body) != 1
                    or not isinstance(body[0], ast.Assign)
                    or len(body[0].targets) != 1
                    or not isinstance(body[0].targets[0], ast.Name)
                    or not isinstance(body[0].value, ast.Call)
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-007: runtime while requires optional break, "
                        "one Queue update, and optional tail continue"
                    )
                update_statement = body[0]
                variable = update_statement.targets[0].id
                call = update_statement.value
                incoming = by_name.get(variable)
                if (
                    incoming is None
                    or not isinstance(call.func, ast.Attribute)
                    or call.func.attr != "apply"
                    or not isinstance(call.func.value, ast.Name)
                    or call.func.value.id != variable
                    or len(call.args) != 1
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-007: runtime while must rebind one Queue through apply"
                    )
                argument, update = _lambda(call.args[0])

                class QueueCondition(ast.NodeTransformer):
                    def visit_Name(self, node: ast.Name) -> ast.expr:
                        if node.id == variable:
                            return ast.copy_location(ast.Name(id=argument), node)
                        return node

                condition = QueueCondition().visit(copy.deepcopy(statement.test))
                assert isinstance(condition, ast.expr)
                if break_test is not None:
                    rewritten_break = QueueCondition().visit(copy.deepcopy(break_test))
                    assert isinstance(rewritten_break, ast.expr)
                    condition = ast.BoolOp(
                        op=ast.And(),
                        values=[
                            condition,
                            ast.UnaryOp(op=ast.Not(), operand=rewritten_break),
                        ],
                    )
                if continue_test is not None:
                    rewritten_continue = QueueCondition().visit(
                        copy.deepcopy(continue_test)
                    )
                    if not isinstance(rewritten_continue, ast.expr):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-007: continue condition is invalid"
                        )
                    continue_probe = ast.UnaryOp(
                        op=ast.Not(), operand=rewritten_continue
                    )
                    condition = ast.BoolOp(
                        op=ast.And(),
                        values=[
                            condition,
                            ast.Compare(
                                left=continue_probe,
                                ops=[ast.Eq()],
                                comparators=[copy.deepcopy(continue_probe)],
                            ),
                        ],
                    )
                output_name = f"{variable}__feedback{len(feedbacks)}"
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                output = QueueBinding(
                    output_name,
                    incoming.payload,
                    depth,
                    latency,
                    None,
                    scope=scope_path,
                    order=current_order,
                    feedback_output=True,
                )
                queues.append(output)
                by_name[variable] = output
                feedbacks.append(
                    FeedbackBinding(
                        incoming.name,
                        output_name,
                        argument,
                        condition,
                        update,
                        depth,
                        latency,
                        1024,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "merge"
                and isinstance(statement.value.func.value, ast.Name)
                and statement.value.func.value.id in by_name
            ):
                name = statement.targets[0].id
                if name in by_name or name in collections:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-008: merge output requires one fresh name"
                    )
                call = statement.value
                operands = [call.func.value, *call.args]
                inputs = tuple(
                    queue_reference(operand, aliases) for operand in operands
                )
                if len(inputs) < 2:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-008: merge requires at least two Queues"
                    )
                payload = by_name[inputs[0]].payload
                if any(
                    not _types_equal_in_epoch_05(by_name[input_name].payload, payload)
                    for input_name in inputs
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-008: merge Queue payloads must match"
                    )
                policies = [
                    keyword.value
                    for keyword in call.keywords
                    if keyword.arg == "policy"
                ]
                if len(policies) > 1 or (
                    policies
                    and (
                        not isinstance(policies[0], ast.Constant)
                        or policies[0].value not in {"round_robin", "priority"}
                    )
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-008: merge policy must be round_robin or priority"
                    )
                policy = policies[0].value if policies else "round_robin"
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                output = QueueBinding(
                    name,
                    payload,
                    depth,
                    latency,
                    None,
                    scope=scope_path,
                    order=current_order,
                    merge_output=True,
                )
                queues.append(output)
                by_name[name] = output
                merges.append(
                    MergeBinding(
                        inputs,
                        name,
                        policy,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "reorder"
                and isinstance(statement.value.func.value, ast.Name)
                and not statement.value.args
            ):
                name = statement.targets[0].id
                if name in by_name or name in collections:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-013: reorder output requires one fresh name"
                    )
                call = statement.value
                incoming = by_name.get(call.func.value.id)
                if incoming is None:
                    raise QueueFrontendError("ACPY-QUEUE-013: reorder input is unbound")
                allowed_keywords = {"key", "capacity", "start", "depth", "latency"}
                if any(
                    keyword.arg is None or keyword.arg not in allowed_keywords
                    for keyword in call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-013: reorder has an unsupported keyword"
                    )
                keys = [
                    keyword.value for keyword in call.keywords if keyword.arg == "key"
                ]
                if len(keys) != 1:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-013: reorder requires one key lambda"
                    )
                argument, key = _lambda(keys[0])
                capacity = _positive_int(call, "capacity", 16)
                start = _nonnegative_int(call, "start", 0)
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                output = QueueBinding(
                    name,
                    incoming.payload,
                    depth,
                    latency,
                    None,
                    scope=scope_path,
                    order=current_order,
                    reorder_output=True,
                )
                queues.append(output)
                by_name[name] = output
                reorders.append(
                    ReorderBinding(
                        incoming.name,
                        name,
                        argument,
                        key,
                        capacity,
                        start,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "depend"
                and isinstance(statement.value.func.value, ast.Name)
                and not statement.value.args
            ):
                name = statement.targets[0].id
                if name in by_name or name in collections:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-014: dependency output requires one fresh name"
                    )
                call = statement.value
                incoming = by_name.get(call.func.value.id)
                if incoming is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-014: dependency input is unbound"
                    )
                allowed_keywords = {
                    "key",
                    "waits_for",
                    "resource",
                    "cost",
                    "capacity",
                    "resources",
                    "no_dependency",
                    "depth",
                    "latency",
                }
                if any(
                    keyword.arg is None or keyword.arg not in allowed_keywords
                    for keyword in call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-014: dependency has an unsupported keyword"
                    )
                policies: dict[str, ast.expr] = {}
                for policy in ("key", "waits_for", "resource", "cost"):
                    values = [
                        keyword.value
                        for keyword in call.keywords
                        if keyword.arg == policy
                    ]
                    if len(values) != 1:
                        raise QueueFrontendError(
                            f"ACPY-QUEUE-014: dependency requires one {policy} lambda"
                        )
                    policies[policy] = values[0]
                key_argument, key = _lambda(policies["key"])
                waits_argument, waits_for = _lambda(policies["waits_for"])
                resource_argument, resource = _lambda(policies["resource"])
                cost_argument, cost = _lambda(policies["cost"])
                if (
                    len(
                        {
                            key_argument,
                            waits_argument,
                            resource_argument,
                            cost_argument,
                        }
                    )
                    != 1
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-014: dependency lambdas require one argument name"
                    )
                capacity = _positive_int(call, "capacity", 16)
                resources = _positive_int(call, "resources", 1)
                no_dependency = _nonnegative_int(call, "no_dependency", 255)
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                output = QueueBinding(
                    name,
                    incoming.payload,
                    depth,
                    latency,
                    None,
                    scope=scope_path,
                    order=current_order,
                    dependency_output=True,
                )
                queues.append(output)
                by_name[name] = output
                dependencies.append(
                    DependencyBinding(
                        incoming.name,
                        name,
                        key_argument,
                        key,
                        waits_for,
                        resource,
                        cost,
                        capacity,
                        resources,
                        no_dependency,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "select"
                and isinstance(statement.value.func.value, ast.Name)
                and statement.value.func.value.id in collections
            ):
                name = statement.targets[0].id
                if name in by_name or name in collections:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-018: select output requires one fresh name"
                    )
                call = statement.value
                if len(call.args) != 1 or any(
                    keyword.arg is None
                    or keyword.arg not in {"key", "depth", "latency"}
                    for keyword in call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-018: select requires one control Queue"
                    )
                control = queue_reference(call.args[0], aliases)
                collection = collections[call.func.value.id]
                if any(not isinstance(member, str) for _, member in collection.members):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-018: select requires a flat Queue collection"
                    )
                inputs = tuple(
                    member
                    for _, member in collection.members
                    if isinstance(member, str)
                )
                if len(inputs) < 2 or control in inputs:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-018: select requires two unique data Queues"
                    )
                payload = by_name[inputs[0]].payload
                if any(
                    not _types_equal_in_epoch_05(by_name[input_name].payload, payload)
                    for input_name in inputs
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-018: select data Queue payloads must match"
                    )
                keys = [
                    keyword.value for keyword in call.keywords if keyword.arg == "key"
                ]
                if len(keys) != 1:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-018: select requires one key lambda"
                    )
                argument, selector = _lambda(keys[0])
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                output = QueueBinding(
                    name,
                    payload,
                    depth,
                    latency,
                    None,
                    scope=scope_path,
                    order=current_order,
                    select_output=True,
                )
                queues.append(output)
                by_name[name] = output
                selects.append(
                    SelectBinding(
                        control,
                        inputs,
                        name,
                        argument,
                        selector,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "credit"
                and isinstance(statement.value.func.value, ast.Name)
                and not statement.value.args
            ):
                name = statement.targets[0].id
                if name in by_name or name in collections:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-016: credit output requires one fresh name"
                    )
                call = statement.value
                incoming = by_name.get(call.func.value.id)
                if incoming is None:
                    raise QueueFrontendError("ACPY-QUEUE-016: credit input is unbound")
                allowed_keywords = {"cost", "credits", "depth", "latency"}
                if any(
                    keyword.arg is None or keyword.arg not in allowed_keywords
                    for keyword in call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-016: credit has an unsupported keyword"
                    )
                costs = [
                    keyword.value for keyword in call.keywords if keyword.arg == "cost"
                ]
                if len(costs) != 1:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-016: credit requires one cost lambda"
                    )
                argument, cost = _lambda(costs[0])
                credit_count = _positive_int(call, "credits", 16)
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                output = QueueBinding(
                    name,
                    incoming.payload,
                    depth,
                    latency,
                    None,
                    scope=scope_path,
                    order=current_order,
                    credit_output=True,
                )
                queues.append(output)
                by_name[name] = output
                credits.append(
                    CreditBinding(
                        incoming.name,
                        name,
                        argument,
                        cost,
                        credit_count,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "request"
                and isinstance(statement.value.func.value, ast.Name)
                and statement.value.func.value.id in selected_memories
                and not statement.value.args
            ):
                name = statement.targets[0].id
                if (
                    name in by_name
                    or name in collections
                    or name in memory_by_name
                    or name in memory_arrays
                    or name in selected_memories
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory request output requires one fresh name"
                    )
                call = statement.value
                selected_name = call.func.value.id
                selected = selected_memories[selected_name]
                if selected_name in consumed_selected_memories:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: selected memory may be requested only once"
                    )
                if selected.scope != scope_path:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: selected memory must be requested in the "
                        "same lexical scope"
                    )
                incoming = by_name[selected.input_name]
                array = memory_arrays[selected.array]
                (
                    argument,
                    address,
                    write,
                    data,
                    result_field,
                    depth,
                ) = memory_request_parameters(
                    call,
                    incoming,
                    array.data_type,
                    {"merge_policy", "merge_depth", "merge_latency"},
                )
                merge_policies = [
                    keyword.value
                    for keyword in call.keywords
                    if keyword.arg == "merge_policy"
                ]
                if len(merge_policies) > 1 or (
                    merge_policies
                    and (
                        not isinstance(merge_policies[0], ast.Constant)
                        or merge_policies[0].value not in {"priority", "round_robin"}
                    )
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: merge_policy must be priority or round_robin"
                    )
                merge_policy = merge_policies[0].value if merge_policies else "priority"
                merge_depth = _positive_int(call, "merge_depth", 1)
                merge_latency = _positive_int(call, "merge_latency", 1)
                response_names = tuple(
                    f"{name}__bank{index}" for index in range(len(array.members))
                )
                for instance_name, input_name, output_name in zip(
                    array.members,
                    selected.routed_inputs,
                    response_names,
                    strict=True,
                ):
                    if output_name in by_name:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-015: selected memory response Queue "
                            "name collides with an existing binding"
                        )
                    output = QueueBinding(
                        output_name,
                        incoming.payload,
                        depth,
                        1,
                        None,
                        scope=scope_path,
                        order=current_order,
                        memory_output=True,
                    )
                    queues.append(output)
                    by_name[output_name] = output
                    memory_requests.append(
                        MemoryRequestBinding(
                            instance_name,
                            input_name,
                            output_name,
                            argument,
                            address,
                            write,
                            data,
                            result_field,
                            depth,
                            scope_path,
                            current_order,
                        )
                    )
                merge_order = current_order + 1
                output = QueueBinding(
                    name,
                    incoming.payload,
                    merge_depth,
                    merge_latency,
                    None,
                    scope=scope_path,
                    order=merge_order,
                    merge_output=True,
                )
                queues.append(output)
                by_name[name] = output
                merges.append(
                    MergeBinding(
                        response_names,
                        name,
                        str(merge_policy),
                        merge_depth,
                        merge_latency,
                        scope_path,
                        merge_order,
                    )
                )
                consumed_selected_memories.add(selected_name)
                order += 1
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "request"
                and isinstance(statement.value.func.value, ast.Name)
                and len(statement.value.args) == 1
            ):
                name = statement.targets[0].id
                if name in by_name or name in collections:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory request output requires one fresh name"
                    )
                call = statement.value
                instance = memory_by_name.get(call.func.value.id)
                if instance is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory request instance is unbound"
                    )
                if len(scope_path) < len(instance.scope) or (
                    scope_path[: len(instance.scope)] != instance.scope
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory instance is only visible in its "
                        "declaration scope and descendants"
                    )
                incoming_name = queue_reference(call.args[0], aliases)
                incoming = by_name.get(incoming_name)
                if incoming is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory request input is unbound"
                    )
                (
                    argument,
                    address,
                    write,
                    data,
                    result_field,
                    depth,
                ) = memory_request_parameters(call, incoming, instance.data_type)
                output = QueueBinding(
                    name,
                    incoming.payload,
                    depth,
                    1,
                    None,
                    scope=scope_path,
                    order=current_order,
                    memory_output=True,
                )
                queues.append(output)
                by_name[name] = output
                memory_requests.append(
                    MemoryRequestBinding(
                        instance.name,
                        incoming.name,
                        name,
                        argument,
                        address,
                        write,
                        data,
                        result_field,
                        depth,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.value, ast.Call)
                and isinstance(statement.value.func, ast.Attribute)
                and statement.value.func.attr == "memory"
            ):
                raise QueueFrontendError(
                    "ACPY-QUEUE-015: Queue.memory was removed; declare "
                    "ac.memory(...) and connect it with instance.request(...)"
                )
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and isinstance(statement.value, ast.Call)
            ):
                name, call = statement.targets[0].id, statement.value
                if (
                    isinstance(call.func, ast.Name)
                    and call.func.id in recursive_helpers
                ):
                    if (
                        name in by_name
                        or name in collections
                        or len(call.args) != 2
                        or call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-020: recursive helper call is malformed"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                    extent = _static_int(call.args[1])
                    if extent is None or extent < 0 or extent > 1024:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-020: recursion depth must be a compile-time "
                            "integer in [0, 1024]"
                        )
                    helper = recursive_helpers[call.func.id]
                    incoming = by_name[input_name]
                    if extent == 0:
                        by_name[name] = incoming
                        continue
                    previous = incoming
                    for index in range(extent):
                        output_name = (
                            name if index + 1 == extent else f"{name}__rec{index}"
                        )
                        binding = QueueBinding(
                            output_name,
                            incoming.payload,
                            _positive_int(helper.apply_call, "depth", 1),
                            _positive_int(helper.apply_call, "latency", 1),
                            previous.name,
                            helper.argument,
                            copy.deepcopy(helper.expression),
                            scope_path,
                            current_order,
                        )
                        queues.append(binding)
                        by_name[output_name] = binding
                        previous = binding
                    continue
                if name in by_name or name in collections:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-001: queue assignment requires one fresh name"
                    )
                if call_name(call) == "source" and len(call.args) == 1:
                    binding = source_binding(name, call, scope_path, current_order)
                elif call_name(call) == "compute" and len(call.args) == 2:
                    if any(
                        keyword.arg is None
                        or keyword.arg not in {"depth", "latency", "rate"}
                        for keyword in call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-023: compute has an unsupported keyword"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                    incoming = by_name[input_name]
                    argument, expression = _lambda(call.args[1])
                    depth = _positive_int(call, "depth", 1)
                    rate = _positive_int(call, "rate", incoming.rate)
                    if rate > depth:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-025: Queue rate must not exceed depth"
                        )
                    binding = QueueBinding(
                        name,
                        incoming.payload,
                        depth,
                        _positive_int(call, "latency", 1),
                        incoming.name,
                        argument,
                        expression,
                        scope_path,
                        current_order,
                        provider="compute",
                        rate=rate,
                    )
                elif call_name(call) == "pipeline" and len(call.args) == 1:
                    if any(
                        keyword.arg is None
                        or keyword.arg not in {"stages", "depth", "rate"}
                        for keyword in call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: pipeline parameters are invalid"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                    incoming = by_name[input_name]
                    stages = _positive_int(call, "stages", 1)
                    depth = _positive_int(call, "depth", 1)
                    rate = _positive_int(call, "rate", incoming.rate)
                    if rate > depth:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-025: Queue rate must not exceed depth"
                        )
                    binding = QueueBinding(
                        name,
                        incoming.payload,
                        depth,
                        stages,
                        incoming.name,
                        "item",
                        ast.Name(id="item", ctx=ast.Load()),
                        scope_path,
                        current_order,
                        provider="pipeline",
                        rate=rate,
                    )
                elif call_name(call) == "merge":
                    if len(call.args) < 2 or any(
                        keyword.arg is None
                        or keyword.arg not in {"policy", "depth", "latency"}
                        for keyword in call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: merge requires two or more Queues and "
                            "static policy/depth/latency"
                        )
                    input_names = tuple(
                        queue_reference(argument, aliases) for argument in call.args
                    )
                    if len(set(input_names)) != len(input_names):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: merge inputs must be unique Queues"
                        )
                    payloads_used = {by_name[item].payload for item in input_names}
                    if len(payloads_used) != 1:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: merge inputs require one payload type"
                        )
                    depth = _positive_int(call, "depth", 1)
                    latency = _positive_int(call, "latency", 1)
                    policy = policy_value(call)
                    binding = QueueBinding(
                        name,
                        by_name[input_names[0]].payload,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        merge_output=True,
                    )
                    merges.append(
                        MergeBinding(
                            input_names,
                            name,
                            policy,
                            depth,
                            latency,
                            scope_path,
                            current_order,
                        )
                    )
                elif call_name(call) == "schedule":
                    if len(call.args) != 1 or any(
                        keyword.arg is None
                        or keyword.arg
                        not in {
                            "by",
                            "waits_for",
                            "resource",
                            "cost",
                            "entries",
                            "resources",
                            "no_dependency",
                            "depth",
                            "latency",
                        }
                        for keyword in call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: schedule parameters are invalid"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                    incoming = by_name[input_name]
                    argument = "item"
                    key = field_expression(keyword_value(call, "by"), incoming)
                    waits_for = field_expression(
                        keyword_value(call, "waits_for"), incoming
                    )
                    resource = field_expression(
                        keyword_value(call, "resource"), incoming
                    )
                    cost = field_expression(keyword_value(call, "cost"), incoming)
                    capacity = _positive_int(call, "entries", 16)
                    resources = _positive_int(call, "resources", 1)
                    no_dependency = _nonnegative_int(call, "no_dependency", 0)
                    depth = _positive_int(call, "depth", 1)
                    latency = _positive_int(call, "latency", 1)
                    binding = QueueBinding(
                        name,
                        incoming.payload,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        dependency_output=True,
                    )
                    dependencies.append(
                        DependencyBinding(
                            input_name,
                            name,
                            argument,
                            key,
                            waits_for,
                            resource,
                            cost,
                            capacity,
                            resources,
                            no_dependency,
                            depth,
                            latency,
                            scope_path,
                            current_order,
                            provider="schedule",
                        )
                    )
                elif call_name(call) == "engine":
                    if len(call.args) != 1 or any(
                        keyword.arg is None
                        or keyword.arg not in {"cost", "lanes", "depth", "latency"}
                        for keyword in call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: engine parameters are invalid"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                    incoming = by_name[input_name]
                    argument = "item"
                    cost = field_expression(keyword_value(call, "cost"), incoming)
                    lane_count = _positive_int(call, "lanes", 1)
                    depth = _positive_int(call, "depth", 1)
                    latency = _positive_int(call, "latency", 1)
                    binding = QueueBinding(
                        name,
                        incoming.payload,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        credit_output=True,
                    )
                    credits_binding = CreditBinding(
                        input_name,
                        name,
                        argument,
                        cost,
                        lane_count,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                        provider="engine",
                    )
                    credits.append(credits_binding)
                elif call_name(call) == "reorder":
                    if len(call.args) != 1 or any(
                        keyword.arg is None
                        or keyword.arg
                        not in {"by", "entries", "start", "depth", "latency"}
                        for keyword in call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: reorder parameters are invalid"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                    incoming = by_name[input_name]
                    argument = "item"
                    key = field_expression(keyword_value(call, "by"), incoming)
                    capacity = _positive_int(call, "entries", 16)
                    start = _nonnegative_int(call, "start", 0)
                    depth = _positive_int(call, "depth", 1)
                    latency = _positive_int(call, "latency", 1)
                    binding = QueueBinding(
                        name,
                        incoming.payload,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        reorder_output=True,
                    )
                    reorders.append(
                        ReorderBinding(
                            input_name,
                            name,
                            argument,
                            key,
                            capacity,
                            start,
                            depth,
                            latency,
                            scope_path,
                            current_order,
                        )
                    )
                elif call_name(call) in rule_definitions:
                    definition = rule_definitions[call_name(call)]
                    if definition.expression is None:
                        raise QueueFrontendError(
                            "ACPY-RULE-006: outputless rule call must be a "
                            "standalone statement"
                        )
                    table: TableBinding | None = None
                    variable: VarStateBinding | None = None
                    multi_state_writes: tuple[RuleStateWriteBinding, ...] = ()
                    multi_state_reads: tuple[RuleStateReadBinding, ...] = ()
                    multi_state_locals: tuple[RuleLocalBinding, ...] = ()
                    multi_state_finds: tuple[RuleFindBinding, ...] = ()
                    multi_state_owners: tuple[RuleStateOwnerBinding, ...] = ()
                    multi_state_result_type: ValueType | None = None
                    if definition.state_writes:
                        state_count = len(definition.state_arguments)
                        if (
                            len(call.args) != state_count + len(definition.arguments)
                            or call.keywords
                        ):
                            raise QueueFrontendError(
                                "ACPY-RULE-008: multi-state rule invocation "
                                "requires every persistent value followed by "
                                "one Queue per payload parameter"
                            )
                        owners: dict[str, VarStateBinding] = {}
                        for argument, value in zip(
                            definition.state_arguments,
                            call.args[:state_count],
                            strict=True,
                        ):
                            if (
                                not isinstance(value, ast.Name)
                                or value.id not in variable_by_name
                            ):
                                raise QueueFrontendError(
                                    "ACPY-RULE-008: persistent rule parameters "
                                    "must precede payload parameters and bind "
                                    "persistent variables"
                                )
                            owners[argument] = variable_by_name[value.id]
                        multi_state_owners = tuple(
                            RuleStateOwnerBinding(
                                owner.name,
                                argument,
                                owner.value_type,
                                owner.entries,
                            )
                            for argument, owner in owners.items()
                        )
                        for find in definition.finds:
                            owner = owners[find.argument]
                            if owner.entries == 1 or owner.entries > 64:
                                raise QueueFrontendError(
                                    "ACPY-RULE-009: find requires a persistent "
                                    "list with 2..64 entries"
                                )
                        writes: list[RuleStateWriteBinding] = []
                        for write in definition.state_writes:
                            owner = owners[write.argument]
                            if (write.index is None) != (owner.entries == 1):
                                raise QueueFrontendError(
                                    "ACPY-RULE-008: scalar/list assignment does "
                                    "not match persistent variable shape"
                                )
                            writes.append(
                                RuleStateWriteBinding(
                                    owner.name,
                                    write.argument,
                                    owner.value_type,
                                    owner.entries,
                                    copy.deepcopy(write.index),
                                    copy.deepcopy(write.value),
                                    copy.deepcopy(write.guard),
                                    write.guard_negated,
                                )
                            )
                        multi_state_writes = tuple(writes)
                        reads: list[RuleStateReadBinding] = []
                        for read in definition.state_reads:
                            owner = owners[read.argument]
                            if owner.entries == 1:
                                raise QueueFrontendError(
                                    "ACPY-RULE-008: indexed state observation "
                                    "requires a persistent list"
                                )
                            reads.append(
                                RuleStateReadBinding(
                                    read.name,
                                    owner.name,
                                    read.argument,
                                    owner.value_type,
                                    owner.entries,
                                    copy.deepcopy(read.index),
                                )
                            )
                        multi_state_reads = tuple(reads)
                        multi_state_locals = tuple(
                            RuleLocalBinding(local.name, copy.deepcopy(local.value))
                            for local in definition.locals
                        )
                        finds: list[RuleFindBinding] = []
                        for find in definition.finds:
                            owner = owners[find.argument]
                            if owner.entries == 1 or owner.entries > 64:
                                raise QueueFrontendError(
                                    "ACPY-RULE-009: find requires a persistent "
                                    "list with 2..64 entries"
                                )
                            finds.append(
                                RuleFindBinding(
                                    find.name,
                                    owner.name,
                                    find.argument,
                                    owner.value_type,
                                    owner.entries,
                                    find.predicate_argument,
                                    copy.deepcopy(find.predicate),
                                    find.key_argument,
                                    copy.deepcopy(find.key),
                                )
                            )
                        multi_state_finds = tuple(finds)
                        input_names = tuple(
                            queue_reference(argument, aliases)
                            for argument in call.args[state_count:]
                        )
                        if not input_names:
                            multi_state_result_type = (
                                multi_state_finds[0].value_type
                                if multi_state_finds
                                else multi_state_reads[0].value_type
                                if multi_state_reads
                                else multi_state_writes[0].value_type
                            )
                    elif definition.var_argument is not None:
                        if (
                            len(call.args) != len(definition.arguments) + 1
                            or call.keywords
                            or not isinstance(call.args[0], ast.Name)
                            or call.args[0].id not in variable_by_name
                        ):
                            raise QueueFrontendError(
                                "ACPY-RULE-003: variable rule invocation requires "
                                "one persistent variable followed by one Queue "
                                "per payload parameter"
                            )
                        variable = variable_by_name[call.args[0].id]
                        if variable.entries != 1:
                            raise QueueFrontendError(
                                "ACPY-RULE-003: scalar variable assignment cannot "
                                "target a persistent list"
                            )
                        input_names = tuple(
                            queue_reference(argument, aliases)
                            for argument in call.args[1:]
                        )
                    elif definition.table_argument is None:
                        if len(call.args) != len(definition.arguments) or call.keywords:
                            raise QueueFrontendError(
                                "ACPY-RULE-003: pure rule invocation requires "
                                "one Queue per rule parameter"
                            )
                        input_names = tuple(
                            queue_reference(argument, aliases) for argument in call.args
                        )
                        if len(set(input_names)) != len(input_names):
                            raise QueueFrontendError(
                                "ACPY-RULE-003: each multi-input rule parameter "
                                "requires a distinct Queue"
                            )
                    else:
                        owner_name = (
                            call.args[0].id
                            if call.args and isinstance(call.args[0], ast.Name)
                            else None
                        )
                        if (
                            len(call.args) != len(definition.arguments) + 1
                            or call.keywords
                            or owner_name is None
                            or (
                                owner_name not in table_by_name
                                and owner_name not in variable_by_name
                            )
                        ):
                            raise QueueFrontendError(
                                "ACPY-RULE-003: stateful rule invocation requires "
                                "one indexed persistent value followed by one "
                                "Queue per payload "
                                "parameter"
                            )
                        if owner_name in table_by_name:
                            table = table_by_name[owner_name]
                        else:
                            variable = variable_by_name[owner_name]
                            if variable.entries == 1:
                                raise QueueFrontendError(
                                    "ACPY-RULE-003: indexed state rule requires a "
                                    "persistent list"
                                )
                        input_names = tuple(
                            queue_reference(argument, aliases)
                            for argument in call.args[1:]
                        )
                        if len(set(input_names)) != len(input_names):
                            raise QueueFrontendError(
                                "ACPY-RULE-003: each stateful rule payload "
                                "parameter requires a distinct Queue"
                            )
                    incoming_queues = tuple(by_name[item] for item in input_names)
                    incoming = incoming_queues[0] if incoming_queues else None
                    if (
                        table is not None
                        and incoming is not None
                        and not _types_equal_in_epoch_05(
                            table.entry_type, incoming.payload
                        )
                    ):
                        raise QueueFrontendError(
                            "ACPY-RULE-004: stateful rule Queue payload must "
                            "match the Table Entry type"
                        )
                    if (
                        variable is not None
                        and incoming is not None
                        and not _types_equal_in_epoch_05(
                            variable.value_type, incoming.payload
                        )
                    ):
                        raise QueueFrontendError(
                            "ACPY-RULE-004: stateful rule Queue payload must "
                            "match the persistent value type"
                        )
                    indexed_variable = variable is not None and variable.entries != 1
                    binding = QueueBinding(
                        name,
                        (
                            variable.value_type
                            if variable is not None
                            else (
                                table.entry_type
                                if table is not None
                                else (
                                    incoming.payload
                                    if incoming is not None
                                    else multi_state_result_type
                                )
                            )
                        ),
                        1,
                        1,
                        None if incoming is None else incoming.name,
                        definition.arguments[0] if definition.arguments else "item",
                        copy.deepcopy(definition.expression),
                        scope_path,
                        current_order,
                        rule_name=definition.name,
                        rule_source_line=definition.source_line,
                        rule_source_column=definition.source_column,
                        rule_table=None if table is None else table.name,
                        rule_table_index=(
                            copy.deepcopy(definition.table_index)
                            if table is not None
                            else None
                        ),
                        rule_table_value=(
                            copy.deepcopy(definition.table_value)
                            if table is not None
                            else None
                        ),
                        rule_write_fields=(
                            complete_value_fields(variable.value_type)
                            if indexed_variable
                            else (
                                ()
                                if table is None
                                else normalized_write_fields(
                                    table, definition.table_value, ()
                                )
                            )
                        ),
                        rule_table_read_name=(
                            definition.table_read_name if table is not None else None
                        ),
                        rule_table_read_index=(
                            copy.deepcopy(definition.table_read_index)
                            if table is not None
                            else None
                        ),
                        rule_input_names=input_names,
                        rule_arguments=definition.arguments,
                        rule_payloads=tuple(item.payload for item in incoming_queues),
                        rule_var=None if variable is None else variable.name,
                        rule_var_argument=(
                            definition.table_argument
                            if indexed_variable
                            else definition.var_argument
                        ),
                        rule_var_value=copy.deepcopy(
                            definition.table_value
                            if indexed_variable
                            else definition.var_value
                        ),
                        rule_var_index=(
                            copy.deepcopy(definition.table_index)
                            if indexed_variable
                            else None
                        ),
                        rule_var_read_name=(
                            definition.table_read_name if indexed_variable else None
                        ),
                        rule_var_read_index=(
                            copy.deepcopy(definition.table_read_index)
                            if indexed_variable
                            else None
                        ),
                        rule_guard=copy.deepcopy(definition.guard),
                        rule_effect_guard=copy.deepcopy(definition.effect_guard),
                        rule_output_guard=copy.deepcopy(definition.output_guard),
                        rule_state_writes=multi_state_writes,
                        rule_state_reads=multi_state_reads,
                        rule_locals=multi_state_locals,
                        rule_finds=multi_state_finds,
                        rule_state_owners=multi_state_owners,
                    )
                elif call_name(call) == "table":
                    raise QueueFrontendError(
                        "ACPY-TABLE-000: legacy ac.table(value, ...) was removed; "
                        "use ac.memory for request/response memory or "
                        "ac.table[entries, Entry](init=0) for state Table"
                    )
                elif (
                    isinstance(call.func, ast.Attribute) and call.func.attr == "firing"
                ):
                    raise QueueFrontendError(
                        "ACPY-RULE-005: Queue.firing() was removed in contract "
                        "epoch 0.5; express the transaction as @ac.rule"
                    )
                elif (
                    isinstance(call.func, ast.Attribute)
                    and call.func.attr == "apply"
                    and isinstance(call.func.value, ast.Name)
                    and len(call.args) == 1
                ):
                    input_name = call.func.value.id
                    incoming = by_name.get(input_name)
                    if incoming is None:
                        raise QueueFrontendError(
                            f"ACPY-QUEUE-001: input queue {input_name!r} is unbound"
                        )
                    argument, expression = _lambda(call.args[0])
                    binding = QueueBinding(
                        name,
                        incoming.payload,
                        _positive_int(call, "depth", 1),
                        _positive_int(call, "latency", 1),
                        incoming.name,
                        argument,
                        expression,
                        scope_path,
                        current_order,
                    )
                else:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-001: unsupported queue-producing call"
                    )
                queues.append(binding)
                by_name[name] = binding
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], (ast.Tuple, ast.List))
                and all(
                    isinstance(item, ast.Name) for item in statement.targets[0].elts
                )
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) == "barrier"
            ):
                call = statement.value
                if any(
                    keyword.arg is None or keyword.arg not in {"depth", "latency"}
                    for keyword in call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-017: barrier has an unsupported keyword"
                    )
                method_style = (
                    isinstance(call.func, ast.Attribute)
                    and isinstance(call.func.value, ast.Name)
                    and call.func.value.id in by_name
                )
                operands = (
                    [call.func.value, *call.args] if method_style else list(call.args)
                )
                inputs = tuple(
                    queue_reference(operand, aliases) for operand in operands
                )
                outputs = tuple(item.id for item in statement.targets[0].elts)
                if len(inputs) < 2 or len(outputs) != len(inputs):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-017: barrier requires matching input/output arity"
                    )
                if len(set(inputs)) != len(inputs):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-017: barrier inputs must be unique Queues"
                    )
                if len(set(outputs)) != len(outputs) or any(
                    output in by_name or output in collections for output in outputs
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-017: barrier outputs require fresh tuple names"
                    )
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                for input_name, output_name in zip(inputs, outputs, strict=True):
                    output = QueueBinding(
                        output_name,
                        by_name[input_name].payload,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        barrier_output=True,
                    )
                    queues.append(output)
                    by_name[output_name] = output
                barriers.append(
                    BarrierBinding(
                        inputs,
                        outputs,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], (ast.Tuple, ast.List))
                and all(
                    isinstance(item, ast.Name) for item in statement.targets[0].elts
                )
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) == "route"
            ):
                call = statement.value
                method_style = (
                    isinstance(call.func, ast.Attribute)
                    and isinstance(call.func.value, ast.Name)
                    and call.func.value.id in by_name
                )
                if method_style:
                    assert isinstance(call.func, ast.Attribute)
                    assert isinstance(call.func.value, ast.Name)
                    input_name = call.func.value.id
                    if call.args:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-006: method route takes no positional arguments"
                        )
                else:
                    if len(call.args) != 1:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: route requires one input Queue"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                incoming = by_name.get(input_name)
                if incoming is None:
                    raise QueueFrontendError(
                        f"ACPY-QUEUE-001: input queue {input_name!r} is unbound"
                    )
                output_count = _positive_int(call, "outputs", 0)
                names = tuple(item.id for item in statement.targets[0].elts)
                if output_count != len(names) or len(set(names)) != len(names):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-006: route outputs must match fresh tuple names"
                    )
                if method_style:
                    key = [
                        keyword.value
                        for keyword in call.keywords
                        if keyword.arg == "key"
                    ]
                    if len(key) != 1:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-006: route requires one key lambda"
                        )
                    argument, selector = _lambda(key[0])
                else:
                    argument = "item"
                    selector = field_expression(
                        keyword_value(call, "by"), incoming, argument
                    )
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                for name in names:
                    if name in by_name:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-006: route output name is already bound"
                        )
                    output = QueueBinding(
                        name,
                        incoming.payload,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        route_output=True,
                    )
                    queues.append(output)
                    by_name[name] = output
                routes.append(
                    RouteBinding(
                        incoming.name,
                        names,
                        argument,
                        selector,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], (ast.Tuple, ast.List))
                and all(
                    isinstance(item, ast.Name) for item in statement.targets[0].elts
                )
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) == "fork"
            ):
                call = statement.value
                method_style = (
                    isinstance(call.func, ast.Attribute)
                    and isinstance(call.func.value, ast.Name)
                    and call.func.value.id in by_name
                )
                if method_style:
                    assert isinstance(call.func, ast.Attribute)
                    assert isinstance(call.func.value, ast.Name)
                    if call.args:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-012: method fork takes no positional arguments"
                        )
                    input_name = call.func.value.id
                else:
                    if len(call.args) != 1:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-024: fork requires one input Queue"
                        )
                    input_name = queue_reference(call.args[0], aliases)
                incoming = by_name.get(input_name)
                if incoming is None:
                    raise QueueFrontendError("ACPY-QUEUE-012: fork input is unbound")
                output_count = _positive_int(call, "outputs", 0)
                names = tuple(item.id for item in statement.targets[0].elts)
                if output_count != len(names) or len(names) < 2:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-012: fork outputs must match tuple arity"
                    )
                depth = _positive_int(call, "depth", 1)
                latency = _positive_int(call, "latency", 1)
                for name in names:
                    if name in by_name:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-012: fork output name is already bound"
                        )
                    output = QueueBinding(
                        name,
                        incoming.payload,
                        depth,
                        latency,
                        None,
                        scope=scope_path,
                        order=current_order,
                        route_output=True,
                    )
                    queues.append(output)
                    by_name[name] = output
                forks.append(
                    ForkBinding(
                        incoming.name,
                        names,
                        depth,
                        latency,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Expr)
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) == "expect"
                and len(statement.value.args) == 1
            ):
                call = statement.value
                if any(
                    keyword.arg is None or keyword.arg not in {"predicate", "message"}
                    for keyword in call.keywords
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-021: expect has an unsupported keyword"
                    )
                predicates = [
                    keyword.value
                    for keyword in call.keywords
                    if keyword.arg == "predicate"
                ]
                messages = [
                    keyword.value
                    for keyword in call.keywords
                    if keyword.arg == "message"
                ]
                if len(predicates) != 1 or len(messages) != 1:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-021: expect requires predicate and message"
                    )
                if (
                    not isinstance(messages[0], ast.Constant)
                    or type(messages[0].value) is not str
                    or not messages[0].value
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-021: expect message must be a static string"
                    )
                argument, predicate = _lambda(predicates[0])
                expectations.append(
                    ExpectBinding(
                        queue_reference(call.args[0], aliases),
                        argument,
                        predicate,
                        messages[0].value,
                        scope_path,
                        current_order,
                    )
                )
                continue
            if (
                isinstance(statement, ast.Expr)
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) in rule_definitions
            ):
                call = statement.value
                definition = rule_definitions[call_name(call)]
                if definition.expression is not None:
                    raise QueueFrontendError(
                        "ACPY-RULE-006: value-returning rule call must be assigned"
                    )
                if definition.state_writes:
                    state_count = len(definition.state_arguments)
                    if (
                        len(call.args) != state_count + len(definition.arguments)
                        or call.keywords
                    ):
                        raise QueueFrontendError(
                            "ACPY-RULE-008: outputless multi-state rule requires "
                            "every persistent value followed by its payload "
                            "Queues"
                        )
                    owners: dict[str, VarStateBinding] = {}
                    for argument, value in zip(
                        definition.state_arguments,
                        call.args[:state_count],
                        strict=True,
                    ):
                        if (
                            not isinstance(value, ast.Name)
                            or value.id not in variable_by_name
                        ):
                            raise QueueFrontendError(
                                "ACPY-RULE-008: persistent rule parameters "
                                "must precede payload parameters and bind "
                                "persistent variables"
                            )
                        owners[argument] = variable_by_name[value.id]
                    for find in definition.finds:
                        owner = owners[find.argument]
                        if owner.entries == 1 or owner.entries > 64:
                            raise QueueFrontendError(
                                "ACPY-RULE-009: find requires a persistent "
                                "list with 2..64 entries"
                            )
                    writes: list[RuleStateWriteBinding] = []
                    for write in definition.state_writes:
                        owner = owners[write.argument]
                        if (write.index is None) != (owner.entries == 1):
                            raise QueueFrontendError(
                                "ACPY-RULE-008: scalar/list assignment does not "
                                "match persistent variable shape"
                            )
                        writes.append(
                            RuleStateWriteBinding(
                                owner.name,
                                write.argument,
                                owner.value_type,
                                owner.entries,
                                copy.deepcopy(write.index),
                                copy.deepcopy(write.value),
                                copy.deepcopy(write.guard),
                                write.guard_negated,
                            )
                        )
                    reads: list[RuleStateReadBinding] = []
                    for read in definition.state_reads:
                        owner = owners[read.argument]
                        reads.append(
                            RuleStateReadBinding(
                                read.name,
                                owner.name,
                                read.argument,
                                owner.value_type,
                                owner.entries,
                                copy.deepcopy(read.index),
                            )
                        )
                    finds: list[RuleFindBinding] = []
                    for find in definition.finds:
                        owner = owners[find.argument]
                        if owner.entries == 1 or owner.entries > 64:
                            raise QueueFrontendError(
                                "ACPY-RULE-009: find requires a persistent "
                                "list with 2..64 entries"
                            )
                        finds.append(
                            RuleFindBinding(
                                find.name,
                                owner.name,
                                find.argument,
                                owner.value_type,
                                owner.entries,
                                find.predicate_argument,
                                copy.deepcopy(find.predicate),
                                find.key_argument,
                                copy.deepcopy(find.key),
                            )
                        )
                    input_names = tuple(
                        queue_reference(argument, aliases)
                        for argument in call.args[state_count:]
                    )
                    incoming_queues = tuple(by_name[item] for item in input_names)
                    effect_rules.append(
                        QueueBinding(
                            f"{definition.name}__effect_{current_order}",
                            writes[0].value_type,
                            1,
                            1,
                            None if not incoming_queues else incoming_queues[0].name,
                            (
                                definition.arguments[0]
                                if definition.arguments
                                else "item"
                            ),
                            None,
                            scope_path,
                            current_order,
                            rule_name=definition.name,
                            rule_source_line=definition.source_line,
                            rule_source_column=definition.source_column,
                            rule_input_names=input_names,
                            rule_arguments=definition.arguments,
                            rule_payloads=tuple(
                                item.payload for item in incoming_queues
                            ),
                            rule_has_output=False,
                            rule_guard=copy.deepcopy(definition.guard),
                            rule_effect_guard=copy.deepcopy(definition.effect_guard),
                            rule_output_guard=copy.deepcopy(definition.output_guard),
                            rule_state_writes=tuple(writes),
                            rule_state_reads=tuple(reads),
                            rule_locals=tuple(
                                RuleLocalBinding(local.name, copy.deepcopy(local.value))
                                for local in definition.locals
                            ),
                            rule_finds=tuple(finds),
                            rule_state_owners=tuple(
                                RuleStateOwnerBinding(
                                    owner.name,
                                    argument,
                                    owner.value_type,
                                    owner.entries,
                                )
                                for argument, owner in owners.items()
                            ),
                        )
                    )
                    continue
                if definition.table_argument is None:
                    raise QueueFrontendError(
                        "ACPY-RULE-006: outputless rule must update indexed state"
                    )
                owner_name = (
                    call.args[0].id
                    if call.args and isinstance(call.args[0], ast.Name)
                    else None
                )
                if (
                    len(call.args) != len(definition.arguments) + 1
                    or call.keywords
                    or owner_name is None
                    or (
                        owner_name not in table_by_name
                        and owner_name not in variable_by_name
                    )
                ):
                    raise QueueFrontendError(
                        "ACPY-RULE-006: outputless state rule requires one "
                        "indexed persistent value followed by one Queue per "
                        "payload parameter"
                    )
                table = table_by_name.get(owner_name)
                variable = variable_by_name.get(owner_name)
                if variable is not None and variable.entries == 1:
                    raise QueueFrontendError(
                        "ACPY-RULE-006: indexed state rule requires a persistent list"
                    )
                input_names = tuple(
                    queue_reference(argument, aliases) for argument in call.args[1:]
                )
                if len(set(input_names)) != len(input_names):
                    raise QueueFrontendError(
                        "ACPY-RULE-006: each payload parameter requires a distinct "
                        "Queue"
                    )
                incoming_queues = tuple(by_name[item] for item in input_names)
                incoming = incoming_queues[0]
                value_type = (
                    table.entry_type if table is not None else variable.value_type
                )
                if table is not None and not _types_equal_in_epoch_05(
                    incoming.payload, value_type
                ):
                    raise QueueFrontendError(
                        "ACPY-RULE-004: stateful rule primary Queue payload must "
                        "match the persistent value type"
                    )
                effect_rules.append(
                    QueueBinding(
                        f"{definition.name}__effect_{current_order}",
                        value_type,
                        1,
                        1,
                        incoming.name,
                        definition.arguments[0],
                        None,
                        scope_path,
                        current_order,
                        rule_name=definition.name,
                        rule_source_line=definition.source_line,
                        rule_source_column=definition.source_column,
                        rule_table=None if table is None else table.name,
                        rule_table_index=(
                            copy.deepcopy(definition.table_index)
                            if table is not None
                            else None
                        ),
                        rule_table_value=(
                            copy.deepcopy(definition.table_value)
                            if table is not None
                            else None
                        ),
                        rule_write_fields=(
                            normalized_write_fields(table, definition.table_value, ())
                            if table is not None
                            else complete_value_fields(value_type)
                        ),
                        rule_table_read_name=(
                            definition.table_read_name if table is not None else None
                        ),
                        rule_table_read_index=(
                            copy.deepcopy(definition.table_read_index)
                            if table is not None
                            else None
                        ),
                        rule_input_names=input_names,
                        rule_arguments=definition.arguments,
                        rule_payloads=tuple(item.payload for item in incoming_queues),
                        rule_var=None if variable is None else variable.name,
                        rule_var_argument=(
                            definition.table_argument if variable is not None else None
                        ),
                        rule_var_value=(
                            copy.deepcopy(definition.table_value)
                            if variable is not None
                            else None
                        ),
                        rule_var_index=(
                            copy.deepcopy(definition.table_index)
                            if variable is not None
                            else None
                        ),
                        rule_var_read_name=(
                            definition.table_read_name if variable is not None else None
                        ),
                        rule_var_read_index=(
                            copy.deepcopy(definition.table_read_index)
                            if variable is not None
                            else None
                        ),
                        rule_has_output=False,
                        rule_guard=copy.deepcopy(definition.guard),
                        rule_effect_guard=copy.deepcopy(definition.effect_guard),
                        rule_output_guard=copy.deepcopy(definition.output_guard),
                    )
                )
                continue
            if (
                isinstance(statement, ast.Expr)
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) == "observe"
                and len(statement.value.args) == 1
            ):
                name = queue_reference(statement.value.args[0], aliases)
                observations.append(
                    ObservationBinding(
                        name, f"observe_{current_order}", scope_path, current_order
                    )
                )
                continue
            if (
                isinstance(statement, ast.Expr)
                and isinstance(statement.value, ast.Call)
                and call_name(statement.value) == "sink"
                and len(statement.value.args) == 1
            ):
                name = queue_reference(statement.value.args[0], aliases)
                sinks.append(SinkBinding(name, scope_path, current_order))
                continue
            if isinstance(statement, ast.Return):
                if statement.value is None:
                    if result_payloads not in {None, ()}:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-026: typed system results must be returned"
                        )
                    continue
                values = (
                    tuple(statement.value.elts)
                    if isinstance(statement.value, (ast.Tuple, ast.List))
                    else (statement.value,)
                )
                returned = tuple(queue_reference(value, aliases) for value in values)
                if result_payloads is not None:
                    if len(returned) != len(result_payloads):
                        raise QueueFrontendError(
                            "ACPY-QUEUE-026: system return arity does not match "
                            "its annotation"
                        )
                    for index, (queue_name, expected_payload) in enumerate(
                        zip(returned, result_payloads, strict=True)
                    ):
                        if not _types_equal_in_epoch_05(
                            by_name[queue_name].payload, expected_payload
                        ):
                            raise QueueFrontendError(
                                "ACPY-QUEUE-026: system return "
                                f"{index} payload does not match its annotation"
                            )
                for index, queue_name in enumerate(returned):
                    sinks.append(
                        SinkBinding(queue_name, scope_path, current_order + index)
                    )
                order += len(returned) - 1
                continue
            raise QueueFrontendError(
                f"ACPY-QUEUE-001: unsupported statement {type(statement).__name__}"
            )

    visit(function.body, ())
    unused_selected = sorted(set(selected_memories) - consumed_selected_memories)
    if unused_selected:
        raise QueueFrontendError(
            "ACPY-QUEUE-015: selected memory is not requested: "
            + ", ".join(repr(name) for name in unused_selected)
        )
    requests_by_instance: dict[str, list[MemoryRequestBinding]] = {}
    for request in memory_requests:
        requests_by_instance.setdefault(request.instance, []).append(request)
    for instance in memory_instances:
        endpoints = requests_by_instance.get(instance.name, [])
        if not endpoints:
            raise QueueFrontendError(
                f"ACPY-QUEUE-015: memory instance {instance.name!r} is not connected"
            )
        payload_types = {by_name[endpoint.input_name].payload for endpoint in endpoints}
        if len(payload_types) != 1:
            raise QueueFrontendError(
                "ACPY-QUEUE-015: all endpoints of one memory require one payload struct"
            )
    for table in tables:
        endpoint_count = (
            sum(read.table == table.name for read in table_reads)
            + sum(write.table == table.name for write in table_writes)
            + sum(write.table == table.name for write in masked_table_writes)
            + sum(candidate.table == table.name for candidate in candidates)
            + sum(queue.rule_table == table.name for queue in queues)
        )
        if endpoint_count == 0:
            raise QueueFrontendError(
                f"ACPY-TABLE-005: table {table.name!r} requires a read/write endpoint"
            )
    for slot in slots:
        if not any(release.slot == slot.name for release in slot_releases):
            raise QueueFrontendError(
                f"ACPY-SLOT-002: slot {slot.name!r} requires one release endpoint"
            )
    if not queues or (not sinks and not effect_rules):
        raise QueueFrontendError(
            "ACPY-QUEUE-001: a queue system requires an external value and a "
            "consuming rule or result boundary"
        )
    return QueueProgram(
        system,
        payloads,
        enums,
        bitfields,
        tuple(queues),
        tuple(effect_rules),
        tuple(scopes),
        tuple(routes),
        tuple(forks),
        tuple(feedbacks),
        tuple(merges),
        tuple(reorders),
        tuple(dependencies),
        tuple(credits),
        tuple(barriers),
        tuple(selects),
        tuple(memory_instances),
        tuple(memory_requests),
        tuple(memories),
        tuple(variables),
        tuple(tables),
        tuple(table_reads),
        tuple(table_writes),
        tuple(masked_table_writes),
        tuple(slots),
        tuple(slot_releases),
        tuple(candidates),
        tuple(selections),
        tuple(collection_bindings),
        tuple(observations),
        tuple(expectations),
        tuple(sinks),
        specialization_fingerprint,
    )


@dataclass(frozen=True, slots=True)
class _ExpressionFact:
    value_type: ValueType
    constraint: Constraint


class _ExpressionEmitter:
    def __init__(
        self,
        payloads: dict[str, Payload],
        argument: str,
        payload: ValueType,
        *,
        root_name: str = "item",
        root_values: Mapping[str, tuple[str, ValueType]] | None = None,
        prefix: str = "",
        table_views: Mapping[str, tuple[str, ast.expr, ValueType]] | None = None,
        slot_views: Mapping[str, tuple[str, ValueType]] | None = None,
        candidates: Mapping[str, CandidateSetBinding] | None = None,
        selections: Mapping[str, SelectionBinding] | None = None,
        candidate_values: Mapping[str, tuple[str, ValueType]] | None = None,
        selection_values: Mapping[str, tuple[str, ValueType, str, ValueType]]
        | None = None,
        find_values: Mapping[
            str,
            tuple[str, ValueType, str, ValueType, str, ValueType, str | None],
        ]
        | None = None,
        state_views: Mapping[str, tuple[str, ValueType, int]] | None = None,
        table_domains: Mapping[str, tuple[ValueType, int]] | None = None,
        bitfields: Mapping[str, BitfieldLayout] | None = None,
    ) -> None:
        self.payloads = payloads
        self.enum_types: dict[str, EnumType] = {}

        def collect_enums(descriptor: ValueType) -> None:
            if isinstance(descriptor, EnumType):
                existing = self.enum_types.get(descriptor.name)
                if existing is not None and existing != descriptor:
                    raise QueueFrontendError(
                        "ACPY-TYPE-005: enum identity has conflicting declarations"
                    )
                self.enum_types[descriptor.name] = descriptor
            elif isinstance(descriptor, StructType):
                for field in descriptor.fields:
                    collect_enums(field.type)
            elif isinstance(descriptor, TupleType):
                for element in descriptor.elements:
                    collect_enums(element)
            elif isinstance(descriptor, ArrayType):
                collect_enums(descriptor.element)

        for payload_definition in payloads.values():
            collect_enums(payload_definition.descriptor)
        self.argument = argument
        self.payload = payload
        self.root_name = root_name
        self.root_values = dict(root_values or {})
        self.prefix = prefix
        self.table_views = dict(table_views or {})
        self.slot_views = dict(slot_views or {})
        self.candidates = dict(candidates or {})
        self.selections = dict(selections or {})
        self.candidate_values = dict(candidate_values or {})
        self.selection_values = dict(selection_values or {})
        self.find_values = dict(find_values or {})
        self.state_views = dict(state_views or {})
        self.table_domains = dict(table_domains or {})
        self.bitfields = dict(bitfields or {})
        self.lines: list[str] = []
        self.index = 0
        self.priority_values: dict[str, tuple[str, ValueType, str, ValueType]] = {}
        self.table_view_values: dict[str, tuple[str, ValueType]] = {}
        self.expression_facts: dict[str, _ExpressionFact] = {}

    def _new(self) -> str:
        name = f"{self.prefix}v{self.index}"
        self.index += 1
        return name

    def _remember(
        self,
        name: str,
        value_type: ValueType,
        constraint: Constraint | None = None,
    ) -> tuple[str, ValueType]:
        self.expression_facts[name] = _ExpressionFact(
            value_type,
            constraint_for_type(value_type) if constraint is None else constraint,
        )
        return name, value_type

    def constraint_for_result(self, name: str, value_type: ValueType) -> Constraint:
        fact = self.expression_facts.get(name)
        if fact is None:
            return constraint_for_type(value_type)
        if not _types_equal_in_epoch_05(fact.value_type, value_type):
            raise AssertionError("expression fact type does not match emitted result")
        return fact.constraint

    def reject_constant_index_outside(
        self,
        name: str,
        value_type: ValueType,
        entries: int,
        diagnostic: str,
    ) -> None:
        """Reject a disproven constant and defer every non-constant to MLIR."""

        fact = self.constraint_for_result(name, value_type)
        if not isinstance(fact, Constant):
            return
        if type(fact.value) is not int or not prove_within(fact, 0, entries - 1):
            raise QueueFrontendError(diagnostic)

    def _bitfield_view(
        self, node: ast.expr
    ) -> tuple[str, BitfieldLayout, ast.expr] | None:
        if not isinstance(node, ast.Call) or len(node.args) != 1 or node.keywords:
            return None
        schema_name: str | None = None
        if isinstance(node.func, ast.Name):
            schema_name = node.func.id
        elif (
            isinstance(node.func, ast.Attribute)
            and node.func.attr == "view"
            and isinstance(node.func.value, ast.Name)
        ):
            schema_name = node.func.value.id
        layout = self.bitfields.get(schema_name or "")
        if layout is None or schema_name is None:
            return None
        return schema_name, layout, node.args[0]

    def _emit_bitfield_field(
        self,
        schema_name: str,
        layout: BitfieldLayout,
        base: str,
        base_type: ValueType,
        field_name: str,
    ) -> tuple[str, ValueType]:
        try:
            msb, lsb = layout.field(field_name)
        except KeyError as exc:
            raise QueueFrontendError(f"ACPY-BITFIELD-002: {exc.args[0]}") from exc
        if not _types_equal_in_epoch_05(base_type, BitsType(layout.width)):
            raise QueueFrontendError(
                "ACPY-BITFIELD-002: bitfield value width does not match its schema"
            )
        width = msb - lsb + 1
        result_type = BitsType(width)
        name = self._new()
        self.lines.append(
            f"    %{name} = ac.var.extract %{base} from {lsb} width {width} "
            f"{{ac.bitfield_field = {json.dumps(field_name)}, "
            f"ac.bitfield_fingerprint = {json.dumps(layout.fingerprint)}, "
            f"ac.bitfield_schema = @types::@{schema_name}}} : "
            f"!ac.var<{_render_type(base_type)}> -> "
            f"!ac.var<{_render_type(result_type)}>"
        )
        return name, result_type

    def emit(
        self, node: ast.expr, expected: ValueType | None = None
    ) -> tuple[str, ValueType]:
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id in self.enum_types
        ):
            enumeration = self.enum_types[node.value.id]
            if node.attr not in enumeration.enumerants:
                raise QueueFrontendError(
                    f"ACPY-TYPE-005: unknown enumerant {node.value.id}.{node.attr}"
                )
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.enum @types::@{enumeration.name} "
                f"{json.dumps(node.attr)} : "
                f"!ac.var<{_render_type(enumeration)}>"
            )
            return self._remember(name, enumeration, Constant(node.attr))
        if isinstance(node, (ast.Tuple, ast.List)):
            aggregate = expected
            if not isinstance(aggregate, (TupleType, ArrayType)):
                raise QueueFrontendError(
                    "ACPY-TYPE-006: aggregate literal requires a tuple or value-array context"
                )
            if isinstance(aggregate, TupleType):
                element_types = aggregate.elements
                operation = "tuple"
            elif isinstance(aggregate, ArrayType):
                element_types = (aggregate.element,) * aggregate.length
                operation = "array"
            else:
                raise AssertionError("unreachable aggregate descriptor")
            if len(node.elts) != len(element_types):
                raise QueueFrontendError(
                    "ACPY-TYPE-006: aggregate literal arity must match its type"
                )
            values: list[str] = []
            value_types: list[ValueType] = []
            for element, descriptor in zip(node.elts, element_types, strict=True):
                value, value_type = self.emit(element, descriptor)
                if not _types_equal_in_epoch_05(value_type, descriptor):
                    raise QueueFrontendError(
                        "ACPY-TYPE-006: aggregate element type mismatch"
                    )
                values.append(value)
                value_types.append(value_type)
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.{operation} "
                + ", ".join(f"%{value}" for value in values)
                + " : "
                + ", ".join(
                    f"!ac.var<{_render_type(value_type)}>" for value_type in value_types
                )
                + f" -> !ac.var<{_render_type(aggregate)}>"
            )
            return name, aggregate
        if isinstance(node, ast.Subscript):
            view = self._bitfield_view(node.value)
            if view is not None:
                schema_name, layout, base_node = view
                keys = (
                    tuple(node.slice.elts)
                    if isinstance(node.slice, ast.Tuple)
                    else (node.slice,)
                )
                if not keys or not all(
                    isinstance(key, ast.Constant) and type(key.value) is str
                    for key in keys
                ):
                    raise QueueFrontendError(
                        "ACPY-BITFIELD-002: bitfield selection requires static field names"
                    )
                base, base_type = self.emit(base_node)
                selected = [
                    self._emit_bitfield_field(
                        schema_name, layout, base, base_type, key.value
                    )
                    for key in keys
                    if isinstance(key, ast.Constant) and type(key.value) is str
                ]
                if len(selected) == 1:
                    return selected[0]
                result_width = sum(value_type.bit_width() for _, value_type in selected)
                if result_width > 64:
                    raise QueueFrontendError(
                        "ACPY-BITFIELD-002: selected bitfield width must be in [1, 64]"
                    )
                name = self._new()
                field_names = [
                    key.value
                    for key in keys
                    if isinstance(key, ast.Constant) and type(key.value) is str
                ]
                self.lines.append(
                    f"    %{name} = ac.var.concat "
                    + ", ".join(f"%{value}" for value, _ in selected)
                    + " {ac.bitfield_fields = "
                    + json.dumps(field_names)
                    + ", ac.bitfield_fingerprint = "
                    + json.dumps(layout.fingerprint)
                    + ", ac.bitfield_schema = @types::@"
                    + schema_name
                    + "} : "
                    + ", ".join(
                        f"!ac.var<{_render_type(value_type)}>"
                        for _, value_type in selected
                    )
                    + f" -> !ac.var<i{result_width}>"
                )
                return name, BitsType(result_width)
        if isinstance(node, ast.Attribute):
            view = self._bitfield_view(node.value)
            if view is not None:
                schema_name, layout, base_node = view
                base, base_type = self.emit(base_node)
                return self._emit_bitfield_field(
                    schema_name, layout, base, base_type, node.attr
                )
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "update"
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id in self.bitfields
        ):
            if len(node.args) != 1 or any(
                keyword.arg is None for keyword in node.keywords
            ):
                raise QueueFrontendError(
                    "ACPY-BITFIELD-003: bitfield update requires one value and named fields"
                )
            schema_name = node.func.value.id
            layout = self.bitfields[schema_name]
            base, base_type = self.emit(node.args[0])
            if not _types_equal_in_epoch_05(base_type, BitsType(layout.width)):
                raise QueueFrontendError(
                    "ACPY-BITFIELD-003: bitfield value width does not match its schema"
                )
            values = {
                keyword.arg: keyword.value
                for keyword in node.keywords
                if keyword.arg is not None
            }
            try:
                writes = layout.checked_writes(values)
            except (ValueError, KeyError) as exc:
                message = exc.args[0] if exc.args else str(exc)
                raise QueueFrontendError(f"ACPY-BITFIELD-003: {message}") from exc
            current = base
            for lsb, msb, field_name in writes:
                width = msb - lsb + 1
                field_type = BitsType(width)
                value, value_type = self.emit(values[field_name], field_type)
                if not _types_equal_in_epoch_05(value_type, field_type):
                    raise QueueFrontendError(
                        f"ACPY-BITFIELD-003: field {field_name!r} requires i{width}"
                    )
                name = self._new()
                self.lines.append(
                    f"    %{name} = ac.var.insert %{current}, %{value} at {lsb} "
                    f"{{ac.bitfield_field = {json.dumps(field_name)}, "
                    f"ac.bitfield_fingerprint = {json.dumps(layout.fingerprint)}, "
                    f"ac.bitfield_schema = @types::@{schema_name}}} : "
                    f"!ac.var<{_render_type(base_type)}>, "
                    f"!ac.var<{_render_type(value_type)}> -> "
                    f"!ac.var<{_render_type(base_type)}>"
                )
                current = name
            return current, base_type
        if (
            isinstance(node, ast.Subscript)
            and isinstance(node.value, ast.Name)
            and node.value.id in self.state_views
        ):
            variable, value_type, entries = self.state_views[node.value.id]
            index, index_type = self.emit(node.slice)
            index_width = _epoch_05_integer_width(index_type)
            if index_width is None:
                raise QueueFrontendError(
                    "ACPY-RULE-009: persistent find capture index must be integer"
                )
            self.reject_constant_index_outside(
                index,
                index_type,
                entries,
                "ACPY-RULE-009: persistent find capture index is out of range",
            )
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.read_element @{variable}[%{index}] : "
                f"!ac.var<{_render_type(index_type)}> -> "
                f"!ac.var<{_render_type(value_type)}>"
            )
            return name, value_type
        if isinstance(node, ast.Subscript):
            value, value_type = self.emit(node.value)
            if isinstance(value_type, (TupleType, ArrayType)):
                aggregate = value_type
                index = _constant_integer(node.slice)
                if index is None:
                    raise QueueFrontendError(
                        "ACPY-TYPE-006: aggregate index must be a static integer"
                    )
                if isinstance(aggregate, TupleType):
                    if not _proven_integer_in(index, 0, len(aggregate.elements) - 1):
                        raise QueueFrontendError(
                            "ACPY-TYPE-006: tuple index is out of range"
                        )
                    result_type = aggregate.elements[index]
                elif isinstance(aggregate, ArrayType):
                    if not _proven_integer_in(index, 0, aggregate.length - 1):
                        raise QueueFrontendError(
                            "ACPY-TYPE-006: value-array index is out of range"
                        )
                    result_type = aggregate.element
                else:
                    raise AssertionError("unreachable aggregate descriptor")
                name = self._new()
                self.lines.append(
                    f"    %{name} = ac.var.element %{value} at {index} : "
                    f"!ac.var<{_render_type(value_type)}> -> "
                    f"!ac.var<{_render_type(result_type)}>"
                )
                return name, result_type
            source_width = _epoch_05_integer_width(value_type)
            if source_width is None:
                raise QueueFrontendError(
                    "ACPY-BITS-001: bit extraction requires a bits value"
                )
            if isinstance(node.slice, ast.Slice):
                if node.slice.step is not None:
                    raise QueueFrontendError(
                        "ACPY-BITS-001: bit slice step is not supported"
                    )
                lower = node.slice.lower
                upper = node.slice.upper
                lsb = _constant_integer(lower) if lower is not None else None
                end = _constant_integer(upper) if upper is not None else None
                if lsb is None or end is None:
                    raise QueueFrontendError(
                        "ACPY-BITS-001: bit slice bounds must be static integers"
                    )
            elif (index := _constant_integer(node.slice)) is not None:
                lsb = index
                end = lsb + 1
            else:
                raise QueueFrontendError(
                    "ACPY-BITS-001: bit index must be a static integer"
                )
            if (
                end <= lsb
                or not _proven_integer_in(lsb, 0, source_width - 1)
                or not _proven_integer_in(end, 1, source_width)
            ):
                raise QueueFrontendError(
                    "ACPY-BITS-001: bit slice is empty or out of range"
                )
            result_width = end - lsb
            result_type = BitsType(result_width)
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.extract %{value} from {lsb} width "
                f"{result_width} : !ac.var<{_render_type(value_type)}> -> "
                f"!ac.var<{_render_type(result_type)}>"
            )
            return name, result_type
        if isinstance(node, ast.Name) and node.id in self.root_values:
            return self.root_values[node.id]
        if isinstance(node, ast.Name) and node.id == self.argument:
            return self.root_name, self.payload
        if isinstance(node, ast.Name) and node.id in self.candidate_values:
            return self.candidate_values[node.id]
        if isinstance(node, ast.Name) and node.id in self.candidates:
            candidate = self.candidates[node.id]
            domain = self.table_domains.get(candidate.table)
            if domain is None:
                raise QueueFrontendError(
                    "ACPY-TABLE-008: CandidateSet domain is unresolved"
                )
            entry_type, mask_width = domain
            predicate_emitter = _ExpressionEmitter(
                self.payloads,
                candidate.argument,
                entry_type,
                root_name="entry",
                prefix=f"{self.prefix}m{self.index}_",
                slot_views=self.slot_views,
                bitfields=self.bitfields,
            )
            predicate, predicate_type = predicate_emitter.emit(
                candidate.predicate, BoolType()
            )
            if not _is_epoch_05_bool_compatible(predicate_type):
                raise QueueFrontendError(
                    "ACPY-TABLE-006: match predicate must lower to i1"
                )
            mask = self._new()
            self.lines.append(
                f"    %{mask} = ac.table.match @{candidate.table} predicate {{"
            )
            self.lines.append(
                f"    ^predicate(%entry: !ac.var<{_render_type(entry_type)}>):"
            )
            self.lines.extend(predicate_emitter.lines)
            self.lines.append(f"      ac.table.match.yield %{predicate} : !ac.var<i1>")
            self.lines.append(f"    }} -> !ac.var<i{mask_width}>")
            return mask, BitsType(mask_width)
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id in self.find_values
            and node.attr in {"index", "valid", "value"}
        ):
            index, index_type, valid, valid_type, variable, value_type, value = (
                self.find_values[node.value.id]
            )
            if node.attr == "index":
                return index, index_type
            if node.attr == "valid":
                return valid, valid_type
            if value is None:
                value = self._new()
                self.lines.append(
                    f"    %{value} = ac.var.read_element @{variable}[%{index}] : "
                    f"!ac.var<{_render_type(index_type)}> -> "
                    f"!ac.var<{_render_type(value_type)}>"
                )
                self.find_values[node.value.id] = (
                    index,
                    index_type,
                    valid,
                    valid_type,
                    variable,
                    value_type,
                    value,
                )
            return value, value_type
        if isinstance(node, ast.Name) and node.id in self.table_views:
            if node.id in self.table_view_values:
                return self.table_view_values[node.id]
            table, address, entry_type = self.table_views[node.id]
            index, index_type = self.emit(address)
            if _epoch_05_integer_width(index_type) is None:
                raise QueueFrontendError(
                    "ACPY-TABLE-003: table index must lower to an integer"
                )
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.table.get @{table} [%{index}] : "
                f"!ac.var<{_render_type(index_type)}> -> "
                f"!ac.var<{_render_type(entry_type)}>"
            )
            self.table_view_values[node.id] = (name, entry_type)
            return self.table_view_values[node.id]
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id in self.selection_values
            and node.attr in {"index", "valid"}
        ):
            index, index_type, valid, valid_type = self.selection_values[node.value.id]
            return (index, index_type) if node.attr == "index" else (valid, valid_type)
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id in self.slot_views
            and node.attr in {"valid", "value"}
        ):
            slot, payload = self.slot_views[node.value.id]
            valid = self._new()
            value = self._new()
            self.lines.append(
                f"    %{valid}, %{value} = ac.slot.get @{slot} : "
                f"!ac.var<i1>, !ac.var<{_render_type(payload)}>"
            )
            return (valid, BoolType()) if node.attr == "valid" else (value, payload)
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id in self.selections
            and node.attr in {"index", "valid"}
        ):
            selection = self.selections[node.value.id]
            candidate = self.candidates[selection.candidates]
            domain = self.table_domains.get(selection.table)
            if domain is None:
                raise QueueFrontendError(
                    "ACPY-TABLE-007: selection domain is unresolved"
                )
            entry_type, mask_width = domain
            predicate_emitter = _ExpressionEmitter(
                self.payloads,
                candidate.argument,
                entry_type,
                root_name="entry",
                prefix=f"{self.prefix}m{self.index}_",
                slot_views=self.slot_views,
                bitfields=self.bitfields,
            )
            predicate, predicate_type = predicate_emitter.emit(
                candidate.predicate, BoolType()
            )
            if not _is_epoch_05_bool_compatible(predicate_type):
                raise QueueFrontendError(
                    "ACPY-TABLE-006: match predicate must lower to i1"
                )
            mask = self._new()
            self.lines.append(
                f"    %{mask} = ac.table.match @{selection.table} predicate {{"
            )
            self.lines.append(
                f"    ^predicate(%entry: !ac.var<{_render_type(entry_type)}>):"
            )
            self.lines.extend(predicate_emitter.lines)
            self.lines.append(f"      ac.table.match.yield %{predicate} : !ac.var<i1>")
            self.lines.append(f"    }} -> !ac.var<i{mask_width}>")
            index = self._new()
            valid = self._new()
            index_width = max(1, (mask_width - 1).bit_length())
            if selection.policy == "first":
                key_region = "{}"
                self.lines.append(
                    f"    %{index}, %{valid} = ac.table.choose @{selection.table} "
                    f'%{mask} : !ac.var<i{mask_width}> count 1 policy "first" '
                    f"key {key_region} -> "
                    f"!ac.var<i{index_width}>, !ac.var<i1>"
                )
            else:
                assert selection.argument is not None and selection.key is not None
                key_emitter = _ExpressionEmitter(
                    self.payloads,
                    selection.argument,
                    entry_type,
                    root_name="entry",
                    prefix=f"{self.prefix}k{self.index}_",
                    bitfields=self.bitfields,
                )
                key, key_type = key_emitter.emit(selection.key)
                if _epoch_05_integer_width(key_type) is None:
                    raise QueueFrontendError(
                        "ACPY-TABLE-007: choose key must lower to an integer"
                    )
                self.lines.append(
                    f"    %{index}, %{valid} = ac.table.choose @{selection.table} "
                    f"%{mask} : !ac.var<i{mask_width}> count 1 "
                    f'policy "{selection.policy}" key {{'
                )
                self.lines.append(
                    f"    ^key(%entry: !ac.var<{_render_type(entry_type)}>):"
                )
                self.lines.extend(key_emitter.lines)
                self.lines.append(
                    f"      ac.table.choose.yield %{key} : "
                    f"!ac.var<{_render_type(key_type)}>"
                )
                self.lines.append(f"    }} -> !ac.var<i{index_width}>, !ac.var<i1>")
            return (
                (index, BitsType(index_width))
                if node.attr == "index"
                else (valid, BoolType())
            )
        if isinstance(node, ast.Constant) and type(node.value) in {int, bool}:
            typ = expected or (BoolType() if type(node.value) is bool else BitsType(64))
            name = self._new()
            value = (
                "true"
                if node.value is True
                else "false"
                if node.value is False
                else str(node.value)
            )
            attribute = (
                value if type(node.value) is bool else f"{value} : {_render_type(typ)}"
            )
            self.lines.append(
                f"    %{name} = ac.var.constant {attribute} as "
                f"!ac.var<{_render_type(typ)}>"
            )
            return self._remember(name, typ, Constant(node.value))
        if (
            isinstance(node, ast.Attribute)
            and node.attr in {"index", "valid"}
            and isinstance(node.value, ast.Call)
            and _decorator_name(node.value.func).rsplit(".", 1)[-1] == "priority_encode"
        ):
            call = node.value
            if len(call.args) != 1 or any(
                keyword.arg != "order" for keyword in call.keywords
            ):
                raise QueueFrontendError(
                    "ACPY-QUEUE-025: priority_encode requires one value and optional order"
                )
            order = "low"
            if call.keywords:
                raw_order = call.keywords[0].value
                if (
                    not isinstance(raw_order, ast.Constant)
                    or type(raw_order.value) is not str
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-025: priority_encode order must be static"
                    )
                order = raw_order.value.strip().lower()
            if order not in {"low", "high"}:
                raise QueueFrontendError(
                    "ACPY-QUEUE-025: priority_encode order must be low or high"
                )
            key = ast.dump(call, include_attributes=False)
            cached = self.priority_values.get(key)
            if cached is None:
                value, value_type = self.emit(call.args[0])
                width = _epoch_05_integer_width(value_type)
                if width is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-025: priority_encode requires an integer payload"
                    )
                if not 1 <= width <= 64:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-025: priority_encode width must be in [1, 64]"
                    )
                index_type = BitsType(max(1, (width - 1).bit_length()))
                index = self._new()
                valid = self._new()
                self.lines.append(
                    f"    %{index}, %{valid} = ac.var.priority_encode %{value} "
                    f'order "{order}" : !ac.var<{_render_type(value_type)}> -> '
                    f"!ac.var<{_render_type(index_type)}>, !ac.var<i1>"
                )
                cached = (index, index_type, valid, BoolType())
                self.priority_values[key] = cached
            index, index_type, valid, valid_type = cached
            return (
                (index, index_type)
                if node.attr == "index"
                else (
                    valid,
                    valid_type,
                )
            )
        if isinstance(node, ast.Attribute):
            record, record_type = self.emit(node.value)
            if not isinstance(record_type, StructType):
                raise QueueFrontendError(f"ACPY-QUEUE-003: unknown field {node.attr!r}")
            try:
                field_type = record_type.field(node.attr).type
            except KeyError as exc:
                raise QueueFrontendError(
                    f"ACPY-QUEUE-003: unknown field {node.attr!r}"
                ) from exc
            rendered_record_type = _render_type(record_type)
            rendered_field_type = _render_type(field_type)
            name = self._new()
            self.lines.append(
                f'    %{name} = ac.var.get %{record} field "{node.attr}" : '
                f"!ac.var<{rendered_record_type}> -> "
                f"!ac.var<{rendered_field_type}>"
            )
            return name, field_type
        if isinstance(node, ast.BinOp) and isinstance(
            node.op,
            (
                ast.Add,
                ast.Sub,
                ast.Mult,
                ast.BitAnd,
                ast.BitOr,
                ast.BitXor,
                ast.LShift,
                ast.RShift,
            ),
        ):
            left, left_type = self.emit(node.left)
            right, right_type = self.emit(node.right, left_type)
            if not _types_equal_in_epoch_05(left_type, right_type):
                raise QueueFrontendError("ACPY-QUEUE-003: binary operands must match")
            if isinstance(left_type, EnumType):
                raise QueueFrontendError(
                    "ACPY-TYPE-005: enum values support only equality comparison"
                )
            opcode = {
                ast.Add: "add",
                ast.Sub: "sub",
                ast.Mult: "mul",
                ast.BitAnd: "and",
                ast.BitOr: "or",
                ast.BitXor: "xor",
                ast.LShift: "shl",
                ast.RShift: "shr",
            }[type(node.op)]
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.{opcode} %{left}, %{right} : "
                f"!ac.var<{_render_type(left_type)}>"
            )
            width = _epoch_05_integer_width(left_type)
            constraint = (
                transfer_bits(
                    opcode,
                    self.constraint_for_result(left, left_type),
                    self.constraint_for_result(right, right_type),
                    width=width,
                )
                if width is not None
                else Unknown()
            )
            return self._remember(name, left_type, constraint)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Invert):
            value, value_type = self.emit(node.operand)
            if _epoch_05_integer_width(value_type) is None:
                raise QueueFrontendError(
                    "ACPY-QUEUE-003: bitwise not requires an integer payload"
                )
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.not %{value} : "
                f"!ac.var<{_render_type(value_type)}> -> "
                f"!ac.var<{_render_type(value_type)}>"
            )
            return name, value_type
        if isinstance(node, ast.BoolOp) and isinstance(node.op, ast.And):
            if len(node.values) < 2:
                raise QueueFrontendError(
                    "ACPY-QUEUE-003: boolean and requires two operands"
                )
            current, current_type = self.emit(node.values[0], BoolType())
            if not _is_epoch_05_bool_compatible(current_type):
                raise QueueFrontendError("ACPY-QUEUE-003: boolean operands must be i1")
            for operand in node.values[1:]:
                value, value_type = self.emit(operand, BoolType())
                if not _is_epoch_05_bool_compatible(value_type):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-003: boolean operands must be i1"
                    )
                name = self._new()
                self.lines.append(
                    f"    %{name} = ac.var.mul %{current}, %{value} : !ac.var<i1>"
                )
                current = name
            return current, BoolType()
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            value, value_type = self.emit(node.operand, BoolType())
            if not _is_epoch_05_bool_compatible(value_type):
                raise QueueFrontendError("ACPY-QUEUE-003: boolean not requires i1")
            false_value = self._new()
            self.lines.append(
                f"    %{false_value} = ac.var.constant false as !ac.var<i1>"
            )
            name = self._new()
            self.lines.append(
                f'    %{name} = ac.var.cmp "eq" %{value}, %{false_value} : '
                "!ac.var<i1> -> !ac.var<i1>"
            )
            return name, BoolType()
        if (
            isinstance(node, ast.Compare)
            and len(node.ops) == len(node.comparators) == 1
        ):
            left, left_type = self.emit(node.left)
            right, right_type = self.emit(node.comparators[0], left_type)
            if not _types_equal_in_epoch_05(left_type, right_type):
                raise QueueFrontendError(
                    "ACPY-QUEUE-003: comparison operands must match"
                )
            predicates = {
                ast.Eq: "eq",
                ast.NotEq: "ne",
                ast.Lt: "ult",
                ast.LtE: "ule",
                ast.Gt: "ugt",
                ast.GtE: "uge",
            }
            predicate = predicates.get(type(node.ops[0]))
            if predicate is None:
                raise QueueFrontendError("ACPY-QUEUE-003: unsupported comparison")
            if isinstance(left_type, EnumType) and predicate not in {"eq", "ne"}:
                raise QueueFrontendError(
                    "ACPY-TYPE-005: enum values support only equality comparison"
                )
            name = self._new()
            self.lines.append(
                f'    %{name} = ac.var.cmp "{predicate}" %{left}, %{right} : '
                f"!ac.var<{_render_type(left_type)}> -> !ac.var<i1>"
            )
            return name, BoolType()
        if (
            isinstance(node, ast.Call)
            and _decorator_name(node.func).rsplit(".", 1)[-1] == "matches"
        ):
            if len(node.args) != 2 or node.keywords:
                raise QueueFrontendError(
                    "ACPY-BITS-004: matches requires two positional arguments"
                )
            if not (
                isinstance(node.args[1], ast.Constant)
                and type(node.args[1].value) is str
            ):
                raise QueueFrontendError(
                    "ACPY-BITS-004: matches pattern must be a compile-time str"
                )
            value, value_type = self.emit(node.args[0])
            if not isinstance(value_type, BitsType):
                raise QueueFrontendError("ACPY-BITS-004: matches requires a bits value")
            try:
                mask, expected_value = parse_bitmask_checked(
                    node.args[1].value,
                    width=value_type.width,
                    extended=False,
                )
            except (TypeError, ValueError) as error:
                raise QueueFrontendError(f"ACPY-BITS-004: {error}") from error
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.matches %{value} mask {mask} "
                f"value {expected_value} : "
                f"!ac.var<{_render_type(value_type)}> -> !ac.var<i1>"
            )
            return self._remember(name, BoolType())
        if (
            isinstance(node, ast.Call)
            and _decorator_name(node.func).rsplit(".", 1)[-1] == "concat"
        ):
            if not node.args or node.keywords:
                raise QueueFrontendError(
                    "ACPY-BITS-002: concat requires one or more positional values"
                )
            operands: list[str] = []
            operand_types: list[ValueType] = []
            result_width = 0
            for argument in node.args:
                operand, operand_type = self.emit(argument)
                operand_width = _epoch_05_integer_width(operand_type)
                if operand_width is None:
                    raise QueueFrontendError(
                        "ACPY-BITS-002: concat operands must be bits values"
                    )
                operands.append(operand)
                operand_types.append(operand_type)
                result_width += operand_width
            if result_width > 64:
                raise QueueFrontendError(
                    "ACPY-BITS-002: concat result width must be in [1, 64]"
                )
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.concat "
                + ", ".join(f"%{operand}" for operand in operands)
                + " : "
                + ", ".join(f"!ac.var<{_render_type(typ)}>" for typ in operand_types)
                + f" -> !ac.var<i{result_width}>"
            )
            return name, BitsType(result_width)
        if (
            isinstance(node, ast.Call)
            and _decorator_name(node.func).rsplit(".", 1)[-1] == "insert"
        ):
            lsb_values = [
                keyword.value for keyword in node.keywords if keyword.arg == "lsb"
            ]
            if (
                len(node.args) != 2
                or len(lsb_values) != 1
                or len(node.keywords) != 1
                or _constant_integer(lsb_values[0]) is None
            ):
                raise QueueFrontendError(
                    "ACPY-BITS-003: insert requires value, field, and static lsb"
                )
            base, base_type = self.emit(node.args[0])
            field, field_type = self.emit(node.args[1])
            base_width = _epoch_05_integer_width(base_type)
            field_width = _epoch_05_integer_width(field_type)
            if base_width is None or field_width is None:
                raise QueueFrontendError(
                    "ACPY-BITS-003: insert operands must be bits values"
                )
            lsb = _constant_integer(lsb_values[0])
            assert lsb is not None
            if field_width > base_width or not _proven_integer_in(
                lsb, 0, base_width - field_width
            ):
                raise QueueFrontendError(
                    "ACPY-BITS-003: inserted field is out of range"
                )
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.insert %{base}, %{field} at {lsb} : "
                f"!ac.var<{_render_type(base_type)}>, "
                f"!ac.var<{_render_type(field_type)}> -> "
                f"!ac.var<{_render_type(base_type)}>"
            )
            return name, base_type
        if (
            isinstance(node, ast.Call)
            and _decorator_name(node.func).rsplit(".", 1)[-1] == "popcount"
        ):
            if len(node.args) != 1 or node.keywords:
                raise QueueFrontendError(
                    "ACPY-QUEUE-003: popcount requires exactly one positional operand"
                )
            value, value_type = self.emit(node.args[0])
            width = _epoch_05_integer_width(value_type)
            if width is None:
                raise QueueFrontendError(
                    "ACPY-QUEUE-003: popcount operand must be an integer payload"
                )
            if width <= 0:
                raise QueueFrontendError(
                    "ACPY-QUEUE-003: popcount operand width must be positive"
                )
            result_width = width.bit_length()
            name = self._new()
            self.lines.append(
                f"    %{name} = ac.var.popcount %{value} : "
                f"!ac.var<{_render_type(value_type)}> -> !ac.var<i{result_width}>"
            )
            return name, BitsType(result_width)
        if isinstance(node, ast.Call) and _decorator_name(node.func).rsplit(".", 1)[
            -1
        ] in {"count_leading_zeros", "count_trailing_zeros"}:
            operation = _decorator_name(node.func).rsplit(".", 1)[-1]
            if len(node.args) != 1 or node.keywords:
                raise QueueFrontendError(
                    f"ACPY-QUEUE-003: {operation} requires exactly one positional operand"
                )
            value, value_type = self.emit(node.args[0])
            width = _epoch_05_integer_width(value_type)
            if width is None:
                raise QueueFrontendError(
                    f"ACPY-QUEUE-003: {operation} operand must be an integer payload"
                )
            if width <= 0:
                raise QueueFrontendError(
                    f"ACPY-QUEUE-003: {operation} operand width must be positive"
                )
            result_width = width.bit_length()
            name = self._new()
            direction = "trailing" if operation == "count_trailing_zeros" else "leading"
            self.lines.append(
                f'    %{name} = ac.var.count_zeros %{value} direction "{direction}" : '
                f"!ac.var<{_render_type(value_type)}> -> !ac.var<i{result_width}>"
            )
            return name, BitsType(result_width)
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "with_fields"
            and not node.args
        ):
            record, record_type = self.emit(node.func.value)
            current = record
            for keyword in node.keywords:
                if keyword.arg is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-003: field unpacking is forbidden"
                    )
                if not isinstance(record_type, StructType):
                    raise QueueFrontendError(
                        f"ACPY-QUEUE-003: unknown field {keyword.arg!r}"
                    )
                try:
                    field_type = record_type.field(keyword.arg).type
                except KeyError as exc:
                    raise QueueFrontendError(
                        f"ACPY-QUEUE-003: unknown field {keyword.arg!r}"
                    ) from exc
                value, value_type = self.emit(keyword.value, field_type)
                if not _types_equal_in_epoch_05(value_type, field_type):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-003: field update type mismatch"
                    )
                name = self._new()
                self.lines.append(
                    f"    %{name} = ac.var.with %{current}, %{value} field "
                    f'"{keyword.arg}" : !ac.var<{_render_type(record_type)}>, '
                    f"!ac.var<{_render_type(field_type)}> -> "
                    f"!ac.var<{_render_type(record_type)}>"
                )
                current = name
            return current, record_type
        raise QueueFrontendError("ACPY-QUEUE-003: unsupported lambda expression")


def lower_queue_program(
    program: QueueProgram, *, module: _ModuleRenderSpec | None = None
) -> str:
    specialization = (
        ""
        if program.specialization_fingerprint is None
        else f', ac.specialization = "{program.specialization_fingerprint}"'
    )
    module_inputs = set() if module is None else {name for name, _ in module.inputs}
    module_outputs = set() if module is None else {name for name, _ in module.outputs}
    initial_mapping: dict[str, str] = {}
    if module is None:
        lines = [
            f'module attributes {{ac.contract_epoch = "0.5", '
            f'ac.model_kind = "queue_graph", '
            f'ac.queue_graph_domain = "cycle", '
            f'ac.system = "{program.system}"{specialization}}} {{'
        ]
        content_indent = "  "
    else:
        argument_types = ", ".join(
            f"%input_{index}: !ac.queue<{_render_type(payload)}>"
            for index, (_, payload) in enumerate(module.inputs)
        )
        result_types = ", ".join(
            f"!ac.queue<{_render_type(payload)}>" for _, payload in module.outputs
        )
        result_signature = (
            ""
            if not module.outputs
            else " -> "
            + (result_types if len(module.outputs) == 1 else f"({result_types})")
        )
        scope_results = [
            f"module_result_{index}" for index in range(len(module.outputs))
        ]
        scope_lhs = (
            ""
            if not scope_results
            else ", ".join(f"%{name}" for name in scope_results) + " = "
        )
        scope_operands = ", ".join(
            f"%input_{index}" for index in range(len(module.inputs))
        )
        scope_arguments = ", ".join(
            f"%borrowed_{index}: !ac.queue<{_render_type(payload)}>"
            for index, (_, payload) in enumerate(module.inputs)
        )
        lines = [
            f"  ac.module @{module.name}({argument_types}){result_signature} "
            "parameters {} graph {",
            f"    {scope_lhs}ac.scope @body({scope_operands}) {{",
            f"    ^bb0({scope_arguments}):" if scope_arguments else "    ^bb0:",
        ]
        initial_mapping = {
            name: f"borrowed_{index}" for index, (name, _) in enumerate(module.inputs)
        }
        content_indent = "      "
    payloads = {item.name: item for item in program.payloads}
    bitfields = {item.name: item.layout for item in program.bitfields}
    if (program.payloads or program.enums or program.bitfields) and module is None:
        lines.append("  ac.type_scope @types {")
        for enumeration in program.enums:
            lines.append(_render_enum(enumeration, "    "))
        for payload in program.payloads:
            fields = ", ".join(
                f'{{name = "{name}", type = {typ}}}' for name, typ in payload.fields
            )
            lines.append(f"    ac.struct @{payload.name} fields [{fields}]")
        for bitfield in program.bitfields:
            lines.append(_render_bitfield(bitfield, "    "))
        layouts = [
            *(_enum_layout_entry(enumeration) for enumeration in program.enums),
            *(_payload_layout_entry(payload) for payload in program.payloads),
        ]
        if layouts:
            lines.append(
                "  } {dlti.dl_spec = #dlti.dl_spec<" + ", ".join(layouts) + ">}"
            )
        else:
            lines.append("  }")
    for instance in sorted(
        program.memory_instances,
        key=lambda value: (value.scope, value.order, value.name),
    ):
        owner = "/" + "/".join(instance.scope) if instance.scope else "/"
        stable_id = (
            "/".join((*instance.scope, instance.name))
            if instance.scope
            else instance.name
        )
        if module is not None:
            raise QueueFrontendError(
                "ACPY-MODULE-005: module-local memory instances are not implemented"
            )
        lines.append(
            f"{content_indent}ac.memory.instance @{instance.name} "
            f"data {_render_type(instance.data_type)} "
            f"entries {instance.entries} init {instance.init} "
            f'latency {instance.latency} owner "{owner}" '
            f'stable_id "memory/{stable_id}"'
        )
    for variable in sorted(
        program.variables, key=lambda value: (value.scope, value.order, value.name)
    ):
        owner_scope = variable.scope if module is None else ("body", *variable.scope)
        owner = "/" + "/".join(owner_scope) if owner_scope else "/"
        stable_id = (
            "/".join((*owner_scope, variable.name)) if owner_scope else variable.name
        )
        init = (
            "true"
            if variable.init is True
            else "false"
            if variable.init is False
            else f"{variable.init} : "
            + (
                "i64"
                if isinstance(
                    variable.value_type,
                    (StructType, TupleType, ArrayType, EnumType),
                )
                else _render_type(variable.value_type)
            )
        )
        lines.append(
            f"{content_indent}ac.var.decl @{variable.name} "
            f"type {_render_type(variable.value_type)} "
            f'init {init} owner "{owner}" stable_id "var/{stable_id}"'
            + (f" shape [{variable.entries}]" if variable.entries != 1 else "")
        )
    for table in sorted(
        program.tables, key=lambda value: (value.scope, value.order, value.name)
    ):
        owner_scope = table.scope if module is None else ("body", *table.scope)
        owner = "/" + "/".join(owner_scope) if owner_scope else "/"
        stable_id = "/".join((*owner_scope, table.name)) if owner_scope else table.name
        lines.append(
            f"{content_indent}ac.table @{table.name} "
            f"entry {_render_type(table.entry_type)} "
            f'entries {table.entries} init 0 owner "{owner}" '
            f'stable_id "table/{stable_id}"'
        )
    by_name = {item.name: item for item in program.queues}
    memory_ordinals: dict[tuple[str, str], int] = {}
    requests_by_instance: dict[str, list[MemoryRequestBinding]] = {}
    for request in program.memory_requests:
        requests_by_instance.setdefault(request.instance, []).append(request)
    for instance, requests in requests_by_instance.items():
        for ordinal, request in enumerate(
            sorted(
                requests,
                key=lambda value: (value.scope, value.order, value.output_name),
            )
        ):
            memory_ordinals[(instance, request.output_name)] = ordinal

    def name_array(names: list[str] | tuple[str, ...]) -> str:
        return "[" + ", ".join(f'"{name}"' for name in names) + "]"

    consumers: dict[str, list[tuple[QueueBinding, int]]] = {}
    for queue in (*program.queues, *program.effect_rules):
        input_names = (
            queue.rule_input_names
            if queue.rule_input_names
            else (() if queue.input_name is None else (queue.input_name,))
        )
        for input_index, input_name in enumerate(input_names):
            consumers.setdefault(input_name, []).append((queue, input_index))
    fanouts: dict[
        str, tuple[tuple[str, ...], tuple[tuple[QueueBinding, int], ...]]
    ] = {}

    def common_scope(scopes: list[tuple[str, ...]]) -> tuple[str, ...]:
        common: list[str] = []
        for parts in zip(*scopes, strict=False):
            if len(set(parts)) != 1:
                break
            common.append(parts[0])
        return tuple(common)

    for source_name, group in consumers.items():
        if len(group) < 2:
            continue
        fanouts[source_name] = (
            common_scope([consumer.scope for consumer, _ in group]),
            tuple(group),
        )
    payload_by_queue = {name: queue.payload for name, queue in by_name.items()}
    slot_views = {slot.name: (slot.name, slot.payload) for slot in program.slots}
    candidate_views = {candidate.name: candidate for candidate in program.candidates}
    selection_views = {selection.name: selection for selection in program.selections}
    table_domains = {
        table.name: (table.entry_type, table.entries) for table in program.tables
    }
    variable_domains = {
        variable.name: (variable.value_type, variable.entries)
        for variable in program.variables
    }
    materialized_candidates: dict[str, tuple[str, ValueType]] = {}
    materialized_selections: dict[str, tuple[str, ValueType, str, ValueType]] = {}
    queue_scope = {name: queue.scope for name, queue in by_name.items()}
    effective_input: dict[tuple[str, int], str] = {}
    for source_name, (fanout_scope, group) in fanouts.items():
        for index, (consumer, input_index) in enumerate(group):
            synthetic = f"{source_name}__fanout{index}"
            effective_input[(consumer.name, input_index)] = synthetic
            payload_by_queue[synthetic] = by_name[source_name].payload
            queue_scope[synthetic] = fanout_scope

    uses: dict[str, list[tuple[str, ...]]] = {name: [] for name in payload_by_queue}
    for queue in (*program.queues, *program.effect_rules):
        input_names = (
            queue.rule_input_names
            if queue.rule_input_names
            else (() if queue.input_name is None else (queue.input_name,))
        )
        for input_index, input_name in enumerate(input_names):
            selected = effective_input.get((queue.name, input_index), input_name)
            uses[selected].append(queue.scope)
    for source_name, (fanout_scope, _) in fanouts.items():
        uses[source_name].append(fanout_scope)
    for sink_binding in program.sinks:
        uses[sink_binding.queue].append(sink_binding.scope)
    for observation in program.observations:
        uses[observation.queue].append(observation.scope)
    for expectation in program.expectations:
        uses[expectation.queue].append(expectation.scope)
    for route in program.routes:
        uses[route.input_name].append(route.scope)
    for fork in program.forks:
        uses[fork.input_name].append(fork.scope)
    for feedback in program.feedbacks:
        uses[feedback.input_name].append(feedback.scope)
    for merge in program.merges:
        for input_name in merge.inputs:
            uses[input_name].append(merge.scope)
    for reorder in program.reorders:
        uses[reorder.input_name].append(reorder.scope)
    for dependency in program.dependencies:
        uses[dependency.input_name].append(dependency.scope)
    for credit in program.credits:
        uses[credit.input_name].append(credit.scope)
    for barrier in program.barriers:
        for input_name in barrier.inputs:
            uses[input_name].append(barrier.scope)
    for select in program.selects:
        uses[select.control].append(select.scope)
        for input_name in select.inputs:
            uses[input_name].append(select.scope)
    for request in program.memory_requests:
        uses[request.input_name].append(request.scope)
    for read in program.table_reads:
        if read.input_name is not None:
            uses[read.input_name].append(read.scope)
    for write in program.table_writes:
        if write.input_name is not None:
            uses[write.input_name].append(write.scope)
    for slot in program.slots:
        uses[slot.input_name].append(slot.scope)

    def inside(container: tuple[str, ...], candidate: tuple[str, ...]) -> bool:
        return candidate[: len(container)] == container

    def scope_io(path: tuple[str, ...]) -> tuple[list[str], list[str]]:
        inputs = [
            name
            for name, producer_scope in queue_scope.items()
            if not inside(path, producer_scope)
            and any(inside(path, use) for use in uses[name])
        ]
        outputs = [
            name
            for name, producer_scope in queue_scope.items()
            if inside(path, producer_scope)
            and any(not inside(path, use) for use in uses[name])
        ]
        return inputs, outputs

    def queue_attributes(name: str, rates: tuple[int, ...]) -> str:
        attributes = [f'ac.name = "{name}"']
        if any(rate != 1 for rate in rates):
            attributes.append(
                "ac.output_rates = array<i64: "
                + ", ".join(str(rate) for rate in rates)
                + ">"
            )
        return "{" + ", ".join(attributes) + "}"

    def emit_queue(
        queue: QueueBinding,
        output_ssa: str | None,
        mapping: dict[str, str],
        indent: str,
    ) -> None:
        if queue.input_name is None and queue.rule_name is None:
            assert output_ssa is not None
            lines.append(
                f"{indent}%{output_ssa} = ac.source depth {queue.depth} "
                f"latency {queue.latency} "
                f"{queue_attributes(queue.name, (queue.rate,))} : "
                f"!ac.queue<{_render_type(queue.payload)}>"
            )
            mapping[queue.name] = output_ssa
            return
        assert queue.argument is not None
        if queue.rule_name is None:
            assert queue.expression is not None
        if queue.rule_name is not None:
            rule_input_names = queue.rule_input_names
            rule_arguments = queue.rule_arguments
            rule_payloads = queue.rule_payloads
            selected_input_names = tuple(
                effective_input.get((queue.name, index), name)
                for index, name in enumerate(rule_input_names)
            )
            input_ssas = tuple(mapping[name] for name in selected_input_names)
            root_names = tuple(
                "item" if len(rule_arguments) == 1 else f"item{index}"
                for index in range(len(rule_arguments))
            )
            root_values = {
                argument: (root_name, payload)
                for argument, root_name, payload in zip(
                    rule_arguments, root_names, rule_payloads, strict=True
                )
            }
            rule_table_views: dict[str, tuple[str, ast.expr, ValueType]] = {}
            if queue.rule_table_read_name is not None:
                assert queue.rule_table is not None
                assert queue.rule_table_read_index is not None
                entry_type, _ = table_domains[queue.rule_table]
                rule_table_views[queue.rule_table_read_name] = (
                    queue.rule_table,
                    queue.rule_table_read_index,
                    entry_type,
                )
            emitter = _ExpressionEmitter(
                payloads,
                queue.argument,
                queue.payload,
                root_name="item",
                root_values=root_values,
                table_views=rule_table_views,
                state_views={
                    owner.argument: (
                        owner.variable,
                        owner.value_type,
                        owner.entries,
                    )
                    for owner in queue.rule_state_owners
                },
                bitfields=bitfields,
            )
            rule_expressions: list[ast.expr] = []
            if queue.expression is not None:
                rule_expressions.append(queue.expression)
            if queue.rule_guard is not None:
                rule_expressions.append(queue.rule_guard)
            if queue.rule_effect_guard is not None:
                rule_expressions.append(queue.rule_effect_guard)
            if queue.rule_output_guard is not None:
                rule_expressions.append(queue.rule_output_guard)
            rule_expressions.extend(local.value for local in queue.rule_locals)
            for find in queue.rule_finds:
                rule_expressions.append(find.predicate)
                if find.key is not None:
                    rule_expressions.append(find.key)
            rule_expressions.extend(
                read.index for read in queue.rule_state_reads if read.index is not None
            )
            for write in queue.rule_state_writes:
                if write.guard is not None:
                    rule_expressions.append(write.guard)
                if write.index is not None:
                    rule_expressions.append(write.index)
                rule_expressions.append(write.value)
            referenced_names = {
                node.id
                for expression in rule_expressions
                for node in ast.walk(expression)
                if isinstance(node, ast.Name)
            }
            for state_owner in queue.rule_state_owners:
                if (
                    state_owner.entries != 1
                    or state_owner.argument in emitter.root_values
                    or state_owner.argument not in referenced_names
                ):
                    continue
                state_read = emitter._new()
                emitter.lines.append(
                    f"    %{state_read} = ac.var.read @{state_owner.variable} : "
                    f"!ac.var<{_render_type(state_owner.value_type)}>"
                )
                emitter.root_values[state_owner.argument] = (
                    state_read,
                    state_owner.value_type,
                )
            for state_read_binding in queue.rule_state_reads:
                assert state_read_binding.index is not None
                read_index, read_index_type = emitter.emit(state_read_binding.index)
                read_index_width = _epoch_05_integer_width(read_index_type)
                if read_index_width is None:
                    raise QueueFrontendError(
                        "ACPY-RULE-008: persistent list read index must be an "
                        "exact-width integer"
                    )
                emitter.reject_constant_index_outside(
                    read_index,
                    read_index_type,
                    state_read_binding.entries,
                    "ACPY-RULE-008: persistent list read index is out of range",
                )
                state_read = emitter._new()
                emitter.lines.append(
                    f"    %{state_read} = ac.var.read_element "
                    f"@{state_read_binding.variable}[%{read_index}] : "
                    f"!ac.var<{_render_type(read_index_type)}> -> "
                    f"!ac.var<{_render_type(state_read_binding.value_type)}>"
                )
                emitter.root_values[state_read_binding.name] = (
                    state_read,
                    state_read_binding.value_type,
                )
            for find in queue.rule_finds:
                index_width = max(1, (find.entries - 1).bit_length())
                predicate_emitter = _ExpressionEmitter(
                    payloads,
                    find.predicate_argument,
                    find.value_type,
                    root_name="entry",
                    root_values=emitter.root_values,
                    prefix=f"find{emitter.index}_predicate_",
                    state_views=emitter.state_views,
                    bitfields=bitfields,
                )
                predicate, predicate_type = predicate_emitter.emit(
                    find.predicate, BoolType()
                )
                if not _is_epoch_05_bool_compatible(predicate_type):
                    raise QueueFrontendError(
                        "ACPY-RULE-009: find where predicate must lower to bool"
                    )
                mask = emitter._new()
                emitter.lines.append(
                    f"    %{mask} = ac.var.match @{find.variable} predicate {{"
                )
                emitter.lines.append(
                    f"    ^predicate(%entry: !ac.var<{_render_type(find.value_type)}>):"
                )
                emitter.lines.extend(predicate_emitter.lines)
                emitter.lines.append(
                    f"      ac.var.match.yield %{predicate} : !ac.var<i1>"
                )
                emitter.lines.append(f"    }} -> !ac.var<i{find.entries}>")
                selected_index = emitter._new()
                selected_valid = emitter._new()
                if find.key is None:
                    emitter.lines.append(
                        f"    %{selected_index}, %{selected_valid} = "
                        f"ac.var.choose @{find.variable} %{mask} : "
                        f'!ac.var<i{find.entries}> count 1 policy "first" '
                        f"key {{}} -> !ac.var<i{index_width}>, !ac.var<i1>"
                    )
                else:
                    assert find.key_argument is not None
                    key_emitter = _ExpressionEmitter(
                        payloads,
                        find.key_argument,
                        find.value_type,
                        root_name="entry",
                        root_values=emitter.root_values,
                        prefix=f"find{emitter.index}_key_",
                        state_views=emitter.state_views,
                        bitfields=bitfields,
                    )
                    key, key_type = key_emitter.emit(find.key)
                    if _epoch_05_integer_width(key_type) is None:
                        raise QueueFrontendError(
                            "ACPY-RULE-009: find key must lower to an integer"
                        )
                    emitter.lines.append(
                        f"    %{selected_index}, %{selected_valid} = "
                        f"ac.var.choose @{find.variable} %{mask} : "
                        f'!ac.var<i{find.entries}> count 1 policy "min" key {{'
                    )
                    emitter.lines.append(
                        f"    ^key(%entry: !ac.var<{_render_type(find.value_type)}>):"
                    )
                    emitter.lines.extend(key_emitter.lines)
                    emitter.lines.append(
                        f"      ac.var.choose.yield %{key} : "
                        f"!ac.var<{_render_type(key_type)}>"
                    )
                    emitter.lines.append(
                        f"    }} -> !ac.var<i{index_width}>, !ac.var<i1>"
                    )
                emitter.find_values[find.name] = (
                    selected_index,
                    BitsType(index_width),
                    selected_valid,
                    BoolType(),
                    find.variable,
                    find.value_type,
                    None,
                )
            for local in queue.rule_locals:
                local_value, local_type = emitter.emit(local.value)
                emitter.root_values[local.name] = (local_value, local_type)
            if queue.rule_var is not None:
                assert queue.rule_var_argument is not None
                if queue.rule_var_index is None:
                    state_read = emitter._new()
                    emitter.lines.append(
                        f"    %{state_read} = ac.var.read @{queue.rule_var} : "
                        f"!ac.var<{_render_type(queue.payload)}>"
                    )
                    emitter.root_values[queue.rule_var_argument] = (
                        state_read,
                        queue.payload,
                    )
                elif queue.rule_var_read_name is not None:
                    assert queue.rule_var_read_index is not None
                    read_index, read_index_type = emitter.emit(
                        queue.rule_var_read_index
                    )
                    if _epoch_05_integer_width(read_index_type) is None:
                        raise QueueFrontendError(
                            "ACPY-RULE-004: persistent list index must be an "
                            "exact-width integer"
                        )
                    state_read = emitter._new()
                    emitter.lines.append(
                        f"    %{state_read} = ac.var.read_element "
                        f"@{queue.rule_var}[%{read_index}] : "
                        f"!ac.var<{_render_type(read_index_type)}> -> "
                        f"!ac.var<{_render_type(queue.payload)}>"
                    )
                    emitter.root_values[queue.rule_var_read_name] = (
                        state_read,
                        queue.payload,
                    )
            guard_result: str | None = None
            if queue.rule_guard is not None:
                guard_result, guard_type = emitter.emit(queue.rule_guard, BoolType())
                if not _is_epoch_05_bool_compatible(guard_type):
                    raise QueueFrontendError(
                        "ACPY-RULE-007: rule condition must lower to bool"
                    )
            effect_guard_result: str | None = None
            if queue.rule_effect_guard is not None:
                effect_guard_result, effect_guard_type = emitter.emit(
                    queue.rule_effect_guard, BoolType()
                )
                if not _is_epoch_05_bool_compatible(effect_guard_type):
                    raise QueueFrontendError(
                        "ACPY-RULE-010: conditional effect must lower to bool"
                    )
            output_guard_result: str | None = None
            if queue.rule_output_guard is not None:
                output_guard_result, output_guard_type = emitter.emit(
                    queue.rule_output_guard, BoolType()
                )
                if not _is_epoch_05_bool_compatible(output_guard_type):
                    raise QueueFrontendError(
                        "ACPY-RULE-012: optional output condition must lower to bool"
                    )
            condition_result = guard_result
            if effect_guard_result is not None:
                condition_result = emitter._new()
                emitter.lines.append(
                    f"    %{condition_result} = ac.var.constant true as !ac.var<i1>"
                )
            elif output_guard_result is not None or any(
                write.guard is not None for write in queue.rule_state_writes
            ):
                condition_result = emitter._new()
                emitter.lines.append(
                    f"    %{condition_result} = ac.var.constant true as !ac.var<i1>"
                )
            index_result: str | None = None
            index_type: ValueType | None = None
            write_result: str | None = None
            if queue.rule_table is not None:
                assert queue.rule_table_index is not None
                assert queue.rule_table_value is not None
                index_result, index_type = emitter.emit(queue.rule_table_index)
                index_width = _epoch_05_integer_width(index_type)
                if index_width is None:
                    raise QueueFrontendError(
                        "ACPY-RULE-004: stateful rule Table index must be an "
                        "exact-width integer"
                    )
                _, entries = table_domains[queue.rule_table]
                emitter.reject_constant_index_outside(
                    index_result,
                    index_type,
                    entries,
                    "ACPY-RULE-004: stateful rule Table index is out of range",
                )
                write_result, write_type = emitter.emit(queue.rule_table_value)
                if not _types_equal_in_epoch_05(write_type, queue.payload):
                    raise QueueFrontendError(
                        "ACPY-RULE-004: stateful rule assignment must write "
                        "one complete Table Entry"
                    )
            var_write_result: str | None = None
            var_index_result: str | None = None
            var_index_type: ValueType | None = None
            if queue.rule_var is not None:
                assert queue.rule_var_argument is not None
                assert queue.rule_var_value is not None
                if queue.rule_var_index is not None:
                    var_index_result, var_index_type = emitter.emit(
                        queue.rule_var_index
                    )
                    var_index_width = _epoch_05_integer_width(var_index_type)
                    if var_index_width is None:
                        raise QueueFrontendError(
                            "ACPY-RULE-004: persistent list index must be an "
                            "exact-width integer"
                        )
                    _, entries = variable_domains[queue.rule_var]
                    emitter.reject_constant_index_outside(
                        var_index_result,
                        var_index_type,
                        entries,
                        "ACPY-RULE-004: persistent list index is out of range",
                    )
                var_write_result, var_write_type = emitter.emit(queue.rule_var_value)
                if not _types_equal_in_epoch_05(var_write_type, queue.payload):
                    raise QueueFrontendError(
                        "ACPY-RULE-004: persistent variable assignment must "
                        "preserve its declared type"
                    )
                emitter.root_values[queue.rule_var_argument] = (
                    var_write_result,
                    queue.payload,
                )
            multi_state_results: list[
                tuple[
                    RuleStateWriteBinding,
                    ValueType | None,
                    str | None,
                    str,
                    str | None,
                ]
            ] = []
            branch_guard_results: dict[tuple[str, bool], str] = {}

            def emit_state_guard(state_write: RuleStateWriteBinding) -> str | None:
                if state_write.guard is None:
                    return None
                guard_key = ast.dump(state_write.guard, include_attributes=False)
                cache_key = (guard_key, state_write.guard_negated)
                cached = branch_guard_results.get(cache_key)
                if cached is not None:
                    return cached
                base_key = (guard_key, False)
                base_result = branch_guard_results.get(base_key)
                if base_result is None:
                    base_result, base_type = emitter.emit(state_write.guard, BoolType())
                    if not _is_epoch_05_bool_compatible(base_type):
                        raise QueueFrontendError(
                            "ACPY-RULE-011: branch condition must lower to bool"
                        )
                    branch_guard_results[base_key] = base_result
                if not state_write.guard_negated:
                    return base_result
                false_value = emitter._new()
                emitter.lines.append(
                    f"    %{false_value} = ac.var.constant false as !ac.var<i1>"
                )
                result = emitter._new()
                emitter.lines.append(
                    f'    %{result} = ac.var.cmp "eq" %{base_result}, '
                    f"%{false_value} : !ac.var<i1> -> !ac.var<i1>"
                )
                branch_guard_results[cache_key] = result
                return result

            def emit_state_value(state_write: RuleStateWriteBinding) -> str:
                value, value_type = emitter.emit(
                    state_write.value, state_write.value_type
                )
                if not _types_equal_in_epoch_05(value_type, state_write.value_type):
                    raise QueueFrontendError(
                        "ACPY-RULE-008: persistent state assignment must "
                        "preserve its declared type"
                    )
                return value

            def emit_state_index(
                state_write: RuleStateWriteBinding,
            ) -> tuple[str | None, ValueType | None]:
                if state_write.index is None:
                    return None, None
                index, index_type = emitter.emit(state_write.index)
                index_width = _epoch_05_integer_width(index_type)
                if index_width is None:
                    raise QueueFrontendError(
                        "ACPY-RULE-008: persistent list index must be an "
                        "exact-width integer"
                    )
                emitter.reject_constant_index_outside(
                    index,
                    index_type,
                    state_write.entries,
                    "ACPY-RULE-008: persistent list index is out of range",
                )
                return index, index_type

            writes_by_variable: dict[str, list[RuleStateWriteBinding]] = {}
            for state_write in queue.rule_state_writes:
                writes_by_variable.setdefault(state_write.variable, []).append(
                    state_write
                )
            for owner_writes in writes_by_variable.values():
                if len(owner_writes) == 2:
                    true_write = next(
                        write for write in owner_writes if not write.guard_negated
                    )
                    false_write = next(
                        write for write in owner_writes if write.guard_negated
                    )
                    condition = emit_state_guard(true_write)
                    assert condition is not None
                    true_index, true_index_type = emit_state_index(true_write)
                    false_index, false_index_type = emit_state_index(false_write)
                    if not _types_equal_in_epoch_05(true_index_type, false_index_type):
                        raise QueueFrontendError(
                            "ACPY-RULE-011: same-owner branch indices must have "
                            "one exact type"
                        )
                    true_value = emit_state_value(true_write)
                    false_value = emit_state_value(false_write)
                    selected_value = emitter._new()
                    emitter.lines.append(
                        f"    %{selected_value} = ac.var.select %{condition}, "
                        f"%{true_value}, %{false_value} : !ac.var<i1>, "
                        f"!ac.var<{_render_type(true_write.value_type)}> -> "
                        f"!ac.var<{_render_type(true_write.value_type)}>"
                    )
                    selected_index: str | None = None
                    if true_index is not None:
                        assert false_index is not None
                        assert true_index_type is not None
                        selected_index = emitter._new()
                        emitter.lines.append(
                            f"    %{selected_index} = ac.var.select %{condition}, "
                            f"%{true_index}, %{false_index} : !ac.var<i1>, "
                            f"!ac.var<{_render_type(true_index_type)}> -> "
                            f"!ac.var<{_render_type(true_index_type)}>"
                        )
                    multi_state_results.append(
                        (
                            true_write,
                            selected_index,
                            true_index_type,
                            selected_value,
                            None,
                        )
                    )
                    emitter.root_values[true_write.argument] = (
                        selected_value,
                        true_write.value_type,
                    )
                    continue
                state_write = owner_writes[0]
                state_guard_result = emit_state_guard(state_write)
                state_index, state_index_type = emit_state_index(state_write)
                state_value = emit_state_value(state_write)
                multi_state_results.append(
                    (
                        state_write,
                        state_index,
                        state_index_type,
                        state_value,
                        state_guard_result,
                    )
                )
                if state_write.index is None:
                    emitter.root_values[state_write.argument] = (
                        state_value,
                        state_write.value_type,
                    )
            result: str | None = None
            if queue.rule_has_output:
                assert queue.expression is not None
                result, result_type = emitter.emit(queue.expression)
                if not _types_equal_in_epoch_05(result_type, queue.payload):
                    raise QueueFrontendError(
                        "ACPY-RULE-004: rule result must preserve Queue payload type"
                    )
                assert output_ssa is not None
            lines.append(
                (f"{indent}%{output_ssa} = " if output_ssa is not None else indent)
                + "ac.rule "
                + ", ".join(f"%{value}" for value in input_ssas)
                + " "
                + (
                    f"depths [{queue.depth}] latencies [{queue.latency}] "
                    if queue.rule_has_output
                    else "depths [] latencies [] "
                )
                + f"name {json.dumps(queue.rule_name)} "
                f"stable_id {json.dumps('/'.join((*queue.scope, queue.name)))} "
                f'domain "cycle" type exact input_fact committed_input {{'
            )
            block_arguments = ", ".join(
                f"%{root_name}: !ac.var<{_render_type(payload)}>"
                for root_name, payload in zip(root_names, rule_payloads, strict=True)
            )
            lines.append(f"{indent}^rule({block_arguments}):")
            lines.extend(indent + line[2:] for line in emitter.lines)
            if condition_result is not None:
                lines.append(
                    f"{indent}  ac.rule.condition %{condition_result} : !ac.var<i1>"
                )
            effect_presence = (
                f" when %{effect_guard_result} : !ac.var<i1>"
                if effect_guard_result is not None
                else (
                    f" when %{condition_result} : !ac.var<i1>"
                    if output_guard_result is not None
                    else ""
                )
            )
            if queue.rule_var is not None:
                assert var_write_result is not None
                if queue.rule_var_index is None:
                    lines.append(
                        f"{indent}  ac.var.assign @{queue.rule_var} = "
                        f"%{var_write_result}{effect_presence} : "
                        f"!ac.var<{_render_type(queue.payload)}>"
                    )
                else:
                    assert var_index_result is not None
                    assert var_index_type is not None
                    lines.append(
                        f"{indent}  ac.var.assign_element @{queue.rule_var}"
                        f"[%{var_index_result}] = %{var_write_result}"
                        f"{effect_presence} : "
                        f"!ac.var<{_render_type(var_index_type)}>, "
                        f"!ac.var<{_render_type(queue.payload)}>"
                    )
            for (
                state_write,
                state_index,
                state_index_type,
                state_value,
                state_guard_result,
            ) in multi_state_results:
                state_effect_presence = (
                    effect_presence
                    if state_guard_result is None
                    else f" when %{state_guard_result} : !ac.var<i1>"
                )
                if state_index is None:
                    lines.append(
                        f"{indent}  ac.var.assign @{state_write.variable} = "
                        f"%{state_value}{state_effect_presence} : "
                        f"!ac.var<{_render_type(state_write.value_type)}>"
                    )
                else:
                    assert state_index_type is not None
                    lines.append(
                        f"{indent}  ac.var.assign_element "
                        f"@{state_write.variable}[%{state_index}] = "
                        f"%{state_value}{state_effect_presence} : "
                        f"!ac.var<{_render_type(state_index_type)}>, "
                        f"!ac.var<{_render_type(state_write.value_type)}>"
                    )
            if queue.rule_table is not None:
                assert index_result is not None
                assert index_type is not None
                assert write_result is not None
                fields = json.dumps(list(queue.rule_write_fields))
                lines.append(
                    f"{indent}  ac.table.propose @{queue.rule_table} "
                    f"[%{index_result}] = %{write_result}{effect_presence} "
                    f'mode "replace" '
                    f"write_fields {fields} : !ac.var<{_render_type(index_type)}>, "
                    f"!ac.var<{_render_type(queue.payload)}>"
                )
            if queue.rule_has_output:
                assert result is not None
                lines.append(
                    f"{indent}  %rule_ready = ac.marker.obligation %{result} "
                    f"state pending resolver handshake origin "
                    f'{json.dumps(queue.rule_name + ":return")} path "true" : '
                    f"!ac.var<{_render_type(queue.payload)}>"
                )
                if output_guard_result is not None:
                    lines.append(
                        f"{indent}  ac.rule.output %{result} when "
                        f"%{output_guard_result} ordinal 0 : "
                        f"!ac.var<{_render_type(queue.payload)}>, !ac.var<i1>"
                    )
                lines.append(
                    f"{indent}  ac.rule.return %rule_ready : "
                    f"!ac.var<{_render_type(queue.payload)}>"
                )
            else:
                lines.append(f"{indent}  ac.rule.return")
            lines.append(
                f"{indent}}} {queue_attributes(queue.name, (queue.rate,))} : "
                f"("
                + ", ".join(
                    f"!ac.queue<{_render_type(payload)}>" for payload in rule_payloads
                )
                + ") -> "
                + (
                    f"!ac.queue<{_render_type(queue.payload)}> "
                    if queue.rule_has_output
                    else "() "
                )
                + f'loc("<queue-model>":{queue.rule_source_line}:'
                f"{queue.rule_source_column})"
            )
            if output_ssa is not None:
                mapping[queue.name] = output_ssa
            return
        assert output_ssa is not None
        assert queue.input_name is not None
        input_name = effective_input.get((queue.name, 0), queue.input_name)
        input_ssa = mapping[input_name]
        emitter = _ExpressionEmitter(
            payloads,
            queue.argument,
            queue.payload,
            bitfields=bitfields,
        )
        result, result_type = emitter.emit(queue.expression)
        if not _types_equal_in_epoch_05(result_type, queue.payload):
            raise QueueFrontendError(
                "ACPY-QUEUE-003: lambda result must preserve Queue payload type"
            )
        lines.append(
            f"{indent}%{output_ssa} = ac.transform %{input_ssa} "
            f"depths [{queue.depth}] latencies [{queue.latency}] {{"
        )
        lines.append(
            f"{indent}^transform(%item: !ac.var<{_render_type(queue.payload)}>):"
        )
        lines.extend(indent + line[2:] for line in emitter.lines)
        lines.append(
            f"{indent}  ac.transform.yield %{result} : "
            f"!ac.var<{_render_type(queue.payload)}>"
        )
        lines.append(
            f"{indent}}} {queue_attributes(queue.name, (queue.rate,))} : "
            f"(!ac.queue<{_render_type(queue.payload)}>) -> "
            f"!ac.queue<{_render_type(queue.payload)}>"
        )
        mapping[queue.name] = output_ssa

    def render_items(
        path: tuple[str, ...], mapping: dict[str, str], indent: str
    ) -> None:
        def visible_order(consumer: QueueBinding) -> int:
            if consumer.scope == path:
                return consumer.order
            child_path = (*path, consumer.scope[len(path)])
            return next(
                scope.order for scope in program.scopes if scope.path == child_path
            )

        events: list[tuple[float, str, object]] = []
        events.extend(
            (queue.order, "queue", queue)
            for queue in program.queues
            if queue.scope == path
            and queue.name not in module_inputs
            and not queue.route_output
            and not queue.feedback_output
            and not queue.merge_output
            and not queue.reorder_output
            and not queue.dependency_output
            and not queue.credit_output
            and not queue.memory_output
            and not queue.table_read_output
            and not queue.barrier_output
            and not queue.select_output
        )
        events.extend(
            (rule.order, "effect_rule", rule)
            for rule in program.effect_rules
            if rule.scope == path
        )
        events.extend(
            (fork.order, "fork", fork) for fork in program.forks if fork.scope == path
        )
        events.extend(
            (route.order, "route", route)
            for route in program.routes
            if route.scope == path
        )
        events.extend(
            (merge.order, "merge", merge)
            for merge in program.merges
            if merge.scope == path
        )
        events.extend(
            (feedback.order, "feedback", feedback)
            for feedback in program.feedbacks
            if feedback.scope == path
        )
        events.extend(
            (reorder.order, "reorder", reorder)
            for reorder in program.reorders
            if reorder.scope == path
        )
        events.extend(
            (dependency.order, "dependency", dependency)
            for dependency in program.dependencies
            if dependency.scope == path
        )
        events.extend(
            (credit.order, "credit", credit)
            for credit in program.credits
            if credit.scope == path
        )
        events.extend(
            (barrier.order, "barrier", barrier)
            for barrier in program.barriers
            if barrier.scope == path
        )
        events.extend(
            (select.order, "select", select)
            for select in program.selects
            if select.scope == path
        )
        events.extend(
            (request.order, "memory_request", request)
            for request in program.memory_requests
            if request.scope == path
        )
        events.extend(
            (read.order, "table_read", read)
            for read in program.table_reads
            if read.scope == path
        )
        events.extend(
            (candidate.order, "table_match", candidate)
            for candidate in program.candidates
            if candidate.scope == path
        )
        events.extend(
            (selection.order, "table_choose", selection)
            for selection in program.selections
            if selection.scope == path
        )
        events.extend(
            (write.order, "table_write", write)
            for write in program.table_writes
            if write.scope == path
        )
        events.extend(
            (write.order, "masked_table_write", write)
            for write in program.masked_table_writes
            if write.scope == path
        )
        events.extend(
            (slot.order, "slot", slot) for slot in program.slots if slot.scope == path
        )
        events.extend(
            (release.order, "slot_release", release)
            for release in program.slot_releases
            if release.scope == path
        )
        events.extend(
            (
                min(visible_order(consumer) for consumer, _ in group) - 0.4,
                "broadcast",
                source,
            )
            for source, (fanout_scope, group) in fanouts.items()
            if fanout_scope == path
        )
        events.extend(
            (scope.order, "scope", scope)
            for scope in program.scopes
            if scope.path[:-1] == path
        )
        events.extend(
            (observation.order, "observe", observation)
            for observation in program.observations
            if observation.scope == path
        )
        events.extend(
            (expectation.order, "expect", expectation)
            for expectation in program.expectations
            if expectation.scope == path
        )
        events.extend(
            (sink_binding.order, "sink", sink_binding)
            for sink_binding in program.sinks
            if sink_binding.scope == path and sink_binding.queue not in module_outputs
        )
        for _, kind, item in sorted(events, key=lambda event: event[0]):
            if kind in {"queue", "effect_rule"}:
                queue = item
                assert isinstance(queue, QueueBinding)
                output = (
                    (queue.name if not path else f"{queue.name}__local")
                    if queue.rule_has_output
                    else None
                )
                emit_queue(queue, output, mapping, indent)
            elif kind == "table_match":
                candidate = item
                assert isinstance(candidate, CandidateSetBinding)
                table = next(
                    value for value in program.tables if value.name == candidate.table
                )
                emitter = _ExpressionEmitter(
                    payloads,
                    candidate.argument,
                    table.entry_type,
                    root_name="entry",
                    prefix=f"match_{candidate.order}_",
                    slot_views=slot_views,
                    bitfields=bitfields,
                )
                predicate, predicate_type = emitter.emit(
                    candidate.predicate, BoolType()
                )
                if not _is_epoch_05_bool_compatible(predicate_type):
                    raise QueueFrontendError(
                        "ACPY-TABLE-006: match predicate must lower to i1"
                    )
                result = f"table_match_{candidate.order}"
                lines.append(
                    f"{indent}%{result} = ac.table.match @{candidate.table} "
                    "predicate {"
                )
                lines.append(
                    f"{indent}^predicate(%entry: "
                    f"!ac.var<{_render_type(table.entry_type)}>):"
                )
                lines.extend(indent + line[2:] for line in emitter.lines)
                lines.append(
                    f"{indent}  ac.table.match.yield %{predicate} : !ac.var<i1>"
                )
                lines.append(f"{indent}}} -> !ac.var<i{table.entries}>")
                materialized_candidates[candidate.name] = (
                    result,
                    BitsType(table.entries),
                )
            elif kind == "table_choose":
                selection = item
                assert isinstance(selection, SelectionBinding)
                table = next(
                    value for value in program.tables if value.name == selection.table
                )
                mask, mask_type = materialized_candidates[selection.candidates]
                index = f"table_choose_{selection.order}_index"
                valid = f"table_choose_{selection.order}_valid"
                index_type = BitsType(max(1, (table.entries - 1).bit_length()))
                if selection.policy == "first":
                    key_region = "{}"
                else:
                    assert selection.argument is not None and selection.key is not None
                    emitter = _ExpressionEmitter(
                        payloads,
                        selection.argument,
                        table.entry_type,
                        root_name="entry",
                        prefix=f"choose_{selection.order}_",
                        bitfields=bitfields,
                    )
                    key, key_type = emitter.emit(selection.key)
                    if _epoch_05_integer_width(key_type) is None:
                        raise QueueFrontendError(
                            "ACPY-TABLE-007: choose key must lower to an integer"
                        )
                    key_lines = ["{"]
                    key_lines.append(
                        f"{indent}^key(%entry: "
                        f"!ac.var<{_render_type(table.entry_type)}>):"
                    )
                    key_lines.extend(indent + line[2:] for line in emitter.lines)
                    key_lines.append(
                        f"{indent}  ac.table.choose.yield %{key} : "
                        f"!ac.var<{_render_type(key_type)}>"
                    )
                    key_lines.append(f"{indent}}}")
                    key_region = "\n".join(key_lines)
                lines.append(
                    f"{indent}%{index}, %{valid} = ac.table.choose "
                    f"@{selection.table} %{mask} : "
                    f"!ac.var<{_render_type(mask_type)}> count 1 "
                    f'policy "{selection.policy}" key {key_region} -> '
                    f"!ac.var<{_render_type(index_type)}>, !ac.var<i1>"
                )
                materialized_selections[selection.name] = (
                    index,
                    index_type,
                    valid,
                    BoolType(),
                )
            elif kind == "scope":
                scope = item
                assert isinstance(scope, ScopeBinding)
                render_scope(scope, mapping, indent)
            elif kind == "broadcast":
                source = item
                assert isinstance(source, str)
                _, group = fanouts[source]
                outputs = [f"{source}__fanout{index}" for index in range(len(group))]
                lhs = ", ".join(f"%{name}" for name in outputs)
                depths = ", ".join("1" for _ in outputs)
                payload = payload_by_queue[source]
                output_types = ", ".join(
                    f"!ac.queue<{_render_type(payload)}>" for _ in outputs
                )
                lines.append(
                    f"{indent}{lhs} = ac.broadcast %{mapping[source]} depths "
                    f"[{depths}] latencies [{depths}] "
                    f"{{ac.output_names = {name_array(outputs)}}} : "
                    f"!ac.queue<{_render_type(payload)}> -> "
                    f"({output_types})"
                )
                for (consumer, input_index), output in zip(group, outputs, strict=True):
                    mapping[effective_input[(consumer.name, input_index)]] = output
            elif kind == "barrier":
                barrier = item
                assert isinstance(barrier, BarrierBinding)
                output_names = [
                    name if not path else f"{name}__local" for name in barrier.outputs
                ]
                lhs = ", ".join(f"%{name}" for name in output_names)
                operands = ", ".join(
                    f"%{mapping[input_name]}" for input_name in barrier.inputs
                )
                depths = ", ".join(str(barrier.depth) for _ in output_names)
                latencies = ", ".join(str(barrier.latency) for _ in output_names)
                input_types = ", ".join(
                    f"!ac.queue<{_render_type(by_name[input_name].payload)}>"
                    for input_name in barrier.inputs
                )
                output_types = ", ".join(
                    f"!ac.queue<{_render_type(by_name[input_name].payload)}>"
                    for input_name in barrier.inputs
                )
                lines.append(
                    f"{indent}{lhs} = ac.barrier {operands} depths [{depths}] "
                    f"latencies [{latencies}] "
                    f"{{ac.output_names = {name_array(barrier.outputs)}}} : "
                    f"({input_types}) -> ({output_types})"
                )
                for name, output in zip(barrier.outputs, output_names, strict=True):
                    mapping[name] = output
            elif kind == "select":
                select = item
                assert isinstance(select, SelectBinding)
                control = by_name[select.control]
                emitter = _ExpressionEmitter(
                    payloads,
                    select.argument,
                    control.payload,
                    bitfields=bitfields,
                )
                selector, selector_type = emitter.emit(select.selector)
                if _epoch_05_integer_width(selector_type) is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-018: select key must lower to an integer"
                    )
                output = select.output if not path else f"{select.output}__local"
                operands = ", ".join(
                    f"%{mapping[name]}" for name in (select.control, *select.inputs)
                )
                input_types = ", ".join(
                    f"!ac.queue<{_render_type(by_name[name].payload)}>"
                    for name in (select.control, *select.inputs)
                )
                lines.append(
                    f"{indent}%{output} = ac.select {operands} "
                    f"depth {select.depth} latency {select.latency} key {{"
                )
                lines.append(
                    f"{indent}^key(%item: !ac.var<{_render_type(control.payload)}>):"
                )
                lines.extend(indent + line[2:] for line in emitter.lines)
                lines.append(
                    f"{indent}  ac.select.yield %{selector} : "
                    f"!ac.var<{_render_type(selector_type)}>"
                )
                lines.append(
                    f'{indent}}} {{ac.name = "{select.output}"}} : '
                    f"({input_types}) -> "
                    f"!ac.queue<{_render_type(by_name[select.output].payload)}>"
                )
                mapping[select.output] = output
            elif kind == "route":
                route = item
                assert isinstance(route, RouteBinding)
                incoming = by_name[route.input_name]
                emitter = _ExpressionEmitter(
                    payloads,
                    route.argument,
                    incoming.payload,
                    bitfields=bitfields,
                )
                selector, selector_type = emitter.emit(route.selector)
                if route.boolean_selector and not _is_epoch_05_bool_compatible(
                    selector_type
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-011: runtime if condition must lower to bool"
                    )
                if not route.boolean_selector and not isinstance(
                    selector_type, BitsType
                ):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-006: route key must lower to an integer"
                    )
                output_names = [
                    name if not path else f"{name}__local" for name in route.outputs
                ]
                lhs = ", ".join(f"%{name}" for name in output_names)
                depths = ", ".join(str(route.depth) for _ in output_names)
                latencies = ", ".join(str(route.latency) for _ in output_names)
                output_types = ", ".join(
                    f"!ac.queue<{_render_type(incoming.payload)}>" for _ in output_names
                )
                lines.append(
                    f"{indent}{lhs} = ac.route %{mapping[route.input_name]} "
                    f"depths [{depths}] latencies [{latencies}] {{"
                )
                lines.append(
                    f"{indent}^selector(%item: "
                    f"!ac.var<{_render_type(incoming.payload)}>):"
                )
                lines.extend(indent + line[2:] for line in emitter.lines)
                lines.append(
                    f"{indent}  ac.route.yield %{selector} : "
                    f"!ac.var<{_render_type(selector_type)}>"
                )
                lines.append(
                    f"{indent}}} "
                    f"{{ac.output_names = {name_array(route.outputs)}}} : "
                    f"!ac.queue<{_render_type(incoming.payload)}> -> ({output_types})"
                )
                for name, output in zip(route.outputs, output_names, strict=True):
                    mapping[name] = output
            elif kind == "fork":
                fork = item
                assert isinstance(fork, ForkBinding)
                incoming = by_name[fork.input_name]
                output_names = [
                    name if not path else f"{name}__local" for name in fork.outputs
                ]
                lhs = ", ".join(f"%{name}" for name in output_names)
                depths = ", ".join(str(fork.depth) for _ in output_names)
                latencies = ", ".join(str(fork.latency) for _ in output_names)
                output_types = ", ".join(
                    f"!ac.queue<{_render_type(incoming.payload)}>" for _ in output_names
                )
                lines.append(
                    f"{indent}{lhs} = ac.fork %{mapping[fork.input_name]} "
                    f"depths [{depths}] latencies [{latencies}] "
                    f"{{ac.output_names = {name_array(fork.outputs)}}} : "
                    f"!ac.queue<{_render_type(incoming.payload)}> -> ({output_types})"
                )
                for name, output in zip(fork.outputs, output_names, strict=True):
                    mapping[name] = output
            elif kind == "feedback":
                feedback = item
                assert isinstance(feedback, FeedbackBinding)
                incoming = by_name[feedback.input_name]
                emitter = _ExpressionEmitter(
                    payloads,
                    feedback.argument,
                    incoming.payload,
                    bitfields=bitfields,
                )
                condition, condition_type = emitter.emit(feedback.condition)
                update, update_type = emitter.emit(feedback.update)
                if not _is_epoch_05_bool_compatible(
                    condition_type
                ) or not _types_equal_in_epoch_05(update_type, incoming.payload):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-007: while condition must be bool and update "
                        "must preserve Queue payload"
                    )
                output = (
                    feedback.output_name
                    if not path
                    else f"{feedback.output_name}__local"
                )
                lines.append(
                    f"{indent}%{output} = ac.feedback %{mapping[feedback.input_name]} "
                    f"depth {feedback.depth} latency {feedback.latency} "
                    f"max_iterations {feedback.max_iterations} {{"
                )
                lines.append(
                    f"{indent}^body(%item: !ac.var<{_render_type(incoming.payload)}>):"
                )
                lines.extend(indent + line[2:] for line in emitter.lines)
                lines.append(
                    f"{indent}  ac.feedback.yield %{update} continue %{condition} : "
                    f"!ac.var<{_render_type(incoming.payload)}>, !ac.var<i1>"
                )
                lines.append(
                    f'{indent}}} {{ac.name = "{feedback.output_name}"}} : '
                    f"!ac.queue<{_render_type(incoming.payload)}> -> "
                    f"!ac.queue<{_render_type(incoming.payload)}>"
                )
                mapping[feedback.output_name] = output
            elif kind == "reorder":
                reorder = item
                assert isinstance(reorder, ReorderBinding)
                incoming = by_name[reorder.input_name]
                emitter = _ExpressionEmitter(
                    payloads,
                    reorder.argument,
                    incoming.payload,
                    bitfields=bitfields,
                )
                key, key_type = emitter.emit(reorder.key)
                if _epoch_05_integer_width(key_type) is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-013: reorder key must lower to an integer"
                    )
                output = (
                    reorder.output_name if not path else f"{reorder.output_name}__local"
                )
                lines.append(
                    f"{indent}%{output} = ac.reorder "
                    f"%{mapping[reorder.input_name]} capacity {reorder.capacity} "
                    f"start {reorder.start} depth {reorder.depth} "
                    f"latency {reorder.latency} {{"
                )
                lines.append(
                    f"{indent}^key(%item: !ac.var<{_render_type(incoming.payload)}>):"
                )
                lines.extend(indent + line[2:] for line in emitter.lines)
                lines.append(
                    f"{indent}  ac.reorder.yield %{key} : "
                    f"!ac.var<{_render_type(key_type)}>"
                )
                lines.append(
                    f'{indent}}} {{ac.name = "{reorder.output_name}"}} : '
                    f"!ac.queue<{_render_type(incoming.payload)}> -> "
                    f"!ac.queue<{_render_type(incoming.payload)}>"
                )
                mapping[reorder.output_name] = output
            elif kind == "dependency":
                dependency = item
                assert isinstance(dependency, DependencyBinding)
                incoming = by_name[dependency.input_name]
                policies = (
                    ("key", dependency.key),
                    ("waits_for", dependency.waits_for),
                    ("resource", dependency.resource),
                    ("cost", dependency.cost),
                )
                emitted: list[tuple[str, ValueType, list[str]]] = []
                for policy_name, expression in policies:
                    emitter = _ExpressionEmitter(
                        payloads,
                        dependency.argument,
                        incoming.payload,
                        bitfields=bitfields,
                    )
                    value, value_type = emitter.emit(expression)
                    if _epoch_05_integer_width(value_type) is None:
                        raise QueueFrontendError(
                            "ACPY-QUEUE-014: dependency policies must lower to integers"
                        )
                    emitted.append((value, value_type, emitter.lines))
                if not _types_equal_in_epoch_05(emitted[0][1], emitted[1][1]):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-014: key and waits_for types must match"
                    )
                output = (
                    dependency.output_name
                    if not path
                    else f"{dependency.output_name}__local"
                )
                lines.append(
                    f"{indent}%{output} = ac.dependency "
                    f"%{mapping[dependency.input_name]} capacity "
                    f"{dependency.capacity} resources {dependency.resources} "
                    f"no_dependency "
                    f"{dependency.no_dependency} depth {dependency.depth} "
                    f"latency {dependency.latency} key {{"
                )
                for index, policy_name in enumerate(
                    ("key", "waits_for", "resource", "cost")
                ):
                    if index:
                        lines.append(f"{indent}}} {policy_name} {{")
                    lines.append(
                        f"{indent}^{policy_name}(%item: "
                        f"!ac.var<{_render_type(incoming.payload)}>):"
                    )
                    value, value_type, policy_lines = emitted[index]
                    lines.extend(indent + line[2:] for line in policy_lines)
                    lines.append(
                        f"{indent}  ac.dependency.yield %{value} : "
                        f"!ac.var<{_render_type(value_type)}>"
                    )
                lines.append(
                    f'{indent}}} {{ac.name = "{dependency.output_name}"}} : '
                    f"!ac.queue<{_render_type(incoming.payload)}> -> "
                    f"!ac.queue<{_render_type(incoming.payload)}>"
                )
                mapping[dependency.output_name] = output
            elif kind == "credit":
                credit = item
                assert isinstance(credit, CreditBinding)
                incoming = by_name[credit.input_name]
                emitter = _ExpressionEmitter(
                    payloads,
                    credit.argument,
                    incoming.payload,
                    bitfields=bitfields,
                )
                cost, cost_type = emitter.emit(credit.cost)
                if _epoch_05_integer_width(cost_type) is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-016: credit cost must lower to an integer"
                    )
                output = (
                    credit.output_name if not path else f"{credit.output_name}__local"
                )
                lines.append(
                    f"{indent}%{output} = ac.credit "
                    f"%{mapping[credit.input_name]} credits {credit.credits} "
                    f"depth {credit.depth} latency {credit.latency} cost {{"
                )
                lines.append(
                    f"{indent}^cost(%item: !ac.var<{_render_type(incoming.payload)}>):"
                )
                lines.extend(indent + line[2:] for line in emitter.lines)
                lines.append(
                    f"{indent}  ac.credit.yield %{cost} : "
                    f"!ac.var<{_render_type(cost_type)}>"
                )
                lines.append(
                    f'{indent}}} {{ac.name = "{credit.output_name}"}} : '
                    f"!ac.queue<{_render_type(incoming.payload)}> -> "
                    f"!ac.queue<{_render_type(incoming.payload)}>"
                )
                mapping[credit.output_name] = output
            elif kind == "memory_request":
                memory = item
                assert isinstance(memory, MemoryRequestBinding)
                incoming = by_name[memory.input_name]
                instance = next(
                    value
                    for value in program.memory_instances
                    if value.name == memory.instance
                )
                policies = (
                    ("address", memory.address),
                    ("write", memory.write),
                    ("data", memory.data),
                )
                emitted: list[tuple[str, ValueType, list[str]]] = []
                for policy_name, expression in policies:
                    emitter = _ExpressionEmitter(
                        payloads,
                        memory.argument,
                        incoming.payload,
                        bitfields=bitfields,
                    )
                    value, value_type = emitter.emit(expression)
                    emitted.append((value, value_type, emitter.lines))
                if _epoch_05_integer_width(emitted[0][1]) is None:
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory address must lower to an integer"
                    )
                if not _is_epoch_05_bool_compatible(emitted[1][1]):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory write must lower to bool"
                    )
                if not _types_equal_in_epoch_05(emitted[2][1], instance.data_type):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-015: memory data must match result_field"
                    )
                output = (
                    memory.output_name if not path else f"{memory.output_name}__local"
                )
                lines.append(
                    f"{indent}%{output} = ac.memory.request @{memory.instance}, "
                    f"%{mapping[memory.input_name]} ordinal "
                    f"{memory_ordinals[(memory.instance, memory.output_name)]} "
                    f'result_field "{memory.result_field}" '
                    f"depth {memory.depth} address {{"
                )
                for index, policy_name in enumerate(("address", "write", "data")):
                    if index:
                        lines.append(f"{indent}}} {policy_name} {{")
                    lines.append(
                        f"{indent}^{policy_name}(%item: "
                        f"!ac.var<{_render_type(incoming.payload)}>):"
                    )
                    value, value_type, policy_lines = emitted[index]
                    lines.extend(indent + line[2:] for line in policy_lines)
                    lines.append(
                        f"{indent}  ac.memory.yield %{value} : "
                        f"!ac.var<{_render_type(value_type)}>"
                    )
                lines.append(
                    f'{indent}}} {{ac.endpoint_path = "'
                    f'{"/" + "/".join((*memory.scope, memory.output_name))}", '
                    f'ac.name = "{memory.output_name}"}} : '
                    f"!ac.queue<{_render_type(incoming.payload)}> -> "
                    f"!ac.queue<{_render_type(incoming.payload)}>"
                )
                mapping[memory.output_name] = output
            elif kind == "table_read":
                read = item
                assert isinstance(read, TableReadBinding)
                table = next(
                    value for value in program.tables if value.name == read.table
                )
                input_payload = (
                    table.entry_type
                    if read.input_name is None
                    else by_name[read.input_name].payload
                )
                argument = read.argument or ""
                table_views = (
                    {
                        read.view_alias: (
                            read.table,
                            read.address,
                            table.entry_type,
                        )
                    }
                    if read.view_alias
                    else {}
                )
                address_emitter = _ExpressionEmitter(
                    payloads,
                    argument,
                    input_payload,
                    slot_views=slot_views,
                    candidates=candidate_views,
                    selections=selection_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                address, address_type = address_emitter.emit(read.address)
                when_emitter = _ExpressionEmitter(
                    payloads,
                    argument,
                    input_payload,
                    table_views=table_views,
                    slot_views=slot_views,
                    candidates=candidate_views,
                    selections=selection_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                condition, condition_type = when_emitter.emit(read.when, BoolType())
                if not isinstance(
                    address_type, BitsType
                ) or not _is_epoch_05_bool_compatible(condition_type):
                    raise QueueFrontendError(
                        "ACPY-TABLE-003: read address/when type mismatch"
                    )
                output = read.output_name if not path else f"{read.output_name}__local"
                operand = (
                    ""
                    if read.input_name is None
                    else f", %{mapping[read.input_name]} : "
                    f"!ac.queue<{_render_type(input_payload)}> "
                )
                lines.append(
                    f"{indent}%{output} = ac.table.read @{read.table}{operand}"
                    f" depth {read.depth} latency {read.latency} address {{"
                )
                block_argument = (
                    ""
                    if read.input_name is None
                    else f"(%item: !ac.var<{_render_type(input_payload)}>)"
                )
                lines.append(f"{indent}^address{block_argument}:")
                lines.extend(indent + line[2:] for line in address_emitter.lines)
                lines.append(
                    f"{indent}  ac.table.yield %{address} : "
                    f"!ac.var<{_render_type(address_type)}>"
                )
                lines.append(f"{indent}}} when {{")
                lines.append(f"{indent}^when{block_argument}:")
                lines.extend(indent + line[2:] for line in when_emitter.lines)
                lines.append(f"{indent}  ac.table.yield %{condition} : !ac.var<i1>")
                lines.append(
                    f'{indent}}} {{ac.endpoint_path = "'
                    f'{"/" + "/".join((*read.scope, read.output_name))}", '
                    f'ac.name = "{read.output_name}"}} -> '
                    f"!ac.queue<{_render_type(table.entry_type)}>"
                )
                mapping[read.output_name] = output
            elif kind == "table_write":
                write = item
                assert isinstance(write, TableWriteBinding)
                table = next(
                    value for value in program.tables if value.name == write.table
                )
                input_payload = (
                    table.entry_type
                    if write.input_name is None
                    else by_name[write.input_name].payload
                )
                argument = write.argument or ""
                address_emitter = _ExpressionEmitter(
                    payloads,
                    argument,
                    input_payload,
                    slot_views=slot_views,
                    candidates=candidate_views,
                    selections=selection_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                address, address_type = address_emitter.emit(write.address)
                enable_emitter = _ExpressionEmitter(
                    payloads,
                    argument,
                    input_payload,
                    slot_views=slot_views,
                    candidates=candidate_views,
                    selections=selection_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                enabled, enable_type = enable_emitter.emit(write.enable, BoolType())
                value_emitter = _ExpressionEmitter(
                    payloads,
                    argument,
                    input_payload,
                    table_views={
                        "__old": (write.table, write.address, table.entry_type)
                    },
                    slot_views=slot_views,
                    candidates=candidate_views,
                    selections=selection_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                if write.value is not None:
                    value, value_type = value_emitter.emit(
                        write.value, table.entry_type
                    )
                else:
                    patch_call = ast.Call(
                        func=ast.Attribute(
                            value=ast.Name(id="__old", ctx=ast.Load()),
                            attr="with_fields",
                            ctx=ast.Load(),
                        ),
                        args=[],
                        keywords=[
                            ast.keyword(arg=name, value=expression)
                            for name, expression in write.patch_fields
                        ],
                    )
                    value, value_type = value_emitter.emit(patch_call, table.entry_type)
                if (
                    _epoch_05_integer_width(address_type) is None
                    or not _is_epoch_05_bool_compatible(enable_type)
                    or not _types_equal_in_epoch_05(value_type, table.entry_type)
                ):
                    raise QueueFrontendError(
                        "ACPY-TABLE-004: write address/enable/value type mismatch"
                    )
                lines.append(
                    f"{indent}ac.table.write @{write.table}"
                    + (
                        ""
                        if write.input_name is None
                        else f", %{mapping[write.input_name]} : "
                        f"!ac.queue<{_render_type(input_payload)}>"
                    )
                    + f' mode "{write.write_mode}" write_fields ['
                    + ", ".join(f'"{field}"' for field in write.write_fields)
                    + "] address {"
                )
                block_argument = (
                    ""
                    if write.input_name is None
                    else f"(%item: !ac.var<{_render_type(input_payload)}>)"
                )
                policies = (
                    ("address", address, address_type, address_emitter.lines),
                    ("enable", enabled, enable_type, enable_emitter.lines),
                    ("value", value, value_type, value_emitter.lines),
                )
                for index, (
                    policy_name,
                    policy_value,
                    policy_type,
                    policy_lines,
                ) in enumerate(policies):
                    if index:
                        lines.append(f"{indent}}} {policy_name} {{")
                    lines.append(f"{indent}^{policy_name}{block_argument}:")
                    lines.extend(indent + line[2:] for line in policy_lines)
                    lines.append(
                        f"{indent}  ac.table.yield %{policy_value} : "
                        f"!ac.var<{_render_type(policy_type)}>"
                    )
                endpoint_base = (
                    f"{write.table}__allocate"
                    if write.write_mode == "replace"
                    else f"{write.table}__write"
                )
                prior_writes = sum(
                    candidate.table == write.table
                    and candidate.write_mode == write.write_mode
                    and candidate.order < write.order
                    for candidate in program.table_writes
                )
                endpoint_name = endpoint_base + (
                    "" if prior_writes == 0 else f"_{prior_writes}"
                )
                lines.append(
                    f'{indent}}} {{ac.endpoint_path = "'
                    f'{"/" + "/".join((*write.scope, endpoint_name))}", '
                    f'ac.name = "{endpoint_name}"}}'
                )
            elif kind == "masked_table_write":
                write = item
                assert isinstance(write, MaskedTableWriteBinding)
                table = next(
                    value for value in program.tables if value.name == write.table
                )
                mask_emitter = _ExpressionEmitter(
                    payloads,
                    "",
                    table.entry_type,
                    prefix=f"mask_{write.order}_",
                    slot_views=slot_views,
                    candidates=candidate_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                mask, mask_type = mask_emitter.emit(
                    ast.Name(id=write.candidates, ctx=ast.Load())
                )
                enable_emitter = _ExpressionEmitter(
                    payloads,
                    "",
                    table.entry_type,
                    prefix="enable_",
                    slot_views=slot_views,
                    candidates=candidate_views,
                    selections=selection_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                enabled, enable_type = enable_emitter.emit(write.enable, BoolType())
                value_emitter = _ExpressionEmitter(
                    payloads,
                    "__old",
                    table.entry_type,
                    root_name="old",
                    prefix="value_",
                    slot_views=slot_views,
                    candidates=candidate_views,
                    selections=selection_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                if write.value is not None:
                    value, value_type = value_emitter.emit(
                        write.value, table.entry_type
                    )
                else:
                    patch_call = ast.Call(
                        func=ast.Attribute(
                            value=ast.Name(id="__old", ctx=ast.Load()),
                            attr="with_fields",
                            ctx=ast.Load(),
                        ),
                        args=[],
                        keywords=[
                            ast.keyword(arg=name, value=expression)
                            for name, expression in write.patch_fields
                        ],
                    )
                    value, value_type = value_emitter.emit(patch_call, table.entry_type)
                if (
                    mask_type != BitsType(table.entries)
                    or not _is_epoch_05_bool_compatible(enable_type)
                    or not _types_equal_in_epoch_05(value_type, table.entry_type)
                ):
                    raise QueueFrontendError(
                        "ACPY-TABLE-008: masked write mask/enable/value type mismatch"
                    )
                lines.extend(indent + line[2:] for line in mask_emitter.lines)
                lines.append(
                    f"{indent}ac.table.masked_write @{write.table} %{mask} : "
                    f'!ac.var<{_render_type(mask_type)}> mode "field" write_fields ['
                    + ", ".join(f'"{field}"' for field in write.write_fields)
                    + "] enable {"
                )
                lines.append(f"{indent}^enable:")
                lines.extend(indent + line[2:] for line in enable_emitter.lines)
                lines.append(f"{indent}  ac.table.yield %{enabled} : !ac.var<i1>")
                lines.append(f"{indent}}} value {{")
                lines.append(
                    f"{indent}^value(%old: !ac.var<{_render_type(table.entry_type)}>):"
                )
                lines.extend(indent + line[2:] for line in value_emitter.lines)
                lines.append(
                    f"{indent}  ac.table.yield %{value} : "
                    f"!ac.var<{_render_type(value_type)}>"
                )
                prior_writes = sum(
                    candidate.table == write.table and candidate.order < write.order
                    for candidate in program.masked_table_writes
                )
                endpoint_name = f"{write.table}__masked_write" + (
                    "" if prior_writes == 0 else f"_{prior_writes}"
                )
                lines.append(
                    f'{indent}}} {{ac.endpoint_path = "'
                    f'{"/" + "/".join((*write.scope, endpoint_name))}", '
                    f'ac.name = "{endpoint_name}"}}'
                )
            elif kind == "slot":
                slot = item
                assert isinstance(slot, SlotBinding)
                owner = "/" + "/".join(slot.scope) if slot.scope else "/"
                stable_id = "slot/" + (
                    "/".join((*slot.scope, slot.name)) if slot.scope else slot.name
                )
                lines.append(
                    f"{indent}ac.slot @{slot.name}, %{mapping[slot.input_name]} "
                    f'owner "{owner}" stable_id "{stable_id}" : '
                    f"!ac.queue<{_render_type(slot.payload)}>"
                )
            elif kind == "slot_release":
                release = item
                assert isinstance(release, SlotReleaseBinding)
                slot = next(
                    value for value in program.slots if value.name == release.slot
                )
                emitter = _ExpressionEmitter(
                    payloads,
                    "",
                    slot.payload,
                    slot_views=slot_views,
                    candidates=candidate_views,
                    selections=selection_views,
                    candidate_values=materialized_candidates,
                    selection_values=materialized_selections,
                    table_domains=table_domains,
                    bitfields=bitfields,
                )
                condition, condition_type = emitter.emit(release.when, BoolType())
                if not _is_epoch_05_bool_compatible(condition_type):
                    raise QueueFrontendError(
                        "ACPY-SLOT-002: slot release condition must lower to i1"
                    )
                lines.append(f"{indent}ac.slot.release @{release.slot} when {{")
                lines.append(f"{indent}^when:")
                lines.extend(indent + line[2:] for line in emitter.lines)
                lines.append(f"{indent}  ac.slot.yield %{condition} : !ac.var<i1>")
                endpoint_name = f"{release.slot}__release"
                lines.append(
                    f'{indent}}} {{ac.endpoint_path = "'
                    f'{"/" + "/".join((*release.scope, endpoint_name))}", '
                    f'ac.name = "{endpoint_name}"}}'
                )
            elif kind == "merge":
                merge = item
                assert isinstance(merge, MergeBinding)
                output = merge.output if not path else f"{merge.output}__local"
                operands = ", ".join(f"%{mapping[name]}" for name in merge.inputs)
                input_types = ", ".join(
                    f"!ac.queue<{_render_type(by_name[name].payload)}>"
                    for name in merge.inputs
                )
                payload = by_name[merge.output].payload
                lines.append(
                    f'{indent}%{output} = ac.merge {operands} policy "{merge.policy}" '
                    f"depth {merge.depth} latency {merge.latency} "
                    f'{{ac.name = "{merge.output}"}} : '
                    f"({input_types}) -> !ac.queue<{_render_type(payload)}>"
                )
                mapping[merge.output] = output
            elif kind == "expect":
                expectation = item
                assert isinstance(expectation, ExpectBinding)
                queue = by_name[expectation.queue]
                emitter = _ExpressionEmitter(
                    payloads,
                    expectation.argument,
                    queue.payload,
                    bitfields=bitfields,
                )
                condition, condition_type = emitter.emit(expectation.predicate)
                if not _is_epoch_05_bool_compatible(condition_type):
                    raise QueueFrontendError(
                        "ACPY-QUEUE-021: expect predicate must lower to bool"
                    )
                lines.append(
                    f"{indent}ac.expect %{mapping[expectation.queue]} message "
                    f"{json.dumps(expectation.message)} {{"
                )
                lines.append(
                    f"{indent}^predicate(%item: "
                    f"!ac.var<{_render_type(queue.payload)}>):"
                )
                lines.extend(indent + line[2:] for line in emitter.lines)
                lines.append(f"{indent}  ac.expect.yield %{condition} : !ac.var<i1>")
                lines.append(
                    f'{indent}}} {{ac.name = "expect_{expectation.order}"}} : '
                    f"!ac.queue<{_render_type(queue.payload)}>"
                )
            elif kind == "observe":
                observation = item
                assert isinstance(observation, ObservationBinding)
                queue = by_name[observation.queue]
                lines.append(
                    f"{indent}ac.observe %{mapping[observation.queue]} name "
                    f'"{observation.name}" : '
                    f"!ac.queue<{_render_type(queue.payload)}>"
                )
            else:
                sink_binding = item
                assert isinstance(sink_binding, SinkBinding)
                queue = by_name[sink_binding.queue]
                lines.append(
                    f"{indent}ac.sink %{mapping[sink_binding.queue]} "
                    f'{{ac.name = "sink_{sink_binding.order}"}} : '
                    f"!ac.queue<{_render_type(queue.payload)}>"
                )

    def render_scope(
        scope: ScopeBinding, parent_mapping: dict[str, str], indent: str
    ) -> None:
        inputs, outputs = scope_io(scope.path)
        result_names = [
            name if len(scope.path) == 1 else f"{name}__inner" for name in outputs
        ]
        lhs = (
            ""
            if not result_names
            else ", ".join(f"%{name}" for name in result_names) + " = "
        )
        operands = ", ".join(f"%{parent_mapping[name]}" for name in inputs)
        input_types = ", ".join(
            f"!ac.queue<{_render_type(payload_by_queue[name])}>" for name in inputs
        )
        output_types = ", ".join(
            f"!ac.queue<{_render_type(payload_by_queue[name])}>" for name in outputs
        )
        lines.append(f"{indent}{lhs}ac.scope @{scope.name}({operands}) {{")
        local_mapping = dict(parent_mapping)
        if inputs:
            args = ", ".join(
                f"%{name}__in: !ac.queue<{_render_type(payload_by_queue[name])}>"
                for name in inputs
            )
            lines.append(f"{indent}^body({args}):")
            for name in inputs:
                local_mapping[name] = f"{name}__in"
        else:
            lines.append(f"{indent}^body:")
        render_items(scope.path, local_mapping, indent + "  ")
        yielded = ", ".join(f"%{local_mapping[name]}" for name in outputs)
        yield_types = ", ".join(
            f"!ac.queue<{_render_type(payload_by_queue[name])}>" for name in outputs
        )
        lines.append(
            f"{indent}  ac.scope.yield"
            + (f" {yielded} : {yield_types}" if outputs else "")
        )
        result_signature = output_types if len(outputs) == 1 else f"({output_types})"
        lines.append(f"{indent}}} : ({input_types}) -> {result_signature}")
        for name, result in zip(outputs, result_names, strict=True):
            parent_mapping[name] = result

    render_items((), initial_mapping, content_indent)
    if module is None:
        lines.append("}")
    else:
        yielded = ", ".join(f"%{initial_mapping[name]}" for name, _ in module.outputs)
        yield_types = ", ".join(
            f"!ac.queue<{_render_type(payload)}>" for _, payload in module.outputs
        )
        lines.append(
            "      ac.scope.yield"
            + (f" {yielded} : {yield_types}" if module.outputs else "")
        )
        input_types = ", ".join(
            f"!ac.queue<{_render_type(payload)}>" for _, payload in module.inputs
        )
        output_types = ", ".join(
            f"!ac.queue<{_render_type(payload)}>" for _, payload in module.outputs
        )
        output_signature = (
            "()"
            if not module.outputs
            else output_types
            if len(module.outputs) == 1
            else f"({output_types})"
        )
        lines.append(f"    }} : ({input_types}) -> {output_signature}")
        returned = ", ".join(
            f"%module_result_{index}" for index in range(len(module.outputs))
        )
        lines.append(
            "    ac.return"
            + (f" {returned} : {output_types}" if module.outputs else "")
        )
        lines.append("  }")
    return "\n".join(lines) + "\n"


def _lower_simple_module_source(
    text: str, system: str, *, host_results: bool = False
) -> str | None:
    tree = ast.parse(text, filename="<queue-model>", type_comments=True)
    enum_bindings = _enums(tree)
    payloads = _payloads(tree, enum_bindings)
    payload_map = {payload.name: payload for payload in payloads}
    bitfield_bindings = _bitfields(tree)
    bitfield_map = {binding.name: binding.layout for binding in bitfield_bindings}
    modules = {
        node.name: node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and any(
            _decorator_name(decorator).rsplit(".", 1)[-1] == "module"
            for decorator in node.decorator_list
        )
    }
    if not modules:
        return None
    systems = [
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == system
        and any(
            _decorator_name(decorator).rsplit(".", 1)[-1] == "system"
            for decorator in node.decorator_list
        )
    ]
    if len(systems) != 1:
        raise QueueFrontendError(
            f"ACPY-MODULE-001: system {system!r} is missing or ambiguous"
        )
    if not any(
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id in modules
        for node in ast.walk(systems[0])
    ):
        return None

    def result_payloads(annotation: ast.expr | None) -> tuple[ValueType, ...]:
        if annotation is None:
            raise QueueFrontendError(
                "ACPY-MODULE-001: module systems require typed returns"
            )
        if isinstance(annotation, ast.Subscript) and _decorator_name(
            annotation.value
        ).rsplit(".", 1)[-1] in {"tuple", "Tuple"}:
            elements = (
                annotation.slice.elts
                if isinstance(annotation.slice, ast.Tuple)
                else (annotation.slice,)
            )
            return tuple(_payload(element, payload_map) for element in elements)
        return (_payload(annotation, payload_map),)

    @dataclass(frozen=True, slots=True)
    class ModuleState:
        name: str
        value_type: ValueType

    @dataclass(frozen=True, slots=True)
    class ModuleAssignment:
        state: str
        expression: ast.expr

    @dataclass(frozen=True, slots=True)
    class ModuleDefinition:
        argument: str
        input_type: ValueType
        output_type: ValueType
        expression: ast.expr
        states: tuple[ModuleState, ...] = ()
        assignments: tuple[ModuleAssignment, ...] = ()

    @dataclass(frozen=True, slots=True)
    class RuleModuleDefinition:
        inputs: tuple[tuple[str, ValueType], ...]
        outputs: tuple[tuple[str, ValueType], ...]
        program: QueueProgram

    module_types: dict[str, ModuleDefinition] = {}
    rule_modules: dict[str, RuleModuleDefinition] = {}
    rule_names = {
        node.name
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and any(
            _decorator_name(decorator).rsplit(".", 1)[-1] == "rule"
            for decorator in node.decorator_list
        )
    }
    for name, function in modules.items():
        contains_rule_call = any(
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id in rule_names
            for node in ast.walk(function)
        )
        if contains_rule_call:
            if (
                not function.args.args
                or function.args.posonlyargs
                or function.args.kwonlyargs
                or function.args.vararg is not None
                or function.args.kwarg is not None
                or function.args.defaults
                or function.args.kw_defaults
                or any(
                    isinstance(decorator, ast.Call)
                    for decorator in function.decorator_list
                )
            ):
                raise QueueFrontendError(
                    "ACPY-MODULE-005: rule modules require typed positional inputs"
                )
            inputs = tuple(
                (parameter.arg, _payload(parameter.annotation, payload_map))
                for parameter in function.args.args
            )
            output_types = result_payloads(function.returns)
            body = list(function.body)
            if (
                body
                and isinstance(body[0], ast.Expr)
                and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)
            ):
                body.pop(0)
            returned = body[-1] if body else None
            if not isinstance(returned, ast.Return) or returned.value is None:
                raise QueueFrontendError(
                    "ACPY-MODULE-005: rule module requires a typed return"
                )
            result_nodes = (
                tuple(returned.value.elts)
                if isinstance(returned.value, (ast.Tuple, ast.List))
                else (returned.value,)
            )
            if len(result_nodes) != len(output_types) or not all(
                isinstance(result, ast.Name) for result in result_nodes
            ):
                raise QueueFrontendError(
                    "ACPY-MODULE-005: rule module return names must match its arity"
                )
            outputs = tuple(
                (result.id, payload)
                for result, payload in zip(result_nodes, output_types, strict=True)
                if isinstance(result, ast.Name)
            )
            rule_modules[name] = RuleModuleDefinition(
                inputs,
                outputs,
                parse_queue_program(text, name, entry_kind="module"),
            )
            continue
        if (
            len(function.args.args) != 1
            or function.args.posonlyargs
            or function.args.kwonlyargs
            or function.args.vararg is not None
            or function.args.kwarg is not None
            or function.args.defaults
            or function.args.kw_defaults
            or any(
                isinstance(decorator, ast.Call) for decorator in function.decorator_list
            )
        ):
            raise QueueFrontendError(
                "ACPY-MODULE-001: first module slice requires one typed "
                "positional parameter"
            )
        parameter = function.args.args[0]
        body = list(function.body)
        if (
            body
            and isinstance(body[0], ast.Expr)
            and isinstance(body[0].value, ast.Constant)
            and isinstance(body[0].value.value, str)
        ):
            body.pop(0)
        outputs = result_payloads(function.returns)
        if len(outputs) != 1:
            raise QueueFrontendError(
                "ACPY-MODULE-001: first module slice requires one typed result"
            )
        input_type = _payload(parameter.annotation, payload_map)
        if (
            len(body) == 1
            and isinstance(body[0], ast.Return)
            and body[0].value is not None
        ):
            module_types[name] = ModuleDefinition(
                parameter.arg,
                input_type,
                outputs[0],
                copy.deepcopy(body[0].value),
            )
            continue
        if body and isinstance(body[-1], ast.Return) and body[-1].value is not None:
            states: list[ModuleState] = []
            state_names: set[str] = set()
            cursor = 0
            while cursor < len(body) - 1 and isinstance(body[cursor], ast.AnnAssign):
                declaration = body[cursor]
                assert isinstance(declaration, ast.AnnAssign)
                if (
                    not isinstance(declaration.target, ast.Name)
                    or declaration.value is None
                    or not isinstance(declaration.value, ast.Constant)
                    or type(declaration.value.value) is not int
                    or declaration.value.value != 0
                ):
                    raise QueueFrontendError(
                        "ACPY-MODULE-004: module state requires a typed zero "
                        "initializer"
                    )
                state_name = declaration.target.id
                if state_name == parameter.arg:
                    raise QueueFrontendError(
                        "ACPY-MODULE-004: module state must not shadow its parameter"
                    )
                if state_name in state_names:
                    raise QueueFrontendError(
                        "ACPY-MODULE-004: module state names must be unique"
                    )
                state_type = _payload(declaration.annotation, payload_map)
                if _epoch_05_integer_width(state_type) is None:
                    raise QueueFrontendError(
                        "ACPY-MODULE-004: first module state slice requires scalars"
                    )
                state_names.add(state_name)
                states.append(ModuleState(state_name, state_type))
                cursor += 1
            assignment_nodes = body[cursor:-1]
            if states and assignment_nodes:
                assignments: list[ModuleAssignment] = []
                assigned: set[str] = set()
                for statement in assignment_nodes:
                    if (
                        not isinstance(statement, ast.Assign)
                        or len(statement.targets) != 1
                        or not isinstance(statement.targets[0], ast.Name)
                        or statement.targets[0].id not in state_names
                    ):
                        raise QueueFrontendError(
                            "ACPY-MODULE-004: stateful module statements must "
                            "assign declared lexical state"
                        )
                    state_name = statement.targets[0].id
                    if state_name in assigned:
                        raise QueueFrontendError(
                            "ACPY-MODULE-004: each module state may be assigned once"
                        )
                    assigned.add(state_name)
                    assignments.append(
                        ModuleAssignment(state_name, copy.deepcopy(statement.value))
                    )
                if assigned != state_names:
                    raise QueueFrontendError(
                        "ACPY-MODULE-004: first stateful module slice requires "
                        "one assignment per declared state"
                    )
                module_types[name] = ModuleDefinition(
                    parameter.arg,
                    input_type,
                    outputs[0],
                    copy.deepcopy(body[-1].value),
                    tuple(states),
                    tuple(assignments),
                )
                continue
        raise QueueFrontendError(
            "ACPY-MODULE-001: module body requires one expression return or "
            "zero-initialized typed state assignments followed by return"
        )

    def module_signature(
        name: str,
    ) -> tuple[tuple[tuple[str, ValueType], ...], tuple[tuple[str, ValueType], ...]]:
        if name in rule_modules:
            definition = rule_modules[name]
            return definition.inputs, definition.outputs
        definition = module_types[name]
        return (
            ((definition.argument, definition.input_type),),
            (("result", definition.output_type),),
        )

    function = systems[0]
    if (
        function.args.posonlyargs
        or function.args.kwonlyargs
        or function.args.vararg is not None
        or function.args.kwarg is not None
        or function.args.defaults
        or function.args.kw_defaults
    ):
        raise QueueFrontendError(
            "ACPY-MODULE-001: first module system requires positional typed inputs"
        )
    external: list[tuple[str, ValueType]] = []
    for parameter in function.args.args:
        if (
            isinstance(parameter.annotation, ast.Subscript)
            and _decorator_name(parameter.annotation.value).rsplit(".", 1)[-1]
            == "const"
        ):
            raise QueueFrontendError(
                "ACPY-MODULE-001: static module parameters are not implemented"
            )
        external.append((parameter.arg, _payload(parameter.annotation, payload_map)))
    expected_results = result_payloads(function.returns)
    values = dict(external)
    uses = {name: 0 for name, _ in external}
    instances: list[tuple[tuple[str, ...], str, tuple[str, ...], tuple[str, ...]]] = []
    returned_names: tuple[str, ...] | None = None
    for statement in function.body:
        if (
            isinstance(statement, ast.Expr)
            and isinstance(statement.value, ast.Constant)
            and isinstance(statement.value.value, str)
        ):
            continue
        if (
            isinstance(statement, ast.Assign)
            and len(statement.targets) == 1
            and isinstance(statement.targets[0], (ast.Name, ast.Tuple, ast.List))
            and isinstance(statement.value, ast.Call)
            and isinstance(statement.value.func, ast.Name)
            and statement.value.func.id in modules
            and not statement.value.keywords
            and all(isinstance(argument, ast.Name) for argument in statement.value.args)
        ):
            target = statement.targets[0]
            results = (
                (target.id,)
                if isinstance(target, ast.Name)
                else tuple(
                    item.id for item in target.elts if isinstance(item, ast.Name)
                )
            )
            if not results or (
                not isinstance(target, ast.Name) and len(results) != len(target.elts)
            ):
                raise QueueFrontendError(
                    "ACPY-MODULE-002: module results require fresh tuple names"
                )
            module_name = statement.value.func.id
            input_signature, output_signature = module_signature(module_name)
            sources = tuple(
                argument.id
                for argument in statement.value.args
                if isinstance(argument, ast.Name)
            )
            if len(sources) != len(input_signature) or len(results) != len(
                output_signature
            ):
                raise QueueFrontendError(
                    "ACPY-MODULE-002: module call arity does not match its signature"
                )
            if any(result in values for result in results) or any(
                source not in values for source in sources
            ):
                raise QueueFrontendError(
                    "ACPY-MODULE-002: module call values must be defined once"
                )
            if any(
                not _types_equal_in_epoch_05(values[source], expected_type)
                for source, (_, expected_type) in zip(
                    sources, input_signature, strict=True
                )
            ):
                raise QueueFrontendError(
                    "ACPY-MODULE-002: module input payload type mismatch"
                )
            for source in sources:
                uses[source] = uses.get(source, 0) + 1
            output_types = tuple(payload for _, payload in output_signature)
            for result, output_type in zip(results, output_types, strict=True):
                values[result] = output_type
                uses[result] = 0
            instances.append((results, module_name, sources, output_types))
            continue
        if isinstance(statement, ast.Return) and statement.value is not None:
            returned = (
                tuple(statement.value.elts)
                if isinstance(statement.value, (ast.Tuple, ast.List))
                else (statement.value,)
            )
            if not all(isinstance(value, ast.Name) for value in returned):
                raise QueueFrontendError(
                    "ACPY-MODULE-002: module system returns require named values"
                )
            returned_names = tuple(value.id for value in returned)
            for name in returned_names:
                if name not in values:
                    raise QueueFrontendError(
                        "ACPY-MODULE-002: returned module value is undefined"
                    )
                uses[name] = uses.get(name, 0) + 1
            continue
        raise QueueFrontendError(
            f"ACPY-MODULE-002: unsupported module system statement "
            f"{type(statement).__name__}"
        )
    if (
        returned_names is None
        or len(returned_names) != len(expected_results)
        or any(
            not _types_equal_in_epoch_05(values[name], expected)
            for name, expected in zip(returned_names, expected_results, strict=True)
        )
    ):
        raise QueueFrontendError(
            "ACPY-MODULE-002: module system return type or arity mismatch"
        )
    if any(count != 1 for count in uses.values()):
        raise QueueFrontendError(
            "ACPY-MODULE-002: every module Queue value requires one consumer"
        )

    lines = [
        'builtin.module attributes {ac.contract_epoch = "0.5", '
        'ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle"} {'
    ]
    if payloads or enum_bindings or bitfield_bindings:
        lines.append("  ac.type_scope @types {")
        for enumeration in enum_bindings:
            lines.append(_render_enum(enumeration, "    "))
        for payload in payloads:
            fields = ", ".join(
                f'{{name = "{field}", type = {typ}}}' for field, typ in payload.fields
            )
            lines.append(f"    ac.struct @{payload.name} fields [{fields}]")
        for bitfield in bitfield_bindings:
            lines.append(_render_bitfield(bitfield, "    "))
        layouts = [
            *(_enum_layout_entry(enumeration) for enumeration in enum_bindings),
            *(_payload_layout_entry(payload) for payload in payloads),
        ]
        if layouts:
            lines.append(
                "  } {dlti.dl_spec = #dlti.dl_spec<" + ", ".join(layouts) + ">}"
            )
        else:
            lines.append("  }")
    lines.append(
        f'  ac.system @{system} root @Top as "root" tick 0 "cycle" '
        'seed {kind = "fixed", value = 0 : i64} instrumentation [] '
        'results {id = "default", format = "json"} selected true'
    )
    for name, definition in module_types.items():
        argument = definition.argument
        input_type = definition.input_type
        output_type = definition.output_type
        expression = definition.expression
        if (
            not definition.states
            and isinstance(expression, ast.Call)
            and isinstance(expression.func, ast.Name)
            and expression.func.id in module_types
        ):
            if (
                len(expression.args) != 1
                or expression.keywords
                or not isinstance(expression.args[0], ast.Name)
                or expression.args[0].id != argument
            ):
                raise QueueFrontendError(
                    "ACPY-MODULE-003: nested module call requires the module "
                    "parameter as its sole argument"
                )
            child = expression.func.id
            child_definition = module_types[child]
            child_input = child_definition.input_type
            child_output = child_definition.output_type
            if not _types_equal_in_epoch_05(
                child_input, input_type
            ) or not _types_equal_in_epoch_05(child_output, output_type):
                raise QueueFrontendError(
                    "ACPY-MODULE-003: nested module call signature mismatch"
                )
            lines.extend(
                [
                    f"  ac.module @{name}(%input: "
                    f"!ac.queue<{_render_type(input_type)}>) -> "
                    f"!ac.queue<{_render_type(output_type)}> parameters {{}} graph {{",
                    f"    %output = ac.instance @result of @{child}(%input) "
                    'static {} id "result" path "result" '
                    f": (!ac.queue<{_render_type(input_type)}>) -> "
                    f"!ac.queue<{_render_type(output_type)}>",
                    f"    ac.return %output : !ac.queue<{_render_type(output_type)}>",
                    "  }",
                ]
            )
            continue
        if definition.states:
            state_types = {state.name: state.value_type for state in definition.states}
            root_values = {
                state.name: (
                    "old" if index == 0 else f"old_{index}",
                    state.value_type,
                )
                for index, state in enumerate(definition.states)
            }
            emitter = _ExpressionEmitter(
                payload_map,
                argument,
                input_type,
                root_values=root_values,
                bitfields=bitfield_map,
            )
            lines.extend(
                [
                    f"  ac.module @{name}(%input: "
                    f"!ac.queue<{_render_type(input_type)}>) -> "
                    f"!ac.queue<{_render_type(output_type)}> parameters {{}} graph {{",
                    "    %output = ac.scope @body(%input) {",
                    f"    ^bb0(%borrowed: !ac.queue<{_render_type(input_type)}>):",
                ]
            )
            for state in definition.states:
                lines.append(
                    f"      ac.var.decl @{state.name} "
                    f"type {_render_type(state.value_type)} "
                    f'init 0 : {_render_type(state.value_type)} owner "/body" '
                    "stable_id "
                    f'"var/body/{state.name}"'
                )
            lines.extend(
                [
                    "      %next = ac.rule %borrowed depths [1] latencies [1] ",
                    f'          name "{name}" stable_id "{name}_0" domain "cycle" ',
                    "          type exact input_fact committed_input {",
                    f"      ^body(%item: !ac.var<{_render_type(input_type)}>):",
                ]
            )
            for index, state in enumerate(definition.states):
                old = "old" if index == 0 else f"old_{index}"
                lines.append(
                    f"        %{old} = ac.var.read @{state.name} : "
                    f"!ac.var<{_render_type(state.value_type)}>"
                )
            emitted_lines = 0
            for assignment in definition.assignments:
                state_type = state_types[assignment.state]
                value, value_type = emitter.emit(assignment.expression, state_type)
                if not _types_equal_in_epoch_05(value_type, state_type):
                    raise QueueFrontendError(
                        "ACPY-MODULE-004: assigned module state type mismatch"
                    )
                lines.extend("    " + line for line in emitter.lines[emitted_lines:])
                emitted_lines = len(emitter.lines)
                lines.append(
                    f"        ac.var.assign @{assignment.state} = %{value} : "
                    f"!ac.var<{_render_type(state_type)}>"
                )
                emitter.root_values[assignment.state] = (value, state_type)
            value, value_type = emitter.emit(expression, output_type)
            if not _types_equal_in_epoch_05(value_type, output_type):
                raise QueueFrontendError(
                    "ACPY-MODULE-004: stateful module result type mismatch"
                )
            lines.extend("    " + line for line in emitter.lines[emitted_lines:])
            lines.extend(
                [
                    f"        %rule_ready = ac.marker.obligation %{value} "
                    "state pending resolver handshake "
                    f'origin "{name}:return" path "true" : '
                    f"!ac.var<{_render_type(output_type)}>",
                    f"        ac.rule.return %rule_ready : "
                    f"!ac.var<{_render_type(output_type)}>",
                    f'      }} {{ac.name = "result"}} : '
                    f"(!ac.queue<{_render_type(input_type)}>) "
                    f"-> !ac.queue<{_render_type(output_type)}>",
                    f"      ac.scope.yield %next : "
                    f"!ac.queue<{_render_type(output_type)}>",
                    f"    }} : (!ac.queue<{_render_type(input_type)}>) -> "
                    f"!ac.queue<{_render_type(output_type)}>",
                    f"    ac.return %output : !ac.queue<{_render_type(output_type)}>",
                    "  }",
                ]
            )
            continue
        emitter = _ExpressionEmitter(
            payload_map,
            argument,
            input_type,
            bitfields=bitfield_map,
        )
        value, value_type = emitter.emit(expression, output_type)
        if not _types_equal_in_epoch_05(value_type, output_type):
            raise QueueFrontendError(
                "ACPY-MODULE-001: module result payload type mismatch"
            )
        lines.extend(
            [
                f"  ac.module @{name}(%input: "
                f"!ac.queue<{_render_type(input_type)}>) -> "
                f"!ac.queue<{_render_type(output_type)}> parameters {{}} graph {{",
                "    %output = ac.scope @body(%input) {",
                f"    ^bb0(%borrowed: !ac.queue<{_render_type(input_type)}>):",
                "      %transformed = ac.transform %borrowed depths [1] "
                "latencies [1] {",
                f"      ^bb0(%item: !ac.var<{_render_type(input_type)}>):",
            ]
        )
        lines.extend("    " + line for line in emitter.lines)
        lines.extend(
            [
                f"        ac.transform.yield %{value} : "
                f"!ac.var<{_render_type(output_type)}>",
                f'      }} {{ac.name = "result"}} : '
                f"(!ac.queue<{_render_type(input_type)}>) "
                f"-> !ac.queue<{_render_type(output_type)}>",
                f"      ac.scope.yield %transformed : "
                f"!ac.queue<{_render_type(output_type)}>",
                f"    }} : (!ac.queue<{_render_type(input_type)}>) -> "
                f"!ac.queue<{_render_type(output_type)}>",
                f"    ac.return %output : !ac.queue<{_render_type(output_type)}>",
                "  }",
            ]
        )
    for name, definition in rule_modules.items():
        lines.extend(
            lower_queue_program(
                definition.program,
                module=_ModuleRenderSpec(
                    name,
                    definition.inputs,
                    definition.outputs,
                ),
            )
            .rstrip()
            .splitlines()
        )
    root_result_types = ", ".join(
        f"!ac.queue<{_render_type(payload)}>" for payload in expected_results
    )
    root_result_signature = (
        root_result_types if len(expected_results) == 1 else f"({root_result_types})"
    )
    lines.append(
        "  ac.module @Top()"
        + (f" -> {root_result_signature}" if host_results else "")
        + " parameters {} graph {"
    )
    source_values = [f"%source_{index}" for index in range(len(external))]
    top_values: dict[str, str] = {}
    if external:
        result_name = "%inputs"
        suffix = f":{len(external)}" if len(external) > 1 else ""
        lines.append(f"    {result_name}{suffix} = ac.scope @inputs() {{")
        for index, (name, payload) in enumerate(external):
            lines.append(
                f"      {source_values[index]} = ac.source depth 1 latency 1 "
                f'{{ac.name = "{name}"}} : '
                f"!ac.queue<{_render_type(payload)}>"
            )
        lines.append(
            "      ac.scope.yield "
            + ", ".join(source_values)
            + " : "
            + ", ".join(
                f"!ac.queue<{_render_type(payload)}>" for _, payload in external
            )
        )
        lines.append(
            "    } : () -> ("
            + ", ".join(
                f"!ac.queue<{_render_type(payload)}>" for _, payload in external
            )
            + ")"
        )
        for index, (name, _) in enumerate(external):
            top_values[name] = f"%inputs#{index}" if len(external) > 1 else "%inputs"
    for results, module_name, sources, output_types in instances:
        input_types = tuple(values[source] for source in sources)
        lhs = ", ".join(f"%{result}" for result in results)
        operands = ", ".join(top_values[source] for source in sources)
        input_signature = ", ".join(
            f"!ac.queue<{_render_type(payload)}>" for payload in input_types
        )
        output_signature = ", ".join(
            f"!ac.queue<{_render_type(payload)}>" for payload in output_types
        )
        result_type = (
            output_signature if len(output_types) == 1 else f"({output_signature})"
        )
        instance_name = "__".join(results)
        lines.append(
            f"    {lhs} = ac.instance @{instance_name} of @{module_name}"
            f'({operands}) static {{}} id "{instance_name}" path "{instance_name}" '
            f": ({input_signature}) -> {result_type}"
        )
        for result in results:
            top_values[result] = f"%{result}"
    returned_operands = [top_values[name] for name in returned_names]
    if host_results:
        lines.append(
            "    ac.return " + ", ".join(returned_operands) + " : " + root_result_types
        )
    else:
        lines.append("    ac.scope @outputs(" + ", ".join(returned_operands) + ") {")
        lines.append(
            "    ^bb0("
            + ", ".join(
                f"%result_{index}: !ac.queue<{_render_type(values[name])}>"
                for index, name in enumerate(returned_names)
            )
            + "):"
        )
        for index, _ in enumerate(returned_names):
            lines.append(
                f'      ac.sink %result_{index} {{ac.name = "sink_{index}"}} '
                f": !ac.queue<{_render_type(expected_results[index])}>"
            )
        lines.extend(
            [
                "      ac.scope.yield",
                "    } : ("
                + ", ".join(
                    f"!ac.queue<{_render_type(payload)}>"
                    for payload in expected_results
                )
                + ") -> ()",
                "    ac.return",
            ]
        )
    lines.extend(["  }", "}"])
    return "\n".join(lines) + "\n"


def lower_queue_source(
    text: str,
    system: str,
    static_arguments: Mapping[str, StaticValue] | None = None,
    specialization_fingerprint: str | None = None,
    *,
    host_results: bool = False,
) -> str:
    if lowered := _lower_simple_module_source(text, system, host_results=host_results):
        return lowered
    if host_results:
        raise QueueFrontendError(
            "ACPY-MODULE-006: host result boundaries require structured modules"
        )
    return lower_queue_program(
        parse_queue_program(
            text,
            system,
            static_arguments=static_arguments,
            specialization_fingerprint=specialization_fingerprint,
        )
    )


def build_queue_acpy(text: str, system: str, source_path: str) -> AcpyDocument:
    """Build the minimal verified ACPy provenance for a Queue/rule source."""

    tree = ast.parse(text, filename=source_path, type_comments=True)
    systems = [
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == system
        and any(
            _decorator_name(decorator).rsplit(".", 1)[-1] == "system"
            for decorator in node.decorator_list
        )
    ]
    if len(systems) != 1:
        raise QueueFrontendError(
            f"ACPY-QUEUE-001: system {system!r} is missing or ambiguous"
        )

    def span(node: ast.AST) -> SourceSpan:
        return SourceSpan(
            source_path,
            node.lineno,
            node.col_offset + 1,
            getattr(node, "end_lineno", node.lineno),
            getattr(node, "end_col_offset", node.col_offset) + 1,
        )

    allocator = EntityAllocator()
    system_entity = allocator.allocate(
        kind="system",
        scope=system,
        source=span(systems[0]),
        properties=(Property("frontend", "queue_rule"),),
    )
    for node in tree.body:
        if not isinstance(node, ast.FunctionDef) or not any(
            _decorator_name(decorator).rsplit(".", 1)[-1] == "rule"
            for decorator in node.decorator_list
        ):
            continue
        allocator.allocate(
            kind="rule",
            scope=f"{system}.{node.name}",
            source=span(node),
            parent=system_entity.id,
            properties=(Property("name", node.name),),
        )
    document = AcpyDocument(
        entry=system_entity.id,
        sources=(SourceFile(source_path, sha256_bytes(text.encode("utf-8"))),),
        entities=allocator.freeze(),
    )
    errors = document.verify()
    if errors:
        raise QueueFrontendError(
            "ACPY-VERIFY-001: " + "; ".join(error.message for error in errors)
        )
    return document
