"""Move v1 texture datapack PNGs to the direct v3 document asset paths.

The default action is a read-only plan. --migrate applies a fully checked plan;
--save-map preserves the mapping across extraction publication.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys

from kazarov_textures import document_path, legacy_document_path


class MigrationError(ValueError):
    pass


def digest(path: Path) -> str:
    hash_ = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(4 << 20), b""):
            hash_.update(block)
    return hash_.hexdigest()


def checked_path(root: Path, relative: str) -> Path:
    if not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative:
        raise MigrationError(f"unsafe asset path: {relative!r}")
    parts = PurePosixPath(relative).parts
    if not parts or any(part in ("", ".", "..") for part in parts) or relative.startswith("/"):
        raise MigrationError(f"unsafe asset path: {relative!r}")
    path = root.joinpath(*parts)
    if not path.resolve().is_relative_to(root.resolve()):
        raise MigrationError(f"asset escapes datapack: {relative!r}")
    cursor = root
    for part in parts:
        cursor /= part
        if cursor.is_symlink() or getattr(cursor, "is_junction", lambda: False)():
            raise MigrationError(f"symlink or junction in asset path: {cursor}")
    return path


def entry_key(entry: dict) -> tuple:
    return entry["name"], entry["picture"], entry.get("block", 0)


def grouped_entries(entries: list[dict]) -> dict[tuple, list[dict]]:
    groups: dict[tuple, list[dict]] = defaultdict(list)
    for entry in entries:
        if entry.get("type") == "texture":
            groups[entry_key(entry)].append(entry)
    return groups


def has_legacy_pngs(source: dict) -> bool:
    return any(variant.get("levels") for entry in source["entries"]
               if entry.get("type") == "texture" for variant in entry["variants"])


def texture_documents(root: Path) -> list[tuple[str, dict, dict[int, str]]]:
    """Find direct texture documents and texture sets embedded in game sidecars."""
    result = []
    for path in sorted(root.rglob("*.json")):
        relative = path.relative_to(root).as_posix()
        checked_path(root, relative)
        if relative == "catalog.json" or relative == "manifest.json":
            continue
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict):
            continue
        if set(value) == {"schema_version", "groups", "entries"} and value["schema_version"] == 3:
            result.append((relative, value, {}))
        if not relative.endswith(".game.json"):
            continue
        sets = value.get("resources", value).get("texture_sets", [])
        if not isinstance(sets, list):
            raise MigrationError(f"invalid texture sets: {relative}")
        images = {}
        for row in value.get("images", []):
            if (not isinstance(row, dict) or type(row.get("image")) is not int
                    or row["image"] < 0 or row["image"] in images):
                raise MigrationError(f"invalid image row: {relative}")
            checked_path(root, row.get("png"))
            if not row["png"].lower().endswith(".png"):
                raise MigrationError(f"invalid image URI: {relative}")
            images[row["image"]] = row["png"]
        for index, item in enumerate(sets):
            if not isinstance(item, dict) or not isinstance(item.get("document"), dict):
                raise MigrationError(f"invalid texture set: {relative}#{index}")
            doc = item["document"]
            if set(doc) != {"schema_version", "groups", "entries"} or doc["schema_version"] != 3:
                raise MigrationError(f"invalid embedded texture document: {relative}#{index}")
            result.append((relative, doc, images))
    return result


def asset_reference(root: Path, owner: str, reference: str, images: dict[int, str]) -> str:
    if not isinstance(reference, str):
        raise MigrationError(f"invalid image URI: {reference!r}")
    if reference.startswith("#"):
        match = re.fullmatch(r"#/images/(0|[1-9][0-9]*)", reference)
        if not match or (index := int(match.group(1))) not in images:
            raise MigrationError(f"invalid image URI: {reference!r}")
        target = images[index]
    else:
        if images:
            raise MigrationError(f"invalid embedded image URI: {reference!r}")
        target = (PurePosixPath(owner).parent / reference).as_posix()
    checked_path(root, target)
    if not target.lower().endswith(".png"):
        raise MigrationError(f"invalid image URI: {reference!r}")
    return target


def normalized_document(root: Path, owner: str, document: dict, images: dict[int, str]) -> str:
    value = json.loads(json.dumps(document))
    for entry in value["entries"]:
        for picture in entry.get("pictures", []):
            if "levels" in picture:
                picture["levels"] = [asset_reference(root, owner, level, images)
                                     for level in picture["levels"]]
            for variant in picture.get("variants", []):
                variant["levels"] = [asset_reference(root, owner, level, images)
                                     for level in variant["levels"]]
    return json.dumps(value, sort_keys=True)


def document_assets(root: Path, owner: str, document: dict, images: dict[int, str]):
    for entry in document["entries"]:
        for picture in entry.get("pictures", []):
            for reference in picture.get("levels", []):
                yield asset_reference(root, owner, reference, images)
            for variant in picture.get("variants", []):
                for reference in variant["levels"]:
                    yield asset_reference(root, owner, reference, images)


def source_mapping(old_source: dict, owner: str, document: dict,
                   images: dict[int, str], old_root: Path, new_root: Path) -> dict[str, str]:
    alias = old_source["source"]
    old_groups = grouped_entries(old_source["entries"])
    new_groups = defaultdict(list)
    for entry in document["entries"]:
        if "pictures" not in entry:
            continue
        for picture_index, picture in enumerate(entry["pictures"]):
            new_groups[(entry["name"], picture_index, entry.get("block", 0))].append(picture)
    if old_groups.keys() != new_groups.keys():
        raise MigrationError(f"texture entry identity changed: {alias}")
    mapping = {}
    for key, old_entries in old_groups.items():
        new_entries = new_groups[key]
        if len(old_entries) != len(new_entries):
            raise MigrationError(f"duplicate texture entry count changed: {alias} {key}")
        for old_entry, new_picture in zip(old_entries, new_entries):
            old_variants = defaultdict(list)
            new_variants = defaultdict(list)
            for variant in old_entry["variants"]:
                old_variants[variant["palette_source"]].append(variant)
            new_variants[key[0]].append({"levels": new_picture.get("levels", [])})
            for variant in new_picture.get("variants", []):
                new_variants[variant["selector"]].append(variant)
            if old_variants.keys() != new_variants.keys():
                raise MigrationError(f"palette selectors changed: {alias} {key}")
            for selector, old_items in old_variants.items():
                new_items = new_variants[selector]
                if len(old_items) != len(new_items):
                    raise MigrationError(f"palette variant count changed: {alias} {key} {selector}")
                for old_variant, new_variant in zip(old_items, new_items):
                    old_levels, new_levels = old_variant["levels"], new_variant["levels"]
                    if len(old_levels) != len(new_levels):
                        raise MigrationError(f"mip count changed: {alias} {key} {selector}")
                    for old_level, new_level in zip(old_levels, new_levels):
                        old_relative = PurePosixPath(old_level).as_posix()
                        checked_path(old_root, old_relative)
                        new_relative = asset_reference(new_root, owner, new_level, images)
                        if old_relative in mapping and mapping[old_relative] != new_relative:
                            raise MigrationError(f"ambiguous v1 asset: {old_relative}")
                        mapping[old_relative] = new_relative
    return mapping


def build_mapping(old_catalog: Path, new_location: Path) -> dict[str, str]:
    old = json.loads(old_catalog.read_text(encoding="utf-8"))
    if old.get("schema_version") != 1:
        raise MigrationError("expected a v1 source catalog")
    new_root = new_location if new_location.is_dir() else new_location.parent
    new = None
    if new_location.is_file():
        new = json.loads(new_location.read_text(encoding="utf-8"))
        if new.get("schema_version") != 3 or not isinstance(new.get("sources"), dict):
            raise MigrationError("expected a v3 document catalog")
    candidates = texture_documents(new_root) if new is None else []
    by_path = defaultdict(list)
    by_png_prefix = defaultdict(set)
    for index, (relative, document, images) in enumerate(candidates):
        by_path[relative].append(index)
        for asset in document_assets(new_root, relative, document, images):
            match = re.fullmatch(r"(.*)_[0-9]+\.png", asset, re.IGNORECASE)
            if match:
                by_png_prefix[match.group(1)].add(index)
    mapping: dict[str, str] = {}
    targets = {}
    for source in old["sources"]:
        if not has_legacy_pngs(source):
            continue
        alias = source["source"]
        expected = document_path(alias)
        if new is not None:
            selected = new["sources"].get(alias)
            if selected is None:
                raise MigrationError(f"v1 source absent from v3 catalog: {alias}")
            path = checked_path(new_root, selected)
            if not path.is_file():
                raise MigrationError(f"v3 texture document missing: {selected}")
            document = json.loads(path.read_text(encoding="utf-8"))
            options = [(selected, document, {})]
        else:
            prefix = PurePosixPath(expected)
            collision = prefix.stem + "-" + hashlib.sha256(alias.encode()).hexdigest()[:12] + "-"
            options = [item for item in candidates
                       if not item[2] and PurePosixPath(item[0]).parent == prefix.parent
                       and PurePosixPath(item[0]).stem.startswith(collision)]
            if not options:
                options = [candidates[index] for path in (expected, legacy_document_path(alias))
                           for index in by_path[path] if not candidates[index][2]]
            if not options:
                # Model/CHR embedding or zero-entry PACK compaction may remove or
                # rename the expected document while preserving its PNG identities.
                prefix = PurePosixPath(expected)
                options = []
                prefixes = {prefix.with_suffix("").as_posix(),
                            PurePosixPath(legacy_document_path(alias)).with_suffix("").as_posix()}
                raw = alias.split("/")
                owner = next((part.split("#", 1)[0] for part in reversed(raw)
                              if part.split("#", 1)[0].lower().endswith((".chr", ".chr_"))), None)
                if owner:
                    stem = owner.rsplit(".", 1)[0] + ("_" if owner.lower().endswith(".chr_") else "")
                    prefixes.add((prefix.parent / stem).as_posix())
                indices = set().union(*(by_png_prefix[value] for value in prefixes))
                # Collision suffixes preserve the source hash in the document stem.
                indices.update(index for index, (name, _, _) in enumerate(candidates)
                               if PurePosixPath(name).parent == prefix.parent
                               and PurePosixPath(name).stem.startswith(collision))
                for index in sorted(indices):
                    item = candidates[index]
                    try:
                        proposed = source_mapping(source, *item, old_catalog.parent, new_root)
                    except MigrationError as error:
                        if "identity changed" in str(error) or "count changed" in str(error) or "selectors changed" in str(error):
                            continue
                        raise
                    if proposed:
                        options.append(item)
        if len(options) > 1:
            signatures = {(normalized_document(new_root, *item),
                           tuple(sorted(source_mapping(source, *item, old_catalog.parent, new_root).items())))
                          for item in options}
            if len(signatures) == 1:
                options = options[:1]
        if len(options) != 1:
            raise MigrationError(f"ambiguous or missing native texture document: {alias} ({len(options)} candidates)")
        relative, document, images = options[0]
        if document.get("schema_version") != 3 or set(document) != {"schema_version", "groups", "entries"}:
            raise MigrationError(f"invalid v3 document: {relative}")
        proposed = source_mapping(source, relative, document, images, old_catalog.parent, new_root)
        for old_relative, new_relative in proposed.items():
            if old_relative in mapping and mapping[old_relative] != new_relative:
                raise MigrationError(f"ambiguous v1 asset: {old_relative}")
            if new_relative in targets and targets[new_relative] != old_relative:
                raise MigrationError(f"native asset has multiple v1 donors: {new_relative}")
            mapping[old_relative] = new_relative
            targets[new_relative] = old_relative
    return mapping


def save_mapping(path: Path, mapping: dict[str, str]) -> None:
    path.write_text(json.dumps({"schema_version": 1, "paths": mapping}, indent=2) + "\n",
                    encoding="utf-8")


def load_mapping(path: Path) -> dict[str, str]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema_version") != 1 or not isinstance(payload.get("paths"), dict):
        raise MigrationError("invalid saved migration map")
    return payload["paths"]


def pack_directories(datapacks: Path) -> list[Path]:
    if (not datapacks.is_dir() or datapacks.is_symlink()
            or getattr(datapacks, "is_junction", lambda: False)()):
        raise MigrationError("datapacks must be a real directory")
    packs = []
    for path in sorted(datapacks.iterdir()):
        if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
            raise MigrationError(f"symlink or junction in datapacks: {path}")
        if path.is_dir():
            packs.append(path)
    return packs


def plan(datapacks: Path, mapping: dict[str, str]) -> list[tuple[Path, Path]]:
    moves = []
    for pack in pack_directories(datapacks):
        for directory, names, files in os.walk(pack, followlinks=False):
            here = Path(directory)
            for name in names + files:
                item = here / name
                if item.is_symlink() or getattr(item, "is_junction", lambda: False)():
                    raise MigrationError(f"symlink or junction in datapack: {item}")
            for name in files:
                source = here / name
                relative = source.relative_to(pack).as_posix()
                if relative not in mapping:
                    if source.suffix.lower() == ".png" and any(
                            part.lower().endswith(".img") for part in source.relative_to(pack).parts[:-1]):
                        raise MigrationError(f"unmapped legacy PNG: {source}")
                    continue
                target = checked_path(pack, mapping[relative])
                if source == target:
                    continue
                if target.exists() and digest(source) != digest(target):
                    raise MigrationError(f"different bytes already at destination: {target}")
                moves.append((source, target))
    targets = set()
    for source, target in moves:
        if target in targets:
            raise MigrationError(f"two datapack files target one path: {target}")
        targets.add(target)
    return moves


def migrate(moves: list[tuple[Path, Path]], datapacks: Path) -> None:
    packs = set(pack_directories(datapacks))
    for source, target in moves:
        relative = source.relative_to(datapacks)
        pack = datapacks / relative.parts[0]
        if pack not in packs:
            raise MigrationError(f"source no longer belongs to a datapack: {source}")
        checked_path(pack, source.relative_to(pack).as_posix())
        checked_path(pack, target.relative_to(pack).as_posix())
        if target.exists() and digest(source) != digest(target):
            raise MigrationError(f"different bytes already at destination: {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            source.unlink()
        else:
            source.replace(target)
    for pack in pack_directories(datapacks):
        directories = sorted((path for path in pack.rglob("*") if path.is_dir()),
                             key=lambda path: len(path.parts), reverse=True)
        for directory in directories:
            if directory.suffix.lower() == ".img":
                try:
                    directory.rmdir()
                except OSError:
                    pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-catalog", type=Path)
    parser.add_argument("--new-root", type=Path, help="port_data directory with direct v3 texture documents")
    parser.add_argument("--new-catalog", type=Path, help="legacy v3 catalog, if one was saved")
    parser.add_argument("--mapping-file", type=Path, help="use a map saved before publishing v3")
    parser.add_argument("--save-map", type=Path, help="save the catalog mapping for a later migration")
    parser.add_argument("--datapacks", required=True, type=Path)
    parser.add_argument("--migrate", action="store_true", help="apply the checked plan")
    args = parser.parse_args(argv)
    try:
        if args.mapping_file:
            mapping = load_mapping(args.mapping_file)
        elif args.old_catalog and (args.new_root or args.new_catalog):
            if args.new_root and args.new_catalog:
                parser.error("choose --new-root or --new-catalog")
            mapping = build_mapping(args.old_catalog, args.new_root or args.new_catalog)
        else:
            parser.error("provide --mapping-file or --old-catalog with --new-root")
        if args.save_map:
            save_mapping(args.save_map, mapping)
        moves = plan(args.datapacks, mapping)
        print(f"{len(mapping)} mapped assets; {len(moves)} datapack files to move")
        for source, target in moves[:20]:
            print(f"{source} -> {target}")
        if len(moves) > 20:
            print(f"... {len(moves) - 20} more moves")
        if args.migrate:
            migrate(moves, args.datapacks)
            print("migration complete")
        return 0
    except (MigrationError, OSError, KeyError, TypeError, ValueError) as error:
        print(f"migration failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
