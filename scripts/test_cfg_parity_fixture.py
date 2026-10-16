"""Checks private oracle paths and source identities used by native CFG parity."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("cfg_parity_fixture", Path(__file__).with_name("cfg_parity_fixture.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class TextureOracleFixtureTests(unittest.TestCase):
    def test_large_unparsed_files_remain_original_resource_identities(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            raw, native, output = (root / part for part in ("raw", "native", "fixture"))
            (raw / "movie").mkdir(parents=True)
            native.mkdir()
            with (raw / "movie/end1.pss").open("wb") as movie:
                movie.truncate((128 << 20) + 1)
            fixture.prepare(raw, native, output)
            manifest = json.loads((output / "parity.json").read_text())
            self.assertEqual(manifest["identities"], ["movie/end1.pss"])
            self.assertEqual(manifest["sources"], [])

    def test_general_config_families_have_distinct_direct_paths(self):
        self.assertEqual(fixture.native_path("map/room.cfg", "json"), "map/room.cfg.json")
        self.assertEqual(fixture.native_path("map/room.map", "json"), "map/room.map.json")
        self.assertEqual(fixture.native_path("map/room.img", "json"), "map/room.json")

    def test_texture_document_paths_preserve_markers_and_case(self):
        self.assertEqual(fixture.texture_document_path("a.chr/sheet.IMG"), "a.chr/sheet.json")
        self.assertEqual(fixture.texture_document_path("a.pac#offset:80"), "a.pac/embedded-offset-80.json")
        self.assertEqual(fixture.texture_document_path("a.chr/sheet.img#entry:2"), "a.chr/sheet-entry-2.json")
        self.assertEqual(fixture.texture_document_path("a.img/sheet.tm2"), "a-img/sheet.json")

    def test_texture_document_paths_encode_components(self):
        self.assertEqual(fixture.texture_document_path("a.chr/a~ b.img"), "a.chr/a%7E%20b.json")
        self.assertEqual(fixture.texture_document_path("../sheet.img"), "%2E%2E/sheet.json")
        self.assertEqual(fixture.texture_document_path("a/" + "x" * 190 + ".img").split("/")[-1][:10], "long-path-")

    def test_manifest_texture_binding_is_original_member_identity(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            raw, native, oracle, output = (root / part for part in ("raw", "native", "oracle", "fixture"))
            raw.mkdir(); native.mkdir(); (oracle / "hero.chr").mkdir(parents=True)
            payloads = [("info.cfg", b'IMG 0,"sheet.IMG";'), ("sheet.IMG", b"original image bytes")]
            headers, data = [], bytearray()
            for index, (member, payload) in enumerate(payloads):
                headers.append(struct.pack("<64sIII4x", member.encode(), 240 + len(data) - index * 80, len(payload), 80))
                data.extend(payload)
            (raw / "hero.chr").write_bytes(b"".join(headers) + bytes(80) + data)
            (native / "hero.game.json").write_text(json.dumps({"profiles": {"info": {"commands": []}}}))
            expected = oracle / "hero.chr/sheet.json"
            expected.write_text(json.dumps({"schema_version": 3, "groups": [], "entries": []}))
            fixture.prepare(raw, native, output, oracle)
            manifest = json.loads((output / "parity.json").read_text())
            self.assertEqual(manifest["texture_documents"], {"hero.chr/sheet.img": str(expected.resolve())})
            self.assertEqual(manifest["sources"][0]["profile"], "info")


if __name__ == "__main__":
    unittest.main()
