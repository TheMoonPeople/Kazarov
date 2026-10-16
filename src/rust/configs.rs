//! Publishes tagged configuration scripts as typed, directly executable JSON documents.
use super::{CoreError, CoreResult};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}
fn slice(b: &[u8], at: usize, n: usize) -> CoreResult<&[u8]> {
    b.get(
        at..at
            .checked_add(n)
            .ok_or_else(|| fail("CFG range overflow"))?,
    )
    .ok_or_else(|| fail(format!("CFG range {at}+{n} exceeds {}", b.len())))
}
fn word(b: &[u8], at: usize) -> CoreResult<u32> {
    Ok(u32::from_le_bytes(slice(b, at, 4)?.try_into().unwrap()))
}
fn short(b: &[u8], at: usize) -> CoreResult<i16> {
    Ok(i16::from_le_bytes(slice(b, at, 2)?.try_into().unwrap()))
}
fn whitespace(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\r' | b'\n')
}
fn hex(b: &[u8]) -> String {
    b.iter().map(|v| format!("{v:02x}")).collect()
}

fn decode_text(b: &[u8]) -> (String, bool) {
    let (text, invalid) = encoding_rs::SHIFT_JIS.decode_without_bom_handling(b);
    let (roundtrip, _, encode_error) = encoding_rs::SHIFT_JIS.encode(&text);
    let preserve_bytes = invalid || encode_error || roundtrip.as_ref() != b;
    (text.into_owned(), preserve_bytes)
}
fn string_argument(b: &[u8]) -> Value {
    let (text, invalid) = decode_text(b);
    let mut argument = json!({"type":"string","value":text});
    if invalid {
        argument["game_bytes"] = json!(hex(b))
    }
    argument
}

fn preprocess(raw: &[u8]) -> Vec<u8> {
    let mut b = raw.to_vec();
    let mut at = 0;
    while at < b.len() {
        if b.get(at..at + 2) == Some(b"//") {
            while at < b.len() && !matches!(b[at], b'\n' | b'\r') {
                b[at] = b' ';
                at += 1;
            }
        }
        if b.get(at..at + 2) == Some(b"/*") {
            while at < b.len() {
                if b.get(at..at + 2) == Some(b"*/") {
                    b[at] = b' ';
                    b[at + 1] = b' ';
                    break;
                }
                b[at] = b' ';
                at += 1;
            }
        } else {
            at += 1
        }
    }
    b
}
fn integer_prefix(token: &[u8]) -> i32 {
    let (negative, at) = if token.first() == Some(&b'-') {
        (true, 1)
    } else {
        (false, 0)
    };
    let mut value = 0u32;
    for &b in &token[at..] {
        if !b.is_ascii_digit() {
            break;
        }
        value = value.wrapping_mul(10).wrapping_add((b - b'0') as u32)
    }
    if negative {
        value.wrapping_neg() as i32
    } else {
        value as i32
    }
}
fn float_prefix(token: &[u8]) -> f32 {
    let mut end = usize::from(token.first() == Some(&b'-'));
    let mut dot = false;
    while end < token.len() {
        let b = token[end];
        if b.is_ascii_digit() {
            end += 1;
        } else if b == b'.' && !dot {
            dot = true;
            end += 1;
        } else {
            break;
        }
    }
    std::str::from_utf8(&token[..end])
        .ok()
        .and_then(|s| s.parse::<f32>().ok())
        .unwrap_or(0.)
}
fn text_argument(mut token: Vec<u8>) -> CoreResult<Value> {
    let mut quotes = usize::from(token.first() == Some(&b'"'));
    if token.last() == Some(&b'"') {
        quotes += 1;
        token.pop();
    }
    if quotes == 2 {
        return Ok(string_argument(&token[1..]));
    }
    let mut float = false;
    let mut non_numeric = false;
    if quotes == 0 {
        for i in 0..token.len() {
            let b = token[i];
            if b == b'.' {
                float = true
            }
            if whitespace(b) {
                token.truncate(i);
                break;
            }
            if !b.is_ascii_digit() && b != b'-' && b != b'.' {
                non_numeric = true
            }
        }
    }
    if token.is_empty() || non_numeric {
        return Ok(json!({"type":"invalid","value":null}));
    }
    if float {
        let value = float_prefix(&token);
        if !value.is_finite() {
            return Err(fail("nonfinite CFG argument"));
        }
        Ok(json!({"type":"float","value":value}))
    } else {
        Ok(json!({"type":"int","value":integer_prefix(&token)}))
    }
}
fn parse_text(raw: &[u8]) -> CoreResult<Vec<Value>> {
    let b = preprocess(raw);
    let mut at = 0;
    let mut commands = Vec::new();
    while at < b.len() {
        while at < b.len() && (whitespace(b[at]) || b[at] == 0) {
            at += 1
        }
        if at == b.len() {
            break;
        }
        let start = at;
        while at < b.len() && !whitespace(b[at]) && b[at] != b';' && b[at] != 0 {
            at += 1
        }
        if start == at {
            at += 1;
            continue;
        }
        let command = decode_text(&b[start..at]).0;
        if at < b.len() && whitespace(b[at]) {
            at += 1
        }
        let mut args = Vec::new();
        let mut more = true;
        while more && at < b.len() {
            while at < b.len() && whitespace(b[at]) {
                at += 1
            }
            if at == b.len() || b[at] == 0 {
                break;
            }
            let mut token = Vec::new();
            let mut quoted = false;
            let mut terminated = false;
            while at < b.len() {
                let mut byte = b[at];
                at += 1;
                if byte == 0 {
                    at = b.len();
                    break;
                }
                if byte == b'"' {
                    quoted = !quoted
                }
                if quoted {
                    if byte & 0x80 != 0 && !(0xa1..=0xdf).contains(&byte) {
                        token.push(byte);
                        if at == b.len() {
                            break;
                        }
                        byte = b[at];
                        at += 1;
                    } else if byte == b'\\' && b.get(at) == Some(&b'"') {
                        byte = b[at];
                        at += 1;
                    }
                }
                if !quoted && matches!(byte, b',' | b';') {
                    more = byte == b',';
                    terminated = true;
                    break;
                }
                token.push(byte);
            }
            if !terminated {
                break;
            }
            if token.is_empty() && !more {
                break;
            }
            args.push(text_argument(token)?);
        }
        commands.push(json!({"command":command,"args":args}));
    }
    Ok(commands)
}
fn parse_binary(b: &[u8]) -> CoreResult<Vec<Value>> {
    let mut at = 4;
    let mut commands = Vec::new();
    while at < b.len() {
        let tag = short(b, at)?;
        at += 2;
        if tag < 0 {
            break;
        }
        let count = short(b, at)?;
        at += 2;
        if count < 0 || count > 256 {
            return Err(fail("invalid binary CFG argument count"));
        }
        let count = count as usize;
        let types = slice(b, at, count)?.to_vec();
        at += count;
        let mut args = Vec::new();
        if count > 0 {
            at = at.next_multiple_of(4);
            for kind in types {
                match kind {
                    1 => {
                        args.push(json!({"type":"int","value":word(b,at)? as i32}));
                        at += 4;
                    }
                    2 => {
                        let value = f32::from_bits(word(b, at)?);
                        if !value.is_finite() {
                            return Err(fail("nonfinite binary CFG float"));
                        }
                        args.push(json!({"type":"float","value":value}));
                        at += 4;
                    }
                    3 => {
                        let end = b
                            .get(at..)
                            .ok_or_else(|| fail("binary CFG string outside file"))?
                            .iter()
                            .position(|&v| v == 0)
                            .ok_or_else(|| fail("unterminated binary CFG string"))?;
                        args.push(string_argument(slice(b, at, end)?));
                        at += (end + 1).next_multiple_of(4);
                    }
                    _ => return Err(fail("unknown binary CFG argument type")),
                }
            }
        }
        commands.push(json!({"tag_index":tag,"args":args}));
    }
    Ok(commands)
}
fn double_prefix(raw: &[u8]) -> CoreResult<f64> {
    let b = raw.trim_ascii_start();
    let mut at = usize::from(b.first().is_some_and(|c| matches!(*c, b'+' | b'-')));
    let mut digits = 0;
    while b.get(at).is_some_and(|v| v.is_ascii_digit()) {
        at += 1;
        digits += 1
    }
    if b.get(at) == Some(&b'.') {
        at += 1;
        while b.get(at).is_some_and(|v| v.is_ascii_digit()) {
            at += 1;
            digits += 1
        }
    }
    if digits == 0 {
        return Ok(0.);
    }
    if b.get(at).is_some_and(|v| matches!(*v, b'e' | b'E')) {
        let exponent = at;
        at += 1;
        if b.get(at).is_some_and(|v| matches!(*v, b'+' | b'-')) {
            at += 1
        }
        let start = at;
        while b.get(at).is_some_and(|v| v.is_ascii_digit()) {
            at += 1
        }
        if start == at {
            at = exponent
        }
    }
    let value = std::str::from_utf8(&b[..at])
        .map_err(|_| fail("invalid subtitle time"))?
        .parse::<f64>()
        .map_err(|_| fail("invalid subtitle time"))?;
    if !value.is_finite() {
        return Err(fail("nonfinite subtitle time"));
    }
    Ok(value)
}
fn subtitle_text(line: &[u8]) -> Value {
    let mut text = Vec::new();
    let mut at = 0;
    while at < line.len() {
        if line.get(at..at + 2) == Some(b"\\n") {
            text.push(b'\n');
            at += 2;
        } else {
            text.push(line[at]);
            at += 1;
        }
    }
    let mut value = string_argument(&text);
    value.as_object_mut().unwrap().remove("type");
    let content = value.as_object_mut().unwrap().remove("value").unwrap();
    value["text"] = content;
    value
}
fn parse_subtitles(raw: &[u8]) -> CoreResult<Value> {
    let mut chapters = Vec::new();
    let mut id = None;
    let mut entries = Vec::new();
    let mut start = 0.;
    let mut clear = 0.;
    let mut pending = false;
    let mut ended = false;
    fn finish(
        chapters: &mut Vec<Value>,
        id: &mut Option<i32>,
        entries: &mut Vec<Value>,
        pending: bool,
        start: f64,
        clear: f64,
    ) {
        if let Some(id) = id.take() {
            if pending {
                entries.push(json!({"start_seconds":start,"clear_seconds":clear,"text":""}));
            }
            chapters.push(json!({"id":id,"entries":std::mem::take(entries)}));
        }
    }
    for line in raw.split_inclusive(|&b| b == b'\n') {
        let content = line.strip_suffix(b"\n").unwrap_or(line);
        let trimmed = content.trim_ascii_start();
        if let Some(header) = trimmed.strip_prefix(b"@") {
            finish(&mut chapters, &mut id, &mut entries, pending, start, clear);
            id = Some(integer_prefix(header));
            start = 0.;
            clear = 0.;
            pending = false;
            ended = false;
        } else if !ended && id.is_some() {
            if let Some(value) = trimmed.strip_prefix(b"_STA ") {
                start = double_prefix(value)?;
                pending = true;
            } else if let Some(value) = trimmed.strip_prefix(b"_CLR ") {
                clear = double_prefix(value)?;
                pending = true;
            } else if let Some(value) = trimmed.strip_prefix(b"_STR ") {
                if !line.ends_with(b"\n") {
                    return Err(fail("unterminated subtitle text line"));
                }
                let mut entry = subtitle_text(value);
                entry["start_seconds"] = json!(start);
                entry["clear_seconds"] = json!(clear);
                entries.push(entry);
                start = 0.;
                clear = 0.;
                pending = false;
                if entries.len() > 20 {
                    return Err(fail("subtitle exceeds native20slots"));
                }
            } else if trimmed.starts_with(b"_END") {
                ended = true;
            }
        }
    }
    finish(&mut chapters, &mut id, &mut entries, pending, start, clear);
    Ok(json!({"schema_version":1,"kind":"movie_subtitles","chapters":chapters}))
}
pub(crate) fn parse(raw: &[u8]) -> CoreResult<Value> {
    if raw.trim_ascii_start().starts_with(b"@") {
        if !raw
            .windows(5)
            .any(|v| matches!(v, b"_STA " | b"_CLR " | b"_STR "))
        {
            return Err(fail("unrecognized @ CFG document format"));
        }
        return parse_subtitles(raw);
    }
    Ok(
        json!({"schema_version":1,"commands":if raw.starts_with(b"BIN"){parse_binary(raw)?}else{parse_text(raw)?}}),
    )
}

#[derive(Clone)]
struct Member {
    name: String,
    start: usize,
    size: usize,
    size_field: usize,
}
fn pack(b: &[u8]) -> Option<Vec<Member>> {
    if super::native_scripts::is_valid_script(b) {
        return None;
    }
    let mut at = 0usize;
    let mut seen = BTreeSet::new();
    let mut out = Vec::new();
    while seen.insert(at) && out.len() < 16384 {
        let h = b.get(at..at + 76)?;
        let end = h[..64].iter().position(|&v| v == 0)?;
        if end == 0 {
            return if out.is_empty() { None } else { Some(out) };
        }
        if h[..end]
            .iter()
            .any(|&v| v < 32 || matches!(v, b'/' | b'\\'))
        {
            return None;
        }
        let name = std::str::from_utf8(&h[..end]).ok()?.to_owned();
        let offset = word(h, 64).ok()? as usize;
        let size = word(h, 68).ok()? as usize;
        let next = word(h, 72).ok()? as usize;
        if offset < 76 || next < 76 || at.checked_add(offset)?.checked_add(size)? > b.len() {
            return None;
        }
        out.push(Member {
            name,
            start: at + offset,
            size,
            size_field: at + 68,
        });
        at = at.checked_add(next)?;
    }
    None
}
fn is_cfg(alias: &str) -> bool {
    let name = alias
        .split("#entry:")
        .next()
        .unwrap_or(alias)
        .to_ascii_lowercase();
    name.ends_with(".cfg") || name.ends_with(".map")
}
pub(crate) fn native_path(alias: &str, extension: &str) -> String {
    let extension = if extension == "gltf"
        && matches!(alias, "etc/gsys.pak/a_mu.mds" | "etc/gsys.pak/sukkopu.mds")
    {
        "model.gltf"
    } else if extension == "json"
        && alias
            .rsplit('/')
            .next()
            .unwrap_or(alias)
            .split('#')
            .next()
            .unwrap_or("")
            .to_ascii_lowercase()
            .ends_with(".map")
    {
        "map.json"
    } else if extension == "json"
        && alias
            .rsplit('/')
            .next()
            .unwrap_or(alias)
            .split('#')
            .next()
            .unwrap_or("")
            .to_ascii_lowercase()
            .ends_with(".cfg")
    {
        "cfg.json"
    } else {
        extension
    };
    let mut parts: Vec<String> = alias.split('/').map(str::to_owned).collect();
    let last = parts.len().saturating_sub(1);
    for (i, part) in parts.iter_mut().enumerate() {
        let (base, marker) = part
            .split_once('#')
            .map_or((part.as_str(), None), |(base, marker)| (base, Some(marker)));
        let marker = marker.map(|m| m.replace(':', "_"));
        if i == last {
            let stem = if marker.as_ref().is_some_and(|m| m.starts_with("offset_")) {
                base
            } else {
                base.rsplit_once('.').map_or(base, |p| p.0)
            };
            *part = format!(
                "{stem}{}{}.{extension}",
                if base.to_ascii_lowercase().ends_with(".chr_") {
                    "_"
                } else {
                    ""
                },
                marker.map_or(String::new(), |m| format!(".{m}"))
            );
        } else if base
            .to_ascii_lowercase()
            .trim_end_matches('_')
            .ends_with(".chr")
        {
            let character = base.trim_end_matches('_');
            *part = format!(
                "{}{}{}.assets",
                &character[..character.len() - 4],
                if base.ends_with('_') { "_" } else { "" },
                marker.map_or(String::new(), |m| format!(".{m}"))
            );
        } else if let Some(marker) = marker {
            *part = format!("{base}.{marker}");
        }
    }
    parts.join("/")
}
fn output_path(alias: &str) -> String {
    native_path(alias, "json")
}
fn inventory(
    raw: &[u8],
    alias: &str,
    identities: &mut BTreeSet<String>,
    depth: usize,
) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("resource PACK nesting exceeds 16"));
    }
    identities.insert(alias.replace('\\', "/").to_ascii_lowercase());
    if let Some(members) = pack(raw) {
        for member in members {
            if member.size > 0 {
                inventory(
                    slice(raw, member.start, member.size)?,
                    &format!("{alias}/{}", member.name),
                    identities,
                    depth + 1,
                )?;
            }
        }
    }
    Ok(())
}
fn migrate_references(document: &mut Value, source: &str, identities: &BTreeSet<String>) {
    let Some(commands) = document["commands"].as_array_mut() else {
        return;
    };
    for row in commands {
        let command = row["command"]
            .as_str()
            .unwrap_or_else(|| {
                if source.to_ascii_lowercase().ends_with(".map") && row["tag_index"] == 43 {
                    "PIECE"
                } else {
                    ""
                }
            })
            .to_owned();
        let Some(args) = row["args"].as_array_mut() else {
            continue;
        };
        for (index, arg) in args.iter_mut().enumerate() {
            let resource = match command.as_str() {
                "CHARA" => index == 0,
                "MDS" | "MODEL" | "SHADOW_MODEL" | "SKY_BG" | "WATER_PARTS_NAME" | "PIECE" => {
                    index == 0
                }
                "IMG" if source.to_ascii_lowercase().ends_with(".map") => index == 0,
                "IMG" | "MOVIE" | "SKYB_MDS" | "SKY_IMG" | "SKY_MDS" | "SUN_MDS"
                | "MASK_PARTS_NAME" => index == 1,
                "MOTION" => (1..=3).contains(&index),
                "SHADOW_MOTION" => (1..=2).contains(&index),
                "RIVER_PARTS_NAME" => index == 2,
                _ => false,
            };
            if command == "CHARA" && index == 1 {
                if let Some(value) = arg["value"]
                    .as_str()
                    .filter(|s| s.to_ascii_lowercase().ends_with(".cfg"))
                {
                    arg["value"] = json!(&value[..value.len() - 4]);
                    arg.as_object_mut().unwrap().remove("game_bytes");
                }
                continue;
            }
            if !resource {
                continue;
            }
            let Some(value) = arg["value"].as_str() else {
                continue;
            };
            let normalized = value.replace('\\', "/");
            let lower = normalized.to_ascii_lowercase();
            let ext = lower.rsplit_once('.').map(|p| p.1).unwrap_or("");
            let replacement = match ext {
                "chr" | "chr_" | "mds" | "mdt" | "mot" | "wgt" | "bbp" => "gltf",
                "cfg" | "map" | "img" | "tm2" => "json",
                "snd" => "sound.json",
                "pss" => "mp4",
                _ => continue,
            };
            let parent = source
                .rsplit_once('/')
                .map_or("", |p| p.0)
                .to_ascii_lowercase();
            let relative = format!("{parent}/{lower}");
            let exists = identities.contains(&lower)
                || identities.contains(&relative)
                || identities.iter().any(|identity| {
                    identity.rsplit('/').next() == Some(lower.rsplit('/').next().unwrap())
                });
            if exists {
                arg["value"] = json!(native_path(&normalized, replacement));
                arg.as_object_mut().unwrap().remove("game_bytes");
            }
        }
    }
}
fn in_character(alias: &str) -> bool {
    alias.split('/').rev().skip(1).any(|p| {
        p.to_ascii_lowercase()
            .trim_end_matches('_')
            .ends_with(".chr")
    })
}

fn save(path: &Path, value: &Value) -> CoreResult<()> {
    fs::create_dir_all(
        path.parent()
            .ok_or_else(|| fail("CFG output has no parent"))?,
    )?;
    fs::write(path, serde_json::to_vec_pretty(value)?)?;
    Ok(())
}
fn convert(
    raw: &[u8],
    alias: &str,
    published: &Path,
    excluded: &BTreeSet<String>,
    catalog: &mut BTreeMap<String, Value>,
    identities: &BTreeSet<String>,
    depth: usize,
) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("CFG PACK nesting exceeds 16"));
    }
    if is_cfg(alias) {
        if !excluded.contains(alias) && !in_character(alias) && !super::audio::owns_config(alias) {
            let path = output_path(alias);
            let mut document = parse(raw).map_err(|e| fail(format!("{alias}: {e}")))?;
            migrate_references(&mut document, alias, identities);
            let target = published.join("port_data").join(&path);
            if let Some((owner, _)) = catalog.iter().find(|(_, value)| {
                value
                    .as_str()
                    .is_some_and(|existing| existing.eq_ignore_ascii_case(&path))
            }) {
                return Err(fail(format!(
                    "native CFG output collision: {owner} and {alias} -> {path}"
                )));
            }
            if target.exists() {
                return Err(fail(format!(
                    "native CFG output collision with converted asset: {alias} -> {path}"
                )));
            }
            save(&target, &document)?;
            if catalog.insert(alias.into(), json!(path)).is_some() {
                return Err(fail("duplicate CFG alias"));
            }
        }
        return Ok(());
    }
    if let Some(members) = pack(raw) {
        let mut names = BTreeSet::new();
        for (i, m) in members.iter().enumerate() {
            if m.size == 0 && !is_cfg(&m.name) {
                continue;
            }
            let child = if names.insert(m.name.clone()) {
                format!("{alias}/{}", m.name)
            } else {
                format!("{alias}/{}#entry:{i}", m.name)
            };
            convert(
                slice(raw, m.start, m.size)?,
                &child,
                published,
                excluded,
                catalog,
                identities,
                depth + 1,
            )?;
        }
    }
    Ok(())
}
fn strip(
    raw: &mut [u8],
    alias: &str,
    consumed: &BTreeSet<String>,
    depth: usize,
) -> CoreResult<bool> {
    if depth > 16 {
        return Err(fail("CFG PACK nesting exceeds 16"));
    }
    let mut changed = false;
    if let Some(members) = pack(raw) {
        let mut names = BTreeSet::new();
        for (i, m) in members.iter().enumerate() {
            let child = if names.insert(m.name.clone()) {
                format!("{alias}/{}", m.name)
            } else {
                format!("{alias}/{}#entry:{i}", m.name)
            };
            if consumed.contains(&child) {
                raw[m.start..m.start + m.size].fill(0);
                raw[m.size_field..m.size_field + 4].fill(0);
                changed = true;
            } else if m.size > 0 {
                changed |= strip(
                    &mut raw[m.start..m.start + m.size],
                    &child,
                    consumed,
                    depth + 1,
                )?;
            }
        }
    }
    Ok(changed)
}
fn files(directory: &Path, out: &mut Vec<PathBuf>) -> CoreResult<()> {
    for entry in fs::read_dir(directory)? {
        let path = entry?.path();
        if path.is_dir() {
            files(&path, out)?
        } else if path.is_file() {
            out.push(path)
        }
    }
    Ok(())
}

/// Publishes every direct or PACK-contained CFG and removes its compatibility payload.
pub fn convert_tree(raw: &Path, published: &Path, _jobs: usize) -> CoreResult<()> {
    let excluded = BTreeSet::new();
    let mut catalog = BTreeMap::new();
    let mut all = Vec::new();
    files(raw, &mut all)?;
    all.sort();
    let mut identities = BTreeSet::new();
    for path in &all {
        let alias = path
            .strip_prefix(raw)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        identities.insert(alias.to_ascii_lowercase());
        if fs::metadata(path)?.len() <= 128 << 20 {
            let alias = path
                .strip_prefix(raw)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            inventory(&fs::read(path)?, &alias, &mut identities, 0)?;
        }
    }
    for path in &all {
        if fs::metadata(path)?.len() > 128 << 20 {
            continue;
        }
        let alias = path
            .strip_prefix(raw)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        convert(
            &fs::read(path)?,
            &alias,
            published,
            &excluded,
            &mut catalog,
            &identities,
            0,
        )?;
    }
    let consumed: BTreeSet<_> = catalog.keys().cloned().collect();
    for path in all {
        let alias = path
            .strip_prefix(raw)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        let target = published.join("data").join(path.strip_prefix(raw).unwrap());
        if consumed.contains(&alias) {
            if target.is_file() {
                fs::remove_file(target)?
            }
            continue;
        }
        if target.is_file() && fs::metadata(&target)?.len() <= 128 << 20 {
            let mut b = fs::read(&target)?;
            if strip(&mut b, &alias, &consumed, 0)? {
                fs::write(target, b)?
            }
        }
    }

    Ok(())
}

fn validate_document(document: &Value) -> CoreResult<()> {
    if document["schema_version"] != 1 {
        return Err(fail("unsupported CFG document schema"));
    }
    if document["kind"] == "movie_subtitles" {
        for chapter in document["chapters"]
            .as_array()
            .ok_or_else(|| fail("subtitle chapters missing"))?
        {
            if chapter["id"]
                .as_i64()
                .is_none_or(|n| i32::try_from(n).is_err())
            {
                return Err(fail("invalid subtitle chapterid"));
            }
            let entries = chapter["entries"]
                .as_array()
                .ok_or_else(|| fail("subtitle entries missing"))?;
            if entries.len() > 20 {
                return Err(fail("subtitle exceeds native slots"));
            }
            for entry in entries {
                for key in ["start_seconds", "clear_seconds"] {
                    if entry[key].as_f64().is_none_or(|v| !v.is_finite()) {
                        return Err(fail("invalid subtitle timing"));
                    }
                }
                let mut text = entry.clone();
                text["type"] = json!("string");
                text["value"] = entry["text"].clone();
                validate_document(
                    &json!({"schema_version":1,"commands":[{"command":"TEXT","args":[text]}]}),
                )?;
            }
        }
        return Ok(());
    }
    if !document["kind"].is_null() {
        return Err(fail("unknown CFG document kind"));
    }
    for command in document["commands"]
        .as_array()
        .ok_or_else(|| fail("CFG commands missing"))?
    {
        if command["command"].as_str().is_none()
            && command["tag_index"]
                .as_i64()
                .is_none_or(|v| !(0..=i16::MAX as i64).contains(&v))
        {
            return Err(fail("CFG command identity missing"));
        }
        for argument in command["args"]
            .as_array()
            .ok_or_else(|| fail("CFG arguments missing"))?
        {
            match argument["type"].as_str() {
                Some("int") => {
                    if argument["value"]
                        .as_i64()
                        .is_none_or(|v| i32::try_from(v).is_err())
                    {
                        return Err(fail("invalid CFG integer"));
                    }
                }
                Some("float") => {
                    if argument["value"]
                        .as_f64()
                        .is_none_or(|v| !(v as f32).is_finite())
                    {
                        return Err(fail("invalid CFG float"));
                    }
                }
                Some("string") => {
                    if !argument["value"].is_string() {
                        return Err(fail("invalid CFG string"));
                    }
                    if let Some(bytes) = argument.get("game_bytes") {
                        let hex = bytes
                            .as_str()
                            .ok_or_else(|| fail("invalid CFG string byte fallback"))?;
                        if !hex.len().is_multiple_of(2)
                            || !hex.bytes().all(|b| b.is_ascii_hexdigit())
                        {
                            return Err(fail("invalid CFG string byte fallback"));
                        }
                    }
                }
                Some("invalid") => {
                    if !argument["value"].is_null() {
                        return Err(fail("invalid CFG missing-value argument"));
                    }
                }
                _ => return Err(fail("unknown CFG argument type")),
            }
        }
    }
    Ok(())
}
fn audit_payload(b: &[u8], alias: &str, depth: usize) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("CFG audit PACK nesting exceeds16"));
    }
    if is_cfg(alias) {
        return Err(fail(format!("proprietary CFG payload remains: {alias}")));
    }
    if let Some(members) = pack(b) {
        for m in members {
            if m.size > 0 {
                audit_payload(
                    slice(b, m.start, m.size)?,
                    &format!("{alias}/{}", m.name),
                    depth + 1,
                )?
            }
        }
    }
    Ok(())
}
/// Verifies typed CFG documents and rejects any retained compatibility CFG payloads.
pub fn verify(root: &Path) -> CoreResult<()> {
    let port = root.join("port_data");
    if port.is_dir() {
        let mut all = Vec::new();
        files(&port, &mut all)?;
        for path in all {
            if path.extension().is_some_and(|e| e == "json") {
                let doc: Value = serde_json::from_slice(&fs::read(&path)?)?;
                if doc["commands"].is_array() || doc["kind"] == "movie_subtitles" {
                    validate_document(&doc)?;
                }
            }
        }
    }
    let data = root.join("data");
    if data.is_dir() {
        let mut all = Vec::new();
        files(&data, &mut all)?;
        for path in all {
            if fs::metadata(&path)?.len() > 128 << 20 {
                continue;
            }
            let alias = path
                .strip_prefix(&data)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            audit_payload(&fs::read(path)?, &alias, 0)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn text_types_comments_and_escapes_match_interpreter() {
        let doc =
            parse(b"v2; // comment\nTAG -7,1.5,\"a\\\"b\",,bad,12 trailing; /* done */").unwrap();
        let args = &doc["commands"][1]["args"];
        assert_eq!(args[0], json!({"type":"int","value":-7}));
        assert_eq!(args[1], json!({"type":"float","value":1.5}));
        assert_eq!(args[2]["value"], "a\"b");
        assert_eq!(args[3]["type"], "invalid");
        assert_eq!(args[4]["type"], "invalid");
        assert_eq!(args[5]["value"], 12);
    }
    #[test]
    fn binary_tag_indices_and_types_are_preserved() {
        let mut b = b"BIN\0".to_vec();
        b.extend(7i16.to_le_bytes());
        b.extend(3i16.to_le_bytes());
        b.extend([1, 2, 3, 0]);
        b.extend((-9i32).to_le_bytes());
        b.extend(2.5f32.to_le_bytes());
        b.extend(b"ok\0\0");
        b.extend((-1i16).to_le_bytes());
        let d = parse(&b).unwrap();
        assert_eq!(d["commands"][0]["tag_index"], 7);
        assert_eq!(d["commands"][0]["args"][2]["value"], "ok");
    }
    #[test]
    fn zero_index_binary_tag_with_no_arguments_executes() {
        let d = parse(b"BIN\0\0\0\0\0").unwrap();
        assert_eq!(d["commands"], json!([{"tag_index":0,"args":[]}]));
    }
    #[test]
    fn movie_subtitles_preserve_slots_doubles_linefeeds_and_carriage_returns() {
        let document=parse(b"@901\r\n_STA 1.65\r\n_CLR 9.55\r\n_STR one\\ntwo\r\n_STR without timings\r\n_STA 7.123456789123\r\n_END\r\n@902\r\n_END\r\n").unwrap();
        assert_eq!(document["kind"], "movie_subtitles");
        let entries = &document["chapters"][0]["entries"];
        assert_eq!(entries[0]["text"], "one\ntwo\r");
        assert_eq!(entries[0]["start_seconds"], 1.65);
        assert_eq!(entries[1]["start_seconds"], 0.);
        assert_eq!(entries[2]["text"], "");
        assert_eq!(entries[2]["start_seconds"], 7.123456789123);
        assert_eq!(document["chapters"][1]["entries"], json!([]));
        validate_document(&document).unwrap();
    }
    #[test]
    fn shift_jis_text_is_readable_unicode() {
        assert_eq!(string_argument(&[0x93, 0xfa, 0x96, 0x7b])["value"], "日本");
    }
    #[test]
    fn missing_terminator_drops_incomplete_argument() {
        let d = parse(b"TAG 1,2").unwrap();
        assert_eq!(d["commands"][0]["args"], json!([{"type":"int","value":1}]));
    }
    #[test]
    fn resource_fields_migrate_without_rewriting_gameplay_strings() {
        let mut doc=parse(b"MDS \"room.mds\"; NAME \"room.mds\"; CHARA \"chara/hero.chr\",\"info.cfg\"; MOVIE 3,\"OPEN.PSS\";").unwrap();
        migrate_references(
            &mut doc,
            "map/room/info.cfg",
            &BTreeSet::from([
                "map/room/room.mds".into(),
                "chara/hero.chr".into(),
                "movie/open.pss".into(),
            ]),
        );
        assert_eq!(doc["commands"][0]["args"][0]["value"], "room.gltf");
        assert_eq!(doc["commands"][1]["args"][0]["value"], "room.mds");
        assert_eq!(doc["commands"][2]["args"][0]["value"], "chara/hero.gltf");
        assert_eq!(doc["commands"][2]["args"][1]["value"], "info");
        assert_eq!(doc["commands"][3]["args"][1]["value"], "OPEN.mp4");
    }
    #[test]
    fn physical_paths_preserve_pack_context_and_disambiguate_embedded_members() {
        assert_eq!(
            native_path("etc/gsys.pak/a_mu.mds", "gltf"),
            "etc/gsys.pak/a_mu.model.gltf"
        );
        assert_eq!(
            native_path("etc/gsys.pak/sukkopu.mds", "gltf"),
            "etc/gsys.pak/sukkopu.model.gltf"
        );
        assert_eq!(
            native_path("etc/gsys.pak/a_mu.chr", "gltf"),
            "etc/gsys.pak/a_mu.gltf"
        );
        assert_eq!(
            native_path("etc/other.pak/a_mu.mds", "gltf"),
            "etc/other.pak/a_mu.gltf"
        );
        assert_eq!(
            native_path("outer.pcp/hero.chr/child.chr", "gltf"),
            "outer.pcp/hero.assets/child.gltf"
        );
        assert_eq!(
            native_path("outer.pcp/hero.mds#entry:2", "gltf"),
            "outer.pcp/hero.entry_2.gltf"
        );
        assert_eq!(
            native_path("container.bin#offset:80", "gltf"),
            "container.bin.offset_80.gltf"
        );
    }

    #[test]
    fn map_and_cfg_scripts_keep_distinct_physical_paths_and_commands() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let output = temp.path().join("out");
        fs::create_dir_all(&raw).unwrap();
        fs::write(raw.join("room.cfg"), b"WATER_SURFACE_NUM 1;").unwrap();
        fs::write(raw.join("room.map"), b"PIECE room;").unwrap();
        convert_tree(&raw, &output, 1).unwrap();
        let water: Value =
            serde_json::from_slice(&fs::read(output.join("port_data/room.cfg.json")).unwrap())
                .unwrap();
        let map: Value =
            serde_json::from_slice(&fs::read(output.join("port_data/room.map.json")).unwrap())
                .unwrap();
        assert_eq!(water["commands"][0]["command"], "WATER_SURFACE_NUM");
        assert_eq!(map["commands"][0]["command"], "PIECE");
        assert_eq!(
            native_path("outer.pac/room.MAP#entry:2", "json"),
            "outer.pac/room.entry_2.map.json"
        );
    }

    #[test]
    fn unresolved_configuration_output_collision_fails_before_overwrite() {
        let temp = tempfile::tempdir().unwrap();
        let mut catalog = BTreeMap::new();
        convert(
            b"FIRST 1;",
            "room.cfg",
            temp.path(),
            &BTreeSet::new(),
            &mut catalog,
            &BTreeSet::new(),
            0,
        )
        .unwrap();
        let error = convert(
            b"SECOND 2;",
            "ROOM.CFG",
            temp.path(),
            &BTreeSet::new(),
            &mut catalog,
            &BTreeSet::new(),
            0,
        )
        .unwrap_err()
        .to_string();
        assert!(error.contains("collision"), "{error}");
        let first: Value =
            serde_json::from_slice(&fs::read(temp.path().join("port_data/room.cfg.json")).unwrap())
                .unwrap();
        assert_eq!(first["commands"][0]["command"], "FIRST");
    }

    #[test]
    fn configuration_cannot_replace_a_texture_or_other_converted_asset() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let output = temp.path().join("out");
        fs::create_dir_all(&raw).unwrap();
        fs::create_dir_all(output.join("port_data")).unwrap();
        fs::write(raw.join("room.cfg"), b"WATER_SURFACE_NUM 1;").unwrap();
        fs::write(
            output.join("port_data/room.cfg.json"),
            b"earlier converted texture",
        )
        .unwrap();
        assert!(
            convert_tree(&raw, &output, 1)
                .unwrap_err()
                .to_string()
                .contains("collision")
        );
        assert_eq!(
            fs::read(output.join("port_data/room.cfg.json")).unwrap(),
            b"earlier converted texture"
        );
    }

    #[test]
    fn nested_cfg_payload_is_zeroed_and_audited() {
        let mut b = vec![0; 176];
        b[..9].copy_from_slice(b"info.cfg\0");
        b[64..68].copy_from_slice(&80u32.to_le_bytes());
        b[68..72].copy_from_slice(&16u32.to_le_bytes());
        b[72..76].copy_from_slice(&96u32.to_le_bytes());
        b[80..88].copy_from_slice(b"TAG 42;\0");
        assert!(audit_payload(&b, "hero.chr", 0).is_err());
        let consumed = BTreeSet::from(["hero.chr/info.cfg".into()]);
        assert!(strip(&mut b, "hero.chr", &consumed, 0).unwrap());
        assert_eq!(word(&b, 68).unwrap(), 0);
        assert!(b[80..96].iter().all(|&v| v == 0));
        audit_payload(&b, "hero.chr", 0).unwrap();
    }

    #[test]
    #[ignore = "requires extracted retail assets in KZ_CFG_RAW and isolated KZ_CFG_OUTPUT"]
    fn retail_cfg_audit() {
        let raw = PathBuf::from(std::env::var("KZ_CFG_RAW").expect("KZ_CFG_RAW"));
        let output = PathBuf::from(std::env::var("KZ_CFG_OUTPUT").expect("KZ_CFG_OUTPUT"));
        fs::create_dir_all(output.join("port_data")).unwrap();
        if let Some(audio) = std::env::var_os("KZ_CFG_AUDIO_CATALOG") {
            fs::copy(audio, output.join("port_data/audio_catalog.json")).unwrap();
        }
        convert_tree(&raw, &output, 4).unwrap();
        verify(&output).unwrap();
    }
}
