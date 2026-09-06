from __future__ import annotations

import importlib
import unittest
from dataclasses import FrozenInstanceError

PUBLIC = {
    "system",
    "module",
    "extern_module",
    "struct",
    "packet",
    "transaction",
    "protocol",
    "interface",
    "process",
    "rule",
    "scope",
    "array",
    "map",
    "set",
    "instances",
    "view",
    "find",
    "bits",
    "BitfieldSpec",
    "concat",
    "insert",
    "matches",
    "queue",
    "ResourceRef",
    "address_space",
    "address_map",
    "Static",
    "Flow",
    "Endpoint",
    "source",
    "count_leading_zeros",
    "count_trailing_zeros",
    "popcount",
    "priority_encode",
    "memory",
    "sink",
    "observe",
    "expect",
    "compute",
    "pipeline",
    "config",
    "const",
    "jit",
    "route",
    "merge",
    "schedule",
    "engine",
    "reorder",
    "round_robin",
    "priority",
    "fork",
    "barrier",
    "table",
    "slot",
    *(f"u{width}" for width in range(1, 65)),
    "s8",
    "s16",
    "s32",
    "s64",
}


class ReadyValid:
    """Local schema marker used to form a public Flow annotation."""


class PublicApiTest(unittest.TestCase):
    def test_exact_public_inventory_is_importable(self) -> None:
        api = importlib.import_module("agentic_circuit")

        self.assertEqual(PUBLIC, set(api.__all__))
        for name in PUBLIC:
            self.assertIsNotNone(getattr(api, name))

    def test_variable_has_no_long_form_public_alias(self) -> None:
        api = importlib.import_module("agentic_circuit")

        self.assertFalse(hasattr(api, "variable"))

    def test_unsigned_bit_types_cover_every_width_from_one_through_sixty_four(
        self,
    ) -> None:
        api = importlib.import_module("agentic_circuit")

        for width in range(1, 65):
            bit_type = getattr(api, f"u{width}")
            self.assertEqual(width, bit_type.width)
            self.assertFalse(bit_type.signed)

    def test_bits_factory_uses_the_same_static_width_contract(self) -> None:
        api = importlib.import_module("agentic_circuit")

        for width in (1, 3, 5, 17, 64):
            self.assertEqual(getattr(api, f"u{width}"), api.bits[width])
        for width in (0, 65):
            with self.assertRaisesRegex(ValueError, r"\[1, 64\]"):
                api.bits[width]

    def test_bitfield_spec_is_immutable_and_has_stable_layout_metadata(self) -> None:
        api = importlib.import_module("agentic_circuit")

        first = api.BitfieldSpec(
            width=32,
            fields={"opcode": (31, 26), "rd": (25, 21), "imm26": (25, 0)},
        )
        reordered = api.BitfieldSpec(
            width=32,
            fields={"imm26": (25, 0), "rd": (25, 21), "opcode": (31, 26)},
        )

        self.assertEqual(first.fingerprint, reordered.fingerprint)
        self.assertEqual((26, 6), first.field_slices()["opcode"])
        self.assertEqual(26, first.field_width("imm26"))
        with self.assertRaises(TypeError):
            first.fields["new"] = (1, 0)
        with self.assertRaises(FrozenInstanceError):
            first._layout = reordered._layout

    def test_bitfield_spec_rejects_invalid_layouts_and_wide_values(self) -> None:
        api = importlib.import_module("agentic_circuit")

        with self.assertRaisesRegex(ValueError, "out of range"):
            api.BitfieldSpec(width=8, fields={"bad": (8, 0)})
        with self.assertRaisesRegex(ValueError, r"\[1, 64\]"):
            api.BitfieldSpec(width=65, fields={"wide": (64, 0)})

    def test_array_annotation_uses_existing_pythonic_array_intrinsic(self) -> None:
        api = importlib.import_module("agentic_circuit")

        descriptor = api.array[4, api.bits[5]]
        self.assertEqual("!ac.value_array<4 x i5>", descriptor.mlir())
        self.assertEqual(20, descriptor.bit_width())
        with self.assertRaisesRegex(ValueError, "positive"):
            api.array[0, api.bits[5]]

    def test_scalar_type_rejects_out_of_range_widths(self) -> None:
        types = importlib.import_module("agentic_circuit._types")

        for width in (0, 65):
            with self.subTest(width=width):
                with self.assertRaisesRegex(ValueError, r"\[1, 64\]"):
                    types.ScalarType(width)

    def test_symbolic_values_reject_python_coercion(self) -> None:
        types = importlib.import_module("agentic_circuit._types")
        value = types._test_symbolic("request", types.Flow[int, ReadyValid])

        for operation in (bool, int, hash, iter):
            with self.subTest(operation=operation.__name__):
                with self.assertRaisesRegex(TypeError, "ACPY-STATIC-002"):
                    operation(value)

    def test_symbolic_values_reject_python_equality(self) -> None:
        types = importlib.import_module("agentic_circuit._types")
        left = types._test_symbolic("left", object())
        right = types._test_symbolic("right", object())

        with self.assertRaisesRegex(TypeError, "ACPY-STATIC-002"):
            left == right

    def test_symbolic_value_repr_uses_only_stable_identity(self) -> None:
        types = importlib.import_module("agentic_circuit._types")

        value = types._test_symbolic("request", object())

        self.assertEqual("SymbolicValue('request')", repr(value))

    def test_decorators_create_immutable_definition_metadata(self) -> None:
        api = importlib.import_module("agentic_circuit")

        @api.module
        def producer() -> None:
            raise AssertionError("decorating a definition must not execute it")

        self.assertEqual("module", producer.kind)
        self.assertEqual(producer.function.__qualname__, producer.qualified_name)
        self.assertTrue(producer.qualified_name.endswith(".<locals>.producer"))
        self.assertEqual((), producer.explicit_options)
        with self.assertRaises(FrozenInstanceError):
            producer.kind = "system"

    def test_decorator_options_are_canonicalized(self) -> None:
        api = importlib.import_module("agentic_circuit")

        @api.module(zeta=2, alpha=1)
        def configured() -> None:
            pass

        self.assertEqual((("alpha", 1), ("zeta", 2)), configured.explicit_options)

    def test_generated_module_is_not_a_public_compatibility_alias(self) -> None:
        api = importlib.import_module("agentic_circuit")

        self.assertFalse(hasattr(api, "generated_module"))

    def test_rule_decorator_captures_without_executing(self) -> None:
        api = importlib.import_module("agentic_circuit")

        @api.rule
        def complete(item):
            raise AssertionError("decorating a rule must not execute it")

        self.assertEqual("rule", complete.kind)
        self.assertEqual("complete", complete.__name__)

    def test_ast_only_markers_reject_runtime_execution(self) -> None:
        api = importlib.import_module("agentic_circuit")

        operations = (
            lambda: api.scope("nested"),
            lambda: api.array(1, 2),
            lambda: api.map({"a": object()}),
            lambda: api.set({object()}),
            lambda: api.instances(1, 2),
            lambda: api.view(object(), "field"),
            lambda: api.concat(object(), object()),
            lambda: api.insert(object(), object(), lsb=0),
            lambda: api.matches(object(), "1xx0"),
            lambda: api.source(int),
            lambda: api.count_leading_zeros(object()),
            lambda: api.count_trailing_zeros(object()),
            lambda: api.popcount(object()),
            lambda: api.priority_encode(object()),
            lambda: api.sink(object()),
            lambda: api.observe(object()),
            lambda: api.expect(
                object(), predicate=lambda value: True, message="expected"
            ),
            lambda: api.compute(object(), lambda value: value),
            lambda: api.pipeline(object(), stages=2),
            lambda: api.route(object(), by=object(), outputs=2),
            lambda: api.merge(object(), object()),
            lambda: api.schedule(
                object(),
                by=object(),
                waits_for=object(),
                resource=object(),
                cost=object(),
            ),
            lambda: api.engine(object(), cost=object()),
            lambda: api.reorder(object(), by=object()),
            lambda: api.fork(object(), outputs=2),
            lambda: api.barrier(object(), object()),
        )
        for operation in operations:
            with self.subTest(operation=operation):
                with self.assertRaises(NotImplementedError):
                    operation()

    def test_table_factory_is_subscript_only_and_legacy_call_is_removed(self) -> None:
        import agentic_circuit as api

        with self.assertRaises(NotImplementedError):
            api.table[16, api.u16](init=0)
        with self.assertRaisesRegex(TypeError, "use ac.memory"):
            api.table(object(), address=object())


if __name__ == "__main__":
    unittest.main()
