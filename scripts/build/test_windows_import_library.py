"""Focused import generation tests; no dependency SDK is modified."""
import importlib.util
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("windows_imports", Path(__file__).with_name("windows-import-library.py"))
imports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(imports)

READER = """Format: COFF-x86-64
IMAGE_FILE_DLL
ExportTableRVA: 0x3000
ExportTableSize: 0x100
  Section {
    VirtualAddress: 0x1000
    VirtualSize: 0x100
    IMAGE_SCN_CNT_CODE
  }
  Section {
    VirtualAddress: 0x2000
    VirtualSize: 0x100
  }
Export {
  Ordinal: 1
  Name: code
  RVA: 0x1010
}
Export {
  Ordinal: 2
  Name: variable
  RVA: 0x2010
}
"""


class ImportTests(unittest.TestCase):
    def test_function_and_data(self):
        self.assertEqual(imports.exports_from_reader(READER), [(1, "code", False), (2, "variable", True)])

    def test_malformed_and_forwarders(self):
        for bad in (READER.replace("COFF-x86-64", "COFF-ARM64"),
                    READER.replace("0x1010", "0x3010"),
                    READER.replace("0x1010", "0x9000"),
                    READER.replace("Ordinal: 2", "Ordinal: 1"),
                    READER.replace("Name: variable", "Name: code"),
                    READER.replace("VirtualSize: 0x100", "VirtualSize: 0x2000", 1)):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                imports.exports_from_reader(bad)

    def test_key_changes_with_identity(self):
        self.assertNotEqual(imports.key_for({"dll": "a"}), imports.key_for({"dll": "b"}))

    def test_cache_noop_restores_missing_and_corrupt_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            dll = output / "test.dll"
            dll.write_bytes(b"fixture")
            calls = []

            def dlltool(command, check):
                calls.append(command)
                Path(command[-1]).write_bytes(b"!<arch>\nfixture")

            with patch.object(imports, "run", return_value=READER), patch.object(imports.subprocess, "run", side_effect=dlltool):
                def generate():
                    imports.generate(dll, Path("reader"), Path("dlltool"), output, {"fixture": True})
                generate()
                definition = (output / "exports.def").read_text()
                self.assertIn("variable @2 DATA", definition)
                before = [(output / name).stat().st_mtime_ns for name in ("imports.lib", "exports.def", "manifest.json")]
                generate()
                self.assertEqual(len(calls), 1)
                self.assertEqual(before, [(output / name).stat().st_mtime_ns for name in ("imports.lib", "exports.def", "manifest.json")])
                (output / "imports.lib").unlink()
                generate()
                self.assertEqual(len(calls), 2)
                (output / "exports.def").write_text("corrupt")
                generate()
                self.assertEqual(len(calls), 3)

    def test_failure_keeps_published_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            dll = output / "test.dll"
            (output / "imports.lib").write_bytes(b"previous")
            with patch.object(imports, "run", return_value=READER), patch.object(imports.subprocess, "run", side_effect=RuntimeError("failed")):
                with self.assertRaises(RuntimeError):
                    imports.generate(dll, Path("reader"), Path("dlltool"), output, {})
            self.assertEqual((output / "imports.lib").read_bytes(), b"previous")

    def test_runtime_uses_actual_imports_and_target_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            compiler = root / "compiler"
            compiler.mkdir()
            config = root / "Debug"
            config.mkdir()
            exe = config / "test.exe"
            exe.write_bytes(b"executable")
            runtime = "libclang_rt.asan_dynamic-x86_64.dll"
            with patch.object(sys, "argv", ["imports", "--runtime-executable", str(config / "missing.exe"),
                                           "--compiler-bin", str(compiler), "--readobj", "reader",
                                           "--skip-missing-executable"]), patch.object(imports, "run") as reader:
                imports.main()
                reader.assert_not_called()
            with patch.object(imports, "run", return_value="  Name: kernel32.dll\n"):
                imports.copy_runtime(exe, Path("reader"), compiler)
                self.assertEqual(list(config.iterdir()), [exe])
            with patch.object(imports, "run", return_value=f"  Name: {runtime}\n"):
                with self.assertRaisesRegex(ValueError, "runtime is missing"):
                    imports.copy_runtime(exe, Path("reader"), compiler)
                self.assertEqual(exe.read_bytes(), b"executable")
                (compiler / runtime).write_bytes(b"runtime")
                imports.copy_runtime(exe, Path("reader"), compiler)
                self.assertEqual((config / runtime).read_bytes(), b"runtime")
                before = (config / runtime).stat().st_mtime_ns
                imports.copy_runtime(exe, Path("reader"), compiler)
                self.assertEqual((config / runtime).stat().st_mtime_ns, before)
                self.assertEqual(exe.read_bytes(), b"executable")


if __name__ == "__main__":
    unittest.main()
