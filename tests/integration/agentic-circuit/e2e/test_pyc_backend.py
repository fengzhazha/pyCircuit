from __future__ import annotations

import json
import os
import runpy
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from agentic_circuit._queue_frontend import RULE_LOWERING_PIPELINE, lower_queue_source

ROOT = Path(__file__).resolve().parents[4]
EXAMPLE = ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_queue_pipeline.py"
DAVINCIOO_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "davincioo_queue_model.py"
)
STRUCT_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_struct_pipeline.py"
)
ROUTE_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_route_merge_pipeline.py"
)
RULE_ROB_EXAMPLE = ROOT / "examples/agentic-circuit" / "state" / "rob.py"
REORDER_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_reorder_pipeline.py"
)
DEPENDENCY_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_dependency_pipeline.py"
)
MEMORY_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_memory_pipeline.py"
)
MEMORY_BANKS_EXAMPLE = ROOT / "examples/agentic-circuit" / "memory" / "memory_banks.py"
FORK_EXAMPLE = ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_fork_pipeline.py"
CONDITIONAL_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_conditional_pipeline.py"
)
FEEDBACK_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "pyc_feedback_pipeline.py"
)
BITFIELD_SCALAR_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "bitfield_scalar_pipeline.py"
)
MASKED_DECODE_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "masked_decode_pipeline.py"
)
NESTED_PAYLOAD_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "nested_payload_pipeline.py"
)
ENUM_PAYLOAD_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "enum_payload_pipeline.py"
)
AGGREGATE_PAYLOAD_EXAMPLE = (
    ROOT / "examples/agentic-circuit" / "pipelines" / "aggregate_payload_pipeline.py"
)
RECURSIVE_AGGREGATE_PAYLOAD_EXAMPLE = (
    ROOT
    / "examples/agentic-circuit"
    / "pipelines"
    / "recursive_aggregate_payload_pipeline.py"
)
PYC_REPOSITORY = ROOT
DEFAULT_TOOLCHAIN = PYC_REPOSITORY / ".pycircuit_out/toolchain/install"
DAVINCIOO_PROJECTION = (
    ROOT / "tests/goldens/agentic-circuit/davincioo/softmax-projection.json"
)
DAVINCIOO_RUN = (
    ROOT / "tests/goldens/agentic-circuit/davincioo/davincioo-softmax-run.json"
)


def _freeze_command(raw: Path) -> tuple[str, str, str]:
    return (
        str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt-internal"),
        "--pass-pipeline=builtin.module(ac-freeze-topology)",
        str(raw),
    )


class PycBackendTest(unittest.TestCase):
    def test_recursive_aggregate_payload_is_cycle_equivalent_in_pyc_cpp_and_verilog(
        self,
    ) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        pycgen = Path(
            os.environ.get(
                "ACIR_QUEUE_PYCGEN",
                ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen",
            )
        )
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not pycgen.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = RECURSIVE_AGGREGATE_PAYLOAD_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "recursive-aggregate.raw.ac.mlir"
            frozen = root / "recursive-aggregate.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "recursive_aggregate_payload_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                (
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt-internal"),
                    f"--pass-pipeline={RULE_LOWERING_PIPELINE}",
                    str(raw),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(pycgen),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertIn("%in_data: i13", pyc)
            self.assertIn("pyc.concat", pyc)
            self.assertIn("pyc.extract", pyc)
            self.assertNotIn("pyc.rtl.", pyc)

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "recursive_aggregate_payload_pipeline.hpp"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint16_t input = 2962;
  pyc::gen::recursive_aggregate_payload_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(cycle == 1 ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<13>(cycle == 1 ? input : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    if (dut.out_valid.value())
      std::cout << cycle << " " << dut.out_data.value() << "\\n";
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_sources = sorted(
                output.joinpath("cpp").glob("recursive_aggregate_payload_pipeline*.cpp")
            )
            self.assertGreater(len(cpp_sources), 0)
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    *(str(path) for path in cpp_sources),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vrecursive_aggregate_payload_pipeline.h"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint16_t input = 2962;
  Vrecursive_aggregate_payload_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = cycle == 1 ? 1 : 0;
    dut.in_data = cycle == 1 ? input : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    if (dut.out_valid)
      std::cout << cycle << " " << dut.out_data << "\\n";
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "recursive_aggregate_payload_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/recursive_aggregate_payload_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vrecursive_aggregate_payload_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            self.assertEqual("2 7125\n", cpp_run.stdout)

    def test_aggregate_payload_is_cycle_equivalent_in_pyc_cpp_and_verilog(
        self,
    ) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        pycgen = Path(
            os.environ.get(
                "ACIR_QUEUE_PYCGEN",
                ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen",
            )
        )
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not pycgen.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = AGGREGATE_PAYLOAD_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "aggregate.raw.ac.mlir"
            frozen = root / "aggregate.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "aggregate_payload_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                (
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt-internal"),
                    f"--pass-pipeline={RULE_LOWERING_PIPELINE}",
                    str(raw),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(pycgen),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertIn("%in_data: i28", pyc)
            self.assertIn("pyc.concat", pyc)
            self.assertIn("pyc.extract", pyc)

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "aggregate_payload_pipeline.hpp"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint32_t pair = (5u << 5) | 29u;
  constexpr std::uint32_t lanes = 0x1234u;
  constexpr std::uint32_t input = (pair << 20) | (lanes << 4) | 0xfu;
  pyc::gen::aggregate_payload_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(cycle == 1 ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<28>(cycle == 1 ? input : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    if (dut.out_valid.value())
      std::cout << cycle << " " << dut.out_data.value() << "\\n";
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_sources = sorted(
                output.joinpath("cpp").glob("aggregate_payload_pipeline*.cpp")
            )
            self.assertGreater(len(cpp_sources), 0)
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    *(str(path) for path in cpp_sources),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vaggregate_payload_pipeline.h"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint32_t pair = (5u << 5) | 29u;
  constexpr std::uint32_t lanes = 0x1234u;
  constexpr std::uint32_t input = (pair << 20) | (lanes << 4) | 0xfu;
  Vaggregate_payload_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = cycle == 1 ? 1 : 0;
    dut.in_data = cycle == 1 ? input : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    if (dut.out_valid)
      std::cout << cycle << " " << dut.out_data << "\\n";
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "aggregate_payload_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/aggregate_payload_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vaggregate_payload_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            expected_pair = (6 << 5) | 30
            expected = (expected_pair << 20) | (0x2341 << 4) | 3
            self.assertEqual(f"2 {expected}\n", cpp_run.stdout)

    def test_nominal_enum_is_cycle_equivalent_in_pyc_cpp_and_verilog(
        self,
    ) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = ENUM_PAYLOAD_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "enum.raw.ac.mlir"
            frozen = root / "enum.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "enum_payload_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                (
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt-internal"),
                    f"--pass-pipeline={RULE_LOWERING_PIPELINE}",
                    str(raw),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertIn("%in_data: i26", pyc)
            self.assertIn("pyc.constant 1 : i2", pyc)
            self.assertIn("pyc.constant 2 : i2", pyc)
            self.assertIn("pyc.eq", pyc)

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "enum_payload_pipeline.hpp"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint32_t input =
      (42u << 20) | (2u << 18) | (0x12345u << 1);
  pyc::gen::enum_payload_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(cycle == 1 ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<26>(cycle == 1 ? input : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    if (dut.out_valid.value())
      std::cout << cycle << " " << dut.out_data.value() << "\\n";
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_sources = sorted(
                output.joinpath("cpp").glob("enum_payload_pipeline*.cpp")
            )
            self.assertGreater(len(cpp_sources), 0)
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    *(str(path) for path in cpp_sources),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Venum_payload_pipeline.h"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint32_t input =
      (42u << 20) | (2u << 18) | (0x12345u << 1);
  Venum_payload_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = cycle == 1 ? 1 : 0;
    dut.in_data = cycle == 1 ? input : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    if (dut.out_valid)
      std::cout << cycle << " " << dut.out_data << "\\n";
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "enum_payload_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/enum_payload_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Venum_payload_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            expected = (42 << 20) | (1 << 18) | (0x12345 << 1) | 1
            self.assertEqual(f"2 {expected}\n", cpp_run.stdout)

    def test_nested_payload_is_cycle_equivalent_in_pyc_cpp_and_verilog(
        self,
    ) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = NESTED_PAYLOAD_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "nested.raw.ac.mlir"
            frozen = root / "nested.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "nested_payload_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                (
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt-internal"),
                    f"--pass-pipeline={RULE_LOWERING_PIPELINE}",
                    str(raw),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertIn("%in_data: i26", pyc)
            self.assertIn("i26 -> i9", pyc)
            self.assertIn("i9 -> i3", pyc)

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "nested_payload_pipeline.hpp"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint32_t input = (42u << 20) | (7u << 17) | 0x12345u;
  pyc::gen::nested_payload_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(cycle == 1 ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<26>(cycle == 1 ? input : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    if (dut.out_valid.value())
      std::cout << cycle << " " << dut.out_data.value() << "\\n";
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_sources = sorted(
                output.joinpath("cpp").glob("nested_payload_pipeline*.cpp")
            )
            self.assertGreater(len(cpp_sources), 0)
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    *(str(path) for path in cpp_sources),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vnested_payload_pipeline.h"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint32_t input = (42u << 20) | (7u << 17) | 0x12345u;
  Vnested_payload_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = cycle == 1 ? 1 : 0;
    dut.in_data = cycle == 1 ? input : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    if (dut.out_valid)
      std::cout << cycle << " " << dut.out_data << "\\n";
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "nested_payload_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/nested_payload_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vnested_payload_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            expected = (42 << 20) | 0x12345
            self.assertEqual(f"2 {expected}\n", cpp_run.stdout)

    def test_bitfield_scalar_is_cycle_equivalent_in_pyc_cpp_and_verilog(
        self,
    ) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = BITFIELD_SCALAR_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "bitfield.raw.ac.mlir"
            frozen = root / "bitfield.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "bitfield_scalar_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                (
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt-internal"),
                    f"--pass-pipeline={RULE_LOWERING_PIPELINE}",
                    str(raw),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertEqual(4, pyc.count("pyc.extract"))
            self.assertEqual(2, pyc.count("pyc.concat"))

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "bitfield_scalar_pipeline.hpp"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint32_t word = 0xd5a12345u;
  pyc::gen::bitfield_scalar_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(cycle == 1 ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<32>(cycle == 1 ? word : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    if (dut.out_valid.value())
      std::cout << cycle << " " << dut.out_data.value() << "\\n";
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    str(output / "cpp/bitfield_scalar_pipeline.cpp"),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vbitfield_scalar_pipeline.h"
#include <cstdint>
#include <iostream>

int main() {
  constexpr std::uint32_t word = 0xd5a12345u;
  Vbitfield_scalar_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = cycle == 1 ? 1 : 0;
    dut.in_data = cycle == 1 ? word : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    if (dut.out_valid)
      std::cout << cycle << " " << dut.out_data << "\\n";
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "bitfield_scalar_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/bitfield_scalar_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vbitfield_scalar_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            word = 0xD5A12345
            rotated = ((word & 0x1FFFF) << 15) | ((word >> 17) & 0x7FFF)
            expected = (rotated & ~0x7) | (word & 0x7)
            self.assertEqual(f"2 {expected}\n", cpp_run.stdout)

    def test_masked_decode_is_cycle_equivalent_in_pyc_cpp_and_verilog(
        self,
    ) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = MASKED_DECODE_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "masked-decode.raw.ac.mlir"
            frozen = root / "masked-decode.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "masked_decode_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                (
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt-internal"),
                    f"--pass-pipeline={RULE_LOWERING_PIPELINE}",
                    str(raw),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertEqual(1, pyc.count("pyc.and"))
            self.assertEqual(1, pyc.count("pyc.eq"))

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "masked_decode_pipeline.hpp"
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::masked_decode_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    const bool valid = cycle == 1 || cycle == 2;
    const std::uint64_t packed = cycle == 1 ? 16 : (cycle == 2 ? 18 : 0);
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(valid ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<5>(packed);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    if (dut.out_valid.value())
      std::cout << cycle << " " << dut.out_data.value() << "\\n";
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    str(output / "cpp/masked_decode_pipeline.cpp"),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vmasked_decode_pipeline.h"
#include <cstdint>
#include <iostream>

int main() {
  Vmasked_decode_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 10; ++cycle) {
    const bool valid = cycle == 1 || cycle == 2;
    const std::uint64_t packed = cycle == 1 ? 16 : (cycle == 2 ? 18 : 0);
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = valid ? 1 : 0;
    dut.in_data = packed;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    if (dut.out_valid)
      std::cout << cycle << " " << static_cast<unsigned>(dut.out_data)
                << "\\n";
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "masked_decode_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/masked_decode_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vmasked_decode_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            self.assertEqual("2 17\n3 18\n", cpp_run.stdout)

    def test_memory_array_frontend_expands_to_one_sync_mem_per_bank(self) -> None:
        acir_opt = ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-opt-internal"
        pycgen = ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"
        if not acir_opt.is_file() or not pycgen.is_file():
            self.skipTest("ACIR optimizer or Queue PYC generator is unavailable")
        source = MEMORY_BANKS_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "memory_banks.raw.ac.mlir"
            frozen = root / "memory_banks.frozen.ac.mlir"
            raw.write_text(lower_queue_source(source, "memory_banks"), encoding="utf-8")
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            generated = subprocess.run(
                (str(pycgen), str(frozen)),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, generated.returncode, generated.stderr)
            self.assertEqual(4, generated.stdout.count("pyc.sync_mem"))
            for bank in range(4):
                self.assertIn(f'name = "banks__{bank}"', generated.stdout)

    def test_bounded_feedback_is_cycle_equivalent_in_pyc_cpp_and_verilog(
        self,
    ) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = FEEDBACK_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "feedback.raw.ac.mlir"
            frozen = root / "feedback.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "pyc_feedback_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertEqual(3, pyc.count("pyc.reg"))
            self.assertIn("pyc.ult", pyc)
            self.assertIn("feedback_iteration_limit", pyc)

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "pyc_feedback_pipeline.hpp"
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::pyc_feedback_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 14; ++cycle) {
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(cycle == 1 ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<36>(cycle == 1 ? ((10ULL << 4) | 3) : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    std::cout << cycle << " " << dut.out_valid.value() << " "
              << dut.out_data.value() << " " << dut.in_ready.value() << "\\n";
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    str(output / "cpp/pyc_feedback_pipeline.cpp"),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vpyc_feedback_pipeline.h"
#include <cstdint>
#include <iostream>

int main() {
  Vpyc_feedback_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 14; ++cycle) {
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = cycle == 1 ? 1 : 0;
    dut.in_data = cycle == 1 ? ((10ULL << 4) | 3) : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    std::cout << cycle << " " << unsigned(dut.out_valid) << " "
              << dut.out_data << " " << unsigned(dut.in_ready) << "\\n";
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "pyc_feedback_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/pyc_feedback_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vpyc_feedback_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            transactions = [
                int(fields[2])
                for line in cpp_run.stdout.splitlines()
                if len(fields := line.split()) == 4 and fields[1] == "1"
            ]
            self.assertEqual([13 << 4], transactions)

            gfsim_model = root / "gfsim_model.cpp"
            gfsim_harness = root / "gfsim_harness.cpp"
            gfsim_executable = root / "gfsim_model"
            gfsim_generated = subprocess.run(
                (
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen"),
                    str(frozen),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, gfsim_generated.returncode, gfsim_generated.stderr)
            gfsim_model.write_text(gfsim_generated.stdout, encoding="utf-8")
            gfsim_harness.write_text(
                f"""#include "{gfsim_model.name}"
#include <cstddef>
#include <iostream>

int main() {{
  ac_generated::PycFeedbackPipeline model;
  if (!model.current().proposePush(ac_generated::Item{{10, 3}}))
    return 1;
  auto rows = model.dispatch_rows();
  for (std::size_t tick = 0; tick < 14; ++tick) {{
    const gfsim::Epoch epoch{{tick, 0}};
    for (auto &row : rows)
      row.work(row.object, epoch);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Arbitrate);
    for (auto &row : rows)
      row.xfer(row.object, epoch, gfsim::XferPhase::Commit);
  }}
  for (const auto &item : model.sink_0_values())
    std::cout << ((static_cast<unsigned long long>(item.value) << 4) |
                  static_cast<unsigned long long>(item.remaining)) << "\\n";
}}
""",
                encoding="utf-8",
            )
            gfsim_build = subprocess.run(
                (
                    cxx,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(gfsim_harness),
                    "-o",
                    str(gfsim_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, gfsim_build.returncode, gfsim_build.stderr)
            gfsim_run = subprocess.run(
                (str(gfsim_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, gfsim_run.returncode, gfsim_run.stderr)
            self.assertEqual(
                transactions, [int(value) for value in gfsim_run.stdout.split()]
            )

    def test_serial_runtime_if_builds_pyc_cpp_and_verilog(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = CONDITIONAL_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "conditional.raw.ac.mlir"
            frozen = root / "conditional.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "pyc_conditional_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertIn("pyc.eq", pyc)
            self.assertIn("pyc.fifo", pyc)
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual("0.5", manifest["contract_epoch"])
            verilog = "\n".join(
                path.read_text(encoding="utf-8")
                for path in sorted((output / "verilog").glob("*.v"))
            )
            self.assertIn("==", verilog)

    def test_decoupled_fork_builds_delivered_state_in_pyc(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = FORK_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "fork.raw.ac.mlir"
            frozen = root / "fork.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "pyc_fork_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual("0.5", manifest["contract_epoch"])
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertEqual(2, pyc.count("pyc.reg"))
            self.assertEqual(3, pyc.count("pyc.fifo"))
            self.assertIn("%out0_ready", pyc)
            self.assertIn("%out1_ready", pyc)

    def test_rule_retirement_builds_pyc_and_verilog(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        pycgen = Path(
            os.environ.get(
                "ACIR_QUEUE_PYCGEN",
                ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen",
            )
        )
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not pycgen.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        specialization = runpy.run_path(str(RULE_ROB_EXAMPLE))["specialization"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = specialization.materialize_pyc(
                root,
                pycgen_tool=pycgen,
                pycc=pycc,
                toolchain_metadata=metadata,
                compiler=cxx,
                verilator=verilator,
            )
            self.assertFalse(artifact.cache_hit)
            pyc = artifact.pyc.read_text(encoding="utf-8")
            self.assertIn("%in_valid", pyc)
            self.assertIn("%out_ready", pyc)
            self.assertIn('result_names = ["out_valid", "out_data", "in_ready"]', pyc)

            harness = artifact.cpp / "rule_rob_harness.cpp"
            executable = artifact.cpp / "rule_rob_harness"
            harness.write_text(
                """#include "rob.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cpp/pyc_tb.hpp>

using pyc::cpp::Testbench;

int main() {
  pyc::gen::rob dut;
  Testbench<pyc::gen::rob> tb(dut);
  tb.addClock(dut.clk, 1, 0, false);
  dut.in_valid = pyc::cpp::Wire<1>({0});
  dut.out_ready = pyc::cpp::Wire<1>({1});
  tb.reset(dut.rst, 2, 1);

  const std::array<std::uint64_t, 3> input{
      (2ull << 17) | (20ull << 1),
      (0ull << 17) | (10ull << 1),
      (1ull << 17) | (15ull << 1),
  };
  const std::array<unsigned, 3> expected_values{10, 15, 20};
  std::size_t offered = 0;
  std::size_t retired = 0;
  for (std::uint64_t cycle = 0; cycle < 64 && retired < 3; ++cycle) {
    if (offered < input.size()) {
      dut.in_valid = pyc::cpp::Wire<1>({1});
      dut.in_data = pyc::cpp::Wire<21>({input[offered]});
    } else {
      dut.in_valid = pyc::cpp::Wire<1>({0});
    }
    dut.out_ready = pyc::cpp::Wire<1>({1});
    tb.runCycleAutoTrace(cycle, nullptr);
    if (dut.in_valid.value() && dut.in_ready.value())
      ++offered;
    if (dut.out_valid.value() && dut.out_ready.value()) {
      const std::uint64_t value = dut.out_data.value();
      const unsigned sequence = static_cast<unsigned>((value >> 17) & 0xf);
      const unsigned payload = static_cast<unsigned>((value >> 1) & 0xffff);
      const bool done = (value & 1) != 0;
      if (sequence != retired || payload != expected_values[retired] || !done)
        return 2;
      ++retired;
    }
  }
  return offered == input.size() && retired == input.size() ? 0 : 3;
}
""",
                encoding="utf-8",
            )
            sources = sorted(
                path for path in artifact.cpp.glob("rob*.cpp") if path != harness
            )
            linked = subprocess.run(
                (
                    cxx,
                    "-std=c++20",
                    "-I",
                    str(artifact.cpp),
                    "-I",
                    str(toolchain / "include"),
                    str(harness),
                    *(str(path) for path in sources),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, linked.returncode, linked.stderr)
            executed = subprocess.run(
                (str(executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)

    def test_dependency_is_cycle_equivalent_in_pyc_cpp_and_verilog(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = DEPENDENCY_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "dependency.raw.ac.mlir"
            frozen = root / "dependency.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "pyc_dependency_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertEqual(4, pyc.count("pyc.reg"))
            self.assertIn("pyc.sub", pyc)
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(
                ["ac.dependency", "ac.sink", "ac.source"],
                manifest["opcode_lowering_inventory"],
            )

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "pyc_dependency_pipeline.hpp"
#include <array>
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::pyc_dependency_pipeline dut;
  const std::array<std::uint64_t, 3> input{
      (0ULL << 25) | (15ULL << 21) | (0ULL << 20) | (4ULL << 16) | 10,
      (1ULL << 25) | (15ULL << 21) | (0ULL << 20) | (1ULL << 16) | 20,
      (2ULL << 25) | (15ULL << 21) | (1ULL << 20) | (1ULL << 16) | 30};
  std::size_t cursor = 0;
  for (std::uint64_t cycle = 0; cycle < 24; ++cycle) {
    const bool offering = cycle != 0 && cursor < input.size();
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(offering ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<29>(offering ? input[cursor] : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    std::cout << cycle << " " << dut.out_valid.value() << " "
              << dut.out_data.value() << " " << dut.in_ready.value() << "\\n";
    if (offering && dut.in_ready.value() != 0)
      ++cursor;
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_sources = sorted(
                output.joinpath("cpp").glob("pyc_dependency_pipeline*.cpp")
            )
            self.assertGreater(len(cpp_sources), 0)
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    *(str(path) for path in cpp_sources),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vpyc_dependency_pipeline.h"
#include <array>
#include <cstdint>
#include <iostream>

int main() {
  Vpyc_dependency_pipeline dut;
  const std::array<std::uint64_t, 3> input{
      (0ULL << 25) | (15ULL << 21) | (0ULL << 20) | (4ULL << 16) | 10,
      (1ULL << 25) | (15ULL << 21) | (0ULL << 20) | (1ULL << 16) | 20,
      (2ULL << 25) | (15ULL << 21) | (1ULL << 20) | (1ULL << 16) | 30};
  std::size_t cursor = 0;
  for (std::uint64_t cycle = 0; cycle < 24; ++cycle) {
    const bool offering = cycle != 0 && cursor < input.size();
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = offering ? 1 : 0;
    dut.in_data = offering ? input[cursor] : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    std::cout << cycle << " " << unsigned(dut.out_valid) << " "
              << dut.out_data << " " << unsigned(dut.in_ready) << "\\n";
    if (offering && dut.in_ready != 0)
      ++cursor;
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "pyc_dependency_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/pyc_dependency_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vpyc_dependency_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            transactions = [
                int(fields[2])
                for line in cpp_run.stdout.splitlines()
                if len(fields := line.split()) == 4 and fields[1] == "1"
            ]
            self.assertEqual(
                [
                    (2 << 25) | (15 << 21) | (1 << 20) | (1 << 16) | 30,
                    (0 << 25) | (15 << 21) | (0 << 20) | (4 << 16) | 10,
                    (1 << 25) | (15 << 21) | (0 << 20) | (1 << 16) | 20,
                ],
                transactions,
            )

    def test_memory_is_old_data_and_cycle_equivalent_in_pyc_cpp_and_verilog(
        self,
    ) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = MEMORY_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "memory.raw.ac.mlir"
            frozen = root / "memory.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "pyc_memory_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertEqual(4, pyc.count("pyc.reg"))
            self.assertEqual(1, pyc.count("pyc.sync_mem"))
            self.assertIn("pyc.sub", pyc)
            self.assertIn('{depth = 16, name = "sram"}', pyc)
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(
                [
                    "ac.memory.instance",
                    "ac.memory.request",
                    "ac.sink",
                    "ac.source",
                ],
                manifest["opcode_lowering_inventory"],
            )

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "pyc_memory_pipeline.hpp"
#include <array>
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::pyc_memory_pipeline dut;
  const std::array<std::uint64_t, 4> input{
      (3ULL << 25) | (1ULL << 24) | (42ULL << 8) | 1,
      (3ULL << 25) | (0ULL << 24) | (0ULL << 8) | 2,
      (3ULL << 25) | (1ULL << 24) | (99ULL << 8) | 3,
      (3ULL << 25) | (0ULL << 24) | (0ULL << 8) | 4};
  std::size_t cursor = 0;
  for (std::uint64_t cycle = 0; cycle < 24; ++cycle) {
    const bool offering = cycle != 0 && cursor < input.size();
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(offering ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<29>(offering ? input[cursor] : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    std::cout << cycle << " " << dut.out_valid.value() << " "
              << dut.out_data.value() << " " << dut.in_ready.value() << "\\n";
    if (offering && dut.in_ready.value() != 0)
      ++cursor;
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    str(output / "cpp/pyc_memory_pipeline.cpp"),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vpyc_memory_pipeline.h"
#include <array>
#include <cstdint>
#include <iostream>

int main() {
  Vpyc_memory_pipeline dut;
  const std::array<std::uint64_t, 4> input{
      (3ULL << 25) | (1ULL << 24) | (42ULL << 8) | 1,
      (3ULL << 25) | (0ULL << 24) | (0ULL << 8) | 2,
      (3ULL << 25) | (1ULL << 24) | (99ULL << 8) | 3,
      (3ULL << 25) | (0ULL << 24) | (0ULL << 8) | 4};
  std::size_t cursor = 0;
  for (std::uint64_t cycle = 0; cycle < 24; ++cycle) {
    const bool offering = cycle != 0 && cursor < input.size();
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = offering ? 1 : 0;
    dut.in_data = offering ? input[cursor] : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    std::cout << cycle << " " << unsigned(dut.out_valid) << " "
              << dut.out_data << " " << unsigned(dut.in_ready) << "\\n";
    if (offering && dut.in_ready != 0)
      ++cursor;
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "pyc_memory_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/pyc_memory_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vpyc_memory_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            transactions = [
                int(fields[2])
                for line in cpp_run.stdout.splitlines()
                if len(fields := line.split()) == 4 and fields[1] == "1"
            ]
            self.assertEqual(
                [
                    (3 << 25) | (1 << 24) | (0 << 8) | 1,
                    (3 << 25) | (0 << 24) | (42 << 8) | 2,
                    (3 << 25) | (1 << 24) | (42 << 8) | 3,
                    (3 << 25) | (0 << 24) | (99 << 8) | 4,
                ],
                transactions,
            )

    def test_reorder_is_cycle_equivalent_in_pyc_cpp_and_verilog(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = REORDER_EXAMPLE.read_text(encoding="utf-8").replace(
            "capacity=16", "capacity=4"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "reorder.raw.ac.mlir"
            frozen = root / "reorder.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "pyc_reorder_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertEqual(5, pyc.count("pyc.reg"))
            self.assertIn("pyc.ult", pyc)
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(
                ["ac.reorder", "ac.sink", "ac.source"],
                manifest["opcode_lowering_inventory"],
            )
            self.assertEqual("strict", manifest["hierarchy_policy"])
            self.assertEqual(["cpp", "verilog"], manifest["targets"])

            cpp_harness = root / "cpp_harness.cpp"
            cpp_executable = root / "cpp_model"
            cpp_harness.write_text(
                """#include "pyc_reorder_pipeline.hpp"
#include <array>
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::pyc_reorder_pipeline dut;
  const std::array<std::uint64_t, 3> input{
      (2ULL << 32) | 20, (0ULL << 32) | 0, (1ULL << 32) | 10};
  std::size_t cursor = 0;
  for (std::uint64_t cycle = 0; cycle < 20; ++cycle) {
    const bool offering = cycle != 0 && cursor < input.size();
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(offering ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<64>(offering ? input[cursor] : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    std::cout << cycle << " " << dut.out_valid.value() << " "
              << dut.out_data.value() << " " << dut.in_ready.value() << "\\n";
    if (offering && dut.in_ready.value() != 0)
      ++cursor;
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    str(output / "cpp/pyc_reorder_pipeline.cpp"),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stdout + cpp_run.stderr)

            verilator_harness = root / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vpyc_reorder_pipeline.h"
#include <array>
#include <cstdint>
#include <iostream>

int main() {
  Vpyc_reorder_pipeline dut;
  const std::array<std::uint64_t, 3> input{
      (2ULL << 32) | 20, (0ULL << 32) | 0, (1ULL << 32) | 10};
  std::size_t cursor = 0;
  for (std::uint64_t cycle = 0; cycle < 20; ++cycle) {
    const bool offering = cycle != 0 && cursor < input.size();
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = offering ? 1 : 0;
    dut.in_data = offering ? input[cursor] : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    std::cout << cycle << " " << unsigned(dut.out_valid) << " "
              << dut.out_data << " " << unsigned(dut.in_ready) << "\\n";
    if (offering && dut.in_ready != 0)
      ++cursor;
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = root / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "pyc_reorder_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/pyc_reorder_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vpyc_reorder_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            transactions = [
                int(fields[2])
                for line in cpp_run.stdout.splitlines()
                if len(fields := line.split()) == 4 and fields[1] == "1"
            ]
            self.assertEqual([0, (1 << 32) | 10, (2 << 32) | 20], transactions)

    def test_davincioo_like_graph_builds_full_pyc_and_verilog(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = DAVINCIOO_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "davincioo.raw.ac.mlir"
            frozen = root / "davincioo.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "davincioo_queue_model"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertIn("%in_data: i106", pyc)
            self.assertIn("pyc.reg", pyc)
            self.assertIn(": i2", pyc)
            self.assertGreaterEqual(pyc.count("pyc.fifo"), 14)
            self.assertGreaterEqual(pyc.count("pyc.reg"), 50)
            self.assertTrue((output / "verilog/davincioo_queue_model.v").is_file())
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(
                [
                    "ac.dependency",
                    "ac.merge",
                    "ac.observe",
                    "ac.reorder",
                    "ac.route",
                    "ac.scope",
                    "ac.sink",
                    "ac.source",
                    "ac.transform",
                ],
                manifest["opcode_lowering_inventory"],
            )

            projection = json.loads(DAVINCIOO_PROJECTION.read_text(encoding="utf-8"))
            run = json.loads(DAVINCIOO_RUN.read_text(encoding="utf-8"))
            opcodes = {
                span["sequence"]: span["opcode"]
                for span in run["spans"]
                if span["stage"] == "incoming"
            }
            self.assertEqual(run["record_count"], len(opcodes))
            records = [
                {"sequence_id": sequence, "opcode": opcodes[sequence]}
                for sequence in range(run["record_count"])
            ]
            packed_inputs: list[tuple[int, int]] = []
            packed_outputs: list[tuple[int, int]] = []
            for row, value in zip(
                records, projection["architectural_values"], strict=True
            ):
                sequence = row["sequence_id"]
                opcode = projection["opcode_ids"][row["opcode"]]
                route = projection["routes"][row["opcode"]]
                waits_for = projection["waits_for"][sequence]
                cycles = projection["model_cost"][row["opcode"]]
                high = (
                    cycles
                    | (waits_for << 16)
                    | (route << 24)
                    | (opcode << 26)
                    | (sequence << 34)
                )
                packed_inputs.append((sequence * 10, high))
                output_high = high
                packed_outputs.append((value, output_high))
            input_rows = ",\n      ".join(
                f"Packed{{{low}ULL, {high}ULL}}" for low, high in packed_inputs
            )

            cpp_harness = root / "davinci_cpp_harness.cpp"
            cpp_executable = root / "davinci_cpp_model"
            cpp_harness.write_text(
                f"""#include "davincioo_queue_model.hpp"
#include <array>
#include <cstdint>
#include <iostream>

struct Packed {{ std::uint64_t low; std::uint64_t high; }};

int main() {{
  pyc::gen::davincioo_queue_model dut;
  const std::array<Packed, 15> input{{
      {input_rows},
  }};
  std::size_t cursor = 0;
  for (std::uint64_t cycle = 0; cycle < 700; ++cycle) {{
    const bool offering = cycle != 0 && cursor < input.size();
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(offering ? 1 : 0);
    dut.in_data = offering
                      ? pyc::cpp::Wire<106>{{input[cursor].low, input[cursor].high}}
                      : pyc::cpp::Wire<106>{{}};
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    std::cout << cycle << " " << dut.out_valid.value() << " "
              << dut.out_data.word(0) << " " << dut.out_data.word(1) << " "
              << dut.in_ready.value() << "\\n";
    if (offering && dut.in_ready.value() != 0)
      ++cursor;
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }}
}}
""",
                encoding="utf-8",
            )
            cpp_sources = sorted(
                output.joinpath("cpp").glob("davincioo_queue_model*.cpp")
            )
            self.assertGreater(len(cpp_sources), 0)
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(output / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    *(str(path) for path in cpp_sources),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = root / "davinci_verilator_harness.cpp"
            verilator_harness.write_text(
                f"""#include "Vdavincioo_queue_model.h"
#include <array>
#include <cstdint>
#include <iostream>

struct Packed {{ std::uint64_t low; std::uint64_t high; }};

int main() {{
  Vdavincioo_queue_model dut;
  const std::array<Packed, 15> input{{
      {input_rows},
  }};
  std::size_t cursor = 0;
  for (std::uint64_t cycle = 0; cycle < 700; ++cycle) {{
    const bool offering = cycle != 0 && cursor < input.size();
    const Packed value = offering ? input[cursor] : Packed{{0, 0}};
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = offering ? 1 : 0;
    dut.in_data[0] = static_cast<std::uint32_t>(value.low);
    dut.in_data[1] = static_cast<std::uint32_t>(value.low >> 32);
    dut.in_data[2] = static_cast<std::uint32_t>(value.high);
    dut.in_data[3] = static_cast<std::uint32_t>(value.high >> 32);
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    const std::uint64_t outLow =
        static_cast<std::uint64_t>(dut.out_data[0]) |
        (static_cast<std::uint64_t>(dut.out_data[1]) << 32);
    const std::uint64_t outHigh =
        static_cast<std::uint64_t>(dut.out_data[2]) |
        (static_cast<std::uint64_t>(dut.out_data[3]) << 32);
    std::cout << cycle << " " << unsigned(dut.out_valid) << " " << outLow
              << " " << outHigh << " " << unsigned(dut.in_ready) << "\\n";
    if (offering && dut.in_ready != 0)
      ++cursor;
    dut.clk = 0;
    dut.eval();
  }}
}}
""",
                encoding="utf-8",
            )
            object_dir = root / "davinci_verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "davincioo_queue_model",
                    "--Mdir",
                    str(object_dir),
                    str(output / "verilog/pyc_primitives.v"),
                    str(output / "verilog/davincioo_queue_model.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vdavincioo_queue_model"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            transactions = [
                (int(fields[2]), int(fields[3]))
                for line in cpp_run.stdout.splitlines()
                if len(fields := line.split()) == 5 and fields[1] == "1"
            ]
            self.assertEqual(packed_outputs, transactions)

    def test_route_and_priority_merge_lower_to_static_pyc_topology(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = ROUTE_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "route.raw.ac.mlir"
            frozen = root / "route.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "pyc_route_merge_pipeline"),
                encoding="utf-8",
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            self.assertIn("pyc.eq", pyc)
            self.assertIn("pyc.mux", pyc)
            self.assertIn("pyc.not", pyc)
            self.assertIn("pyc.or", pyc)
            self.assertIn("pyc.and", pyc)
            self.assertTrue((output / "verilog/pyc_route_merge_pipeline.v").is_file())

    def test_struct_payload_has_stable_packed_pyc_and_verilog_layout(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )
        source = STRUCT_EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "struct.raw.ac.mlir"
            frozen = root / "struct.frozen.ac.mlir"
            output = root / "output"
            raw.write_text(
                lower_queue_source(source, "pyc_struct_pipeline"), encoding="utf-8"
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")
            completed = subprocess.run(
                (
                    str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                    str(frozen),
                    "--pycgen-tool",
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"),
                    "--pycc",
                    str(pycc),
                    "--toolchain-lock",
                    str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                    "--toolchain-metadata",
                    str(metadata),
                    "--cxx",
                    cxx,
                    "--verilator",
                    verilator,
                    "--pyc-output",
                    str(output / "model.pyc"),
                    "--cpp-output-dir",
                    str(output / "cpp"),
                    "--verilog-output-dir",
                    str(output / "verilog"),
                    "--manifest",
                    str(output / "manifest.json"),
                ),
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            pyc = (output / "model.pyc").read_text(encoding="utf-8")
            verilog = (output / "verilog/pyc_struct_pipeline.v").read_text(
                encoding="utf-8"
            )
            self.assertIn("%in_data: i128", pyc)
            self.assertIn("pyc.extract", pyc)
            self.assertIn("pyc.concat", pyc)
            self.assertIn("input [127:0] in_data", verilog)
            self.assertIn("pyc_fifo #(.WIDTH(128), .DEPTH(2))", verilog)

    def test_frozen_acir_builds_deterministic_pyc_cpp_and_verilog(self) -> None:
        toolchain = Path(os.environ.get("PYC_TOOLCHAIN_ROOT", DEFAULT_TOOLCHAIN))
        pycc = toolchain / "bin" / "pycc"
        metadata = toolchain / "share" / "pycircuit" / "toolchain-metadata.json"
        cxx = shutil.which("c++")
        verilator = shutil.which("verilator")
        if (
            not pycc.is_file()
            or not metadata.is_file()
            or cxx is None
            or verilator is None
        ):
            self.skipTest(
                "pinned pyCircuit toolchain, C++, or Verilator is unavailable"
            )

        source = EXAMPLE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "model.raw.ac.mlir"
            frozen = root / "model.frozen.ac.mlir"
            raw.write_text(
                lower_queue_source(source, "pyc_queue_pipeline"), encoding="utf-8"
            )
            optimized = subprocess.run(
                _freeze_command(raw),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, optimized.returncode, optimized.stderr)
            frozen.write_text(optimized.stdout, encoding="utf-8")

            manifests: list[bytes] = []
            pyc_files: list[bytes] = []
            artifact_sets: list[dict[str, bytes]] = []
            for index in range(2):
                output = root / f"run_{index}"
                completed = subprocess.run(
                    (
                        str(ROOT / "compiler/acir/tools/ac-queue-pyc-build.py"),
                        str(frozen),
                        "--pycgen-tool",
                        str(
                            ROOT
                            / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-pycgen"
                        ),
                        "--pycc",
                        str(pycc),
                        "--toolchain-lock",
                        str(ROOT / "toolchains/agentic-circuit/pyc.lock.json"),
                        "--toolchain-metadata",
                        str(metadata),
                        "--cxx",
                        cxx,
                        "--verilator",
                        verilator,
                        "--pyc-output",
                        str(output / "model.pyc"),
                        "--cpp-output-dir",
                        str(output / "cpp"),
                        "--verilog-output-dir",
                        str(output / "verilog"),
                        "--manifest",
                        str(output / "manifest.json"),
                    ),
                    cwd=ROOT,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(0, completed.returncode, completed.stderr)
                manifests.append((output / "manifest.json").read_bytes())
                pyc_files.append((output / "model.pyc").read_bytes())
                artifact_sets.append(
                    {
                        path.relative_to(output).as_posix(): path.read_bytes()
                        for path in sorted(output.rglob("*"))
                        if path.is_file() and path.name != "manifest.json"
                    }
                )
            self.assertEqual(manifests[0], manifests[1])
            self.assertEqual(pyc_files[0], pyc_files[1])
            self.assertEqual(artifact_sets[0], artifact_sets[1])
            self.assertIn(b"pyc.fifo", pyc_files[0])
            self.assertIn(b"pyc.add", pyc_files[0])

            first = root / "run_0"
            cpp_harness = first / "cpp_harness.cpp"
            cpp_executable = first / "cpp_model"
            cpp_harness.write_text(
                """#include "pyc_queue_pipeline.hpp"
#include <cstdint>
#include <iostream>

int main() {
  pyc::gen::pyc_queue_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 7; ++cycle) {
    dut.rst = pyc::cpp::Wire<1>(cycle == 0 ? 1 : 0);
    dut.in_valid = pyc::cpp::Wire<1>(cycle == 1 ? 1 : 0);
    dut.in_data = pyc::cpp::Wire<64>(cycle == 1 ? 10 : 0);
    dut.out_ready = pyc::cpp::Wire<1>(1);
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
    dut.clk = pyc::cpp::Wire<1>(1);
    dut.step();
    std::cout << cycle << " " << dut.out_valid.value() << " "
              << dut.out_data.value() << " " << dut.in_ready.value() << "\\n";
    dut.clk = pyc::cpp::Wire<1>(0);
    dut.step();
  }
}
""",
                encoding="utf-8",
            )
            cpp_build = subprocess.run(
                (
                    cxx,
                    "-std=c++17",
                    "-I",
                    str(first / "cpp"),
                    "-I",
                    str(toolchain / "include"),
                    str(first / "cpp/pyc_queue_pipeline.cpp"),
                    str(cpp_harness),
                    str(toolchain / "lib/libpyc6_runtime.a"),
                    "-o",
                    str(cpp_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_build.returncode, cpp_build.stderr)
            cpp_run = subprocess.run(
                (str(cpp_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, cpp_run.returncode, cpp_run.stderr)

            verilator_harness = first / "verilator_harness.cpp"
            verilator_harness.write_text(
                """#include "Vpyc_queue_pipeline.h"
#include <cstdint>
#include <iostream>

int main() {
  Vpyc_queue_pipeline dut;
  for (std::uint64_t cycle = 0; cycle < 7; ++cycle) {
    dut.rst = cycle == 0 ? 1 : 0;
    dut.in_valid = cycle == 1 ? 1 : 0;
    dut.in_data = cycle == 1 ? 10 : 0;
    dut.out_ready = 1;
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    std::cout << cycle << " " << unsigned(dut.out_valid) << " "
              << dut.out_data << " " << unsigned(dut.in_ready) << "\\n";
    dut.clk = 0;
    dut.eval();
  }
}
""",
                encoding="utf-8",
            )
            object_dir = first / "verilator_obj"
            verilator_build = subprocess.run(
                (
                    verilator,
                    "--cc",
                    "--exe",
                    "--build",
                    "-Wno-fatal",
                    "--top-module",
                    "pyc_queue_pipeline",
                    "--Mdir",
                    str(object_dir),
                    str(first / "verilog/pyc_primitives.v"),
                    str(first / "verilog/pyc_queue_pipeline.v"),
                    str(verilator_harness),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_build.returncode, verilator_build.stderr)
            verilator_run = subprocess.run(
                (str(object_dir / "Vpyc_queue_pipeline"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, verilator_run.returncode, verilator_run.stderr)
            self.assertEqual(cpp_run.stdout, verilator_run.stdout)
            pyc_transactions = [
                int(fields[2])
                for line in cpp_run.stdout.splitlines()
                if len(fields := line.split()) == 4 and fields[1] == "1"
            ]
            self.assertEqual([11], pyc_transactions)

            gfsim_model = first / "gfsim_model.cpp"
            gfsim_harness = first / "gfsim_harness.cpp"
            gfsim_executable = first / "gfsim_model"
            gfsim_generated = subprocess.run(
                (
                    str(ROOT / ".pycircuit_out/acir/dev-llvm22/bin/acir-queue-cxxgen"),
                    str(frozen),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, gfsim_generated.returncode, gfsim_generated.stderr)
            gfsim_model.write_text(gfsim_generated.stdout, encoding="utf-8")
            gfsim_harness.write_text(
                f"""#include "{gfsim_model.name}"
#include <cstddef>
#include <iostream>

int main() {{
  ac_generated::PycQueuePipeline model;
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
  for (auto value : model.sink_0_values())
    std::cout << value.value() << "\\n";
}}
""",
                encoding="utf-8",
            )
            gfsim_build = subprocess.run(
                (
                    cxx,
                    "-std=c++20",
                    "-I",
                    str(ROOT / "simulator/gfsim/include"),
                    str(gfsim_harness),
                    "-o",
                    str(gfsim_executable),
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, gfsim_build.returncode, gfsim_build.stderr)
            gfsim_run = subprocess.run(
                (str(gfsim_executable),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, gfsim_run.returncode, gfsim_run.stderr)
            gfsim_transactions = [
                int(value) for value in gfsim_run.stdout.splitlines() if value
            ]
            self.assertEqual(pyc_transactions, gfsim_transactions)


if __name__ == "__main__":
    unittest.main()
