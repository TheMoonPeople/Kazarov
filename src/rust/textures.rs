use super::{CoreError, CoreResult};
use rayon::prelude::*;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}
fn slice(bytes: &[u8], at: usize, size: usize) -> CoreResult<&[u8]> {
    bytes
        .get(at..at.checked_add(size).ok_or_else(|| fail("range overflow"))?)
        .ok_or_else(|| fail(format!("range {at}+{size} exceeds {} bytes", bytes.len())))
}
fn u16le(b: &[u8], at: usize) -> CoreResult<u16> {
    Ok(u16::from_le_bytes(slice(b, at, 2)?.try_into().unwrap()))
}
fn u32le(b: &[u8], at: usize) -> CoreResult<u32> {
    Ok(u32::from_le_bytes(slice(b, at, 4)?.try_into().unwrap()))
}
fn u64le(b: &[u8], at: usize) -> CoreResult<u64> {
    Ok(u64::from_le_bytes(slice(b, at, 8)?.try_into().unwrap()))
}
fn alpha(a: u8) -> u8 {
    a.saturating_mul(2)
}
fn csm1(i: usize) -> usize {
    (i & !0x18) | ((i & 8) << 1) | ((i & 16) >> 1)
}
fn swizzled_index(x: usize, y: usize, width: usize) -> usize {
    let block = (y & !15) * width + (x & !15) * 2;
    let swap = (((y + 2) >> 2) & 1) * 4;
    let pos_y = (((y & !3) >> 1) + (y & 1)) & 7;
    let column = pos_y * width * 2 + ((x + swap) & 7) * 4;
    block + column + (((y >> 1) & 1) + ((x >> 2) & 2))
}
fn level_size(width: usize, height: usize, kind: u8) -> CoreResult<usize> {
    let count = width
        .checked_mul(height)
        .ok_or_else(|| fail("image dimensions overflow"))?;
    match kind {
        1 => Ok(count * 2),
        2 => Ok(count * 3),
        3 => Ok(count * 4),
        4 => Ok(count.div_ceil(2)),
        5 => Ok(count),
        _ => Err(fail("unsupported TIM2 type")),
    }
}

#[derive(Clone)]
struct Level {
    width: usize,
    height: usize,
    pixels: Vec<u8>,
    indexed: bool,
}
#[derive(Clone)]
struct Picture {
    width: usize,
    height: usize,
    source_bpp: u8,
    tex0: u64,
    tex1: u64,
    levels: Vec<Level>,
    palette: Option<Vec<u8>>,
    size: usize,
}
fn decode_level(
    raw: &[u8],
    width: usize,
    height: usize,
    kind: u8,
    swizzled: bool,
) -> CoreResult<Level> {
    let count = width * height;
    let mut pixels = Vec::with_capacity(count * if kind >= 4 { 1 } else { 4 });
    match kind {
        1 => {
            for i in 0..count {
                let v = u16le(raw, i * 2)?;
                let (r, g, b) = (
                    ((v & 31) << 3) as u8,
                    (((v >> 5) & 31) << 3) as u8,
                    (((v >> 10) & 31) << 3) as u8,
                );
                pixels.extend_from_slice(&[
                    r,
                    g,
                    b,
                    if v & 0x8000 != 0 || r | g | b != 0 {
                        255
                    } else {
                        0
                    },
                ]);
            }
        }
        2 => {
            for i in 0..count {
                pixels.extend_from_slice(slice(raw, i * 3, 3)?);
                pixels.push(255);
            }
        }
        3 => {
            for i in 0..count {
                let p = slice(raw, i * 4, 4)?;
                pixels.extend_from_slice(&[p[0], p[1], p[2], alpha(p[3])]);
            }
        }
        4 => {
            for i in 0..count {
                pixels.push((raw[i / 2] >> if i & 1 == 0 { 0 } else { 4 }) & 15);
            }
        }
        5 => {
            if swizzled && width.is_multiple_of(16) && height.is_multiple_of(16) {
                for y in 0..height {
                    for x in 0..width {
                        pixels.push(
                            *raw.get(swizzled_index(x, y, width))
                                .ok_or_else(|| fail("swizzle outside image"))?,
                        );
                    }
                }
            } else {
                pixels.extend_from_slice(slice(raw, 0, count)?);
            }
        }
        _ => return Err(fail("unsupported TIM2 image type")),
    }
    Ok(Level {
        width,
        height,
        pixels,
        indexed: kind >= 4,
    })
}
fn decode_palette(raw: &[u8], image_type: u8, clut_type: u8) -> CoreResult<Vec<u8>> {
    let count = if image_type == 4 { 16 } else { 256 };
    let clut16 = clut_type & 0x3f == 1;
    slice(raw, 0, count * if clut16 { 2 } else { 4 })?;
    let mut out = vec![0; 256 * 4];
    for i in 0..count {
        let rgba = if clut16 {
            let v = u16le(raw, i * 2)?;
            let (r, g, b) = (
                ((v & 31) << 3) as u8,
                (((v >> 5) & 31) << 3) as u8,
                (((v >> 10) & 31) << 3) as u8,
            );
            [
                r,
                g,
                b,
                if v & 0x8000 != 0 || r | g | b != 0 {
                    255
                } else {
                    0
                },
            ]
        } else {
            let p = slice(raw, i * 4, 4)?;
            [p[0], p[1], p[2], alpha(p[3])]
        };
        let slot = if image_type == 5 && clut_type & 0xc0 == 0 {
            csm1(i)
        } else {
            i
        };
        out[slot * 4..slot * 4 + 4].copy_from_slice(&rgba);
    }
    Ok(out)
}
fn parse_picture(b: &[u8], offset: usize, swizzled: bool, no_image: bool) -> CoreResult<Picture> {
    let h = slice(b, offset, 0x30)?;
    let mut total = u32le(h, 0)? as usize;
    let clut_size = u32le(h, 4)? as usize;
    let image_size = u32le(h, 8)? as usize;
    let header_size = u16le(h, 12)? as usize;
    let (mips, clut_type, kind) = (h[17], h[18], h[19]);
    let (width, height) = (u16le(h, 20)? as usize, u16le(h, 22)? as usize);
    if width == 0 || height == 0 || !(1..=5).contains(&kind) || mips > 16 {
        return Err(fail("invalid TIM2 picture dimensions or format"));
    }
    let minimum = header_size + if no_image { 0 } else { image_size + clut_size };
    let min_header = if mips > 1 {
        0x40 + 4 * usize::from(mips.min(4))
    } else {
        0x30
    };
    if header_size < min_header || offset + minimum > b.len() {
        return Err(fail("invalid TIM2 picture sizes"));
    }
    if total < minimum || offset + total > b.len() {
        total = minimum;
    }
    let mut levels = Vec::new();
    let mut position = offset + header_size;
    if !no_image {
        for i in 0..usize::from(mips.clamp(1, 4)) {
            let (w, h) = ((width >> i).max(1), (height >> i).max(1));
            let needed = level_size(w, h, kind)?;
            if position + needed > offset + header_size + image_size {
                return Err(fail("TIM2 mip extends beyond image"));
            }
            levels.push(decode_level(
                slice(b, position, needed)?,
                w,
                h,
                kind,
                swizzled,
            )?);
            position += if mips > 1 {
                u32le(b, offset + 0x40 + i * 4)? as usize
            } else {
                needed
            };
        }
    }
    let palette = if kind >= 4 && !no_image {
        if clut_size == 0 {
            return Err(fail("indexed TIM2 has no CLUT"));
        }
        Some(decode_palette(
            slice(b, offset + header_size + image_size, clut_size)?,
            kind,
            clut_type,
        )?)
    } else {
        None
    };
    Ok(Picture {
        width,
        height,
        source_bpp: [0, 16, 24, 32, 4, 8][kind as usize],
        tex0: u64le(h, 24)?,
        tex1: u64le(h, 32)?,
        levels,
        palette,
        size: total,
    })
}
fn parse_tim2(b: &[u8], swizzled: bool, no_image: bool) -> CoreResult<Vec<Picture>> {
    let h = slice(b, 0, 16)?;
    if &h[..4] != b"TIM2" || h[5] > 1 {
        return Err(fail("invalid TIM2 header"));
    }
    let count = u16le(h, 6)? as usize;
    if count == 0 || count > 256 {
        return Err(fail("invalid TIM2 picture count"));
    }
    let mut position = if h[5] != 0 { 0x80 } else { 16 };
    let mut out = Vec::with_capacity(count);
    for _ in 0..count {
        let picture = parse_picture(b, position, swizzled, no_image)?;
        position += picture.size;
        out.push(picture);
    }
    Ok(out)
}
fn tim2_extent(b: &[u8]) -> CoreResult<usize> {
    let pictures = parse_tim2(b, false, false)?;
    let first = if b[5] != 0 { 0x80 } else { 16 };
    if pictures.len() == 1 {
        let h = slice(b, first, 0x30)?;
        Ok(first + u16le(h, 12)? as usize + u32le(h, 8)? as usize + u32le(h, 4)? as usize)
    } else {
        Ok(first + pictures.iter().map(|p| p.size).sum::<usize>())
    }
}

fn encoded(bytes: &[u8]) -> String {
    let mut result = String::new();
    for &byte in bytes {
        if byte >= 128 || byte == b'%' {
            result.push_str(&format!("%{byte:02X}"));
        } else if byte == b'\\' {
            result.push('/');
        } else {
            result.push(char::from(byte));
        }
    }
    result
}
fn safe_component(input: &str) -> String {
    if input == "." || input == ".." {
        return "%2E".repeat(input.len());
    }
    let mut out = String::new();
    for byte in input.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-') {
            out.push(char::from(byte));
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}
pub(super) fn document_path(key: &str) -> CoreResult<String> {
    let mut raw: Vec<String> = key
        .split('/')
        .filter(|p| !p.is_empty())
        .map(str::to_owned)
        .collect();
    let mut position = 0;
    while position < raw.len() {
        if let Some((parent, offset)) = raw[position].rsplit_once("#offset:") {
            let parent = parent.to_owned();
            let offset = offset.to_owned();
            raw[position] = parent;
            raw.insert(position + 1, format!("embedded-offset-{offset}.json"));
            continue;
        }
        if let Some((name, ordinal)) = raw[position].split_once("#entry:") {
            let stem = name.rsplit_once('.').map_or(name, |(stem, _)| stem);
            raw[position] = format!("{stem}-entry-{ordinal}.json");
        }
        position += 1;
    }
    let mut parts: Vec<String> = raw.iter().map(|p| safe_component(p)).collect();
    if parts.is_empty() {
        return Err(fail("empty texture source key"));
    }
    let prior_count = parts.len() - 1;
    for part in &mut parts[..prior_count] {
        if part.to_ascii_lowercase().ends_with(".img") {
            part.truncate(part.len() - 4);
            part.push_str("-img");
        }
    }
    let last = parts.last_mut().unwrap();
    let lower = last.to_ascii_lowercase();
    if lower.ends_with(".tm2") || lower.ends_with(".img") {
        last.truncate(last.len() - 4);
        last.push_str(".json");
    } else if !last.ends_with(".json") {
        parts.push("embedded.json".into());
    }
    let path = parts.join("/");
    if path.len() > 180 {
        let digest = Sha256::digest(path.as_bytes());
        let prefix = hex_digest(&digest)[..20].to_owned();
        parts.truncate(parts.len().min(2));
        parts.push(format!("long-path-{prefix}.json"));
        return Ok(parts.join("/"));
    }
    Ok(path)
}
fn hex_digest(digest: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(digest.len() * 2);
    for &byte in digest {
        out.push(char::from(HEX[(byte >> 4) as usize]));
        out.push(char::from(HEX[(byte & 0xf) as usize]));
    }
    out
}
fn file_hash(path: &Path) -> CoreResult<String> {
    use std::io::Read;
    let mut file = fs::File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1024 * 1024];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(hex_digest(&hasher.finalize()))
}
fn png_rgba(width: usize, height: usize, pixels: &[u8]) -> CoreResult<Vec<u8>> {
    if pixels.len() != width * height * 4 {
        return Err(fail("invalid PNG RGBA raster"));
    }
    let mut bytes = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut bytes, width as u32, height as u32);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().map_err(|e| fail(e.to_string()))?;
        writer
            .write_image_data(pixels)
            .map_err(|e| fail(e.to_string()))?;
    }
    Ok(bytes)
}
fn colorize(indices: &[u8], palette: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(indices.len() * 4);
    for &index in indices {
        out.extend_from_slice(&palette[index as usize * 4..index as usize * 4 + 4]);
    }
    out
}

#[derive(Clone)]
struct ImgEntry {
    name: String,
    offset: usize,
    swizzled: bool,
    block: i32,
    no_image: bool,
    clamp: u64,
    size: usize,
}
fn parse_img(b: &[u8]) -> CoreResult<(&'static str, Vec<ImgEntry>)> {
    let h = slice(b, 0, 16)?;
    let kind = match &h[..4] {
        b"IMG\0" => "img",
        b"IM2\0" => "im2",
        b"IM3\0" => "img3",
        _ => return Err(fail("invalid IMG header")),
    };
    let count = u32le(h, if kind == "img3" { 8 } else { 4 })? as usize;
    let stride = if kind == "img3" { 64 } else { 48 };
    if count > 16384 || 16 + count * stride > b.len() {
        return Err(fail("invalid IMG entry count"));
    }
    let mut entries = Vec::with_capacity(count);
    for i in 0..count {
        let e = slice(b, 16 + i * stride, stride)?;
        let name_end = e[..32].iter().position(|&v| v == 0).unwrap_or(32);
        let name = encoded(&e[..name_end]);
        let offset = u32le(e, if kind == "img3" { 36 } else { 32 })? as usize;
        if offset >= b.len() {
            return Err(fail(format!("IMG entry {i} offset outside archive")));
        }
        let (swizzled, block, no_image, clamp, size) = if kind == "img3" {
            (
                u32le(e, 40)? != 0,
                i32::from_le_bytes(slice(e, 44, 4)?.try_into().unwrap()).max(0),
                u16le(e, 48)? != 0,
                u64le(e, 56)?,
                u32le(e, 52)? as usize,
            )
        } else {
            (kind == "im2", 0, false, 0, 0)
        };
        entries.push(ImgEntry {
            name,
            offset,
            swizzled,
            block,
            no_image,
            clamp,
            size,
        });
    }
    Ok((kind, entries))
}
fn img_extent(b: &[u8]) -> CoreResult<usize> {
    let (kind, entries) = parse_img(b)?;
    let mut end = 16 + entries.len() * if kind == "img3" { 64 } else { 48 };
    for entry in entries {
        let size = if entry.size == 0 && !entry.name.starts_with('#') {
            tim2_extent(&b[entry.offset..])?
        } else {
            entry.size
        };
        end = end.max(entry.offset + size);
    }
    slice(b, 0, end)?;
    Ok(end)
}
fn parse_pack(b: &[u8]) -> Option<Vec<(String, usize, usize, usize)>> {
    if b.len() < 76 || super::native_scripts::is_valid_script(b) {
        return None;
    }
    let mut members = Vec::new();
    let mut seen = BTreeSet::new();
    let mut cursor = 0usize;
    while seen.insert(cursor) && members.len() < 16384 {
        let e = b.get(cursor..cursor + 76)?;
        let end = e[..64].iter().position(|&v| v == 0).unwrap_or(64);
        if end == 0 {
            return if members.is_empty() {
                None
            } else {
                Some(members)
            };
        }
        let name = &e[..end];
        if end == 64 || name.iter().any(|v| *v < 32 || *v == b'/' || *v == b'\\') {
            return None;
        }
        let offset = u32le(e, 64).ok()? as usize;
        let size = u32le(e, 68).ok()? as usize;
        let advance = u32le(e, 72).ok()? as usize;
        if offset < 76 || cursor.checked_add(offset)?.checked_add(size)? > b.len() || advance < 76 {
            return None;
        }
        members.push((encoded(name), cursor + offset, size, cursor + 68));
        cursor = cursor.checked_add(advance)?;
    }
    None
}
fn parse_cfg(raw: &[u8]) -> CoreResult<Vec<Value>> {
    let mut statements = Vec::new();
    let end = raw
        .iter()
        .rposition(|&byte| byte != 0)
        .map_or(0, |index| index + 1);
    let trimmed = &raw[..end];
    for statement in trimmed.split(|&b| b == b';') {
        let statement = statement.trim_ascii();
        if statement.is_empty() || statement.starts_with(b"//") {
            continue;
        }
        let split = statement
            .iter()
            .position(|&b| b == b' ')
            .unwrap_or(statement.len());
        let (command, arguments) = (
            &statement[..split],
            statement.get(split + 1..).unwrap_or(&[]),
        );
        if !command
            .iter()
            .all(|c| c.is_ascii_alphanumeric() || *c == b'_')
        {
            return Err(fail("invalid animation statement"));
        }
        let command =
            std::str::from_utf8(command).map_err(|_| fail("invalid animation command"))?;
        let mut args = Vec::new();
        if !arguments.is_empty() {
            for value in arguments.split(|&b| b == b',') {
                let value = value.trim_ascii();
                if value.starts_with(b"\"") && value.ends_with(b"\"") {
                    args.push(json!({"text":encoded(&value[1..value.len()-1])}));
                } else if value.contains(&b'.') {
                    let text = std::str::from_utf8(value)
                        .map_err(|_| fail("invalid animation argument"))?;
                    args.push(json!({"number":text.parse::<f64>().map_err(|_|fail("invalid animation argument"))?}));
                } else {
                    let text = std::str::from_utf8(value)
                        .map_err(|_| fail("invalid animation argument"))?;
                    let number = if let Some(hex) = text.strip_prefix("0x") {
                        i64::from_str_radix(hex, 16)
                    } else {
                        text.parse::<i64>()
                    };
                    args.push(
                        json!({"number":number.map_err(|_|fail("invalid animation argument"))?}),
                    );
                }
            }
        }
        statements.push(json!({"command":command,"args":args}));
    }
    Ok(statements)
}

pub(super) fn audit_payload(bytes: &[u8], name: &str) -> CoreResult<()> {
    let mut at = 0;
    while at + 4 <= bytes.len() {
        let candidate = &bytes[at..];
        let valid = if candidate.starts_with(b"TIM2") {
            parse_tim2(candidate, false, false).is_ok()
        } else if candidate.starts_with(b"IMG\0")
            || candidate.starts_with(b"IM2\0")
            || candidate.starts_with(b"IM3\0")
        {
            img_extent(candidate).is_ok()
        } else {
            false
        };
        if valid {
            return Err(fail(format!(
                "proprietary texture payload remains in {name} at {at}"
            )));
        }
        at += 1;
    }
    Ok(())
}
pub(super) fn large_known_non_texture(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    lower == "sound/sound.dat" || (lower.starts_with("movie/") && lower.ends_with(".pss"))
}
fn possible_texture(path: &Path, name: &str) -> CoreResult<bool> {
    if fs::metadata(path)?.len() <= 128 << 20 {
        return Ok(true);
    }
    if large_known_non_texture(name) {
        return Ok(false);
    }
    Err(fail(format!(
        "large file requires explicit texture audit: {name}"
    )))
}
fn write_if_changed(path: &Path, content: &[u8]) -> CoreResult<String> {
    let digest = hex_digest(&Sha256::digest(content));
    if path.is_file() && file_hash(path)? == digest {
        return Ok(digest);
    }
    fs::create_dir_all(path.parent().ok_or_else(|| fail("no output parent"))?)?;
    for attempt in 0..30 {
        match fs::write(path, content) {
            Ok(()) => return Ok(digest),
            Err(error)
                if attempt < 29
                    && matches!(
                        error.kind(),
                        std::io::ErrorKind::PermissionDenied | std::io::ErrorKind::InvalidInput
                    ) =>
            {
                std::thread::sleep(std::time::Duration::from_secs(1));
            }
            Err(error) => return Err(error.into()),
        }
    }
    unreachable!()
}
fn pretty_json(value: &Value) -> CoreResult<Vec<u8>> {
    let mut bytes = serde_json::to_vec_pretty(value)?;
    bytes.push(b'\n');
    Ok(bytes)
}
fn collect_pairs(container: &str, script: &[Value], pairs: &mut Vec<(String, String, String)>) {
    let (mut source, mut dest) = (None, None);
    for statement in script {
        let command = statement["command"].as_str().unwrap_or("");
        let args = statement["args"].as_array();
        match command {
            "SRC_TEX" => {
                source = args
                    .and_then(|a| a.first())
                    .and_then(|v| v["text"].as_str())
                    .map(str::to_owned)
            }
            "DEST_TEX" => {
                dest = args
                    .and_then(|a| a.first())
                    .and_then(|v| v["text"].as_str())
                    .map(str::to_owned)
            }
            "TEX_ANIME_DATA_END" => {
                if let (Some(s), Some(d)) = (source.take(), dest.take()) {
                    pairs.push((container.to_owned(), s, d));
                }
            }
            _ => {}
        }
    }
}
#[derive(Clone)]
struct PictureRecord {
    source_idx: usize,
    entry_idx: usize,
    source: String,
    name: String,
    picture: Picture,
    document: String,
}
fn relative_raster(document: &str, asset: &str) -> CoreResult<String> {
    let mut directory: Vec<_> = document.split('/').collect();
    directory
        .pop()
        .ok_or_else(|| fail("texture document has no filename"))?;
    let parts: Vec<_> = asset.split('/').collect();
    let common = directory
        .iter()
        .zip(&parts)
        .take_while(|(a, b)| a == b)
        .count();
    let mut relative = vec![".."; directory.len() - common];
    relative.extend_from_slice(&parts[common..]);
    Ok(relative.join("/"))
}
struct Unit {
    sources: Vec<Value>,
    pictures: Vec<PictureRecord>,
    pairs: Vec<(String, String, String)>,
    written: BTreeMap<String, String>,
    data_hashes: BTreeMap<String, String>,
    asset_counts: BTreeMap<String, usize>,
    raster_origins: BTreeMap<String, (String, usize)>,
}
impl Unit {
    fn new() -> Self {
        Self {
            sources: Vec::new(),
            pictures: Vec::new(),
            pairs: Vec::new(),
            written: BTreeMap::new(),
            data_hashes: BTreeMap::new(),
            asset_counts: BTreeMap::new(),
            raster_origins: BTreeMap::new(),
        }
    }
    fn write_asset(
        &mut self,
        output: &Path,
        document: &str,
        pixels: &[u8],
        w: usize,
        h: usize,
    ) -> CoreResult<String> {
        let count = self.asset_counts.entry(document.to_owned()).or_default();
        let stem = document
            .strip_suffix(".json")
            .ok_or_else(|| fail("invalid document path"))?;
        let relative = format!("{stem}_{count}.png");
        *count += 1;
        let content = png_rgba(w, h, pixels)?;
        let digest = write_if_changed(&output.join("port_data").join(&relative), &content)?;
        self.written.insert(relative.clone(), digest);
        Ok(relative)
    }
    #[allow(clippy::too_many_arguments)]
    fn convert_pictures(
        &mut self,
        output: &Path,
        key: &str,
        name: &str,
        pictures: Vec<Picture>,
        metadata: &Value,
        ordinal: usize,
        source_idx: usize,
        entry_start: usize,
    ) -> CoreResult<Vec<Value>> {
        let mut entries = Vec::new();
        for (index, picture) in pictures.into_iter().enumerate() {
            let logical_document = document_path(key)?;
            let document = if let Some((parent, offset)) = self.raster_origins.get(key) {
                document_path(&format!("{parent}#offset:{offset}"))?
            } else {
                logical_document.clone()
            };
            let mut levels = Vec::new();
            for level in &picture.levels {
                let rgba = if level.indexed {
                    colorize(&level.pixels, picture.palette.as_ref().unwrap())
                } else {
                    level.pixels.clone()
                };
                let asset =
                    self.write_asset(output, &document, &rgba, level.width, level.height)?;
                levels.push(relative_raster(&logical_document, &asset)?);
            }
            let mut entry = json!({"type":"texture","name":name,"header":ordinal,"picture":index,
                "width":picture.width,"height":picture.height,"bpp":32,"source_bpp":picture.source_bpp,
                "tex0":picture.tex0,"tex1":picture.tex1,"format":"rgba8",
                "variants":[{"palette_source":name,"levels":levels}]});
            for (k, v) in metadata.as_object().unwrap() {
                entry[k] = v.clone();
            }
            if picture.palette.is_some() {
                self.pictures.push(PictureRecord {
                    source_idx,
                    entry_idx: entry_start + entries.len(),
                    source: key.to_owned(),
                    name: name.to_owned(),
                    picture,
                    document,
                });
            }
            entries.push(entry);
        }
        Ok(entries)
    }
    fn transform(
        &mut self,
        output: &Path,
        original: &[u8],
        key: &str,
        depth: usize,
    ) -> CoreResult<Option<Vec<u8>>> {
        if depth > 8 {
            return Err(fail(format!("PACK nesting exceeds 8 at {key}")));
        }
        if original.starts_with(b"IMG\0")
            || original.starts_with(b"IM2\0")
            || original.starts_with(b"IM3\0")
        {
            let (kind, mut entries) = parse_img(original)?;
            if kind == "img3" {
                for a in 0..entries.len() {
                    for b in a + 1..entries.len() {
                        if entries[a].block > entries[b].block && !entries[b].name.starts_with('#')
                        {
                            entries.swap(a, b);
                        }
                    }
                }
            }
            let source_idx = self.sources.len();
            let mut converted = Vec::new();
            for (ordinal, e) in entries.iter().enumerate() {
                if e.name.starts_with('#') {
                    let script = parse_cfg(slice(original, e.offset, e.size)?)?;
                    collect_pairs(key, &script, &mut self.pairs);
                    converted.push(json!({"type":"cfg","name":e.name,"script":script}));
                } else {
                    let pictures = parse_tim2(&original[e.offset..], e.swizzled, e.no_image)?;
                    let metadata = json!({"block":e.block,"swizzled":e.swizzled,"clamp":e.clamp,"no_image":e.no_image});
                    converted.extend(self.convert_pictures(
                        output,
                        key,
                        &e.name,
                        pictures,
                        &metadata,
                        ordinal,
                        source_idx,
                        converted.len(),
                    )?);
                }
            }
            self.sources
                .push(json!({"source":key,"kind":kind,"entries":converted}));
            return Ok(None);
        }
        if original.starts_with(b"TIM2") {
            let pictures = parse_tim2(original, false, false)?;
            let stem = Path::new(key)
                .file_stem()
                .and_then(|x| x.to_str())
                .unwrap_or(key);
            let source_idx = self.sources.len();
            let metadata = json!({"block":0,"swizzled":false,"clamp":0,"no_image":false});
            let entries =
                self.convert_pictures(output, key, stem, pictures, &metadata, 0, source_idx, 0)?;
            self.sources
                .push(json!({"source":key,"kind":"tm2","entries":entries}));
            return Ok(None);
        }
        if let Some(members) = parse_pack(original) {
            let mut result = original.to_vec();
            let mut changed = false;
            let mut seen = BTreeSet::new();
            let origin = self.raster_origins.get(key).cloned().or_else(|| {
                members
                    .iter()
                    .any(|member| member.2 == 0)
                    .then(|| (key.to_owned(), 0))
            });
            for (ordinal, (name, start, size, size_field)) in members.into_iter().enumerate() {
                let child_key = if seen.insert(name.clone()) {
                    format!("{key}/{name}")
                } else {
                    format!("{key}/{name}#entry:{ordinal}")
                };
                if size == 0 {
                    continue;
                }
                if let Some((parent, offset)) = &origin {
                    self.raster_origins.insert(
                        child_key.clone(),
                        (
                            parent.clone(),
                            offset
                                .checked_add(start)
                                .ok_or_else(|| fail("raster origin overflow"))?,
                        ),
                    );
                }
                match self.transform(
                    output,
                    &original[start..start + size],
                    &child_key,
                    depth + 1,
                )? {
                    None => {
                        result[start..start + size].fill(0);
                        result[size_field..size_field + 4].fill(0);
                        changed = true;
                    }
                    Some(next) if next != original[start..start + size] => {
                        if next.len() != size {
                            return Err(fail("PACK member size changed"));
                        }
                        result[start..start + size].copy_from_slice(&next);
                        changed = true;
                    }
                    _ => {}
                }
            }
            return Ok(Some(if changed { result } else { original.to_vec() }));
        }
        let mut result = original.to_vec();
        let mut changed = false;
        let mut position = 0;
        while position + 4 <= original.len() {
            let found = (position..original.len() - 3).find(|&i| {
                let x = &original[i..i + 4];
                x == b"TIM2" || x == b"IMG\0" || x == b"IM2\0" || x == b"IM3\0"
            });
            let Some(at) = found else { break };
            let candidate = &original[at..];
            let size = if candidate.starts_with(b"TIM2") {
                tim2_extent(candidate)
            } else {
                img_extent(candidate)
            };
            if let Ok(size) = size
                && size > 0
            {
                let child_key = format!("{key}#offset:{at}");
                if let Some((parent, offset)) = self.raster_origins.get(key).cloned() {
                    self.raster_origins.insert(
                        child_key.clone(),
                        (
                            parent,
                            offset
                                .checked_add(at)
                                .ok_or_else(|| fail("raster origin overflow"))?,
                        ),
                    );
                }
                if self
                    .transform(output, &candidate[..size], &child_key, depth + 1)?
                    .is_none()
                {
                    result[at..at + size].fill(0);
                    changed = true;
                    position = at + size;
                    continue;
                }
            }
            position = at + 4;
        }
        Ok(Some(if changed { result } else { original.to_vec() }))
    }
}

#[derive(Clone, Debug)]
pub struct ConvertedTextureDocument {
    pub source_alias: String,
    pub document_path: String,
    pub document: Value,
}

pub struct ConversionReport {
    pub sources: u64,
    pub pngs: u64,
    pub documents: Vec<ConvertedTextureDocument>,
}
fn collect_files(root: &Path, directory: &Path, files: &mut Vec<PathBuf>) -> CoreResult<()> {
    for entry in fs::read_dir(directory)? {
        let path = entry?.path();
        if path.is_dir() {
            collect_files(root, &path, files)?;
        } else if path.is_file() && path.strip_prefix(root).unwrap() != Path::new("manifest.txt") {
            files.push(path);
        }
    }
    Ok(())
}
fn prune_unmanifested(root: &Path, allowed: &BTreeSet<String>) -> CoreResult<()> {
    fn visit(root: &Path, dir: &Path, allowed: &BTreeSet<String>) -> CoreResult<()> {
        for entry in fs::read_dir(dir)? {
            let path = entry?.path();
            if path.is_dir() {
                visit(root, &path, allowed)?;
                if fs::read_dir(&path)?.next().is_none() {
                    fs::remove_dir(&path)?;
                }
            } else if path.is_file() {
                let relative = path
                    .strip_prefix(root)
                    .map_err(|e| fail(e.to_string()))?
                    .to_string_lossy()
                    .replace('\\', "/");
                if !allowed.contains(&relative) {
                    fs::remove_file(path)?;
                }
            }
        }
        Ok(())
    }
    visit(root, root, allowed)
}
fn process_file(raw: &Path, output: &Path, path: &Path) -> CoreResult<Unit> {
    let mut unit = Unit::new();
    let relative = path.strip_prefix(raw).map_err(|e| fail(e.to_string()))?;
    let key = relative.to_string_lossy().replace('\\', "/");
    let texture_extension = path
        .extension()
        .and_then(|s| s.to_str())
        .is_some_and(|s| s.eq_ignore_ascii_case("img") || s.eq_ignore_ascii_case("tm2"));
    if texture_extension && fs::metadata(path)?.len() == 0 {
        unit.sources
            .push(json!({"source":key,"kind":"empty","entries":[]}));
        return Ok(unit);
    }
    let target = output.join("data").join(relative);
    if possible_texture(path, &key)? {
        let original = fs::read(path)?;
        let transformed = unit.transform(output, &original, &key, 0)?;
        let Some(bytes) = transformed else {
            return Ok(unit);
        };
        if texture_extension {
            return Err(fail(format!(
                "unrecognized standalone texture format: {key}"
            )));
        }
        write_if_changed(&target, &bytes)?;
        audit_payload(&bytes, &key)?;
    } else {
        fs::create_dir_all(target.parent().ok_or_else(|| fail("no data parent"))?)?;
        if !target.is_file() || file_hash(&target)? != file_hash(path)? {
            fs::copy(path, &target)?;
        }
    }
    unit.data_hashes.insert(key, file_hash(&target)?);
    Ok(unit)
}
fn bake_variants(state: &mut Unit, output: &Path, raw: &Path) -> CoreResult<()> {
    let mut by_source: BTreeMap<(String, String), Vec<usize>> = BTreeMap::new();
    let mut by_name: BTreeMap<String, Vec<usize>> = BTreeMap::new();
    for (i, record) in state.pictures.iter().enumerate() {
        by_source
            .entry((record.source.clone(), record.name.clone()))
            .or_default()
            .push(i);
        by_name.entry(record.name.clone()).or_default().push(i);
    }
    let resolve = |container: &str, name: &str| -> Vec<usize> {
        if let Some(local) = by_source.get(&(container.to_owned(), name.to_owned())) {
            return local.clone();
        }
        let global = by_name.get(name).cloned().unwrap_or_default();
        let sibling_root = container
            .rsplit_once('/')
            .map(|(parent, _)| parent)
            .filter(|parent| {
                let mut ancestor = *parent;
                loop {
                    if raw.join(ancestor).is_file() {
                        return true;
                    }
                    let Some((next, _)) = ancestor.rsplit_once('/') else {
                        return false;
                    };
                    ancestor = next;
                }
            });
        if let Some(parent) = sibling_root {
            let prefix = format!("{parent}/");
            let siblings: Vec<_> = global
                .iter()
                .copied()
                .filter(|&i| state.pictures[i].source.starts_with(&prefix))
                .collect();
            let sources: BTreeSet<_> = siblings
                .iter()
                .map(|&i| &state.pictures[i].source)
                .collect();
            if sources.len() == 1 {
                return siblings;
            }
        }
        let sources: BTreeSet<_> = global.iter().map(|&i| &state.pictures[i].source).collect();
        if sources.len() == 1 {
            global
        } else {
            Vec::new()
        }
    };
    let pairs: BTreeSet<_> = state.pairs.iter().cloned().collect();
    let mut origins: BTreeMap<(String, String), BTreeSet<String>> = BTreeMap::new();
    for (container, source, dest) in &pairs {
        origins
            .entry((container.clone(), source.clone()))
            .or_default()
            .insert(source.clone());
        origins
            .entry((container.clone(), dest.clone()))
            .or_default()
            .insert(dest.clone());
    }
    loop {
        let mut changed = false;
        for (container, source, dest) in &pairs {
            let inherited = origins
                .get(&(container.clone(), source.clone()))
                .cloned()
                .unwrap_or_default();
            let dest_origins = origins.get_mut(&(container.clone(), dest.clone())).unwrap();
            let before = dest_origins.len();
            dest_origins.extend(inherited);
            changed |= dest_origins.len() != before;
        }
        if !changed {
            break;
        }
    }
    let mut requests = Vec::new();
    for ((container, dest), palette_origins) in &origins {
        let dests = resolve(container, dest);
        for origin in palette_origins {
            if origin == dest {
                continue;
            }
            let sources = resolve(container, origin);
            if let Some(&source) = sources.first() {
                for &target in &dests {
                    requests.push((source, target, origin.clone()));
                }
            }
        }
    }
    for (source_idx, dest_idx, origin) in requests {
        let source_palette = state.pictures[source_idx]
            .picture
            .palette
            .as_ref()
            .unwrap()
            .clone();
        let dest = state.pictures[dest_idx].clone();
        let entry = &state.sources[dest.source_idx]["entries"][dest.entry_idx];
        if entry["variants"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v["palette_source"] == origin)
        {
            continue;
        }
        let mut levels = Vec::new();
        let logical_document = document_path(&dest.source)?;
        for level in &dest.picture.levels {
            let rgba = colorize(&level.pixels, &source_palette);
            let asset =
                state.write_asset(output, &dest.document, &rgba, level.width, level.height)?;
            levels.push(relative_raster(&logical_document, &asset)?);
        }
        state.sources[dest.source_idx]["entries"][dest.entry_idx]["variants"]
            .as_array_mut()
            .unwrap()
            .push(json!({"palette_source":origin,"levels":levels}));
    }
    Ok(())
}

fn runtime_document(source: &Value) -> CoreResult<Value> {
    let kind = source["kind"]
        .as_str()
        .ok_or_else(|| fail("missing source kind"))?;
    let flat = source["entries"]
        .as_array()
        .ok_or_else(|| fail("missing source entries"))?;
    let mut groups = BTreeSet::new();
    let mut entries: Vec<Value> = Vec::new();
    let mut last_header = None;
    for item in flat {
        if item["type"] == "cfg" {
            entries.push(json!({"name":item["name"],"script":item["script"]}));
            last_header = None;
            continue;
        }
        if item["type"] != "texture" {
            continue;
        }
        let header = item["header"]
            .as_u64()
            .ok_or_else(|| fail("missing texture header"))?;
        let name = item["name"]
            .as_str()
            .ok_or_else(|| fail("missing texture name"))?;
        let block = item["block"]
            .as_i64()
            .ok_or_else(|| fail("missing texture block"))?;
        if block < 0 {
            return Err(fail("negative texture block"));
        }
        groups.insert(block);
        if last_header != Some(header) {
            let mut entry = json!({"name":name,"pictures":[]});
            if block != 0 {
                entry["block"] = json!(block);
            }
            entries.push(entry);
            last_header = Some(header);
        }
        let variants = item["variants"]
            .as_array()
            .ok_or_else(|| fail("missing texture variants"))?;
        let default = variants
            .first()
            .ok_or_else(|| fail("missing default texture variant"))?;
        let levels = default["levels"]
            .as_array()
            .ok_or_else(|| fail("missing texture levels"))?;
        let mut picture = if levels.is_empty() {
            json!({"canvas":{"width":item["width"],"height":item["height"]}})
        } else {
            json!({"levels":levels})
        };
        let extra: Vec<Value> = variants
            .iter()
            .skip(1)
            .map(|variant| json!({"selector":variant["palette_source"],"levels":variant["levels"]}))
            .collect();
        if !extra.is_empty() {
            picture["variants"] = json!(extra);
        }
        let animation = match item["source_bpp"].as_u64() {
            Some(24 | 32) => None,
            Some(4 | 16) => Some("copy"),
            Some(8) => Some("copy_variant"),
            _ => return Err(fail("unsupported texture animation depth")),
        };
        if let Some(animation) = animation {
            picture["animation"] = json!(animation);
        }
        let clamp = item["clamp"]
            .as_u64()
            .ok_or_else(|| fail("missing texture clamp"))?;
        let mut sampler = serde_json::Map::new();
        if clamp & 3 != 0 {
            sampler.insert("wrap_u".into(), json!("clamp"));
        }
        if (clamp >> 2) & 3 != 0 {
            sampler.insert("wrap_v".into(), json!("clamp"));
        }
        if !sampler.is_empty() {
            picture["sampler"] = Value::Object(sampler);
        }
        entries.last_mut().unwrap()["pictures"]
            .as_array_mut()
            .unwrap()
            .push(picture);
    }
    if kind != "img3" {
        groups.clear();
        groups.insert(0);
    }
    Ok(json!({"schema_version":3,"groups":groups,"entries":entries}))
}
pub fn convert_tree(raw: &Path, output: &Path, jobs: usize) -> CoreResult<ConversionReport> {
    convert_tree_with_progress(raw, output, jobs, &|_, _| {})
}

/// Reports completed conversion files while retaining deterministic output ordering.
pub fn convert_tree_with_progress(
    raw: &Path,
    output: &Path,
    jobs: usize,
    progress: &(dyn Fn(usize, usize) + Sync),
) -> CoreResult<ConversionReport> {
    use std::sync::atomic::{AtomicUsize, Ordering};
    let completed = AtomicUsize::new(0);
    if jobs == 0 || jobs > 64 {
        return Err(fail("jobs must be between 1 and 64"));
    }
    fs::create_dir_all(output.join("data"))?;
    fs::create_dir_all(output.join("port_data"))?;
    let mut files = Vec::new();
    collect_files(raw, raw, &mut files)?;
    files.sort();
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(jobs)
        .build()
        .map_err(|e| fail(e.to_string()))?;
    let results: Vec<_> = pool.install(|| {
        files
            .par_iter()
            .map(|p| {
                let result = process_file(raw, output, p);
                if result.is_ok() {
                    progress(completed.fetch_add(1, Ordering::Relaxed) + 1, files.len());
                }
                result
            })
            .collect()
    });
    let mut state = Unit::new();
    let mut errors = Vec::new();
    for (path, result) in files.iter().zip(results) {
        match result {
            Ok(mut unit) => {
                let base = state.sources.len();
                for record in &mut unit.pictures {
                    record.source_idx += base;
                }
                state.sources.extend(unit.sources);
                state.pictures.extend(unit.pictures);
                state.pairs.extend(unit.pairs);
                state.written.extend(unit.written);
                state.data_hashes.extend(unit.data_hashes);
                for (document, count) in unit.asset_counts {
                    if state.asset_counts.insert(document.clone(), count).is_some() {
                        errors.push(format!("asset collision: {document}"));
                    }
                }
            }
            Err(error) => errors.push(format!("{}: {error}", path.display())),
        }
    }
    if !errors.is_empty() {
        return Err(fail(format!(
            "{} conversion workers failed: {}",
            errors.len(),
            errors
                .iter()
                .take(8)
                .cloned()
                .collect::<Vec<_>>()
                .join("; ")
        )));
    }
    bake_variants(&mut state, output, raw)?;
    let mut documents = BTreeSet::new();
    let mut aliases = BTreeSet::new();
    let mut converted_documents = Vec::with_capacity(state.sources.len());
    for source in &state.sources {
        let key = source["source"]
            .as_str()
            .ok_or_else(|| fail("texture source missing"))?;
        let path = document_path(key)?;
        if !aliases.insert(key.to_ascii_lowercase()) || !documents.insert(path.to_ascii_lowercase())
        {
            return Err(fail(format!("document collision: {path}")));
        }
        let document = runtime_document(source)?;
        let content = pretty_json(&document)?;
        state.written.insert(
            path.clone(),
            write_if_changed(&output.join("port_data").join(&path), &content)?,
        );
        converted_documents.push(ConvertedTextureDocument {
            source_alias: key.to_owned(),
            document_path: path,
            document,
        });
    }
    let pngs = state.written.keys().filter(|k| k.ends_with(".png")).count() as u64;
    let expected_port: BTreeSet<String> = state.written.keys().cloned().collect();
    let expected_data: BTreeSet<String> = state.data_hashes.keys().cloned().collect();
    prune_unmanifested(&output.join("port_data"), &expected_port)?;
    prune_unmanifested(&output.join("data"), &expected_data)?;
    Ok(ConversionReport {
        sources: converted_documents.len() as u64,
        pngs,
        documents: converted_documents,
    })
}

fn direct_relative(path: &str) -> CoreResult<Vec<String>> {
    if path.is_empty()
        || Path::new(path).is_absolute()
        || path.chars().any(|c| matches!(c, '\\' | ':' | '\0'))
    {
        return Err(fail(format!("invalid texture path: {path}")));
    }
    let parts: Vec<String> = path.split('/').map(str::to_owned).collect();
    if parts
        .iter()
        .any(|part| part.is_empty() || part == "." || part == "..")
    {
        return Err(fail(format!("invalid texture path component: {path}")));
    }
    Ok(parts)
}

fn asset_relative(document_path: &str, reference: &str) -> CoreResult<String> {
    if reference.is_empty()
        || Path::new(reference).is_absolute()
        || reference.chars().any(|c| matches!(c, '\\' | ':' | '\0'))
    {
        return Err(fail(format!("invalid texture asset path: {reference}")));
    }
    let mut parts = direct_relative(document_path)?;
    parts.pop();
    for part in reference.split('/') {
        match part {
            "" | "." => return Err(fail(format!("invalid texture asset path: {reference}"))),
            ".." => {
                if parts.pop().is_none() {
                    return Err(fail("texture asset escapes port_data"));
                }
            }
            _ => parts.push(part.to_owned()),
        }
    }
    let path = parts.join("/");
    if !path.to_ascii_lowercase().ends_with(".png") {
        return Err(fail(format!("texture asset is not PNG: {reference}")));
    }
    Ok(path)
}

fn picture_assets(document_path: &str, document: &Value) -> CoreResult<BTreeSet<String>> {
    let mut assets = BTreeSet::new();
    let entries = document["entries"]
        .as_array()
        .ok_or_else(|| fail("texture document entries missing"))?;
    for entry in entries {
        let Some(pictures) = entry.get("pictures") else {
            if entry.get("script").is_some() {
                continue;
            }
            return Err(fail("texture entry pictures missing"));
        };
        for picture in pictures
            .as_array()
            .ok_or_else(|| fail("invalid texture pictures"))?
        {
            if let Some(levels) = picture.get("levels") {
                for level in levels
                    .as_array()
                    .ok_or_else(|| fail("invalid texture levels"))?
                {
                    let reference = level
                        .as_str()
                        .ok_or_else(|| fail("invalid texture level path"))?;
                    assets.insert(asset_relative(document_path, reference)?);
                }
            }
            if let Some(variants) = picture.get("variants") {
                for variant in variants
                    .as_array()
                    .ok_or_else(|| fail("invalid texture variants"))?
                {
                    for level in variant["levels"]
                        .as_array()
                        .ok_or_else(|| fail("invalid texture variant levels"))?
                    {
                        let reference = level
                            .as_str()
                            .ok_or_else(|| fail("invalid texture variant path"))?;
                        assets.insert(asset_relative(document_path, reference)?);
                    }
                }
            }
        }
    }
    Ok(assets)
}

/// Removes texture assets embedded in converted models while preserving shared PNGs.
pub fn remove_embedded(
    published: &Path,
    documents: &[ConvertedTextureDocument],
    consumed: &BTreeSet<String>,
    referenced: &BTreeSet<String>,
) -> CoreResult<()> {
    let port = published.join("port_data");
    let canonical_port = fs::canonicalize(&port)?;
    let check_file = |relative: &str| -> CoreResult<(PathBuf, PathBuf)> {
        direct_relative(relative)?;
        let path = port.join(relative);
        let canonical = fs::canonicalize(&path)?;
        if !canonical.starts_with(&canonical_port) || !path.is_file() {
            return Err(fail(format!("texture path escapes port_data: {relative}")));
        }
        Ok((path, canonical))
    };
    let mut aliases = BTreeSet::new();
    let mut alias_keys = BTreeSet::new();
    let mut paths = BTreeSet::new();
    let mut consumed_files = Vec::new();
    let mut consumed_assets = BTreeMap::new();
    let mut retained_assets = BTreeSet::new();
    for item in documents {
        if !aliases.insert(item.source_alias.clone())
            || !alias_keys.insert(item.source_alias.to_ascii_lowercase())
            || !paths.insert(item.document_path.to_ascii_lowercase())
            || document_path(&item.source_alias)? != item.document_path
        {
            return Err(fail("invalid or duplicate texture inventory source"));
        }
        let (file, _) = check_file(&item.document_path)?;
        let saved: Value = serde_json::from_slice(&fs::read(&file)?)?;
        if saved != item.document {
            return Err(fail(format!(
                "texture document changed: {}",
                item.document_path
            )));
        }
        let assets = picture_assets(&item.document_path, &item.document)?;
        let mut checked_assets = Vec::with_capacity(assets.len());
        for asset in &assets {
            checked_assets.push(check_file(asset)?);
        }
        if consumed.contains(&item.source_alias) {
            consumed_files.push(file);
            consumed_assets.extend(
                checked_assets
                    .into_iter()
                    .map(|(path, canonical)| (canonical, path)),
            );
        } else {
            retained_assets.extend(checked_assets.into_iter().map(|(_, canonical)| canonical));
        }
    }
    if !consumed.is_subset(&aliases) {
        return Err(fail("unknown consumed texture source"));
    }
    // Model images reference standalone PNGs, so consumed assets they use remain published.
    for relative in referenced {
        retained_assets.insert(check_file(relative)?.1);
    }
    let mut removed = Vec::new();
    for file in consumed_files {
        fs::remove_file(&file)?;
        removed.push(file);
    }
    for (key, file) in consumed_assets {
        if !retained_assets.contains(&key) {
            fs::remove_file(&file)?;
            removed.push(file);
        }
    }
    for file in removed {
        let mut parent = file.parent();
        while let Some(dir) = parent {
            if dir == port {
                break;
            }
            match fs::remove_dir(dir) {
                Ok(()) => parent = dir.parent(),
                Err(error) if error.kind() == std::io::ErrorKind::DirectoryNotEmpty => break,
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => break,
                Err(error) => return Err(error.into()),
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn assert_inventory(output: &Path, report: &ConversionReport) {
        assert_eq!(report.sources as usize, report.documents.len());
        assert!(!output.join("port_data/catalog.json").exists());
        assert!(!output.join("port_data/manifest.json").exists());
        assert!(!output.join("data/manifest.json").exists());
        let mut paths = Vec::new();
        collect_files(
            &output.join("port_data"),
            &output.join("port_data"),
            &mut paths,
        )
        .unwrap();
        assert_eq!(
            paths
                .iter()
                .filter(|path| path.extension().is_some_and(|e| e == "png"))
                .count() as u64,
            report.pngs
        );
        for item in &report.documents {
            let saved: Value = serde_json::from_slice(
                &fs::read(output.join("port_data").join(&item.document_path)).unwrap(),
            )
            .unwrap();
            assert_eq!(saved, item.document);
            assert_eq!(saved["schema_version"], 3);
        }
    }
    #[test]
    fn embedded_cleanup_preserves_png_shared_with_unconsumed_document() {
        let temp = tempfile::tempdir().unwrap();
        let port = temp.path().join("port_data");
        fs::create_dir_all(port.join("a")).unwrap();
        fs::create_dir_all(port.join("b")).unwrap();
        let first = json!({"schema_version":3,"groups":[0],"entries":[
            {"name":"first","pictures":[{"levels":["a_0.png","../shared.png"]}]}
        ]});
        let second = json!({"schema_version":3,"groups":[0],"entries":[
            {"name":"second","pictures":[{"levels":["b_0.png","../shared.png"]}]}
        ]});
        fs::write(port.join("a/a.json"), pretty_json(&first).unwrap()).unwrap();
        fs::write(port.join("b/b.json"), pretty_json(&second).unwrap()).unwrap();
        fs::write(port.join("a/a_0.png"), b"first").unwrap();
        fs::write(port.join("b/b_0.png"), b"second").unwrap();
        fs::write(port.join("shared.png"), b"shared").unwrap();
        let documents = vec![
            ConvertedTextureDocument {
                source_alias: "a/a.img".into(),
                document_path: "a/a.json".into(),
                document: first,
            },
            ConvertedTextureDocument {
                source_alias: "b/b.img".into(),
                document_path: "b/b.json".into(),
                document: second,
            },
        ];
        remove_embedded(
            temp.path(),
            &documents,
            &BTreeSet::from(["a/a.img".into()]),
            &BTreeSet::new(),
        )
        .unwrap();
        assert!(!port.join("a").exists());
        assert_eq!(fs::read(port.join("shared.png")).unwrap(), b"shared");
        assert_eq!(fs::read(port.join("b/b_0.png")).unwrap(), b"second");
        assert!(port.join("b/b.json").is_file());
    }

    #[test]
    fn embedded_cleanup_keeps_png_referenced_by_model_images() {
        let temp = tempfile::tempdir().unwrap();
        let port = temp.path().join("port_data");
        fs::create_dir_all(port.join("a")).unwrap();
        let document = json!({"schema_version":3,"groups":[0],"entries":[
            {"name":"first","pictures":[{"levels":["a_0.png","a_1.png"]}]}
        ]});
        fs::write(port.join("a/a.json"), pretty_json(&document).unwrap()).unwrap();
        fs::write(port.join("a/a_0.png"), b"base").unwrap();
        fs::write(port.join("a/a_1.png"), b"mip").unwrap();
        let documents = vec![ConvertedTextureDocument {
            source_alias: "a/a.img".into(),
            document_path: "a/a.json".into(),
            document,
        }];
        remove_embedded(
            temp.path(),
            &documents,
            &BTreeSet::from(["a/a.img".into()]),
            &BTreeSet::from(["a/a_0.png".into()]),
        )
        .unwrap();
        assert!(!port.join("a/a.json").exists());
        assert!(!port.join("a/a_1.png").exists());
        assert_eq!(fs::read(port.join("a/a_0.png")).unwrap(), b"base");
    }

    #[test]
    fn embedded_cleanup_rejects_escape_before_deleting_anything() {
        let temp = tempfile::tempdir().unwrap();
        let port = temp.path().join("port_data");
        fs::create_dir_all(port.join("a")).unwrap();
        let document = json!({"schema_version":3,"groups":[0],"entries":[
            {"name":"first","pictures":[{"levels":["../../outside.png"]}]}
        ]});
        fs::write(port.join("a/a.json"), pretty_json(&document).unwrap()).unwrap();
        fs::write(temp.path().join("outside.png"), b"outside").unwrap();
        let documents = vec![ConvertedTextureDocument {
            source_alias: "a/a.img".into(),
            document_path: "a/a.json".into(),
            document,
        }];
        assert!(
            remove_embedded(
                temp.path(),
                &documents,
                &BTreeSet::from(["a/a.img".into()]),
                &BTreeSet::new()
            )
            .is_err()
        );
        assert!(port.join("a/a.json").is_file());
        assert_eq!(
            fs::read(temp.path().join("outside.png")).unwrap(),
            b"outside"
        );
    }
    fn picture_t8() -> Vec<u8> {
        let mut h = vec![0u8; 0x30];
        h[..4].copy_from_slice(&(0x30u32 + 4 + 1024).to_le_bytes());
        h[4..8].copy_from_slice(&1024u32.to_le_bytes());
        h[8..12].copy_from_slice(&4u32.to_le_bytes());
        h[12..14].copy_from_slice(&0x30u16.to_le_bytes());
        h[17] = 1;
        h[18] = 3;
        h[19] = 5;
        h[20..22].copy_from_slice(&2u16.to_le_bytes());
        h[22..24].copy_from_slice(&2u16.to_le_bytes());
        h.extend_from_slice(&[4, 5, 6, 7]);
        for i in 0..256 {
            h.extend_from_slice(&[i as u8, 0, 0, 64]);
        }
        h
    }
    #[test]
    fn indexed_tim2_bakes_rgba() {
        let mut b = vec![0u8; 16];
        b[..4].copy_from_slice(b"TIM2");
        b[6] = 1;
        b.extend_from_slice(&picture_t8());
        let pictures = parse_tim2(&b, false, false).unwrap();
        assert_eq!(pictures.len(), 1);
        assert_eq!(pictures[0].source_bpp, 8);
        let rgba = colorize(
            &pictures[0].levels[0].pixels,
            pictures[0].palette.as_ref().unwrap(),
        );
        assert_eq!(&rgba[..4], &[4, 0, 0, 128]);
    }
    #[test]
    fn runtime_document_preserves_groups_pictures_canvas_and_sampling() {
        let internal = json!({"kind":"img3","entries":[
            {"type":"cfg","name":"#cfg","script":[{"command":"TEX_ANIME_DATA_END","args":[]}]},
            {"type":"texture","header":1,"name":"first","block":2,"width":2,"height":2,
             "source_bpp":8,"clamp":5,"variants":[{"palette_source":"first","levels":["sample_0.png"]},
              {"palette_source":"donor","levels":["sample_1.png"]}]},
            {"type":"texture","header":1,"name":"first","block":2,"width":1,"height":1,
             "source_bpp":8,"clamp":0,"variants":[{"palette_source":"first","levels":["sample_2.png"]}]},
            {"type":"texture","header":2,"name":"blank","block":4,"width":16,"height":8,
             "source_bpp":32,"clamp":0,"variants":[{"palette_source":"blank","levels":[]}]}
        ]});
        let doc = runtime_document(&internal).unwrap();
        assert_eq!(doc["groups"], json!([2, 4]));
        assert_eq!(doc["entries"].as_array().unwrap().len(), 3);
        assert_eq!(
            doc["entries"][0],
            json!({"name":"#cfg","script":[{"command":"TEX_ANIME_DATA_END","args":[]}]})
        );
        assert_eq!(doc["entries"][1]["pictures"].as_array().unwrap().len(), 2);
        assert_eq!(
            doc["entries"][1]["pictures"][0]["sampler"],
            json!({"wrap_u":"clamp","wrap_v":"clamp"})
        );
        assert_eq!(
            doc["entries"][1]["pictures"][0]["variants"][0]["selector"],
            "donor"
        );
        assert_eq!(
            doc["entries"][2]["pictures"][0]["canvas"],
            json!({"width":16,"height":8})
        );
        assert!(doc["entries"][2]["pictures"][0].get("animation").is_none());
    }
    #[test]
    fn pack_member_bounds_and_cfg_bytes() {
        assert!(parse_pack(&super::super::native_scripts::ambiguous_script_fixture()).is_none());
        let mut b = vec![0u8; 152];
        b[..7].copy_from_slice(b"one.img");
        b[64..68].copy_from_slice(&152u32.to_le_bytes());
        b[68..72].copy_from_slice(&4u32.to_le_bytes());
        b[72..76].copy_from_slice(&76u32.to_le_bytes());
        b.extend_from_slice(b"TEST");
        assert_eq!(parse_pack(&b).unwrap()[0].0, "one.img");
        let script =
            parse_cfg(b"SRC_TEX \"\x82\xA0\",0;DEST_TEX \"dest\",0;TEX_ANIME_DATA_END;").unwrap();
        assert_eq!(script[0]["args"][0]["text"], "%82%A0");
    }
    #[test]
    fn named_images_after_empty_pack_members_keep_historical_png_paths() {
        fn pack(entries: &[(&str, &[u8])]) -> Vec<u8> {
            let mut output = Vec::new();
            for (name, payload) in entries {
                let mut header = vec![0; 80];
                header[..name.len()].copy_from_slice(name.as_bytes());
                for (at, value) in [
                    (64, 80),
                    (68, payload.len() as u32),
                    (72, 80 + payload.len() as u32),
                ] {
                    header[at..at + 4].copy_from_slice(&value.to_le_bytes());
                }
                output.extend(header);
                output.extend_from_slice(payload);
            }
            output.extend([0; 80]);
            output
        }
        let temporary = tempfile::tempdir().unwrap();
        let raw = temporary.path().join("raw");
        let output = temporary.path().join("out");
        fs::create_dir_all(&raw).unwrap();
        let mut tim2 = vec![0; 16];
        tim2[..4].copy_from_slice(b"TIM2");
        tim2[6] = 1;
        tim2.extend(picture_t8());
        let mut img = vec![0; 80];
        img[..4].copy_from_slice(b"IM3\0");
        img[8] = 1;
        img[16..21].copy_from_slice(b"icons");
        img[52..56].copy_from_slice(&80u32.to_le_bytes());
        img.extend(tim2);
        let nested = pack(&[("nested.img", &img)]);
        let original = pack(&[
            ("wmap.cfg", b""),
            ("frametex.img", &img),
            ("inner.pac", &nested),
            ("keep.bin", b"unchanged"),
            ("zero.bin", &[0; 4]),
        ]);
        let offsets: Vec<_> = original
            .windows(4)
            .enumerate()
            .filter_map(|(offset, bytes)| (bytes == b"IM3\0").then_some(offset))
            .collect();
        fs::write(raw.join("bundle.pac"), original).unwrap();
        let report = convert_tree(&raw, &output, 2).unwrap();
        assert_inventory(&output, &report);
        assert_eq!(report.documents[0].source_alias, "bundle.pac/frametex.img");
        assert_eq!(
            report.documents[0].document_path,
            "bundle.pac/frametex.json"
        );
        assert_eq!(
            report.documents[0].document["entries"][0]["pictures"][0]["levels"][0],
            format!("embedded-offset-{}_0.png", offsets[0])
        );
        assert_eq!(
            report.documents[1].document["entries"][0]["pictures"][0]["levels"][0],
            format!("../embedded-offset-{}_0.png", offsets[1])
        );
        for offset in offsets {
            assert!(
                output
                    .join(format!(
                        "port_data/bundle.pac/embedded-offset-{offset}_0.png"
                    ))
                    .is_file()
            );
        }
        super::super::configs::convert_tree(&raw, &output, 1).unwrap();
        let empty: Value = serde_json::from_slice(
            &fs::read(output.join("port_data/bundle.pac/wmap.cfg.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(empty["commands"], json!([]));
        super::super::packs::convert_tree(&raw, &output).unwrap();
        assert!(!output.join("port_data/bundle.pac/frametex.img").exists());
        assert!(
            !output
                .join("port_data/bundle.pac/inner.pac/nested.img")
                .exists()
        );
        assert_eq!(
            fs::read(output.join("port_data/bundle.pac/keep.bin")).unwrap(),
            b"unchanged"
        );
        assert_eq!(
            fs::read(output.join("port_data/bundle.pac/zero.bin")).unwrap(),
            [0; 4]
        );
        super::super::packs::verify(&output).unwrap();
    }
    #[test]
    fn malformed_tim2_is_rejected() {
        let mut b = vec![0u8; 16];
        b[..4].copy_from_slice(b"TIM2");
        b[6] = 1;
        b.extend_from_slice(&picture_t8());
        b.pop();
        assert!(parse_tim2(&b, false, false).is_err());
    }
    #[test]
    fn document_paths_handle_markers_and_case() {
        assert_eq!(document_path("img/FOO.IMG").unwrap(), "img/FOO.json");
        assert_eq!(
            document_path("model.chr#offset:80").unwrap(),
            "model.chr/embedded-offset-80.json"
        );
        assert_eq!(
            document_path("outer.img/inner.img#entry:2").unwrap(),
            "outer-img/inner-entry-2.json"
        );
        assert_eq!(
            document_path("outer.img#entry:1#offset:80").unwrap(),
            "outer-entry-1.json/embedded-offset-80.json"
        );
        assert_eq!(document_path("../bad.img").unwrap(), "%2E%2E/bad.json");
    }
    #[test]
    fn original_hex_encodes_every_byte_without_changing_case_or_width() {
        let bytes: Vec<u8> = (0..=u8::MAX).collect();
        let expected: String = bytes.iter().map(|byte| format!("{byte:02x}")).collect();
        assert_eq!(hex_digest(&bytes), expected);
    }
    #[test]
    fn compact_document_omits_source_bytes_and_preserves_png() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        fs::create_dir(&raw).unwrap();
        let mut tim2 = vec![0u8; 16];
        tim2[..4].copy_from_slice(b"TIM2");
        tim2[6] = 1;
        tim2.extend_from_slice(&picture_t8());
        tim2.extend_from_slice(b"\0\xffpadding\0");
        fs::write(raw.join("UPPER.TM2"), &tim2).unwrap();
        fs::write(raw.join("empty.IMG"), []).unwrap();
        let out = temp.path().join("out");
        let report = convert_tree(&raw, &out, 2).unwrap();
        assert_eq!(report.sources, 2);
        assert_inventory(&out, &report);
        let document: Value =
            serde_json::from_slice(&fs::read(out.join("port_data/UPPER.json")).unwrap()).unwrap();
        assert_eq!(
            document
                .as_object()
                .unwrap()
                .keys()
                .cloned()
                .collect::<BTreeSet<_>>(),
            ["entries", "groups", "schema_version"]
                .into_iter()
                .map(str::to_owned)
                .collect()
        );
        assert_eq!(document["groups"], json!([0]));
        assert_eq!(
            document["entries"][0]["pictures"][0]["levels"][0],
            "UPPER_0.png"
        );
        assert_eq!(
            document["entries"][0]["pictures"][0]["animation"],
            "copy_variant"
        );
        let empty: Value =
            serde_json::from_slice(&fs::read(out.join("port_data/empty.json")).unwrap()).unwrap();
        assert_eq!(empty, json!({"schema_version":3,"groups":[0],"entries":[]}));
        assert!(document.get("original").is_none());
    }
    #[test]
    fn colliding_json_paths_are_rejected() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        fs::create_dir(&raw).unwrap();
        let mut tim2 = vec![0u8; 16];
        tim2[..4].copy_from_slice(b"TIM2");
        tim2[6] = 1;
        tim2.extend_from_slice(&picture_t8());
        fs::write(raw.join("same.img"), &tim2).unwrap();
        fs::write(raw.join("same.tm2"), &tim2).unwrap();
        assert!(convert_tree(&raw, &temp.path().join("out"), 2).is_err());
    }
    #[test]
    fn serial_parallel_conversion_is_identical() {
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::current_dir()
            .unwrap()
            .join(format!(".kazarov-core-test-{}-{nonce}", std::process::id()));
        let raw = root.join("raw");
        fs::create_dir_all(&raw).unwrap();
        let mut tim2 = vec![0u8; 16];
        tim2[..4].copy_from_slice(b"TIM2");
        tim2[6] = 1;
        tim2.extend_from_slice(&picture_t8());
        let mut img = vec![0u8; 80];
        img[..4].copy_from_slice(b"IM3\0");
        img[8] = 1;
        img[16..21].copy_from_slice(b"icons");
        img[52..56].copy_from_slice(&80u32.to_le_bytes());
        img.extend_from_slice(&tim2);
        fs::write(raw.join("a.img"), &img).unwrap();
        fs::write(raw.join("b.tm2"), &tim2).unwrap();
        fs::write(raw.join("note.txt"), b"ordinary").unwrap();
        let serial = root.join("serial");
        let parallel = root.join("parallel");
        let serial_report = convert_tree(&raw, &serial, 1).unwrap();
        let parallel_report = convert_tree(&raw, &parallel, 4).unwrap();
        assert_inventory(&serial, &serial_report);
        assert_inventory(&parallel, &parallel_report);
        let mut paths = Vec::new();
        collect_files(&serial, &serial, &mut paths).unwrap();
        for path in paths {
            let relative = path.strip_prefix(&serial).unwrap();
            assert_eq!(
                fs::read(&path).unwrap(),
                fs::read(parallel.join(relative)).unwrap(),
                "{}",
                relative.display()
            );
        }
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn duplicate_pack_members_get_distinct_bundles() {
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::current_dir().unwrap().join(format!(
            ".kazarov-duplicate-test-{}-{nonce}",
            std::process::id()
        ));
        let raw = root.join("raw");
        fs::create_dir_all(&raw).unwrap();
        let mut tim2 = vec![0u8; 16];
        tim2[..4].copy_from_slice(b"TIM2");
        tim2[6] = 1;
        tim2.extend_from_slice(&picture_t8());
        let mut img = vec![0u8; 80];
        img[..4].copy_from_slice(b"IM3\0");
        img[8] = 1;
        img[16..21].copy_from_slice(b"icons");
        img[52..56].copy_from_slice(&80u32.to_le_bytes());
        img.extend_from_slice(&tim2);
        let mut pack = vec![0u8; 228];
        for i in 0..2 {
            let at = i * 76;
            pack[at..at + 8].copy_from_slice(b"same.img");
            let offset = (228 + i * img.len() - at) as u32;
            pack[at + 64..at + 68].copy_from_slice(&offset.to_le_bytes());
            pack[at + 68..at + 72].copy_from_slice(&(img.len() as u32).to_le_bytes());
            pack[at + 72..at + 76].copy_from_slice(&76u32.to_le_bytes());
            pack.extend_from_slice(&img);
        }
        fs::write(raw.join("bundle.pac"), pack).unwrap();
        let output = root.join("published");
        let report = convert_tree(&raw, &output, 2).unwrap();
        assert!(output.join("port_data/bundle.pac/same.json").is_file());
        assert!(
            output
                .join("port_data/bundle.pac/same-entry-1.json")
                .is_file()
        );
        assert_inventory(&output, &report);
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn nested_pack_palette_copy_prefers_nearest_sibling() {
        fn pack(entries: &[(&str, Vec<u8>)]) -> Vec<u8> {
            let mut out = vec![0u8; 76 * (entries.len() + 1)];
            for (i, (name, content)) in entries.iter().enumerate() {
                let at = i * 76;
                out[at..at + name.len()].copy_from_slice(name.as_bytes());
                let offset = (out.len() - at) as u32;
                out[at + 64..at + 68].copy_from_slice(&offset.to_le_bytes());
                out[at + 68..at + 72].copy_from_slice(&(content.len() as u32).to_le_bytes());
                out[at + 72..at + 76].copy_from_slice(&76u32.to_le_bytes());
                out.extend_from_slice(content);
            }
            out
        }
        fn img(name: &str, cfg: Option<&[u8]>) -> Vec<u8> {
            let count = if cfg.is_some() { 2 } else { 1 };
            let mut out = vec![0u8; 16 + 64 * count];
            out[..4].copy_from_slice(b"IM3\0");
            out[8] = count as u8;
            let entry = 16;
            out[entry..entry + name.len()].copy_from_slice(name.as_bytes());
            let offset = out.len() as u32;
            out[entry + 36..entry + 40].copy_from_slice(&offset.to_le_bytes());
            let mut tim2 = vec![0u8; 16];
            tim2[..4].copy_from_slice(b"TIM2");
            tim2[6] = 1;
            tim2.extend_from_slice(&picture_t8());
            out.extend_from_slice(&tim2);
            if let Some(cfg) = cfg {
                let entry = 16 + 64;
                out[entry..entry + 4].copy_from_slice(b"#cfg");
                let offset = out.len() as u32;
                out[entry + 36..entry + 40].copy_from_slice(&offset.to_le_bytes());
                out[entry + 52..entry + 56].copy_from_slice(&(cfg.len() as u32).to_le_bytes());
                out.extend_from_slice(cfg);
            }
            out
        }
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        fs::create_dir(&raw).unwrap();
        let cfg = b"SRC_TEX \"src\",0;DEST_TEX \"dst\",0;TEX_ANIME_DATA_END;";
        let inner = pack(&[
            ("source.img", img("src", None)),
            ("dest.img", img("dst", Some(cfg))),
        ]);
        fs::write(raw.join("outer.pac"), pack(&[("nested.ipk", inner)])).unwrap();
        fs::write(raw.join("other.img"), img("src", None)).unwrap();
        let out = temp.path().join("out");
        let report = convert_tree(&raw, &out, 2).unwrap();
        let dest = report
            .documents
            .iter()
            .find(|document| document.source_alias == "outer.pac/nested.ipk/dest.img")
            .unwrap();
        assert!(
            dest.document["entries"][0]["pictures"][0]["variants"]
                .as_array()
                .unwrap()
                .iter()
                .any(|v| v["selector"] == "src")
        );
        assert_inventory(&out, &report);
    }
    #[test]
    fn cfg_only_bundle_and_stale_png_are_handled() {
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::current_dir()
            .unwrap()
            .join(format!(".kazarov-cfg-test-{}-{nonce}", std::process::id()));
        let raw = root.join("raw");
        fs::create_dir_all(&raw).unwrap();
        let script = b"TEX_ANIME_DATA_END;";
        let mut img = vec![0u8; 80];
        img[..4].copy_from_slice(b"IM3\0");
        img[8] = 1;
        img[16..20].copy_from_slice(b"#cfg");
        img[52..56].copy_from_slice(&80u32.to_le_bytes());
        img[68..72].copy_from_slice(&(script.len() as u32).to_le_bytes());
        img.extend_from_slice(script);
        fs::write(raw.join("cfg.img"), img).unwrap();
        let output = root.join("published");
        let first = convert_tree(&raw, &output, 2).unwrap();
        assert_eq!(first.sources, 1);
        assert_eq!(first.pngs, 0);
        assert_inventory(&output, &first);
        let bundle = output.join("port_data/cfg.json");
        assert!(bundle.is_file());
        let source: Value = serde_json::from_slice(&fs::read(&bundle).unwrap()).unwrap();
        assert_eq!(
            source,
            json!({"schema_version":3,"groups":[],"entries":[{"name":"#cfg","script":[{"command":"TEX_ANIME_DATA_END","args":[]}]}]})
        );
        let stale = output.join("port_data/stale.png");
        fs::write(&stale, b"obsolete").unwrap();
        let stale_dir = output.join("port_data/obsolete.img");
        fs::create_dir(&stale_dir).unwrap();
        fs::write(stale_dir.join("catalog.json"), b"obsolete").unwrap();
        let second = convert_tree(&raw, &output, 2).unwrap();
        assert_eq!(second.pngs, 0);
        assert!(!stale.exists());
        assert!(!stale_dir.exists());
        assert_inventory(&output, &second);
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    #[ignore = "requires KAZAROV_RETAIL_RAW and KAZAROV_RETAIL_OUT"]
    fn retail_tree_conversion_audit() {
        let raw = PathBuf::from(std::env::var("KAZAROV_RETAIL_RAW").unwrap());
        let output = PathBuf::from(std::env::var("KAZAROV_RETAIL_OUT").unwrap());
        let report = convert_tree(&raw, &output, 4).unwrap();
        assert!(report.sources > 5000);
        assert!(report.pngs > 10000);
        assert_inventory(&output, &report);
    }
    #[test]
    #[ignore = "requires KAZAROV_RETAIL_OUT"]
    fn retail_stage_verify_only() {
        let output = PathBuf::from(std::env::var("KAZAROV_RETAIL_OUT").unwrap());
        assert!(super::super::publish::verify(&output).unwrap() > 10000);
    }
    #[test]
    #[ignore = "requires KAZAROV_PARITY_RAW and KAZAROV_PARITY_OUT"]
    fn parity_sample_conversion() {
        let raw = PathBuf::from(std::env::var("KAZAROV_PARITY_RAW").unwrap());
        let output = PathBuf::from(std::env::var("KAZAROV_PARITY_OUT").unwrap());
        let report = convert_tree(&raw, &output, 2).unwrap();
        assert!(report.sources > 0 && report.pngs > 0);
        assert_inventory(&output, &report);
    }
}
