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
fn asset_folder(key: &str) -> CoreResult<String> {
    let mut raw: Vec<String> = key
        .split('/')
        .filter(|p| !p.is_empty())
        .map(str::to_owned)
        .collect();
    if let Some(last) = raw.last_mut()
        && let Some((name, ordinal)) = last.split_once("#entry:")
    {
        let name = name.strip_suffix(".img").unwrap_or(name);
        *last = format!("{name}-entry-{ordinal}.img");
    }
    let mut parts: Vec<String> = raw.iter().map(|p| safe_component(p)).collect();
    if parts.is_empty() {
        return Err(fail("empty texture source key"));
    }
    let last = parts.last_mut().unwrap();
    if last.ends_with(".tm2") {
        last.push_str(".img");
    } else if !last.ends_with(".img") {
        parts.push("embedded.img".into());
    }
    let path = parts.join("/");
    if path.len() > 180 {
        let digest = Sha256::digest(path.as_bytes());
        let prefix = hex_digest(&digest)[..20].to_owned();
        parts.truncate(parts.len().min(2));
        parts.push(format!("long-path-{prefix}.img"));
        return Ok(parts.join("/"));
    }
    Ok(path)
}
fn hex_digest(digest: &[u8]) -> String {
    let mut out = String::with_capacity(digest.len() * 2);
    for b in digest {
        out.push_str(&format!("{b:02x}"));
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
    if b.len() < 76 {
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
        if offset < 76
            || size == 0
            || cursor.checked_add(offset)?.checked_add(size)? > b.len()
            || advance < 76
        {
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
    folder: String,
    label: String,
}
struct Unit {
    sources: Vec<Value>,
    pictures: Vec<PictureRecord>,
    pairs: Vec<(String, String, String)>,
    written: BTreeMap<String, String>,
    data_hashes: BTreeMap<String, String>,
}
impl Unit {
    fn new() -> Self {
        Self {
            sources: Vec::new(),
            pictures: Vec::new(),
            pairs: Vec::new(),
            written: BTreeMap::new(),
            data_hashes: BTreeMap::new(),
        }
    }
    fn write_asset(
        &mut self,
        output: &Path,
        folder: &str,
        label: &str,
        pixels: &[u8],
        w: usize,
        h: usize,
    ) -> CoreResult<String> {
        let relative = format!("{folder}/{label}.png");
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
            let folder = asset_folder(key)?;
            let label = format!("{}-entry-{ordinal}-picture-{index}", safe_component(name));
            let mut levels = Vec::new();
            for (mip, level) in picture.levels.iter().enumerate() {
                let rgba = if level.indexed {
                    colorize(&level.pixels, picture.palette.as_ref().unwrap())
                } else {
                    level.pixels.clone()
                };
                levels.push(self.write_asset(
                    output,
                    &folder,
                    &format!("{label}-variant-self-mip-{mip}"),
                    &rgba,
                    level.width,
                    level.height,
                )?);
            }
            let mut entry = json!({"type":"texture","name":name,"picture":index,
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
                    folder,
                    label,
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
                .push(json!({"source":key,"kind":kind,"size":original.len(),"entries":converted}));
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
                .push(json!({"source":key,"kind":"tm2","size":original.len(),"entries":entries}));
            return Ok(None);
        }
        if let Some(members) = parse_pack(original) {
            let mut result = original.to_vec();
            let mut changed = false;
            let mut seen = BTreeSet::new();
            for (ordinal, (name, start, size, size_field)) in members.into_iter().enumerate() {
                let child_key = if seen.insert(name.clone()) {
                    format!("{key}/{name}")
                } else {
                    format!("{key}/{name}#entry:{ordinal}")
                };
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
                && self
                    .transform(
                        output,
                        &candidate[..size],
                        &format!("{key}#offset:{at}"),
                        depth + 1,
                    )?
                    .is_none()
            {
                result[at..at + size].fill(0);
                changed = true;
                position = at + size;
                continue;
            }
            position = at + 4;
        }
        Ok(Some(if changed { result } else { original.to_vec() }))
    }
}

pub struct ConversionReport {
    pub sources: u64,
    pub pngs: u64,
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
    if matches!(
        path.extension().and_then(|s| s.to_str()),
        Some("img" | "tm2")
    ) && fs::metadata(path)?.len() == 0
    {
        return Ok(unit);
    }
    let target = output.join("data").join(relative);
    if possible_texture(path, &key)? {
        let original = fs::read(path)?;
        let transformed = unit.transform(output, &original, &key, 0)?;
        let Some(bytes) = transformed else {
            return Ok(unit);
        };
        if matches!(
            path.extension().and_then(|s| s.to_str()),
            Some("img" | "tm2")
        ) {
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
        for (mip, level) in dest.picture.levels.iter().enumerate() {
            let rgba = colorize(&level.pixels, &source_palette);
            levels.push(state.write_asset(
                output,
                &dest.folder,
                &format!(
                    "{}-variant-{}-mip-{mip}",
                    dest.label,
                    safe_component(&origin)
                ),
                &rgba,
                level.width,
                level.height,
            )?);
        }
        state.sources[dest.source_idx]["entries"][dest.entry_idx]["variants"]
            .as_array_mut()
            .unwrap()
            .push(json!({"palette_source":origin,"levels":levels}));
    }
    Ok(())
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
    let mut folders = BTreeSet::new();
    for source in &state.sources {
        let key = source["source"]
            .as_str()
            .ok_or_else(|| fail("catalog source missing"))?;
        let folder = asset_folder(key)?;
        if !folders.insert(folder.clone()) {
            return Err(fail(format!("bundle collision: {folder}")));
        }
        let mut local = source.clone();
        for entry in local["entries"].as_array_mut().unwrap() {
            if entry["type"] == "texture" {
                for variant in entry["variants"].as_array_mut().unwrap() {
                    for level in variant["levels"].as_array_mut().unwrap() {
                        let path = level.as_str().unwrap();
                        *level = json!(
                            Path::new(path)
                                .file_name()
                                .unwrap()
                                .to_string_lossy()
                                .to_string()
                        );
                    }
                }
            }
        }
        local["schema_version"] = json!(1);
        let relative = format!("{folder}/catalog.json");
        let content = serde_json::to_vec(&local)?;
        state.written.insert(
            relative.clone(),
            write_if_changed(&output.join("port_data").join(relative), &content)?,
        );
    }
    let data_manifest = json!({"schema_version":1,"files":state.data_hashes});
    let data_manifest_bytes = serde_json::to_vec(&data_manifest)?;
    state.data_hashes.insert(
        "manifest.json".into(),
        write_if_changed(&output.join("data/manifest.json"), &data_manifest_bytes)?,
    );
    let catalog = json!({"schema_version":1,"sources":state.sources});
    state.written.insert(
        "catalog.json".into(),
        write_if_changed(
            &output.join("port_data/catalog.json"),
            &serde_json::to_vec(&catalog)?,
        )?,
    );
    let pngs = state.written.keys().filter(|k| k.ends_with(".png")).count() as u64;
    let manifest = json!({"schema_version":1,"files":state.written,"data_files":state.data_hashes});
    write_if_changed(
        &output.join("port_data/manifest.json"),
        &serde_json::to_vec(&manifest)?,
    )?;
    let mut expected_port: BTreeSet<String> = manifest["files"]
        .as_object()
        .unwrap()
        .keys()
        .cloned()
        .collect();
    expected_port.insert("manifest.json".into());
    let expected_data: BTreeSet<String> = manifest["data_files"]
        .as_object()
        .unwrap()
        .keys()
        .cloned()
        .collect();
    prune_unmanifested(&output.join("port_data"), &expected_port)?;
    prune_unmanifested(&output.join("data"), &expected_data)?;
    write_if_changed(
        &output.join("data.json"),
        &serde_json::to_vec(&json!({"schema_version":1,"data_version":super::data_version()}))?,
    )?;
    Ok(ConversionReport {
        sources: catalog["sources"].as_array().unwrap().len() as u64,
        pngs,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
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
    fn pack_member_bounds_and_cfg_bytes() {
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
    fn malformed_tim2_is_rejected() {
        let mut b = vec![0u8; 16];
        b[..4].copy_from_slice(b"TIM2");
        b[6] = 1;
        b.extend_from_slice(&picture_t8());
        b.pop();
        assert!(parse_tim2(&b, false, false).is_err());
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
        convert_tree(&raw, &serial, 1).unwrap();
        convert_tree(&raw, &parallel, 4).unwrap();
        super::super::publish::verify(&serial).unwrap();
        super::super::publish::verify(&parallel).unwrap();
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
        convert_tree(&raw, &output, 2).unwrap();
        assert!(
            output
                .join("port_data/bundle.pac/same.img/catalog.json")
                .is_file()
        );
        assert!(
            output
                .join("port_data/bundle.pac/same-entry-1.img/catalog.json")
                .is_file()
        );
        super::super::publish::verify(&output).unwrap();
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
        convert_tree(&raw, &out, 2).unwrap();
        let catalog: Value =
            serde_json::from_slice(&fs::read(out.join("port_data/catalog.json")).unwrap()).unwrap();
        let dest = catalog["sources"]
            .as_array()
            .unwrap()
            .iter()
            .find(|s| s["source"] == "outer.pac/nested.ipk/dest.img")
            .unwrap();
        assert!(
            dest["entries"][0]["variants"]
                .as_array()
                .unwrap()
                .iter()
                .any(|v| v["palette_source"] == "src")
        );
        super::super::publish::verify(&out).unwrap();
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
        let bundle = output.join("port_data/cfg.img");
        assert!(bundle.join("catalog.json").is_file());
        let source: Value =
            serde_json::from_slice(&fs::read(bundle.join("catalog.json")).unwrap()).unwrap();
        assert_eq!(source["entries"][0]["type"], "cfg");
        fs::write(bundle.join("stale.png"), b"obsolete").unwrap();
        let second = convert_tree(&raw, &output, 2).unwrap();
        assert_eq!(second.pngs, 0);
        assert!(!bundle.join("stale.png").exists());
        super::super::publish::verify(&output).unwrap();
        let run = root.join("run");
        super::super::publish::publish(&output, &run).unwrap();
        convert_tree(&raw, &output, 2).unwrap();
        let refreshed = json!({"schema_version":1,"data_version":super::super::data_version(),"source_sha256":"new-source"});
        fs::write(
            output.join("data.json"),
            serde_json::to_vec(&refreshed).unwrap(),
        )
        .unwrap();
        super::super::publish::publish(&output, &run).unwrap();
        let marker: Value =
            serde_json::from_slice(&fs::read(run.join("data.json")).unwrap()).unwrap();
        assert_eq!(marker, refreshed);
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
        super::super::publish::verify(&output).unwrap();
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
        super::super::publish::verify(&output).unwrap();
    }
}
