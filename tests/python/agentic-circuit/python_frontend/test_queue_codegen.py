from __future__ import annotations

import ast
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[4]

SOURCE = """
from agentic_circuit import sink, source, struct, system

@struct
class Item:
    value: int
    remaining: int

@system
def pipeline() -> None:
    input_queue = source(Item, depth=4, latency=1)
    updated = input_queue.apply(
        lambda item: item.with_fields(
            value=item.value + 1,
            remaining=item.remaining - 1,
        ),
        depth=8,
        latency=2,
    )
    sink(updated)
"""

ROUTE_MERGE_SOURCE = """
from agentic_circuit import sink, source, struct, system

@struct
class Item:
    value: int
    route: int

@system
def pipeline() -> None:
    input_queue = source(Item, depth=2)
    left, right = input_queue.route(
        outputs=2,
        key=lambda item: item.route,
        depth=1,
        latency=1,
    )
    merged = left.merge(right, policy="round_robin", depth=2, latency=1)
    sink(merged)
"""

FEEDBACK_SOURCE = """
from agentic_circuit import sink, source, struct, system

@struct
class Item:
    value: int
    remaining: int

@system
def pipeline() -> None:
    current = source(Item)
    while current.remaining > 0:
        current = current.apply(
            lambda item: item.with_fields(
                value=item.value + 1,
                remaining=item.remaining - 1,
            )
        )
    sink(current)
"""

BROADCAST_SOURCE = """
from agentic_circuit import sink, source, system

@system
def pipeline() -> None:
    input_queue = source(int)
    left = input_queue.apply(lambda item: item + 1)
    right = input_queue.apply(lambda item: item + 2)
    sink(left)
    sink(right)
"""

ARRAY_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    grid = ac.array(
        2,
        lambda row: ac.array(
            2,
            lambda column: ac.source(int, depth=row + column + 1),
        ),
    )
    ac.sink(grid[1][0])
"""

SCOPE_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    input_queue = ac.source(int)
    with ac.scope("normalize"):
        local = input_queue.apply(lambda item: item + 1)
        exported = local.apply(lambda item: item * 2)
    ac.sink(exported)
"""

SLOT_TABLE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    valid: bool
    age: ac.u8

@ac.struct
class Request:
    age: ac.u8

@ac.system
def pipeline() -> None:
    requests = ac.source(Request)
    pending = ac.slot(requests)
    issue = ac.table[4, Entry](init=0)
    ready = issue.match(lambda entry: entry.valid)
    grant = issue.choose(
        ready, count=1, policy="min", key=lambda entry: entry.age
    )
    issue.view(grant.index).patch(enable=grant.valid, valid=False)
    pending.release(when=pending.valid and grant.valid)
    snapshots = issue.view(grant.index).read(when=grant.valid)
    ac.sink(snapshots)
"""


BIT_WIDTH_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Bits:
    left: ac.u3
    right: ac.u3
    result: ac.u3
    priority_index: ac.u2
    priority_valid: ac.u1
    count: ac.u2
    leading: ac.u2
    trailing: ac.u2

@ac.system
def pipeline() -> None:
    incoming = ac.source(Bits)
    outgoing = incoming.apply(
        lambda item: item.with_fields(
            result=((item.left & item.right) ^ (~item.left)) << 1,
            priority_index=ac.priority_encode(item.left).index,
            priority_valid=ac.priority_encode(item.left).valid,
            count=ac.popcount(item.left),
            leading=ac.count_leading_zeros(item.left),
            trailing=ac.count_trailing_zeros(item.left),
        )
    )
    ac.sink(outgoing)
"""

MASKED_MATCH_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Item:
    opcode: ac.bits[4]
    matched: bool

@ac.system
def pipeline() -> None:
    incoming = ac.source(Item)
    decoded = incoming.apply(
        lambda item: item.with_fields(
            matched=ac.matches(item.opcode, "10x1"),
        )
    )
    ac.sink(decoded)
"""


class QueueCodegenTest(unittest.TestCase):
    def test_rule_program_cannot_bypass_native_mlir_lowering(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_program_to_cpp
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            parse_queue_program,
        )

        source = ROOT / "examples/agentic-circuit/state/rob.py"
        program = parse_queue_program(source.read_text(encoding="utf-8"), "rob")
        with self.assertRaisesRegex(QueueFrontendError, "native MLIR"):
            lower_queue_program_to_cpp(program)

    def test_slot_release_uses_epoch_scoped_shared_table_caches(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(SLOT_TABLE_SOURCE, "pipeline")
        self.assertIn("struct slot_0_release_policy", generated)
        self.assertIn("table_match_0_cache *table_match_0{};", generated)
        self.assertIn("table_selection_0_cache *table_selection_0{};", generated)
        self.assertIn("bool operator()(gfsim::Epoch epoch) const", generated)
        self.assertIn("table_selection_0->get(epoch).valid", generated)

        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "slot_table.cpp"
            output.write_text(generated, encoding="utf-8")
            compiled = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    "-fsyntax-only",
                    str(output),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, compiled.returncode, compiled.stderr)

    def test_slot_release_expression_fails_closed_without_shared_refs(self) -> None:
        from agentic_circuit._queue_codegen import _CppExpression
        from agentic_circuit._queue_frontend import QueueFrontendError

        candidate = SimpleNamespace(table="issue")
        with self.assertRaisesRegex(QueueFrontendError, "reference is missing"):
            _CppExpression(
                "",
                candidates={"ready": candidate},
                require_shared_refs=True,
            ).emit(ast.Name(id="ready", ctx=ast.Load()))
        with self.assertRaisesRegex(QueueFrontendError, "reference is missing"):
            _CppExpression(
                "",
                candidates={"ready": candidate},
                selections={"grant": SimpleNamespace(candidates="ready")},
                require_shared_refs=True,
            ).emit(
                ast.Attribute(
                    value=ast.Name(id="grant", ctx=ast.Load()),
                    attr="valid",
                    ctx=ast.Load(),
                )
            )

    def test_serial_python_generates_typed_queue_wired_cpp(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(SOURCE, "pipeline")
        self.assertIn("struct Item", generated)
        self.assertIn("gfsim::SimQueue<Item> input_queue_;", generated)
        self.assertIn("gfsim::SimQueue<Item> updated_;", generated)
        self.assertIn("gfsim::QueueTransform<Item, Item, updated_policy>", generated)
        self.assertIn("result.value = (item.value + 1);", generated)
        self.assertIn("gfsim::QueueSink<Item> sink_0_;", generated)
        self.assertEqual(generated, lower_queue_source_to_cpp(SOURCE, "pipeline"))

    def test_bit_width_uses_exact_gfsim_storage(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(BIT_WIDTH_SOURCE, "pipeline")
        self.assertIn('#include "gfsim/bits.h"', generated)
        self.assertIn('#include "gfsim/count_zeros.h"', generated)
        self.assertIn('#include "gfsim/priority_encode.h"', generated)
        self.assertIn('#include "gfsim/popcount.h"', generated)
        self.assertIn("gfsim::UInt<3> left{};", generated)
        self.assertIn("gfsim::UInt<3> right{};", generated)
        self.assertIn("gfsim::UInt<3> result{};", generated)
        self.assertIn("((item.left & item.right) ^ (~item.left))", generated)
        self.assertIn("gfsim::priorityEncode(item.left, true).index", generated)
        self.assertIn("gfsim::priorityEncode(item.left, true).valid", generated)
        self.assertIn("gfsim::populationCount(item.left)", generated)
        self.assertIn("gfsim::countLeadingZeros(item.left)", generated)
        self.assertIn("gfsim::countTrailingZeros(item.left)", generated)

        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "bits.cpp"
            output.write_text(generated, encoding="utf-8")
            compiled = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    "-fsyntax-only",
                    str(output),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, compiled.returncode, compiled.stderr)

    def test_masked_match_generates_and_executes_basic_pattern(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(MASKED_MATCH_SOURCE, "pipeline")
        self.assertIn("result.matched = ((item.opcode & 13) == 9);", generated)

        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "masked_match.cpp"
            harness = root / "masked_match_harness.cpp"
            executable = root / "masked_match"
            model.write_text(generated, encoding="utf-8")
            harness.write_text(
                f"""#include "{model.name}"
#include <cstdint>

bool run(std::uint64_t opcode, bool expected) {{
  ac_generated::Pipeline model;
  if (!model.incoming().proposePush(ac_generated::Item{{opcode, false}}))
    return false;
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 5; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  return values.size() == 1 && static_cast<bool>(values[0].matched) == expected;
}}

int main() {{
  return run(0b1001, true) && run(0b0001, false) ? 0 : 1;
}}
""",
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    "-o",
                    str(executable),
                ),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, linked.returncode, linked.stderr)
            executed = subprocess.run(
                (str(executable),),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)

    def test_masked_match_direct_codegen_checks_real_operand_width(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp
        from agentic_circuit._queue_frontend import QueueFrontendError

        for pattern, actual_width in (("1x", 2), ("10xx11", 6)):
            with self.subTest(pattern=pattern):
                source = MASKED_MATCH_SOURCE.replace('"10x1"', repr(pattern))
                with self.assertRaisesRegex(
                    QueueFrontendError,
                    rf"width {actual_width}, expected 4",
                ):
                    lower_queue_source_to_cpp(source, "pipeline")

        non_bits = MASKED_MATCH_SOURCE.replace(
            'ac.matches(item.opcode, "10x1")',
            'ac.matches(item.matched, "x")',
        )
        with self.assertRaisesRegex(QueueFrontendError, "requires a bits value"):
            lower_queue_source_to_cpp(non_bits, "pipeline")

    def test_route_and_merge_generate_standard_typed_blocks(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(ROUTE_MERGE_SOURCE, "pipeline")
        self.assertIn("gfsim::QueueRoute<Item, 2, route_0_policy>", generated)
        self.assertIn("return static_cast<size_t>(item.route);", generated)
        self.assertIn("gfsim::QueueMerge<Item, 2>", generated)
        self.assertIn("gfsim::QueueMergePolicy::RoundRobin", generated)
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "route_merge.cpp"
            output.write_text(generated, encoding="utf-8")
            compiled = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    "-fsyntax-only",
                    str(output),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, compiled.returncode, compiled.stderr)
            harness = Path(directory) / "route_merge_harness.cpp"
            executable = Path(directory) / "route_merge"
            harness.write_text(
                f"""#include "{output.name}"
#include <cstddef>

int main() {{
  ac_generated::Pipeline model;
  if (!model.input_queue().proposePush(ac_generated::Item{{7, 1}}))
    return 1;
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 5; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  return values.size() == 1 && values[0].value == 7 && values[0].route == 1
             ? 0
             : 2;
}}
""",
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    "-o",
                    str(executable),
                ),
                cwd=Path(directory),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, linked.returncode, linked.stderr)
            executed = subprocess.run(
                (str(executable),),
                cwd=Path(directory),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)

    def test_explicit_tool_writes_cpp_accepted_by_cxx20_compiler(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "pipeline.py"
            output = root / "pipeline.cpp"
            source.write_text(SOURCE, encoding="utf-8")
            environment = os.environ.copy()
            environment["PYTHONPATH"] = os.pathsep.join(
                (
                    str(ROOT / "python/semantic-core/src"),
                    str(ROOT / "python/agentic-circuit/src"),
                )
            )
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools" / "ac-queue-cxxgen.py"),
                    str(source),
                    "--system",
                    "pipeline",
                    "-o",
                    str(output),
                ),
                cwd=ROOT,
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            compiled = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    "-fsyntax-only",
                    str(output),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, compiled.returncode, compiled.stderr)

            harness = root / "harness.cpp"
            executable = root / "pipeline"
            harness.write_text(
                f"""#include "{output.name}"
#include <cstddef>

int main() {{
  ac_generated::Pipeline model;
  if (!model.input_queue().proposePush(ac_generated::Item{{41, 3}}))
    return 1;
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 5; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  return values.size() == 1 && values[0].value == 42 &&
                 values[0].remaining == 2
             ? 0
             : 2;
}}
""",
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    "-o",
                    str(executable),
                ),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, linked.returncode, linked.stderr)
            executed = subprocess.run(
                (str(executable),),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)

    def test_serial_while_generates_parent_owned_feedback_queue(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(FEEDBACK_SOURCE, "pipeline")
        self.assertIn("gfsim::SimQueue<gfsim::FeedbackToken<Item>>", generated)
        self.assertIn("gfsim::QueueFeedback<Item", generated)
        self.assertIn("return (item.remaining > 0);", generated)
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "feedback.cpp"
            harness = root / "harness.cpp"
            executable = root / "feedback"
            model.write_text(generated, encoding="utf-8")
            harness.write_text(
                f"""#include "{model.name}"
#include <cstddef>

int main() {{
  ac_generated::Pipeline model;
  if (!model.current().proposePush(ac_generated::Item{{10, 3}}))
    return 1;
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 8; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  return values.size() == 1 && values[0].value == 13 &&
                 values[0].remaining == 0
             ? 0
             : 2;
}}
""",
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    "-o",
                    str(executable),
                ),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, linked.returncode, linked.stderr)
            executed = subprocess.run(
                (str(executable),),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)

    def test_multiple_consumers_generate_strict_broadcast(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(BROADCAST_SOURCE, "pipeline")
        self.assertIn("gfsim::QueueBroadcast<gfsim::UInt<64>, 2>", generated)
        self.assertIn("input_queue__fanout0_", generated)
        self.assertIn("input_queue__fanout1_", generated)
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "broadcast.cpp"
            harness = root / "harness.cpp"
            executable = root / "broadcast"
            model.write_text(generated, encoding="utf-8")
            harness.write_text(
                f"""#include "{model.name}"
#include <cstddef>

int main() {{
  ac_generated::Pipeline model;
  if (!model.input_queue().proposePush(10))
    return 1;
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  return model.sink_0_values().size() == 1 &&
                 model.sink_0_values()[0] == 11 &&
                 model.sink_1_values().size() == 1 &&
                 model.sink_1_values()[0] == 12
             ? 0
             : 2;
}}
""",
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    "-o",
                    str(executable),
                ),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, linked.returncode, linked.stderr)
            executed = subprocess.run(
                (str(executable),),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)

    def test_nested_array_generates_nested_simqueue_template(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(ARRAY_SOURCE, "pipeline")
        self.assertIn(
            "std::array<std::array<gfsim::SimQueue<gfsim::UInt<64>>, 2>, 2> grid_;",
            generated,
        )
        self.assertIn('"grid__1__0"', generated)
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "array.cpp"
            harness = root / "harness.cpp"
            executable = root / "array"
            model.write_text(generated, encoding="utf-8")
            harness.write_text(
                f"""#include "{model.name}"
#include <cstddef>

int main() {{
  ac_generated::Pipeline model;
  if (!model.grid__1__0().proposePush(17))
    return 1;
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 3; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  return model.sink_0_values().size() == 1 &&
                 model.sink_0_values()[0] == 17
             ? 0
             : 2;
}}
""",
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    "-o",
                    str(executable),
                ),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, linked.returncode, linked.stderr)
            executed = subprocess.run(
                (str(executable),),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)

    def test_scope_generates_hierarchy_with_lca_queue_ownership(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_source_to_cpp

        generated = lower_queue_source_to_cpp(SCOPE_SOURCE, "pipeline")
        self.assertIn("gfsim::Module scope_normalize_;", generated)
        self.assertIn("scope_normalize_.attachChild(local_);", generated)
        self.assertIn("attachChild(exported_);", generated)
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "scope.cpp"
            harness = root / "harness.cpp"
            executable = root / "scope"
            model.write_text(generated, encoding="utf-8")
            harness.write_text(
                f"""#include "{model.name}"
#include <cstddef>

int main() {{
  ac_generated::Pipeline model;
  auto *normalize = model.findChild("normalize");
  if (normalize == nullptr || normalize->asModule() == nullptr ||
      normalize->asModule()->findChild("local") == nullptr ||
      model.findChild("exported") == nullptr)
    return 1;
  if (!model.input_queue().proposePush(10))
    return 2;
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  return model.sink_0_values().size() == 1 &&
                 model.sink_0_values()[0] == 22
             ? 0
             : 3;
}}
""",
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    "-o",
                    str(executable),
                ),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, linked.returncode, linked.stderr)
            executed = subprocess.run(
                (str(executable),),
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)


if __name__ == "__main__":
    unittest.main()
