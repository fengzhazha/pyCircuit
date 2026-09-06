from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[4]
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "lowering"


EXACT_PUBLIC_API = {
    "system",
    "module",
    "extern_module",
    "generated_module",
    "struct",
    "packet",
    "transaction",
    "protocol",
    "interface",
    "process",
    "rule",
    "scope",
    "array",
    "instances",
    "view",
    "queue",
    "ResourceRef",
    "address_space",
    "address_map",
    "Static",
    "Flow",
    "Endpoint",
    "source",
    "matches",
    "priority_encode",
    "sink",
    "observe",
    *(f"u{width}" for width in range(1, 65)),
    "s8",
    "s16",
    "s32",
    "s64",
}


@dataclass(frozen=True, slots=True)
class CoverageRow:
    positive: tuple[str, ...]
    negative: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class CorpusArtifacts:
    acpy_files: tuple[tuple[str, str], ...]
    acir_files: tuple[tuple[str, str], ...]


def elaborate_corpus(root: Path, *, hash_seed: int) -> CorpusArtifacts:
    for name in ("hierarchy.py", "process.py"):
        shutil.copyfile(FIXTURES / name, root / name)
    script = """
import json
import sys
from pathlib import Path
from python_frontend.test_lower_acir import elaborate

root = Path(sys.argv[1])
acpy = {}
acir = {}
for name in ("hierarchy", "process"):
    result = elaborate(root / f"{name}.py", root)
    if result.diagnostics or result.document is None or result.acir is None:
        raise RuntimeError((name, result.diagnostics))
    acpy[f"{name}.acpy.json"] = result.document.canonical_bytes().decode("utf-8")
    acir[f"{name}.ac.mlir"] = result.acir
print(json.dumps({"acpy": acpy, "acir": acir}, sort_keys=True, separators=(",", ":")))
"""
    environment = os.environ.copy()
    environment["PYTHONHASHSEED"] = str(hash_seed)
    environment["PYTHONPATH"] = os.pathsep.join(
        (
            str(REPOSITORY / "python/agentic-circuit/src"),
            str(REPOSITORY / "tests/python/agentic-circuit"),
            str(REPOSITORY),
        )
    )
    completed = subprocess.run(
        (sys.executable, "-c", script, str(root)),
        cwd=REPOSITORY,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    payload = json.loads(completed.stdout)
    return CorpusArtifacts(
        acpy_files=tuple(sorted(payload["acpy"].items())),
        acir_files=tuple(sorted(payload["acir"].items())),
    )


def frontend_test_ledger() -> dict[str, CoverageRow]:
    public_import = (
        "python_frontend.test_public_api."
        "PublicApiTest.test_exact_public_inventory_is_importable"
    )
    definition_positive = (
        "python_frontend.test_definitions."
        "DefinitionCaptureTest.test_definition_matches_ast_and_system_selection"
    )
    definition_negative = (
        "python_frontend.test_definitions."
        "DefinitionCaptureTest.test_non_static_defaults_and_variadic_parameters_are_rejected"
    )
    decorator_row = CoverageRow(
        (public_import, definition_positive), (definition_negative,)
    )
    symbolic_negative = (
        "python_frontend.test_public_api."
        "PublicApiTest.test_symbolic_values_reject_python_coercion"
    )
    annotation_row = CoverageRow((public_import,), (symbolic_negative,))
    collection_negative = (
        "python_frontend.test_scopes_collections."
        "ScopeCollectionTest.test_ragged_collection_is_rejected"
    )
    marker_negative = (
        "python_frontend.test_public_api."
        "PublicApiTest.test_ast_only_markers_reject_runtime_execution"
    )
    return {
        "system": decorator_row,
        "module": decorator_row,
        "extern_module": decorator_row,
        "generated_module": decorator_row,
        "struct": decorator_row,
        "packet": decorator_row,
        "transaction": decorator_row,
        "protocol": decorator_row,
        "interface": decorator_row,
        "process": CoverageRow(
            (
                "python_frontend.test_process."
                "ProcessFrontendTest.test_nested_control_and_suspension_build_closed_cfg",
            ),
            (
                "python_frontend.test_process."
                "ProcessFrontendTest.test_busy_wait_coroutine_generator_and_undeclared_effect_are_rejected",
            ),
        ),
        "scope": CoverageRow(
            (
                "python_frontend.test_scopes_collections."
                "ScopeCollectionTest.test_scope_signature_is_minimal_and_ordered",
            ),
            (
                "python_frontend.test_scopes_collections."
                "ScopeCollectionTest.test_owned_resource_cannot_escape_its_scope",
            ),
        ),
        "array": CoverageRow(
            (
                "python_frontend.test_scopes_collections."
                "ScopeCollectionTest.test_rectangular_homogeneous_collection_selects_array",
            ),
            (collection_negative,),
        ),
        "instances": CoverageRow(
            (
                "python_frontend.test_scopes_collections."
                "ScopeCollectionTest.test_specialization_difference_selects_instances",
            ),
            (collection_negative,),
        ),
        "view": CoverageRow((public_import,), (marker_negative,)),
        "queue": CoverageRow(
            (
                "python_frontend.test_resources."
                "ResourceFrontendTest.test_queue_and_address_map_are_static_records",
            ),
            (
                "python_frontend.test_resources."
                "ResourceFrontendTest.test_dynamic_address_and_nonpositive_depth_are_rejected",
            ),
        ),
        "ResourceRef": annotation_row,
        "address_space": CoverageRow(
            (
                "python_frontend.test_resources."
                "ResourceFrontendTest.test_queue_and_address_map_are_static_records",
            ),
            (
                "python_frontend.test_resources."
                "ResourceFrontendTest.test_dynamic_address_and_nonpositive_depth_are_rejected",
            ),
        ),
        "address_map": CoverageRow(
            (
                "python_frontend.test_resources."
                "ResourceFrontendTest.test_queue_and_address_map_are_static_records",
            ),
            (
                "python_frontend.test_resources."
                "ResourceFrontendTest.test_equal_priority_address_overlap_is_rejected",
            ),
        ),
        "Static": annotation_row,
        "Flow": annotation_row,
        "Endpoint": annotation_row,
        "source": CoverageRow((public_import,), (marker_negative,)),
        "matches": CoverageRow(
            (
                "python_frontend.test_queue_frontend."
                "QueueFrontendTest.test_masked_match_emits_exact_canonical_unsigned_attributes",
            ),
            (
                "python_frontend.test_queue_frontend."
                "QueueFrontendTest.test_masked_match_rejects_nonstatic_malformed_or_mistyped_patterns",
            ),
        ),
        "priority_encode": CoverageRow((public_import,), (marker_negative,)),
        "sink": CoverageRow((public_import,), (marker_negative,)),
        "observe": CoverageRow((public_import,), (marker_negative,)),
        "rule": CoverageRow(
            (
                "python_frontend.test_public_api."
                "PublicApiTest.test_rule_decorator_captures_without_executing",
            ),
            (
                "python_frontend.test_queue_frontend."
                "QueueFrontendTest.test_rule_frontend_rejects_unsupported_control_flow",
            ),
        ),
        **{f"u{width}": annotation_row for width in range(1, 65)},
        "s8": annotation_row,
        "s16": annotation_row,
        "s32": annotation_row,
        "s64": annotation_row,
    }


class FrontendDeterminismTest(unittest.TestCase):
    def test_corpus_is_identical_across_roots_and_hash_seeds(self) -> None:
        with (
            tempfile.TemporaryDirectory() as first,
            tempfile.TemporaryDirectory() as second,
        ):
            first_root = Path(first)
            second_root = Path(second)
            first_result = elaborate_corpus(first_root, hash_seed=1)
            second_result = elaborate_corpus(second_root, hash_seed=99)

            self.assertEqual(first_result.acpy_files, second_result.acpy_files)
            self.assertEqual(first_result.acir_files, second_result.acir_files)
            serialized = repr((first_result.acpy_files, first_result.acir_files))
            self.assertNotIn(str(first_root), serialized)
            self.assertNotIn(str(second_root), serialized)

    def test_every_public_name_has_positive_and_negative_coverage(self) -> None:
        ledger = frontend_test_ledger()

        self.assertEqual(EXACT_PUBLIC_API, set(ledger))
        self.assertTrue(all(row.positive and row.negative for row in ledger.values()))
        for public_name, row in ledger.items():
            for test_name in (*row.positive, *row.negative):
                with self.subTest(public_name=public_name, test_name=test_name):
                    suite = unittest.defaultTestLoader.loadTestsFromName(test_name)
                    loaded = tuple(suite)
                    self.assertEqual(1, len(loaded))
                    self.assertNotEqual("_FailedTest", type(loaded[0]).__name__)


if __name__ == "__main__":
    unittest.main()
