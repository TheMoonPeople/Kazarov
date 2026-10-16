//! Migrates compiled-script resource operands to directly published native files.
use super::{CoreError, CoreResult};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::Path;

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}
fn word(b: &[u8], at: usize) -> CoreResult<i32> {
    Ok(i32::from_le_bytes(
        b.get(at..at + 4)
            .ok_or_else(|| fail("truncated script word"))?
            .try_into()
            .unwrap(),
    ))
}
fn offset(value: i32) -> CoreResult<usize> {
    usize::try_from(value).map_err(|_| fail("negative script offset"))
}
#[derive(Clone, Copy)]
struct Instruction {
    at: usize,
    op: i32,
    a1: i32,
    a2: i32,
}
fn function(b: &[u8], code: usize, block: usize) -> CoreResult<Vec<Instruction>> {
    if block < 28 || block % 4 != 0 {
        return Err(fail("invalid script function block"));
    }
    let local = word(b, block + 8)?;
    let args = word(b, block + 12)?;
    if args < 0 || args > local {
        return Err(fail("invalid script frame"));
    }
    let mut at = code
        .checked_add(offset(word(b, block)?)?)
        .ok_or_else(|| fail("script code overflow"))?;
    if at % 4 != 0 {
        return Err(fail("unaligned script code"));
    }
    let mut farthest = at;
    let mut out = Vec::new();
    loop {
        let insn = Instruction {
            at,
            op: word(b, at)?,
            a1: word(b, at + 4)?,
            a2: word(b, at + 8)?,
        };
        if !(1..=30).contains(&insn.op) {
            return Err(fail("invalid script opcode"));
        }
        if matches!(insn.op, 16..=18) {
            let target = code
                .checked_add(offset(insn.a1)?)
                .ok_or_else(|| fail("script jump overflow"))?;
            if target % 4 != 0 || target + 12 > b.len() {
                return Err(fail("script jump out of range"));
            }
            farthest = farthest.max(target);
        }
        if insn.op == 19
            && code
                .checked_add(offset(insn.a2)?)
                .is_none_or(|n| n + 16 > b.len())
        {
            return Err(fail("script call out of range"));
        }
        out.push(insn);
        at += 12;
        if matches!(insn.op, 15 | 27) && at > farthest {
            break;
        }
    }
    Ok(out)
}
fn decode(b: &[u8]) -> CoreResult<(usize, Vec<Vec<Instruction>>)> {
    if b.len() < 28 || !matches!(b.get(..3), Some(b"SB2" | b"STB")) {
        return Err(fail("invalid compiled script header"));
    }
    let code = offset(word(b, 8)?)?;
    let prog = offset(word(b, 12)?)?;
    let count = offset(word(b, 16)?)?;
    if code < 28
        || code >= b.len()
        || code % 4 != 0
        || prog < 28
        || count > 4096
        || prog + count * 8 > b.len()
    {
        return Err(fail("invalid compiled script sections"));
    }
    let mut todo = vec![offset(word(b, 4)?)?];
    for i in 0..count {
        todo.push(offset(word(b, prog + i * 8 + 4)?)?);
    }
    let mut seen = BTreeSet::new();
    let mut covered = vec![false; b.len()];
    let mut functions = Vec::new();
    let mut next = 0;
    while next < todo.len() {
        let block = todo[next];
        next += 1;
        if !seen.insert(block) {
            continue;
        }
        let instructions = function(b, code, block)?;
        for insn in &instructions {
            if insn.op == 19 {
                todo.push(code + offset(insn.a2)?);
            }
        }
        covered[instructions[0].at..instructions.last().unwrap().at + 12].fill(true);
        functions.push(instructions);
    }
    let mut at = code;
    while at + 0x38 + 12 <= b.len() {
        if word(b, at)? as usize == at - code + 0x38 && !covered[at] && !seen.contains(&at) {
            let local = word(b, at + 8)?;
            let args = word(b, at + 12)?;
            if args >= 0 && args <= local && local < 256 && (1..=30).contains(&word(b, at + 0x38)?)
            {
                if let Ok(instructions) = function(b, code, at) {
                    seen.insert(at);
                    covered[instructions[0].at..instructions.last().unwrap().at + 12].fill(true);
                    at = instructions.last().unwrap().at + 8;
                    functions.push(instructions);
                }
            }
        }
        at += 4;
    }
    Ok((code, functions))
}
fn literal<'a>(b: &'a [u8], code: usize, insn: &Instruction) -> CoreResult<&'a [u8]> {
    let at = code
        .checked_add(offset(insn.a2)?)
        .ok_or_else(|| fail("script string overflow"))?;
    let tail = b
        .get(at..)
        .ok_or_else(|| fail("script string out of range"))?;
    let end = tail
        .iter()
        .position(|v| *v == 0)
        .ok_or_else(|| fail("unterminated script string"))?;
    Ok(&tail[..end])
}
/// Recognizes complete compiled VM scripts before untagged archive heuristics inspect their headers.
pub(crate) fn is_valid_script(bytes: &[u8]) -> bool {
    if !matches!(bytes.get(..4), Some(b"SB2\0" | b"STB\0")) {
        return false;
    }
    let Ok((code, functions)) = decode(bytes) else {
        return false;
    };
    functions
        .iter()
        .flatten()
        .filter(|instruction| instruction.op == 3 && instruction.a1 == 3)
        .all(|instruction| literal(bytes, code, instruction).is_ok())
}
#[cfg(test)]
pub(crate) fn ambiguous_script_fixture() -> Vec<u8> {
    let mut bytes = vec![0; 700];
    bytes[..4].copy_from_slice(b"SB2\0");
    for (at, value) in [
        (4, 80i32),
        (8, 80),
        (12, 64),
        (16, 2),
        (64, 500),
        (68, 80),
        (72, 150),
        (76, 80),
        (80, 56),
        (136, 15),
    ] {
        bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    bytes
}
fn normalize(value: &str) -> String {
    let value = value.replace('\\', "/").to_ascii_lowercase();
    let value = value
        .split_once(':')
        .map_or(value.as_str(), |(_, rest)| rest);
    value
        .trim_start_matches('/')
        .trim_end_matches(";1")
        .to_owned()
}
struct Inventory {
    paths: BTreeMap<String, String>,
    leaves: BTreeMap<String, BTreeSet<String>>,
    profiles: BTreeMap<String, BTreeSet<String>>,
}
impl Inventory {
    fn resource(&self, text: &str) -> Option<String> {
        let key = normalize(text);
        if let Some(path) = self.paths.get(&key) {
            return Some(path.clone());
        }
        if let Some((stem, extension)) = key.rsplit_once('.') {
            let trimmed = stem.trim_end_matches(|character: char| character.is_ascii_whitespace());
            if trimmed.len() != stem.len() {
                let normalized = format!("{trimmed}.{extension}");
                if let Some(path) = self.paths.get(&normalized) {
                    return Some(path.clone());
                }
            }
        }
        if key.contains('/') {
            return None;
        }
        let candidates = self.leaves.get(&key)?;
        if candidates.len() != 1 {
            return None;
        }
        Some(candidates.first()?.clone())
    }
    fn profile(&self, text: &str, character: Option<&str>) -> Option<String> {
        let key = normalize(text);
        if key.contains('/') || !key.ends_with(".cfg") {
            return None;
        }
        let stem = key.trim_end_matches(".cfg");
        if let Some(character) = character {
            if self
                .profiles
                .get(&normalize(character))
                .is_some_and(|p| p.contains(stem))
            {
                return Some(stem.to_owned());
            }
        }
        None
    }
}
#[derive(Clone, Copy)]
enum Token {
    Integer(i32),
    String(usize),
    Unknown,
}
fn profile_arguments(
    b: &[u8],
    code: usize,
    functions: &[Vec<Instruction>],
    inventory: &Inventory,
) -> CoreResult<BTreeSet<usize>> {
    let mut locations = BTreeSet::new();
    let profiles: BTreeSet<_> = inventory
        .profiles
        .values()
        .flat_map(|names| names.iter().cloned())
        .collect();
    for function in functions {
        let mut stack = Vec::new();
        for insn in function {
            match insn.op {
                1 | 2 => {
                    if matches!(insn.a2, 2 | 4 | 16 | 32) {
                        stack.pop();
                    }
                    stack.push(Token::Unknown);
                }
                3 => stack.push(match insn.a1 {
                    1 => Token::Integer(insn.a2),
                    3 => Token::String(insn.at),
                    _ => Token::Unknown,
                }),
                4 => {
                    stack.pop();
                }
                5 => {
                    let value = stack.pop().unwrap_or(Token::Unknown);
                    stack.pop();
                    stack.push(value);
                }
                6..=10 | 14 | 24 | 25 => {
                    stack.pop();
                    stack.pop();
                    stack.push(Token::Unknown);
                }
                11..=13 | 26 | 29 | 30 => {
                    stack.pop();
                    stack.push(Token::Unknown);
                }
                15..=18 | 27 => stack.clear(),
                19 => {
                    let block = code + offset(insn.a2)?;
                    let args = offset(word(b, block + 12)?)?;
                    stack.truncate(stack.len().saturating_sub(args));
                    stack.push(Token::Unknown);
                }
                20 => {
                    stack.pop();
                }
                21 => {
                    let count = offset(insn.a1)?;
                    if count <= stack.len() {
                        let start = stack.len() - count;
                        if matches!(
                            stack.get(start),
                            Some(Token::Integer(9 | 19 | 30 | 511 | 1048))
                        ) {
                            for token in &stack[start + 1..] {
                                if let Token::String(at) = token {
                                    let insn = Instruction {
                                        at: *at,
                                        op: 3,
                                        a1: 3,
                                        a2: word(b, *at + 8)?,
                                    };
                                    let bytes = literal(b, code, &insn)?;
                                    if bytes.is_ascii() {
                                        let text = normalize(std::str::from_utf8(bytes).unwrap());
                                        if !text.contains('/')
                                            && text.ends_with(".cfg")
                                            && profiles.contains(text.trim_end_matches(".cfg"))
                                        {
                                            locations.insert(*at);
                                        }
                                    }
                                }
                            }
                        }
                        stack.truncate(start);
                    } else {
                        stack.clear();
                    }
                }
                _ => {}
            }
        }
    }
    Ok(locations)
}
fn migrate(b: &[u8], inventory: &Inventory) -> CoreResult<(Vec<u8>, usize)> {
    let (code, functions) = decode(b)?;
    let profile_locations = profile_arguments(b, code, &functions, inventory)?;
    let mut replacements = BTreeMap::new();
    for function in functions {
        let mut previous_character: Option<(usize, String)> = None;
        for insn in function {
            if insn.op != 3 || insn.a1 != 3 {
                continue;
            }
            let bytes = literal(b, code, &insn)?;
            if !bytes.is_ascii() {
                continue;
            }
            let text = std::str::from_utf8(bytes).unwrap();
            let key = normalize(text);
            let character = previous_character
                .as_ref()
                .filter(|(at, _)| *at + 12 == insn.at)
                .map(|(_, name)| name.as_str());
            let replacement = if profile_locations.contains(&insn.at) {
                Some(key.trim_end_matches(".cfg").to_owned())
            } else {
                inventory
                    .profile(text, character)
                    .or_else(|| inventory.resource(text))
            };
            previous_character = if key.ends_with(".chr") {
                Some((insn.at, key))
            } else {
                None
            };
            if let Some(replacement) = replacement.filter(|v| v.as_bytes() != bytes) {
                replacements.insert(insn.at, replacement);
            }
        }
    }
    let mut out = b.to_vec();
    let mut strings = BTreeMap::new();
    for (at, text) in &replacements {
        let relative = if let Some(relative) = strings.get(text) {
            *relative
        } else {
            let relative = i32::try_from(out.len() - code)
                .map_err(|_| fail("migrated script exceeds VM offsets"))?;
            out.extend(text.as_bytes());
            out.push(0);
            strings.insert(text.clone(), relative);
            relative
        };
        out[*at + 8..*at + 12].copy_from_slice(&relative.to_le_bytes());
    }
    Ok((out, replacements.len()))
}
#[derive(Clone)]
struct Member {
    name: String,
    header: usize,
    start: usize,
    size: usize,
}
fn pack(b: &[u8]) -> Option<Vec<Member>> {
    if is_valid_script(b) {
        return None;
    }
    let mut members = Vec::new();
    let mut at = 0;
    let mut seen = BTreeSet::new();
    while seen.insert(at) && members.len() < 16384 {
        let entry = b.get(at..at + 76)?;
        let end = entry[..64].iter().position(|v| *v == 0)?;
        if end == 0 {
            return (!members.is_empty()).then_some(members);
        }
        if entry[..end]
            .iter()
            .any(|v| *v < 32 || matches!(*v, b'/' | b'\\'))
        {
            return None;
        }
        let start = at.checked_add(u32::from_le_bytes(entry[64..68].try_into().ok()?) as usize)?;
        let size = u32::from_le_bytes(entry[68..72].try_into().ok()?) as usize;
        let advance = u32::from_le_bytes(entry[72..76].try_into().ok()?) as usize;
        if start < at + 76 || start.checked_add(size)? > b.len() || advance < 76 {
            return None;
        }
        let name = entry[..end]
            .iter()
            .map(|v| {
                if v.is_ascii() {
                    (*v as char).to_ascii_lowercase().to_string()
                } else {
                    format!("%{v:02X}")
                }
            })
            .collect();
        members.push(Member {
            name,
            header: at,
            start,
            size,
        });
        at = at.checked_add(advance)?;
    }
    None
}
fn gather(dir: &Path, files: &mut Vec<std::path::PathBuf>) -> CoreResult<()> {
    for entry in fs::read_dir(dir)? {
        let p = entry?.path();
        if p.is_dir() {
            gather(&p, files)?;
        } else {
            files.push(p);
        }
    }
    Ok(())
}
fn inventory_resource(
    bytes: &[u8],
    alias: &str,
    assets: &Path,
    inventory: &mut Inventory,
    depth: usize,
) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("script resource archive nesting exceeds 16"));
    }
    let ext = Path::new(alias)
        .extension()
        .and_then(|v| v.to_str())
        .unwrap_or("");
    let extension = match ext {
        "chr" | "mds" | "mdt" | "mot" | "wgt" | "bbp" => Some("gltf"),
        "cfg" | "map" | "clo" | "img" | "tm2" => Some("json"),
        "snd" => Some("sound.json"),
        "pss" => Some("mp4"),
        "bin" if alias == "snd2/rev.bin" => Some("json"),
        "txt" if alias == "snd2/map_index.txt" => Some("json"),
        _ => None,
    };
    if let Some(extension) = extension {
        let path = super::configs::native_path(alias, extension);
        if assets.join(&path).is_file() {
            inventory.paths.insert(alias.to_owned(), path.clone());
            let leaf = Path::new(alias)
                .file_name()
                .unwrap()
                .to_string_lossy()
                .into_owned();
            let target = if ext == "pss" {
                path.clone()
            } else {
                Path::new(&path)
                    .file_name()
                    .unwrap()
                    .to_string_lossy()
                    .into_owned()
            };
            inventory.leaves.entry(leaf).or_default().insert(target);
        }
        if ext == "chr" {
            let sidecar = super::configs::native_path(alias, "game.json");
            if assets.join(&sidecar).is_file() {
                let value: serde_json::Value =
                    serde_json::from_slice(&fs::read(assets.join(sidecar))?)?;
                if let Some(profiles) = value["profiles"].as_object() {
                    inventory
                        .profiles
                        .insert(alias.to_owned(), profiles.keys().cloned().collect());
                }
            }
        }
    }
    if let Some(members) = pack(bytes) {
        for member in members {
            if member.size > 0 {
                inventory_resource(
                    &bytes[member.start..member.start + member.size],
                    &format!("{alias}/{}", member.name),
                    assets,
                    inventory,
                    depth + 1,
                )?;
            }
        }
    }
    Ok(())
}
fn migrate_container(
    bytes: &[u8],
    inventory: &Inventory,
    depth: usize,
) -> CoreResult<(Vec<u8>, usize)> {
    if depth > 16 {
        return Err(fail("compiled script archive nesting exceeds 16"));
    }
    if is_valid_script(bytes) {
        return migrate(bytes, inventory);
    }
    let Some(members) = pack(bytes) else {
        return Ok((bytes.to_vec(), 0));
    };
    let mut output = bytes.to_vec();
    let mut changes = 0;
    for member in members {
        if member.size == 0 {
            continue;
        }
        let (converted, count) = migrate_container(
            &bytes[member.start..member.start + member.size],
            inventory,
            depth + 1,
        )?;
        if count == 0 {
            continue;
        }
        let start = (output.len() + 3) & !3;
        output.resize(start, 0);
        let relative = u32::try_from(start - member.header)
            .map_err(|_| fail("migrated PACK offset overflow"))?;
        let size =
            u32::try_from(converted.len()).map_err(|_| fail("migrated PACK member overflow"))?;
        output[member.header + 64..member.header + 68].copy_from_slice(&relative.to_le_bytes());
        output[member.header + 68..member.header + 72].copy_from_slice(&size.to_le_bytes());
        output.extend(converted);
        changes += count;
    }
    Ok((output, changes))
}
/// Rewrites resource operands in retained bytecode after native assets have been published.
pub fn convert_tree(raw: &Path, published: &Path) -> CoreResult<()> {
    let assets = published.join("port_data");
    let mut inventory = Inventory {
        paths: BTreeMap::new(),
        leaves: BTreeMap::new(),
        profiles: BTreeMap::new(),
    };
    let mut files = Vec::new();
    gather(raw, &mut files)?;
    for p in files {
        let alias = p
            .strip_prefix(raw)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/")
            .to_ascii_lowercase();
        inventory_resource(&fs::read(p)?, &alias, &assets, &mut inventory, 0)?;
    }
    let mut files = Vec::new();
    if published.join("data").is_dir() {
        gather(&published.join("data"), &mut files)?;
    }
    let mut native = Vec::new();
    if assets.is_dir() {
        gather(&assets, &mut native)?;
    }
    files.extend(
        native
            .into_iter()
            .filter(|p| p.extension().is_some_and(|v| v == "stb")),
    );
    let (mut scripts, mut operands) = (0, 0);
    for p in files {
        let bytes = fs::read(&p)?;
        let (output, count) = migrate_container(&bytes, &inventory, 0)
            .map_err(|e| fail(format!("{}: {e}", p.display())))?;
        if count > 0 {
            fs::write(p, output)?;
            scripts += 1;
            operands += count;
        }
    }
    eprintln!("native compiled scripts: {operands} resource operands rewritten in {scripts} files");
    Ok(())
}

fn script_sources(
    bytes: &[u8],
    alias: &str,
    sources: &mut BTreeMap<String, Vec<u8>>,
    depth: usize,
) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("script source archive nesting exceeds 16"));
    }
    if is_valid_script(bytes) {
        sources.insert(alias.to_owned(), bytes.to_vec());
    } else if let Some(members) = super::models::pack(bytes) {
        let mut names = BTreeSet::new();
        for (ordinal, member) in members.iter().enumerate() {
            let child = if names.insert(member.name.clone()) {
                format!("{alias}/{}", member.name)
            } else {
                format!("{alias}/{}#entry:{ordinal}", member.name)
            };
            script_sources(
                &bytes[member.start..member.start + member.size],
                &child,
                sources,
                depth + 1,
            )?;
        }
    }
    Ok(())
}

/// Checks retained VM source coverage and exact deterministic operand migration before cache reuse.
pub fn verify_tree(raw: &Path, published: &Path) -> CoreResult<()> {
    let assets = published.join("port_data");
    let mut inventory = Inventory {
        paths: BTreeMap::new(),
        leaves: BTreeMap::new(),
        profiles: BTreeMap::new(),
    };
    let mut files = Vec::new();
    gather(raw, &mut files)?;
    let mut sources = BTreeMap::new();
    for source in files {
        let alias = source
            .strip_prefix(raw)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/")
            .to_ascii_lowercase();
        let bytes = fs::read(source)?;
        inventory_resource(&bytes, &alias, &assets, &mut inventory, 0)?;
        script_sources(&bytes, &alias, &mut sources, 0)?;
    }
    let mut operands = 0;
    for (alias, original) in &sources {
        let extension = Path::new(alias)
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or("stb")
            .split('#')
            .next()
            .unwrap();
        let native = super::configs::native_path(alias, extension);
        let direct = published.join("data").join(alias);
        let path = if direct.is_file() {
            direct
        } else {
            assets.join(&native)
        };
        let actual = fs::read(&path)
            .map_err(|error| fail(format!("missing native VM script {alias}: {error}")))?;
        let (expected, count) = migrate(original, &inventory)?;
        if actual != expected || !is_valid_script(&actual) {
            return Err(fail(format!(
                "native VM script differs from source migration: {}",
                path.display()
            )));
        }
        operands += count;
        let parts: Vec<_> = alias.split('/').collect();
        if let Some(parent) = (0..parts.len().saturating_sub(1)).rev().find(|index| {
            parts[*index]
                .split('#')
                .next()
                .unwrap()
                .trim_end_matches('_')
                .ends_with(".chr")
        }) {
            let character = parts[..=parent].join("/");
            let game_path = assets.join(super::configs::native_path(&character, "game.json"));
            let game: serde_json::Value = serde_json::from_slice(&fs::read(game_path)?)?;
            if !game["resources"]["scripts"]
                .as_array()
                .is_some_and(|scripts| scripts.iter().any(|script| script["path"] == native))
            {
                return Err(fail(format!(
                    "character omits native VM script resource: {alias}"
                )));
            }
        }
    }
    eprintln!(
        "native compiled script verification: {} scripts, {operands} migrated resource operands",
        sources.len()
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ambiguous_vm_sources_survive_all_converters_and_missing_cache_is_rejected() {
        fn archive(entries: &[(&str, &[u8])]) -> Vec<u8> {
            let mut output = Vec::new();
            for (name, payload) in entries {
                let mut header = vec![0; 80];
                header[..name.len()].copy_from_slice(name.as_bytes());
                for (at, value) in [(64, 80), (68, payload.len()), (72, 80 + payload.len())] {
                    header[at..at + 4].copy_from_slice(&(value as u32).to_le_bytes());
                }
                output.extend(header);
                output.extend_from_slice(payload);
            }
            output.extend([0; 80]);
            output
        }
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let published = temp.path().join("published");
        fs::create_dir_all(&raw).unwrap();
        let script = ambiguous_script_fixture();
        let names: Vec<_> = (0..23).map(|index| format!("effect_{index}.stb")).collect();
        let mut entries: Vec<_> = names
            .iter()
            .map(|name| (name.as_str(), script.as_slice()))
            .collect();
        entries.push(("info.cfg", b"EFFECT \"effect_0.stb\",1,0;"));
        fs::write(raw.join("effects.chr"), archive(&entries)).unwrap();
        fs::write(raw.join("test.stb"), &script).unwrap();
        let textures = super::super::textures::convert_tree(&raw, &published, 1).unwrap();
        super::super::models::convert_tree(&raw, &published, 1, &textures.documents).unwrap();
        super::super::configs::convert_tree(&raw, &published, 1).unwrap();
        convert_tree(&raw, &published).unwrap();
        super::super::packs::convert_tree(&raw, &published).unwrap();
        verify_tree(&raw, &published).unwrap();
        assert_eq!(fs::read(published.join("data/test.stb")).unwrap(), script);
        let game: serde_json::Value = serde_json::from_slice(
            &fs::read(published.join("port_data/effects.game.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(game["resources"]["scripts"].as_array().unwrap().len(), 23);
        assert_eq!(
            game["profiles"]["info"]["commands"][0]["args"][0]["kind"],
            "script"
        );
        assert!(!game["profiles"]["info"]["commands"][0]["args"][0]["index"].is_null());
        fs::remove_file(published.join("data/test.stb")).unwrap();
        fs::create_dir_all(published.join("port_data/test.stb")).unwrap();
        fs::write(
            published.join("port_data/test.stb/pack.json"),
            b"{\"schema_version\":1,\"kind\":\"asset_directory\"}",
        )
        .unwrap();
        assert!(verify_tree(&raw, &published).is_err());
    }

    #[test]
    #[ignore = "requires extracted retail data in KZ_NATIVE_SCRIPT_RAW"]
    fn every_retail_vm_is_distinct_from_an_archive() {
        let raw = std::path::PathBuf::from(std::env::var_os("KZ_NATIVE_SCRIPT_RAW").unwrap());
        let mut files = Vec::new();
        gather(&raw, &mut files).unwrap();
        let mut sources = BTreeMap::new();
        for file in files {
            let alias = file
                .strip_prefix(&raw)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            script_sources(&fs::read(file).unwrap(), &alias, &mut sources, 0).unwrap();
        }
        assert!(
            sources.len() >= 1390,
            "retail script coverage: {}",
            sources.len()
        );
        assert_eq!(
            sources
                .keys()
                .filter(|alias| alias.starts_with("dungeon/articles/eff01.chr/"))
                .count(),
            32
        );
        for (alias, bytes) in &sources {
            assert!(pack(bytes).is_none(), "{alias}");
            assert!(super::super::models::pack(bytes).is_none(), "{alias}");
        }
        eprintln!("retail VM/archive recognition: {} scripts", sources.len());
    }

    #[test]
    #[ignore = "requires extracted retail data in KZ_NATIVE_SCRIPT_RAW and KZ_NATIVE_SCRIPT_PUBLISHED"]
    fn verify_offline_retail_script_coverage() {
        verify_tree(
            Path::new(&std::env::var("KZ_NATIVE_SCRIPT_RAW").unwrap()),
            Path::new(&std::env::var("KZ_NATIVE_SCRIPT_PUBLISHED").unwrap()),
        )
        .unwrap();
    }
    fn put(b: &mut [u8], at: usize, value: i32) {
        b[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    fn fixture() -> Vec<u8> {
        let mut bytes = vec![0; 84 + 6 * 12];
        bytes[..3].copy_from_slice(b"SB2");
        for (at, value) in [(4, 28), (8, 28), (12, 28), (16, 0), (28, 56)] {
            put(&mut bytes, at, value);
        }
        let strings: [&[u8]; 4] = [
            b"chara/hero.chr",
            b"info.cfg",
            &[0x82, 0xa0, 0x82, 0xa2],
            b"docs/hero.chr",
        ];
        for (i, text) in strings.iter().enumerate() {
            let relative = (bytes.len() - 28) as i32;
            bytes.extend(*text);
            bytes.push(0);
            put(&mut bytes, 84 + i * 12, 3);
            put(&mut bytes, 88 + i * 12, 3);
            put(&mut bytes, 92 + i * 12, relative);
        }
        put(&mut bytes, 132, 16);
        put(&mut bytes, 136, 144 - 28);
        put(&mut bytes, 144, 15);
        bytes
    }
    fn inventory() -> Inventory {
        Inventory {
            paths: BTreeMap::from([("chara/hero.chr".into(), "chara/hero.gltf".into())]),
            leaves: BTreeMap::new(),
            profiles: BTreeMap::from([("chara/hero.chr".into(), BTreeSet::from(["info".into()]))]),
        }
    }
    #[test]
    fn append_pool_preserves_code_jumps_cp932_and_nonresource_text() {
        let original = fixture();
        let (converted, count) = migrate(&original, &inventory()).unwrap();
        assert_eq!(count, 2);
        for i in 0..original.len() {
            if !(92..96).contains(&i) && !(104..108).contains(&i) {
                assert_eq!(converted[i], original[i], "byte {i}");
            }
        }
        let (code, functions) = decode(&converted).unwrap();
        assert_eq!(
            literal(&converted, code, &functions[0][0]).unwrap(),
            b"chara/hero.gltf"
        );
        assert_eq!(
            literal(&converted, code, &functions[0][1]).unwrap(),
            b"info"
        );
        assert_eq!(
            literal(&converted, code, &functions[0][2]).unwrap(),
            [0x82, 0xa0, 0x82, 0xa2]
        );
        assert_eq!(
            literal(&converted, code, &functions[0][3]).unwrap(),
            b"docs/hero.chr"
        );
        assert_eq!(migrate(&converted, &inventory()).unwrap(), (converted, 0));
    }
    #[test]
    fn unreferenced_helpers_are_decoded_and_invalid_jumps_fail() {
        let mut bytes = fixture();
        let helper = (bytes.len() + 3) & !3;
        bytes.resize(helper + 56 + 24, 0);
        put(&mut bytes, helper, (helper + 56 - 28) as i32);
        put(&mut bytes, helper + 56, 3);
        put(&mut bytes, helper + 60, 3);
        let string = (bytes.len() - 28) as i32;
        put(&mut bytes, helper + 64, string);
        bytes.extend(b"chara/hero.chr\0");
        put(&mut bytes, helper + 68, 15);
        let (_, count) = migrate(&bytes, &inventory()).unwrap();
        assert_eq!(count, 3);
        put(&mut bytes, 136, -1);
        assert!(migrate(&bytes, &inventory()).is_err());
    }
    #[test]
    fn growing_nested_script_preserves_neighbor_payload_and_archive_traversal() {
        let script = fixture();
        let neighbor = b"unchanged payload";
        let mut bytes = vec![0; 228];
        bytes[..8].copy_from_slice(b"test.stb");
        bytes[76..84].copy_from_slice(b"text.mes");
        put(&mut bytes, 64, 228);
        put(&mut bytes, 68, script.len() as i32);
        put(&mut bytes, 72, 76);
        put(&mut bytes, 140, (228 + script.len() - 76) as i32);
        put(&mut bytes, 144, neighbor.len() as i32);
        put(&mut bytes, 148, 76);
        bytes.extend(script);
        bytes.extend(neighbor);
        let (converted, count) = migrate_container(&bytes, &inventory(), 0).unwrap();
        assert_eq!(count, 2);
        let members = pack(&converted).unwrap();
        assert_eq!(members[1].header, 76);
        assert_eq!(
            &converted[members[1].start..members[1].start + members[1].size],
            neighbor
        );
        assert!(members[0].start >= bytes.len());
        assert_eq!(word(&converted, 72).unwrap(), 76);
    }
    #[test]
    fn sound_stem_whitespace_migrates_only_to_an_existing_exact_asset() {
        let mut inventory = inventory();
        inventory.paths.insert(
            "snd2/mon/en_044.snd".into(),
            "snd2/mon/en_044.sound.json".into(),
        );
        assert_eq!(inventory.resource("snd2/mon/missing\t.snd"), None);
        assert_eq!(inventory.resource("unrelated missing\t.snd"), None);
        let mut bytes = fixture();
        bytes.truncate(84);
        bytes.resize(84 + 7 * 12, 0);
        for index in 0..6 {
            let relative = (bytes.len() - 28) as i32;
            put(&mut bytes, 84 + index * 12, 3);
            put(&mut bytes, 88 + index * 12, 3);
            put(&mut bytes, 92 + index * 12, relative);
            bytes.extend(b"snd2/mon/EN_044\t.snd\0");
        }
        put(&mut bytes, 84 + 6 * 12, 15);
        let (converted, count) = migrate(&bytes, &inventory).unwrap();
        assert_eq!(count, 6);
        let (code, functions) = decode(&converted).unwrap();
        for instruction in &functions[0][..6] {
            assert_eq!(
                literal(&converted, code, instruction).unwrap(),
                b"snd2/mon/en_044.sound.json"
            );
        }
        assert_eq!(migrate(&converted, &inventory).unwrap().1, 0);
        inventory.paths.insert(
            "snd2/mon/en_044\t.snd".into(),
            "literal-tab.sound.json".into(),
        );
        assert_eq!(
            inventory.resource("snd2/mon/EN_044\t.snd"),
            Some("literal-tab.sound.json".into())
        );
    }
    #[test]
    fn archive_named_sb2_is_not_misclassified_as_a_script() {
        let script = fixture();
        let mut bytes = vec![0; 152];
        bytes[..3].copy_from_slice(b"SB2");
        put(&mut bytes, 64, 152);
        put(&mut bytes, 68, script.len() as i32);
        put(&mut bytes, 72, 76);
        bytes.extend(script);
        assert!(!is_valid_script(&bytes));
        let (converted, count) = migrate_container(&bytes, &inventory(), 0).unwrap();
        assert_eq!(count, 2);
        let members = pack(&converted).unwrap();
        assert_eq!(members.len(), 1);
        let member = &members[0];
        let nested = &converted[member.start..member.start + member.size];
        assert!(is_valid_script(nested));
        assert_eq!(migrate(nested, &inventory()).unwrap().1, 0);
    }
    #[test]
    fn profile_context_handles_dynamic_character_paths_without_rewriting_print_text() {
        let mut bytes = fixture();
        // EXT LOAD_CHARA(stack, dynamic path, profile, slot).
        bytes.truncate(84);
        bytes.resize(84 + 7 * 12, 0);
        let instructions = [
            (3, 1, 9),
            (3, 1, 4),
            (1, 0, 1),
            (3, 3, 0),
            (3, 1, 8),
            (21, 5, 0),
            (15, 0, 0),
        ];
        for (i, (o, a, c)) in instructions.into_iter().enumerate() {
            put(&mut bytes, 84 + i * 12, o);
            put(&mut bytes, 88 + i * 12, a);
            put(&mut bytes, 92 + i * 12, c);
        }
        let relative = (bytes.len() - 28) as i32;
        put(&mut bytes, 128, relative);
        bytes.extend(b"info.cfg\0");
        let (converted, count) = migrate(&bytes, &inventory()).unwrap();
        assert_eq!(count, 1);
        let (code, functions) = decode(&converted).unwrap();
        assert_eq!(
            literal(&converted, code, &functions[0][3]).unwrap(),
            b"info"
        );
        put(&mut bytes, 84, 20);
        assert_eq!(migrate(&bytes, &inventory()).unwrap().1, 0);
    }
}
