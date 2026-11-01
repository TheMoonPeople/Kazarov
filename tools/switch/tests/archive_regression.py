#!/usr/bin/env python3
"""Exercise the native indexed ZIP reader through its validation probe."""
import argparse
import ctypes
import ctypes.util
import io
import json
import random
import re
import struct
import subprocess
import tempfile
import zipfile
import zlib
from pathlib import Path


def fnv(data):
    result = 14695981039346656037
    for byte in data:
        result = ((result ^ byte) * 1099511628211) & 0xffffffffffffffff
    return result


def run(probe, path, indices, reject=False):
    result = subprocess.run([str(probe), str(path), *map(str, indices)], capture_output=True, text=True, encoding='utf-8')
    if reject:
        assert result.returncode != 0, f'{path.name}: invalid data accepted'
        return {}
    assert result.returncode == 0, f'{path.name}: {result.stdout} {result.stderr}'
    rows = {}
    for line in result.stdout.splitlines()[1:]:
        match = re.fullmatch(r'(\d+) (\d+) (\d+) (.*?) read_ms ([\d.eE+-]+)', line)
        assert match, line
        rows[int(match[1])] = {'size': int(match[2]), 'hash': int(match[3]), 'name': match[4]}
    return rows


def zstd_library():
    name = ctypes.util.find_library('zstd')
    if not name:
        raise RuntimeError('libzstd is required to independently test method 93')
    library = ctypes.CDLL(name)
    library.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
    library.ZSTD_compressBound.restype = ctypes.c_size_t
    library.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    library.ZSTD_compress.restype = ctypes.c_size_t
    library.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    library.ZSTD_decompress.restype = ctypes.c_size_t
    library.ZSTD_isError.argtypes = [ctypes.c_size_t]
    library.ZSTD_isError.restype = ctypes.c_uint
    return library


def zstd_zip(path, library, payload):
    source = ctypes.create_string_buffer(payload)
    output = ctypes.create_string_buffer(library.ZSTD_compressBound(len(payload)))
    size = library.ZSTD_compress(output, len(output), source, len(payload), 3)
    assert not library.ZSTD_isError(size)
    encoded = output.raw[:size]
    name = b'zstd-asset'
    crc = zlib.crc32(payload)
    local = struct.pack('<IHHHHHIIIHH', 0x04034b50, 63, 0, 93, 0, 0, crc, len(encoded), len(payload), len(name), 0) + name + encoded
    central = struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 63, 63, 0, 93, 0, 0, crc, len(encoded), len(payload), len(name), 0, 0, 0, 0, 0, 0) + name
    path.write_bytes(local + central + struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, 1, 1, len(central), len(local), 0))


def decode_entry(archive, stream, entry, library):
    if entry.compress_type != 93:
        return archive.read(entry)
    stream.seek(entry.header_offset)
    header = stream.read(30)
    name_size, extra_size = struct.unpack_from('<HH', header, 26)
    stream.seek(name_size + extra_size, 1)
    compressed = ctypes.create_string_buffer(stream.read(entry.compress_size))
    output = ctypes.create_string_buffer(entry.file_size)
    size = library.ZSTD_decompress(output, len(output), compressed, entry.compress_size)
    assert not library.ZSTD_isError(size) and size == entry.file_size
    assert zlib.crc32(output.raw) == entry.CRC
    return output.raw


def check(probe, root, archive_path=None):
    library = zstd_library()
    fixture = root / 'fixture.zip'
    payloads = [b'archive-validation\x00' * 100, b'deflated payload' * 100, b'', b'utf8 name', b'child']
    with zipfile.ZipFile(fixture, 'w') as archive:
        archive.writestr('stored', payloads[0], compress_type=zipfile.ZIP_STORED)
        archive.writestr('deflate', payloads[1], compress_type=zipfile.ZIP_DEFLATED)
        archive.writestr('empty-deflate', payloads[2], compress_type=zipfile.ZIP_DEFLATED)
        archive.writestr('folder/caf\u00e9.txt', payloads[3])
        archive.writestr('folder/', b'')
        archive.writestr('folder/child', payloads[4])
    rows = run(probe, fixture, range(5))
    for index, payload in enumerate(payloads):
        assert rows[index]['size'] == len(payload) and rows[index]['hash'] == fnv(payload)
    for payload in [b'zstandard payload' * 100, b'']:
        path = root / 'zstd.zip'
        zstd_zip(path, library, payload)
        assert run(probe, path, [0])[0]['hash'] == fnv(payload)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        for index in range(65536):
            archive.writestr(str(index), b'')
    path = root / 'count64.zip'
    path.write_bytes(buffer.getvalue())
    rows = run(probe, path, [0, 65534, 65535])
    assert all(row['hash'] == fnv(b'') for row in rows.values())
    malformed = {'truncated': b'PK\x05\x06', 'garbage': b'A' * 100}
    for name, offset in [('crc-bad', 30 + len('stored')), ('local-name', 30), ('local-method', 8)]:
        damaged = bytearray(fixture.read_bytes())
        damaged[offset] ^= 1
        malformed[name] = damaged
    for name, data in malformed.items():
        path = root / (name + '.zip')
        path.write_bytes(data)
        run(probe, path, [0], reject=True)
    result = {'fixtures': 'passed', 'malformed_rejected': list(malformed), 'real_assets': 0}
    if archive_path:
        with zipfile.ZipFile(archive_path) as archive, archive_path.open('rb') as stream:
            entries = [entry for entry in archive.infolist() if not entry.filename.endswith('/')]
            selection = {0, len(entries) - 1}
            selection.update(i for i in [65534, 65535, 65536] if i < len(entries))
            selection.update(random.Random(932).sample(range(len(entries)), min(16, len(entries))))
            selection = sorted(i for i in selection if entries[i].file_size <= 1024 * 1024)
            actual = run(probe, archive_path, selection)
            for index in selection:
                data = decode_entry(archive, stream, entries[index], library)
                assert actual[index]['name'] == entries[index].filename
                assert actual[index]['size'] == len(data) and actual[index]['hash'] == fnv(data)
            result['real_assets'] = len(selection)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='kazarov-zip-test-') as directory:
        print(json.dumps(check(args.probe.resolve(), Path(directory), args.archive.resolve() if args.archive else None), indent=2))


if __name__ == '__main__':
    main()
