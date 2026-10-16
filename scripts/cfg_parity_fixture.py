#!/usr/bin/env python3
"""Prepare original CFG inputs for the native JSON/interpreter parity test."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from urllib.parse import quote
import struct


_audit_spec = importlib.util.spec_from_file_location(
    "chronicle_script_audit",
    Path(__file__).resolve().parents[3] / "scripts/port/audit_script_resources.py",
)
_audit = importlib.util.module_from_spec(_audit_spec)
_audit_spec.loader.exec_module(_audit)


def members(data):
    """Return validated retail PACK members, including zero-length entries."""
    if _audit.is_valid_script(data):
        return None
    result = []
    seen = set()
    cursor = 0
    while cursor not in seen and len(result) < 16384:
        seen.add(cursor)
        if cursor + 76 > len(data):
            return None
        header = data[cursor:cursor + 76]
        end = header.find(b"\0", 0, 64)
        if end < 0:
            return None
        if end == 0:
            return result or None
        name = header[:end]
        if any(byte < 32 or byte in (47, 92) for byte in name):
            return None
        try:
            name = name.decode("utf-8")
        except UnicodeDecodeError:
            return None
        offset, size, advance = struct.unpack_from("<III", header, 64)
        if offset < 76 or advance < 76 or cursor + offset + size > len(data):
            return None
        result.append((name, cursor + offset, size))
        cursor += advance
    return None


def prepare(raw, port_data, destination, texture_oracle=None):
    found = {}
    identities=set()
    ordered=[]
    character_resources={}
    originals = destination / "originals"
    originals.mkdir(parents=True, exist_ok=True)

    def visit(data, alias, depth=0):
        identities.add(alias)
        ordered.append(alias)
        if depth > 16:
            raise ValueError(f"PACK nesting exceeds 16: {alias}")
        if alias.lower().split("#entry:")[0].endswith((".cfg",".map")) and not any(part.lower().endswith(".snd") for part in alias.split("/")[:-1]):
            path = originals / (hashlib.sha256(alias.encode()).hexdigest() + ".cfg")
            path.write_bytes(data)
            if alias in found:
                raise ValueError(f"duplicate CFG alias: {alias}")
            parent = next((alias[:index] for index in range(len(alias)-1,-1,-1) if alias[index]=="/" and alias[:index].lower().rstrip("_").endswith(".chr")),None)
            entry={"alias":alias,"original":str(path.resolve())}
            if parent:
                entry["gltf"]=native_path(parent,"gltf")
                entry["game"]=native_path(parent,"game.json")
                entry["profile"]=Path(alias).stem
                document=json.loads((port_data/entry["game"]).read_bytes())
                if entry["profile"] not in document.get("profiles",{}):raise ValueError(f"missing profile {alias}: {entry['game']} kind={document.get('kind')} keys={list(document)}")
            else:
                entry["json"]=native_path(alias,"json")
                if not (port_data/entry["json"]).is_file():raise ValueError(f"missing native CFG {alias}: {entry['json']}")
            found[alias] = entry
            return
        children = members(data)
        if children is None:
            return
        character=alias.lower().rstrip("_").endswith(".chr")
        if character:
            character_resources[alias]={kind:[] for kind in ["animation","bind_pose","weights","cloth","script","character"]}
        names = set()
        for index, (name, start, size) in enumerate(children):
            child = f"{alias}/{name}"
            if name in names:
                child += f"#entry:{index}"
            names.add(name)
            if character and size:
                kind={".mot":"animation",".bbp":"bind_pose",".wgt":"weights",".clo":"cloth",".stb":"script",".chr":"character"}.get(Path(name).suffix.lower())
                if kind:character_resources[alias][kind].append(child)
            if size:
                visit(data[start:start + size], child, depth + 1)

    for path in sorted(raw.rglob("*")):
        if path.is_file():
            alias = path.relative_to(raw).as_posix()
            identities.add(alias)
            if path.stat().st_size <= 128 << 20:
                visit(path.read_bytes(), alias)
    manifest = {
        "port_data": str(port_data.resolve()),
        "sources": [found[alias] for alias in sorted(found)],
        "identities": sorted(identities),
        "character_resources": character_resources,
    }
    if texture_oracle is not None:
        manifest["texture_documents"] = {}
        for alias in sorted(identities):
            if alias.lower().split("#entry:")[0].endswith((".img", ".tm2")):
                path = texture_oracle / texture_document_path(alias)
                if path.is_file():
                    manifest["texture_documents"][alias.lower()] = str(path.resolve())
    for alias,resources in character_resources.items():
        for child in ordered:
            if child.startswith(alias+"/") and child.endswith(".stb") and child not in resources["script"]:resources["script"].append(child)
    target = destination / "parity.json"
    target.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{len(found)} original CFG files: {target.resolve()}")


def native_path(alias, extension):
    if extension == "json" and alias.rsplit("/", 1)[-1].split("#", 1)[0].lower().endswith(".map"):
        extension = "map.json"
    elif extension == "json" and alias.rsplit("/", 1)[-1].split("#", 1)[0].lower().endswith(".cfg"):
        extension = "cfg.json"
    parts=alias.split("/")
    for index,part in enumerate(parts):
        base,_,marker=part.partition("#")
        suffix="."+marker.replace(":","_") if marker else ""
        if index==len(parts)-1:
            stem=base if marker.startswith("offset_") or marker.startswith("offset:") else base.rsplit(".",1)[0]
            parts[index]=stem+("_" if base.lower().endswith(".chr_") else "")+suffix+"."+extension
        elif base.lower().rstrip("_").endswith(".chr"):
            parts[index]=base.rstrip("_")[:-4]+("_" if base.endswith("_") else "")+suffix+".assets"
        elif marker:
            parts[index]=base+suffix
    return "/".join(parts)


def texture_document_path(alias):
    """Match the extractor's physical texture document path for private oracle lookup."""
    parts = alias.split("/")
    expanded = []
    for part in parts:
        if "#offset:" in part:
            parent, offset = part.rsplit("#offset:", 1)
            expanded.extend((parent, f"embedded-offset-{offset}.json"))
        elif "#entry:" in part:
            name, ordinal = part.split("#entry:", 1)
            expanded.append(name.rsplit(".", 1)[0] + f"-entry-{ordinal}.json")
        else:
            expanded.append(part)
    parts = ["%2E" * len(part) if part in (".", "..") else quote(part, safe="._-").replace("~", "%7E") for part in expanded]
    for index in range(len(parts)-1):
        if parts[index].lower().endswith(".img"):
            parts[index] = parts[index][:-4] + "-img"
    if parts[-1].lower().endswith((".img", ".tm2")):
        parts[-1] = parts[-1][:-4] + ".json"
    elif not parts[-1].endswith(".json"):
        parts.append("embedded.json")
    path = "/".join(parts)
    if len(path) > 180:
        parts = parts[:2] + ["long-path-" + hashlib.sha256(path.encode()).hexdigest()[:20] + ".json"]
    return "/".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("port_data", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--texture-oracle", type=Path,
                        help="Private port_data directory decoded from original IMG/TIM2 resources")
    args = parser.parse_args()
    prepare(args.raw, args.port_data, args.destination, args.texture_oracle)


if __name__ == "__main__":
    main()
