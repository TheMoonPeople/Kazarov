#!/usr/bin/env python3
"""Build deterministic LLVM short COFF import libraries without modifying an SDK."""

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True, encoding="utf-8")


def number(text, key):
    matches = re.findall(r"^\s*" + re.escape(key) + r": (0x[0-9a-fA-F]+|[0-9]+)\s*$", text, re.M)
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {key}")
    return int(matches[0], 0)


def exports_from_reader(text):
    """Classify exports by their section and reject PE export-directory forwarders."""
    if not re.search(r"^Format: COFF-x86-64$", text, re.M):
        raise ValueError("only COFF-x86-64 DLLs are supported")
    if "IMAGE_FILE_DLL" not in text:
        raise ValueError("input is not a DLL")
    export_start = number(text, "ExportTableRVA")
    export_end = export_start + number(text, "ExportTableSize")
    sections = []
    for block in re.findall(r"^  Section \{(.*?)^  \}", text, re.M | re.S):
        start = number(block, "VirtualAddress")
        size = number(block, "VirtualSize")
        if not size:
            raise ValueError("empty section virtual range")
        sections.append((start, start + size, "IMAGE_SCN_CNT_CODE" in block))
    exports = []
    names = set()
    ordinals = set()
    for block in re.findall(r"^Export \{(.*?)^\}", text, re.M | re.S):
        ordinal = number(block, "Ordinal")
        rva = number(block, "RVA")
        match = re.search(r"^  Name: *(.*?)\s*$", block, re.M)
        if match is None:
            raise ValueError("export has no name field")
        name = match[1]
        if name and not re.fullmatch(r"[A-Za-z_?$@][A-Za-z0-9_?$@.]*", name):
            raise ValueError(f"unsupported export name: {name!r}")
        if not 1 <= ordinal <= 65535 or ordinal in ordinals or (name and name in names):
            raise ValueError("duplicate export name/ordinal or invalid ordinal")
        if export_start <= rva < export_end or "Forwarder:" in block:
            raise ValueError(f"forwarded export is unsupported: {name or ordinal}")
        containing = [section for section in sections if section[0] <= rva < section[1]]
        if len(containing) != 1:
            raise ValueError(f"ambiguous or unmapped export RVA: {name or ordinal}")
        names.add(name)
        ordinals.add(ordinal)
        exports.append((ordinal, name, not containing[0][2]))
    if not exports:
        raise ValueError("DLL has no exports")
    return sorted(exports)


def identity(dll, reader, dlltool):
    tools = []
    for tool in (reader, dlltool):
        version = subprocess.run([str(tool), "--version"], capture_output=True, text=True, encoding="utf-8")
        # llvm-dlltool has no --version on LLVM releases that expose only its help text.
        help_text = None
        if version.returncode != 0:
            help_result = subprocess.run([str(tool), "--help"], capture_output=True, text=True, encoding="utf-8")
            help_text = (help_result.stdout + help_result.stderr).strip()
        tools.append({"path": str(tool.resolve()), "sha256": digest(tool),
                      "version": version.stdout.strip() if version.returncode == 0 else None,
                      "help": help_text})
    return {
        "dll": {"path": str(dll.resolve()), "sha256": digest(dll)},
        "tools": tools,
        "generator_sha256": digest(__file__),
    }


def key_for(record):
    return hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()


@contextlib.contextmanager
def locked(path):
    with path.open("a+b") as stream:
        stream.seek(0)
        stream.write(b"\0")
        stream.flush()
        stream.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX)
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


def generate(dll, reader, dlltool, output, record):
    output.mkdir(parents=True, exist_ok=True)
    library = output / "imports.lib"
    definition = output / "exports.def"
    manifest = output / "manifest.json"
    with locked(output / "generator.lock"):
        try:
            old = json.loads(manifest.read_text())
            if (old["identity"] == record and old["outputs"] ==
                    {path.name: digest(path) for path in (library, definition)}):
                return
        except (OSError, ValueError, KeyError):
            pass
        exports = exports_from_reader(run(reader, "--file-headers", "--sections", "--coff-exports", dll))
        if not re.fullmatch(r"[A-Za-z0-9_.-]+\.dll", dll.name, re.I):
            raise ValueError("unsupported DLL basename")
        with tempfile.TemporaryDirectory(prefix="imports-", dir=output) as temporary:
            temporary = Path(temporary)
            temp_def = temporary / definition.name
            temp_lib = temporary / library.name
            temp_def.write_text(f"LIBRARY {dll.name}\nEXPORTS\n" + "".join(
                f"  {name or ('ordinal_' + str(ordinal))} @{ordinal}"
                + (" NONAME" if not name else "") + (" DATA" if data else "") + "\n"
                for ordinal, name, data in exports), encoding="ascii", newline="\n")
            subprocess.run([str(dlltool), "-m", "i386:x86-64", "-d", str(temp_def),
                            "-D", dll.name, "-l", str(temp_lib)], check=True)
            if not temp_lib.read_bytes().startswith(b"!<arch>\n"):
                raise ValueError("llvm-dlltool did not produce an import archive")
            metadata = {"identity": record, "cache_key": key_for(record), "exports": len(exports),
                        "data_exports": sum(item[2] for item in exports),
                        "outputs": {path.name: digest(path) for path in (temp_lib, temp_def)}}
            temp_manifest = temporary / manifest.name
            temp_manifest.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            for source, destination in ((temp_lib, library), (temp_def, definition), (temp_manifest, manifest)):
                os.replace(source, destination)


def copy_runtime(executable, reader, compiler_bin):
    """Copy only the selected compiler's ASan runtime requested by the linked image."""
    text = run(reader, "--coff-imports", executable)
    names = re.findall(r"^  Name: (.+)$", text, re.M)
    runtimes = {name for name in names if "asan_dynamic" in name.lower()}
    for name in sorted(runtimes):
        if not re.fullmatch(r"libclang_rt\.asan_dynamic-x86_64\.dll", name, re.I):
            raise ValueError(f"unsupported ASan runtime import: {name}")
        source = compiler_bin.resolve() / name
        if not source.is_file() or source.resolve().parent != compiler_bin.resolve():
            raise ValueError(f"compiler-matching ASan runtime is missing: {source}")
        destination = executable.resolve().parent / name
        if destination.exists() and digest(destination) == digest(source):
            continue
        with tempfile.NamedTemporaryFile(prefix="asan-runtime-", dir=destination.parent, delete=False) as stream:
            temporary = Path(stream.name)
        try:
            shutil.copyfile(source, temporary)
            os.replace(temporary, destination)
        finally:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--readobj", type=Path, required=True)
    parser.add_argument("--dlltool", type=Path)
    parser.add_argument("--runtime-executable", type=Path)
    parser.add_argument("--compiler-bin", type=Path)
    parser.add_argument("--skip-missing-executable", action="store_true")
    parser.add_argument("--describe", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--expected-key")
    args = parser.parse_args()
    if args.runtime_executable:
        if args.compiler_bin is None:
            parser.error("--compiler-bin is required with --runtime-executable")
        if args.skip_missing_executable and not args.runtime_executable.exists():
            return
        copy_runtime(args.runtime_executable, args.readobj, args.compiler_bin)
        return
    if args.dll is None or args.dlltool is None:
        parser.error("--dll and --dlltool are required for import generation")
    record = identity(args.dll, args.readobj, args.dlltool)
    key = key_for(record)
    if args.describe:
        print(key)
        return
    if args.expected_key and args.expected_key != key:
        raise ValueError("DLL, tool or generator changed; reconfigure CMake")
    if args.output_dir is None:
        parser.error("--output-dir is required unless --describe is used")
    generate(args.dll, args.readobj, args.dlltool, args.output_dir, record)


if __name__ == "__main__":
    main()
