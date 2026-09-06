from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / "examples/agentic-circuit" / "pipelines" / "davincioo_queue_model.py"
CONDITIONAL_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_conditional_pipeline.py"
)
REORDER_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_reorder_pipeline.py"
)
RULE_ROB_SOURCE = ROOT / "examples/agentic-circuit" / "state" / "rob.py"
STATEFUL_RULE_SOURCE = ROOT / "examples/agentic-circuit" / "state" / "table_rule.py"
MULTI_INPUT_RULE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_multi_input_rule_pipeline.py"
)
INFERRED_BOUNDARY_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "inferred_boundary_pipeline.py"
)
INFERRED_MODULE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "inferred_module_pipeline.py"
)
INFERRED_NESTED_MODULE_SOURCE = (
    ROOT
    / "examples/agentic-circuit"
    / "pipelines"
    / "inferred_nested_module_pipeline.py"
)
BITFIELD_DECODE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "bitfield_decode_pipeline.py"
)
NESTED_PAYLOAD_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "nested_payload_pipeline.py"
)
ENUM_PAYLOAD_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "enum_payload_pipeline.py"
)
AGGREGATE_PAYLOAD_SOURCE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "aggregate_payload_pipeline.py"
)
RECURSIVE_AGGREGATE_PAYLOAD_SOURCE = (
    ROOT
    / "examples/agentic-circuit"
    / "pipelines"
    / "recursive_aggregate_payload_pipeline.py"
)
INFERRED_STATEFUL_MODULE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "inferred_stateful_module.py"
)
INFERRED_MULTI_STATE_MODULE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "inferred_multi_state_module.py"
)
STATEFUL_MULTI_INPUT_RULE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "table_multi_input_rule.py"
)
VARIABLE_ACCUMULATOR_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "variable_accumulator.py"
)
BRANCH_LOCAL_STATE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "branch_local_state.py"
)
BRANCH_JOIN_STATE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "branch_join_state.py"
)
INDEXED_BRANCH_JOIN_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "indexed_branch_join.py"
)
OPTIONAL_OUTPUT_STATE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "optional_output_state.py"
)
INDEXED_VARIABLE_ARRAY_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "indexed_variable_array.py"
)
SHARED_INDEXED_RULES_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "shared_indexed_rules.py"
)
CONSUME_ONLY_COMPLETION_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "consume_only_completion.py"
)
STATE_DRIVEN_RETIRE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "state_driven_retire.py"
)
MULTI_STATE_ALLOCATE_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "multi_state_allocate.py"
)
CIRCULAR_ROB_SOURCE = ROOT / "examples/agentic-circuit" / "state" / "circular_rob.py"
REUSABLE_CIRCULAR_ROB_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "reusable_circular_rob.py"
)
REUSABLE_OLDEST_READY_ISQ_SOURCE = (
    ROOT / "examples/agentic-circuit" / "state" / "reusable_oldest_ready_isq.py"
)
DAVINCIOO_TRACE = (
    ROOT
    / "references/davincioo-gfsim/upstream/tests/fixtures/traces"
    / "examples_intermediate_softmax.pto.trace"
)
DAVINCIOO_PROJECTION = (
    ROOT / "tests/goldens/agentic-circuit/davincioo/softmax-projection.json"
)


class QueueCodegenTest(unittest.TestCase):
    def test_recursive_nominal_aggregate_payload_runs_packed_in_gfsim(
        self,
    ) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native recursive-aggregate tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "recursive_aggregate_payload.cpp"
            acir = root / "recursive_aggregate_payload.frozen.mlir"
            plan = root / "recursive_aggregate_payload.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(RECURSIVE_AGGREGATE_PAYLOAD_SOURCE),
                    "--system",
                    "recursive_aggregate_payload_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": os.pathsep.join(
                        (
                            str(ROOT / "python/semantic-core/src"),
                            str(ROOT / "python/agentic-circuit/src"),
                        )
                    ),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            plan_value = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(
                [
                    {
                        "elements": ["!ac.enum<@types::@Mode>", "i3"],
                        "kind": "tuple",
                        "length": 2,
                        "type": "tuple<!ac.enum<@types::@Mode>, i3>",
                        "width": 4,
                    },
                    {
                        "elements": ["!ac.struct<@types::@Header>", "i3"],
                        "kind": "tuple",
                        "length": 2,
                        "type": "tuple<!ac.struct<@types::@Header>, i3>",
                        "width": 6,
                    },
                    {
                        "elements": ["!ac.enum<@types::@Mode>"],
                        "kind": "array",
                        "length": 2,
                        "type": "!ac.value_array<2 x !ac.enum<@types::@Mode>>",
                        "width": 2,
                    },
                ],
                plan_value["aggregates"],
            )
            source = model.read_text(encoding="utf-8")
            self.assertIn("gfsim::UInt<1>{static_cast<std::uint64_t>(v0)}", source)
            self.assertIn("Header unpacked{}", source)
            self.assertIn("static_cast<Mode>", source)

            harness = root / "harness.cpp"
            executable = root / "recursive_aggregate_payload"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  constexpr std::uint64_t input_packed = 2962u;
  constexpr std::uint64_t expected_packed = 7125u;
  ac_generated::Packet input;
  input.tagged = gfsim::UInt<4>{{(input_packed >> 9) & 0xfu}};
  input.nested = gfsim::UInt<6>{{(input_packed >> 3) & 0x3fu}};
  input.modes = gfsim::UInt<2>{{(input_packed >> 1) & 0x3u}};
  input.flag = gfsim::UInt<1>{{input_packed & 0x1u}};
  ac_generated::RecursiveAggregatePayloadPipeline model;
  if (!model.incoming().proposePush(input))
    return 1;
  model.incoming().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  if (values.size() != 1)
    return 2;
  const auto &value = values.front();
  const std::uint64_t output_packed =
      (value.tagged.value() << 9) | (value.nested.value() << 3) |
      (value.modes.value() << 1) | value.flag.value();
  return output_packed == expected_packed ? 0 : 3;
}}
''',
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

    def test_tuple_and_value_array_payload_runs_packed_in_gfsim(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native aggregate-payload tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "aggregate_payload.cpp"
            acir = root / "aggregate_payload.frozen.mlir"
            plan = root / "aggregate_payload.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(AGGREGATE_PAYLOAD_SOURCE),
                    "--system",
                    "aggregate_payload_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": os.pathsep.join(
                        (
                            str(ROOT / "python/semantic-core/src"),
                            str(ROOT / "python/agentic-circuit/src"),
                        )
                    ),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            plan_value = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(
                [
                    {
                        "elements": ["i3", "i5"],
                        "kind": "tuple",
                        "length": 2,
                        "type": "tuple<i3, i5>",
                        "width": 8,
                    },
                    {
                        "elements": ["i4"],
                        "kind": "array",
                        "length": 4,
                        "type": "!ac.value_array<4 x i4>",
                        "width": 16,
                    },
                ],
                plan_value["aggregates"],
            )
            source = model.read_text(encoding="utf-8")
            self.assertIn("gfsim::UInt<8> pair", source)
            self.assertIn("gfsim::UInt<16> lanes", source)
            self.assertIn("gfsim::bitExtract<3>(v0, 5)", source)
            self.assertIn("gfsim::bitExtract<4>(v9, 8)", source)

            harness = root / "harness.cpp"
            executable = root / "aggregate_payload"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  constexpr std::uint64_t input_pair = (5u << 5) | 17u;
  constexpr std::uint64_t input_lanes = 0x1234u;
  constexpr std::uint64_t expected_pair = (6u << 5) | 18u;
  constexpr std::uint64_t expected_lanes = 0x2341u;
  ac_generated::AggregatePacket input;
  input.pair = gfsim::UInt<8>{{input_pair}};
  input.lanes = gfsim::UInt<16>{{input_lanes}};
  input.selected = gfsim::UInt<4>{{0}};
  ac_generated::AggregatePayloadPipeline model;
  if (!model.incoming().proposePush(input))
    return 1;
  model.incoming().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  if (values.size() != 1)
    return 2;
  const auto &value = values.front();
  return value.pair.value() == expected_pair &&
                 value.lanes.value() == expected_lanes &&
                 value.selected.value() == 3u
             ? 0
             : 3;
}}
''',
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

    def test_nominal_enum_nested_payload_runs_in_gfsim(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native enum-payload tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "enum_payload.cpp"
            acir = root / "enum_payload.frozen.mlir"
            plan = root / "enum_payload.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(ENUM_PAYLOAD_SOURCE),
                    "--system",
                    "enum_payload_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": os.pathsep.join(
                        (
                            str(ROOT / "python/semantic-core/src"),
                            str(ROOT / "python/agentic-circuit/src"),
                        )
                    ),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            plan_value = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(
                [{"enumerants": ["IDLE", "RUN", "WAIT"], "name": "Mode", "width": 2}],
                plan_value["enums"],
            )
            source = model.read_text(encoding="utf-8")
            self.assertIn("enum class Mode : std::uint8_t", source)
            self.assertIn("Mode::RUN", source)
            self.assertIn("Mode::WAIT", source)

            harness = root / "harness.cpp"
            executable = root / "enum_payload"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::Packet input;
  input.header.opcode = 42;
  input.header.mode = ac_generated::Mode::WAIT;
  input.payload = 0x12345;
  ac_generated::EnumPayloadPipeline model;
  if (!model.incoming().proposePush(input))
    return 1;
  model.incoming().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  return values.size() == 1 &&
                 values[0].header.mode == ac_generated::Mode::RUN &&
                 values[0].matched == 1 && values[0].payload == 0x12345
             ? 0
             : 2;
}}
''',
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

    def test_nested_payload_runs_with_dependency_ordered_cpp_types(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native nested-payload tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "nested_payload.cpp"
            acir = root / "nested_payload.frozen.mlir"
            plan = root / "nested_payload.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(NESTED_PAYLOAD_SOURCE),
                    "--system",
                    "nested_payload_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": os.pathsep.join(
                        (
                            str(ROOT / "python/semantic-core/src"),
                            str(ROOT / "python/agentic-circuit/src"),
                        )
                    ),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            source = model.read_text(encoding="utf-8")
            self.assertLess(
                source.index("struct Header"), source.index("struct Packet")
            )
            self.assertIn("auto v0 = item.header", source)
            self.assertIn("v4.mode = v3", source)

            harness = root / "harness.cpp"
            executable = root / "nested_payload"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::Packet input;
  input.header.opcode = 42;
  input.header.mode = 7;
  input.payload = 0x12345;
  ac_generated::NestedPayloadPipeline model;
  if (!model.incoming().proposePush(input))
    return 1;
  model.incoming().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  return values.size() == 1 && values[0].header.opcode == 42 &&
                 values[0].header.mode == 0 && values[0].payload == 0x12345
             ? 0
             : 2;
}}
''',
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

    def test_bitfield_decode_runs_through_frozen_queuegraph_and_gfsim(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native bitfield tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "bitfield_decode.cpp"
            acir = root / "bitfield_decode.frozen.mlir"
            plan = root / "bitfield_decode.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(BITFIELD_DECODE_SOURCE),
                    "--system",
                    "bitfield_decode_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": os.pathsep.join(
                        (
                            str(ROOT / "python/semantic-core/src"),
                            str(ROOT / "python/agentic-circuit/src"),
                        )
                    ),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn("ac.bitfield @INSTRUCTION width 32", frozen)
            self.assertNotIn("ac.rule ", frozen)
            expression_kinds = {
                expression["kind"]
                for block in json.loads(plan.read_text(encoding="utf-8"))["blocks"]
                for expression in block["expressions"]
            }
            self.assertTrue(
                {"bit_extract", "bit_concat", "bit_insert"} <= expression_kinds
            )

            harness = root / "harness.cpp"
            executable = root / "bitfield_decode"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  constexpr std::uint32_t word = 0xd5a12345u;
  constexpr std::uint32_t mode = 5u;
  constexpr std::uint32_t rd = 17u;
  ac_generated::DecodeItem input;
  input.word = word;
  input.mode = mode;
  input.rd = rd;
  ac_generated::BitfieldDecodePipeline model;
  if (!model.incoming().proposePush(input))
    return 1;
  model.incoming().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  if (values.size() != 1)
    return 2;
  const auto &value = values.front();
  const std::uint32_t opcode = (word >> 26) & 0x3fu;
  const std::uint32_t old_rd = (word >> 21) & 0x1fu;
  const std::uint32_t expected =
      (word & ~((0x7u << 1) | (0x1fu << 21))) |
      (mode << 1) | (rd << 21);
  return value.opcode.value() == opcode &&
                 value.opcode_rd.value() == ((opcode << 5) | old_rd) &&
                 value.immediate.value() == ((word >> 4) & 0x1ffffu) &&
                 value.updated.value() == expected
             ? 0
             : 3;
}}
''',
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

    def test_inferred_system_boundaries_generate_and_run(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native inferred-boundary tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "inferred_boundary.cpp"
            acir = root / "inferred_boundary.frozen.mlir"
            plan = root / "inferred_boundary.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(INFERRED_BOUNDARY_SOURCE),
                    "--system",
                    "inferred_boundary_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": os.pathsep.join(
                        (
                            str(ROOT / "python/semantic-core/src"),
                            str(ROOT / "python/agentic-circuit/src"),
                        )
                    ),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn("ac.source", frozen)
            self.assertIn("ac.sink", frozen)
            self.assertNotIn("ac.rule ", frozen)

            harness = root / "harness.cpp"
            executable = root / "inferred_boundary"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::InferredBoundaryPipeline model;
  if (!model.value().proposePush(gfsim::UInt<8>{{41}}))
    return 1;
  model.value().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  return values.size() == 1 && values[0] == 42 ? 0 : 2;
}}
''',
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

    def test_generic_ac_var_state_runs_through_storage_selection(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native generic ac.var tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "variable_accumulator.cpp"
            acir = root / "variable_accumulator.frozen.mlir"
            plan = root / "variable_accumulator.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(VARIABLE_ACCUMULATOR_SOURCE),
                    "--system",
                    "variable_accumulator",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": os.pathsep.join(
                        (
                            str(ROOT / "python/semantic-core/src"),
                            str(ROOT / "python/agentic-circuit/src"),
                        )
                    ),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertNotIn("ac.var.decl", frozen)
            self.assertNotIn("ac.var.read", frozen)
            self.assertNotIn("ac.var.assign", frozen)
            self.assertIn("kind = #ac<rule_check_kind input_available>", frozen)
            self.assertIn("kind = #ac<rule_check_kind output_capacity>", frozen)
            self.assertIn("ac.transaction_resources", frozen)

            harness = root / "harness.cpp"
            executable = root / "variable_accumulator"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::VariableAccumulator model;
  auto rows = model.dispatch_rows();
  for (unsigned tick = 0; tick < 8; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    if (tick == 0 && !model.incoming().proposePush(gfsim::UInt<8>{{1}}))
      return 1;
    if (tick == 2 && !model.incoming().proposePush(gfsim::UInt<8>{{2}}))
      return 2;
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  if (values.size() != 2 || values[0] != 1 || values[1] != 3)
    return 3;
  return 0;
}}
''',
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

    def test_branch_local_state_updates_commit_exactly_one_owner(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native branch-local state tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "branch_local_state.cpp"
            acir = root / "branch_local_state.frozen.mlir"
            plan = root / "branch_local_state.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(BRANCH_LOCAL_STATE_SOURCE),
                    "--system",
                    "branch_local_state",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            source = BRANCH_LOCAL_STATE_SOURCE.read_text(encoding="utf-8")
            for forbidden in (
                "source(",
                "sink(",
                "ac.table",
                "ac.queue",
                ".pop(",
                ".push(",
                ".empty(",
                ".full(",
                "atomic",
            ):
                self.assertNotIn(forbidden, source)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            firing = next(
                block for block in parsed_plan["blocks"] if block["kind"] == "firing"
            )
            self.assertEqual(
                {"left", "right"},
                {write["table"] for write in firing["state_writes"]},
            )
            self.assertEqual(
                2, len({write["present"] for write in firing["state_writes"]})
            )

            harness = root / "harness.cpp"
            executable = root / "branch_local_state"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::BranchLocalState model;
  auto rows = model.dispatch_rows();
  auto run = [&](unsigned first, unsigned last) {{
    for (unsigned tick = first; tick < last; ++tick) {{
      const gfsim::Epoch epoch{{tick, 0}};
      for (auto &row : rows)
        row.work(row.object, epoch);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
    }}
  }};
  if (!model.command().proposePush(
          ac_generated::Command{{gfsim::UInt<1>{{0}}, gfsim::UInt<8>{{7}}}}))
    return 1;
  model.command().doXfer({{0, 0}});
  run(1, 4);
  if (model.table_left().at(0) != 7 || model.table_right().at(0) != 0)
    return 2;
  if (!model.command().proposePush(
          ac_generated::Command{{gfsim::UInt<1>{{1}}, gfsim::UInt<8>{{9}}}}))
    return 3;
  model.command().doXfer({{4, 0}});
  run(5, 8);
  if (model.table_left().at(0) != 7 || model.table_right().at(0) != 9)
    return 4;
  return 0;
}}
''',
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

    def test_same_owner_branch_join_emits_one_state_proposal(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native same-owner branch tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "branch_join_state.cpp"
            acir = root / "branch_join_state.frozen.mlir"
            plan = root / "branch_join_state.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(BRANCH_JOIN_STATE_SOURCE),
                    "--system",
                    "branch_join_state",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn("ac.var.select", frozen)
            self.assertEqual(1, frozen.count("ac.table.propose @total"))
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            firing = next(
                block for block in parsed_plan["blocks"] if block["kind"] == "firing"
            )
            self.assertEqual(1, len(firing["state_writes"]))
            self.assertEqual(firing["guard"], firing["state_writes"][0]["present"])
            self.assertEqual(
                1,
                sum(
                    expression["kind"] == "value_select"
                    for expression in firing["expressions"]
                ),
            )

            harness = root / "harness.cpp"
            executable = root / "branch_join_state"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::BranchJoinState model;
  auto rows = model.dispatch_rows();
  auto run = [&](unsigned first, unsigned last) {{
    for (unsigned tick = first; tick < last; ++tick) {{
      const gfsim::Epoch epoch{{tick, 0}};
      for (auto &row : rows)
        row.work(row.object, epoch);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
    }}
  }};
  if (!model.command().proposePush(
          ac_generated::Command{{gfsim::UInt<1>{{0}}, gfsim::UInt<8>{{7}}}}))
    return 1;
  model.command().doXfer({{0, 0}});
  run(1, 4);
  if (model.table_total().at(0) != 8)
    return 2;
  if (!model.command().proposePush(
          ac_generated::Command{{gfsim::UInt<1>{{1}}, gfsim::UInt<8>{{9}}}}))
    return 3;
  model.command().doXfer({{4, 0}});
  run(5, 8);
  return model.table_total().at(0) == 9 ? 0 : 4;
}}
''',
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

    def test_indexed_branch_join_selects_one_index_and_value(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native indexed branch-join tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "indexed_branch_join.cpp"
            acir = root / "indexed_branch_join.frozen.mlir"
            plan = root / "indexed_branch_join.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(INDEXED_BRANCH_JOIN_SOURCE),
                    "--system",
                    "indexed_branch_join",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            firing = next(
                block for block in parsed_plan["blocks"] if block["kind"] == "firing"
            )
            self.assertEqual(1, len(firing["state_writes"]))
            self.assertEqual(
                2,
                sum(
                    expression["kind"] == "value_select"
                    for expression in firing["expressions"]
                ),
            )

            harness = root / "harness.cpp"
            executable = root / "indexed_branch_join"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::IndexedBranchJoin model;
  auto rows = model.dispatch_rows();
  auto run = [&](unsigned first, unsigned last) {{
    for (unsigned tick = first; tick < last; ++tick) {{
      const gfsim::Epoch epoch{{tick, 0}};
      for (auto &row : rows)
        row.work(row.object, epoch);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
      for (auto &row : rows)
        row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
    }}
  }};
  if (!model.command().proposePush(ac_generated::Command{{
          gfsim::UInt<1>{{0}}, gfsim::UInt<2>{{1}},
          gfsim::UInt<2>{{3}}, gfsim::UInt<8>{{7}}}}))
    return 1;
  model.command().doXfer({{0, 0}});
  run(1, 4);
  if (model.table_entries().at(1) != 8 || model.table_entries().at(3) != 0)
    return 2;
  if (!model.command().proposePush(ac_generated::Command{{
          gfsim::UInt<1>{{1}}, gfsim::UInt<2>{{0}},
          gfsim::UInt<2>{{3}}, gfsim::UInt<8>{{9}}}}))
    return 3;
  model.command().doXfer({{4, 0}});
  run(5, 8);
  return model.table_entries().at(1) == 8 &&
                 model.table_entries().at(3) == 9
             ? 0
             : 4;
}}
''',
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

    def test_optional_output_checks_backpressure_only_when_present(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native optional-output tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "optional_output_state.cpp"
            acir = root / "optional_output_state.frozen.mlir"
            plan = root / "optional_output_state.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(OPTIONAL_OUTPUT_STATE_SOURCE),
                    "--system",
                    "optional_output_state",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            firing = next(
                block for block in parsed_plan["blocks"] if block["kind"] == "firing"
            )
            self.assertNotEqual(
                firing["guard"], firing["output_presence"][0]["present"]
            )
            self.assertEqual(firing["guard"], firing["state_writes"][0]["present"])

            harness = root / "harness.cpp"
            executable = root / "optional_output_state"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  using Event = ac_generated::Event;
  ac_generated::OptionalOutputState model;
  auto rows = model.dispatch_rows();
  auto *filtered = static_cast<gfsim::SimQueue<Event> *>(rows[1].object);
  auto runWithoutSink = [&](unsigned first, unsigned last) {{
    for (unsigned tick = first; tick < last; ++tick) {{
      const gfsim::Epoch epoch{{tick, 0}};
      for (std::size_t index = 0; index < rows.size(); ++index)
        if (index != model.sink_0_id())
          rows[index].work(rows[index].object, epoch);
      for (std::size_t index = 0; index < rows.size(); ++index)
        if (index != model.sink_0_id())
          rows[index].xfer(rows[index].object, epoch,
                           gfsim::XferPhase::Arbitrate);
      for (std::size_t index = 0; index < rows.size(); ++index)
        if (index != model.sink_0_id())
          rows[index].xfer(rows[index].object, epoch,
                           gfsim::XferPhase::Probe);
      for (std::size_t index = 0; index < rows.size(); ++index)
        if (index != model.sink_0_id())
          rows[index].xfer(rows[index].object, epoch,
                           gfsim::XferPhase::Commit);
    }}
  }};
  if (!filtered->proposePush(Event{{gfsim::UInt<1>{{1}}, gfsim::UInt<8>{{99}}}}))
    return 1;
  filtered->doXfer({{0, 0}});
  if (!model.event().proposePush(
          Event{{gfsim::UInt<1>{{0}}, gfsim::UInt<8>{{7}}}}))
    return 2;
  model.event().doXfer({{0, 0}});
  runWithoutSink(1, 4);
  if (model.table_count().at(0) != 1 || !model.event().isEmpty() ||
      filtered->committedSize() != 1)
    return 3;
  if (!model.event().proposePush(
          Event{{gfsim::UInt<1>{{1}}, gfsim::UInt<8>{{9}}}}))
    return 4;
  model.event().doXfer({{4, 0}});
  runWithoutSink(5, 8);
  if (model.table_count().at(0) != 1 || model.event().isEmpty())
    return 5;
  if (!filtered->proposePop())
    return 6;
  filtered->doXfer({{8, 0}});
  runWithoutSink(9, 12);
  if (model.table_count().at(0) != 2 || !model.event().isEmpty() ||
      filtered->committedSize() != 1 || filtered->peek()->value != 9)
    return 7;
  return 0;
}}
''',
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

    def test_indexed_python_list_runs_through_storage_selection(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native indexed ac.var tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "indexed_variable_array.cpp"
            acir = root / "indexed_variable_array.frozen.mlir"
            plan = root / "indexed_variable_array.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(INDEXED_VARIABLE_ARRAY_SOURCE),
                    "--system",
                    "indexed_variable_array",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertNotIn("ac.var.decl", frozen)
            self.assertNotIn("ac.var.read_element", frozen)
            self.assertNotIn("ac.var.assign_element", frozen)
            self.assertIn("ac.table @entries", frozen)
            self.assertIn("entries 5", frozen)

            harness = root / "harness.cpp"
            executable = root / "indexed_variable_array"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::IndexedVariableArray model;
  auto rows = model.dispatch_rows();
  auto run = [&](unsigned tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }};
  if (!model.incoming().proposePush(
          ac_generated::Entry{{gfsim::UInt<2>{{1}}, gfsim::UInt<8>{{9}}}}))
    return 1;
  model.incoming().doXfer({{0, 0}});
  run(1);
  run(2);
  if (!model.incoming().proposePush(
          ac_generated::Entry{{gfsim::UInt<2>{{1}}, gfsim::UInt<8>{{11}}}}))
    return 2;
  model.incoming().doXfer({{3, 0}});
  run(4);
  run(5);
  run(6);
  const auto &values = model.sink_0_values();
  if (values.size() != 2)
    return 3;
  if (static_cast<unsigned long long>(values[0].value) != 0 ||
      static_cast<unsigned long long>(values[1].value) != 9)
    return 4;
  return 0;
}}
''',
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

    def test_shared_indexed_rules_use_mlir_priority_and_runtime_arbitration(
        self,
    ) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native shared-state rule tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "shared_indexed_rules.cpp"
            acir = root / "shared_indexed_rules.frozen.mlir"
            plan = root / "shared_indexed_rules.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(SHARED_INDEXED_RULES_SOURCE),
                    "--system",
                    "shared_indexed_rules",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertEqual(2, frozen.count(" = ac.firing "))
            self.assertIn("ac.rule_priority = 0 : i64", frozen)
            self.assertIn("ac.rule_priority = 1 : i64", frozen)
            self.assertEqual(2, frozen.count("ac.rule_footprints"))

            harness = root / "harness.cpp"
            executable = root / "shared_indexed_rules"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::SharedIndexedRules model;
  if (!model.first().proposePush(
          ac_generated::Entry{{gfsim::UInt<2>{{1}}, gfsim::UInt<8>{{9}}}}) ||
      !model.second().proposePush(
          ac_generated::Entry{{gfsim::UInt<2>{{1}}, gfsim::UInt<8>{{11}}}}))
    return 1;
  model.first().doXfer({{0, 0}});
  model.second().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick < 8; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    // Reverse Work deliberately; Arbitrate retains generated stable order.
    for (auto row = rows.rbegin(); row != rows.rend(); ++row)
      row->work(row->object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &first = model.sink_0_values();
  const auto &second = model.sink_1_values();
  if (first.size() != 1 || second.size() != 1)
    return 2;
  if (static_cast<unsigned long long>(first[0].value) != 0 ||
      static_cast<unsigned long long>(second[0].value) != 9)
    return 3;
  return 0;
}}
''',
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

    def test_consume_only_completion_updates_state_without_dummy_sink(
        self,
    ) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native consume-only rule tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "consume_only_completion.cpp"
            acir = root / "consume_only_completion.frozen.mlir"
            plan = root / "consume_only_completion.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(CONSUME_ONLY_COMPLETION_SOURCE),
                    "--system",
                    "consume_only_completion",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn("kind = #ac<rule_check_kind input_available>", frozen)
            self.assertNotIn("ac.sink", frozen)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            firing = next(
                block for block in parsed_plan["blocks"] if block["kind"] == "firing"
            )
            self.assertEqual(["completion"], firing["inputs"])
            self.assertNotEqual(firing["guard"], firing["state_writes"][0]["present"])
            generated_source = model.read_text(encoding="utf-8")
            self.assertIn("proposal_present", generated_source)

            harness = root / "harness.cpp"
            executable = root / "consume_only_completion"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::ConsumeOnlyCompletion model;
  auto rows = model.dispatch_rows();
  auto cycle = [&](gfsim::Epoch epoch, bool expects_table_write) {{
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    if (model.table_entries().hasPendingCommit() != expects_table_write)
      return false;
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
    return true;
  }};
  if (!model.completion().proposePush(ac_generated::Entry{{
          gfsim::UInt<2>{{2}}, gfsim::UInt<8>{{1}}, gfsim::UInt<8>{{33}}}}))
    return 1;
  model.completion().doXfer({{0, 0}});
  if (!cycle({{1, 0}}, false))
    return 2;
  if (!model.completion().isEmpty())
    return 3;
  const auto &stale = model.table_entries().at(2);
  if (static_cast<unsigned long long>(stale.generation) != 0 ||
      static_cast<unsigned long long>(stale.value) != 0)
    return 4;
  if (!model.completion().proposePush(ac_generated::Entry{{
          gfsim::UInt<2>{{2}}, gfsim::UInt<8>{{0}}, gfsim::UInt<8>{{55}}}}))
    return 5;
  model.completion().doXfer({{2, 0}});
  if (!cycle({{3, 0}}, true))
    return 6;
  if (!model.completion().isEmpty())
    return 7;
  const auto &fresh = model.table_entries().at(2);
  return static_cast<unsigned long long>(fresh.value) == 55 ? 0 : 8;
}}
''',
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

    def test_state_driven_retire_preserves_entry_under_output_backpressure(
        self,
    ) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native state-driven rule tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "state_driven_retire.cpp"
            acir = root / "state_driven_retire.frozen.mlir"
            plan = root / "state_driven_retire.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(STATE_DRIVEN_RETIRE_SOURCE),
                    "--system",
                    "state_driven_retire",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn("ac.guard_kind = #ac<rule_guard_kind predicate>", frozen)
            self.assertIn("ac.firing.condition", frozen)
            self.assertIn("kind = #ac<rule_check_kind output_capacity>", frozen)

            harness = root / "harness.cpp"
            executable = root / "state_driven_retire"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::StateDrivenRetire model;
  const auto &table = model.table_entries();
  if (!model.incoming().proposePush(
          ac_generated::Entry{{gfsim::UInt<1>{{0}},
                                  gfsim::UInt<7>{{41}}, true}}))
    return 1;
  model.incoming().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  auto run = [&](unsigned tick, bool run_sink) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      if (run_sink || row.kind != gfsim::ObjectKind::Sink)
        row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }};

  run(1, false);
  if (!table.at(0).valid)
    return 2;
  run(2, false);
  if (table.at(0).valid)
    return 3;
  if (!model.incoming().proposePush(
          ac_generated::Entry{{gfsim::UInt<1>{{0}},
                                  gfsim::UInt<7>{{77}}, true}}))
    return 4;
  model.incoming().doXfer({{2, 0}});

  // The first retire value still occupies the result Queue. The second
  // candidate must not clear its entry while that Queue is backpressured.
  run(3, false);
  if (!table.at(0).valid)
    return 5;
  run(4, false);
  if (!table.at(0).valid)
    return 6;
  // A ready sink permits the old result pop and new retire push to share the
  // next committed boundary. The retire candidate is recomputed from the new
  // Queue snapshot on the following tick.
  run(5, true);
  if (!table.at(0).valid)
    return 7;
  run(6, true);
  if (table.at(0).valid)
    return 8;
  run(7, true);

  const auto &values = model.sink_0_values();
  if (values.size() != 2)
    return 9;
  if (static_cast<unsigned long long>(values[0].value) != 41 ||
      static_cast<unsigned long long>(values[1].value) != 77)
    return 10;
  return 0;
}}
''',
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

    def test_multi_state_rule_commits_tail_entry_and_output_together(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native multi-state rule tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "multi_state_allocate.cpp"
            acir = root / "multi_state_allocate.frozen.mlir"
            plan = root / "multi_state_allocate.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(MULTI_STATE_ALLOCATE_SOURCE),
                    "--system",
                    "multi_state_allocate",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertEqual(2, frozen.count("ac.table.propose"))
            self.assertIn("ac.table.propose @tail", frozen)
            self.assertIn("ac.table.propose @entries", frozen)
            self.assertIn(
                "gfsim::QueueStateTransition",
                model.read_text(encoding="utf-8"),
            )

            harness = root / "harness.cpp"
            executable = root / "multi_state_allocate"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::MultiStateAllocate model;
  auto rows = model.dispatch_rows();
  auto run = [&](unsigned tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }};

  if (!model.incoming().proposePush(
          ac_generated::Entry{{gfsim::UInt<2>{{3}}, gfsim::UInt<8>{{10}}}}))
    return 1;
  model.incoming().doXfer({{0, 0}});
  run(1);
  run(2);
  if (!model.incoming().proposePush(
          ac_generated::Entry{{gfsim::UInt<2>{{2}}, gfsim::UInt<8>{{20}}}}))
    return 2;
  model.incoming().doXfer({{2, 0}});
  run(3);
  run(4);
  run(5);

  if (static_cast<unsigned long long>(model.table_tail().at(0)) != 2)
    return 3;
  if (static_cast<unsigned long long>(model.table_entries().at(0).value) != 10 ||
      static_cast<unsigned long long>(model.table_entries().at(1).value) != 20)
    return 4;
  const auto &values = model.sink_0_values();
  if (values.size() != 2 ||
      static_cast<unsigned long long>(values[0].value) != 10 ||
      static_cast<unsigned long long>(values[1].value) != 20)
    return 5;
  return 0;
}}
''',
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

    def test_circular_rob_wraps_rejects_stale_completion_and_recovers(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native circular ROB tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "circular_rob.cpp"
            acir = root / "circular_rob.frozen.mlir"
            plan = root / "circular_rob.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(CIRCULAR_ROB_SOURCE),
                    "--system",
                    "circular_rob",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            source = CIRCULAR_ROB_SOURCE.read_text(encoding="utf-8")
            for forbidden in (
                "source(",
                "sink(",
                "ac.table",
                "ac.queue",
                ".pop(",
                ".push(",
                ".empty(",
                ".full(",
            ):
                self.assertNotIn(forbidden, source)
            frozen = acir.read_text(encoding="utf-8")
            self.assertEqual(
                4, frozen.count(" = ac.firing ") + frozen.count("  ac.firing ")
            )
            self.assertIn("kind = #ac<rule_check_kind input_available>", frozen)
            self.assertIn("kind = #ac<rule_check_kind output_capacity>", frozen)
            self.assertIn(
                "gfsim::QueueStateTransition", model.read_text(encoding="utf-8")
            )

            harness = root / "harness.cpp"
            executable = root / "circular_rob"
            harness.write_text(
                f'''#include "{model.name}"

#include <array>
#include <cstdint>

int main() {{
  ac_generated::CircularRob model;
  auto rows = model.dispatch_rows();
  std::uint64_t tick = 0;
  auto event = [](unsigned index, unsigned generation, unsigned epoch,
                  unsigned value, bool done = false) {{
    return ac_generated::RobEvent{{gfsim::UInt<2>{{index}},
                                  gfsim::UInt<16>{{generation}},
                                  gfsim::UInt<16>{{epoch}},
                                  gfsim::UInt<16>{{value}},
                                  gfsim::UInt<1>{{done}}}};
  }};
  auto offer = [&](auto &queue, const ac_generated::RobEvent &value) {{
    if (!queue.proposePush(value))
      return false;
    queue.doXfer({{tick++, 0}});
    return true;
  }};
  auto cycle = [&](bool drain_allocate = true, bool drain_retire = true) {{
    const gfsim::Epoch epoch{{tick++, 0}};
    for (auto &row : rows) {{
      const bool run = row.kind != gfsim::ObjectKind::Sink ||
                       (row.id == model.sink_0_id() && drain_allocate) ||
                       (row.id == model.sink_1_id() && drain_retire);
      if (run)
        row.work(row.object, epoch);
    }}
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }};
  auto scalar = [](const auto &table) {{
    return static_cast<unsigned long long>(table.at(0));
  }};

  // Allocation output backpressure is inferred: with the first tag undrained,
  // the second request cannot advance tail/count or update its entry.
  if (!offer(model.allocate_request(), event(0, 0, 0, 10)))
    return 1;
  cycle(false, true);
  if (!offer(model.allocate_request(), event(0, 0, 0, 20)))
    return 1;
  cycle(false, true);
  if (model.allocate_request().committedSize() != 1 ||
      scalar(model.table_count()) != 1 || scalar(model.table_tail()) != 1)
    return 2;
  cycle(true, true);
  cycle(true, true);
  cycle();

  // Fill the remaining entries. Fixed-width tail wraps to zero while count
  // records the full/empty distinction.
  for (unsigned value : {{30u, 40u}}) {{
    if (!offer(model.allocate_request(), event(0, 0, 0, value)))
      return 1;
    cycle();
    cycle();
  }}
  if (scalar(model.table_count()) != 4 || scalar(model.table_tail()) != 0)
    return 3;

  // A fifth allocation remains at the input boundary while the ROB is full.
  if (!offer(model.allocate_request(), event(0, 0, 0, 50)))
    return 4;
  cycle();
  if (model.allocate_request().committedSize() != 1 ||
      scalar(model.table_count()) != 4)
    return 5;

  // Complete out of order. Head zero retires first and fills the deliberately
  // undrained retirement output; only then may the waiting fifth allocation
  // reuse slot zero with generation two.
  if (!offer(model.completion(), event(2, 1, 0, 0)))
    return 5;
  cycle();
  if (!offer(model.completion(), event(0, 1, 0, 0)))
    return 6;
  cycle();
  cycle(true, false);
  if (scalar(model.table_head()) != 1 || scalar(model.table_count()) != 3)
    return 7;
  cycle(true, false);
  if (model.allocate_request().committedSize() != 0 ||
      scalar(model.table_tail()) != 1 || scalar(model.table_count()) != 4 ||
      static_cast<unsigned long long>(
          model.table_entries().at(0).generation) != 2)
    return 8;

  // The stale generation is consumed but cannot mark the reused slot done.
  if (!offer(model.completion(), event(0, 1, 0, 0)))
    return 9;
  cycle(true, false);
  if (static_cast<bool>(model.table_entries().at(0).done))
    return 10;

  const std::array<ac_generated::RobEvent, 3> completions{{{{
      event(3, 1, 0, 0),
      event(1, 1, 0, 0),
      event(0, 2, 0, 0),
  }}}};
  for (const auto &completion : completions) {{
    if (!offer(model.completion(), completion))
      return 11;
    cycle(true, false);
  }}
  const auto held_head = scalar(model.table_head());
  const auto held_count = scalar(model.table_count());
  cycle(true, false);
  if (scalar(model.table_head()) != held_head ||
      scalar(model.table_count()) != held_count)
    return 12;
  for (unsigned iteration = 0; iteration < 20; ++iteration)
    cycle();
  if (scalar(model.table_count()) != 0 || scalar(model.table_head()) != 1)
    return 13;
  const auto &retired_before_flush = model.sink_1_values();
  if (retired_before_flush.size() != 5)
    return 14;
  constexpr unsigned expected[5] = {{10, 20, 30, 40, 50}};
  for (unsigned index = 0; index < 5; ++index)
    if (static_cast<unsigned long long>(retired_before_flush[index].value) !=
        expected[index])
      return 15;

  // Flush discards one uncompleted entry by moving head to tail, clearing
  // occupancy, and advancing recovery epoch. Its old completion is harmless.
  if (!offer(model.allocate_request(), event(0, 0, 0, 60)))
    return 16;
  cycle();
  cycle();
  const unsigned old_index = 1;
  const unsigned old_generation = 2;
  if (!offer(model.flush_request(), event(0, 0, 0, 0)))
    return 17;
  cycle();
  if (scalar(model.table_count()) != 0 || scalar(model.table_head()) != 2 ||
      scalar(model.table_tail()) != 2 || scalar(model.table_epoch()) != 1)
    return 18;
  if (!offer(model.completion(),
             event(old_index, old_generation, 0, 0)))
    return 19;
  cycle();
  if (static_cast<bool>(model.table_entries().at(old_index).done))
    return 20;

  if (!offer(model.allocate_request(), event(0, 0, 0, 70)))
    return 21;
  cycle();
  cycle();
  if (!offer(model.completion(), event(2, 2, 1, 0)))
    return 22;
  cycle();
  for (unsigned iteration = 0; iteration < 8; ++iteration)
    cycle();
  const auto &retired = model.sink_1_values();
  if (retired.size() != 6 ||
      static_cast<unsigned long long>(retired.back().value) != 70)
    return 23;
  return 0;
}}
''',
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

    def test_reusable_circular_rob_preserves_rules_and_instance_state(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native reusable ROB tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "reusable_circular_rob.cpp"
            acir = root / "reusable_circular_rob.frozen.mlir"
            plan = root / "reusable_circular_rob.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(REUSABLE_CIRCULAR_ROB_SOURCE),
                    "--system",
                    "reusable_circular_rob",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            source = REUSABLE_CIRCULAR_ROB_SOURCE.read_text(encoding="utf-8")
            self.assertNotIn("\n    epoch = epoch\n", source)
            for forbidden in (
                "source(",
                "sink(",
                "ac.table",
                "ac.queue",
                ".pop(",
                ".push(",
                ".empty(",
                ".full(",
            ):
                self.assertNotIn(forbidden, source)
            frozen = acir.read_text(encoding="utf-8")
            self.assertEqual(
                4, frozen.count(" = ac.firing ") + frozen.count("  ac.firing ")
            )
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(2, len(parsed_plan["module_instances"]))
            self.assertEqual(1, len(parsed_plan["module_specializations"]))
            self.assertGreater(len(parsed_plan["activation_edges"]), 0)
            specialization = parsed_plan["module_specializations"][0]
            self.assertEqual("rob", specialization["definition"])
            self.assertEqual(3, len(specialization["interface_inputs"]))
            self.assertEqual(2, len(specialization["interface_outputs"]))
            self.assertEqual(5, len(specialization["tables"]))
            self.assertEqual(4, len(specialization["blocks"]))
            completion = next(
                block
                for block in specialization["blocks"]
                if block["name"].startswith("complete")
            )
            self.assertEqual(
                {"entries", "epoch"},
                {
                    reservation["table"]
                    for reservation in completion["state_reservations"]
                },
            )
            self.assertEqual(
                {"entries": ["generation", "epoch"], "epoch": ["$entry"]},
                {
                    reservation["table"]: reservation["fields"]
                    for reservation in completion["state_reservations"]
                },
            )
            self.assertEqual(
                ["entries"],
                [write["table"] for write in completion["state_writes"]],
            )
            self.assertEqual(
                {"entries"},
                {
                    resource["resource"]
                    for resource in completion["transaction_resources"]
                    if resource["kind"] == "state"
                },
            )
            self.assertGreater(len(specialization["activation_edges"]), 0)
            self.assertGreater(len(specialization["initial_activation"]), 0)
            generated_source = model.read_text(encoding="utf-8")
            self.assertEqual(1, generated_source.count("class Rob_"))
            self.assertIn("gfsim::QueueStateTransition<", generated_source)
            self.assertIn("activation_offsets()", generated_source)
            self.assertIn("activation_complete() { return true; }", generated_source)
            self.assertIn("activation_targets()", generated_source)
            self.assertIn("work_closure_offsets()", generated_source)
            self.assertIn("work_closure_targets()", generated_source)
            self.assertIn("initial_work_ids()", generated_source)
            self.assertIn("schedule_initial_work", generated_source)
            self.assertIn("offer_left_allocate", generated_source)

            harness = root / "harness.cpp"
            executable = root / "reusable_circular_rob"
            harness.write_text(
                f'''#include "{model.name}"

#include <cstdint>

int main() {{
  ac_generated::ReusableCircularRob model;
  auto rows = model.dispatch_rows();
  std::uint64_t tick = 0;
  auto event = [](unsigned index, unsigned generation, unsigned epoch,
                  unsigned value, bool done = false) {{
    return ac_generated::RobEvent{{gfsim::UInt<2>{{index}},
                                  gfsim::UInt<16>{{generation}},
                                  gfsim::UInt<16>{{epoch}},
                                  gfsim::UInt<16>{{value}},
                                  gfsim::UInt<1>{{done}}}};
  }};
  auto offer = [&](auto &queue, const ac_generated::RobEvent &value) {{
    if (!queue.proposePush(value))
      return false;
    queue.doXfer({{tick++, 0}});
    return true;
  }};
  auto cycle = [&]() {{
    const gfsim::Epoch epoch{{tick++, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Probe);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }};

  if (!offer(model.left_allocate(), event(0, 0, 0, 100)) ||
      !offer(model.right_allocate(), event(0, 0, 0, 200)))
    return 1;
  for (unsigned index = 0; index < 8; ++index)
    cycle();
  const auto &left_allocated = model.sink_0_values();
  const auto &right_allocated = model.sink_2_values();
  if (left_allocated.size() != 1 || right_allocated.size() != 1 ||
      static_cast<unsigned long long>(left_allocated[0].index) != 0 ||
      static_cast<unsigned long long>(right_allocated[0].index) != 0 ||
      static_cast<unsigned long long>(left_allocated[0].value) != 100 ||
      static_cast<unsigned long long>(right_allocated[0].value) != 200)
    return 2;

  if (!offer(model.left_completion(),
             event(static_cast<unsigned long long>(left_allocated[0].index),
                   static_cast<unsigned long long>(left_allocated[0].generation),
                   static_cast<unsigned long long>(left_allocated[0].epoch), 0)) ||
      !offer(model.right_completion(),
             event(static_cast<unsigned long long>(right_allocated[0].index),
                   static_cast<unsigned long long>(right_allocated[0].generation),
                   static_cast<unsigned long long>(right_allocated[0].epoch), 0)))
    return 3;
  for (unsigned index = 0; index < 12; ++index)
    cycle();
  const auto &left_retired = model.sink_1_values();
  const auto &right_retired = model.sink_3_values();
  if (left_retired.size() != 1 || right_retired.size() != 1 ||
      static_cast<unsigned long long>(left_retired[0].value) != 100 ||
      static_cast<unsigned long long>(right_retired[0].value) != 200)
    return 4;

  if (!offer(model.left_allocate(), event(0, 0, 0, 300)))
    return 5;
  for (unsigned index = 0; index < 8; ++index)
    cycle();
  if (left_allocated.size() != 2 || right_allocated.size() != 1 ||
      static_cast<unsigned long long>(left_allocated[1].index) != 1 ||
      static_cast<unsigned long long>(left_allocated[1].value) != 300)
    return 6;

  ac_generated::ReusableCircularRob incremental;
  static_assert(ac_generated::ReusableCircularRob::activation_complete());
  gfsim::SimSystem activation_system("rob_activation");
  auto activation_rows = incremental.dispatch_rows();
  constexpr auto activation_offsets =
      ac_generated::ReusableCircularRob::activation_offsets();
  constexpr auto activation_targets =
      ac_generated::ReusableCircularRob::activation_targets();
  constexpr auto closure_offsets =
      ac_generated::ReusableCircularRob::work_closure_offsets();
  constexpr auto closure_targets =
      ac_generated::ReusableCircularRob::work_closure_targets();
  if (!activation_system.setDispatchTable(activation_rows) ||
      !activation_system.setActivationPlan(activation_offsets,
                                           activation_targets) ||
      !activation_system.setWorkClosurePlan(closure_offsets, closure_targets))
    return 7;
  if (!ac_generated::ReusableCircularRob::schedule_initial_work(
          activation_system))
    return 8;
  if (!incremental.offer_left_allocate(activation_system,
                                       event(0, 0, 0, 100)) ||
      !incremental.offer_left_completion(activation_system,
                                         event(0, 1, 0, 0)) ||
      !incremental.offer_right_allocate(activation_system,
                                        event(0, 0, 0, 200)) ||
      !incremental.offer_right_completion(activation_system,
                                          event(0, 1, 0, 0)))
    return 9;
  const auto activation_result = activation_system.run();
  if (activation_result.classification != gfsim::TerminationClass::Completed)
    return 11;
  const auto &active_left_allocated = incremental.sink_0_values();
  const auto &active_left_retired = incremental.sink_1_values();
  const auto &active_right_allocated = incremental.sink_2_values();
  const auto &active_right_retired = incremental.sink_3_values();
  if (active_left_allocated.size() != 1 || active_left_retired.size() != 1 ||
      active_right_allocated.size() != 1 || active_right_retired.size() != 1 ||
      active_left_allocated[0] != left_allocated[0] ||
      active_left_retired[0] != left_retired[0] ||
      active_right_allocated[0] != right_allocated[0] ||
      active_right_retired[0] != right_retired[0])
    return 12;
  if (activation_system.activationTraversalCount() == 0 ||
      activation_system.workClosureTraversalCount() == 0 ||
      activation_system.workInvocationCount() >= activation_rows.size() * 2)
    return 13;
  return 0;
}}
''',
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/gfsim/libgfsim.a"),
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

    def test_reusable_rob_scan_and_activation_match_every_tick(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native reusable ROB tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "reusable_rob_equivalence.cpp"
            acir = root / "reusable_rob_equivalence.frozen.mlir"
            plan = root / "reusable_rob_equivalence.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(REUSABLE_CIRCULAR_ROB_SOURCE),
                    "--system",
                    "reusable_circular_rob",
                    "--host-results",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(2, len(parsed_plan["module_instances"]))
            self.assertEqual(1, len(parsed_plan["module_specializations"]))

            harness = root / "harness.cpp"
            executable = root / "reusable_rob_equivalence"
            harness.write_text(
                """#include "__MODEL__"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

using Model = ac_generated::ReusableCircularRob;
using Event = ac_generated::RobEvent;
constexpr gfsim::ObjectId kModelObjects = 28;
constexpr gfsim::ObjectId kClockId = kModelObjects;

class EquivalenceClock final : public gfsim::SimObject {
public:
  EquivalenceClock(gfsim::SimSystem &system, bool fullScan, gfsim::Tick limit)
      : gfsim::SimObject(gfsim::ObjectKind::Process, "equivalence_clock",
                         kClockId),
        system_(system), fullScan_(fullScan), limit_(limit) {}

  void doWork(gfsim::Epoch epoch) override {
    if (epoch.time + 1 >= limit_)
      return;
    const gfsim::Epoch next{epoch.time + 1, 0};
    if (fullScan_)
      for (gfsim::ObjectId id = 0; id < kModelObjects; ++id)
        valid_ = system_.scheduleWork(id, next) && valid_;
    valid_ = system_.scheduleWork(id(), next) && valid_;
  }

  bool validate() const { return valid_; }

private:
  gfsim::SimSystem &system_;
  bool fullScan_ = false;
  gfsim::Tick limit_ = 0;
  bool valid_ = true;
};

template <typename T>
T &objectAs(gfsim::DispatchRow &row) {
  return *static_cast<T *>(static_cast<gfsim::SimObject *>(row.object));
}

struct Snapshot {
  std::array<std::vector<Event>, 10> queues;
  std::array<unsigned long long, 8> scalars{};
  std::array<Event, 8> entries{};

  bool operator==(const Snapshot &) const = default;
};

template <size_t N>
std::vector<uint32_t> extendOffsets(const std::array<uint32_t, N> &input) {
  std::vector<uint32_t> result(input.begin(), input.end());
  result.push_back(result.back());
  return result;
}

std::array<gfsim::DispatchRow, kModelObjects + 1>
rowsWithClock(Model &model, EquivalenceClock &clock) {
  std::array<gfsim::DispatchRow, kModelObjects + 1> result{};
  auto modelRows = model.dispatch_rows();
  std::copy(modelRows.begin(), modelRows.end(), result.begin());
  result.back() = gfsim::makeDispatchRow(&clock);
  return result;
}

Snapshot snapshot(std::array<gfsim::DispatchRow, kModelObjects + 1> &rows) {
  Snapshot result;
  for (size_t index = 0; index < result.queues.size(); ++index)
    result.queues[index] =
        objectAs<gfsim::SimQueue<Event>>(rows[index]).committedValues();

  result.scalars[0] = static_cast<unsigned long long>(
      objectAs<gfsim::SimTable<gfsim::UInt<2>>>(rows[14]).at(0));
  result.scalars[1] = static_cast<unsigned long long>(
      objectAs<gfsim::SimTable<gfsim::UInt<2>>>(rows[15]).at(0));
  result.scalars[2] = static_cast<unsigned long long>(
      objectAs<gfsim::SimTable<gfsim::UInt<3>>>(rows[16]).at(0));
  result.scalars[3] = static_cast<unsigned long long>(
      objectAs<gfsim::SimTable<gfsim::UInt<16>>>(rows[17]).at(0));
  result.scalars[4] = static_cast<unsigned long long>(
      objectAs<gfsim::SimTable<gfsim::UInt<2>>>(rows[23]).at(0));
  result.scalars[5] = static_cast<unsigned long long>(
      objectAs<gfsim::SimTable<gfsim::UInt<2>>>(rows[24]).at(0));
  result.scalars[6] = static_cast<unsigned long long>(
      objectAs<gfsim::SimTable<gfsim::UInt<3>>>(rows[25]).at(0));
  result.scalars[7] = static_cast<unsigned long long>(
      objectAs<gfsim::SimTable<gfsim::UInt<16>>>(rows[26]).at(0));

  const auto &leftEntries = objectAs<gfsim::SimTable<Event>>(rows[18]);
  const auto &rightEntries = objectAs<gfsim::SimTable<Event>>(rows[27]);
  for (size_t index = 0; index < 4; ++index) {
    result.entries[index] = leftEntries.at(index);
    result.entries[index + 4] = rightEntries.at(index);
  }
  return result;
}

int main() {
  Model scanModel;
  Model incrementalModel;
  gfsim::SimSystem scanSystem("rob_scan");
  gfsim::SimSystem incrementalSystem("rob_incremental");
  scanSystem.setBuildProfile(gfsim::BuildProfile::Validated);
  incrementalSystem.setBuildProfile(gfsim::BuildProfile::Validated);
  EquivalenceClock scanClock(scanSystem, true, 256);
  EquivalenceClock incrementalClock(incrementalSystem, false, 256);
  auto scanRows = rowsWithClock(scanModel, scanClock);
  auto incrementalRows = rowsWithClock(incrementalModel, incrementalClock);
  if (!scanSystem.setDispatchTable(scanRows) ||
      !incrementalSystem.setDispatchTable(incrementalRows))
    return 1;

  constexpr auto baseActivationOffsets = Model::activation_offsets();
  constexpr auto activationTargets = Model::activation_targets();
  constexpr auto baseClosureOffsets = Model::work_closure_offsets();
  constexpr auto closureTargets = Model::work_closure_targets();
  const auto activationOffsets = extendOffsets(baseActivationOffsets);
  const auto closureOffsets = extendOffsets(baseClosureOffsets);
  if (!incrementalSystem.setActivationPlan(activationOffsets,
                                            activationTargets) ||
      !incrementalSystem.setWorkClosurePlan(closureOffsets, closureTargets) ||
      !Model::schedule_initial_work(incrementalSystem))
    return 2;
  for (gfsim::ObjectId id = 0; id < kModelObjects; ++id)
    if (!scanSystem.scheduleWork(id, {0, 0}))
      return 3;
  if (!scanSystem.scheduleWork(kClockId, {0, 0}) ||
      !incrementalSystem.scheduleWork(kClockId, {0, 0}))
    return 4;

  auto event = [](unsigned index, unsigned generation, unsigned epoch,
                  unsigned value, bool done = false) {
    return Event{gfsim::UInt<2>{index}, gfsim::UInt<16>{generation},
                 gfsim::UInt<16>{epoch}, gfsim::UInt<16>{value},
                 gfsim::UInt<1>{done}};
  };
  bool equivalent = true;
  auto advance = [&]() {
    if (scanSystem.currentEpoch() != incrementalSystem.currentEpoch())
      return false;
    if (!scanSystem.step() || !incrementalSystem.step())
      return false;
    equivalent = equivalent && snapshot(scanRows) == snapshot(incrementalRows);
    equivalent = equivalent &&
                 scanSystem.commitTimeline() ==
                     incrementalSystem.commitTimeline();
    return equivalent;
  };
  auto advanceUntil = [&](auto predicate) {
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
      if (predicate())
        return true;
      if (!advance())
        return false;
    }
    return false;
  };
  auto offerLeftAllocate = [&](Event value) {
    return scanModel.offer_left_allocate(scanSystem, value) &&
           incrementalModel.offer_left_allocate(incrementalSystem, value);
  };
  auto offerLeftCompletion = [&](Event value) {
    return scanModel.offer_left_completion(scanSystem, value) &&
           incrementalModel.offer_left_completion(incrementalSystem, value);
  };
  auto offerLeftFlush = [&](Event value) {
    return scanModel.offer_left_flush(scanSystem, value) &&
           incrementalModel.offer_left_flush(incrementalSystem, value);
  };
  auto offerRightAllocate = [&](Event value) {
    return scanModel.offer_right_allocate(scanSystem, value) &&
           incrementalModel.offer_right_allocate(incrementalSystem, value);
  };
  auto offerRightCompletion = [&](Event value) {
    return scanModel.offer_right_completion(scanSystem, value) &&
           incrementalModel.offer_right_completion(incrementalSystem, value);
  };
  auto take = [&](unsigned port) -> std::optional<Event> {
    std::optional<Event> scanValue;
    std::optional<Event> incrementalValue;
    switch (port) {
    case 0:
      scanValue = scanModel.try_take_result_0(scanSystem);
      incrementalValue =
          incrementalModel.try_take_result_0(incrementalSystem);
      break;
    case 1:
      scanValue = scanModel.try_take_result_1(scanSystem);
      incrementalValue =
          incrementalModel.try_take_result_1(incrementalSystem);
      break;
    case 2:
      scanValue = scanModel.try_take_result_2(scanSystem);
      incrementalValue =
          incrementalModel.try_take_result_2(incrementalSystem);
      break;
    case 3:
      scanValue = scanModel.try_take_result_3(scanSystem);
      incrementalValue =
          incrementalModel.try_take_result_3(incrementalSystem);
      break;
    default:
      return std::nullopt;
    }
    if (!scanValue || !incrementalValue || *scanValue != *incrementalValue) {
      equivalent = false;
      return std::nullopt;
    }
    return scanValue;
  };
  auto queueEmpty = [&](size_t id) {
    return objectAs<gfsim::SimQueue<Event>>(scanRows[id]).isEmpty();
  };
  auto leftState = [&]() { return snapshot(scanRows).scalars; };

  if (!offerLeftAllocate(event(0, 0, 0, 10)) ||
      !offerRightAllocate(event(0, 0, 0, 100)) ||
      !advanceUntil([&] {
        return !scanModel.result_0().isEmpty() &&
               !scanModel.result_2().isEmpty();
      }))
    return 5;
  const Event left10 = scanModel.result_0().committedValues().front();
  const Event right100 = scanModel.result_2().committedValues().front();
  if (!take(2) || !advance() ||
      !offerRightCompletion(event(
          static_cast<unsigned long long>(right100.index),
          static_cast<unsigned long long>(right100.generation),
          static_cast<unsigned long long>(right100.epoch), 0)) ||
      !advanceUntil([&] { return !scanModel.result_3().isEmpty(); }))
    return 6;
  auto retiredRight = take(3);
  if (!retiredRight ||
      static_cast<unsigned long long>(retiredRight->value) != 100 ||
      !advance())
    return 7;

  // Hold the first allocation result full. The second request may reach the
  // input Queue, but count/tail/entries must not partially advance.
  if (!offerLeftAllocate(event(0, 0, 0, 20)) || !advance() || !advance())
    return 8;
  auto state = leftState();
  if (queueEmpty(1) || state[1] != 1 || state[2] != 1)
    return 9;
  if (!take(0) || !advance() ||
      !advanceUntil([&] { return !scanModel.result_0().isEmpty(); }))
    return 10;
  const Event left20 = scanModel.result_0().committedValues().front();
  if (static_cast<unsigned long long>(left20.index) != 1 || !take(0) ||
      !advance())
    return 11;

  std::array<Event, 2> later{};
  for (auto [index, value] :
       std::array<std::pair<unsigned, unsigned>, 2>{{{0, 30}, {1, 40}}}) {
    (void)index;
    if (!offerLeftAllocate(event(0, 0, 0, value)) ||
        !advanceUntil([&] { return !scanModel.result_0().isEmpty(); }))
      return 12;
    later[value == 30 ? 0 : 1] =
        scanModel.result_0().committedValues().front();
    if (!take(0) || !advance())
      return 13;
  }
  state = leftState();
  if (state[1] != 0 || state[2] != 4)
    return 14;

  if (!offerLeftAllocate(event(0, 0, 0, 50)) || !advance() || !advance())
    return 15;
  state = leftState();
  if (queueEmpty(1) || state[2] != 4)
    return 16;

  auto complete = [&](const Event &tag) {
    return offerLeftCompletion(event(
               static_cast<unsigned long long>(tag.index),
               static_cast<unsigned long long>(tag.generation),
               static_cast<unsigned long long>(tag.epoch), 0)) &&
           advance() &&
           advanceUntil([&] { return queueEmpty(2); });
  };
  if (!complete(later[0]) || !complete(left10) ||
      !advanceUntil([&] { return !scanModel.result_1().isEmpty(); }))
    return 17;
  state = leftState();
  if (state[0] != 1 || state[2] != 3 ||
      !advanceUntil([&] { return !scanModel.result_0().isEmpty(); }))
    return 18;
  const Event left50 = scanModel.result_0().committedValues().front();
  if (static_cast<unsigned long long>(left50.index) != 0 ||
      static_cast<unsigned long long>(left50.generation) != 2 || !take(0) ||
      !advance())
    return 19;

  // A stale completion still commits its input transaction. Its state
  // reservation serializes against an overlapping writer, but the unselected
  // effects must produce no epoch/entry Table commit.
  const size_t staleCommitStart = scanSystem.commitTimeline().size();
  if (!complete(left10))
    return 20;
  const auto &staleTimeline = scanSystem.commitTimeline();
  if (std::any_of(staleTimeline.begin() + staleCommitStart,
                  staleTimeline.end(), [](const gfsim::CommitEvent &event) {
                    return event.objectId == 17 || event.objectId == 18;
                  }))
    return 39;
  const auto staleState = snapshot(scanRows);
  if (static_cast<bool>(staleState.entries[0].done))
    return 21;
  if (!complete(later[1]) || !complete(left20) || !complete(left50))
    return 22;
  state = leftState();
  for (unsigned iteration = 0; iteration < 3; ++iteration)
    if (!advance())
      return 23;
  if (leftState() != state)
    return 24;

  std::vector<unsigned long long> retiredValues;
  for (unsigned expected : {10u, 20u, 30u, 40u, 50u}) {
    if (!advanceUntil([&] { return !scanModel.result_1().isEmpty(); }))
      return 25;
    auto value = take(1);
    if (!value || static_cast<unsigned long long>(value->value) != expected)
      return 26;
    retiredValues.push_back(static_cast<unsigned long long>(value->value));
    if (!advance())
      return 27;
  }
  state = leftState();
  if (state[0] != 1 || state[2] != 0)
    return 28;

  if (!offerLeftAllocate(event(0, 0, 0, 60)) ||
      !advanceUntil([&] { return !scanModel.result_0().isEmpty(); }))
    return 29;
  const Event left60 = scanModel.result_0().committedValues().front();
  if (!take(0) || !advance() ||
      !offerLeftFlush(event(0, 0, 0, 0)) ||
      !advance() ||
      !advanceUntil([&] { return queueEmpty(0); }))
    return 30;
  state = leftState();
  if (state[0] != 2 || state[1] != 2 || state[2] != 0 || state[3] != 1)
    return 31;
  if (!complete(left60))
    return 32;
  if (static_cast<bool>(snapshot(scanRows).entries[1].done))
    return 33;

  if (!offerLeftAllocate(event(0, 0, 0, 70)) ||
      !advanceUntil([&] { return !scanModel.result_0().isEmpty(); }))
    return 34;
  const Event left70 = scanModel.result_0().committedValues().front();
  if (static_cast<unsigned long long>(left70.index) != 2 ||
      static_cast<unsigned long long>(left70.epoch) != 1 || !take(0) ||
      !advance() || !complete(left70) ||
      !advanceUntil([&] { return !scanModel.result_1().isEmpty(); }))
    return 35;
  auto retired70 = take(1);
  if (!retired70 || static_cast<unsigned long long>(retired70->value) != 70 ||
      !advance())
    return 36;

  state = leftState();
  if (!equivalent || state[2] != 0 || state[4] != 1 || state[5] != 1 ||
      state[6] != 0 || state[7] != 0 || retiredValues.size() != 5)
    return 37;
  if (incrementalSystem.activationTraversalCount() == 0 ||
      incrementalSystem.workClosureTraversalCount() == 0 ||
      incrementalSystem.workInvocationCount() * 3 >=
          scanSystem.workInvocationCount())
    return 38;
  std::cout << "scan_work=" << scanSystem.workInvocationCount()
            << " incremental_work=" << incrementalSystem.workInvocationCount()
            << " activation="
            << incrementalSystem.activationTraversalCount()
            << " closure="
            << incrementalSystem.workClosureTraversalCount() << std::endl;
  return 0;
}
""".replace("__MODEL__", model.name),
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/gfsim/libgfsim.a"),
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
            self.assertEqual(
                "scan_work=1769 incremental_work=182 activation=215 closure=511\n",
                executed.stdout,
            )

    def test_reusable_oldest_ready_isq_closes_lost_wakeup_and_backpressure(
        self,
    ) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native reusable ISQ tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "reusable_oldest_ready_isq.cpp"
            acir = root / "reusable_oldest_ready_isq.frozen.mlir"
            plan = root / "reusable_oldest_ready_isq.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(REUSABLE_OLDEST_READY_ISQ_SOURCE),
                    "--system",
                    "reusable_oldest_ready_isq",
                    "--host-results",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            source = REUSABLE_OLDEST_READY_ISQ_SOURCE.read_text(encoding="utf-8")
            for forbidden in (
                "source(",
                "sink(",
                "ac.table",
                "ac.queue",
                ".pop(",
                ".push(",
                ".empty(",
                ".full(",
            ):
                self.assertNotIn(forbidden, source)
            self.assertIn("ac.find", source)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn("ac.table.match @entries", frozen)
            self.assertIn("ac.table.choose @entries", frozen)
            self.assertIn("resource = @ready_tags", frozen)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(2, len(parsed_plan["module_instances"]))
            self.assertEqual(1, len(parsed_plan["module_specializations"]))
            specialization = parsed_plan["module_specializations"][0]
            self.assertEqual("isq", specialization["definition"])
            self.assertEqual(2, len(specialization["tables"]))
            self.assertEqual(3, len(specialization["blocks"]))
            issue = next(
                block for block in specialization["blocks"] if block["name"] == "issued"
            )
            self.assertEqual(
                {"entries", "ready_tags"},
                {
                    resource["resource"]
                    for resource in issue["activation_sources"]
                    if resource["kind"] == "state"
                },
            )
            self.assertEqual(
                {"entries"},
                {
                    resource["resource"]
                    for resource in issue["transaction_resources"]
                    if resource["kind"] == "state"
                },
            )
            self.assertEqual(
                {"entries": "all", "ready_tags": "set"},
                {
                    reservation["table"]: reservation["index_kind"]
                    for reservation in issue["state_reservations"]
                },
            )
            ready_snapshot = next(
                reservation
                for reservation in issue["state_reservations"]
                if reservation["table"] == "ready_tags"
            )
            self.assertTrue(ready_snapshot["source"])
            generated_source = model.read_text(encoding="utf-8")
            self.assertEqual(1, generated_source.count("class Isq_"))
            self.assertIn("snapshot_set_", generated_source)
            self.assertEqual(0, generated_source.count("snapshot_entry"))
            self.assertEqual(2, generated_source.count("snapshot_set_1_0 |= "))
            self.assertIn("activation_complete() { return true; }", generated_source)

            harness = root / "harness.cpp"
            executable = root / "reusable_oldest_ready_isq"
            harness.write_text(
                """#include "__MODEL__"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

using Model = ac_generated::ReusableOldestReadyIsq;
using Entry = ac_generated::IssueEntry;
using Readiness = ac_generated::Readiness;
constexpr gfsim::ObjectId kModelObjects = 16;
constexpr gfsim::ObjectId kClockId = kModelObjects;

class EquivalenceClock final : public gfsim::SimObject {
public:
  EquivalenceClock(gfsim::SimSystem &system, bool fullScan)
      : gfsim::SimObject(gfsim::ObjectKind::Process, "host_clock", kClockId),
        system_(system), fullScan_(fullScan) {}

  void doWork(gfsim::Epoch epoch) override {
    if (epoch.time >= 255)
      return;
    const gfsim::Epoch next{epoch.time + 1, 0};
    if (fullScan_)
      for (gfsim::ObjectId id = 0; id < kModelObjects; ++id)
        valid_ = system_.scheduleWork(id, next) && valid_;
    valid_ = system_.scheduleWork(id(), next) && valid_;
  }

  bool validate() const { return valid_; }

private:
  gfsim::SimSystem &system_;
  bool fullScan_ = false;
  bool valid_ = true;
};

template <typename T>
T &objectAs(gfsim::DispatchRow &row) {
  return *static_cast<T *>(static_cast<gfsim::SimObject *>(row.object));
}

template <size_t N>
std::vector<uint32_t> extendOffsets(const std::array<uint32_t, N> &input) {
  std::vector<uint32_t> result(input.begin(), input.end());
  result.push_back(result.back());
  return result;
}

struct IsqSnapshot {
  std::array<std::vector<Entry>, 4> entryQueues;
  std::array<std::vector<Readiness>, 2> readinessQueues;
  std::array<Entry, 8> entries{};
  std::array<bool, 128> ready{};

  bool operator==(const IsqSnapshot &) const = default;
};

std::array<gfsim::DispatchRow, kModelObjects + 1>
isqRowsWithClock(Model &model, EquivalenceClock &clock) {
  std::array<gfsim::DispatchRow, kModelObjects + 1> result{};
  auto modelRows = model.dispatch_rows();
  std::copy(modelRows.begin(), modelRows.end(), result.begin());
  result.back() = gfsim::makeDispatchRow(&clock);
  return result;
}

IsqSnapshot
isqSnapshot(std::array<gfsim::DispatchRow, kModelObjects + 1> &rows) {
  IsqSnapshot result;
  constexpr std::array<size_t, 4> entryQueueIds{0, 2, 4, 5};
  constexpr std::array<size_t, 2> readinessQueueIds{1, 3};
  for (size_t index = 0; index < entryQueueIds.size(); ++index)
    result.entryQueues[index] =
        objectAs<gfsim::SimQueue<Entry>>(rows[entryQueueIds[index]])
            .committedValues();
  for (size_t index = 0; index < readinessQueueIds.size(); ++index)
    result.readinessQueues[index] =
        objectAs<gfsim::SimQueue<Readiness>>(rows[readinessQueueIds[index]])
            .committedValues();
  const auto &leftEntries = objectAs<gfsim::SimTable<Entry>>(rows[9]);
  const auto &leftReady =
      objectAs<gfsim::SimTable<gfsim::UInt<1>>>(rows[10]);
  const auto &rightEntries = objectAs<gfsim::SimTable<Entry>>(rows[14]);
  const auto &rightReady =
      objectAs<gfsim::SimTable<gfsim::UInt<1>>>(rows[15]);
  for (size_t index = 0; index < 4; ++index) {
    result.entries[index] = leftEntries.at(index);
    result.entries[index + 4] = rightEntries.at(index);
  }
  for (size_t index = 0; index < 64; ++index) {
    result.ready[index] = static_cast<bool>(leftReady.at(index));
    result.ready[index + 64] = static_cast<bool>(rightReady.at(index));
  }
  return result;
}

int main() {
  Model scanModel;
  Model incrementalModel;
  gfsim::SimSystem scanSystem("oldest_ready_isq_scan");
  gfsim::SimSystem incrementalSystem("oldest_ready_isq_incremental");
  scanSystem.setBuildProfile(gfsim::BuildProfile::Validated);
  incrementalSystem.setBuildProfile(gfsim::BuildProfile::Validated);
  EquivalenceClock scanClock(scanSystem, true);
  EquivalenceClock incrementalClock(incrementalSystem, false);
  auto scanRows = isqRowsWithClock(scanModel, scanClock);
  auto incrementalRows =
      isqRowsWithClock(incrementalModel, incrementalClock);
  constexpr auto baseActivationOffsets = Model::activation_offsets();
  constexpr auto activationTargets = Model::activation_targets();
  constexpr auto baseClosureOffsets = Model::work_closure_offsets();
  constexpr auto closureTargets = Model::work_closure_targets();
  const auto activationOffsets = extendOffsets(baseActivationOffsets);
  const auto closureOffsets = extendOffsets(baseClosureOffsets);
  if (!scanSystem.setDispatchTable(scanRows) ||
      !incrementalSystem.setDispatchTable(incrementalRows) ||
      !incrementalSystem.setActivationPlan(activationOffsets,
                                            activationTargets) ||
      !incrementalSystem.setWorkClosurePlan(closureOffsets, closureTargets) ||
      !Model::schedule_initial_work(incrementalSystem))
    return 1;
  for (gfsim::ObjectId id = 0; id < kModelObjects; ++id)
    if (!scanSystem.scheduleWork(id, {0, 0}))
      return 1;
  if (!scanSystem.scheduleWork(kClockId, {0, 0}) ||
      !incrementalSystem.scheduleWork(kClockId, {0, 0}))
    return 1;

  auto entry = [](unsigned age, unsigned tag, unsigned value) {
    return Entry{gfsim::UInt<2>{0}, gfsim::UInt<8>{age},
                 gfsim::UInt<6>{tag}, gfsim::UInt<6>{tag},
                 gfsim::UInt<16>{value}, gfsim::UInt<1>{false}};
  };
  auto readiness = [](unsigned tag, bool ready = true) {
    return Readiness{gfsim::UInt<6>{tag}, gfsim::UInt<1>{ready}};
  };
  auto advance = [&]() {
    if (scanSystem.currentEpoch() != incrementalSystem.currentEpoch())
      return false;
    const bool scanAdvanced = scanSystem.step();
    const bool incrementalAdvanced = incrementalSystem.step();
    if (!scanAdvanced || !incrementalAdvanced) {
      std::cerr << scanSystem.terminationResult().diagnosticCode << "/"
                << incrementalSystem.terminationResult().diagnosticCode
                << std::endl;
      return false;
    }
    return isqSnapshot(scanRows) == isqSnapshot(incrementalRows) &&
           scanSystem.commitTimeline() == incrementalSystem.commitTimeline();
  };
  auto wait = [&](auto predicate) {
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
      if (predicate())
        return true;
      if (!advance())
        return false;
    }
    return false;
  };
  auto &leftEntries = objectAs<gfsim::SimTable<Entry>>(scanRows[9]);
  auto &leftReady =
      objectAs<gfsim::SimTable<gfsim::UInt<1>>>(scanRows[10]);
  auto &rightEntries = objectAs<gfsim::SimTable<Entry>>(scanRows[14]);
  auto &rightReady =
      objectAs<gfsim::SimTable<gfsim::UInt<1>>>(scanRows[15]);
  auto validCount = [](const auto &table) {
    unsigned count = 0;
    for (size_t index = 0; index < table.size(); ++index)
      count += static_cast<bool>(table.at(index).valid);
    return count;
  };
  auto offerLeftRequest = [&](Entry value) {
    return scanModel.offer_left_request(scanSystem, value) &&
           incrementalModel.offer_left_request(incrementalSystem, value);
  };
  auto offerLeftReadiness = [&](Readiness value) {
    return scanModel.offer_left_readiness(scanSystem, value) &&
           incrementalModel.offer_left_readiness(incrementalSystem, value);
  };
  auto commitLeftReadinessForRuleCompetition = [&](Readiness value) {
    if (!scanModel.left_readiness().proposePush(value) ||
        !incrementalModel.left_readiness().proposePush(value))
      return false;
    const gfsim::Epoch epoch = scanSystem.currentEpoch();
    if (epoch != incrementalSystem.currentEpoch())
      return false;
    scanModel.left_readiness().doXfer(epoch);
    incrementalModel.left_readiness().doXfer(epoch);
    return scanSystem.scheduleWork(6, epoch) &&
           incrementalSystem.scheduleWork(6, epoch);
  };
  auto offerRightRequest = [&](Entry value) {
    return scanModel.offer_right_request(scanSystem, value) &&
           incrementalModel.offer_right_request(incrementalSystem, value);
  };
  auto offerRightReadiness = [&](Readiness value) {
    return scanModel.offer_right_readiness(scanSystem, value) &&
           incrementalModel.offer_right_readiness(incrementalSystem, value);
  };
  auto takeLeft = [&]() -> std::optional<Entry> {
    auto scan = scanModel.try_take_result_0(scanSystem);
    auto incremental =
        incrementalModel.try_take_result_0(incrementalSystem);
    if (!scan || !incremental || *scan != *incremental)
      return std::nullopt;
    return scan;
  };
  auto takeRight = [&]() -> std::optional<Entry> {
    auto scan = scanModel.try_take_result_1(scanSystem);
    auto incremental =
        incrementalModel.try_take_result_1(incrementalSystem);
    if (!scan || !incremental || *scan != *incremental)
      return std::nullopt;
    return scan;
  };
  Model &model = scanModel;

  // A readiness update and dispatch in the same committed epoch must be
  // reconciled by next-epoch activation, independent of Work order.
  if (!offerLeftReadiness(readiness(1)) ||
      !offerLeftRequest(entry(1, 1, 100)) ||
      !advance() ||
      !wait([&] { return !model.result_0().isEmpty(); }))
    return 2;
  if (static_cast<unsigned long long>(
          model.result_0().committedValues().front().value) != 100)
    return 3;

  // A previously committed readiness bit is observed by a later dispatch.
  if (!offerLeftReadiness(readiness(5)) ||
      !advance() ||
      !wait([&] { return static_cast<bool>(leftReady.at(5)); }) ||
      !offerLeftRequest(entry(3, 5, 300)) ||
      !advance() ||
      !wait([&] { return model.left_request().isEmpty(); }))
    return 4;

  // Same-epoch readiness plus dispatch is the lost-wakeup boundary.
  if (!offerLeftReadiness(readiness(9)) ||
      !offerLeftRequest(entry(4, 9, 400)) ||
      !advance() ||
      !wait([&] {
        return model.left_readiness().isEmpty() &&
               model.left_request().isEmpty();
      }))
    return 5;

  // Resident request becomes eligible only after its readiness update.
  if (!offerLeftRequest(entry(2, 7, 200)) ||
      !advance() ||
      !wait([&] { return model.left_request().isEmpty(); }) ||
      !advance() || !advance() || static_cast<bool>(leftReady.at(7)) ||
      !offerLeftReadiness(readiness(7)) ||
      !advance() ||
      !wait([&] { return static_cast<bool>(leftReady.at(7)); }))
    return 6;

  if (!offerLeftReadiness(readiness(11)) ||
      !offerLeftRequest(entry(5, 11, 500)) ||
      !advance() ||
      !wait([&] {
        return model.left_readiness().isEmpty() &&
               model.left_request().isEmpty();
      }) ||
      !wait([&] { return validCount(leftEntries) == 4; }))
    return 7;

  // Output backpressure cannot clear any selected resident entry.
  std::array<Entry, 4> held{};
  for (size_t index = 0; index < held.size(); ++index)
    held[index] = leftEntries.at(index);
  if (!advance() || !advance() || !advance() ||
      validCount(leftEntries) != 4 || model.result_0().committedSize() != 1)
    return 8;
  for (size_t index = 0; index < leftEntries.size(); ++index)
    if (leftEntries.at(index) != held[index])
      return 9;

  // A fifth request is retained at the input boundary while all four entries
  // are occupied.
  if (!offerLeftRequest(entry(6, 1, 600)) || !advance() ||
      !advance() || !advance() || model.left_request().committedSize() != 1 ||
      validCount(leftEntries) != 4)
    return 10;

  // Readiness is per module instance, not shared with the reused class.
  if (!offerRightRequest(entry(1, 5, 900)) ||
      !advance() ||
      !wait([&] { return model.right_request().isEmpty(); }) || !advance() ||
      !advance() || !model.result_1().isEmpty() ||
      validCount(rightEntries) != 1 || static_cast<bool>(rightReady.at(5)))
    return 11;
  if (!offerRightReadiness(readiness(5)) ||
      !advance() ||
      !wait([&] { return !model.result_1().isEmpty(); }))
    return 12;
  auto right = takeRight();
  if (!right || static_cast<unsigned long long>(right->value) != 900 ||
      !advance())
    return 13;

  auto first = takeLeft();
  if (!first || static_cast<unsigned long long>(first->value) != 100 ||
      !advance())
    return 14;
  // Releasing output backpressure and clearing the oldest entry's readiness
  // in the same epoch must invalidate the issue candidate computed from the
  // old ready bit. The ready writer commits; entries and output do not.
  const size_t clearTimelineStart = scanSystem.commitTimeline().size();
  if (!commitLeftReadinessForRuleCompetition(readiness(7, false)))
    return 30;
  if (!advance())
    return 38;
  if (static_cast<bool>(leftReady.at(7)))
    return 39;
  if (!model.result_0().isEmpty())
    return 40;
  if (validCount(leftEntries) != 4)
    return 41;
  const auto &clearTimeline = scanSystem.commitTimeline();
  if (std::any_of(clearTimeline.begin() + clearTimelineStart,
                  clearTimeline.end(), [](const gfsim::CommitEvent &event) {
                    return event.objectId == 9;
                  }))
    return 31;
  if (!commitLeftReadinessForRuleCompetition(readiness(7)) || !advance() ||
      !model.result_0().isEmpty())
    return 32;
  constexpr std::array<unsigned, 5> expected{200, 300, 400, 500, 600};
  for (size_t position = 0; position < expected.size(); ++position) {
    const unsigned value = expected[position];
    if (!wait([&] { return !model.result_0().isEmpty(); }))
      return 15;
    if (position == 0) {
      if (!advance() || !advance() || validCount(leftEntries) != 4)
        return 16;
    }
    auto issued = takeLeft();
    if (!issued || static_cast<unsigned long long>(issued->value) != value ||
        !advance())
      return 17;
  }
  // A reused physical tag can be cleared before dispatch. The stale ready bit
  // must not leak into the new request, and a later true update releases it.
  if (!offerLeftReadiness(readiness(12)) || !advance() ||
      !wait([&] { return static_cast<bool>(leftReady.at(12)); }) ||
      !offerLeftReadiness(readiness(12, false)) || !advance() ||
      !wait([&] { return !static_cast<bool>(leftReady.at(12)); }) ||
      !offerLeftRequest(entry(7, 12, 700)) || !advance() ||
      !wait([&] { return model.left_request().isEmpty(); }) || !advance() ||
      !advance() || !model.result_0().isEmpty() || validCount(leftEntries) != 1)
    return 18;
  if (!offerLeftReadiness(readiness(12)) || !advance() ||
      !wait([&] { return !model.result_0().isEmpty(); }))
    return 19;
  auto reused = takeLeft();
  if (!reused || static_cast<unsigned long long>(reused->value) != 700 ||
      !advance())
    return 20;

  // Accumulate four ready residents behind one full output. Their duplicated
  // src0/src1 tags include bit 63. Once the output drains, an unrelated
  // readiness write is committed on every issue opportunity; the exact
  // snapshot set must allow finite progress instead of taking a full-table
  // read lock.
  constexpr std::array<unsigned, 4> stressTags{20, 21, 22, 63};
  for (size_t index = 0; index < stressTags.size(); ++index) {
    const unsigned tag = stressTags[index];
    if (!offerLeftReadiness(readiness(tag)) || !advance() ||
        !wait([&] { return static_cast<bool>(leftReady.at(tag)); }) ||
        !offerLeftRequest(entry(20 + index, tag, 800 + index)) ||
        !advance() || !wait([&] { return model.left_request().isEmpty(); }))
      return 33;
  }
  if (model.result_0().committedSize() != 1 ||
      validCount(leftEntries) != 3)
    return 34;
  auto stressFirst = takeLeft();
  if (!stressFirst ||
      static_cast<unsigned long long>(stressFirst->value) != 800 || !advance())
    return 35;
  for (size_t index = 0; index < 3; ++index) {
    if (!offerLeftReadiness(readiness(40 + index, (index & 1) != 0)) ||
        !advance() || model.result_0().isEmpty())
      return 36;
    auto issued = takeLeft();
    if (!issued || static_cast<unsigned long long>(issued->value) != 801 + index ||
        !advance())
      return 37;
  }

  if (validCount(leftEntries) != 0 || validCount(rightEntries) != 0 ||
      !model.left_request().isEmpty() ||
      incrementalSystem.activationTraversalCount() == 0 ||
      incrementalSystem.workClosureTraversalCount() == 0 ||
      incrementalSystem.workInvocationCount() * 3 >=
          scanSystem.workInvocationCount())
    return 21;
  std::cout << "scan_work=" << scanSystem.workInvocationCount()
            << " incremental_work="
            << incrementalSystem.workInvocationCount() << " activation="
            << incrementalSystem.activationTraversalCount() << " closure="
            << incrementalSystem.workClosureTraversalCount() << std::endl;
  return 0;
}
""".replace("__MODEL__", model.name),
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/gfsim/libgfsim.a"),
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
            self.assertEqual(
                "scan_work=1377 incremental_work=200 activation=162 closure=238\n",
                executed.stdout,
            )

    def test_host_result_boundary_preserves_real_rob_backpressure(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native host-result ROB tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "host_result_rob.cpp"
            acir = root / "host_result_rob.frozen.mlir"
            plan = root / "host_result_rob.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(REUSABLE_CIRCULAR_ROB_SOURCE),
                    "--system",
                    "reusable_circular_rob",
                    "--host-results",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn("ac.module @Top() -> (", frozen)
            self.assertNotIn("ac.sink", frozen)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(4, len(parsed_plan["interface_outputs"]))
            self.assertFalse(
                any(block["kind"] == "sink" for block in parsed_plan["blocks"])
            )
            generated_source = model.read_text(encoding="utf-8")
            self.assertIn("try_take_result_0", generated_source)
            self.assertIn("scheduleExternalXfer", generated_source)

            harness = root / "harness.cpp"
            executable = root / "host_result_rob"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::ReusableCircularRob model;
  gfsim::SimSystem system("host_result_rob");
  auto rows = model.dispatch_rows();
  constexpr auto activation_offsets =
      ac_generated::ReusableCircularRob::activation_offsets();
  constexpr auto activation_targets =
      ac_generated::ReusableCircularRob::activation_targets();
  constexpr auto closure_offsets =
      ac_generated::ReusableCircularRob::work_closure_offsets();
  constexpr auto closure_targets =
      ac_generated::ReusableCircularRob::work_closure_targets();
  if (!system.setDispatchTable(rows) ||
      !system.setActivationPlan(activation_offsets, activation_targets) ||
      !system.setWorkClosurePlan(closure_offsets, closure_targets) ||
      !ac_generated::ReusableCircularRob::schedule_initial_work(system))
    return 1;
  auto event = [](unsigned value) {{
    return ac_generated::RobEvent{{gfsim::UInt<2>{{0}},
                                  gfsim::UInt<16>{{0}},
                                  gfsim::UInt<16>{{0}},
                                  gfsim::UInt<16>{{value}},
                                  gfsim::UInt<1>{{false}}}};
  }};
  if (!model.offer_left_allocate(system, event(100)))
    return 2;
  for (unsigned step = 0; step < 16 && model.result_0().isEmpty(); ++step)
    if (!system.step())
      return 3;
  if (model.result_0().committedSize() != 1)
    return 4;

  if (!model.offer_left_allocate(system, event(300)))
    return 5;
  if (!system.step())
    return 6;
  if (model.left_allocate().committedSize() != 1 ||
      model.result_0().committedSize() != 1)
    return 7;

  auto first = model.try_take_result_0(system);
  if (!first || static_cast<unsigned long long>(first->index) != 0 ||
      static_cast<unsigned long long>(first->value) != 100)
    return 8;
  if (!system.step())
    return 9;
  for (unsigned step = 0; step < 16 && model.result_0().isEmpty(); ++step)
    if (!system.step())
      return 10;
  auto second = model.try_take_result_0(system);
  if (!second || static_cast<unsigned long long>(second->index) != 1 ||
      static_cast<unsigned long long>(second->value) != 300)
    return 11;
  system.step();
  if (!model.left_allocate().isEmpty() || !model.result_0().isEmpty())
    return 12;
  return 0;
}}
''',
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/gfsim/libgfsim.a"),
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

    def test_stateful_multi_input_rule_commits_all_resources_together(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native stateful multi-input rule tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "stateful_multi_input.cpp"
            acir = root / "stateful_multi_input.frozen.mlir"
            plan = root / "stateful_multi_input.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(STATEFUL_MULTI_INPUT_RULE_SOURCE),
                    "--system",
                    "table_multi_input_rule",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn(
                "kind = #ac<rule_check_kind input_available>, ordinal = 0",
                frozen,
            )
            self.assertIn(
                "kind = #ac<rule_check_kind input_available>, ordinal = 1",
                frozen,
            )
            self.assertIn(
                "kind = #ac<rule_check_kind output_capacity>, ordinal = 0",
                frozen,
            )
            self.assertIn(
                "std::tuple<Entry, Delta>",
                model.read_text(encoding="utf-8"),
            )

            harness = root / "harness.cpp"
            executable = root / "stateful_multi_input"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::TableMultiInputRule model;
  auto rows = model.dispatch_rows();
  auto run = [&](unsigned tick, bool run_sink) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (std::size_t index = 0; index < rows.size(); ++index)
      if (run_sink || index + 1 != rows.size())
        rows[index].work(rows[index].object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }};

  if (!model.incoming().proposePush(
          ac_generated::Entry{{gfsim::UInt<1>{{1}}, gfsim::UInt<7>{{40}}}}))
    return 1;
  model.incoming().doXfer({{0, 0}});
  run(1, false);
  if (model.incoming().committedSize() != 1 ||
      static_cast<unsigned long long>(model.table_rob().at(1).value) != 0)
    return 2;

  if (!model.deltas().proposePush(
          ac_generated::Delta{{gfsim::UInt<7>{{2}}}}))
    return 3;
  model.deltas().doXfer({{2, 0}});
  run(3, false);
  if (!model.incoming().isEmpty() || !model.deltas().isEmpty() ||
      static_cast<unsigned long long>(model.table_rob().at(1).value) != 42)
    return 4;

  if (!model.incoming().proposePush(
          ac_generated::Entry{{gfsim::UInt<1>{{1}}, gfsim::UInt<7>{{8}}}}) ||
      !model.deltas().proposePush(ac_generated::Delta{{gfsim::UInt<7>{{1}}}}))
    return 5;
  model.incoming().doXfer({{4, 0}});
  model.deltas().doXfer({{4, 0}});
  run(5, false);
  if (model.incoming().committedSize() != 1 ||
      model.deltas().committedSize() != 1 ||
      static_cast<unsigned long long>(model.table_rob().at(1).value) != 42)
    return 6;

  run(6, true);
  run(7, true);
  run(8, true);
  if (!model.incoming().isEmpty() || !model.deltas().isEmpty() ||
      static_cast<unsigned long long>(model.table_rob().at(1).value) != 9)
    return 7;
  const auto &values = model.sink_0_values();
  if (values.size() != 2 ||
      static_cast<unsigned long long>(values[0].value) != 0 ||
      static_cast<unsigned long long>(values[1].value) != 42)
    return 8;
  return 0;
}}
''',
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

    def test_multi_input_rule_infers_atomic_gfsim_backpressure(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        missing = [name for name, path in tools.items() if not path.is_file()]
        if missing:
            self.skipTest(
                "native multi-input rule tools are unavailable: " + ", ".join(missing)
            )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "multi_input.cpp"
            acir = root / "multi_input.frozen.mlir"
            plan = root / "multi_input.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(MULTI_INPUT_RULE_SOURCE),
                    "--system",
                    "pyc_multi_input_rule_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            lowered = acir.read_text(encoding="utf-8")
            self.assertIn(
                "kind = #ac<rule_check_kind input_available>, ordinal = 0",
                lowered,
            )
            self.assertIn(
                "kind = #ac<rule_check_kind input_available>, ordinal = 1",
                lowered,
            )
            self.assertIn(
                "kind = #ac<rule_check_kind output_capacity>, ordinal = 0",
                lowered,
            )
            self.assertNotIn("ac.rule ", lowered)
            generated_cpp = model.read_text(encoding="utf-8")
            self.assertIn("gfsim::QueueAtomicTransform", generated_cpp)

            harness = root / "harness.cpp"
            executable = root / "multi_input"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::PycMultiInputRulePipeline model;
  if (!model.left().proposePush(1) || !model.right().proposePush(2))
    return 1;
  model.left().doXfer({{0, 0}});
  model.right().doXfer({{0, 0}});
  if (!model.left().proposePush(3) || !model.right().proposePush(4))
    return 1;
  model.left().doXfer({{1, 0}});
  model.right().doXfer({{1, 0}});
  auto rows = model.dispatch_rows();
  auto run = [&](unsigned tick, bool run_sink) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (std::size_t index = 0; index < rows.size(); ++index)
      if (run_sink || index + 1 != rows.size())
        rows[index].work(rows[index].object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }};
  run(2, false);
  if (model.left().committedSize() != 1 ||
      model.right().committedSize() != 1)
    return 2;
  run(3, false);
  if (model.left().committedSize() != 1 ||
      model.right().committedSize() != 1)
    return 3;
  run(4, true);
  run(5, true);
  run(6, true);
  const auto &values = model.sink_0_values();
  if (values.size() != 2 || values[0] != 3 || values[1] != 7)
    return 4;
  return 0;
}}
''',
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

    def test_stateful_table_rule_runs_through_mlir_and_grouped_gfsim(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": Path(
                os.environ.get(
                    "ACIR_OPT",
                    ROOT / ".pycircuit_out/toolchain/build/bin/acir-opt-internal",
                )
            ),
            "plan": Path(
                os.environ.get(
                    "ACIR_QUEUE_PLAN",
                    ROOT / ".pycircuit_out/toolchain/build/bin/acir-queue-plan",
                )
            ),
            "cxxgen": Path(
                os.environ.get(
                    "ACIR_QUEUE_CXXGEN",
                    ROOT / ".pycircuit_out/toolchain/build/bin/acir-queue-cxxgen",
                )
            ),
        }
        missing = [name for name, path in tools.items() if not path.is_file()]
        if missing:
            self.skipTest(
                "native stateful-rule tools are unavailable: " + ", ".join(missing)
            )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "table_rule.cpp"
            acir = root / "table_rule.frozen.mlir"
            plan = root / "table_rule.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(STATEFUL_RULE_SOURCE),
                    "--system",
                    "table_rule",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            lowered = acir.read_text(encoding="utf-8")
            self.assertNotIn("ac.rule ", lowered)
            self.assertNotIn("ac.marker.", lowered)
            self.assertIn("ac.firing ", lowered)
            self.assertIn("ac.table.propose @rob", lowered)
            self.assertIn("kind = #ac<rule_check_kind input_available>", lowered)
            self.assertIn("kind = #ac<rule_check_kind output_capacity>", lowered)
            self.assertIn("ac.transaction_resources", lowered)
            document = json.loads(plan.read_text(encoding="utf-8"))
            firing = next(
                block for block in document["blocks"] if block["kind"] == "firing"
            )
            self.assertEqual("rob", firing["table"])
            self.assertEqual("replace", firing["write_mode"])
            self.assertEqual(["index", "value"], firing["write_fields"])

            harness = root / "harness.cpp"
            executable = root / "table_rule"
            harness.write_text(
                f'''#include "{model.name}"
#include <array>
#include <cstddef>

int main() {{
  ac_generated::TableRule model;
  const std::array<ac_generated::Entry, 2> input{{
      ac_generated::Entry{{gfsim::UInt<1>{{1}}, gfsim::UInt<7>{{42}}}},
      ac_generated::Entry{{gfsim::UInt<1>{{1}}, gfsim::UInt<7>{{9}}}},
  }};
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 6; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    if (tick < input.size() && !model.incoming().proposePush(input[tick]))
      return 1;
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
    if (tick == 3 &&
        (static_cast<unsigned long long>(model.table_rob().at(1).value) != 42 ||
         model.incoming().committedSize() != 1))
      return 4;
  }}
  const auto &stored = model.table_rob().at(1);
  if (static_cast<unsigned long long>(stored.value) != 9)
    return 2;
  const auto &observed = model.sink_0_values();
  if (observed.size() != 2 ||
      static_cast<unsigned long long>(observed[0].value) != 0 ||
      static_cast<unsigned long long>(observed[1].value) != 42)
    return 3;
  return 0;
}}
''',
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

    def test_rule_retirement_demo_runs_through_mlir_passes_and_gfsim(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        missing = [name for name, path in tools.items() if not path.is_file()]
        if missing:
            self.skipTest(
                "native QueueGraph tools are unavailable: " + ", ".join(missing)
            )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "rob.cpp"
            acir = root / "rob.ac.mlir"
            plan = root / "rob.queue-plan.json"
            raw = root / "rob.raw.ac.mlir"
            from agentic_circuit._queue_frontend import lower_queue_source

            raw.write_text(
                lower_queue_source(RULE_ROB_SOURCE.read_text(encoding="utf-8"), "rob"),
                encoding="utf-8",
            )
            rejected = subprocess.run(
                (str(tools["plan"]), str(raw)),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(0, rejected.returncode)
            self.assertIn("unresolved rule or typed marker", rejected.stderr)
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(RULE_ROB_SOURCE),
                    "--system",
                    "rob",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": os.pathsep.join(
                        (
                            str(ROOT / "python/semantic-core/src"),
                            str(ROOT / "python/agentic-circuit/src"),
                        )
                    ),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            lowered = acir.read_text(encoding="utf-8")
            self.assertNotIn("ac.rule ", lowered)
            self.assertNotIn("ac.firing ", lowered)
            self.assertNotIn("ac.marker.", lowered)
            self.assertIn("ac.topology_frozen = true", lowered)
            self.assertIn('ac.rule_stable_id = "completed"', lowered)
            self.assertIn('ac.rule_time_domain = "cycle"', lowered)

            harness = root / "harness.cpp"
            executable = root / "rob"
            harness.write_text(
                f'''#include "{model.name}"
#include <array>
#include <cstddef>

int main() {{
  ac_generated::Rob model;
  const std::array<ac_generated::Entry, 3> input{{
      ac_generated::Entry{{2, 20, false}},
      ac_generated::Entry{{0, 10, false}},
      ac_generated::Entry{{1, 15, false}},
  }};
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 24; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    if (tick < input.size() && !model.issued().proposePush(input[tick]))
      return 1;
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &retired = model.sink_0_values();
  if (retired.size() != input.size())
    return 2;
  const std::array<unsigned, 3> values{{10, 15, 20}};
  for (std::size_t index = 0; index < retired.size(); ++index)
    if (retired[index].sequence != index || !retired[index].done ||
        retired[index].value != values[index])
      return 3;
  return 0;
}}
''',
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

    def test_reorder_python_generates_and_runs_typed_cpp(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "reorder.cpp"
            acir = root / "reorder.ac.mlir"
            plan = root / "reorder.queue-plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(REORDER_SOURCE),
                    "--system",
                    "pyc_reorder_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt"),
                    "--queue-plan-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan"),
                    "--queue-cxxgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen"),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            content = model.read_text(encoding="utf-8")
            self.assertIn("gfsim::QueueReorder<Token", content)

            harness = root / "harness.cpp"
            executable = root / "reorder"
            harness.write_text(
                f"""#include "{model.name}"
#include <array>
#include <cstddef>

int main() {{
  ac_generated::PycReorderPipeline model;
  const std::array<ac_generated::Token, 3> input{{
      ac_generated::Token{{2, 20}},
      ac_generated::Token{{0, 0}},
      ac_generated::Token{{1, 10}},
  }};
  for (std::size_t index = 0; index < input.size(); ++index) {{
    const auto &item = input[index];
    if (!model.completed().proposePush(item))
      return 1;
    model.completed().doXfer({{index, 0}});
  }}
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 3; tick < 20; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &values = model.sink_0_values();
  if (values.size() != 3)
    return 2;
  for (std::size_t index = 0; index < values.size(); ++index)
    if (values[index].sequence != index)
      return 3;
  return 0;
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

    def test_serial_runtime_if_generates_and_runs_common_blocks(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "conditional.cpp"
            acir = root / "conditional.ac.mlir"
            plan = root / "conditional.queue-plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(CONDITIONAL_SOURCE),
                    "--system",
                    "pyc_conditional_pipeline",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt"),
                    "--queue-plan-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan"),
                    "--queue-cxxgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen"),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            content = model.read_text(encoding="utf-8")
            self.assertIn("gfsim::QueueRoute<Item, 2", content)
            self.assertIn("gfsim::QueueTransform<Item, Item", content)
            self.assertIn("gfsim::QueueMerge<Item, 2>", content)
            plan_document = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual("0.5", plan_document["contract_epoch"])

            harness = root / "harness.cpp"
            executable = root / "conditional"
            harness.write_text(
                f"""#include "{model.name}"
#include <array>
#include <cstddef>

int main() {{
  ac_generated::PycConditionalPipeline model;
  if (!model.input_queue().proposePush(ac_generated::Item{{1, 0}}))
    return 1;
  model.input_queue().doXfer({{0, 0}});
  if (!model.input_queue().proposePush(ac_generated::Item{{1, 1}}))
    return 1;
  model.input_queue().doXfer({{1, 0}});
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 2; tick < 16; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto values = model.sink_0_values();
  if (values.size() != 2)
    return 2;
  return values[0].value == 11 && values[1].value == 21 ? 0 : 3;
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

    def test_native_frozen_acir_codegen_covers_common_state_blocks(self) -> None:
        from python_frontend.test_queue_codegen import (
            BROADCAST_SOURCE,
            FEEDBACK_SOURCE,
        )
        from python_frontend.test_queue_frontend import (
            BARRIER_SOURCE,
            CREDIT_SOURCE,
            EXPECT_SOURCE,
            LOOP_CONTROL_SOURCE,
            MEMORY_SOURCE,
            RECURSION_SOURCE,
            SELECT_SOURCE,
        )

        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, source, expected in (
                ("barrier", BARRIER_SOURCE, "gfsim::QueueBarrier"),
                ("broadcast", BROADCAST_SOURCE, "gfsim::QueueBroadcast"),
                ("credit", CREDIT_SOURCE, "gfsim::QueueCredit"),
                ("expect", EXPECT_SOURCE, "gfsim::QueueExpect"),
                ("feedback", FEEDBACK_SOURCE, "gfsim::QueueFeedback"),
                ("loop_control", LOOP_CONTROL_SOURCE, "gfsim::QueueFeedback"),
                ("memory", MEMORY_SOURCE, "gfsim::QueueMemory"),
                ("recursion", RECURSION_SOURCE, "gfsim::QueueTransform"),
                ("select", SELECT_SOURCE, "gfsim::QueueSelect"),
            ):
                python = root / f"{name}.py"
                model = root / f"{name}.cpp"
                acir = root / f"{name}.ac.mlir"
                plan = root / f"{name}.queue-plan.json"
                python.write_text(source, encoding="utf-8")
                generated = subprocess.run(
                    (
                        str(ROOT / "compiler/acir/tools" / "ac-queue-cxxgen.py"),
                        str(python),
                        "--system",
                        "pipeline",
                        "--acir-output",
                        str(acir),
                        "--plan-output",
                        str(plan),
                        "--acir-opt",
                        str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt"),
                        "--queue-plan-tool",
                        str(
                            ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan"
                        ),
                        "--queue-cxxgen-tool",
                        str(
                            ROOT
                            / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen"
                        ),
                        "-o",
                        str(model),
                    ),
                    cwd=ROOT,
                    env={
                        **os.environ,
                        "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                    },
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(0, generated.returncode, generated.stderr)
                self.assertIn(expected, model.read_text(encoding="utf-8"))
                compiled = subprocess.run(
                    (
                        compiler,
                        "-std=c++20",
                        "-I",
                        str(ROOT / "simulator/gfsim/include"),
                        "-fsyntax-only",
                        str(model),
                    ),
                    cwd=root,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(0, compiled.returncode, compiled.stderr)

    def test_davincioo_like_python_generates_and_runs_typed_cpp(self) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        if not DAVINCIOO_TRACE.is_file() or not DAVINCIOO_PROJECTION.is_file():
            self.skipTest("DavinciOO reference trace fixture is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "model.cpp"
            acir = root / "model.ac.mlir"
            plan = root / "model.queue-plan.json"
            harness = root / "harness.cpp"
            executable = root / "model"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools" / "ac-queue-cxxgen.py"),
                    str(SOURCE),
                    "--system",
                    "davincioo_queue_model",
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(
                        ROOT
                        / ".pycircuit_out"
                        / "acir"
                        / "dev-llvm22"
                        / "bin"
                        / "acir-opt"
                    ),
                    "--queue-plan-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan"),
                    "--queue-cxxgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen"),
                    "-o",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            content = model.read_text(encoding="utf-8")
            plan_document = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual("davincioo_queue_model", plan_document["system"])
            self.assertEqual(8, len(plan_document["scopes"]))
            self.assertEqual(14, len(plan_document["queues"]))
            self.assertEqual(16, len(plan_document["blocks"]))
            copied_source = root / "copied_model.py"
            copied_model = root / "copied_model.cpp"
            copied_acir = root / "copied_model.ac.mlir"
            copied_plan = root / "copied_model.queue-plan.json"
            shutil.copyfile(SOURCE, copied_source)
            regenerated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools" / "ac-queue-cxxgen.py"),
                    str(copied_source),
                    "--system",
                    "davincioo_queue_model",
                    "--acir-output",
                    str(copied_acir),
                    "--plan-output",
                    str(copied_plan),
                    "--acir-opt",
                    str(
                        ROOT
                        / ".pycircuit_out"
                        / "acir"
                        / "dev-llvm22"
                        / "bin"
                        / "acir-opt"
                    ),
                    "--queue-plan-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan"),
                    "--queue-cxxgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen"),
                    "-o",
                    str(copied_model),
                ),
                cwd=root,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, regenerated.returncode, regenerated.stderr)
            self.assertEqual(content, copied_model.read_text(encoding="utf-8"))
            for scope in (
                "frontend",
                "dependency",
                "dispatch",
                "scalar_engine",
                "vector_engine",
                "cube_engine",
                "tma_engine",
                "retire",
            ):
                self.assertIn(f'("{scope}", gfsim::kInvalidObjectId', content)
            self.assertIn("gfsim::QueueRoute<WorkItem, 4", content)
            self.assertIn("gfsim::QueueMerge<WorkItem, 4>", content)
            self.assertNotIn("gfsim::QueueFeedback<WorkItem", content)
            self.assertIn("gfsim::QueueDependency<WorkItem", content)
            self.assertIn("gfsim::QueueReorder<WorkItem", content)

            records = [
                json.loads(line)
                for line in DAVINCIOO_TRACE.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]
            self.assertEqual(list(range(15)), [row["sequence_id"] for row in records])
            projection = json.loads(DAVINCIOO_PROJECTION.read_text(encoding="utf-8"))
            opcode_ids = projection["opcode_ids"]
            model_cost = projection["model_cost"]
            routes = projection["routes"]
            items = [
                (
                    row["sequence_id"],
                    opcode_ids[row["opcode"]],
                    routes[row["opcode"]],
                    projection["waits_for"][row["sequence_id"]],
                    model_cost[row["opcode"]],
                    row["sequence_id"] * 10,
                )
                for row in records
            ]
            input_rows = ",\n      ".join(
                "ac_generated::WorkItem{" + ", ".join(map(str, item)) + "}"
                for item in items
            )
            expected_counts = [0] * len(opcode_ids)
            for opcode, count in projection["opcode_counts"].items():
                expected_counts[opcode_ids[opcode]] = count
            expected_counts_text = ", ".join(map(str, expected_counts))
            completion_order_text = ", ".join(map(str, projection["completion_order"]))
            architectural_values_text = ", ".join(
                map(str, projection["architectural_values"])
            )
            occupancy = projection["occupancy_projection"]
            resource_peaks_text = ", ".join(
                map(str, occupancy["resource_executing_peaks"])
            )

            oracle_summary = root / "oracle-summary.json"
            oracle = subprocess.run(
                (
                    str(
                        ROOT
                        / ".pycircuit_out/acir/dev-llvm22/bin/davincioo-gfsim-reference"
                    ),
                    "simulate",
                    "--trace",
                    str(DAVINCIOO_TRACE),
                    "--summary-out",
                    str(oracle_summary),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, oracle.returncode, oracle.stderr)
            oracle_document = json.loads(oracle_summary.read_text(encoding="utf-8"))
            self.assertEqual(15, oracle_document["record_count"])
            self.assertEqual(
                projection["simulated_cycles"],
                oracle_document["simulated_cycles"],
            )
            self.assertEqual(
                projection["opcode_counts"], oracle_document["opcode_counts"]
            )

            harness.write_text(
                f"""#include "{model.name}"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

int main() {{
  ac_generated::DavinciooQueueModel model;
  const std::array<ac_generated::WorkItem, 15> input{{
      {input_rows},
  }};
  for (const auto &item : input)
    if (!model.trace().proposePush(item))
      return 1;
  auto rows = model.dispatch_rows();
  std::size_t simulatedCycles = 0;
  std::size_t dependencyPeak = 0;
  std::size_t reorderPeak = 0;
  std::array<std::size_t, 4> resourcePeaks{{}};
  for (std::size_t tick = 0; tick < 600; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
    dependencyPeak = std::max(dependencyPeak, model.dependency_0_active());
    reorderPeak = std::max(reorderPeak, model.reorder_0_active());
    for (std::size_t resource = 0; resource < resourcePeaks.size(); ++resource)
      resourcePeaks[resource] = std::max(
          resourcePeaks[resource],
          model.dependency_0_resource_active(resource));
    if (model.sink_0_values().size() == input.size()) {{
      simulatedCycles = tick + 1;
      break;
    }}
  }}
  if (simulatedCycles != {projection["simulated_cycles"]}) {{
    std::cerr << "simulated_cycles=" << simulatedCycles << "\\n";
    return 2;
  }}
  const auto &values = model.sink_0_values();
  if (values.size() != input.size())
    return 3;
  std::array<std::size_t, 8> opcodeCounts{{}};
  const std::array<std::int64_t, 15> architecturalValues{{
      {architectural_values_text}}};
  for (std::size_t index = 0; index < values.size(); ++index) {{
    const auto &value = values[index];
    if (value.sequence_id != index || value.opcode != input[index].opcode ||
        value.waits_for != input[index].waits_for ||
        value.cycles != input[index].cycles)
      return 4;
    if (value.value != architecturalValues[index])
      return 5;
    ++opcodeCounts[value.opcode];
  }}
  const std::array<std::size_t, 8> expectedCounts{{{expected_counts_text}}};
  if (opcodeCounts != expectedCounts)
    return 6;
  const std::array<std::uint8_t, 15> completionOrder{{
      {completion_order_text}}};
  const auto &completed = model.observation_0_values();
  if (completed.size() != completionOrder.size())
    return 7;
  for (std::size_t index = 0; index < completed.size(); ++index)
    if (completed[index].sequence_id != completionOrder[index])
      return 8;
  if (dependencyPeak != {occupancy["dependency_window_peak"]} ||
      reorderPeak != {occupancy["reorder_window_peak"]})
    return 9;
  const std::array<std::size_t, 4> expectedResourcePeaks{{
      {resource_peaks_text}}};
  if (resourcePeaks != expectedResourcePeaks)
    return 10;
  return 0;
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

    def _assert_typed_module_example(
        self,
        source: Path,
        system: str,
        class_name: str,
        *,
        nested: bool,
    ) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native module tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "inferred_module.cpp"
            acir = root / "inferred_module.frozen.mlir"
            plan = root / "inferred_module.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(source),
                    "--system",
                    system,
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertIn("ac.module @increment", frozen)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(1, len(parsed_plan["module_specializations"]))
            self.assertEqual(2, len(parsed_plan["module_instances"]))
            if nested:
                self.assertIn("ac.module @wrapper", frozen)
                self.assertEqual(1, frozen.count(" of @increment"))
                wrapper = parsed_plan["module_specializations"][0]
                self.assertEqual("wrapper", wrapper["definition"])
                self.assertEqual(
                    "increment", wrapper["module_specializations"][0]["definition"]
                )
            else:
                self.assertEqual(2, frozen.count(" of @increment"))
            generated_source = model.read_text(encoding="utf-8")
            self.assertIn("activation_complete() { return true; }", generated_source)

            harness = root / "harness.cpp"
            executable = root / "inferred_module"
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::{class_name} model;
  if (!model.left().proposePush(gfsim::UInt<8>{{5}}) ||
      !model.right().proposePush(gfsim::UInt<8>{{10}}))
    return 1;
  model.left().doXfer({{0, 0}});
  model.right().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick != 8; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &left = model.sink_0_values();
  const auto &right = model.sink_1_values();
  if (left.size() != 1 || left[0] != 6 || right.size() != 1 || right[0] != 11)
    return 2;

  ac_generated::{class_name} activated;
  static_assert(ac_generated::{class_name}::activation_complete());
  gfsim::SimSystem system("module_activation");
  auto active_rows = activated.dispatch_rows();
  constexpr auto offsets = ac_generated::{class_name}::activation_offsets();
  constexpr auto targets = ac_generated::{class_name}::activation_targets();
  constexpr auto closure_offsets =
      ac_generated::{class_name}::work_closure_offsets();
  constexpr auto closure_targets =
      ac_generated::{class_name}::work_closure_targets();
  if (!system.setDispatchTable(active_rows) ||
      !system.setActivationPlan(offsets, targets) ||
      !system.setWorkClosurePlan(closure_offsets, closure_targets) ||
      !ac_generated::{class_name}::schedule_initial_work(system) ||
      !activated.offer_left(system, gfsim::UInt<8>{{5}}) ||
      !activated.offer_right(system, gfsim::UInt<8>{{10}}))
    return 3;
  if (system.run().classification != gfsim::TerminationClass::Completed)
    return 4;
  const auto &active_left = activated.sink_0_values();
  const auto &active_right = activated.sink_1_values();
  return active_left.size() == 1 && active_left[0] == 6 &&
                 active_right.size() == 1 && active_right[0] == 11
             ? 0
             : 5;
}}
''',
                encoding="utf-8",
            )
            linked = subprocess.run(
                (
                    compiler,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(harness),
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/gfsim/libgfsim.a"),
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

    def test_typed_module_calls_generate_one_reusable_class_and_run(self) -> None:
        self._assert_typed_module_example(
            INFERRED_MODULE_SOURCE,
            "inferred_module_pipeline",
            "InferredModulePipeline",
            nested=False,
        )

    def test_nested_python_module_calls_preserve_reuse_and_run(self) -> None:
        self._assert_typed_module_example(
            INFERRED_NESTED_MODULE_SOURCE,
            "inferred_nested_module_pipeline",
            "InferredNestedModulePipeline",
            nested=True,
        )

    def _assert_stateful_python_module(
        self,
        source: Path,
        system: str,
        class_name: str,
        definition: str,
        table_names: tuple[str, ...],
        left_expected: tuple[int, ...],
        right_expected: tuple[int, ...],
    ) -> None:
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is unavailable")
        tools = {
            "opt": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt",
            "plan": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-plan",
            "cxxgen": ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen",
        }
        if any(not path.is_file() for path in tools.values()):
            self.skipTest("native stateful-module tools are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / f"{system}.cpp"
            acir = root / f"{system}.frozen.mlir"
            plan = root / f"{system}.plan.json"
            generated = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-cxxgen.py"),
                    str(source),
                    "--system",
                    system,
                    "--acir-output",
                    str(acir),
                    "--plan-output",
                    str(plan),
                    "--acir-opt",
                    str(tools["opt"]),
                    "--queue-plan-tool",
                    str(tools["plan"]),
                    "--queue-cxxgen-tool",
                    str(tools["cxxgen"]),
                    "--output",
                    str(model),
                ),
                cwd=ROOT,
                env={
                    **os.environ,
                    "PYTHONPATH": str(ROOT / "python/agentic-circuit/src"),
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            frozen = acir.read_text(encoding="utf-8")
            self.assertNotIn("ac.var.decl", frozen)
            for table in table_names:
                self.assertIn(f"ac.table @{table}", frozen)
            self.assertIn("kind = #ac<rule_check_kind input_available>", frozen)
            self.assertIn("kind = #ac<rule_check_kind output_capacity>", frozen)
            self.assertIn("ac.transaction_resources", frozen)
            parsed_plan = json.loads(plan.read_text(encoding="utf-8"))
            self.assertEqual(1, len(parsed_plan["module_specializations"]))
            self.assertEqual(2, len(parsed_plan["module_instances"]))
            specialization = parsed_plan["module_specializations"][0]
            self.assertEqual(definition, specialization["definition"])
            self.assertEqual(len(table_names), len(specialization["tables"]))
            firing = next(
                block for block in specialization["blocks"] if block["kind"] == "firing"
            )
            self.assertEqual(len(table_names), len(firing["state_writes"]))
            generated_source = model.read_text(encoding="utf-8")
            implementation_prefix = definition[:1].upper() + definition[1:]
            self.assertEqual(
                1, generated_source.count(f"class {implementation_prefix}_")
            )
            if len(table_names) > 1:
                self.assertIn("gfsim::QueueStateTransition<", generated_source)

            harness = root / "harness.cpp"
            executable = root / system
            left_checks = " && ".join(
                f"left[{index}] == {value}" for index, value in enumerate(left_expected)
            )
            right_checks = " && ".join(
                f"right[{index}] == {value}"
                for index, value in enumerate(right_expected)
            )
            harness.write_text(
                f'''#include "{model.name}"

int main() {{
  ac_generated::{class_name} model;
  if (!model.left().proposePush(gfsim::UInt<8>{{1}}) ||
      !model.right().proposePush(gfsim::UInt<8>{{10}}))
    return 1;
  model.left().doXfer({{0, 0}});
  model.right().doXfer({{0, 0}});
  auto rows = model.dispatch_rows();
  for (unsigned tick = 1; tick != 12; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    if (tick == 4 && !model.left().proposePush(gfsim::UInt<8>{{2}}))
      return 2;
    if (tick == 5 && model.left().committedSize() != 1)
      return 4;
    for (auto &row : rows) {{
      if (tick < 6 && row.kind == gfsim::ObjectKind::Sink)
        continue;
      row.work(row.object, epoch);
    }}
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  const auto &left = model.sink_0_values();
  const auto &right = model.sink_1_values();
  return left.size() == {len(left_expected)} && {left_checks} &&
                 right.size() == {len(right_expected)} && {right_checks}
             ? 0
             : 3;
}}
''',
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

    def test_stateful_python_module_reuses_class_with_independent_state(self) -> None:
        self._assert_stateful_python_module(
            INFERRED_STATEFUL_MODULE_SOURCE,
            "inferred_stateful_module",
            "InferredStatefulModule",
            "accumulator",
            ("total",),
            (1, 3),
            (10,),
        )

    def test_multi_state_python_module_commits_all_instance_state(self) -> None:
        self._assert_stateful_python_module(
            INFERRED_MULTI_STATE_MODULE_SOURCE,
            "inferred_multi_state_module",
            "InferredMultiStateModule",
            "tally",
            ("count", "total"),
            (2, 5),
            (11,),
        )


if __name__ == "__main__":
    unittest.main()
