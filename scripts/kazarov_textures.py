"""Offline conversion of retail TIM2 and IMG textures into PNG and JSON.

The input tree is a private dc2data staging tree. No source texture bytes are
copied into the published data tree. PACK containers retain their entry table
and non-texture members, but texture member bodies are zeroed in place so
offsets used by the game remain valid.
"""

from __future__ import annotations

import hashlib
import json
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
import posixpath
import re
import shutil
import struct
import time
import uuid
import zlib
from dataclasses import dataclass
from pathlib import Path


class TextureError(ValueError):
    pass


def u16(b: bytes, p: int) -> int:
    return struct.unpack_from("<H", b, p)[0]


def u32(b: bytes, p: int) -> int:
    return struct.unpack_from("<I", b, p)[0]


def u64(b: bytes, p: int) -> int:
    return struct.unpack_from("<Q", b, p)[0]


def safe_slice(b: bytes, start: int, size: int) -> bytes:
    if start < 0 or size < 0 or start > len(b) or size > len(b) - start:
        raise TextureError(f"range {start}+{size} exceeds {len(b)} bytes")
    return b[start:start + size]


def name_bytes(b: bytes) -> str:
    return source_key(b.split(b"\0", 1)[0].decode("latin1"))


def png_chunk(tag: bytes, payload: bytes) -> bytes:
    data = tag + payload
    return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data))


def png(width: int, height: int, pixels: bytes) -> bytes:
    if len(pixels) != width * height * 4:
        raise TextureError("invalid PNG raster")
    rows = b"".join(b"\0" + pixels[y * width * 4:(y + 1) * width * 4]
                    for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    result = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header)
    return result + png_chunk(b"IDAT", zlib.compress(rows, 9)) + png_chunk(b"IEND", b"")


def colorize(indices: bytes, palette: bytes) -> bytes:
    return b"".join(palette[index * 4:index * 4 + 4] for index in indices)


def alpha(a: int) -> int:
    return min(a * 2, 255)


def csm1(i: int) -> int:
    return (i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1)


def swizzled_index(x: int, y: int, width: int) -> int:
    block = (y & ~15) * width + (x & ~15) * 2
    swap = (((y + 2) >> 2) & 1) * 4
    pos_y = (((y & ~3) >> 1) + (y & 1)) & 7
    column = pos_y * width * 2 + ((x + swap) & 7) * 4
    return block + column + (((y >> 1) & 1) + ((x >> 2) & 2))


def decode_level(raw: bytes, width: int, height: int, image_type: int, swizzled: bool) -> tuple[bytes, int]:
    count = width * height
    if image_type == 1:
        out = bytearray(count * 4)
        for i in range(count):
            v = u16(raw, i * 2)
            r, g, b = (v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3
            out[i * 4:i * 4 + 4] = bytes((r, g, b, 255 if v & 0x8000 else (0 if not (r | g | b) else 255)))
        return bytes(out), 4
    if image_type == 2:
        return b"".join(raw[i * 3:i * 3 + 3] + b"\xff" for i in range(count)), 4
    if image_type == 3:
        out = bytearray(raw[:count * 4])
        for i in range(3, len(out), 4):
            out[i] = alpha(out[i])
        return bytes(out), 4
    if image_type == 4:
        return bytes((raw[i // 2] >> (4 if i & 1 else 0)) & 15 for i in range(count)), 1
    if image_type == 5:
        if swizzled and width % 16 == 0 and height % 16 == 0:
            return bytes(raw[swizzled_index(x, y, width)] for y in range(height) for x in range(width)), 1
        return raw[:count], 1
    raise TextureError(f"unsupported TIM2 image type {image_type}")


def level_bytes(width: int, height: int, image_type: int) -> int:
    count = width * height
    return {1: count * 2, 2: count * 3, 3: count * 4, 4: (count + 1) // 2, 5: count}[image_type]


def decode_palette(raw: bytes, image_type: int, clut_type: int) -> bytes:
    count = 16 if image_type == 4 else 256
    clut16 = (clut_type & 0x3f) == 1
    size = 2 if clut16 else 4
    safe_slice(raw, 0, count * size)
    result = bytearray(256 * 4)
    for i in range(count):
        if clut16:
            v = u16(raw, i * 2)
            r, g, b = (v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3
            rgba = (r, g, b, 255 if v & 0x8000 else (0 if not (r | g | b) else 255))
        else:
            p = raw[i * 4:i * 4 + 4]
            rgba = (p[0], p[1], p[2], alpha(p[3]))
        slot = csm1(i) if image_type == 5 and (clut_type & 0xc0) == 0 else i
        result[slot * 4:slot * 4 + 4] = bytes(rgba)
    return bytes(result)


@dataclass
class Picture:
    width: int
    height: int
    bpp: int
    tex0: int
    tex1: int
    levels: list[tuple[int, int, bytes, int]]
    palette: bytes | None
    size: int


def parse_picture(b: bytes, offset: int, swizzled: bool = False, no_image: bool = False) -> Picture:
    h = safe_slice(b, offset, 0x30)
    total, clut_size, image_size = struct.unpack_from("<III", h)
    header_size, colors = struct.unpack_from("<HH", h, 12)
    mip_count, clut_type, image_type = h[17], h[18], h[19]
    width, height = struct.unpack_from("<HH", h, 20)
    if not width or not height or image_type not in (1, 2, 3, 4, 5) or mip_count > 16:
        raise TextureError("invalid TIM2 picture dimensions or format")
    minimum_size = header_size if no_image else header_size + image_size + clut_size
    minimum_header = 0x40 + 4 * min(mip_count, 4) if mip_count > 1 else 0x30
    if header_size < minimum_header or minimum_size > len(b) - offset:
        raise TextureError("invalid TIM2 picture sizes")
    # Some retail files record an uncompressed VRAM reservation in total_size.
    # The image and CLUT sizes still delimit their actual serialized payload.
    if total < minimum_size or total > len(b) - offset:
        total = minimum_size
    levels = []
    position = offset + header_size
    for i in range(0 if no_image else min(max(mip_count, 1), 4)):
        w, hgt = max(1, width >> i), max(1, height >> i)
        needed = level_bytes(w, hgt, image_type)
        if position + needed > offset + header_size + image_size:
            raise TextureError("TIM2 mip level extends beyond image")
        raster, channels = decode_level(safe_slice(b, position, needed), w, hgt, image_type, swizzled)
        levels.append((w, hgt, raster, channels))
        position += u32(b, offset + 0x40 + i * 4) if mip_count > 1 else needed
    palette = None
    if image_type in (4, 5) and clut_size and not no_image:
        palette = decode_palette(safe_slice(b, offset + header_size + image_size, clut_size), image_type, clut_type)
    elif image_type in (4, 5) and not no_image:
        raise TextureError("indexed TIM2 picture has no CLUT")
    return Picture(width, height, {1: 16, 2: 24, 3: 32, 4: 4, 5: 8}[image_type],
                   u64(h, 24), u64(h, 32), levels, palette, total)


def parse_tim2(b: bytes, swizzled: bool = False, no_image: bool = False) -> list[Picture]:
    h = safe_slice(b, 0, 16)
    if h[:4] != b"TIM2" or h[5] not in (0, 1):
        raise TextureError("invalid TIM2 header")
    count = u16(h, 6)
    if count == 0 or count > 256:
        raise TextureError("invalid TIM2 picture count")
    position = 0x80 if h[5] else 16
    out = []
    for _ in range(count):
        picture = parse_picture(b, position, swizzled, no_image)
        out.append(picture)
        position += picture.size
    return out


def tim2_extent(b: bytes, no_image: bool = False) -> int:
    pictures = parse_tim2(b, no_image=no_image)
    first = 0x80 if b[5] else 16
    if len(pictures) == 1:
        h = safe_slice(b, first, 0x30)
        return first + u16(h, 12) + (0 if no_image else u32(h, 8) + u32(h, 4))
    return first + sum(picture.size for picture in pictures)


def parse_img(b: bytes) -> tuple[str, list[dict]]:
    h = safe_slice(b, 0, 16)
    kind = h[:3]
    if kind not in (b"IMG", b"IM2", b"IM3") or h[3] != 0:
        raise TextureError("invalid IMG header")
    count = u32(h, 8 if kind == b"IM3" else 4)
    stride = 64 if kind == b"IM3" else 48
    if count > 16384 or 16 + count * stride > len(b):
        raise TextureError("invalid IMG entry count")
    entries = []
    for i in range(count):
        start = 16 + i * stride
        e = safe_slice(b, start, stride)
        name = name_bytes(e[:32])
        offset = u32(e, 36 if kind == b"IM3" else 32)
        if offset >= len(b):
            raise TextureError(f"IMG entry {i} offset is outside archive")
        if kind == b"IM3":
            entry = {"name": name, "offset": offset, "swizzled": bool(u32(e, 40)),
                     "block": max(0, struct.unpack_from("<i", e, 44)[0]),
                     "no_image": bool(u16(e, 48)), "clamp": u64(e, 56), "size": u32(e, 52)}
        else:
            entry = {"name": name, "offset": offset, "swizzled": kind == b"IM2",
                     "block": 0, "no_image": False, "clamp": 0, "size": 0}
        entries.append(entry)
    return {b"IMG": "img", b"IM2": "im2", b"IM3": "img3"}[kind], entries


def img_extent(b: bytes) -> int:
    kind, entries = parse_img(b)
    end = 16 + len(entries) * (64 if kind == "img3" else 48)
    for entry in entries:
        size = entry["size"]
        if not size and not entry["name"].startswith("#"):
            size = tim2_extent(b[entry["offset"]:], entry["no_image"])
        end = max(end, entry["offset"] + size)
    safe_slice(b, 0, end)
    return end


def parse_pack(b: bytes) -> list[tuple[str, int, int]] | None:
    """Return PACK members only when the whole table passes strict bounds checks."""
    if len(b) < 76:
        return None
    members = []
    cursor = 0
    seen = set()
    while cursor not in seen and len(members) < 16384:
        seen.add(cursor)
        if cursor + 76 > len(b):
            return None
        e = b[cursor:cursor + 76]
        name = e[:64].split(b"\0", 1)[0]
        if not name:
            return members if members else None
        if len(name) == 64 or any(c < 32 or c in (47, 92) for c in name):
            return None
        offset, size, advance = struct.unpack_from("<III", e, 64)
        if offset < 76 or size == 0 or cursor + offset + size > len(b) or advance < 76:
            return None
        members.append((source_key(name.decode("latin1")), cursor + offset, size, cursor + 68))
        cursor += advance
    return None


def source_key(raw: str) -> str:
    return "".join(f"%{ord(c):02X}" if ord(c) >= 128 or c == "%" else c for c in raw).replace("\\", "/")


def data_version() -> int:
    version = Path(__file__).resolve().parents[1] / "data_version.json"
    metadata = json.loads(version.read_text(encoding="utf-8"))
    if metadata.get("schema_version") != 1 or not isinstance(metadata.get("data_version"), int):
        raise TextureError("invalid shared data version")
    return metadata["data_version"]


def safe_component(value: str) -> str:
    if value in ("", ".", ".."):
        return "".join(f"%{ord(c):02X}" for c in value)
    return "".join(chr(c) if re.fullmatch(rb"[A-Za-z0-9._-]", bytes([c]))
                   else f"%{c:02X}" for c in value.encode("utf-8"))


def legacy_document_path(key: str) -> str:
    raw = [part for part in key.split("/") if part]
    position = 0
    while position < len(raw):
        part = raw[position]
        if "#offset:" in part:
            base, offset = part.rsplit("#offset:", 1)
            raw[position] = base
            raw.insert(position + 1, f"embedded-offset-{offset}.json")
            continue
        if "#entry:" in part:
            base, ordinal = part.split("#entry:", 1)
            stem = base.rsplit(".", 1)[0] if "." in base else base
            raw[position] = f"{stem}-entry-{ordinal}.json"
        position += 1
    parts = [safe_component(x) for x in raw]
    if not parts:
        raise TextureError("empty texture source key")
    for index, part in enumerate(parts[:-1]):
        if part.lower().endswith(".img"):
            parts[index] = part[:-4] + "-img"
    last = parts[-1]
    if last.lower().endswith((".img", ".tm2")):
        parts[-1] = last.rsplit(".", 1)[0] + ".json"
    elif not last.endswith(".json"):
        parts.append("embedded.json")
    path = "/".join(parts)
    if len(path) > 180:
        path = "/".join(parts[:2] + ["long-path-" + hashlib.sha256(path.encode()).hexdigest()[:20] + ".json"])
    return path


def document_path(key: str) -> str:
    """Mirror the native extractor's texture document namespace before collisions."""
    legacy = legacy_document_path(key)
    raw = [part for part in key.split("/") if part]
    parts = ["textures"]
    character = None
    for index, component in enumerate(raw):
        base = component.split("#", 1)[0]
        if "." in base:
            if base.rsplit(".", 1)[1].lower() in ("chr", "chr_"):
                character = safe_component(base)
        elif index + 1 < len(raw):
            parts.append(safe_component(base))
    leaf = legacy.rsplit("/", 1)[-1]
    if character is not None:
        stem = character.rsplit(".", 1)[0]
        suffix = "_" if character.lower().endswith(".chr_") else ""
        leaf = f"{stem}{suffix}-{leaf}"
    parts.append(leaf)
    path = "/".join(parts)
    if len(path) > 160:
        return "textures/long-path-" + hashlib.sha256(key.encode()).hexdigest()[:24] + ".json"
    return path


def relative_to_document(document: str, asset: str) -> str:
    return posixpath.relpath(asset, posixpath.dirname(document) or ".")


def pretty_json(value: dict) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def serialize_source(source: dict) -> dict:
    """Keep only game-facing metadata after all PNGs and variants are baked."""
    textures = [entry for entry in source["entries"] if entry["type"] == "texture"]
    raw_blocks = sorted({entry["block"] for entry in textures}) if source["kind"] == "img3" else [0]
    entries = []
    by_ordinal = {}
    for item in source["entries"]:
        if item["type"] == "cfg":
            entries.append({"name": item["name"], "script": item["script"]})
            continue
        ordinal = item["_ordinal"]
        if ordinal not in by_ordinal:
            block = item["block"] if source["kind"] == "img3" else 0
            entry = {"name": item["name"], "pictures": []}
            if block:
                entry["block"] = block
            by_ordinal[ordinal] = entry
            entries.append(entry)
        picture = {"levels": item["variants"][0]["levels"]} if item["variants"][0]["levels"] else {
            "canvas": {"width": item["width"], "height": item["height"]}}
        variants = [{"selector": variant["palette_source"], "levels": variant["levels"]}
                    for variant in item["variants"][1:]]
        if variants:
            picture["variants"] = variants
        clamp = item["clamp"]
        wrap_u = "repeat" if clamp & 3 == 0 else "clamp"
        wrap_v = "repeat" if (clamp >> 2) & 3 == 0 else "clamp"
        if wrap_u != "repeat" or wrap_v != "repeat":
            picture["sampler"] = {}
            if wrap_u != "repeat":
                picture["sampler"]["wrap_u"] = wrap_u
            if wrap_v != "repeat":
                picture["sampler"]["wrap_v"] = wrap_v
        animation = {24: "draw", 32: "draw", 4: "copy", 16: "copy",
                     8: "copy_variant"}[item["source_bpp"]]
        if animation != "draw":
            picture["animation"] = animation
        by_ordinal[ordinal]["pictures"].append(picture)
    return {"schema_version": 3, "groups": raw_blocks, "entries": entries}


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def may_have_textures(path: Path) -> bool:
    if path.stat().st_size <= (128 << 20):
        return True
    with path.open("rb") as source:
        header = source.read(76)
    if header[:4] == b"TIM2" or header[:3] in (b"IMG", b"IM2", b"IM3") and header[3:4] == b"\0":
        return True
    if len(header) < 76:
        return False
    name = header[:64].split(b"\0", 1)[0]
    if not name or len(name) == 64 or any(c < 32 or c in (47, 92) for c in name):
        return False
    offset, size, advance = struct.unpack_from("<III", header, 64)
    return 76 <= offset < path.stat().st_size and 0 < size <= path.stat().st_size - offset and advance >= 76


def audit_texture_payload(path: Path) -> None:
    """Reject format-valid texture signatures anywhere in the published data."""
    signatures = (b"TIM2", b"IMG\0", b"IM2\0", b"IM3\0")
    with path.open("rb") as source:
        offset = 0
        overlap = b""
        while block := source.read(4 << 20):
            window = overlap + block
            positions = set()
            for signature in signatures:
                start = 0
                while (index := window.find(signature, start)) >= 0:
                    positions.add(offset - len(overlap) + index)
                    start = index + 1
            resume = source.tell()
            for position in sorted(positions):
                source.seek(position)
                candidate = source.read(min(64 << 20, path.stat().st_size - position))
                try:
                    if candidate[:4] == b"TIM2":
                        parse_tim2(candidate)
                    else:
                        parse_img(candidate)
                except (TextureError, struct.error, KeyError):
                    continue
                raise TextureError(f"recognizable retail texture payload remains in {path} at offset {position}")
            source.seek(resume)
            offset += len(block)
            overlap = window[-3:]


class Converter:
    def __init__(self, raw: Path, output: Path):
        self.raw = raw
        self.output = output
        self.data = output / "data"
        self.assets = output / "port_data"
        self.sources: list[dict] = []
        self.written: dict[str, str] = {}
        self.data_hashes: dict[str, str] = {}
        self.pictures: list[dict] = []
        self.animation_pairs: list[tuple[str, str, str]] = []
        self.next_asset_index: dict[str, int] = {}

    def write_asset(self, document: str, content: bytes) -> str:
        index = self.next_asset_index.get(document, 0)
        self.next_asset_index[document] = index + 1
        relative = posixpath.join(posixpath.dirname(document),
                                  f"{posixpath.basename(document)[:-5]}_{index}.png")
        path = self.assets / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256(content).hexdigest()
        if path.is_file() and file_hash(path) == digest:
            self.written[relative] = digest
            return relative
        for attempt in range(30):
            try:
                path.write_bytes(content)
                break
            except OSError as error:
                # Windows can transiently reject an overwrite while a scanner
                # has a generated PNG open, including errno 22 on this host.
                if attempt == 29 or not isinstance(error, PermissionError) and error.errno != 22:
                    raise
                time.sleep(1)
        self.written[relative] = digest
        return relative

    def convert_pictures(self, key: str, name: str, pictures: list[Picture], metadata: dict,
                         ordinal: int = 0) -> list[dict]:
        out = []
        document = legacy_document_path(key)
        for index, picture in enumerate(pictures):
            levels = [self.write_asset(document,
                                       png(w, h, colorize(raster, picture.palette)
                                           if channels == 1 else raster))
                      for w, h, raster, channels in picture.levels]
            entry = {"type": "texture", "name": name, "picture": index,
                     "_ordinal": ordinal,
                     "width": picture.width, "height": picture.height,
                     "bpp": 32, "source_bpp": picture.bpp,
                     "tex0": picture.tex0, "tex1": picture.tex1,
                     "format": "rgba8", "variants": [{"palette_source": name,
                     "levels": [relative_to_document(document, level) for level in levels]}],
                     **metadata}
            if picture.palette is not None:
                self.pictures.append({"source": key, "name": name, "picture": picture,
                                      "entry": entry, "document": document})
            out.append(entry)
        return out

    def bake_animation_variants(self) -> None:
        by_source: dict[tuple[str, str], list[dict]] = {}
        by_name: dict[str, list[dict]] = {}
        for record in self.pictures:
            by_source.setdefault((record["source"], record["name"]), []).append(record)
            by_name.setdefault(record["name"], []).append(record)
        def resolve(container: str, name: str) -> list[dict]:
            local = by_source.get((container, name))
            if local:
                return local
            global_records = by_name.get(name, [])
            return global_records if len({r["source"] for r in global_records}) == 1 else []

        pairs = sorted(set(self.animation_pairs))
        origins: dict[tuple[str, str], set[str]] = {}
        for container, source_name, dest_name in pairs:
            origins.setdefault((container, source_name), {source_name})
            origins.setdefault((container, dest_name), {dest_name})
        # A copied source may itself already have adopted another palette.
        # Close the finite CFG graph so every reachable colour origin is baked.
        changed = True
        while changed:
            changed = False
            for container, source_name, dest_name in pairs:
                dest_origins = origins[(container, dest_name)]
                before = len(dest_origins)
                dest_origins.update(origins[(container, source_name)])
                changed |= len(dest_origins) != before
        for (container, dest_name), palette_origins in sorted(origins.items()):
            dests = resolve(container, dest_name)
            for origin in sorted(palette_origins - {dest_name}):
                sources = resolve(container, origin)
                if not sources:
                    continue
                source_palette = sources[0]["picture"].palette
                for dest in dests:
                    picture = dest["picture"]
                    if not picture.levels:
                        continue
                    entry = dest["entry"]
                    if any(v["palette_source"] == origin for v in entry["variants"]):
                        continue
                    levels = [self.write_asset(dest["document"],
                                               png(w, h, colorize(raster, source_palette)))
                              for w, h, raster, _ in picture.levels]
                    entry["variants"].append({"palette_source": origin,
                        "levels": [relative_to_document(dest["document"], level) for level in levels]})

    def collect_animation_pairs(self, container: str, script: list[dict]) -> None:
        source = dest = None
        for statement in script:
            args = statement["args"]
            if statement["command"] == "SRC_TEX" and args:
                source = args[0].get("text")
            elif statement["command"] == "DEST_TEX" and args:
                dest = args[0].get("text")
            elif statement["command"] == "TEX_ANIME_DATA_END":
                if source and dest:
                    self.animation_pairs.append((container, source, dest))
                source = dest = None

    def transform(self, original: bytes, key: str, depth: int = 0) -> bytes | None:
        if depth > 8:
            raise TextureError(f"PACK nesting exceeds 8 at {key}")
        if original[:3] in (b"IMG", b"IM2", b"IM3") and original[3:4] == b"\0":
            kind, entries = parse_img(original)
            if kind == "img3":
                # Retail sorts IMG3 headers in place before entering them. CFG
                # headers are barred from moving into the earlier slot.
                for a in range(len(entries) - 1):
                    for b in range(a + 1, len(entries)):
                        if entries[a]["block"] > entries[b]["block"] and not entries[b]["name"].startswith("#"):
                            entries[a], entries[b] = entries[b], entries[a]
            converted = []
            for ordinal, e in enumerate(entries):
                if e["name"].startswith("#"):
                    raw_cfg = safe_slice(original, e["offset"], e["size"])
                    script = parse_cfg(raw_cfg)
                    self.collect_animation_pairs(key, script)
                    converted.append({"type": "cfg", "name": e["name"], "script": script})
                    continue
                pictures = parse_tim2(original[e["offset"]:], e["swizzled"], e["no_image"])
                converted.extend(self.convert_pictures(key, e["name"], pictures,
                    {k: e[k] for k in ("block", "swizzled", "clamp", "no_image")}, ordinal))
            self.sources.append({"source": key, "kind": kind, "entries": converted})
            return None
        if original[:4] == b"TIM2":
            pictures = parse_tim2(original)
            self.sources.append({"source": key, "kind": "tm2", "entries":
                                 self.convert_pictures(key, Path(key).stem, pictures,
                                                       {"block": 0, "swizzled": False,
                                                        "clamp": 0, "no_image": False})})
            return None
        members = parse_pack(original)
        if members is not None:
            changed = False
            result = bytearray(original)
            for name, start, size, size_field in members:
                child = original[start:start + size]
                transformed = self.transform(child, f"{key}/{name}", depth + 1)
                if transformed is None:
                    result[start:start + size] = bytes(size)
                    struct.pack_into("<I", result, size_field, 0)
                    changed = True
                elif transformed != child:
                    result[start:start + size] = transformed
                    changed = True
            if changed:
                return bytes(result)
            return original
        # Character and effect wrappers can point directly to IMG/TIM2 bodies
        # without using PACK_ENTRY. Preserve all other bytes and file offsets.
        result = bytearray(original)
        changed = False
        position = 0
        while position + 4 <= len(original):
            candidates = [index for signature in (b"IMG\0", b"IM2\0", b"IM3\0", b"TIM2")
                          if (index := original.find(signature, position)) >= 0]
            if not candidates:
                break
            position = min(candidates)
            candidate = original[position:]
            try:
                if candidate[:4] == b"TIM2":
                    size = tim2_extent(candidate)
                else:
                    size = img_extent(candidate)
                transformed = self.transform(candidate[:size], f"{key}#offset:{position}", depth + 1)
                if transformed is not None:
                    raise TextureError("embedded texture did not convert")
            except (TextureError, struct.error, KeyError):
                position += 4
                continue
            result[position:position + size] = bytes(size)
            changed = True
            position += size
        if changed:
            return bytes(result)
        return original

    def process_file(self, path: Path) -> None:
        relative = path.relative_to(self.raw)
        key = relative.as_posix()
        target = self.data / relative
        if path.suffix.lower() in (".img", ".tm2") and path.stat().st_size == 0:
            self.sources.append({"source": key, "kind": "empty", "entries": []})
            return
        if may_have_textures(path):
            transformed = self.transform(path.read_bytes(), key)
            if transformed is None:
                return
            if path.suffix.lower() in (".img", ".tm2"):
                raise TextureError(f"unrecognized standalone texture format: {key}")
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.is_file() or file_hash(target) != hashlib.sha256(transformed).hexdigest():
                target.write_bytes(transformed)
        else:
            if path.suffix.lower() in (".img", ".tm2"):
                raise TextureError(f"unrecognized standalone texture format: {key}")
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.is_file() or file_hash(target) != file_hash(path):
                shutil.copyfile(path, target)
        self.data_hashes[key] = file_hash(target)
        audit_texture_payload(target)

    def run(self, jobs: int | None = None) -> dict:
        self.data.mkdir(parents=True, exist_ok=True)
        self.assets.mkdir(parents=True, exist_ok=True)
        jobs = min(8, os.cpu_count() or 1) if jobs is None else jobs
        if jobs < 1 or jobs > 64:
            raise TextureError("jobs must be between 1 and 64")
        paths = [path for path in sorted(self.raw.rglob("*"))
                 if path.is_file() and path.relative_to(self.raw).as_posix() != "manifest.txt"]
        def work(path: Path) -> Converter:
            unit = Converter(self.raw, self.output)
            unit.process_file(path)
            return unit
        results: dict[int, Converter] = {}
        errors = []
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            pending = {pool.submit(work, path): (index, path) for index, path in enumerate(paths)}
            for future in as_completed(pending):
                index, path = pending[future]
                try:
                    results[index] = future.result()
                except Exception as error:
                    errors.append((path.relative_to(self.raw).as_posix(), error))
        if errors:
            sample = "; ".join(f"{name}: {error}" for name, error in errors[:8])
            raise TextureError(f"{len(errors)} texture conversion worker(s) failed: {sample}")
        for index in sorted(results):
            unit = results[index]
            self.sources.extend(unit.sources)
            self.pictures.extend(unit.pictures)
            self.animation_pairs.extend(unit.animation_pairs)
            self.written.update(unit.written)
            self.data_hashes.update(unit.data_hashes)
            self.next_asset_index.update(unit.next_asset_index)
        self.bake_animation_variants()
        document_paths = set()
        for source in self.sources:
            relative = legacy_document_path(source["source"])
            collision_key = relative.casefold()
            if collision_key in document_paths or collision_key in ("catalog.json", "manifest.json"):
                raise TextureError(f"texture document path collision: {relative}")
            document_paths.add(collision_key)
            document = self.assets / relative
            document.parent.mkdir(parents=True, exist_ok=True)
            document.write_text(pretty_json(serialize_source(source)), encoding="utf-8")
            self.written[relative] = file_hash(document)
        (self.output / "data.json").write_text(
            pretty_json({"schema_version": 1, "data_version": data_version()}), encoding="utf-8")
        return {"sources": len(self.sources), "pngs": sum(path.endswith(".png") for path in self.written)}


def parse_cfg(raw: bytes) -> list[dict]:
    """Represent animation CFG as statements, never as its source byte stream."""
    text = raw.decode("latin1").strip("\0")
    statements = []
    for statement in text.split(";"):
        statement = statement.strip()
        if not statement or statement.startswith("//"):
            continue
        command, _, arguments = statement.partition(" ")
        if not command.isidentifier():
            raise TextureError(f"unrecognized animation statement {command!r}")
        values = []
        for value in arguments.split(",") if arguments else []:
            value = value.strip()
            if value.startswith('"') and value.endswith('"'):
                values.append({"text": source_key(value[1:-1])})
            else:
                try:
                    values.append({"number": float(value) if "." in value else int(value, 0)})
                except ValueError as error:
                    raise TextureError(f"invalid animation argument {value!r}") from error
        statements.append({"command": command, "args": values})
    return statements


def verify(output: Path) -> int:
    marker = json.loads((output / "data.json").read_text(encoding="utf-8"))
    if (not isinstance(marker, dict) or type(marker.get("schema_version")) is not int
            or marker["schema_version"] != 1 or type(marker.get("data_version")) is not int
            or marker["data_version"] != data_version()):
        raise TextureError("data.json has an unsupported data version")
    assets = output / "port_data"
    seen = set()
    texture_count = 0
    for document in sorted(assets.rglob("*.json")):
        relative = document.relative_to(assets).as_posix()
        if relative.casefold() in ("catalog.json", "manifest.json"):
            raise TextureError(f"obsolete global texture metadata: {relative}")
        payload = document.read_text(encoding="utf-8")
        local = json.loads(payload)
        if not isinstance(local, dict) or not (
                "groups" in local or local.get("schema_version") == 3 and "entries" in local):
            # Audio, model, and configuration JSON documents share port_data.
            continue
        if relative.casefold() in seen:
            raise TextureError(f"colliding texture document path: {relative}")
        seen.add(relative.casefold())
        texture_count += 1
        if (not payload.endswith("\n") or set(local) != {"schema_version", "groups", "entries"}
                or type(local.get("schema_version")) is not int or local["schema_version"] != 3):
            raise TextureError(f"invalid texture document: {relative}")
        groups = local["groups"]
        if (not isinstance(groups, list) or any(type(group) is not int for group in groups)
                or groups != sorted(set(groups)) or any(group < 0 for group in groups)
                or not isinstance(local["entries"], list)):
            raise TextureError(f"invalid texture groups or entries: {relative}")
        for entry in local["entries"]:
            if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
                raise TextureError(f"invalid texture entry: {relative}")
            if "script" in entry:
                if set(entry) != {"name", "script"} or not isinstance(entry["script"], list):
                    raise TextureError(f"invalid animation entry: {relative}")
                continue
            if (not set(entry) <= {"name", "block", "pictures"}
                    or not isinstance(entry.get("pictures"), list) or not entry["pictures"]
                    or type(entry.get("block", 0)) is not int
                    or entry.get("block", 0) not in groups
                    or "block" in entry and entry["block"] == 0):
                raise TextureError(f"invalid texture entry: {relative}")
            for picture in entry["pictures"]:
                if (not isinstance(picture, dict) or not set(picture) <=
                        {"levels", "canvas", "variants", "sampler", "animation"}
                        or ("levels" in picture) == ("canvas" in picture)):
                    raise TextureError(f"invalid picture: {relative}")
                if "canvas" in picture:
                    canvas = picture["canvas"]
                    if (not isinstance(canvas, dict) or set(canvas) != {"width", "height"}
                            or any(type(value) is not int or value <= 0 for value in canvas.values())):
                        raise TextureError(f"invalid texture canvas: {relative}")
                if "animation" in picture and picture["animation"] not in ("copy", "copy_variant"):
                    raise TextureError(f"invalid texture animation: {relative}")
                if "sampler" in picture:
                    sampler = picture["sampler"]
                    allowed = {"wrap_u": "repeat", "wrap_v": "repeat", "filter": "linear"}
                    if (not isinstance(sampler, dict) or not sampler or not set(sampler) <= set(allowed)
                            or any(value not in ({"repeat", "clamp"} if key.startswith("wrap") else {"linear", "nearest"})
                                   or value == allowed[key] for key, value in sampler.items())):
                        raise TextureError(f"invalid texture sampler: {relative}")
                variants = picture.get("variants", [])
                if not isinstance(variants, list) or "variants" in picture and not variants:
                    raise TextureError(f"invalid texture variants: {relative}")
                level_sets = [picture["levels"]] if "levels" in picture else []
                for variant in variants:
                    if (not isinstance(variant, dict) or set(variant) != {"selector", "levels"}
                            or not isinstance(variant["selector"], str)
                            or variant["selector"] == entry["name"]):
                        raise TextureError(f"invalid texture variant: {relative}")
                    level_sets.append(variant["levels"])
                for levels in level_sets:
                    if not isinstance(levels, list) or not levels:
                        raise TextureError(f"missing PNG levels: {relative}")
                    for reference in levels:
                        if (not isinstance(reference, str) or not reference.lower().endswith(".png")
                                or "/" in reference or "\\" in reference or reference in (".", "..")):
                            raise TextureError(f"invalid texture asset path: {reference}")
                        canonical = (Path(relative).parent / reference).as_posix()
                        if not (assets / canonical).is_file():
                            raise TextureError(f"texture asset missing: {reference}")
                        with (assets / canonical).open("rb") as image:
                            header = image.read(24)
                        if header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR" or not all(
                                struct.unpack_from(">II", header, 16)):
                            raise TextureError(f"invalid texture PNG: {reference}")
    for tree in (output / "data", assets):
        for item in tree.rglob("*"):
            if item.suffix.lower() == ".img":
                raise TextureError(f"published IMG path remains: {item}")
    for legacy in (output / "data/manifest.json", assets / "manifest.json", assets / "catalog.json"):
        if legacy.exists():
            raise TextureError(f"obsolete global metadata remains: {legacy}")
    return texture_count


def publish(staged: Path, destination: Path) -> None:
    """Replace both generated trees, restoring both old trees if any step fails."""
    verify(staged)
    destination = destination.resolve()
    data = destination / "data"
    textures = destination / "port_data"
    staged_data = staged / "data"
    staged_textures = staged / "port_data"
    if staged_data.resolve() == data or staged_textures.resolve() == textures:
        raise TextureError("staging and published trees overlap")
    destination.mkdir(parents=True, exist_ok=True)
    textures.parent.mkdir(parents=True, exist_ok=True)
    nonce = uuid.uuid4().hex
    backup_data = destination / f".dc2data-data-backup-{nonce}"
    backup_textures = destination / f".dc2data-port-data-backup-{nonce}"
    marker = destination / "data.json"
    backup_marker = destination / f".dc2data-marker-backup-{nonce}"
    old_data = old_textures = new_data = new_textures = old_marker = new_marker = False
    def replace(source: Path, target: Path) -> None:
        # Indexers and antivirus scanners may hold a newly created PNG briefly
        # on Windows. A bounded retry keeps publication atomic in that case.
        if target.exists():
            raise TextureError(f"publication target already exists: {target}")
        for attempt in range(30):
            try:
                source.rename(target)
                return
            except PermissionError:
                if attempt == 29:
                    raise
                time.sleep(1)

    try:
        if marker.exists():
            replace(marker, backup_marker)
            old_marker = True
        if data.exists():
            replace(data, backup_data)
            old_data = True
        if textures.exists():
            replace(textures, backup_textures)
            old_textures = True
        replace(staged_data, data)
        new_data = True
        replace(staged_textures, textures)
        new_textures = True
        replace(staged / "data.json", marker)
        new_marker = True
        verify(destination)
    except Exception:
        if new_data:
            replace(data, staged_data)
        if new_textures:
            replace(textures, staged_textures)
        if old_data:
            replace(backup_data, data)
        if old_textures:
            replace(backup_textures, textures)
        if new_marker:
            replace(marker, staged / "data.json")
        if old_marker:
            replace(backup_marker, marker)
        raise
    if old_data:
        shutil.rmtree(backup_data)
    if old_textures:
        shutil.rmtree(backup_textures)
    if old_marker:
        backup_marker.unlink()
