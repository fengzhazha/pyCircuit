from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
import unittest


ROOT = Path(__file__).resolve().parents[4]
TOOL = ROOT / "compiler/acir/tools/acir-queue-veriloggen.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("acir_queue_veriloggen", TOOL)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load PYC Verilog bridge")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


PYC_ASSERT = """
func.func @guard(%cond: i1) -> (i1) attributes {result_names = ["out"]} {
    pyc.assert %cond {msg = "guard_failed"}
    func.return %cond : i1
}
"""

PYC_COMPARE = """
func.func @compare(%left: i8, %right: i8) -> (i1) attributes {result_names = ["equal"]} {
    %same = pyc.cmp %left, %right {predicate = "eq"} : i8, i8 -> i1
    func.return %same : i1
}
"""


class PycVerilogBackendTest(unittest.TestCase):
    def test_assertion_is_preserved_in_simulation_only_rtl(self) -> None:
        tool = load_tool()
        module = tool.parse_pyc_module(PYC_ASSERT)
        verilog = tool.emit_verilog(module, ROOT / "library/verilog")
        self.assertIn("// synthesis translate_off", verilog)
        self.assertIn("if (!cond)", verilog)
        self.assertIn('$display("guard_failed")', verilog)
        self.assertNotIn("assertion omitted", verilog)
        verilator = shutil.which("verilator")
        if verilator is not None:
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "guard.v"
                source.write_text(verilog, encoding="utf-8")
                linted = subprocess.run(
                    (
                        verilator,
                        "--lint-only",
                        "--timing",
                        "--top-module",
                        "guard",
                        str(source),
                    ),
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(0, linted.returncode, linted.stderr)

    def test_result_names_are_unique_safe_verilog_identifiers(self) -> None:
        tool = load_tool()
        duplicate = PYC_ASSERT.replace("-> (i1)", "-> (i1, i1)").replace(
            '["out"]', '["out", "out"]'
        )
        with self.assertRaisesRegex(tool.PYCVerilogError, "identifier list"):
            tool.parse_pyc_module(duplicate)
        injected = PYC_ASSERT.replace('["out"]', '["out; wire injected"]')
        with self.assertRaisesRegex(tool.PYCVerilogError, "identifier list"):
            tool.parse_pyc_module(injected)

    def test_comparison_annotation_describes_operands_and_result_is_i1(self) -> None:
        tool = load_tool()
        verilog = tool.emit_verilog(
            tool.parse_pyc_module(PYC_COMPARE),
            ROOT / "library/verilog",
        )
        self.assertIn("wire same;", verilog)
        self.assertNotIn("wire [7:0] same;", verilog)
        self.assertIn("assign same = left == right;", verilog)

    def test_cli_rejects_invalid_timeout_and_path_aliases(self) -> None:
        tool = load_tool()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "model.pyc"
            source.write_text(PYC_ASSERT, encoding="utf-8")
            with self.assertRaises(SystemExit) as timeout:
                tool.main([str(source), "--pyc-input", "--timeout", "nan"])
            self.assertEqual(2, timeout.exception.code)
            original = source.read_bytes()
            with self.assertRaises(SystemExit) as alias:
                tool.main([str(source), "--pyc-input", "-o", str(source)])
            self.assertEqual(2, alias.exception.code)
            self.assertEqual(original, source.read_bytes())

    def test_runtime_primitives_pass_bounded_verilator_smoke(self) -> None:
        verilator = shutil.which("verilator")
        if verilator is None:
            self.skipTest("Verilator is unavailable")
        runtime = ROOT / "library/verilog"
        with tempfile.TemporaryDirectory() as directory:
            object_dir = Path(directory) / "obj"
            completed = subprocess.run(
                (
                    verilator,
                    "--binary",
                    "--timing",
                    "--top-module",
                    "pyc_primitives_smoke",
                    "-Mdir",
                    str(object_dir),
                    str(
                        ROOT
                        / "tests/mlir/agentic-circuit/CodeGen/pyc-primitives-smoke.sv"
                    ),
                    str(runtime / "pyc_fifo.v"),
                    str(runtime / "pyc_reg.v"),
                    str(runtime / "pyc_popcount_primitive.v"),
                    str(runtime / "pyc_rr_arbiter.v"),
                    "-o",
                    "pyc-primitives-smoke",
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            executed = subprocess.run(
                (str(object_dir / "pyc-primitives-smoke"),),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, executed.returncode, executed.stderr)
            self.assertIn("PYC_PRIMITIVES_SMOKE PASS", executed.stdout)


if __name__ == "__main__":
    unittest.main()
