"""Tests the resumable Blender import report without starting Blender."""
import io
import json
import os
import shutil
from pathlib import Path
import tempfile
import time
import unittest

from audit_blender_gltf import append_record, fingerprint, load_cached, records, verify_published, write_report


class BlenderImportAuditTests(unittest.TestCase):
    def test_truncated_checkpoint_preserves_completed_results(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "checkpoint.jsonl"
            path.write_text('{"status":"passed"}\n{"incomplete":', encoding="utf-8")
            self.assertEqual(records(path), [{"status": "passed"}])
            self.assertEqual(records(path.with_suffix(".missing")), [])

    def test_resume_uses_newest_matching_file_and_blender(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = {"path": "a.gltf", "fingerprint": {"size": 12, "mtime_ns": 13}}
            base = {**source, "root": str(root), "blender": "Blender 5.2"}
            paths = [root / "new.jsonl", root / "old.jsonl"]
            paths[0].write_text(json.dumps({**base, "status": "passed", "finished_at": 2}) + "\n")
            paths[1].write_text(json.dumps({**base, "status": "failed", "finished_at": 1}) + "\n")
            self.assertEqual(load_cached(paths, root, "Blender 5.2", {"a.gltf": source})["a.gltf"]["status"], "passed")
            self.assertEqual(load_cached(paths, root, "Blender 5.3", {"a.gltf": source}), {})
            changed = {**source, "fingerprint": {"size": 12, "mtime_ns": 14}}
            self.assertEqual(load_cached(paths, root, "Blender 5.2", {"a.gltf": changed}), {})
            self.assertEqual(load_cached(paths[1:], root, "Blender 5.2", {"a.gltf": source}, retry_failed=True), {})

    def test_published_identity_requires_exact_coverage_and_unchanged_imports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshot, final = root / "snapshot", root / "published"
            snapshot.mkdir()
            final.mkdir()
            (snapshot / "a.gltf").write_bytes(b"first model")
            (snapshot / "b.gltf").write_bytes(b"second model")
            os.link(snapshot / "a.gltf", final / "a.gltf")
            shutil.copy2(snapshot / "b.gltf", final / "b.gltf")
            items = {name: {"path": name, "status": "passed", "fingerprint": fingerprint(snapshot / name)} for name in ("a.gltf", "b.gltf")}
            report = root / "report.json"
            write_report(report, snapshot, "Blender 5.2", items, 2, time.monotonic())
            identity = verify_published(report, final)
            self.assertEqual((identity["same_physical_files"], identity["sha256_matches"]), (1, 1))
            (final / "b.gltf").write_bytes(b"changed model")
            with self.assertRaisesRegex(ValueError, "differs from imported"):
                verify_published(report, final)
            shutil.copy2(snapshot / "b.gltf", final / "b.gltf")
            (final / "extra.gltf").write_bytes(b"uncovered")
            with self.assertRaisesRegex(ValueError, "coverage differs"):
                verify_published(report, final)
            (final / "extra.gltf").unlink()
            (snapshot / "a.gltf").write_bytes(b"changed audited file")
            with self.assertRaisesRegex(ValueError, "changed after import"):
                verify_published(report, final)

    def test_atomic_summary_records_partial_coverage_and_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "report.json"
            items = {"b.gltf": {"path": "b.gltf", "status": "failed"}, "a.gltf": {"path": "a.gltf", "status": "passed"}}
            write_report(report, root, "Blender 5.2", items, 3, time.monotonic())
            summary = json.loads(report.read_text())
            self.assertEqual((summary["passed"], summary["failed"], summary["complete"]), (1, 1, False))
            self.assertEqual([item["path"] for item in summary["files"]], ["a.gltf", "b.gltf"])
            self.assertFalse(report.with_suffix(".json.tmp").exists())
            stream = io.StringIO()
            append_record(stream, {"status": "failed", "error": "texture \u00e9"})
            self.assertEqual(json.loads(stream.getvalue())["error"], "texture \u00e9")


if __name__ == "__main__":
    unittest.main()
