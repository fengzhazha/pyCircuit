"""Deterministic QueueProgram to typed gfsim C++ lowering."""

from __future__ import annotations

import ast
from dataclasses import dataclass

from _pycircuit_semantics import (
    BitsType,
    BoolType,
    StructType,
    ValueType,
    parse_bitmask_checked,
)

from ._queue_frontend import (
    CollectionBinding,
    Payload,
    QueueBinding,
    QueueFrontendError,
    QueueProgram,
    StaticQueueCollection,
    TableBinding,
    _decorator_name,
    parse_queue_program,
)


def _cpp_type(value_type: ValueType) -> str:
    if isinstance(value_type, (BitsType, BoolType)):
        return f"gfsim::UInt<{value_type.bit_width()}>"
    if isinstance(value_type, StructType):
        return value_type.name
    raise QueueFrontendError(
        f"ACLOWER-TYPE-MISMATCH: no C++ type for {value_type.canonical()}"
    )


class _CppExpression:
    def __init__(
        self,
        argument: str,
        state_names: dict[str, str] | None = None,
        *,
        argument_type: ValueType | None = None,
        candidates: dict[str, object] | None = None,
        selections: dict[str, object] | None = None,
        candidate_refs: dict[str, str] | None = None,
        selection_refs: dict[str, str] | None = None,
        table_entries: dict[str, int] | None = None,
        table_names: dict[str, str] | None = None,
        require_shared_refs: bool = False,
    ) -> None:
        self.argument = argument
        self.argument_type = argument_type
        self.state_names = state_names or {}
        self.candidates = candidates or {}
        self.selections = selections or {}
        self.candidate_refs = candidate_refs or {}
        self.selection_refs = selection_refs or {}
        self.table_entries = table_entries or {}
        self.table_names = table_names or {}
        self.require_shared_refs = require_shared_refs

    def value_type(self, node: ast.expr) -> ValueType:
        if isinstance(node, ast.Name) and node.id == self.argument:
            if self.argument_type is None:
                raise QueueFrontendError(
                    "ACLOWER-TYPE-MISMATCH: expression root type is unavailable"
                )
            return self.argument_type
        if isinstance(node, ast.Attribute):
            parent = self.value_type(node.value)
            if not isinstance(parent, StructType):
                raise QueueFrontendError(
                    "ACLOWER-TYPE-MISMATCH: field access requires a struct value"
                )
            try:
                return parent.field(node.attr).type
            except KeyError as error:
                raise QueueFrontendError(
                    f"ACLOWER-TYPE-MISMATCH: unknown field {node.attr!r}"
                ) from error
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
            left = self.value_type(node.left)
            right = (
                left
                if isinstance(node.right, ast.Constant)
                and type(node.right.value) is int
                else self.value_type(node.right)
            )
            if left != right:
                raise QueueFrontendError(
                    "ACLOWER-TYPE-MISMATCH: binary operands must match"
                )
            return left
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Invert):
            return self.value_type(node.operand)
        if isinstance(node, ast.Constant) and type(node.value) is bool:
            return BoolType()
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return BitsType(64)
        raise QueueFrontendError(
            "ACLOWER-TYPE-MISMATCH: cannot infer expression value type"
        )

    def emit(self, node: ast.expr) -> str:
        if isinstance(node, ast.Name) and node.id == self.argument:
            return "item"
        if isinstance(node, ast.Name) and node.id in self.state_names:
            return self.state_names[node.id]
        if isinstance(node, ast.Name) and node.id in self.candidate_refs:
            return f"{self.candidate_refs[node.id]}->get(epoch)"
        if isinstance(node, ast.Name) and node.id in self.candidates:
            if self.require_shared_refs:
                raise QueueFrontendError(
                    "ACLOWER-OWNERSHIP: CandidateSet shared cache reference is missing"
                )
            candidate = self.candidates[node.id]
            table = self.table_names.get(candidate.table)
            entries = self.table_entries.get(candidate.table)
            if table is None or entries is None:
                raise QueueFrontendError(
                    "ACLOWER-OWNERSHIP: CandidateSet Table is unavailable to policy"
                )
            predicate = _CppExpression(
                candidate.argument,
                self.state_names,
                candidates=self.candidates,
                selections=self.selections,
                candidate_refs=self.candidate_refs,
                selection_refs=self.selection_refs,
                table_entries=self.table_entries,
                table_names=self.table_names,
                require_shared_refs=self.require_shared_refs,
            ).emit(candidate.predicate)
            return (
                "([&]() { std::uint64_t mask = 0; "
                f"for (std::size_t index = 0; index < {entries}; ++index) {{ "
                f"const auto &item = {table}->at(index); "
                f"if ({predicate}) mask |= (std::uint64_t{{1}} << index); "
                "} return mask; }())"
            )
        if isinstance(node, ast.Constant) and type(node.value) in {int, bool}:
            if node.value is True:
                return "true"
            if node.value is False:
                return "false"
            return str(node.value)
        if (
            isinstance(node, ast.Call)
            and _decorator_name(node.func).rsplit(".", 1)[-1] == "matches"
        ):
            if len(node.args) != 2 or node.keywords:
                raise QueueFrontendError(
                    "ACLOWER-UNSUPPORTED-CONSTRUCT: matches requires two "
                    "positional arguments"
                )
            pattern_node = node.args[1]
            if not (
                isinstance(pattern_node, ast.Constant)
                and type(pattern_node.value) is str
            ):
                raise QueueFrontendError(
                    "ACLOWER-UNSUPPORTED-CONSTRUCT: matches pattern must be "
                    "a compile-time str"
                )
            value_type = self.value_type(node.args[0])
            if not isinstance(value_type, BitsType):
                raise QueueFrontendError(
                    "ACLOWER-TYPE-MISMATCH: matches requires a bits value"
                )
            try:
                mask, expected = parse_bitmask_checked(
                    pattern_node.value,
                    width=value_type.width,
                    extended=False,
                )
            except (TypeError, ValueError) as error:
                raise QueueFrontendError(
                    f"ACLOWER-UNSUPPORTED-CONSTRUCT: {error}"
                ) from error
            value = self.emit(node.args[0])
            return f"(({value} & {mask}) == {expected})"
        if (
            isinstance(node, ast.Call)
            and _decorator_name(node.func).rsplit(".", 1)[-1] == "popcount"
        ):
            if len(node.args) != 1 or node.keywords:
                raise QueueFrontendError(
                    "ACLOWER-UNSUPPORTED-CONSTRUCT: popcount requires one operand"
                )
            return f"gfsim::populationCount({self.emit(node.args[0])})"
        if isinstance(node, ast.Call) and _decorator_name(node.func).rsplit(".", 1)[
            -1
        ] in {"count_leading_zeros", "count_trailing_zeros"}:
            operation = _decorator_name(node.func).rsplit(".", 1)[-1]
            if len(node.args) != 1 or node.keywords:
                raise QueueFrontendError(
                    f"ACLOWER-UNSUPPORTED-CONSTRUCT: {operation} requires one operand"
                )
            helper = (
                "countTrailingZeros"
                if operation == "count_trailing_zeros"
                else "countLeadingZeros"
            )
            return f"gfsim::{helper}({self.emit(node.args[0])})"
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
                    "ACLOWER-UNSUPPORTED-CONSTRUCT: malformed priority_encode"
                )
            order = "low"
            if call.keywords:
                raw_order = call.keywords[0].value
                if (
                    not isinstance(raw_order, ast.Constant)
                    or type(raw_order.value) is not str
                ):
                    raise QueueFrontendError(
                        "ACLOWER-UNSUPPORTED-CONSTRUCT: priority order must be static"
                    )
                order = raw_order.value.strip().lower()
            if order not in {"low", "high"}:
                raise QueueFrontendError(
                    "ACLOWER-UNSUPPORTED-CONSTRUCT: priority order must be low or high"
                )
            return (
                f"gfsim::priorityEncode({self.emit(call.args[0])}, "
                f"{'true' if order == 'low' else 'false'}).{node.attr}"
            )
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id in self.selection_refs
            and node.attr in {"index", "valid"}
        ):
            return f"{self.selection_refs[node.value.id]}->get(epoch).{node.attr}"
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.value, ast.Name)
            and node.value.id in self.selections
            and node.attr in {"index", "valid"}
        ):
            if self.require_shared_refs:
                raise QueueFrontendError(
                    "ACLOWER-OWNERSHIP: Selection shared cache reference is missing"
                )
            selection = self.selections[node.value.id]
            candidate = self.candidates[selection.candidates]
            table = self.table_names.get(selection.table)
            entries = self.table_entries.get(selection.table)
            if table is None or entries is None:
                raise QueueFrontendError(
                    "ACLOWER-OWNERSHIP: selection Table is unavailable to policy"
                )
            predicate = _CppExpression(
                candidate.argument,
                self.state_names,
                candidates=self.candidates,
                selections=self.selections,
                candidate_refs=self.candidate_refs,
                selection_refs=self.selection_refs,
                table_entries=self.table_entries,
                table_names=self.table_names,
                require_shared_refs=self.require_shared_refs,
            ).emit(candidate.predicate)
            body = (
                "([&]() { std::pair<std::uint64_t, bool> selected{0, false}; "
                f"for (std::size_t index = 0; index < {entries}; ++index) {{ "
                f"const auto &item = {table}->at(index); "
                f"if (!({predicate})) continue; "
            )
            if selection.policy == "first":
                body += "selected = {index, true}; break; "
            else:
                assert selection.argument is not None and selection.key is not None
                key = _CppExpression(
                    selection.argument,
                    self.state_names,
                    candidates=self.candidates,
                    selections=self.selections,
                    candidate_refs=self.candidate_refs,
                    selection_refs=self.selection_refs,
                    table_entries=self.table_entries,
                    table_names=self.table_names,
                    require_shared_refs=self.require_shared_refs,
                ).emit(selection.key)
                comparison = "<" if selection.policy == "min" else ">"
                body += (
                    f"const std::uint64_t key = static_cast<std::uint64_t>({key}); "
                    f"if (!selected.second || key {comparison} best) {{ "
                    "selected = {index, true}; best = key; } "
                )
                body = body.replace(
                    "for (std::size_t", "std::uint64_t best{}; for (std::size_t", 1
                )
            body += "} return selected; }())"
            return body + (".first" if node.attr == "index" else ".second")
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == self.argument
        ):
            if (
                node.func.attr in {"peek", "pop"}
                and not node.args
                and not node.keywords
            ):
                return "item"
            if node.func.attr == "push" and len(node.args) == 1 and not node.keywords:
                return self.emit(node.args[0])
        if isinstance(node, ast.Attribute):
            return f"{self.emit(node.value)}.{node.attr}"
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
            operator = {
                ast.Add: "+",
                ast.Sub: "-",
                ast.Mult: "*",
                ast.BitAnd: "&",
                ast.BitOr: "|",
                ast.BitXor: "^",
                ast.LShift: "<<",
                ast.RShift: ">>",
            }[type(node.op)]
            return f"({self.emit(node.left)} {operator} {self.emit(node.right)})"
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Invert):
            return f"(~{self.emit(node.operand)})"
        if isinstance(node, ast.BoolOp) and isinstance(node.op, ast.And):
            return "(" + " && ".join(self.emit(value) for value in node.values) + ")"
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return f"(!{self.emit(node.operand)})"
        if (
            isinstance(node, ast.Compare)
            and len(node.ops) == len(node.comparators) == 1
        ):
            operators = {
                ast.Eq: "==",
                ast.NotEq: "!=",
                ast.Lt: "<",
                ast.LtE: "<=",
                ast.Gt: ">",
                ast.GtE: ">=",
            }
            operator = operators.get(type(node.ops[0]))
            if operator is not None:
                return (
                    f"({self.emit(node.left)} {operator} "
                    f"{self.emit(node.comparators[0])})"
                )
        raise QueueFrontendError(
            "ACLOWER-UNSUPPORTED-CONSTRUCT: lambda is outside C++ expression subset"
        )


def _policy_body(queue: QueueBinding, argument_type: ValueType) -> list[str]:
    assert queue.argument is not None and queue.expression is not None
    return _expression_policy_body(queue.argument, queue.expression, argument_type)


def _expression_policy_body(
    argument: str, node: ast.expr, argument_type: ValueType | None = None
) -> list[str]:
    expression = _CppExpression(argument, argument_type=argument_type)
    if (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "push"
        and len(node.args) == 1
        and not node.keywords
    ):
        node = node.args[0]
    if (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "with_fields"
        and not node.args
    ):
        lines = ["    auto result = item;"]
        for keyword in node.keywords:
            if keyword.arg is None:
                raise QueueFrontendError(
                    "ACLOWER-UNSUPPORTED-CONSTRUCT: field unpacking is forbidden"
                )
            lines.append(
                f"    result.{keyword.arg} = {expression.emit(keyword.value)};"
            )
        lines.extend(("    return result;",))
        return lines
    return [f"    return {expression.emit(node)};"]


def _emit_table_state_expression(
    node: ast.expr,
    view_alias: str | None,
    table: str,
    address: str,
    argument: str = "__state",
    expression: _CppExpression | None = None,
) -> str:
    if view_alias is not None and isinstance(node, ast.Name) and node.id == view_alias:
        return f"{table}->checkedAt(static_cast<size_t>({address}))"
    if isinstance(node, ast.Attribute):
        if (
            view_alias is not None
            and isinstance(node.value, ast.Name)
            and node.value.id == view_alias
        ):
            return f"{table}->checkedAt(static_cast<size_t>({address})).{node.attr}"
        return (expression or _CppExpression(argument)).emit(node)
    if isinstance(node, ast.BoolOp) and isinstance(node.op, ast.And):
        return (
            "("
            + " && ".join(
                _emit_table_state_expression(
                    value, view_alias, table, address, argument, expression
                )
                for value in node.values
            )
            + ")"
        )
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
        return (
            "(!"
            + _emit_table_state_expression(
                node.operand, view_alias, table, address, argument, expression
            )
            + ")"
        )
    return (expression or _CppExpression(argument)).emit(node)


@dataclass(frozen=True, slots=True)
class _ObjectIds:
    queues: dict[str, int]
    fanout_queues: dict[str, int]
    feedback_states: tuple[int, ...]
    broadcasts: tuple[int, ...]
    transforms: dict[str, int]
    routes: tuple[int, ...]
    forks: tuple[int, ...]
    merges: tuple[int, ...]
    barriers: tuple[int, ...]
    feedbacks: tuple[int, ...]
    reorders: tuple[int, ...]
    dependencies: tuple[int, ...]
    credits: tuple[int, ...]
    memories: tuple[int, ...]
    memory_instances: tuple[int, ...]
    tables: tuple[int, ...]
    table_reads: tuple[int, ...]
    table_writes: tuple[int, ...]
    masked_table_writes: tuple[int, ...]
    slots: tuple[int, ...]
    sinks: tuple[int, ...]


@dataclass(frozen=True, slots=True)
class _Fanout:
    source: str
    outputs: tuple[str, ...]
    consumers: tuple[str, ...]
    payload: ValueType
    scope: tuple[str, ...]


def _common_scope(scopes: list[tuple[str, ...]]) -> tuple[str, ...]:
    common: list[str] = []
    for parts in zip(*scopes, strict=False):
        if len(set(parts)) != 1:
            break
        common.append(parts[0])
    return tuple(common)


def _fanouts(program: QueueProgram) -> tuple[_Fanout, ...]:
    consumers: dict[str, list[QueueBinding]] = {}
    for queue in program.queues:
        if queue.input_name is not None:
            consumers.setdefault(queue.input_name, []).append(queue)
    existing = {queue.name for queue in program.queues}
    fanouts: list[_Fanout] = []
    for source, group in consumers.items():
        if len(group) < 2:
            continue
        outputs = tuple(f"{source}__fanout{index}" for index in range(len(group)))
        if existing.intersection(outputs):
            raise QueueFrontendError(
                "ACLOWER-OWNERSHIP: inferred broadcast Queue name collides with source"
            )
        source_queue = next(queue for queue in program.queues if queue.name == source)
        fanouts.append(
            _Fanout(
                source,
                outputs,
                tuple(queue.name for queue in group),
                source_queue.payload,
                _common_scope([source_queue.scope, *(queue.scope for queue in group)]),
            )
        )
    return tuple(fanouts)


def _collection_leaves(
    value: StaticQueueCollection,
    path: tuple[int, ...] = (),
) -> tuple[tuple[str, tuple[int, ...]], ...]:
    leaves: list[tuple[str, tuple[int, ...]]] = []
    for index, (_, member) in enumerate(value.members):
        member_path = (*path, index)
        if isinstance(member, str):
            leaves.append((member, member_path))
        else:
            leaves.extend(_collection_leaves(member, member_path))
    return tuple(leaves)


def _owning_arrays(program: QueueProgram) -> tuple[CollectionBinding, ...]:
    queue_names = {queue.name for queue in program.queues}
    result: list[CollectionBinding] = []
    claimed: set[str] = set()
    for collection in program.collections:
        if collection.value.kind != "array":
            continue
        leaves = {name for name, _ in _collection_leaves(collection.value)}
        if (
            not leaves
            or not leaves.issubset(queue_names)
            or claimed.intersection(leaves)
        ):
            continue
        result.append(collection)
        claimed.update(leaves)
    return tuple(result)


def _array_cpp_type(
    value: StaticQueueCollection, queues: dict[str, QueueBinding]
) -> str:
    first = value.members[0][1]
    element = (
        f"gfsim::SimQueue<{_cpp_type(queues[first].payload)}>"
        if isinstance(first, str)
        else _array_cpp_type(first, queues)
    )
    return f"std::array<{element}, {len(value.members)}>"


def _object_ids(program: QueueProgram, fanouts: tuple[_Fanout, ...]) -> _ObjectIds:
    next_id = 0
    queues: dict[str, int] = {}
    for queue in program.queues:
        queues[queue.name] = next_id
        next_id += 1
    fanout_queues: dict[str, int] = {}
    for fanout in fanouts:
        for output in fanout.outputs:
            fanout_queues[output] = next_id
            next_id += 1
    feedback_states = tuple(range(next_id, next_id + len(program.feedbacks)))
    next_id += len(feedback_states)
    broadcasts = tuple(range(next_id, next_id + len(fanouts)))
    next_id += len(broadcasts)
    transforms: dict[str, int] = {}
    for queue in program.queues:
        if queue.input_name is not None:
            transforms[queue.name] = next_id
            next_id += 1
    routes = tuple(range(next_id, next_id + len(program.routes)))
    next_id += len(routes)
    forks = tuple(range(next_id, next_id + len(program.forks)))
    next_id += len(forks)
    merges = tuple(range(next_id, next_id + len(program.merges)))
    next_id += len(merges)
    barriers = tuple(range(next_id, next_id + len(program.barriers)))
    next_id += len(barriers)
    feedbacks = tuple(range(next_id, next_id + len(program.feedbacks)))
    next_id += len(feedbacks)
    reorders = tuple(range(next_id, next_id + len(program.reorders)))
    next_id += len(reorders)
    dependencies = tuple(range(next_id, next_id + len(program.dependencies)))
    next_id += len(dependencies)
    credits = tuple(range(next_id, next_id + len(program.credits)))
    next_id += len(credits)
    memories = tuple(range(next_id, next_id + len(program.memories)))
    next_id += len(memories)
    memory_instances = tuple(range(next_id, next_id + len(program.memory_instances)))
    next_id += len(memory_instances)
    tables = tuple(range(next_id, next_id + len(program.tables)))
    next_id += len(tables)
    table_reads = tuple(range(next_id, next_id + len(program.table_reads)))
    next_id += len(table_reads)
    table_writes = tuple(range(next_id, next_id + len(program.table_writes)))
    next_id += len(table_writes)
    masked_table_writes = tuple(
        range(next_id, next_id + len(program.masked_table_writes))
    )
    next_id += len(masked_table_writes)
    slots = tuple(range(next_id, next_id + len(program.slots)))
    next_id += len(slots)
    sinks = tuple(range(next_id, next_id + len(program.sinks)))
    return _ObjectIds(
        queues,
        fanout_queues,
        feedback_states,
        broadcasts,
        transforms,
        routes,
        forks,
        merges,
        barriers,
        feedbacks,
        reorders,
        dependencies,
        credits,
        memories,
        memory_instances,
        tables,
        table_reads,
        table_writes,
        masked_table_writes,
        slots,
        sinks,
    )


def lower_queue_program_to_cpp(program: QueueProgram) -> str:
    if any(queue.rule_name is not None for queue in program.queues):
        raise QueueFrontendError(
            "ACLOWER-RULE-001: @ac.rule must pass through the native MLIR "
            "rule-lowering pipeline before gfsim C++ generation"
        )
    fanouts = _fanouts(program)
    arrays = _owning_arrays(program)
    ids = _object_ids(program, fanouts)
    queues_by_name = {queue.name: queue for queue in program.queues}
    tables_by_name = {table.name: table for table in program.tables}
    table_indices = {table.name: index for index, table in enumerate(program.tables)}
    slots_by_name = {slot.name: slot for slot in program.slots}
    candidates_by_name = {candidate.name: candidate for candidate in program.candidates}
    selections_by_name = {selection.name: selection for selection in program.selections}
    table_entries = {table.name: table.entries for table in program.tables}

    def table_merge_policy(
        policy_name: str, table: TableBinding, write_fields: tuple[str, ...]
    ) -> list[str]:
        entry = _cpp_type(table.entry_type)
        if write_fields == ("$entry",):
            ordinals = (0,)
            assignments = ("    target = value;",)
        else:
            if not isinstance(table.entry_type, StructType):
                raise QueueFrontendError(
                    "ACLOWER-TYPE-MISMATCH: field merge requires a struct Table"
                )
            field_ordinals = {
                field.name: index for index, field in enumerate(table.entry_type.fields)
            }
            ordinals = tuple(field_ordinals[field] for field in write_fields)
            assignments = tuple(
                f"    target.{field} = value.{field};" for field in write_fields
            )
        return [
            f"struct {policy_name} {{",
            f"  static constexpr std::array<size_t, {len(ordinals)}> fields{{"
            + ", ".join(str(value) for value in ordinals)
            + "};",
            f"  void operator()({entry} &target, const {entry} &value) const {{",
            *assignments,
            "  }",
            "};",
            "",
        ]

    state_names = {slot.name: f"(*{slot.name})" for slot in program.slots}
    slot_policy_members = [
        f"  gfsim::SlotState<{_cpp_type(slot.payload)}> *{slot.name}{{}};"
        for slot in program.slots
    ]
    slot_policy_arguments = ", ".join(
        f"&slot_state_{index}_" for index, _ in enumerate(program.slots)
    )
    candidate_indices = {
        candidate.name: index for index, candidate in enumerate(program.candidates)
    }
    selection_indices = {
        selection.name: index for index, selection in enumerate(program.selections)
    }
    candidate_refs = {
        name: f"table_match_{index}" for name, index in candidate_indices.items()
    }
    selection_refs = {
        name: f"table_selection_{index}" for name, index in selection_indices.items()
    }
    shared_policy_members = [
        *(
            f"  table_match_{index}_cache *table_match_{index}{{}};"
            for index, _ in enumerate(program.candidates)
        ),
        *(
            f"  table_selection_{index}_cache *table_selection_{index}{{}};"
            for index, _ in enumerate(program.selections)
        ),
    ]
    shared_policy_arguments = ", ".join(
        [
            *(f"&table_match_{index}_" for index, _ in enumerate(program.candidates)),
            *(
                f"&table_selection_{index}_"
                for index, _ in enumerate(program.selections)
            ),
        ]
    )
    release_policy_arguments = [
        *(f"&slot_state_{index}_" for index, _ in enumerate(program.slots)),
        *(f"&table_{index}_" for index, _ in enumerate(program.tables)),
        *(f"&table_match_{index}_" for index, _ in enumerate(program.candidates)),
        *(f"&table_selection_{index}_" for index, _ in enumerate(program.selections)),
    ]
    array_leaf: dict[str, tuple[str, tuple[int, ...]]] = {
        leaf: (collection.name, path)
        for collection in arrays
        for leaf, path in _collection_leaves(collection.value)
    }

    def queue_ref(name: str) -> str:
        owner = array_leaf.get(name)
        if owner is None:
            return f"{name}_"
        collection, path = owner
        return f"{collection}_" + "".join(f"[{index}]" for index in path)

    def queue_initializer(name: str) -> str:
        queue = queues_by_name[name]
        return (
            f'gfsim::SimQueue<{_cpp_type(queue.payload)}>("{queue.name}", '
            f"{ids.queues[queue.name]}, {module_ptr(queue_owner[queue.name])}, "
            f"{queue.depth}, "
            "std::numeric_limits<size_t>::max(), nullptr, "
            f"{queue.latency}, {queue.rate})"
        )

    def array_initializer(value: StaticQueueCollection) -> str:
        members = []
        for _, member in value.members:
            members.append(
                queue_initializer(member)
                if isinstance(member, str)
                else array_initializer(member)
            )
        return "{{" + ", ".join(members) + "}}"

    effective_input = {
        consumer: output
        for fanout in fanouts
        for consumer, output in zip(fanout.consumers, fanout.outputs, strict=True)
    }
    scope_members = {
        scope.path: "scope_" + "__".join(scope.path) + "_" for scope in program.scopes
    }

    def module_ptr(path: tuple[str, ...]) -> str:
        return "this" if not path else f"&{scope_members[path]}"

    def attach(path: tuple[str, ...], member: str) -> str:
        if not path:
            return f"    attachChild({member});"
        return f"    {scope_members[path]}.attachChild({member});"

    queue_uses: dict[str, list[tuple[str, ...]]] = {
        queue.name: [] for queue in program.queues
    }
    for queue in program.queues:
        if queue.input_name is not None:
            queue_uses[queue.input_name].append(queue.scope)
    for route in program.routes:
        queue_uses[route.input_name].append(route.scope)
    for fork in program.forks:
        queue_uses[fork.input_name].append(fork.scope)
    for merge in program.merges:
        for input_name in merge.inputs:
            queue_uses[input_name].append(merge.scope)
    for feedback in program.feedbacks:
        queue_uses[feedback.input_name].append(feedback.scope)
    for reorder in program.reorders:
        queue_uses[reorder.input_name].append(reorder.scope)
    for dependency in program.dependencies:
        queue_uses[dependency.input_name].append(dependency.scope)
    for credit in program.credits:
        queue_uses[credit.input_name].append(credit.scope)
    for barrier in program.barriers:
        for input_name in barrier.inputs:
            queue_uses[input_name].append(barrier.scope)
    for memory in program.memories:
        queue_uses[memory.input_name].append(memory.scope)
    for request in program.memory_requests:
        queue_uses[request.input_name].append(request.scope)
    for read in program.table_reads:
        if read.input_name is not None:
            queue_uses[read.input_name].append(read.scope)
    for write in program.table_writes:
        if write.input_name is not None:
            queue_uses[write.input_name].append(write.scope)
    for slot in program.slots:
        queue_uses[slot.input_name].append(slot.scope)
    for sink in program.sinks:
        queue_uses[sink.queue].append(sink.scope)
    queue_owner = {
        queue.name: (
            _common_scope([queue.scope, *queue_uses[queue.name]])
            if queue_uses[queue.name]
            else queue.scope
        )
        for queue in program.queues
    }
    lines = [
        "// Generated by Agentic Circuit Queue/Var; do not edit.",
        '#include "gfsim/bits.h"',
        '#include "gfsim/count_zeros.h"',
        '#include "gfsim/dispatch.h"',
        '#include "gfsim/object.h"',
        '#include "gfsim/popcount.h"',
        '#include "gfsim/priority_encode.h"',
        '#include "gfsim/queue.h"',
        '#include "gfsim/queue_blocks.h"',
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <limits>",
        "#include <tuple>",
        "#include <utility>",
        "",
        "namespace ac_generated {",
        "",
    ]
    if program.specialization_fingerprint is not None:
        lines.insert(1, f"// Specialization: {program.specialization_fingerprint}")
    for payload in program.payloads:
        lines.extend(_emit_payload(payload))
    for queue in program.queues:
        if queue.input_name is None:
            continue
        payload = _cpp_type(queue.payload)
        lines.extend(
            (
                f"struct {queue.name}_policy {{",
                f"  {payload} operator()(const {payload} &item) const {{",
                *_policy_body(queue, queues_by_name[queue.input_name].payload),
                "  }",
                "};",
                "",
            )
        )
    for index, route in enumerate(program.routes):
        payload = _cpp_type(
            next(
                queue.payload
                for queue in program.queues
                if queue.name == route.input_name
            )
        )
        expression = _CppExpression(
            route.argument,
            argument_type=queues_by_name[route.input_name].payload,
        ).emit(route.selector)
        lines.extend(
            (
                f"struct route_{index}_policy {{",
                f"  size_t operator()(const {payload} &item) const {{",
                f"    return static_cast<size_t>({expression});",
                "  }",
                "};",
                "",
            )
        )
    for index, feedback in enumerate(program.feedbacks):
        payload = _cpp_type(
            next(
                queue.payload
                for queue in program.queues
                if queue.name == feedback.output_name
            )
        )
        lines.extend(
            (
                f"struct feedback_{index}_update_policy {{",
                f"  {payload} operator()(const {payload} &item) const {{",
                *_expression_policy_body(
                    feedback.argument,
                    feedback.update,
                    queues_by_name[feedback.input_name].payload,
                ),
                "  }",
                "};",
                "",
                f"struct feedback_{index}_condition_policy {{",
                f"  bool operator()(const {payload} &item) const {{",
                "    return "
                f"{_CppExpression(feedback.argument, argument_type=queues_by_name[feedback.input_name].payload).emit(feedback.condition)};",
                "  }",
                "};",
                "",
            )
        )
    for index, reorder in enumerate(program.reorders):
        payload = _cpp_type(queues_by_name[reorder.input_name].payload)
        expression = _CppExpression(
            reorder.argument,
            argument_type=queues_by_name[reorder.input_name].payload,
        ).emit(reorder.key)
        lines.extend(
            (
                f"struct reorder_{index}_key_policy {{",
                f"  std::uint64_t operator()(const {payload} &item) const {{",
                f"    return static_cast<std::uint64_t>({expression});",
                "  }",
                "};",
                "",
            )
        )
    for index, dependency in enumerate(program.dependencies):
        payload = _cpp_type(queues_by_name[dependency.input_name].payload)
        for suffix, expression_node in (
            ("key", dependency.key),
            ("dependency", dependency.waits_for),
            ("resource", dependency.resource),
            ("cost", dependency.cost),
        ):
            expression = _CppExpression(
                dependency.argument,
                argument_type=queues_by_name[dependency.input_name].payload,
            ).emit(expression_node)
            lines.extend(
                (
                    f"struct dependency_{index}_{suffix}_policy {{",
                    f"  std::uint64_t operator()(const {payload} &item) const {{",
                    f"    return static_cast<std::uint64_t>({expression});",
                    "  }",
                    "};",
                    "",
                )
            )
    for index, credit in enumerate(program.credits):
        payload = _cpp_type(queues_by_name[credit.input_name].payload)
        expression = _CppExpression(
            credit.argument,
            argument_type=queues_by_name[credit.input_name].payload,
        ).emit(credit.cost)
        lines.extend(
            (
                f"struct credit_{index}_cost_policy {{",
                f"  std::uint64_t operator()(const {payload} &item) const {{",
                f"    return static_cast<std::uint64_t>({expression});",
                "  }",
                "};",
                "",
            )
        )
    for index, memory in enumerate(program.memories):
        payload = _cpp_type(queues_by_name[memory.input_name].payload)
        data_type = _cpp_type(memory.data_type)
        policies = (
            ("address", "std::uint64_t", memory.address),
            ("write", "bool", memory.write),
            ("data", data_type, memory.data),
        )
        for suffix, result_type, expression_node in policies:
            expression = _CppExpression(
                memory.argument,
                argument_type=queues_by_name[memory.input_name].payload,
            ).emit(expression_node)
            lines.extend(
                (
                    f"struct memory_{index}_{suffix}_policy {{",
                    f"  {result_type} operator()(size_t, "
                    f"const {payload} &item) const {{",
                    f"    return static_cast<{result_type}>({expression});",
                    "  }",
                    "};",
                    "",
                )
            )
        lines.extend(
            (
                f"struct memory_{index}_response_policy {{",
                f"  {payload} operator()(size_t, const {payload} &item, "
                f"const {data_type} &old_data) const {{",
                "    auto result = item;",
                f"    result.{memory.result_field} = old_data;",
                "    return result;",
                "  }",
                "};",
                "",
            )
        )
    requests_by_instance = {
        instance.name: tuple(
            sorted(
                (
                    request
                    for request in program.memory_requests
                    if request.instance == instance.name
                ),
                key=lambda request: (
                    request.scope,
                    request.order,
                    request.output_name,
                ),
            )
        )
        for instance in program.memory_instances
    }
    for index, instance in enumerate(program.memory_instances):
        requests = requests_by_instance[instance.name]
        payload = _cpp_type(queues_by_name[requests[0].input_name].payload)
        data_type = _cpp_type(instance.data_type)
        for suffix, result_type, expression_name in (
            ("address", "std::uint64_t", "address"),
            ("write", "bool", "write"),
            ("data", data_type, "data"),
        ):
            lines.extend(
                (
                    f"struct memory_instance_{index}_{suffix}_policy {{",
                    f"  {result_type} operator()(size_t endpoint, "
                    f"const {payload} &item) const {{",
                    "    switch (endpoint) {",
                )
            )
            for ordinal, request in enumerate(requests):
                expression = _CppExpression(
                    request.argument,
                    argument_type=queues_by_name[request.input_name].payload,
                ).emit(getattr(request, expression_name))
                lines.append(
                    f"    case {ordinal}: return static_cast<{result_type}>({expression});"
                )
            lines.extend(("    default: return {};", "    }", "  }", "};", ""))
        lines.extend(
            (
                f"struct memory_instance_{index}_response_policy {{",
                f"  {payload} operator()(size_t endpoint, const {payload} &item, "
                f"const {data_type} &old_data) const {{",
                "    auto result = item;",
                "    switch (endpoint) {",
            )
        )
        for ordinal, request in enumerate(requests):
            lines.append(
                f"    case {ordinal}: result.{request.result_field} = old_data; break;"
            )
        lines.extend(
            (
                "    default: break;",
                "    }",
                "    return result;",
                "  }",
                "};",
                "",
            )
        )
    for index, candidate in enumerate(program.candidates):
        table = tables_by_name[candidate.table]
        entry = _cpp_type(table.entry_type)
        predicate = _CppExpression(
            candidate.argument,
            state_names,
            argument_type=table.entry_type,
            table_entries=table_entries,
            table_names={candidate.table: "table"},
        ).emit(candidate.predicate)
        lines.extend(
            (
                f"struct table_match_{index}_predicate_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                f"  bool operator()(const {entry} &item) const {{",
                f"    return static_cast<bool>({predicate});",
                "  }",
                "};",
                f"using table_match_{index}_cache = ",
                f"    gfsim::TableMatchCache<{entry}, table_match_{index}_predicate_policy>;",
                "",
            )
        )
    for index, selection in enumerate(program.selections):
        table = tables_by_name[selection.table]
        entry = _cpp_type(table.entry_type)
        candidate_index = candidate_indices[selection.candidates]
        if selection.key is None:
            key = "0"
        else:
            assert selection.argument is not None
            key = _CppExpression(
                selection.argument,
                argument_type=table.entry_type,
            ).emit(selection.key)
        policy = {
            "first": "First",
            "min": "Min",
            "max": "Max",
        }[selection.policy]
        lines.extend(
            (
                f"struct table_selection_{index}_mask_policy {{",
                f"  table_match_{candidate_index}_cache *match{{}};",
                "  std::uint64_t operator()(gfsim::Epoch epoch) const {",
                "    return match->get(epoch);",
                "  }",
                "};",
                "",
                f"struct table_selection_{index}_key_policy {{",
                f"  std::uint64_t operator()(const {entry} &item) const {{",
                f"    return static_cast<std::uint64_t>({key});",
                "  }",
                "};",
                f"using table_selection_{index}_cache = ",
                f"    gfsim::TableSelectionCache<{entry}, ",
                f"        table_selection_{index}_mask_policy, ",
                f"        table_selection_{index}_key_policy>;",
                f"inline constexpr auto table_selection_{index}_policy = ",
                f"    gfsim::TableChoosePolicy::{policy};",
                "",
            )
        )
    for index, read in enumerate(program.table_reads):
        table = tables_by_name[read.table]
        entry = _cpp_type(table.entry_type)
        expression = _CppExpression(
            read.argument or "__state",
            state_names,
            argument_type=(
                None
                if read.input_name is None
                else queues_by_name[read.input_name].payload
            ),
            candidates=candidates_by_name,
            selections=selections_by_name,
            candidate_refs=candidate_refs,
            selection_refs=selection_refs,
            table_entries=table_entries,
            table_names={read.table: "table"},
        )
        if read.argument is None:
            address = expression.emit(read.address)
            when = _emit_table_state_expression(
                read.when,
                read.view_alias,
                "table",
                address,
                expression=expression,
            )
            address_signature = "std::uint64_t operator()(gfsim::Epoch epoch) const"
            when_signature = "bool operator()(gfsim::Epoch epoch) const"
        else:
            input_type = _cpp_type(queues_by_name[read.input_name].payload)
            address = expression.emit(read.address)
            when = _emit_table_state_expression(
                read.when,
                read.view_alias,
                "table",
                address,
                read.argument,
                expression,
            )
            address_signature = (
                f"std::uint64_t operator()(gfsim::Epoch epoch, "
                f"const {input_type} &item) const"
            )
            when_signature = (
                f"bool operator()(gfsim::Epoch epoch, const {input_type} &item) const"
            )
        lines.extend(
            (
                f"struct table_read_{index}_address_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                *shared_policy_members,
                f"  {address_signature} {{",
                f"    return static_cast<std::uint64_t>({address});",
                "  }",
                "};",
                "",
                f"struct table_read_{index}_when_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                *shared_policy_members,
                f"  {when_signature} {{",
                f"    return static_cast<bool>({when});",
                "  }",
                "};",
                "",
            )
        )
    for index, write in enumerate(program.table_writes):
        table = tables_by_name[write.table]
        entry = _cpp_type(table.entry_type)
        input_type = (
            None
            if write.input_name is None
            else _cpp_type(queues_by_name[write.input_name].payload)
        )
        argument = write.argument or ""
        expression_context = dict(
            argument_type=(
                None
                if write.input_name is None
                else queues_by_name[write.input_name].payload
            ),
            candidates=candidates_by_name,
            selections=selections_by_name,
            candidate_refs=candidate_refs,
            selection_refs=selection_refs,
            table_entries=table_entries,
            table_names={write.table: "table"},
        )
        address = _CppExpression(argument, state_names, **expression_context).emit(
            write.address
        )
        enable = _CppExpression(argument, state_names, **expression_context).emit(
            write.enable
        )
        signature = "gfsim::Epoch epoch"
        if input_type is not None:
            signature += f", const {input_type} &item"
        lines.extend(
            (
                f"struct table_write_{index}_address_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                *shared_policy_members,
                f"  std::uint64_t operator()({signature}) const {{",
                f"    return static_cast<std::uint64_t>({address});",
                "  }",
                "};",
                "",
                f"struct table_write_{index}_enable_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                *shared_policy_members,
                f"  bool operator()({signature}) const {{",
                f"    return static_cast<bool>({enable});",
                "  }",
                "};",
                "",
                f"struct table_write_{index}_value_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                *shared_policy_members,
                f"  {entry} operator()({signature}) const {{",
            )
        )
        if write.value is not None:
            lines.append(
                f"    return {_CppExpression(argument, state_names, **expression_context).emit(write.value)};"
            )
        else:
            lines.append(
                f"    auto result = table->checkedAt(static_cast<size_t>({address}));"
            )
            expression = _CppExpression(argument, state_names, **expression_context)
            for field, value in write.patch_fields:
                lines.append(f"    result.{field} = {expression.emit(value)};")
            lines.append("    return result;")
        lines.extend(("  }", "};", ""))
        lines.extend(
            table_merge_policy(
                f"table_write_{index}_merge_policy", table, write.write_fields
            )
        )

    for index, write in enumerate(program.masked_table_writes):
        table = tables_by_name[write.table]
        entry = _cpp_type(table.entry_type)
        expression_context = dict(
            argument_type=table.entry_type,
            candidates=candidates_by_name,
            selections=selections_by_name,
            candidate_refs=candidate_refs,
            selection_refs=selection_refs,
            table_entries=table_entries,
            table_names={write.table: "table"},
        )
        mask = _CppExpression("", state_names, **expression_context).emit(
            ast.Name(id=write.candidates, ctx=ast.Load())
        )
        enable = _CppExpression("", state_names, **expression_context).emit(
            write.enable
        )
        lines.extend(
            (
                f"struct table_masked_write_{index}_mask_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                *shared_policy_members,
                "  std::uint64_t operator()(gfsim::Epoch epoch) const {",
                f"    return static_cast<std::uint64_t>({mask});",
                "  }",
                "};",
                "",
                f"struct table_masked_write_{index}_enable_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                *shared_policy_members,
                "  bool operator()(gfsim::Epoch epoch) const {",
                f"    return static_cast<bool>({enable});",
                "  }",
                "};",
                "",
                f"struct table_masked_write_{index}_value_policy {{",
                f"  gfsim::SimTable<{entry}> *table{{}};",
                *slot_policy_members,
                *shared_policy_members,
                f"  {entry} operator()(gfsim::Epoch epoch, "
                f"const {entry} &item) const {{",
            )
        )
        expression = _CppExpression("__old", state_names, **expression_context)
        if write.value is not None:
            lines.append(f"    return {expression.emit(write.value)};")
        else:
            lines.append("    auto result = item;")
            for field, value in write.patch_fields:
                lines.append(f"    result.{field} = {expression.emit(value)};")
            lines.append("    return result;")
        lines.extend(("  }", "};", ""))
        lines.extend(
            table_merge_policy(
                f"table_masked_write_{index}_merge_policy",
                table,
                write.write_fields,
            )
        )

    for index, release in enumerate(program.slot_releases):
        slot = slots_by_name[release.slot]
        payload = _cpp_type(slot.payload)
        release_table_names = {
            table.name: f"table_{table.name}" for table in program.tables
        }
        condition = _CppExpression(
            "",
            state_names,
            candidates=candidates_by_name,
            selections=selections_by_name,
            candidate_refs=candidate_refs,
            selection_refs=selection_refs,
            table_entries=table_entries,
            table_names=release_table_names,
            require_shared_refs=True,
        ).emit(release.when)
        table_policy_members = [
            f"  gfsim::SimTable<{_cpp_type(table.entry_type)}> *table_{table.name}{{}};"
            for table in program.tables
        ]
        lines.extend(
            (
                f"struct slot_{index}_release_policy {{",
                *slot_policy_members,
                *table_policy_members,
                *shared_policy_members,
                "  bool operator()(gfsim::Epoch epoch) const {",
                f"    return static_cast<bool>({condition});",
                "  }",
                "};",
                "",
            )
        )

    class_name = "".join(part.capitalize() for part in program.system.split("_"))
    lines.extend((f"class {class_name} final : public gfsim::Module {{", "public:"))
    lines.append(
        f'  {class_name}() : gfsim::Module("{program.system}", '
        "gfsim::kInvalidObjectId, nullptr),"
    )
    initializers: list[str] = []
    for scope in program.scopes:
        initializers.append(
            f'{scope_members[scope.path]}("{scope.name}", '
            f"gfsim::kInvalidObjectId, {module_ptr(scope.path[:-1])})"
        )
    for queue in program.queues:
        if queue.name in array_leaf:
            continue
        payload = _cpp_type(queue.payload)
        initializers.append(
            f'{queue.name}_("{queue.name}", {ids.queues[queue.name]}, '
            f"{module_ptr(queue_owner[queue.name])}, "
            f"{queue.depth}, std::numeric_limits<size_t>::max(), nullptr, "
            f"{queue.latency}, {queue.rate})"
        )
    for collection in arrays:
        initializers.append(f"{collection.name}_" + array_initializer(collection.value))
    for fanout in fanouts:
        for output in fanout.outputs:
            initializers.append(
                f'{output}_("{output}", {ids.fanout_queues[output]}, '
                f"{module_ptr(fanout.scope)}, "
                "1, std::numeric_limits<size_t>::max(), nullptr, 1)"
            )
    for index, feedback in enumerate(program.feedbacks):
        payload = _cpp_type(
            next(
                queue.payload
                for queue in program.queues
                if queue.name == feedback.output_name
            )
        )
        initializers.append(
            f'feedback_{index}_state_("feedback_{index}_state", '
            f"{ids.feedback_states[index]}, {module_ptr(feedback.scope)}, 1, "
            "std::numeric_limits<size_t>::max(), nullptr, 1)"
        )
    for index, reorder in enumerate(program.reorders):
        initializers.append(
            f'reorder_{index}_block_("reorder_{index}", '
            f"{ids.reorders[index]}, {module_ptr(reorder.scope)}, "
            f"{queue_ref(reorder.input_name)}, {queue_ref(reorder.output_name)})"
        )
    for index, dependency in enumerate(program.dependencies):
        suffix = (
            ")"
            if dependency.provider == "schedule"
            else f", {dependency.capacity}, {dependency.resources}, "
            f"{dependency.no_dependency})"
        )
        initializers.append(
            f'dependency_{index}_block_("{dependency.provider}_{index}", '
            f"{ids.dependencies[index]}, {module_ptr(dependency.scope)}, "
            f"{queue_ref(dependency.input_name)}, "
            f"{queue_ref(dependency.output_name)}{suffix}"
        )
    for index, credit in enumerate(program.credits):
        suffix = ")" if credit.provider == "engine" else f", {credit.credits})"
        initializers.append(
            f'credit_{index}_block_("{credit.provider}_{index}", '
            f"{ids.credits[index]}, {module_ptr(credit.scope)}, "
            f"{queue_ref(credit.input_name)}, {queue_ref(credit.output_name)}"
            f"{suffix}"
        )
    for index, memory in enumerate(program.memories):
        payload = _cpp_type(queues_by_name[memory.input_name].payload)
        initializers.append(
            f'memory_{index}_block_("table_{index}", '
            f"{ids.memories[index]}, {module_ptr(memory.scope)}, "
            f"std::array<gfsim::SimQueue<{payload}> *, 1>{{"
            f"&{queue_ref(memory.input_name)}}}, "
            f"std::array<gfsim::SimQueue<{payload}> *, 1>{{"
            f"&{queue_ref(memory.output_name)}}}, {memory.entries}, "
            f"{memory.init}, {memory.latency})"
        )
    for index, instance in enumerate(program.memory_instances):
        requests = requests_by_instance[instance.name]
        payload = _cpp_type(queues_by_name[requests[0].input_name].payload)
        inputs = ", ".join(f"&{queue_ref(request.input_name)}" for request in requests)
        outputs = ", ".join(
            f"&{queue_ref(request.output_name)}" for request in requests
        )
        count = len(requests)
        initializers.append(
            f'memory_instance_{index}_block_("{instance.name}", '
            f"{ids.memory_instances[index]}, {module_ptr(instance.scope)}, "
            f"std::array<gfsim::SimQueue<{payload}> *, {count}>{{{inputs}}}, "
            f"std::array<gfsim::SimQueue<{payload}> *, {count}>{{{outputs}}}, "
            f"{instance.entries}, {instance.init}, {instance.latency})"
        )
    for index, table in enumerate(program.tables):
        initializers.append(
            f'table_{index}_("{table.name}", {ids.tables[index]}, '
            f"{module_ptr(table.scope)}, {table.entries})"
        )
    for index, candidate in enumerate(program.candidates):
        table_index = table_indices[candidate.table]
        predicate_arguments = f"&table_{table_index}_"
        if slot_policy_arguments:
            predicate_arguments += f", {slot_policy_arguments}"
        initializers.append(
            f"table_match_{index}_(table_{table_index}_, "
            f"table_match_{index}_predicate_policy{{{predicate_arguments}}})"
        )
    for index, selection in enumerate(program.selections):
        table_index = table_indices[selection.table]
        candidate_index = candidate_indices[selection.candidates]
        initializers.append(
            f"table_selection_{index}_(table_{table_index}_, "
            f"table_selection_{index}_mask_policy{{&table_match_{candidate_index}_}}, "
            f"table_selection_{index}_key_policy{{}}, "
            f"table_selection_{index}_policy)"
        )
    for index, slot in enumerate(program.slots):
        release_index = next(
            release_index
            for release_index, release in enumerate(program.slot_releases)
            if release.slot == slot.name
        )
        initializers.append(
            f'slot_{index}_block_("{slot.name}", {ids.slots[index]}, '
            f"{module_ptr(slot.scope)}, {queue_ref(slot.input_name)}, "
            f"slot_state_{index}_, slot_{release_index}_release_policy{{"
            f"{', '.join(release_policy_arguments)}}})"
        )
    for index, write in enumerate(program.table_writes):
        table_index = table_indices[write.table]
        runtime_arguments = f"table_{table_index}_"
        if write.input_name is not None:
            runtime_arguments += f", {queue_ref(write.input_name)}"
        policy_arguments = ", ".join(
            value for value in (slot_policy_arguments, shared_policy_arguments) if value
        )
        address_arguments = f"&table_{table_index}_"
        if policy_arguments:
            address_arguments += f", {policy_arguments}"
        address_init = "{" + address_arguments + "}"
        value_arguments = f"&table_{table_index}_"
        if policy_arguments:
            value_arguments += f", {policy_arguments}"
        initializers.append(
            f'table_write_{index}_block_("table_write_{index}", '
            f"{ids.table_writes[index]}, {module_ptr(write.scope)}, "
            f"{runtime_arguments}, "
            f"table_write_{index}_address_policy{address_init}, "
            f"table_write_{index}_enable_policy{address_init}, "
            f"table_write_{index}_value_policy{{{value_arguments}}}, "
            f"table_write_{index}_merge_policy{{}}, "
            + (
                "gfsim::TableWriteMode::Replace)"
                if write.write_mode == "replace"
                else "gfsim::TableWriteMode::FieldMerge)"
            )
        )
    for index, write in enumerate(program.masked_table_writes):
        table_index = table_indices[write.table]
        policy_arguments = f"&table_{table_index}_"
        all_policy_arguments = ", ".join(
            value for value in (slot_policy_arguments, shared_policy_arguments) if value
        )
        if all_policy_arguments:
            policy_arguments += f", {all_policy_arguments}"
        policy_init = "{" + policy_arguments + "}"
        initializers.append(
            f'table_masked_write_{index}_block_("table_masked_write_{index}", '
            f"{ids.masked_table_writes[index]}, {module_ptr(write.scope)}, "
            f"table_{table_index}_, "
            f"table_masked_write_{index}_mask_policy{policy_init}, "
            f"table_masked_write_{index}_enable_policy{policy_init}, "
            f"table_masked_write_{index}_value_policy{policy_init}, "
            f"table_masked_write_{index}_merge_policy{{}})"
        )
    for index, read in enumerate(program.table_reads):
        table_index = table_indices[read.table]
        policy_arguments = f"&table_{table_index}_"
        all_policy_arguments = ", ".join(
            value for value in (slot_policy_arguments, shared_policy_arguments) if value
        )
        if all_policy_arguments:
            policy_arguments += f", {all_policy_arguments}"
        policy_init = "{" + policy_arguments + "}"
        if read.input_name is None:
            arguments = f"table_{table_index}_, {queue_ref(read.output_name)}"
        else:
            arguments = (
                f"table_{table_index}_, {queue_ref(read.input_name)}, "
                f"{queue_ref(read.output_name)}"
            )
        initializers.append(
            f'table_read_{index}_block_("table_read_{index}", '
            f"{ids.table_reads[index]}, {module_ptr(read.scope)}, {arguments}, "
            f"table_read_{index}_address_policy{policy_init}, "
            f"table_read_{index}_when_policy{policy_init})"
        )
    for index, fanout in enumerate(fanouts):
        payload = _cpp_type(fanout.payload)
        outputs = ", ".join(f"&{queue_ref(name)}" for name in fanout.outputs)
        initializers.append(
            f'broadcast_{index}_block_("broadcast_{index}", '
            f"{ids.broadcasts[index]}, {module_ptr(fanout.scope)}, "
            f"{queue_ref(fanout.source)}, "
            f"std::array<gfsim::SimQueue<{payload}> *, {len(fanout.outputs)}>"
            f"{{{outputs}}})"
        )
    for index, route in enumerate(program.routes):
        payload = _cpp_type(
            next(
                queue.payload
                for queue in program.queues
                if queue.name == route.input_name
            )
        )
        outputs = ", ".join(f"&{queue_ref(name)}" for name in route.outputs)
        initializers.append(
            f'route_{index}_block_("route_{index}", {ids.routes[index]}, '
            f"{module_ptr(route.scope)}, "
            f"{queue_ref(route.input_name)}, std::array<gfsim::SimQueue<{payload}> *, "
            f"{len(route.outputs)}>{{{outputs}}})"
        )
    for index, fork in enumerate(program.forks):
        payload = _cpp_type(queues_by_name[fork.input_name].payload)
        outputs = ", ".join(f"&{queue_ref(name)}" for name in fork.outputs)
        initializers.append(
            f'fork_{index}_block_("fork_{index}", {ids.forks[index]}, '
            f"{module_ptr(fork.scope)}, {queue_ref(fork.input_name)}, "
            f"std::array<gfsim::SimQueue<{payload}> *, {len(fork.outputs)}>"
            f"{{{outputs}}})"
        )
    for index, merge in enumerate(program.merges):
        payload = _cpp_type(
            next(
                queue.payload for queue in program.queues if queue.name == merge.output
            )
        )
        inputs = ", ".join(f"&{queue_ref(name)}" for name in merge.inputs)
        policy = (
            "gfsim::QueueMergePolicy::RoundRobin"
            if merge.policy == "round_robin"
            else "gfsim::QueueMergePolicy::Priority"
        )
        initializers.append(
            f'merge_{index}_block_("merge_{index}", {ids.merges[index]}, '
            f"{module_ptr(merge.scope)}, "
            f"std::array<gfsim::SimQueue<{payload}> *, {len(merge.inputs)}>"
            f"{{{inputs}}}, {queue_ref(merge.output)}, {policy})"
        )
    for index, barrier in enumerate(program.barriers):
        inputs = ", ".join(f"&{queue_ref(name)}" for name in barrier.inputs)
        outputs = ", ".join(f"&{queue_ref(name)}" for name in barrier.outputs)
        initializers.append(
            f'barrier_{index}_block_("barrier_{index}", '
            f"{ids.barriers[index]}, {module_ptr(barrier.scope)}, "
            f"std::tuple{{{inputs}}}, std::tuple{{{outputs}}})"
        )
    for index, feedback in enumerate(program.feedbacks):
        initializers.append(
            f'feedback_{index}_block_("feedback_{index}", '
            f"{ids.feedbacks[index]}, {module_ptr(feedback.scope)}, "
            f"{queue_ref(feedback.input_name)}, "
            f"feedback_{index}_state_, {queue_ref(feedback.output_name)}, "
            f"{feedback.max_iterations})"
        )
    for queue in program.queues:
        if queue.input_name is None:
            continue
        input_name = effective_input.get(queue.name, queue.input_name)
        initializers.append(
            f'{queue.name}_block_("{queue.name}_transform", '
            f"{ids.transforms[queue.name]}, {module_ptr(queue.scope)}, "
            f"{queue_ref(input_name)}, "
            f"{queue_ref(queue.name)})"
        )
    for index, sink in enumerate(program.sinks):
        initializers.append(
            f'sink_{index}_("sink_{index}", {ids.sinks[index]}, '
            f"{module_ptr(sink.scope)}, "
            f"{queue_ref(sink.queue)})"
        )
    for index, initializer in enumerate(initializers):
        suffix = "," if index + 1 < len(initializers) else ""
        lines.append(f"        {initializer}{suffix}")
    lines.extend(("  {", f'    setPath("/{program.system}");'))
    for scope in program.scopes:
        lines.append(attach(scope.path[:-1], scope_members[scope.path]))
    for queue in program.queues:
        lines.append(attach(queue_owner[queue.name], queue_ref(queue.name)))
    for fanout in fanouts:
        for output in fanout.outputs:
            lines.append(attach(fanout.scope, f"{output}_"))
    for index, _ in enumerate(program.feedbacks):
        lines.append(attach(program.feedbacks[index].scope, f"feedback_{index}_state_"))
    for index, reorder in enumerate(program.reorders):
        lines.append(attach(reorder.scope, f"reorder_{index}_block_"))
    for index, dependency in enumerate(program.dependencies):
        lines.append(attach(dependency.scope, f"dependency_{index}_block_"))
    for index, credit in enumerate(program.credits):
        lines.append(attach(credit.scope, f"credit_{index}_block_"))
    for index, memory in enumerate(program.memories):
        lines.append(attach(memory.scope, f"memory_{index}_block_"))
    for index, instance in enumerate(program.memory_instances):
        lines.append(attach(instance.scope, f"memory_instance_{index}_block_"))
    for index, table in enumerate(program.tables):
        lines.append(attach(table.scope, f"table_{index}_"))
    for index, slot in enumerate(program.slots):
        lines.append(attach(slot.scope, f"slot_{index}_block_"))
    for index, write in enumerate(program.table_writes):
        lines.append(attach(write.scope, f"table_write_{index}_block_"))
    for index, write in enumerate(program.masked_table_writes):
        lines.append(attach(write.scope, f"table_masked_write_{index}_block_"))
    for index, read in enumerate(program.table_reads):
        lines.append(attach(read.scope, f"table_read_{index}_block_"))
    for index, _ in enumerate(fanouts):
        lines.append(attach(fanouts[index].scope, f"broadcast_{index}_block_"))
    for queue in program.queues:
        if queue.input_name is not None:
            lines.append(attach(queue.scope, f"{queue.name}_block_"))
    for index, _ in enumerate(program.routes):
        lines.append(attach(program.routes[index].scope, f"route_{index}_block_"))
    for index, fork in enumerate(program.forks):
        lines.append(attach(fork.scope, f"fork_{index}_block_"))
    for index, _ in enumerate(program.merges):
        lines.append(attach(program.merges[index].scope, f"merge_{index}_block_"))
    for index, barrier in enumerate(program.barriers):
        lines.append(attach(barrier.scope, f"barrier_{index}_block_"))
    for index, _ in enumerate(program.feedbacks):
        lines.append(attach(program.feedbacks[index].scope, f"feedback_{index}_block_"))
    for index, _ in enumerate(program.sinks):
        lines.append(attach(program.sinks[index].scope, f"sink_{index}_"))
    lines.extend(
        ("  }", "", "  void reset() override {", "    gfsim::Module::reset();")
    )
    for index, _ in enumerate(program.candidates):
        lines.append(f"    table_match_{index}_.reset();")
    for index, _ in enumerate(program.selections):
        lines.append(f"    table_selection_{index}_.reset();")
    lines.extend(("  }", ""))
    for queue in program.queues:
        payload = _cpp_type(queue.payload)
        lines.append(
            f"  gfsim::SimQueue<{payload}> &{queue.name}() {{ "
            f"return {queue_ref(queue.name)}; }}"
        )
    for index, sink in enumerate(program.sinks):
        payload = _cpp_type(
            next(q.payload for q in program.queues if q.name == sink.queue)
        )
        lines.append(
            f"  const std::vector<{payload}> &sink_{index}_values() const {{ "
            f"return sink_{index}_.received(); }}"
        )
    object_count = (
        len(program.queues)
        + len(ids.fanout_queues)
        + len(ids.feedback_states)
        + len(ids.broadcasts)
        + len(ids.transforms)
        + len(ids.routes)
        + len(ids.forks)
        + len(ids.merges)
        + len(ids.barriers)
        + len(ids.feedbacks)
        + len(ids.reorders)
        + len(ids.dependencies)
        + len(ids.credits)
        + len(ids.memories)
        + len(ids.memory_instances)
        + len(ids.tables)
        + len(ids.table_reads)
        + len(ids.table_writes)
        + len(ids.masked_table_writes)
        + len(ids.slots)
        + len(ids.sinks)
    )
    lines.extend(
        (
            "",
            f"  std::array<gfsim::DispatchRow, {object_count}> dispatch_rows() {{",
            f"    return std::array<gfsim::DispatchRow, {object_count}>{{",
        )
    )
    rows: list[str] = []
    for queue in program.queues:
        rows.append(f"gfsim::makeDispatchRow(&{queue_ref(queue.name)})")
    for fanout in fanouts:
        for output in fanout.outputs:
            rows.append(f"gfsim::makeDispatchRow(&{output}_)")
    for index, _ in enumerate(program.feedbacks):
        rows.append(f"gfsim::makeDispatchRow(&feedback_{index}_state_)")
    for index, _ in enumerate(fanouts):
        rows.append(f"gfsim::makeDispatchRow(&broadcast_{index}_block_)")
    for queue in program.queues:
        if queue.input_name is not None:
            rows.append(f"gfsim::makeDispatchRow(&{queue.name}_block_)")
    for index, _ in enumerate(program.routes):
        rows.append(f"gfsim::makeDispatchRow(&route_{index}_block_)")
    for index, _ in enumerate(program.forks):
        rows.append(f"gfsim::makeDispatchRow(&fork_{index}_block_)")
    for index, _ in enumerate(program.merges):
        rows.append(f"gfsim::makeDispatchRow(&merge_{index}_block_)")
    for index, _ in enumerate(program.barriers):
        rows.append(f"gfsim::makeDispatchRow(&barrier_{index}_block_)")
    for index, _ in enumerate(program.feedbacks):
        rows.append(f"gfsim::makeDispatchRow(&feedback_{index}_block_)")
    for index, _ in enumerate(program.reorders):
        rows.append(f"gfsim::makeDispatchRow(&reorder_{index}_block_)")
    for index, _ in enumerate(program.dependencies):
        rows.append(f"gfsim::makeDispatchRow(&dependency_{index}_block_)")
    for index, _ in enumerate(program.credits):
        rows.append(f"gfsim::makeDispatchRow(&credit_{index}_block_)")
    for index, _ in enumerate(program.memories):
        rows.append(f"gfsim::makeDispatchRow(&memory_{index}_block_)")
    for index, _ in enumerate(program.memory_instances):
        rows.append(f"gfsim::makeDispatchRow(&memory_instance_{index}_block_)")
    for index, _ in enumerate(program.tables):
        rows.append(f"gfsim::makeDispatchRow(&table_{index}_)")
    for index, _ in enumerate(program.slots):
        rows.append(f"gfsim::makeDispatchRow(&slot_{index}_block_)")
    for index, _ in enumerate(program.table_writes):
        rows.append(f"gfsim::makeDispatchRow(&table_write_{index}_block_)")
    for index, _ in enumerate(program.masked_table_writes):
        rows.append(f"gfsim::makeDispatchRow(&table_masked_write_{index}_block_)")
    for index, _ in enumerate(program.table_reads):
        rows.append(f"gfsim::makeDispatchRow(&table_read_{index}_block_)")
    for index, _ in enumerate(program.sinks):
        rows.append(f"gfsim::makeDispatchRow(&sink_{index}_)")
    for index, row in enumerate(rows):
        suffix = "," if index + 1 < len(rows) else ""
        lines.append(f"        {row}{suffix}")
    lines.extend(("    };", "  }", "", "private:"))
    for scope in program.scopes:
        lines.append(f"  gfsim::Module {scope_members[scope.path]};")
    for queue in program.queues:
        if queue.name in array_leaf:
            continue
        lines.append(f"  gfsim::SimQueue<{_cpp_type(queue.payload)}> {queue.name}_;")
    for collection in arrays:
        lines.append(
            f"  {_array_cpp_type(collection.value, queues_by_name)} {collection.name}_;"
        )
    for fanout in fanouts:
        for output in fanout.outputs:
            lines.append(f"  gfsim::SimQueue<{_cpp_type(fanout.payload)}> {output}_;")
    for index, feedback in enumerate(program.feedbacks):
        payload = _cpp_type(
            next(
                queue.payload
                for queue in program.queues
                if queue.name == feedback.output_name
            )
        )
        lines.append(
            f"  gfsim::SimQueue<gfsim::FeedbackToken<{payload}>> "
            f"feedback_{index}_state_;"
        )
    for index, reorder in enumerate(program.reorders):
        payload = _cpp_type(queues_by_name[reorder.input_name].payload)
        lines.append(
            f"  gfsim::Reorder<{payload}, {reorder.capacity}, {reorder.start}, "
            f"reorder_{index}_key_policy> "
            f"reorder_{index}_block_;"
        )
    for index, dependency in enumerate(program.dependencies):
        payload = _cpp_type(queues_by_name[dependency.input_name].payload)
        if dependency.provider == "schedule":
            lines.append(
                f"  gfsim::Schedule<{payload}, {dependency.capacity}, "
                f"{dependency.resources}, {dependency.no_dependency}, "
                f"dependency_{index}_key_policy, "
                f"dependency_{index}_dependency_policy, "
                f"dependency_{index}_resource_policy, "
                f"dependency_{index}_cost_policy> dependency_{index}_block_;"
            )
        else:
            lines.append(
                f"  gfsim::QueueDependency<{payload}, "
                f"dependency_{index}_key_policy, "
                f"dependency_{index}_dependency_policy, "
                f"dependency_{index}_resource_policy, "
                f"dependency_{index}_cost_policy> dependency_{index}_block_;"
            )
    for index, credit in enumerate(program.credits):
        payload = _cpp_type(queues_by_name[credit.input_name].payload)
        provider = (
            f"gfsim::Engine<{payload}, {credit.credits}, "
            if credit.provider == "engine"
            else f"gfsim::QueueCredit<{payload}, "
        )
        lines.append(f"  {provider}credit_{index}_cost_policy> credit_{index}_block_;")
    for index, memory in enumerate(program.memories):
        payload = _cpp_type(queues_by_name[memory.input_name].payload)
        data_type = _cpp_type(memory.data_type)
        lines.append(
            f"  gfsim::QueueMemoryArbiter<{payload}, {data_type}, 1, "
            f"memory_{index}_address_policy, "
            f"memory_{index}_write_policy, memory_{index}_data_policy, "
            f"memory_{index}_response_policy> memory_{index}_block_;"
        )
    for index, instance in enumerate(program.memory_instances):
        requests = requests_by_instance[instance.name]
        payload = _cpp_type(queues_by_name[requests[0].input_name].payload)
        data_type = _cpp_type(instance.data_type)
        lines.append(
            f"  gfsim::QueueMemoryArbiter<{payload}, {data_type}, {len(requests)}, "
            f"memory_instance_{index}_address_policy, "
            f"memory_instance_{index}_write_policy, "
            f"memory_instance_{index}_data_policy, "
            f"memory_instance_{index}_response_policy> "
            f"memory_instance_{index}_block_;"
        )
    for index, table in enumerate(program.tables):
        lines.append(
            f"  gfsim::SimTable<{_cpp_type(table.entry_type)}> table_{index}_;"
        )
    for index, _ in enumerate(program.candidates):
        lines.append(f"  table_match_{index}_cache table_match_{index}_;")
    for index, _ in enumerate(program.selections):
        lines.append(f"  table_selection_{index}_cache table_selection_{index}_;")
    for index, slot in enumerate(program.slots):
        payload = _cpp_type(slot.payload)
        release_index = next(
            release_index
            for release_index, release in enumerate(program.slot_releases)
            if release.slot == slot.name
        )
        lines.append(f"  gfsim::SlotState<{payload}> slot_state_{index}_;")
        lines.append(
            f"  gfsim::QueueSlot<{payload}, slot_{release_index}_release_policy> "
            f"slot_{index}_block_;"
        )
    for index, write in enumerate(program.table_writes):
        table = tables_by_name[write.table]
        entry = _cpp_type(table.entry_type)
        if write.input_name is None:
            provider = f"gfsim::TableWriteSource<{entry}, "
        else:
            provider = (
                f"gfsim::QueueTableWrite<"
                f"{_cpp_type(queues_by_name[write.input_name].payload)}, {entry}, "
            )
        lines.append(
            f"  {provider}table_write_{index}_address_policy, "
            f"table_write_{index}_enable_policy, table_write_{index}_value_policy, "
            f"table_write_{index}_merge_policy> "
            f"table_write_{index}_block_;"
        )
    for index, write in enumerate(program.masked_table_writes):
        table = tables_by_name[write.table]
        entry = _cpp_type(table.entry_type)
        lines.append(
            f"  gfsim::TableMaskedWriteSource<{entry}, "
            f"table_masked_write_{index}_mask_policy, "
            f"table_masked_write_{index}_enable_policy, "
            f"table_masked_write_{index}_value_policy, "
            f"table_masked_write_{index}_merge_policy> "
            f"table_masked_write_{index}_block_;"
        )
    for index, read in enumerate(program.table_reads):
        table = tables_by_name[read.table]
        entry = _cpp_type(table.entry_type)
        if read.input_name is None:
            provider = (
                f"gfsim::TableReadSource<{entry}, "
                f"table_read_{index}_address_policy, table_read_{index}_when_policy>"
            )
        else:
            provider = (
                f"gfsim::QueueTableRead<"
                f"{_cpp_type(queues_by_name[read.input_name].payload)}, {entry}, "
                f"table_read_{index}_address_policy, table_read_{index}_when_policy>"
            )
        lines.append(f"  {provider} table_read_{index}_block_;")
    for index, fanout in enumerate(fanouts):
        payload = _cpp_type(fanout.payload)
        lines.append(
            f"  gfsim::QueueBroadcast<{payload}, {len(fanout.outputs)}> "
            f"broadcast_{index}_block_;"
        )
    for queue in program.queues:
        if queue.input_name is None:
            continue
        payload = _cpp_type(queue.payload)
        if queue.provider == "compute":
            provider = (
                f"gfsim::Compute<{payload}, {payload}, {queue.rate}, "
                f"{queue.name}_policy>"
            )
        elif queue.provider == "pipeline":
            provider = f"gfsim::Pipeline<{payload}, {queue.latency}, {queue.rate}>"
        else:
            provider = (
                f"gfsim::QueueTransform<{payload}, {payload}, {queue.name}_policy>"
            )
        lines.append(f"  {provider} {queue.name}_block_;")
    for index, route in enumerate(program.routes):
        payload = _cpp_type(
            next(
                queue.payload
                for queue in program.queues
                if queue.name == route.input_name
            )
        )
        lines.append(
            f"  gfsim::QueueRoute<{payload}, {len(route.outputs)}, "
            f"route_{index}_policy> route_{index}_block_;"
        )
    for index, fork in enumerate(program.forks):
        payload = _cpp_type(queues_by_name[fork.input_name].payload)
        lines.append(
            f"  gfsim::QueueFork<{payload}, {len(fork.outputs)}> fork_{index}_block_;"
        )
    for index, merge in enumerate(program.merges):
        payload = _cpp_type(
            next(
                queue.payload for queue in program.queues if queue.name == merge.output
            )
        )
        lines.append(
            f"  gfsim::QueueMerge<{payload}, {len(merge.inputs)}> merge_{index}_block_;"
        )
    for index, barrier in enumerate(program.barriers):
        types = ", ".join(
            _cpp_type(queues_by_name[name].payload) for name in barrier.inputs
        )
        lines.append(
            f"  gfsim::QueueBarrier<std::tuple<{types}>> barrier_{index}_block_;"
        )
    for index, feedback in enumerate(program.feedbacks):
        payload = _cpp_type(
            next(
                queue.payload
                for queue in program.queues
                if queue.name == feedback.output_name
            )
        )
        lines.append(
            f"  gfsim::QueueFeedback<{payload}, feedback_{index}_update_policy, "
            f"feedback_{index}_condition_policy> feedback_{index}_block_;"
        )
    for index, sink in enumerate(program.sinks):
        payload = _cpp_type(
            next(q.payload for q in program.queues if q.name == sink.queue)
        )
        lines.append(f"  gfsim::QueueSink<{payload}> sink_{index}_;")
    lines.extend(("};", "", "} // namespace ac_generated", ""))
    return "\n".join(lines)


def _emit_payload(payload: Payload) -> list[str]:
    lines = [f"struct {payload.name} {{"]
    for name, typ in payload.field_descriptors:
        lines.append(f"  {_cpp_type(typ)} {name}{{}};")
    lines.extend(("};", ""))
    return lines


def lower_queue_source_to_cpp(text: str, system: str) -> str:
    return lower_queue_program_to_cpp(parse_queue_program(text, system))
