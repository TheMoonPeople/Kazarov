#!/usr/bin/env python3
"""Generate narrow diagnostic scopes for GoogleTest 1.18's generated declarations."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile


PINNED_HEADERS = {
    "gtest/gtest.h": "c6f886e33c2e426d6f634112b12b0041aa0bad85b9f897be853b083c14aea169",
    "gtest/gtest-param-test.h": "7a246e017cb84010ae4bad5b6f789fb16941b4e46a130ffb8de9f328fa19ee78",
    "gtest/internal/gtest-internal.h": "57b904e1b1763a9ac5f381db3717a92e285e707adf093d0f17c5cfc9603a6f34",
    "gtest/internal/gtest-death-test-internal.h": "5a81ad35daf1049940c677b38c908945040cc25f07366b3ddf577ce72d8c44f1",
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def inputs(include_dir):
    texts = {}
    record = {"generator_sha256": sha(Path(__file__).read_bytes()), "headers": {}}
    for relative, expected in PINNED_HEADERS.items():
        path = include_dir / relative
        raw = path.read_bytes()
        normalized = raw.replace(b"\r\n", b"\n")
        if sha(normalized) != expected:
            raise ValueError(f"unsupported GoogleTest header {path}; expected the pinned GoogleTest 1.18 source")
        texts[relative] = normalized.decode("utf-8")
        record["headers"][relative] = {"path": str(path.resolve()), "sha256": sha(raw),
                                        "normalized_sha256": sha(normalized)}
    return texts, record


def cache_key(record):
    return sha(json.dumps(record, sort_keys=True).encode())


def macro(text, name):
    lines = text.splitlines()
    positions = [index for index, line in enumerate(lines) if re.match(r"^#define " + re.escape(name) + r"\(", line)]
    if len(positions) != 1:
        raise ValueError(f"expected exactly one definition of {name}")
    index = positions[0]
    definition = []
    while True:
        line = lines[index].rstrip()
        continued = line.endswith("\\")
        definition.append(line[:-1] if continued else line)
        index += 1
        if not continued:
            break
    joined = " ".join(definition)
    match = re.fullmatch(r"#define " + re.escape(name) + r"(\([^)]*\))\s+(.*)", joined)
    if match is None:
        raise ValueError(f"malformed {name} definition")
    return match[1], match[2].strip()


def scope(code, warning):
    return ('_Pragma("clang diagnostic push") '
            f'_Pragma("clang diagnostic ignored \\\"{warning}\\\"") '
            + code + ' _Pragma("clang diagnostic pop") ')


def class_end(body, opening):
    """Find a generated class's closing brace while ignoring literals and comments."""
    tokens = re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\{|\}', body[opening:])
    depth = 0
    for token in tokens:
        if token[0] == "{":
            depth += 1
        elif token[0] == "}":
            depth -= 1
            if depth == 0:
                closing = opening + token.end()
                suffix = re.match(r"\s*;", body[closing:])
                if suffix is None:
                    break
                return closing + suffix.end()
    raise ValueError("generated class has no balanced closing declaration")


def test_declaration(body, parameterized):
    start = re.search(r"\bclass\s+GTEST_TEST_CLASS_NAME_\(", body)
    signature = re.search(r"\bvoid\s+GTEST_TEST_CLASS_NAME_\([^)]*\)::TestBody\(\)\s*$", body)
    if start is None or signature is None:
        raise ValueError("test declaration no longer has a generated class and trailing TestBody signature")
    closing = class_end(body, body.index("{", start.end()))
    registration = body[closing:signature.start()].strip()
    expected = "::AddToRegistry();" if parameterized else "::testing::internal::MakeAndRegisterTestInfo("
    if expected not in registration or not registration.endswith(";"):
        raise ValueError("test registration structure changed")
    if parameterized and not registration.startswith("int GTEST_TEST_CLASS_NAME_"):
        raise ValueError("parameterized registration declaration changed")
    if not parameterized and not registration.startswith("::testing::TestInfo*"):
        raise ValueError("test registration declaration changed")
    return (body[:start.start()] + scope(body[start.start():closing], "-Wpadded")
            + scope(registration, "-Wglobal-constructors") + body[signature.start():])


def instantiate(body):
    registration = re.search(r"\[\[maybe_unused\]\]\s+static\s+int\s+gtest_##prefix##test_suite_name##_dummy_", body)
    if registration is None or not body.endswith("__LINE__)"):
        raise ValueError("parameterized suite registration structure changed")
    suffix = body[registration.start():]
    if suffix.count("->AddTestSuiteInstantiation(") != 1 or "__VA_ARGS__" in suffix:
        raise ValueError("parameter generators are no longer separate from suite registration")
    return body[:registration.start()] + scope(suffix, "-Wglobal-constructors")


def death_test(body):
    switch = re.search(r"switch\s*\(gtest_dt->AssumeRole\(\)\)\s*\{", body)
    if switch is None or body.count("GTEST_EXECUTE_DEATH_TEST_STATEMENT_(statement, gtest_dt)") != 1:
        raise ValueError("death-test generated switch structure changed")
    # Restore diagnostics at the opening brace, before every case and the user's statement.
    return body[:switch.start()] + scope(switch[0], "-Wswitch-default") + body[switch.end():]


def adapter(texts):
    definitions = [
        ("GTEST_TEST_", "gtest/internal/gtest-internal.h", lambda body: test_declaration(body, False)),
        ("TEST_P", "gtest/gtest-param-test.h", lambda body: test_declaration(body, True)),
        ("INSTANTIATE_TEST_SUITE_P", "gtest/gtest-param-test.h", instantiate),
        ("GTEST_DEATH_TEST_", "gtest/internal/gtest-death-test-internal.h", death_test),
    ]
    output = ['#pragma once', '', '/**', ' * Scopes diagnostics to GoogleTest-generated declarations while retaining native test diagnostics.',
              ' */', '#include <gtest/gtest.h>', '', '#if defined(__clang__)', '#pragma clang system_header']
    for name, relative, transform in definitions:
        parameters, body = macro(texts[relative], name)
        output += [f'#if defined({name})', f'#undef {name}', f'#define {name}{parameters} {transform(body)}', '#endif', '']
    output += ['#endif', '']
    return '\n'.join(output).encode()


def generate(texts, record, output):
    content = adapter(texts)
    manifest = output.with_suffix(".json")
    metadata = {"identity": record, "cache_key": cache_key(record), "adapter_sha256": sha(content)}
    metadata_bytes = (json.dumps(metadata, sort_keys=True, indent=2) + "\n").encode()
    if output.exists() and manifest.exists() and output.read_bytes() == content and manifest.read_bytes() == metadata_bytes:
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gtest-diagnostics-", dir=output.parent) as directory:
        temporary = Path(directory)
        header = temporary / output.name
        header.write_bytes(content)
        stamp = temporary / manifest.name
        stamp.write_bytes(metadata_bytes)
        os.replace(header, output)
        os.replace(stamp, manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--include-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--describe", action="store_true")
    parser.add_argument("--expected-key")
    args = parser.parse_args()
    texts, record = inputs(args.include_dir)
    key = cache_key(record)
    if args.describe:
        adapter(texts)
        print(key)
        return
    if args.expected_key and key != args.expected_key:
        raise ValueError("GoogleTest or the diagnostics generator changed; reconfigure CMake")
    if args.output is None:
        parser.error("--output is required unless --describe is used")
    generate(texts, record, args.output)


if __name__ == "__main__":
    main()
