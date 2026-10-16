import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from migrate_texture_datapacks import (MigrationError, build_mapping, load_mapping,
                                       migrate, plan, save_mapping)


class DatapackMigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=Path.cwd())
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.old = self.root / "old"
        self.new = self.root / "new"
        self.packs = self.root / "datapacks"
        self.old.mkdir()
        self.new.mkdir()
        self.packs.mkdir()
        (self.new / "textures").mkdir()
        entry = {"type": "texture", "name": "face", "picture": 0, "block": 2,
                 "width": 1, "height": 1, "source_bpp": 8, "tex0": 11, "tex1": 12,
                 "variants": [{"palette_source": "face", "levels": ["hero_0.png"]}]}
        document = {"schema_version": 3, "groups": [2],
                    "entries": [{"name": "face", "block": 2,
                                 "pictures": [{"levels": ["hero_0.png"]}]}]}
        (self.new / "textures/hero.json").write_text(json.dumps(document))
        (self.new / "catalog.json").write_text(json.dumps({"schema_version": 3,
            "sources": {"textures/hero.img": "textures/hero.json"}}))
        old_entry = json.loads(json.dumps(entry))
        old_entry["variants"][0]["levels"] = ["textures/hero.img/face-entry-0-picture-0.png"]
        (self.old / "catalog.json").write_text(json.dumps({"schema_version": 1,
            "sources": [{"source": "textures/hero.img", "entries": [old_entry]}]}))

    def test_sparse_pack_migrates_exact_bytes_and_keeps_unrelated_file(self):
        pack = self.packs / "My textures"
        source = pack / "textures/hero.img/face-entry-0-picture-0.png"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"replacement PNG bytes")
        (pack / "notes.txt").write_text("keep")
        mapping = build_mapping(self.old / "catalog.json", self.new / "catalog.json")
        self.assertEqual(mapping, {"textures/hero.img/face-entry-0-picture-0.png":
                                   "textures/hero_0.png"})
        moves = plan(self.packs, mapping)
        self.assertEqual(len(moves), 1)
        self.assertTrue(source.exists())  # planning is read-only
        migrate(moves, self.packs)
        self.assertEqual((pack / "textures/hero_0.png").read_bytes(), b"replacement PNG bytes")
        self.assertEqual((pack / "notes.txt").read_text(), "keep")
        self.assertFalse((pack / "textures/hero.img").exists())
        self.assertEqual(plan(self.packs, mapping), [])

    def test_direct_document_root_needs_no_new_catalog(self):
        (self.new / "catalog.json").unlink()
        mapping = build_mapping(self.old / "catalog.json", self.new)
        self.assertEqual(mapping, {"textures/hero.img/face-entry-0-picture-0.png":
                                   "textures/hero_0.png"})
        (self.new / "textures/hero.json").unlink()
        with self.assertRaisesRegex(MigrationError, "missing native texture document"):
            build_mapping(self.old / "catalog.json", self.new)

    def embedded_source(self, alias, old_png):
        old = json.loads((self.old / "catalog.json").read_text())
        old["sources"][0]["source"] = alias
        old["sources"][0]["entries"][0]["variants"][0]["levels"] = [old_png]
        (self.old / "catalog.json").write_text(json.dumps(old))
        (self.new / "catalog.json").unlink()
        (self.new / "textures/hero.json").unlink()

    def test_embedded_chr_sidecar_maps_png_without_base_file(self):
        alias = "chara/hero.chr#offset:80"
        old_png = "chara/hero.chr/face-entry-0-picture-0.png"
        target = "chara/hero.chr/embedded-offset-80_0.png"
        self.embedded_source(alias, old_png)
        sidecar = {"schema_version": 1, "kind": "character", "images": [{"image": 7, "png": target}],
                   "resources": {"texture_sets": [{"name": "hero", "document": {
                       "schema_version": 3, "groups": [2], "entries": [{"name": "face", "block": 2,
                       "pictures": [{"levels": ["#/images/7"]}]}]}}]}}
        (self.new / "chara").mkdir()
        (self.new / "chara/hero.game.json").write_text(json.dumps(sidecar))
        mapping = build_mapping(self.old / "catalog.json", self.new)
        self.assertEqual(mapping, {old_png: target})
        self.assertFalse((self.new / target).exists())
        pack = self.packs / "Hero"
        source = pack / old_png
        source.parent.mkdir(parents=True)
        source.write_bytes(b"unchanged replacement")
        migrate(plan(self.packs, mapping), self.packs)
        self.assertEqual((pack / target).read_bytes(), b"unchanged replacement")

    def test_zero_entry_pack_named_document_retains_numbered_png_path(self):
        alias = "ui/menu.pac#offset:24"
        old_png = "ui/menu.pac/face-entry-0-picture-0.png"
        target = "ui/menu.pac/embedded-offset-24_0.png"
        self.embedded_source(alias, old_png)
        (self.new / "ui/menu.pac").mkdir(parents=True)
        document = {"schema_version": 3, "groups": [2], "entries": [{"name": "face", "block": 2,
                    "pictures": [{"levels": ["embedded-offset-24_0.png"]}]}]}
        (self.new / "ui/menu.pac/frametex.json").write_text(json.dumps(document))
        self.assertEqual(build_mapping(self.old / "catalog.json", self.new), {old_png: target})

    def test_ambiguous_renamed_documents_are_rejected(self):
        alias = "ui/menu.pac#offset:24"
        old_png = "ui/menu.pac/face-entry-0-picture-0.png"
        self.embedded_source(alias, old_png)
        (self.new / "ui/menu.pac").mkdir(parents=True)
        document = {"schema_version": 3, "groups": [2], "entries": [{"name": "face", "block": 2,
                    "pictures": [{"levels": ["embedded-offset-24_0.png"]}]}]}
        (self.new / "ui/menu.pac/frametex.json").write_text(json.dumps(document))
        other = json.loads(json.dumps(document))
        other["entries"][0]["pictures"][0]["sampler"] = {"wrap_u": "clamp"}
        (self.new / "ui/menu.pac/map.json").write_text(json.dumps(other))
        with self.assertRaisesRegex(MigrationError, "ambiguous"):
            build_mapping(self.old / "catalog.json", self.new)

    def test_identical_embedded_candidates_are_deduplicated(self):
        alias = "chara/hero.chr#offset:80"
        old_png = "chara/hero.chr/face-entry-0-picture-0.png"
        target = "chara/hero.chr/embedded-offset-80_0.png"
        self.embedded_source(alias, old_png)
        sidecar = {"schema_version": 1, "kind": "character", "images": [{"image": 0, "png": target}],
                   "resources": {"texture_sets": [{"document": {"schema_version": 3, "groups": [2],
                   "entries": [{"name": "face", "block": 2, "pictures": [{"levels": ["#/images/0"]}]}]}}]}}
        (self.new / "chara").mkdir()
        (self.new / "chara/hero.game.json").write_text(json.dumps(sidecar))
        sibling = json.loads(json.dumps(sidecar))
        sibling["images"][0]["image"] = 4
        sibling["resources"]["texture_sets"][0]["document"]["entries"][0]["pictures"][0]["levels"] = ["#/images/4"]
        (self.new / "chara/child.game.json").write_text(json.dumps(sibling))
        self.assertEqual(build_mapping(self.old / "catalog.json", self.new), {old_png: target})

    def test_missing_native_document_is_irrelevant_without_legacy_pngs(self):
        self.embedded_source("chara/hero.chr#offset:80", "chara/hero.chr/old.png")
        old_path = self.old / "catalog.json"
        old = json.loads(old_path.read_text())
        old["sources"][0]["entries"][0]["variants"][0]["levels"] = []
        old_path.write_text(json.dumps(old))
        self.assertEqual(build_mapping(old_path, self.new), {})

    def test_embedded_image_uri_and_escape_are_rejected(self):
        alias = "chara/hero.chr#offset:80"
        old_png = "chara/hero.chr/face-entry-0-picture-0.png"
        target = "chara/hero.chr/embedded-offset-80_0.png"
        self.embedded_source(alias, old_png)
        (self.new / "chara").mkdir()
        sidecar_path = self.new / "chara/hero.game.json"
        sidecar = {"schema_version": 1, "kind": "character", "images": [{"image": 0, "png": target}],
                   "resources": {"texture_sets": [{"document": {"schema_version": 3, "groups": [2],
                   "entries": [{"name": "face", "block": 2, "pictures": [{"levels": ["#/images/9"]}]}]}}]}}
        sidecar_path.write_text(json.dumps(sidecar))
        with self.assertRaisesRegex(MigrationError, "invalid image URI"):
            build_mapping(self.old / "catalog.json", self.new)
        sidecar["resources"]["texture_sets"][0]["document"]["entries"][0]["pictures"][0]["levels"] = ["#/images/0"]
        sidecar["images"][0]["png"] = "../outside.png"
        sidecar_path.write_text(json.dumps(sidecar))
        with self.assertRaisesRegex(MigrationError, "unsafe asset path"):
            build_mapping(self.old / "catalog.json", self.new)

    def test_existing_different_destination_rejects_without_mutation(self):
        pack = self.packs / "Conflict"
        source = pack / "textures/hero.img/face-entry-0-picture-0.png"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"old")
        target = pack / "textures/hero_0.png"
        target.write_bytes(b"different")
        mapping = build_mapping(self.old / "catalog.json", self.new / "catalog.json")
        with self.assertRaisesRegex(MigrationError, "different bytes"):
            plan(self.packs, mapping)
        self.assertEqual((source.read_bytes(), target.read_bytes()), (b"old", b"different"))

    def test_saved_map_and_identical_destination_resume(self):
        mapping = build_mapping(self.old / "catalog.json", self.new / "catalog.json")
        saved = self.root / "mapping.json"
        save_mapping(saved, mapping)
        self.assertEqual(load_mapping(saved), mapping)
        pack = self.packs / "Resume"
        source = pack / "textures/hero.img/face-entry-0-picture-0.png"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"same")
        target = pack / "textures/hero_0.png"
        target.write_bytes(b"same")
        migrate(plan(self.packs, load_mapping(saved)), self.packs)
        self.assertFalse(source.exists())
        self.assertEqual(target.read_bytes(), b"same")

    def test_nested_pictures_and_variant_selectors_map_by_identity(self):
        old_path = self.old / "catalog.json"
        old = json.loads(old_path.read_text())
        base = json.loads(json.dumps(old["sources"][0]["entries"][0]))
        base["picture"] = 1
        base["variants"] = [
            {"palette_source": "face", "levels": ["textures/hero.img/face-picture-1.png"]},
            {"palette_source": "donor", "levels": ["textures/hero.img/face-donor-picture-1.png"]},
        ]
        old["sources"][0]["entries"].append(base)
        old_path.write_text(json.dumps(old))
        document_path = self.new / "textures/hero.json"
        new = json.loads(document_path.read_text())
        new["entries"][0]["pictures"].append({"levels": ["hero_1.png"],
            "variants": [{"selector": "donor", "levels": ["hero_2.png"]}]})
        document_path.write_text(json.dumps(new))
        mapping = build_mapping(old_path, self.new / "catalog.json")
        self.assertEqual(mapping["textures/hero.img/face-picture-1.png"],
                         "textures/hero_1.png")
        self.assertEqual(mapping["textures/hero.img/face-donor-picture-1.png"],
                         "textures/hero_2.png")

    def test_symlink_escape_and_unmapped_legacy_png_rejected(self):
        pack = self.packs / "Unsafe"
        pack.mkdir()
        mapping = build_mapping(self.old / "catalog.json", self.new / "catalog.json")
        legacy = pack / "other.img/extra.png"
        legacy.parent.mkdir()
        legacy.write_bytes(b"extra")
        with self.assertRaisesRegex(MigrationError, "unmapped legacy PNG"):
            plan(self.packs, mapping)
        legacy.unlink()
        legacy.parent.rmdir()
        try:
            (pack / "textures").symlink_to(self.root, target_is_directory=True)
        except (NotImplementedError, OSError):
            self.skipTest("symlink creation unavailable")
        with self.assertRaisesRegex(MigrationError, "symlink"):
            plan(self.packs, mapping)
