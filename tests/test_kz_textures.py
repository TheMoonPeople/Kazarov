import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from kazarov_textures import (Converter, TextureError, parse_cfg, parse_img, parse_pack,
                              parse_tim2, png, publish, verify)


def picture(width, height, image_type, pixels, clut=b"", mips=None):
    mips = mips or [pixels]
    header_size = 0x50 if len(mips) > 1 else 0x30
    header = bytearray(header_size)
    image_size = sum(len(level) for level in mips)
    struct.pack_into("<IIIHH", header, 0, header_size + image_size + len(clut), len(clut),
                     image_size, header_size, 256 if image_type == 5 else 16)
    header[17:20] = bytes((len(mips), 3, image_type))
    struct.pack_into("<HH", header, 20, width, height)
    if len(mips) > 1:
        for i, level in enumerate(mips):
            struct.pack_into("<I", header, 0x40 + 4 * i, len(level))
    return bytes(header) + b"".join(mips) + clut


def tim2(*pictures):
    header = bytearray(16)
    header[:4] = b"TIM2"
    struct.pack_into("<H", header, 6, len(pictures))
    return bytes(header) + b"".join(pictures)


def img(name, texture):
    header = bytearray(16 + 64)
    header[:4] = b"IM3\0"
    struct.pack_into("<I", header, 8, 1)
    header[16:16 + len(name)] = name
    struct.pack_into("<I", header, 16 + 36, len(header))
    return bytes(header) + texture


def img_multi(entries):
    header = bytearray(16 + 64 * len(entries))
    header[:4] = b"IM3\0"
    struct.pack_into("<I", header, 8, len(entries))
    payload = bytearray()
    for i, (name, body) in enumerate(entries):
        at = 16 + 64 * i
        header[at:at + len(name)] = name
        struct.pack_into("<I", header, at + 36, len(header) + len(payload))
        struct.pack_into("<I", header, at + 52, len(body))
        payload.extend(body)
    return bytes(header + payload)


def pack(members):
    table = bytearray((len(members) + 1) * 76)
    payload = bytearray()
    for i, (name, body) in enumerate(members):
        at = i * 76
        table[at:at + len(name)] = name
        struct.pack_into("<III", table, at + 64, len(table) + len(payload) - at, len(body), 76)
        payload.extend(body)
    return bytes(table + payload)


class TextureConversionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=Path.cwd())
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_indexed_sources_become_rgba_pngs(self):
        clut4 = b"".join(bytes((i, 0, 0, 64)) for i in range(16))
        t4 = tim2(picture(2, 2, 4, b"\x10\x32", clut4))
        raw = self.root / "raw" / "img"
        raw.mkdir(parents=True)
        (raw / "icons.img").write_bytes(img(b"icons", t4))
        clut8 = b"".join(bytes((i, 0, 0, 64)) for i in range(256))
        (raw / "indices.tm2").write_bytes(tim2(picture(2, 2, 5, bytes((4, 5, 6, 7)), clut8)))
        output = self.root / "staged"
        Converter(self.root / "raw", output).run()
        catalog = json.loads((output / "port_data/catalog.json").read_text())
        self.assertEqual([source["kind"] for source in catalog["sources"]], ["img3", "tm2"])
        for source, expected in zip(catalog["sources"], [b"\x00\x01\x02\x03", b"\x04\x05\x06\x07"]):
            entry = source["entries"][0]
            level = Path(entry["variants"][0]["levels"][0])
            self.assertEqual(level.parent.suffix, ".img")
            bundle = output / "port_data" / level.parent
            self.assertEqual([p.name for p in bundle.glob("*.json")], ["catalog.json"])
            local = json.loads((bundle / "catalog.json").read_text())
            self.assertEqual(local["schema_version"], 1)
            self.assertEqual(local["source"], source["source"])
            self.assertEqual(local["entries"][0]["variants"][0]["levels"][0], level.name)
            image = (bundle / level.name).read_bytes()
            self.assertEqual(image[25], 6)  # PNG IHDR RGBA colour type
            self.assertNotIn(b"PLTE", image)
            start = image.index(b"IDAT") + 4
            compressed_size = struct.unpack_from(">I", image, start - 8)[0]
            scanlines = zlib.decompress(image[start:start + compressed_size])
            rgba = lambda values: b"".join(bytes((i, 0, 0, 128)) for i in values)
            self.assertEqual(scanlines, b"\0" + rgba(expected[:2]) + b"\0" + rgba(expected[2:]))
            self.assertEqual(entry["bpp"], 32)
            self.assertNotIn("palette", entry)
        self.assertFalse((output / "data/img/icons.img").exists())
        self.assertFalse((output / "data/img/indices.tm2").exists())
        self.assertEqual(verify(output), 5)

    def test_multiple_pictures_and_mips(self):
        first = picture(4, 4, 3, bytes((1, 2, 3, 64)) * 16,
                        mips=[bytes((1, 2, 3, 64)) * 16, bytes((4, 5, 6, 32)) * 4])
        second = picture(1, 1, 2, b"\x07\x08\x09")
        pictures = parse_tim2(tim2(first, second))
        self.assertEqual((len(pictures), len(pictures[0].levels), pictures[1].bpp), (2, 2, 24))
        self.assertEqual(pictures[0].levels[1][2][:4], bytes((4, 5, 6, 64)))

    def test_cfg_palette_copy_has_baked_rgba_destination_variant(self):
        red = b"".join(bytes((i, 0, 0, 64)) for i in range(256))
        blue = b"".join(bytes((0, 0, i, 64)) for i in range(256))
        script = b'TEX_ANIME "group",1;TEX_ANIME_DATA 1,"copy";SRC_TEX "source",0,0,1,1;DEST_TEX "dest",0,0,1,1;CLUT_COPY 1;TEX_ANIME_DATA_END;TEX_ANIME_END;'
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "anim.img").write_bytes(img_multi([
            (b"source", tim2(picture(1, 1, 5, b"\x07", blue))),
            (b"dest", tim2(picture(1, 1, 5, b"\x07", red))),
            (b"#texanime", script),
        ]))
        output = self.root / "output"
        Converter(raw, output).run()
        entries = json.loads((output / "port_data/catalog.json").read_text())["sources"][0]["entries"]
        dest = next(entry for entry in entries if entry["type"] == "texture" and entry["name"] == "dest")
        variants = {item["palette_source"]: item["levels"][0] for item in dest["variants"]}
        self.assertEqual(set(variants), {"dest", "source"})
        image = (output / "port_data" / variants["source"]).read_bytes()
        start = image.index(b"IDAT") + 4
        size = struct.unpack_from(">I", image, start - 8)[0]
        self.assertEqual(zlib.decompress(image[start:start + size]), b"\0\x00\x00\x07\x80")

    def test_transitive_palette_copy_bakes_reachable_origin(self):
        palette = lambda channel: b"".join(bytes((i if channel == 0 else 0,
                                                     i if channel == 1 else 0,
                                                     i if channel == 2 else 0, 64)) for i in range(256))
        script = (b'SRC_TEX "source",0,0,1,1;DEST_TEX "middle",0,0,1,1;TEX_ANIME_DATA_END;'
                  b'SRC_TEX "middle",0,0,1,1;DEST_TEX "dest",0,0,1,1;TEX_ANIME_DATA_END;')
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "chain.img").write_bytes(img_multi([
            (b"source", tim2(picture(1, 1, 5, b"\x07", palette(2)))),
            (b"middle", tim2(picture(1, 1, 5, b"\x07", palette(1)))),
            (b"dest", tim2(picture(1, 1, 5, b"\x07", palette(0)))),
            (b"#texanime", script),
        ]))
        output = self.root / "out"
        Converter(raw, output).run()
        entries = json.loads((output / "port_data/catalog.json").read_text())["sources"][0]["entries"]
        dest = next(entry for entry in entries if entry["type"] == "texture" and entry["name"] == "dest")
        self.assertEqual({variant["palette_source"] for variant in dest["variants"]},
                         {"source", "middle", "dest"})

    def test_cfg_only_img_has_bundle_catalog_without_png(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "only.img").write_bytes(img_multi([(b"#texanime", b"TEX_ANIME_END;")]))
        output = self.root / "output"
        Converter(raw, output).run()
        bundle = output / "port_data/only.img"
        self.assertEqual([path.name for path in bundle.iterdir()], ["catalog.json"])
        local = json.loads((bundle / "catalog.json").read_text())
        self.assertEqual(local["entries"][0]["type"], "cfg")
        verify(output)

    def test_no_image_entry_keeps_metadata_without_png(self):
        texture = bytearray(img_multi([(b"empty", tim2(picture(1, 1, 3, b"\x00" * 4)))]))
        struct.pack_into("<H", texture, 16 + 48, 1)
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "empty.img").write_bytes(texture)
        output = self.root / "output"
        Converter(raw, output).run()
        bundle = output / "port_data/empty.img"
        local = json.loads((bundle / "catalog.json").read_text())
        entry = local["entries"][0]
        self.assertTrue(entry["no_image"])
        self.assertEqual(entry["variants"][0]["levels"], [])
        self.assertEqual([path.name for path in bundle.iterdir()], ["catalog.json"])

    def test_transient_png_write_error_retries(self):
        converter = Converter(self.root / "raw", self.root / "output")
        original_write = Path.write_bytes
        attempts = [0]
        def flaky_write(path, content):
            attempts[0] += 1
            if attempts[0] == 1:
                raise OSError(22, "transient Windows write failure")
            return original_write(path, content)
        with patch.object(Path, "write_bytes", flaky_write), patch("kazarov_textures.time.sleep"):
            relative = converter.write_asset("bundle.img", "image", b"PNG")
        self.assertEqual(attempts[0], 2)
        self.assertEqual((converter.assets / relative).read_bytes(), b"PNG")

    def test_serial_parallel_outputs_identical(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "ordinary.txt").write_text("ordinary")
        for name, color in (("a.img", 1), ("b.img", 2)):
            (raw / name).write_bytes(img(name.encode(), tim2(picture(1, 1, 3,
                                                bytes((color, 2, 3, 64))))))
        serial = self.root / "serial"
        parallel = self.root / "parallel"
        Converter(raw, serial).run(jobs=1)
        Converter(raw, parallel).run(jobs=4)
        serial_files = {p.relative_to(serial).as_posix(): p.read_bytes()
                        for p in serial.rglob("*") if p.is_file()}
        parallel_files = {p.relative_to(parallel).as_posix(): p.read_bytes()
                          for p in parallel.rglob("*") if p.is_file()}
        self.assertEqual(serial_files, parallel_files)

    def test_parallel_worker_failures_are_aggregated_without_marker(self):
        raw = self.root / "raw"
        raw.mkdir()
        for name in ("good.txt", "bad1.txt", "bad2.txt"):
            (raw / name).write_text(name)
        original = Converter.process_file
        def fail_two(converter, path):
            if path.name.startswith("bad"):
                raise TextureError("injected worker failure")
            return original(converter, path)
        output = self.root / "output"
        with patch.object(Converter, "process_file", fail_two):
            with self.assertRaisesRegex(TextureError, "2 texture conversion worker") as error:
                Converter(raw, output).run(jobs=3)
        self.assertIn("bad1.txt", str(error.exception))
        self.assertIn("bad2.txt", str(error.exception))
        self.assertFalse((output / "data.json").exists())

    def test_pack_strips_texture_member_and_preserves_other_member(self):
        texture = img(b"one", tim2(picture(1, 1, 3, b"\x01\x02\x03\x40")))
        original = pack([(b"one.img", texture), (b"note.txt", b"keep me")])
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "bundle.pac").write_bytes(original)
        Converter(raw, self.root / "out").run()
        stripped = (self.root / "out/data/bundle.pac").read_bytes()
        members = parse_pack(stripped)
        self.assertIsNone(members)  # zero-size texture entry makes retail PACK parser reject it
        self.assertEqual(struct.unpack_from("<I", stripped, 68)[0], 0)
        self.assertNotIn(b"TIM2", stripped)
        self.assertIn(b"keep me", stripped)
        catalog = json.loads((self.root / "out/port_data/catalog.json").read_text())
        self.assertEqual(catalog["sources"][0]["source"], "bundle.pac/one.img")

    def test_malformed_picture_and_cfg_rejected(self):
        with self.assertRaises(TextureError):
            parse_tim2(tim2(picture(4, 4, 3, b"\x00" * 64)[:-1]))
        with self.assertRaises(TextureError):
            parse_cfg(b"SCROLL nope;")

    def test_embedded_img_preserves_wrapper_and_offsets(self):
        texture = img(b"embedded", tim2(picture(1, 1, 3, b"\x01\x02\x03\x40")))
        wrapper = b"W" * 80 + texture + b"TRAILER"
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "model.chr").write_bytes(wrapper)
        output = self.root / "out"
        Converter(raw, output).run()
        converted = (output / "data/model.chr").read_bytes()
        self.assertEqual(converted, b"W" * 80 + bytes(len(texture)) + b"TRAILER")
        catalog = json.loads((output / "port_data/catalog.json").read_text())
        self.assertEqual(catalog["sources"][0]["source"], "model.chr#offset:80")

    def test_versioned_json_manifests_and_publish_check(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "ordinary.txt").write_text("ordinary data")
        staged = self.root / "staged"
        Converter(raw, staged).run()
        destination = self.root / "game"
        publish(staged, destination)
        data_manifest = json.loads((destination / "data/manifest.json").read_text())
        self.assertEqual(data_manifest["schema_version"], 1)
        self.assertEqual(verify(destination), 1)
        (destination / "data/ordinary.txt").write_text("changed")
        with self.assertRaises(TextureError):
            verify(destination)

    def test_publish_restores_both_trees_and_marker_on_failed_verification(self):
        destination = self.root / "game"
        old_raw = self.root / "old-raw"
        old_raw.mkdir()
        (old_raw / "old.txt").write_text("old")
        old_stage = self.root / "old-stage"
        Converter(old_raw, old_stage).run()
        publish(old_stage, destination)
        old_marker = (destination / "data.json").read_bytes()
        new_raw = self.root / "new-raw"
        new_raw.mkdir()
        (new_raw / "new.txt").write_text("new")
        new_stage = self.root / "new-stage"
        Converter(new_raw, new_stage).run()
        with patch("kazarov_textures.verify", side_effect=[1, TextureError("injected failure")]):
            with self.assertRaises(TextureError):
                publish(new_stage, destination)
        self.assertEqual((destination / "data/old.txt").read_text(), "old")
        self.assertFalse((destination / "data/new.txt").exists())
        self.assertEqual((destination / "data.json").read_bytes(), old_marker)
        verify(destination)
        verify(new_stage)


if __name__ == "__main__":
    unittest.main()
