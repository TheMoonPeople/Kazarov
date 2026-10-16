import json
import hashlib
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from kazarov_textures import (Converter, TextureError, parse_cfg, parse_img, parse_pack,
                              parse_tim2, png, publish, verify, document_path)


def documents(output):
    raw = output.parent / "raw"
    source_paths = {document_path(path.relative_to(raw).as_posix()): path.relative_to(raw).as_posix()
                    for path in raw.rglob("*") if path.is_file()} if raw.is_dir() else {}
    root = output / "port_data"
    return [({"source": source_paths.get(path.relative_to(root).as_posix()),
              "path": path.relative_to(root).as_posix()}, json.loads(path.read_text()))
            for path in sorted(root.rglob("*.json"))]


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

    def test_document_path_rules(self):
        self.assertEqual(document_path("img/upper.IMG"), "img/upper.json")
        self.assertEqual(document_path("pack.img/one.img"), "pack-img/one.json")
        self.assertEqual(document_path("pack.pac/one.img#entry:3"), "pack.pac/one-entry-3.json")
        self.assertEqual(document_path("model.chr#offset:80"),
                         "model.chr/embedded-offset-80.json")
        self.assertEqual(document_path("x.img#entry:1#offset:80"),
                         "x-entry-1.json/embedded-offset-80.json")
        self.assertEqual(document_path("opaque"), "opaque/embedded.json")
        self.assertEqual(document_path("é.img"), "%C3%A9.json")
        self.assertEqual(document_path("å.img#entry:7#offset:80"),
                         "%C3%A5-entry-7.json/embedded-offset-80.json")
        self.assertEqual(document_path("literal%23offset%3A.img"),
                         "literal%2523offset%253A.json")
        long_name = "root/segment/" + "x" * 190 + ".img"
        full = "root/segment/" + "x" * 190 + ".json"
        digest = hashlib.sha256(full.encode("utf-8")).hexdigest()[:20]
        self.assertEqual(document_path(long_name), f"root/segment/long-path-{digest}.json")

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
        self.assertEqual([record["source"] for record, _ in documents(output)],
                         ["img/icons.img", "img/indices.tm2"])
        for record, source, expected in [(r, d, e) for (r, d), e in zip(documents(output),
                [b"\x00\x01\x02\x03", b"\x04\x05\x06\x07"])]:
            entry = source["entries"][0]
            picture_doc = entry["pictures"][0]
            level = Path(picture_doc["levels"][0])
            self.assertEqual(level.name, Path(record["path"]).stem + "_0.png")
            self.assertEqual(source["schema_version"], 3)
            self.assertEqual(set(source), {"schema_version", "groups", "entries"})
            image = (output / "port_data" / Path(record["path"]).parent / level).read_bytes()
            self.assertEqual(image[25], 6)  # PNG IHDR RGBA colour type
            self.assertNotIn(b"PLTE", image)
            start = image.index(b"IDAT") + 4
            compressed_size = struct.unpack_from(">I", image, start - 8)[0]
            scanlines = zlib.decompress(image[start:start + compressed_size])
            rgba = lambda values: b"".join(bytes((i, 0, 0, 128)) for i in values)
            self.assertEqual(scanlines, b"\0" + rgba(expected[:2]) + b"\0" + rgba(expected[2:]))
            self.assertEqual(picture_doc["animation"], "copy" if expected[0] == 0 else "copy_variant")
            self.assertNotIn("palette_asset", picture_doc)
        self.assertFalse((output / "data/img/icons.img").exists())
        self.assertFalse((output / "data/img/indices.tm2").exists())
        self.assertEqual(verify(output), 2)

    def test_multiple_pictures_and_mips(self):
        first = picture(4, 4, 3, bytes((1, 2, 3, 64)) * 16,
                        mips=[bytes((1, 2, 3, 64)) * 16, bytes((4, 5, 6, 32)) * 4])
        second = picture(1, 1, 2, b"\x07\x08\x09")
        pictures = parse_tim2(tim2(first, second))
        self.assertEqual((len(pictures), len(pictures[0].levels), pictures[1].bpp), (2, 2, 24))
        self.assertEqual(pictures[0].levels[1][2][:4], bytes((4, 5, 6, 64)))

    def test_multiple_pictures_serialize_under_one_entry(self):
        raw = self.root / "raw"
        raw.mkdir()
        first = picture(1, 1, 3, b"\x01\x02\x03\x40")
        second = picture(1, 1, 2, b"\x04\x05\x06")
        (raw / "multi.img").write_bytes(img(b"multi", tim2(first, second)))
        output = self.root / "output"
        Converter(raw, output).run()
        document = documents(output)[0][1]
        self.assertEqual(document["groups"], [0])
        self.assertEqual(document["entries"], [{"name": "multi", "pictures": [
            {"levels": ["multi_0.png"]}, {"levels": ["multi_1.png"]}]}])
        verify(output)

    def test_img3_groups_animation_modes_and_sampler_defaults(self):
        clut4 = bytes((0, 0, 0, 64)) * 16
        clut8 = bytes((0, 0, 0, 64)) * 256
        bodies = [(b"draw", tim2(picture(1, 1, 3, b"\x01\x02\x03\x40"))),
                  (b"copy", tim2(picture(1, 1, 4, b"\x01", clut4))),
                  (b"variant", tim2(picture(1, 1, 5, b"\x01", clut8)))]
        data = bytearray(img_multi(bodies))
        for index, block in enumerate((0, 2, 4)):
            struct.pack_into("<i", data, 16 + index * 64 + 44, block)
        struct.pack_into("<Q", data, 16 + 64 + 56, 1 | (2 << 2))
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "modes.img").write_bytes(data)
        output = self.root / "output"
        Converter(raw, output).run()
        document = documents(output)[0][1]
        self.assertEqual(document["groups"], [0, 2, 4])
        draw, copied, variant = document["entries"]
        self.assertEqual(draw, {"name": "draw", "pictures": [{"levels": ["modes_0.png"]}]})
        self.assertEqual(copied["block"], 2)
        self.assertEqual(copied["pictures"][0]["animation"], "copy")
        self.assertEqual(copied["pictures"][0]["sampler"],
                         {"wrap_u": "clamp", "wrap_v": "clamp"})
        self.assertEqual(variant["block"], 4)
        self.assertEqual(variant["pictures"][0]["animation"], "copy_variant")
        self.assertNotIn("sampler", variant["pictures"][0])
        verify(output)

    def test_16_bit_copy_mode_and_single_axis_sampler(self):
        data = bytearray(img(b"sixteen", tim2(picture(1, 1, 1, b"\xff\x7f"))))
        struct.pack_into("<Q", data, 16 + 56, 1)
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "sixteen.img").write_bytes(data)
        output = self.root / "output"
        Converter(raw, output).run()
        picture_doc = documents(output)[0][1]["entries"][0]["pictures"][0]
        self.assertEqual(picture_doc["animation"], "copy")
        self.assertEqual(picture_doc["sampler"], {"wrap_u": "clamp"})
        verify(output)

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
        record, document = documents(output)[0]
        entries = document["entries"]
        dest = next(entry for entry in entries if entry["name"] == "dest")
        variants = {item["selector"]: item["levels"][0] for item in dest["pictures"][0]["variants"]}
        self.assertEqual(set(variants), {"source"})
        image = (output / "port_data" / Path(record["path"]).parent / variants["source"]).read_bytes()
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
        entries = documents(output)[0][1]["entries"]
        dest = next(entry for entry in entries if entry["name"] == "dest")
        self.assertEqual({variant["selector"] for variant in dest["pictures"][0]["variants"]},
                         {"source", "middle"})

    def test_cross_document_palette_variant_keeps_local_png(self):
        palette = b"".join(bytes((i, 0, 0, 64)) for i in range(256))
        raw = self.root / "raw"
        (raw / "donors").mkdir(parents=True)
        (raw / "targets").mkdir()
        (raw / "donors/source.img").write_bytes(img(b"source", tim2(picture(1, 1, 5, b"\x07", palette))))
        (raw / "targets/dest.img").write_bytes(img(b"dest", tim2(picture(1, 1, 5, b"\x07", palette))))
        script = b'SRC_TEX "source",0,0,1,1;DEST_TEX "dest",0,0,1,1;TEX_ANIME_DATA_END;'
        (raw / "anime.img").write_bytes(img_multi([(b"#anime", script)]))
        output = self.root / "output"
        Converter(raw, output).run()
        target = next((record, doc) for record, doc in documents(output)
                      if record["source"] == "targets/dest.img")
        variant = next(item for item in target[1]["entries"][0]["pictures"][0]["variants"]
                       if item["selector"] == "source")
        self.assertEqual(set(variant), {"selector", "levels"})
        self.assertEqual(variant["levels"], ["dest_1.png"])
        verify(output)

    def test_cfg_only_img_has_document_without_png(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "only.img").write_bytes(img_multi([(b"#texanime", b"TEX_ANIME_END;")]))
        output = self.root / "output"
        Converter(raw, output).run()
        bundle = output / "port_data"
        self.assertEqual(sorted(path.name for path in bundle.iterdir()), ["only.json"])
        local = json.loads((bundle / "only.json").read_text())
        self.assertEqual(set(local["entries"][0]), {"name", "script"})
        self.assertEqual(local["groups"], [])
        verify(output)

    def test_no_image_entry_keeps_metadata_without_png(self):
        texture = bytearray(img_multi([(b"empty", tim2(picture(1, 1, 3, b"\x00" * 4)))]))
        struct.pack_into("<H", texture, 16 + 48, 1)
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "empty.img").write_bytes(texture)
        output = self.root / "output"
        Converter(raw, output).run()
        bundle = output / "port_data"
        local = json.loads((bundle / "empty.json").read_text())
        entry = local["entries"][0]
        self.assertEqual(entry["pictures"], [{"canvas": {"width": 1, "height": 1}}])
        self.assertFalse(list(bundle.glob("*.png")))

    def test_empty_img_has_minimal_document_without_png(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "vacant.IMG").write_bytes(b"")
        output = self.root / "output"
        Converter(raw, output).run()
        record, document = documents(output)[0]
        self.assertEqual(record, {"source": "vacant.IMG", "path": "vacant.json"})
        self.assertEqual(document, {"schema_version": 3, "groups": [0], "entries": []})
        self.assertFalse(list((output / "port_data").rglob("*.png")))
        verify(output)

    def test_document_excludes_original_bytes_and_retains_animation_script(self):
        raw = self.root / "raw"
        raw.mkdir()
        original = img_multi([(b"one", tim2(picture(1, 1, 3, b"\x01\x02\x03\x40"))),
                              (b"#cfg", b"TEX_ANIME_END;\x00")]) + b"\xa5" * 17
        (raw / "archive.img").write_bytes(original)
        output = self.root / "output"
        Converter(raw, output).run()
        record, document = documents(output)[0]
        self.assertEqual(record["path"], "archive.json")
        self.assertEqual(set(document), {"schema_version", "groups", "entries"})
        self.assertEqual([entry["name"] for entry in document["entries"]], ["one", "#cfg"])
        self.assertEqual(document["entries"][1]["script"][0]["command"], "TEX_ANIME_END")
        self.assertTrue((output / "port_data/archive.json").read_text().endswith("\n"))
        verify(output)

    def test_verifier_rejects_provenance_and_redundant_fields(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "one.img").write_bytes(img(b"one", tim2(picture(1, 1, 3, b"\x01\x02\x03\x40"))))
        output = self.root / "output"
        Converter(raw, output).run()
        document_path_ = output / "port_data/one.json"
        baseline = json.loads(document_path_.read_text())
        changes = [lambda doc: doc.update({"original": {"bytes": ""}}),
                   lambda doc: doc["entries"][0]["pictures"][0].update({"palette_asset": "one_0.png"}),
                   lambda doc: doc["entries"][0]["pictures"][0].update({"animation": "draw"}),
                   lambda doc: doc["entries"][0]["pictures"][0].update({"sampler": {"wrap_u": "repeat"}})]
        for change in changes:
            with self.subTest(change=changes.index(change)):
                payload = json.loads(json.dumps(baseline))
                change(payload)
                document_path_.write_text(json.dumps(payload, indent=2) + "\n")
                with self.assertRaises(TextureError):
                    verify(output)

    def test_verifier_traverses_textures_among_other_native_json(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "one.img").write_bytes(img(b"one", tim2(picture(1, 1, 3, b"\x01\x02\x03\x40"))))
        output = self.root / "output"
        Converter(raw, output).run()
        assets = output / "port_data"
        (assets / "audio").mkdir()
        (assets / "audio/bank.json").write_text('{"schema_version":1,"kind":"bank","entries":[]}', encoding="utf-8")
        self.assertEqual(verify(output), 1)
        document = assets / "one.json"
        payload = json.loads(document.read_text())
        del payload["groups"]
        document.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(TextureError, "invalid texture document"):
            verify(output)

    def test_filename_collision_is_rejected(self):
        raw = self.root / "raw"
        raw.mkdir()
        body = tim2(picture(1, 1, 3, b"\x01\x02\x03\x40"))
        (raw / "same.img").write_bytes(img(b"one", body))
        (raw / "same.tm2").write_bytes(body)
        with self.assertRaisesRegex(TextureError, "collision"):
            Converter(raw, self.root / "output").run()

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
            relative = converter.write_asset("bundle.json", b"PNG")
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
        self.assertTrue((self.root / "out/port_data/bundle.pac/one.json").is_file())

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
        self.assertTrue((output / "port_data/model.chr/embedded-offset-80.json").is_file())

    def test_direct_documents_and_publish_check(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "ordinary.txt").write_text("ordinary data")
        staged = self.root / "staged"
        Converter(raw, staged).run()
        destination = self.root / "game"
        publish(staged, destination)
        self.assertFalse((destination / "data/manifest.json").exists())
        self.assertFalse((destination / "port_data/catalog.json").exists())
        self.assertFalse((destination / "port_data/manifest.json").exists())
        self.assertEqual(verify(destination), 0)
        (destination / "data/ordinary.txt").write_text("changed")
        self.assertEqual(verify(destination), 0)

    def test_rust_marker_source_metadata_is_accepted(self):
        raw = self.root / "raw"
        raw.mkdir()
        (raw / "ordinary.txt").write_text("data")
        output = self.root / "output"
        Converter(raw, output).run()
        marker_path = output / "data.json"
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
        marker.update({"source_state": {"path": "disc.iso", "files": []},
                       "source_sha256": "0" * 64})
        marker_path.write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(verify(output), 0)

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

    def test_publish_replaces_existing_marker_and_data(self):
        destination = self.root / "game"
        first_raw = self.root / "first-raw"
        first_raw.mkdir()
        (first_raw / "first.txt").write_text("first")
        first_stage = self.root / "first-stage"
        Converter(first_raw, first_stage).run()
        publish(first_stage, destination)
        second_raw = self.root / "second-raw"
        second_raw.mkdir()
        (second_raw / "second.txt").write_text("second")
        second_stage = self.root / "second-stage"
        Converter(second_raw, second_stage).run()
        publish(second_stage, destination)
        self.assertTrue((destination / "data/second.txt").is_file())
        self.assertFalse((destination / "data/first.txt").exists())
        self.assertEqual(verify(destination), 0)
        self.assertFalse(list(destination.glob(".dc2data-*-backup-*")))


if __name__ == "__main__":
    unittest.main()
