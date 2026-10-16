#!/usr/bin/env python3
"""Import every native glTF in Blender, with bounded workers and resumable results.

Example:
  python audit_blender_gltf.py port/run/port_data --blender /path/to/blender \
      --report .kazarov/blender-import/report.json --jobs 2

The JSON report and adjacent JSONL checkpoint describe actual Blender imports.
Each background process resets all Blender data between files and exits after a
bounded batch. Repeated runs reuse results only for unchanged files and the same
Blender version. Worker logs remain beside the report for diagnosis.
"""

import argparse
import concurrent.futures
import gc
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid


def append_record(stream, record):
    stream.write(json.dumps(record, ensure_ascii=True) + "\n")
    stream.flush()


def records(path):
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return []
    result = []
    for line in lines:
        try:
            result.append(json.loads(line))
        except json.JSONDecodeError:
            # A killed worker can leave its last line incomplete.
            continue
    return result


def fingerprint(path):
    stat = path.stat()
    return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def memory_usage():
    if os.name == "nt":
        import ctypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", ctypes.c_uint32), ("page_faults", ctypes.c_uint32)] + [
                (name, ctypes.c_size_t) for name in (
                    "peak_working_set", "working_set", "peak_paged_pool", "paged_pool",
                    "peak_nonpaged_pool", "nonpaged_pool", "pagefile", "peak_pagefile", "private_usage",
                )
            ]

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_uint32]
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        if psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
            return {"working_set_bytes": counters.working_set, "peak_working_set_bytes": counters.peak_working_set}
    else:
        import resource
        peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return {"peak_working_set_bytes": peak if sys.platform == "darwin" else peak * 1024}
    return {}


def worker(batch_path, output_path):
    import bpy

    batch = json.loads(batch_path.read_text(encoding="utf-8"))
    root = Path(batch["root"])
    with output_path.open("w", encoding="utf-8") as stream:
        for source in batch["files"]:
            base = {
                **source,
                "root": batch["root"],
                "blender": batch["blender"],
            }
            append_record(stream, {**base, "status": "started", "started_at": time.time()})
            started = time.monotonic()
            result = dict(base)
            path = root / source["path"]
            try:
                if fingerprint(path) != source["fingerprint"]:
                    raise RuntimeError("source file changed before import")
                bpy.ops.wm.read_factory_settings(use_empty=True)
                outcome = bpy.ops.import_scene.gltf(filepath=str(path))
                if "FINISHED" not in outcome:
                    raise RuntimeError(f"glTF importer returned {outcome}")
                if fingerprint(path) != source["fingerprint"]:
                    raise RuntimeError("source file changed during import")
                result.update(
                    status="passed",
                    objects=len(bpy.data.objects),
                    meshes=len(bpy.data.meshes),
                    materials=len(bpy.data.materials),
                    images=len(bpy.data.images),
                    actions=len(bpy.data.actions),
                    blender_version=bpy.app.version_string,
                    **memory_usage(),
                )
            except Exception as error:
                result.update(status="failed", error=str(error), traceback=traceback.format_exc())
            finally:
                try:
                    bpy.ops.wm.read_factory_settings(use_empty=True)
                    gc.collect()
                except Exception as error:
                    result.update(status="failed", error=f"Blender cleanup failed: {error}")
            result["finished_at"] = time.time()
            result["seconds"] = round(time.monotonic() - started, 4)
            append_record(stream, result)


def run_batch(blender, root, version, files, work, timeout):
    """Recycle the worker after a timeout/crash and finish its unstarted files."""
    identifier = uuid.uuid4().hex
    batch_path = work / f"{identifier}.batch.json"
    output_path = work / f"{identifier}.results.jsonl"
    log_path = work / f"{identifier}.log"
    batch_path.write_text(json.dumps({"root": str(root), "blender": version, "files": files}), encoding="utf-8")
    command = [
        str(blender), "--background", "--factory-startup", "--disable-autoexec",
        "--python", str(Path(__file__).resolve()), "--", "--worker",
        str(batch_path), str(output_path),
    ]
    startup = time.time()
    timed_out = False
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            command, stdout=log, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        while process.poll() is None:
            output = records(output_path)
            active = next((item for item in reversed(output) if item["status"] == "started"), None)
            last_start = active["started_at"] if active else startup
            if time.time() - last_start > timeout:
                timed_out = True
                process.kill()
                process.wait()
                break
            time.sleep(0.25)
    output = records(output_path)
    finished = {item["path"]: item for item in output if item["status"] != "started"}
    active = next((item for item in reversed(output) if item["status"] == "started" and item["path"] not in finished), None)
    remaining = [item for item in files if item["path"] not in finished]
    if remaining:
        failed = active or remaining[0]
        failure = {
            **failed,
            "root": str(root), "blender": version, "status": "failed",
            "error": "Blender import timed out" if timed_out else f"Blender worker exited {process.returncode} without a result",
            "log": str(log_path),
            "finished_at": time.time(),
        }
        finished[failure["path"]] = failure
        with output_path.open("a", encoding="utf-8") as stream:
            append_record(stream, failure)
        remaining = [item for item in remaining if item["path"] != failure["path"]]
        if remaining:
            finished.update((item["path"], item) for item in run_batch(blender, root, version, remaining, work, timeout))
    for item in finished.values():
        item["log"] = item.get("log", str(log_path))
    return list(finished.values())


def write_report(path, root, blender, results, total, started):
    items = sorted(results.values(), key=lambda item: item["path"])
    value = {
        "schema_version": 1,
        "kind": "blender_gltf_import_audit",
        "root": str(root),
        "blender": blender,
        "total": total,
        "completed": len(items),
        "passed": sum(item["status"] == "passed" for item in items),
        "failed": sum(item["status"] == "failed" for item in items),
        "complete": len(items) == total,
        "seconds": round(time.monotonic() - started, 3),
        "import_seconds": round(sum(item.get("seconds", 0) for item in items), 3),
        "peak_worker_bytes": max((item.get("peak_working_set_bytes", 0) for item in items), default=0),
        "files": items,
    }
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def load_cached(paths, root, version, files, retry_failed=False):
    results = {}
    for path in paths:
        for item in records(path):
            if item.get("root") != str(root) or item.get("blender") != version or item.get("status") not in ("passed", "failed"):
                continue
            source = files.get(item.get("path"))
            if source and source["fingerprint"] == item.get("fingerprint"):
                if retry_failed and item["status"] == "failed":
                    continue
                previous = results.get(item["path"])
                if previous is None or item.get("finished_at", 0) >= previous.get("finished_at", 0):
                    results[item["path"]] = item
    return results


def audit(args):
    root = args.root.resolve()
    blender = args.blender.resolve()
    if not root.is_dir() or not blender.is_file():
        raise ValueError("root must be an asset directory and --blender an executable")
    version_process = subprocess.run(
        [str(blender), "--version"], capture_output=True, text=True, check=True,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    version = version_process.stdout.splitlines()[0].strip()
    report = args.report.resolve()
    report.parent.mkdir(parents=True, exist_ok=True)
    checkpoint = report.with_suffix(".jsonl")
    work = report.with_suffix(".workers")
    work.mkdir(parents=True, exist_ok=True)
    if args.files:
        paths = [root / line for line in args.files.read_text(encoding="utf-8").splitlines() if line.strip()]
        if any(not path.resolve().is_relative_to(root) for path in paths):
            raise ValueError("--files contains a path outside the asset directory")
    else:
        paths = sorted(path for path in root.rglob("*") if path.is_file() and path.suffix.lower() in (".gltf", ".glb"))
    files = {path.relative_to(root).as_posix(): {"path": path.relative_to(root).as_posix(), "fingerprint": fingerprint(path)} for path in paths}
    if not files:
        raise ValueError("no glTF files found")
    results = {} if args.no_resume else load_cached(
        [checkpoint, *sorted(work.glob("*.results.jsonl"))], root, version, files, args.retry_failed,
    )
    pending = [source for name, source in files.items() if name not in results]
    batches = [pending[start:start + args.batch_size] for start in range(0, len(pending), args.batch_size)]
    started = time.monotonic()
    write_report(report, root, version, results, len(files), started)
    print(f"{version}: {len(files)} files, {len(results)} cached, {args.jobs} workers", flush=True)
    with checkpoint.open("a", encoding="utf-8") as stream, concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(run_batch, blender, root, version, batch, work, args.timeout) for batch in batches]
        for future in concurrent.futures.as_completed(futures):
            for item in future.result():
                results[item["path"]] = item
                append_record(stream, item)
            write_report(report, root, version, results, len(files), started)
            failures = sum(item["status"] == "failed" for item in results.values())
            print(f"{len(results)}/{len(files)} imported; {failures} failures", flush=True)
    return int(any(item["status"] == "failed" for item in results.values()))


def verify_published(report_path, published):
    """Bind a completed snapshot audit to the exact final published files."""
    report_path, published = Path(report_path), Path(published).resolve()
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if not report["complete"] or report["failed"] or report["passed"] != report["total"]:
        raise ValueError("publication identity requires a complete passing import audit")
    snapshot = Path(report["root"])
    imported = {item["path"]: item for item in report["files"]}
    actual = {path.relative_to(published).as_posix(): path for path in published.rglob("*") if path.is_file() and path.suffix.lower() in (".gltf", ".glb")}
    if set(imported) != set(actual):
        raise ValueError(f"published glTF coverage differs: {len(imported)} audited, {len(actual)} published")
    same_files, hashed_files = 0, 0
    for name, item in imported.items():
        original, final = snapshot / name, actual[name]
        if fingerprint(original) != item["fingerprint"]:
            raise ValueError(f"audited file changed after import: {name}")
        if os.path.samefile(original, final):
            same_files += 1
        else:
            def digest(path):
                value = hashlib.sha256()
                with path.open("rb") as stream:
                    for block in iter(lambda: stream.read(1 << 20), b""):
                        value.update(block)
                return value.digest()
            before = fingerprint(final)
            if digest(original) != digest(final) or fingerprint(final) != before:
                raise ValueError(f"published file differs from imported snapshot: {name}")
            hashed_files += 1
    report["published_identity"] = {
        "status": "passed", "root": str(published), "files": len(imported),
        "same_physical_files": same_files, "sha256_matches": hashed_files,
    }
    temporary = report_path.with_suffix(report_path.suffix + ".tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(report_path)
    print(f"Published identity verified: {same_files} identical physical files, {hashed_files} SHA-256 matches", flush=True)
    return report["published_identity"]


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    if arguments and arguments[0] == "--worker":
        worker(Path(arguments[1]), Path(arguments[2]))
        return 0
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", type=Path)
    parser.add_argument("--blender", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--timeout", type=float, default=120, help="maximum seconds per import, including startup")
    parser.add_argument("--files", type=Path, help="optional newline-separated paths relative to root")
    parser.add_argument("--no-resume", action="store_true")
    parser.add_argument("--retry-failed", action="store_true")
    parser.add_argument("--verify-published", type=Path, help="prove the final published files match the imported snapshot")
    parser.add_argument("--verify-only", action="store_true", help="only verify a completed report against --verify-published")
    args = parser.parse_args(arguments)
    if not 1 <= args.jobs <= 32 or not 1 <= args.batch_size <= 256 or args.timeout <= 0:
        parser.error("jobs must be 1..32, batch-size 1..256, and timeout positive")
    if args.verify_only:
        if not args.verify_published:
            parser.error("--verify-only requires --verify-published")
        verify_published(args.report.resolve(), args.verify_published)
        return 0
    result = audit(args)
    if not result and args.verify_published:
        verify_published(args.report.resolve(), args.verify_published)
    return result


if __name__ == "__main__":
    sys.exit(main())
