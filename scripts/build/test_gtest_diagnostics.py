"""Check generated-declaration boundaries, pinned inputs and cache restoration."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("gtest_diagnostics", Path(__file__).with_name("gtest-diagnostics.py"))
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)


class GeneratorTests(unittest.TestCase):
    def test_extracts_exactly_one_continued_macro(self):
        self.assertEqual(diagnostics.macro("#define SAMPLE(a) first \\\n second(a)\n", "SAMPLE"), ("(a)", "first   second(a)"))
        with self.assertRaises(ValueError):
            diagnostics.macro("#define SAMPLE(a) first\n#define SAMPLE(a) second\n", "SAMPLE")

    def test_registration_scopes_end_before_handwritten_body(self):
        body = ("class GTEST_TEST_CLASS_NAME_(suite, name) { void TestBody(); }; "
                "::testing::TestInfo* const GTEST_TEST_CLASS_NAME_(suite, name)::test_info_ = "
                "::testing::internal::MakeAndRegisterTestInfo(); "
                "void GTEST_TEST_CLASS_NAME_(suite, name)::TestBody()")
        result = diagnostics.test_declaration(body, False)
        self.assertEqual(result.count('diagnostic push'), 2)
        self.assertTrue(result.endswith('diagnostic pop") void GTEST_TEST_CLASS_NAME_(suite, name)::TestBody()'))
        with self.assertRaises(ValueError):
            diagnostics.test_declaration(body.replace("TestBody()", "ChangedBody()"), False)

    def test_parameter_generator_precedes_suppression(self):
        body = ("auto UserGenerator() { return UserExpression(); } "
                "[[maybe_unused]] static int gtest_##prefix##test_suite_name##_dummy_ = "
                "registry->AddTestSuiteInstantiation(__LINE__)")
        result = diagnostics.instantiate(body)
        self.assertLess(result.index("UserExpression()"), result.index("diagnostic push"))
        with self.assertRaises(ValueError):
            diagnostics.instantiate(body.replace("__LINE__)", "__VA_ARGS__, __LINE__)"))

    def test_death_statement_follows_restored_diagnostics(self):
        body = "switch (gtest_dt->AssumeRole()) { GTEST_EXECUTE_DEATH_TEST_STATEMENT_(statement, gtest_dt); }"
        result = diagnostics.death_test(body)
        self.assertLess(result.index("diagnostic pop"), result.index("GTEST_EXECUTE_DEATH_TEST_STATEMENT_"))
        with self.assertRaises(ValueError):
            diagnostics.death_test(body.replace("AssumeRole", "ChangedRole"))

    def test_input_hash_rejects_a_changed_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "gtest.h"
            header.write_bytes(b"pinned\n")
            with patch.object(diagnostics, "PINNED_HEADERS", {"gtest.h": diagnostics.sha(b"pinned\n")}):
                _, record = diagnostics.inputs(root)
                self.assertEqual(record["headers"]["gtest.h"]["sha256"], diagnostics.sha(b"pinned\n"))
                header.write_bytes(b"different\n")
                with self.assertRaisesRegex(ValueError, "unsupported GoogleTest"):
                    diagnostics.inputs(root)

    def test_cache_noop_and_missing_manifest_or_header_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "adapter.hpp"
            manifest = output.with_suffix(".json")
            record = {"fixture": True}
            with patch.object(diagnostics, "adapter", return_value=b"generated\n"):
                diagnostics.generate({}, record, output)
                before = output.stat().st_mtime_ns, manifest.stat().st_mtime_ns
                diagnostics.generate({}, record, output)
                self.assertEqual(before, (output.stat().st_mtime_ns, manifest.stat().st_mtime_ns))
                manifest.unlink()
                diagnostics.generate({}, record, output)
                self.assertEqual(json.loads(manifest.read_text())["adapter_sha256"], diagnostics.sha(b"generated\n"))
                output.unlink()
                diagnostics.generate({}, record, output)
                self.assertEqual(output.read_bytes(), b"generated\n")


if __name__ == "__main__":
    unittest.main()
