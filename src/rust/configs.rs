//! Publishes tagged configuration scripts as native semantic JSON documents.
//!
//! The tokenizer below reads a script into the commands and typed operands the retail interpreter
//! pushes; `configs_native` turns them into the document of the script's family.
use super::archive::SourceTree;
use super::progress::Phase;
use super::{CoreError, CoreResult};
use rayon::prelude::*;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

include!(concat!(env!("OUT_DIR"), "/tracked_configs.rs"));

/// Returns the build-time fingerprint of gameplay source paths and bytes.
pub fn source_fingerprint() -> String {
    TRACKED_CONFIG_FINGERPRINT.to_owned()
}

/// Lists archive paths and content digests of the required gameplay documents.
pub fn gameplay_manifest() -> &'static [(&'static str, [u8; 32])] {
    GAMEPLAY_DOCUMENTS
}

/// Returns the expected content digest of a gameplay archive entry.
pub fn gameplay_digest(path: &str) -> Option<&'static [u8; 32]> {
    let lower = path.to_ascii_lowercase();
    let index = GAMEPLAY_DOCUMENTS
        .binary_search_by_key(&lower.as_str(), |entry| entry.0)
        .ok()?;
    Some(&GAMEPLAY_DOCUMENTS[index].1)
}

/// Checks a gameplay document against its build-time content digest.
pub fn verify_gameplay_document(path: &str, bytes: &[u8]) -> CoreResult<()> {
    let expected = gameplay_digest(path)
        .ok_or_else(|| fail(format!("untracked gameplay document: {path}")))?;
    if Sha256::digest(bytes).as_slice() != expected {
        return Err(fail(format!("gameplay document changed: {path}")));
    }
    Ok(())
}

/// Reads the required gameplay entries from an external data archive.
fn read_gameplay_archive(path: &Path) -> CoreResult<Vec<(&'static str, Vec<u8>)>> {
    let archive = super::zip_archive::Archive::open(path)?;
    let entries: BTreeMap<_, _> = (0..archive.len())
        .map(|index| (archive.name(index).to_ascii_lowercase(), index))
        .collect();
    gameplay_manifest()
        .iter()
        .map(|&(name, _)| {
            let index = entries
                .get(name)
                .ok_or_else(|| fail(format!("gameplay document missing: {name}")))?;
            let bytes = archive.read(*index)?;
            verify_gameplay_document(name, &bytes)?;
            Ok((name, bytes))
        })
        .collect()
}

/// Loads gameplay from the packaged seed or a matching completed data archive.
pub fn gameplay_documents(root: Option<&Path>) -> CoreResult<Vec<(&'static str, Vec<u8>)>> {
    if gameplay_manifest().is_empty() {
        return Ok(Vec::new());
    }
    let mut candidates = Vec::new();
    if let Some(directory) = std::env::current_exe()?.parent() {
        candidates.push(directory.join("assets/data.zip"));
        candidates.push(directory.join("data.zip"));
    }
    if let Some(root) = root {
        candidates.push(root.join("data.zip"));
    }
    #[cfg(test)]
    candidates.push(PathBuf::from(env!("OUT_DIR")).join("data.zip"));
    let mut failures = Vec::new();
    for path in candidates {
        match read_gameplay_archive(&path) {
            Ok(documents) => return Ok(documents),
            Err(error) => failures.push(format!("{}: {error}", path.display())),
        }
    }
    Err(fail(format!(
        "matching gameplay data.zip is required beside the executable: {}",
        failures.join("; ")
    )))
}

/// Writes the build's gameplay documents into a data folder as loose files at their archive paths
/// (`gameplay/...`), each checked against its digest, for a game that reads its data from a folder
/// rather than the extractor's archive. JSON files under `gameplay/` the build no longer has are
/// removed. Returns how many documents were written.
pub fn publish_gameplay_tree(data_root: &Path) -> CoreResult<usize> {
    let documents = gameplay_documents(None)?;
    for (name, bytes) in &documents {
        let path = data_root.join(name);
        fs::create_dir_all(
            path.parent()
                .ok_or_else(|| fail(format!("bad gameplay path: {name}")))?,
        )?;
        let temp = path.with_extension("json.part");
        fs::write(&temp, bytes)?;
        fs::rename(&temp, &path)?;
    }
    let mut stale = Vec::new();
    collect_json(&data_root.join("gameplay"), &mut stale)?;
    for path in stale {
        let name = path
            .strip_prefix(data_root)
            .map_err(|_| fail("gameplay file outside the data folder"))?
            .to_string_lossy()
            .replace('\\', "/")
            .to_ascii_lowercase();
        if gameplay_digest(&name).is_none() {
            fs::remove_file(&path)?;
        }
    }
    Ok(documents.len())
}

fn collect_json(directory: &Path, files: &mut Vec<PathBuf>) -> CoreResult<()> {
    if !directory.is_dir() {
        return Ok(());
    }
    for entry in fs::read_dir(directory)? {
        let path = entry?.path();
        if path.is_dir() {
            collect_json(&path, files)?;
        } else if path
            .extension()
            .is_some_and(|extension| extension == "json")
        {
            files.push(path);
        }
    }
    Ok(())
}

/// Dark Cloud 2's character animation slicing plan, which a game's gameplay documents may hold.
const CHARACTER_CLIPS: &str = "gameplay/character_clips.json";

/// Digest of only the source-derived animation slicing plan used by asset extraction.
pub fn asset_plan_fingerprint() -> String {
    gameplay_digest(CHARACTER_CLIPS).map_or_else(String::new, |digest| hex(digest))
}

/// True when a relative gameplay path belongs to the tracked source tree.
pub fn is_tracked_gameplay(path: &str) -> bool {
    let normalized = path.replace('\\', "/").to_ascii_lowercase();
    TRACKED_GAMEPLAY_PATHS
        .binary_search(&normalized.as_str())
        .is_ok()
}

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

/// Decodes the string arguments a game's own reader gives as raw bytes in hex
/// (`{"type":"string","bytes":"..."}`) as Shift-JIS, as this module decodes Dark Cloud 2's.
pub(crate) fn decode_string_arguments(document: &mut Value) -> CoreResult<()> {
    for statement in document["commands"].as_array_mut().into_iter().flatten() {
        for argument in statement["args"].as_array_mut().into_iter().flatten() {
            let Some(text) = argument.get("bytes").and_then(Value::as_str) else {
                continue;
            };
            if text.len() % 2 != 0 {
                return Err(fail("odd-length string argument bytes"));
            }
            let bytes = (0..text.len())
                .step_by(2)
                .map(|at| u8::from_str_radix(&text[at..at + 2], 16))
                .collect::<Result<Vec<u8>, _>>()
                .map_err(|_| fail("string argument bytes are not hex"))?;
            *argument = string_argument(&bytes);
        }
    }
    Ok(())
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
        if !(0..=256).contains(&count) {
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
    Ok(
        json!({"schema_version":super::configs_native::SCHEMA_VERSION,"kind":"movie_subtitles","chapters":chapters}),
    )
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
    let leaf = alias
        .rsplit('/')
        .next()
        .unwrap_or(alias)
        .split('#')
        .next()
        .unwrap_or("")
        .to_ascii_lowercase();
    if extension == "json" && (leaf.ends_with(".img") || leaf.ends_with(".tm2")) {
        return super::textures::document_path(alias)
            .expect("texture aliases have a valid native path");
    }
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

/// Whether a retail CFG or MAP alias has a loose native document in the tracked corpus.
pub fn is_tracked(alias: &str) -> bool {
    let path = output_path(alias).to_ascii_lowercase();
    TRACKED_CONFIG_PATHS.binary_search(&path.as_str()).is_ok()
}

/// Retail script leaves replaced by checked-in gameplay documents.
pub fn gameplay_source_aliases(inventory: &super::inventory::Inventory) -> HashSet<String> {
    inventory
        .leaves()
        .iter()
        .filter(|resource| is_cfg(&resource.alias))
        .map(|resource| resource.alias.clone())
        .collect()
}

/// Checks source-to-gameplay path coverage after all archive conversion has completed.
pub fn verify_inventory_paths(inventory: &super::inventory::Inventory) -> CoreResult<usize> {
    let mut discovered = BTreeMap::new();
    for resource in inventory.leaves() {
        let alias = &resource.alias;
        if !is_cfg(alias)
            || in_character(alias)
            || super::audio::owns_config(alias)
            || super::configs_native::unused(alias)
        {
            continue;
        }
        let path = output_path(alias).to_ascii_lowercase();
        if !is_tracked(alias) {
            return Err(fail(format!(
                "retail CFG has no tracked native document: {alias} -> {path}"
            )));
        }
        if let Some(other) = discovered.insert(path.clone(), alias) {
            return Err(fail(format!(
                "native CFG output collision: {other} and {alias} -> {path}"
            )));
        }
    }
    Ok(discovered.len())
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
    let Some(commands) = document.get_mut("commands").and_then(Value::as_array_mut) else {
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
            let mut normalized = value.replace('\\', "/");
            while normalized.contains("//") {
                normalized = normalized.replace("//", "/");
            }
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
                // These operands name members of the map or sky's already-selected PACK.
                let path = if matches!(command.as_str(), "IMG" | "SKY_IMG")
                    && matches!(ext, "img" | "tm2")
                {
                    super::textures::legacy_document_path(&normalized)
                        .expect("texture member aliases have valid native names")
                } else {
                    native_path(&normalized, replacement)
                };
                arg["value"] = json!(path);
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
enum Planned {
    Unused(String),
    Save {
        alias: String,
        path: String,
        document: Value,
    },
}
fn plan(
    raw: &[u8],
    alias: &str,
    excluded: &BTreeSet<String>,
    identities: &BTreeSet<String>,
    depth: usize,
    out: &mut Vec<Planned>,
) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("CFG PACK nesting exceeds 16"));
    }
    if is_cfg(alias) {
        if !excluded.contains(alias) && !in_character(alias) && !super::audio::owns_config(alias) {
            let path = output_path(alias);
            let mut document = parse(raw).map_err(|e| fail(format!("{alias}: {e}")))?;
            migrate_references(&mut document, alias, identities);
            if super::configs_native::unused(alias) {
                out.push(Planned::Unused(alias.into()));
                return Ok(());
            }
            if document["kind"] != "movie_subtitles" {
                document = super::configs_native::build(alias, &document)?.0;
            }
            out.push(Planned::Save {
                alias: alias.into(),
                path,
                document,
            });
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
            plan(
                slice(raw, m.start, m.size)?,
                &child,
                excluded,
                identities,
                depth + 1,
                out,
            )?;
        }
    }
    Ok(())
}
#[derive(Default)]
struct Catalog {
    aliases: BTreeMap<String, Value>,
    paths: BTreeMap<String, String>,
    writes: Vec<(PathBuf, Value)>,
}
impl Catalog {
    fn record(&mut self, planned: Planned, published: &Path) -> CoreResult<()> {
        match planned {
            Planned::Unused(alias) => {
                if self.aliases.insert(alias, Value::Null).is_some() {
                    return Err(fail("duplicate CFG alias"));
                }
            }
            Planned::Save {
                alias,
                path,
                document,
            } => {
                if let Some(owner) = self.paths.get(&path.to_ascii_lowercase()) {
                    return Err(fail(format!(
                        "native CFG output collision: {owner} and {alias} -> {path}"
                    )));
                }
                let target = published.join("config").join(&path);
                if target.exists() {
                    return Err(fail(format!(
                        "native CFG output collision with converted asset: {alias} -> {path}"
                    )));
                }
                if self.aliases.insert(alias.clone(), json!(path)).is_some() {
                    return Err(fail("duplicate CFG alias"));
                }
                self.paths.insert(path.to_ascii_lowercase(), alias);
                self.writes.push((target, document));
            }
        }
        Ok(())
    }
    fn commit(&self) -> CoreResult<()> {
        let saved: Vec<CoreResult<()>> = self
            .writes
            .par_iter()
            .map(|(target, document)| save(target, document))
            .collect();
        saved.into_iter().collect()
    }
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

fn source_cfg_aliases(
    raw: &[u8],
    alias: &str,
    depth: usize,
    out: &mut Vec<String>,
) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("CFG PACK nesting exceeds 16"));
    }
    if is_cfg(alias) {
        out.push(alias.to_owned());
        return Ok(());
    }
    if let Some(members) = pack(raw) {
        let mut names = BTreeSet::new();
        for (index, member) in members.iter().enumerate() {
            if member.size == 0 && !is_cfg(&member.name) {
                continue;
            }
            let child = if names.insert(member.name.clone()) {
                format!("{alias}/{}", member.name)
            } else {
                format!("{alias}/{}#entry:{index}", member.name)
            };
            source_cfg_aliases(
                slice(raw, member.start, member.size)?,
                &child,
                depth + 1,
                out,
            )?;
        }
    }
    Ok(())
}

fn tracked_documents(root: &Path) -> CoreResult<BTreeMap<String, Value>> {
    let mut docs = BTreeMap::new();
    let mut all = Vec::new();
    files(root, &mut all)?;
    for file in all {
        let path = file
            .strip_prefix(root)
            .map_err(|err| fail(err.to_string()))?
            .to_string_lossy()
            .replace('\\', "/");
        if !path.ends_with(".cfg.json") && !path.ends_with(".map.json") {
            continue;
        }
        let document: Value = serde_json::from_slice(&fs::read(&file)?)
            .map_err(|err| fail(format!("tracked config {path}: {err}")))?;
        validate_document(&document)
            .map_err(|err| fail(format!("tracked config {path}: {err}")))?;
        if docs.insert(path.clone(), document).is_some() {
            return Err(fail(format!("duplicate tracked config {path}")));
        }
    }
    Ok(docs)
}

fn source_alias_catalog(
    source: &SourceTree,
    phase: &Phase,
) -> CoreResult<(BTreeMap<String, String>, BTreeSet<String>)> {
    let names: Vec<_> = source.names().map(str::to_owned).collect();
    let entries: Vec<CoreResult<Vec<String>>> = names
        .par_iter()
        .map(|name| {
            let mut aliases = Vec::new();
            if is_cfg(name) {
                aliases.push(name.clone());
            } else if source.len(name)? <= 128 << 20 {
                source_cfg_aliases(&source.read(name)?, name, 0, &mut aliases)?;
            }
            phase.inc(1);
            Ok(aliases)
        })
        .collect();
    let mut catalog = BTreeMap::new();
    let mut consumed = BTreeSet::new();
    for entry in entries {
        for alias in entry? {
            if in_character(&alias) || super::audio::owns_config(&alias) {
                continue;
            }
            consumed.insert(alias.clone());
            let path = output_path(&alias);
            if !super::configs_native::unused(&alias)
                && let Some(other) = catalog.insert(path.clone(), alias.clone())
            {
                return Err(fail(format!(
                    "native CFG output collision: {other} and {alias} -> {path}"
                )));
            }
        }
    }
    Ok((catalog, consumed))
}

/// Removes retail CFG payloads from the converted data tree.
pub fn convert_source(source: &SourceTree, published: &Path, phase: &Phase) -> CoreResult<()> {
    let names: Vec<_> = source.names().map(str::to_owned).collect();
    phase.set_total(names.len() as u64 * 2);
    let (_, consumed) = source_alias_catalog(source, phase)?;
    names.par_iter().try_for_each(|name| -> CoreResult<()> {
        let target = super::archive::compatibility_path(published, name);
        if consumed.contains(name) {
            if target.is_file() {
                fs::remove_file(target)?;
            }
        } else if target.is_file() && fs::metadata(&target)?.len() <= 128 << 20 {
            let mut bytes = fs::read(&target)?;
            if strip(&mut bytes, name, &consumed, 0)? {
                fs::write(target, bytes)?;
            }
        }
        phase.inc(1);
        Ok(())
    })?;
    Ok(())
}

/// Verifies the complete source CFG inventory after conversion has finished.
pub fn verify_inventory(source: &SourceTree, phase: &Phase) -> CoreResult<usize> {
    phase.set_total(source.names().count() as u64);
    let (catalog, _) = source_alias_catalog(source, phase)?;
    let tracked: BTreeSet<_> = TRACKED_CONFIG_PATHS
        .iter()
        .map(|path| (*path).to_owned())
        .collect();
    let discovered: BTreeSet<_> = catalog.keys().cloned().collect();
    if !discovered.is_subset(&tracked) {
        let missing = discovered.difference(&tracked).next().cloned();
        return Err(fail(format!(
            "retail CFG has no tracked native document: {missing:?}"
        )));
    }
    Ok(catalog.len())
}

/// Rebuilds every native CFG document from the ISO index in place and compares its meaning.
pub fn audit_source(
    source: &SourceTree,
    config_root: &Path,
    phase: &Phase,
) -> CoreResult<(usize, usize, usize, usize, usize)> {
    let names: Vec<_> = source.names().map(str::to_owned).collect();
    let docs = tracked_documents(config_root)?;
    let mut identities = BTreeSet::new();
    let mut scripts = Vec::new();
    let mut character = 0;
    let mut audio = 0;
    phase.set_total(names.len() as u64 + docs.len() as u64);
    for name in &names {
        identities.insert(name.to_ascii_lowercase());
        if source.len(name)? <= 128 << 20 {
            let bytes = source.read(name)?;
            inventory(&bytes, name, &mut identities, 0)?;
            let mut aliases = Vec::new();
            source_cfg_aliases(&bytes, name, 0, &mut aliases)?;
            for alias in aliases {
                if in_character(&alias) {
                    character += 1;
                    continue;
                }
                if super::audio::owns_config(&alias) {
                    audio += 1;
                    continue;
                }
                let data = if alias == *name {
                    bytes.clone()
                } else {
                    // The existing planner supplies the nested payload without writing a raw tree.
                    let mut payloads = Vec::new();
                    fn locate(
                        b: &[u8],
                        at: &str,
                        wanted: &str,
                        out: &mut Vec<u8>,
                        depth: usize,
                    ) -> CoreResult<bool> {
                        if at == wanted {
                            out.extend_from_slice(b);
                            return Ok(true);
                        }
                        if depth > 16 {
                            return Err(fail("CFG PACK nesting exceeds 16"));
                        }
                        if let Some(members) = pack(b) {
                            let mut seen = BTreeSet::new();
                            for (i, m) in members.iter().enumerate() {
                                let child = if seen.insert(m.name.clone()) {
                                    format!("{at}/{}", m.name)
                                } else {
                                    format!("{at}/{}#entry:{i}", m.name)
                                };
                                if locate(
                                    slice(b, m.start, m.size)?,
                                    &child,
                                    wanted,
                                    out,
                                    depth + 1,
                                )? {
                                    return Ok(true);
                                }
                            }
                        }
                        Ok(false)
                    }
                    if !locate(&bytes, name, &alias, &mut payloads, 0)? {
                        return Err(fail(format!("cannot locate nested CFG {alias}")));
                    }
                    payloads
                };
                scripts.push((alias, data));
            }
        }
        phase.inc(1);
    }
    let mut checked = BTreeSet::new();
    let mut unused = 0;
    for (alias, raw) in scripts {
        if super::configs_native::unused(&alias) {
            unused += 1;
            continue;
        }
        let path = output_path(&alias);
        let Some(expected) = docs.get(path.as_str()) else {
            return Err(fail(format!("missing tracked CFG {alias} -> {path}")));
        };
        let mut parsed = parse(&raw).map_err(|err| fail(format!("{alias}: {err}")))?;
        migrate_references(&mut parsed, &alias, &identities);
        let actual = if parsed["kind"] == "movie_subtitles" {
            parsed
        } else {
            super::configs_native::build(&alias, &parsed)?.0
        };
        if &actual != expected {
            return Err(fail(format!(
                "tracked CFG differs from PAL source: {alias} -> {path}"
            )));
        }
        if !checked.insert(path.clone()) {
            return Err(fail(format!(
                "duplicate CFG output during source audit: {path}"
            )));
        }
        phase.inc(1);
    }
    if checked.len() != docs.len() {
        return Err(fail(format!(
            "tracked CFG count differs: source {} vs tracked {}",
            checked.len(),
            docs.len()
        )));
    }
    Ok((checked.len(), unused, character, audio, names.len()))
}

/// Explicit developer refresh of the checked-in corpus from indexed retail source extents.
pub fn export_source(source: &SourceTree, destination: &Path) -> CoreResult<usize> {
    if destination.exists() && !destination.is_dir() {
        return Err(fail(format!(
            "native config destination is not a directory: {}",
            destination.display()
        )));
    }
    let names: Vec<_> = source.names().map(str::to_owned).collect();
    let mut identities = BTreeSet::new();
    for name in &names {
        identities.insert(name.to_ascii_lowercase());
        if source.len(name)? <= 128 << 20 {
            inventory(&source.read(name)?, name, &mut identities, 0)?;
        }
    }
    let parent = destination
        .parent()
        .ok_or_else(|| fail("config destination has no parent"))?;
    fs::create_dir_all(parent)?;
    let stem = destination
        .file_name()
        .ok_or_else(|| fail("config destination has no name"))?
        .to_string_lossy();
    let nonce = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|err| fail(format!("system time: {err}")))?
        .as_nanos();
    let staging = parent.join(format!(".{stem}.refresh-{}-{nonce}", std::process::id()));
    fs::create_dir(&staging)?;
    let result = (|| -> CoreResult<usize> {
        let mut catalog = Catalog::default();
        for name in &names {
            if source.len(name)? > 128 << 20 {
                continue;
            }
            let bytes = source.read(name)?;
            let mut planned = Vec::new();
            plan(&bytes, name, &BTreeSet::new(), &identities, 0, &mut planned)?;
            for entry in planned {
                catalog.record(entry, &staging)?;
            }
        }
        catalog.commit()?;
        let fresh = staging.join("config");
        let mut generated = Vec::new();
        files(&fresh, &mut generated)?;
        for path in &generated {
            let document: Value = serde_json::from_slice(&fs::read(path)?)?;
            validate_document(&document)?;
        }
        if destination.is_dir() {
            let mut existing = Vec::new();
            files(destination, &mut existing)?;
            for file in existing {
                let relative = file
                    .strip_prefix(destination)
                    .map_err(|err| fail(err.to_string()))?;
                let name = relative.to_string_lossy().to_ascii_lowercase();
                if name.ends_with(".cfg.json") || name.ends_with(".map.json") {
                    continue;
                }
                let preserved = fresh.join(relative);
                if preserved.exists() {
                    return Err(fail(format!(
                        "native config refresh would replace unrelated file: {}",
                        file.display()
                    )));
                }
                fs::create_dir_all(preserved.parent().unwrap())?;
                fs::copy(&file, preserved)?;
            }
        }
        let backup = parent.join(format!(".{stem}.previous-{}-{nonce}", std::process::id()));
        if destination.exists() {
            fs::rename(destination, &backup)?;
        }
        if let Err(error) = fs::rename(&fresh, destination) {
            if backup.exists() {
                let _ = fs::rename(&backup, destination);
            }
            return Err(error.into());
        }
        if backup.exists() {
            fs::remove_dir_all(backup)?;
        }
        Ok(generated.len())
    })();
    let _ = fs::remove_dir_all(staging);
    result
}

fn character_container(alias: &str) -> Option<String> {
    let mut parent = String::new();
    let mut found = None;
    for component in alias
        .split('/')
        .take(alias.split('/').count().saturating_sub(1))
    {
        if !parent.is_empty() {
            parent.push('/');
        }
        parent.push_str(component);
        if component
            .to_ascii_lowercase()
            .trim_end_matches('_')
            .ends_with(".chr")
        {
            found = Some(parent.clone());
        }
    }
    found
}

/// Compact retail KEY intervals needed to slice animation assets during extraction.
pub(crate) struct CharacterPlan {
    pub profiles: Vec<String>,
    pub actions: super::actions::ProfileActions,
}

fn decode_character_plan(value: &Value) -> CoreResult<CharacterPlan> {
    let mut actions = super::actions::ProfileActions::default();
    let profiles: Vec<String> = serde_json::from_value(value["profiles"].clone())?;
    for entry in value["motions"]
        .as_array()
        .ok_or_else(|| fail("clip plan motions missing"))?
    {
        let motion = entry["motion"]
            .as_str()
            .ok_or_else(|| fail("clip plan motion missing"))?;
        let list = serde_json::from_value::<Vec<Value>>(entry["actions"].clone())?;
        actions.motions.insert(
            motion.to_owned(),
            list.iter()
                .map(decode_key_action)
                .collect::<CoreResult<_>>()?,
        );
    }
    for entry in value["unbound"]
        .as_array()
        .ok_or_else(|| fail("clip plan unbound missing"))?
    {
        let profile = entry["profile"]
            .as_str()
            .ok_or_else(|| fail("clip plan profile missing"))?;
        let command = entry["command"]
            .as_u64()
            .ok_or_else(|| fail("clip plan command missing"))? as usize;
        let list = serde_json::from_value::<Vec<Value>>(entry["actions"].clone())?;
        actions.unbound.insert(
            (profile.to_owned(), command),
            list.iter()
                .map(decode_key_action)
                .collect::<CoreResult<_>>()?,
        );
    }
    for entry in value["order"]
        .as_array()
        .ok_or_else(|| fail("clip plan order missing"))?
    {
        let motion = entry["motion"]
            .as_str()
            .ok_or_else(|| fail("clip plan order motion missing"))?;
        actions
            .order
            .push((motion.to_owned(), decode_key_action(&entry["action"])?));
    }
    Ok(CharacterPlan { profiles, actions })
}

fn decode_key_action(value: &Value) -> CoreResult<super::actions::KeyAction> {
    Ok(super::actions::KeyAction {
        game: value["game"].clone(),
        start: value["start"]
            .as_u64()
            .ok_or_else(|| fail("clip action start missing"))?,
        end: value["end"]
            .as_u64()
            .ok_or_else(|| fail("clip action end missing"))?,
    })
}

/// Character animation plans decoded from the external gameplay archive.
static CHARACTER_PLANS: OnceLock<BTreeMap<String, CharacterPlan>> = OnceLock::new();

/// Initializes model-slicing metadata from a verified external gameplay document.
pub fn initialize_character_plans(documents: &[(&str, Vec<u8>)]) -> CoreResult<()> {
    if CHARACTER_PLANS.get().is_some() {
        return Ok(());
    }
    let Some((name, bytes)) = documents.iter().find(|(name, _)| *name == CHARACTER_CLIPS) else {
        if gameplay_digest(CHARACTER_CLIPS).is_none() {
            let _ = CHARACTER_PLANS.set(BTreeMap::new());
            return Ok(());
        }
        return Err(fail("gameplay character clip plan missing"));
    };
    verify_gameplay_document(name, bytes)?;
    let document: Value = serde_json::from_slice(bytes)?;
    if document["schema_version"] != 1 || document["kind"] != "character_clips" {
        return Err(fail("unsupported character clip plan"));
    }
    let plans = document["characters"]
        .as_object()
        .ok_or_else(|| fail("character clip plans missing"))?
        .iter()
        .map(|(alias, value)| Ok((alias.clone(), decode_character_plan(value)?)))
        .collect::<CoreResult<BTreeMap<_, _>>>()?;
    let _ = CHARACTER_PLANS.set(plans);
    Ok(())
}

/// Returns the character action plan loaded from the external gameplay archive.
pub(crate) fn profile_actions_for(
    character_alias: &str,
) -> CoreResult<Option<&'static CharacterPlan>> {
    if CHARACTER_PLANS.get().is_none() {
        // A game that tracks no plan needs no seed to say so.
        if gameplay_digest(CHARACTER_CLIPS).is_none() {
            let _ = CHARACTER_PLANS.set(BTreeMap::new());
        } else {
            initialize_character_plans(&gameplay_documents(None)?)?;
        }
    }
    let plans = CHARACTER_PLANS
        .get()
        .ok_or_else(|| fail("character clip plans not initialized"))?;
    Ok(plans.get(character_alias))
}

fn encode_key_action(action: &super::actions::KeyAction) -> Value {
    json!({"game":action.game,"start":action.start,"end":action.end})
}

fn collect_character_profiles(
    raw: &[u8],
    alias: &str,
    depth: usize,
    profiles: &mut BTreeMap<String, Vec<(String, Value)>>,
) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("character PACK nesting exceeds 16"));
    }
    if is_cfg(alias) {
        if let Some(character) = character_container(alias) {
            profiles
                .entry(character)
                .or_default()
                .push((alias.to_owned(), parse(raw)?));
        }
        return Ok(());
    }
    if let Some(members) = pack(raw) {
        let mut names = BTreeSet::new();
        for (index, member) in members.iter().enumerate() {
            if member.size == 0 && !is_cfg(&member.name) {
                continue;
            }
            let child = if names.insert(member.name.clone()) {
                format!("{alias}/{}", member.name)
            } else {
                format!("{alias}/{}#entry:{index}", member.name)
            };
            collect_character_profiles(
                slice(raw, member.start, member.size)?,
                &child,
                depth + 1,
                profiles,
            )?;
        }
    }
    Ok(())
}

/// Derives only model-slicing metadata from retail profiles in explicit gameplay refresh mode.
pub fn character_clip_document(source: &SourceTree) -> CoreResult<Value> {
    let mut profiles = BTreeMap::<String, Vec<(String, Value)>>::new();
    for name in source.names() {
        if source.len(name)? <= 128 << 20 {
            collect_character_profiles(&source.read(name)?, name, 0, &mut profiles)?;
        }
    }
    let mut characters = serde_json::Map::new();
    for (character, entries) in profiles {
        let actions = super::actions::key_actions(&entries)?;
        let motions: Vec<Value> = actions.motions.iter().map(|(motion, list)| json!({"motion":motion,"actions":list.iter().map(encode_key_action).collect::<Vec<_>>()})).collect();
        let unbound: Vec<Value> = actions.unbound.iter().map(|((profile, command), list)| json!({"profile":profile,"command":command,"actions":list.iter().map(encode_key_action).collect::<Vec<_>>()})).collect();
        let order: Vec<Value> = actions
            .order
            .iter()
            .map(|(motion, action)| json!({"motion":motion,"action":encode_key_action(action)}))
            .collect();
        let names: Vec<_> = entries.into_iter().map(|(name, _)| name).collect();
        characters.insert(
            character,
            json!({"profiles":names,"motions":motions,"unbound":unbound,"order":order}),
        );
    }
    // A character PACK without a CFG still needs an explicit empty slicing plan.
    let inventory = super::inventory::Inventory::build(source)?;
    for alias in inventory.names() {
        let extension = alias
            .split("#entry:")
            .next()
            .unwrap_or(alias)
            .rsplit('.')
            .next()
            .unwrap_or("");
        if extension.trim_end_matches('_').eq_ignore_ascii_case("chr") {
            characters
                .entry(alias.to_owned())
                .or_insert_with(|| json!({"profiles":[],"motions":[],"unbound":[],"order":[]}));
        }
    }
    Ok(json!({"schema_version":1,"kind":"character_clips","characters":characters}))
}

/// Writes the checked-in, human-readable clip plan in explicit gameplay refresh mode.
pub fn export_character_clip_plan(source: &SourceTree, destination: &Path) -> CoreResult<usize> {
    let document = character_clip_document(source)?;
    let mut bytes = serde_json::to_vec_pretty(&document)?;
    bytes.push(b'\n');
    fs::write(destination, bytes)?;
    Ok(document["characters"].as_object().unwrap().len())
}

/// Compares the checked-in plan with all original KEY intervals before asset extraction is reused.
pub fn audit_character_clip_plan(source: &SourceTree, destination: &Path) -> CoreResult<usize> {
    let actual = character_clip_document(source)?;
    let expected: Value = serde_json::from_slice(&fs::read(destination)?)?;
    if actual != expected {
        return Err(fail("character clip plan differs from PAL source"));
    }
    Ok(actual["characters"].as_object().unwrap().len())
}

fn fixture_walk(
    raw: &[u8],
    alias: &str,
    depth: usize,
    identities: &mut BTreeSet<String>,
    ordered: &mut Vec<String>,
    scripts: &mut Vec<(String, Vec<u8>)>,
    resources: &mut BTreeMap<String, BTreeMap<&'static str, Vec<String>>>,
) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("CFG parity PACK nesting exceeds 16"));
    }
    identities.insert(alias.to_owned());
    ordered.push(alias.to_owned());
    if is_cfg(alias) && !super::audio::owns_config(alias) {
        scripts.push((alias.to_owned(), raw.to_vec()));
        return Ok(());
    }
    let Some(members) = pack(raw) else {
        return Ok(());
    };
    let character = alias
        .to_ascii_lowercase()
        .trim_end_matches('_')
        .ends_with(".chr");
    if character {
        resources.entry(alias.to_owned()).or_default();
    }
    let mut names = BTreeSet::new();
    for (index, member) in members.iter().enumerate() {
        let child = if names.insert(member.name.clone()) {
            format!("{alias}/{}", member.name)
        } else {
            format!("{alias}/{}#entry:{index}", member.name)
        };
        if character && member.size > 0 {
            let kind = match member
                .name
                .rsplit_once('.')
                .map(|pair| pair.1.to_ascii_lowercase())
                .as_deref()
            {
                Some("mot") => Some("animation"),
                Some("bbp") => Some("bind_pose"),
                Some("wgt") => Some("weights"),
                Some("clo") => Some("cloth"),
                Some("stb") => Some("script"),
                Some("chr") => Some("character"),
                _ => None,
            };
            if let Some(kind) = kind {
                resources
                    .get_mut(alias)
                    .unwrap()
                    .entry(kind)
                    .or_default()
                    .push(child.clone());
            }
        }
        if member.size > 0 || is_cfg(&child) {
            fixture_walk(
                slice(raw, member.start, member.size)?,
                &child,
                depth + 1,
                identities,
                ordered,
                scripts,
                resources,
            )?;
        }
    }
    Ok(())
}

fn resolve_gltf_uri(container: &str, uri: &str) -> CoreResult<String> {
    let mut components: Vec<&str> = container
        .rsplit_once('/')
        .map_or("", |(parent, _)| parent)
        .split('/')
        .filter(|part| !part.is_empty())
        .collect();
    for part in uri.split('/') {
        match part {
            "" | "." => {}
            ".." => {
                components
                    .pop()
                    .ok_or_else(|| fail(format!("glTF URI escapes data root: {uri}")))?;
            }
            _ => components.push(part),
        }
    }
    let resolved = components.join("/");
    super::zip_archive::validate_name(&resolved)?;
    Ok(resolved)
}

fn manifest_path(path: &Path) -> String {
    let text = path.to_string_lossy();
    text.strip_prefix(r"\\?\").unwrap_or(&text).to_owned()
}

/// Creates a small private parity fixture directly from ISO extents and converted ZIP entries.
pub fn export_parity_fixture(
    source: &SourceTree,
    archive_path: &Path,
    config_root: &Path,
    destination: &Path,
    texture_oracle: Option<&Path>,
) -> CoreResult<usize> {
    let archive = super::zip_archive::Archive::open(archive_path)?;
    let indexed: BTreeMap<_, _> = (0..archive.len())
        .map(|index| (archive.name(index).to_owned(), index))
        .collect();
    let mut identities = BTreeSet::new();
    let mut ordered = Vec::new();
    let mut scripts = Vec::new();
    let mut resources = BTreeMap::new();
    for alias in source.names() {
        identities.insert(alias.to_owned());
        if source.len(alias)? <= 128 << 20 {
            fixture_walk(
                &source.read(alias)?,
                alias,
                0,
                &mut identities,
                &mut ordered,
                &mut scripts,
                &mut resources,
            )?;
        }
    }
    for (container, kinds) in &mut resources {
        let scripts_for_container = kinds.entry("script").or_default();
        for alias in &ordered {
            if alias.starts_with(&format!("{container}/"))
                && alias.to_ascii_lowercase().ends_with(".stb")
                && !scripts_for_container.contains(alias)
            {
                scripts_for_container.push(alias.clone());
            }
        }
    }
    let parent = destination
        .parent()
        .ok_or_else(|| fail("parity destination has no parent"))?;
    fs::create_dir_all(parent)?;
    let destination = parent.canonicalize()?.join(
        destination
            .file_name()
            .ok_or_else(|| fail("parity destination has no name"))?,
    );
    let nonce = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|err| fail(format!("system time: {err}")))?
        .as_nanos();
    let staging = parent.join(format!(".cfg-parity-{}-{nonce}", std::process::id()));
    fs::create_dir(&staging)?;
    let result = (|| -> CoreResult<usize> {
        let data = staging.join("data");
        let originals = staging.join("originals");
        fs::create_dir_all(&originals)?;
        let mut needed = BTreeSet::new();
        let mut entries = Vec::new();
        for (alias, raw) in &scripts {
            if super::configs_native::unused(alias) {
                continue;
            }
            let original =
                originals.join(format!("{}.cfg", hex(&Sha256::digest(alias.as_bytes()))));
            fs::write(&original, raw).map_err(|err| {
                fail(format!(
                    "writing parity original {}: {err}",
                    original.display()
                ))
            })?;
            let final_original = destination
                .join("originals")
                .join(original.file_name().unwrap());
            let mut entry = json!({"alias":alias,"original":manifest_path(&final_original)});
            if let Some(container) = character_container(alias) {
                let gltf = native_path(&container, "gltf");
                let game = native_path(&container, "game.json");
                let profile = alias
                    .rsplit('/')
                    .next()
                    .unwrap()
                    .rsplit_once('.')
                    .map_or(alias.as_str(), |p| p.0);
                entry["gltf"] = json!(gltf);
                entry["game"] = json!(game);
                entry["profile"] = json!(profile);
                needed.insert(gltf);
                needed.insert(game);
            } else {
                let path = output_path(alias);
                if !config_root.join(&path).is_file() {
                    return Err(fail(format!("missing tracked CFG {path}")));
                }
                entry["json"] = json!(path);
            }
            entries.push(entry);
        }
        let mut copied = BTreeSet::new();
        while let Some(path) = needed.pop_first() {
            if !copied.insert(path.clone()) {
                continue;
            }
            let index = *indexed
                .get(&path)
                .ok_or_else(|| fail(format!("missing converted parity asset {path}")))?;
            let bytes = archive.read(index)?;
            let target = data.join(&path);
            fs::create_dir_all(target.parent().unwrap()).map_err(|err| {
                fail(format!(
                    "creating parity asset directory {}: {err}",
                    target.parent().unwrap().display()
                ))
            })?;
            fs::write(&target, &bytes)
                .map_err(|err| fail(format!("writing parity asset {}: {err}", target.display())))?;
            if path.ends_with(".gltf") {
                let gltf: Value = serde_json::from_slice(&bytes)?;
                for collection in ["buffers", "images"] {
                    for resource in gltf[collection].as_array().into_iter().flatten() {
                        if let Some(uri) = resource["uri"]
                            .as_str()
                            .filter(|uri| !uri.starts_with("data:"))
                        {
                            let sibling = resolve_gltf_uri(&path, uri)?;
                            needed.insert(sibling);
                        }
                    }
                }
            }
        }
        let resource_values: serde_json::Map<String, Value> = resources
            .into_iter()
            .map(|(alias, kinds)| {
                let mut object = serde_json::Map::new();
                for kind in [
                    "animation",
                    "bind_pose",
                    "weights",
                    "cloth",
                    "script",
                    "character",
                ] {
                    object.insert(
                        kind.to_owned(),
                        json!(kinds.get(kind).cloned().unwrap_or_default()),
                    );
                }
                (alias, Value::Object(object))
            })
            .collect();
        let mut manifest = json!({
            "data":manifest_path(&destination.join("data")),
            "config":manifest_path(&config_root.canonicalize()?),
            "sources":entries, "identities":identities, "character_resources":resource_values,
        });
        if let Some(oracle) = texture_oracle {
            let mut documents = serde_json::Map::new();
            for alias in &identities {
                let lower = alias.to_ascii_lowercase();
                if !lower.ends_with(".img") && !lower.ends_with(".tm2") {
                    continue;
                }
                let path = oracle.join(super::textures::document_path(alias)?);
                if path.is_file() {
                    documents.insert(lower, json!(manifest_path(&path.canonicalize()?)));
                }
            }
            manifest["texture_documents"] = Value::Object(documents);
        }
        save(&staging.join("parity.json"), &manifest)
            .map_err(|err| fail(format!("writing parity manifest: {err}")))?;
        let count = manifest["sources"].as_array().unwrap().len();
        let backup = parent.join(format!(
            ".cfg-parity-previous-{}-{nonce}",
            std::process::id()
        ));
        if destination.exists() {
            fs::rename(&destination, &backup)
                .map_err(|err| fail(format!("moving prior parity fixture: {err}")))?;
        }
        if let Err(err) = fs::rename(&staging, &destination) {
            if backup.exists() {
                let _ = fs::rename(&backup, &destination);
            }
            return Err(fail(format!("publishing parity fixture: {err}")));
        }
        if backup.exists() {
            fs::remove_dir_all(backup)?;
        }
        Ok(count)
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(staging);
    }
    result
}

/// Publishes every direct or PACK-contained CFG and removes its compatibility payload.
pub fn convert_tree(raw: &Path, published: &Path, phase: &Phase) -> CoreResult<()> {
    let excluded = BTreeSet::new();
    let mut all = Vec::new();
    files(raw, &mut all)?;
    all.sort();
    phase.set_total(3 * all.len() as u64);
    let entries = all
        .iter()
        .map(|path| -> CoreResult<_> {
            let alias = path
                .strip_prefix(raw)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            Ok((path, alias, fs::metadata(path)?.len()))
        })
        .collect::<CoreResult<Vec<_>>>()?;
    let identity_sets: Vec<CoreResult<BTreeSet<String>>> = entries
        .par_iter()
        .map(|(path, alias, len)| -> CoreResult<BTreeSet<String>> {
            let mut set = BTreeSet::from([alias.to_ascii_lowercase()]);
            if *len <= 128 << 20 {
                inventory(&fs::read(path)?, alias, &mut set, 0)?;
            }
            phase.inc(1);
            Ok(set)
        })
        .collect();
    let mut identities = BTreeSet::new();
    for set in identity_sets {
        identities.extend(set?);
    }
    let planned = entries
        .par_iter()
        .map(|(path, alias, len)| -> CoreResult<_> {
            if *len > 128 << 20 {
                phase.inc(1);
                return Ok(None);
            }
            if *len >= 2 << 20 {
                phase.item(alias);
            }
            let mut out = Vec::new();
            let result = plan(&fs::read(path)?, alias, &excluded, &identities, 0, &mut out);
            phase.inc(1);
            Ok(Some((out, result)))
        })
        .collect::<Vec<_>>();
    let mut catalog = Catalog::default();
    for item in planned {
        let Some((planned, result)) = item? else {
            continue;
        };
        for entry in planned {
            catalog.record(entry, published)?;
        }
        result?;
    }
    catalog.commit()?;
    let consumed: BTreeSet<_> = catalog.aliases.keys().cloned().collect();
    let stripped: Vec<CoreResult<()>> = entries
        .par_iter()
        .map(|(path, alias, _)| -> CoreResult<()> {
            let target = published.join("data").join(path.strip_prefix(raw).unwrap());
            phase.inc(1);
            if consumed.contains(alias) {
                if target.is_file() {
                    fs::remove_file(target)?
                }
                return Ok(());
            }
            if target.is_file() && fs::metadata(&target)?.len() <= 128 << 20 {
                let mut b = fs::read(&target)?;
                if strip(&mut b, alias, &consumed, 0)? {
                    fs::write(target, b)?
                }
            }
            Ok(())
        })
        .collect();
    for result in stripped {
        result?;
    }

    Ok(())
}

fn validate_document(document: &Value) -> CoreResult<()> {
    if document["kind"] == "movie_subtitles" {
        if document["schema_version"] != super::configs_native::SCHEMA_VERSION {
            return Err(fail("unsupported subtitle document schema"));
        }
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
                if !entry["text"].is_string() {
                    return Err(fail("invalid subtitle text"));
                }
                if let Some(bytes) = entry.get("game_bytes") {
                    let hex = bytes
                        .as_str()
                        .ok_or_else(|| fail("invalid subtitle byte fallback"))?;
                    if !hex.len().is_multiple_of(2) || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
                        return Err(fail("invalid subtitle byte fallback"));
                    }
                }
            }
        }
        return Ok(());
    }
    super::configs_native::validate(document)
}
pub(crate) fn audit_payload(b: &[u8], alias: &str, depth: usize) -> CoreResult<()> {
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
/// Validates loose developer configuration documents when present.
pub(crate) fn verify_loose(root: &Path) -> CoreResult<()> {
    let port = root.join("config");
    if port.is_dir() {
        let mut all = Vec::new();
        files(&port, &mut all)?;
        all.sort();
        let checked: Vec<CoreResult<()>> = all
            .par_iter()
            .filter(|path| {
                let name = path.to_string_lossy().to_ascii_lowercase();
                name.ends_with(".cfg.json") || name.ends_with(".map.json")
            })
            .map(|path| -> CoreResult<()> {
                let doc: Value = serde_json::from_slice(&fs::read(path)?)?;
                validate_document(&doc).map_err(|e| fail(format!("{}: {e}", path.display())))
            })
            .collect();
        for result in checked {
            result?;
        }
    }
    Ok(())
}

/// Validates staged developer config documents and rejects retained retail CFG payloads.
pub fn verify(root: &Path) -> CoreResult<()> {
    verify_loose(root)?;
    let data = root.join("data");
    if data.is_dir() {
        let mut all = Vec::new();
        files(&data, &mut all)?;
        all.sort();
        let audited: Vec<CoreResult<()>> = all
            .par_iter()
            .map(|path| -> CoreResult<()> {
                if fs::metadata(path)?.len() > 128 << 20 {
                    return Ok(());
                }
                let alias = path
                    .strip_prefix(&data)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                if alias.ends_with(".cfg.json") || alias.ends_with(".map.json") {
                    return Err(fail(format!(
                        "native config belongs outside data archive: {alias}"
                    )));
                }
                audit_payload(&fs::read(path)?, &alias, 0)
            })
            .collect();
        for result in audited {
            result?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn published_gameplay_tree_holds_the_build_documents_and_drops_stale_ones() {
        let temp = tempfile::tempdir().unwrap();
        let stale = temp.path().join("gameplay/gone/old.json");
        fs::create_dir_all(stale.parent().unwrap()).unwrap();
        fs::write(&stale, b"{}").unwrap();
        let notes = temp.path().join("gameplay/notes.txt");
        fs::write(&notes, b"kept").unwrap();
        let written = publish_gameplay_tree(temp.path()).unwrap();
        assert_eq!(written, gameplay_manifest().len());
        for (name, digest) in gameplay_manifest() {
            let bytes = fs::read(temp.path().join(name)).unwrap();
            assert_eq!(Sha256::digest(&bytes).as_slice(), digest);
        }
        assert!(!stale.exists());
        assert!(notes.exists());
    }

    #[test]
    fn archived_gameplay_preserves_source_decimal() {
        let documents = gameplay_documents(None).unwrap();
        let Some((_, document)) = documents
            .iter()
            .find(|(name, _)| *name == "gameplay/chara/e3_donnie.character.json")
        else {
            // Another game's seed, or none.
            return;
        };
        let text = std::str::from_utf8(document).unwrap();
        assert!(text.contains("\"value\":0.029999999329447746"));
    }

    #[test]
    fn gameplay_archive_requires_namespace_and_matching_contents() {
        let Some(&(name, _)) = gameplay_manifest().first() else {
            return;
        };
        assert!(name.starts_with("gameplay/"));
        assert!(gameplay_digest(name.strip_prefix("gameplay/").unwrap()).is_none());
        assert!(verify_gameplay_document(name, b"{}").is_err());
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("input");
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("unrelated.json"), b"{}").unwrap();
        let archive = temp.path().join("data.zip");
        super::super::zip_archive::pack_directory(&root, &archive).unwrap();
        assert!(
            read_gameplay_archive(&archive)
                .unwrap_err()
                .to_string()
                .contains("gameplay document missing")
        );
    }

    fn record_alias(
        catalog: &mut Catalog,
        raw: &[u8],
        alias: &str,
        published: &Path,
    ) -> CoreResult<()> {
        let mut out = Vec::new();
        plan(raw, alias, &BTreeSet::new(), &BTreeSet::new(), 0, &mut out)?;
        for planned in out {
            catalog.record(planned, published)?;
        }
        Ok(())
    }
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
        let mut doc=parse(b"MDS \"room.mds\"; NAME \"room.mds\"; CHARA \"chara/hero.chr\",\"info.cfg\"; MOVIE 3,\"OPEN.PSS\"; MOVIE 9,\"TUTO\\\\VTUTO1.PSS\";").unwrap();
        migrate_references(
            &mut doc,
            "map/room/info.cfg",
            &BTreeSet::from([
                "map/room/room.mds".into(),
                "chara/hero.chr".into(),
                "movie/open.pss".into(),
                "movie/tuto/vtuto1.pss".into(),
            ]),
        );
        assert_eq!(doc["commands"][0]["args"][0]["value"], "room.gltf");
        assert_eq!(doc["commands"][1]["args"][0]["value"], "room.mds");
        assert_eq!(doc["commands"][2]["args"][0]["value"], "chara/hero.gltf");
        assert_eq!(doc["commands"][2]["args"][1]["value"], "info");
        assert_eq!(doc["commands"][3]["args"][1]["value"], "OPEN.mp4");
        assert_eq!(doc["commands"][4]["args"][1]["value"], "TUTO/VTUTO1.mp4");
    }
    #[test]
    fn generated_map_and_sky_texture_names_remain_logical_pack_members() {
        let identities = BTreeSet::from([
            "map/a/a01/a01ia.img".into(),
            "map/d/d04/b01/d04b01.sky/sky0.img".into(),
        ]);
        let mut map = parse(br#"IMG "a01ia.img";"#).unwrap();
        migrate_references(&mut map, "map/a/a01/a01ia.map", &identities);
        let (map, _) = super::super::configs_native::build("map/a/a01/a01ia.map", &map).unwrap();
        assert_eq!(map["info"]["texture_packs"], json!(["a01ia.json"]));
        let mut sky = parse(br#"SKY_IMG 0,"sky0.img";"#).unwrap();
        migrate_references(&mut sky, "map/d/d04/b01/d04b01.sky/info.cfg", &identities);
        let (sky, _) =
            super::super::configs_native::build("map/d/d04/b01/d04b01.sky/info.cfg", &sky).unwrap();
        assert_eq!(sky["images"], json!([{"band":0,"file":"sky0.json"}]));
        assert_eq!(
            native_path("map/a/a01/a01ia.img", "json"),
            "textures/map/a/a01/a01ia.json"
        );
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
    fn map_and_cfg_scripts_keep_distinct_physical_paths_and_kinds() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let output = temp.path().join("out");
        fs::create_dir_all(raw.join("map/r")).unwrap();
        fs::write(raw.join("map/r/room.cfg"), b"WATER_SURFACE_NUM 1;").unwrap();
        fs::write(raw.join("map/r/room.map"), b"PARTS \"p\"; PARTS_END;").unwrap();
        convert_tree(&raw, &output, &Phase::noop()).unwrap();
        let water: Value =
            serde_json::from_slice(&fs::read(output.join("config/map/r/room.cfg.json")).unwrap())
                .unwrap();
        let map: Value =
            serde_json::from_slice(&fs::read(output.join("config/map/r/room.map.json")).unwrap())
                .unwrap();
        assert_eq!(water["kind"], "scene_config");
        assert_eq!(water["water"]["surface_count"], 1);
        assert_eq!(map["kind"], "scene_map");
        assert_eq!(map["parts"][0]["name"], "p");
        assert!(water.get("commands").is_none() && map.get("commands").is_none());
        verify(&output).unwrap();
        assert_eq!(
            native_path("outer.pac/room.MAP#entry:2", "json"),
            "outer.pac/room.entry_2.map.json"
        );
    }

    #[test]
    fn unused_retail_scripts_are_consumed_without_a_document() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let output = temp.path().join("out");
        fs::create_dir_all(&raw).unwrap();
        fs::write(raw.join("edit.cfg"), b"CHARA \"a.chr\";").unwrap();
        convert_tree(&raw, &output, &Phase::noop()).unwrap();
        assert!(!output.join("config/edit.cfg.json").exists());
        assert!(!output.join("data/edit.cfg").exists());
    }

    #[test]
    fn scripts_no_family_claims_fail_conversion() {
        let mut out = Vec::new();
        let error = plan(
            b"TAG 1;",
            "room.cfg",
            &BTreeSet::new(),
            &BTreeSet::new(),
            0,
            &mut out,
        )
        .unwrap_err()
        .to_string();
        assert!(error.contains("no configuration family"), "{error}");
    }

    #[test]
    fn verify_rejects_command_documents() {
        let temp = tempfile::tempdir().unwrap();
        fs::create_dir_all(temp.path().join("config")).unwrap();
        fs::write(
            temp.path().join("config/old.cfg.json"),
            br#"{"schema_version":1,"commands":[]}"#,
        )
        .unwrap();
        assert!(verify(temp.path()).is_err());
    }

    #[test]
    fn unresolved_configuration_output_collision_fails_before_overwrite() {
        let temp = tempfile::tempdir().unwrap();
        let mut catalog = Catalog::default();
        record_alias(&mut catalog, b"SHOP 1;", "menu/shoplst1.cfg", temp.path()).unwrap();
        catalog.commit().unwrap();
        let error = record_alias(&mut catalog, b"SHOP 2;", "MENU/SHOPLST1.CFG", temp.path())
            .unwrap_err()
            .to_string();
        assert!(error.contains("collision"), "{error}");
        let first: Value = serde_json::from_slice(
            &fs::read(temp.path().join("config/menu/shoplst1.cfg.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(first["shops"][0]["id"], 1);
    }

    #[test]
    fn configuration_cannot_replace_a_texture_or_other_converted_asset() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let output = temp.path().join("out");
        fs::create_dir_all(raw.join("map")).unwrap();
        fs::create_dir_all(output.join("config/map")).unwrap();
        fs::write(raw.join("map/room.cfg"), b"WATER_SURFACE_NUM 1;").unwrap();
        fs::write(
            output.join("config/map/room.cfg.json"),
            b"earlier converted texture",
        )
        .unwrap();
        assert!(
            convert_tree(&raw, &output, &Phase::noop())
                .unwrap_err()
                .to_string()
                .contains("collision")
        );
        assert_eq!(
            fs::read(output.join("config/map/room.cfg.json")).unwrap(),
            b"earlier converted texture"
        );
    }

    #[test]
    fn explicit_config_refresh_preserves_unrelated_files() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let destination = temp.path().join("config");
        fs::create_dir_all(raw.join("menu")).unwrap();
        fs::create_dir_all(&destination).unwrap();
        fs::write(raw.join("menu/shoplst1.cfg"), b"SHOP 1;").unwrap();
        fs::write(destination.join("README.md"), b"developer notes").unwrap();
        fs::write(destination.join("stale.cfg.json"), b"{}").unwrap();
        let source = SourceTree::directory(&raw).unwrap();
        assert_eq!(export_source(&source, &destination).unwrap(), 1);
        assert_eq!(
            fs::read(destination.join("README.md")).unwrap(),
            b"developer notes"
        );
        assert!(!destination.join("stale.cfg.json").exists());
        let native: Value =
            serde_json::from_slice(&fs::read(destination.join("menu/shoplst1.cfg.json")).unwrap())
                .unwrap();
        assert_eq!(native["kind"], "shop_lists");
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

    /// Calls `visit` with every direct or PACK-contained CFG under a raw tree.
    fn walk_cfgs(raw: &[u8], alias: &str, depth: usize, visit: &mut dyn FnMut(&str, &[u8])) {
        if depth > 16 {
            return;
        }
        if is_cfg(alias) {
            visit(alias, raw);
            return;
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
                walk_cfgs(&raw[m.start..m.start + m.size], &child, depth + 1, visit);
            }
        }
    }

    #[test]
    #[ignore = "requires extracted retail assets in KZ_CFG_RAW"]
    fn retail_cfg_family_report() {
        // Reading the raw tree dominates; KZ_CFG_CACHE keeps the migrated command lists between runs.
        let cache = std::env::var("KZ_CFG_CACHE").ok().map(PathBuf::from);
        let mut documents: Vec<(String, Value)> = Vec::new();
        if let Some(cached) = cache.as_ref().filter(|p| p.is_file()) {
            let value: Value = serde_json::from_slice(&fs::read(cached).unwrap()).unwrap();
            for item in value.as_array().unwrap() {
                documents.push((item[0].as_str().unwrap().into(), item[1].clone()));
            }
        } else {
            let raw = PathBuf::from(std::env::var("KZ_CFG_RAW").expect("KZ_CFG_RAW"));
            let mut all = Vec::new();
            files(&raw, &mut all).unwrap();
            all.sort();
            let mut identities = BTreeSet::new();
            let mut scripts = Vec::new();
            for path in &all {
                let alias = path
                    .strip_prefix(&raw)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                identities.insert(alias.to_ascii_lowercase());
                if fs::metadata(path).unwrap().len() > 128 << 20 {
                    continue;
                }
                let bytes = fs::read(path).unwrap();
                inventory(&bytes, &alias, &mut identities, 0).unwrap();
                walk_cfgs(&bytes, &alias, 0, &mut |alias, data| {
                    if !in_character(alias) && !super::super::audio::owns_config(alias) {
                        scripts.push((alias.to_owned(), data.to_vec()));
                    }
                });
            }
            for (alias, data) in &scripts {
                let mut document = parse(data).unwrap();
                migrate_references(&mut document, alias, &identities);
                documents.push((alias.clone(), document));
            }
            if let Some(cached) = &cache {
                let value: Vec<Value> = documents.iter().map(|(a, d)| json!([a, d])).collect();
                fs::write(cached, serde_json::to_vec(&value).unwrap()).unwrap();
            }
        }
        let mut kinds = BTreeMap::<String, usize>::new();
        let mut failures = Vec::new();
        let mut stale = Vec::new();
        let mut dropped = BTreeMap::<String, usize>::new();
        let filter = std::env::var("KZ_CFG_FILTER").unwrap_or_default();
        for (alias, document) in &documents {
            if !alias.contains(filter.as_str()) {
                continue;
            }
            if super::super::configs_native::unused(alias) {
                *kinds.entry("(unused)".into()).or_default() += 1;
                continue;
            }
            if document["kind"] == "movie_subtitles" {
                *kinds.entry("movie_subtitles".into()).or_default() += 1;
                continue;
            }
            match super::super::configs_native::build(alias, document) {
                Ok((native, report)) => {
                    *kinds
                        .entry(native["kind"].as_str().unwrap().into())
                        .or_default() += 1;
                    for s in report.stale {
                        stale.push(format!("{alias}: {s}"));
                    }
                    for d in report.dropped {
                        *dropped.entry(d).or_default() += 1;
                    }
                    if let Ok(out) = std::env::var("KZ_CFG_DUMP") {
                        let target = PathBuf::from(out).join(output_path(alias));
                        save(&target, &native).unwrap();
                    }
                }
                Err(error) => failures.push(error.to_string()),
            }
        }
        println!("{} scripts", documents.len());
        for (kind, count) in &kinds {
            println!("kind {kind}: {count}");
        }
        for (name, count) in &dropped {
            println!("dropped {name}: {count}");
        }
        stale.sort();
        stale.dedup();
        for s in &stale {
            println!("stale {s}");
        }
        for f in &failures {
            println!("FAIL {f}");
        }
        assert!(failures.is_empty(), "{} scripts failed", failures.len());
    }

    #[test]
    #[ignore = "requires extracted retail assets in KZ_CFG_RAW and isolated KZ_CFG_OUTPUT"]
    fn retail_cfg_audit() {
        let raw = PathBuf::from(std::env::var("KZ_CFG_RAW").expect("KZ_CFG_RAW"));
        let output = PathBuf::from(std::env::var("KZ_CFG_OUTPUT").expect("KZ_CFG_OUTPUT"));
        fs::create_dir_all(output.join("config")).unwrap();
        if let Some(audio) = std::env::var_os("KZ_CFG_AUDIO_CATALOG") {
            fs::copy(audio, output.join("config/audio_catalog.json")).unwrap();
        }
        convert_tree(&raw, &output, &Phase::noop()).unwrap();
        verify(&output).unwrap();
    }
}
