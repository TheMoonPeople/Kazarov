//! Migrates compiled-script resource operands to directly published native files.
use super::artifacts::{ArtifactBytes, AssetSink, Category, Job};
use super::inventory::Inventory as ResourceInventory;
use super::progress::Phase;
use super::{CoreError, CoreResult};
use rayon::prelude::*;
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::fs;
use std::path::Path;
use std::sync::Arc;

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
    if block < 28 || !block.is_multiple_of(4) {
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
            if args >= 0
                && args <= local
                && local < 256
                && (1..=30).contains(&word(b, at + 0x38)?)
                && let Ok(instructions) = function(b, code, at)
            {
                seen.insert(at);
                covered[instructions[0].at..instructions.last().unwrap().at + 12].fill(true);
                at = instructions.last().unwrap().at + 8;
                functions.push(instructions);
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
/// How a bare retail name becomes the name of its converted file.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord)]
enum Rename {
    /// The converted file keeps the retail stem in the source's directory or PACK; only the extension changes.
    Extension(String),
    /// The converted name does not keep the retail stem: a movie's fixed path, or a renamed leaf.
    Path(String),
}
#[derive(Default)]
struct Inventory {
    paths: BTreeMap<String, String>,
    /// Lower-case retail leaf name to the renames of its converted sources.
    leaves: BTreeMap<String, BTreeSet<Rename>>,
    /// Lower-case retail leaf name to the published paths of its converted sources.
    counterparts: BTreeMap<String, BTreeSet<String>>,
    profiles: BTreeMap<String, BTreeSet<String>>,
}
/// The rename of one converted source published at `path`. A texture's document may sit at another
/// physical path, but keeps its logical name: in its PACK's `pack.json`, or below `textures/` in the
/// same directory for a texture outside any PACK.
fn rename(alias: &str, extension: &str, path: &str) -> Rename {
    if extension == "mp4" {
        return Rename::Path(path.to_owned());
    }
    let source = alias.rsplit('/').next().unwrap_or(alias);
    let source = source.split('#').next().unwrap_or(source);
    let lower = source.to_ascii_lowercase();
    let logical = if extension == "json" && (lower.ends_with(".img") || lower.ends_with(".tm2")) {
        super::textures::legacy_document_path(alias).unwrap_or_else(|_| path.to_owned())
    } else {
        path.to_owned()
    };
    let leaf = logical.rsplit('/').next().unwrap_or(&logical);
    let stem = source.rsplit_once('.').map_or(source, |(stem, _)| stem);
    match leaf.get(..stem.len()) {
        Some(prefix) if prefix.eq_ignore_ascii_case(stem) => {
            Rename::Extension(leaf[stem.len()..].to_owned())
        }
        _ => Rename::Path(leaf.to_owned()),
    }
}
impl Inventory {
    fn merge(&mut self, other: Inventory) {
        self.paths.extend(other.paths);
        self.profiles.extend(other.profiles);
        for (leaf, renames) in other.leaves {
            self.leaves.entry(leaf).or_default().extend(renames);
        }
        for (leaf, paths) in other.counterparts {
            self.counterparts.entry(leaf).or_default().extend(paths);
        }
    }
    /// Records one converted source and the path its converted file is published at.
    fn insert(&mut self, alias: &str, extension: &str, path: String) {
        let leaf = alias
            .rsplit('/')
            .next()
            .unwrap_or(alias)
            .to_ascii_lowercase();
        self.leaves
            .entry(leaf.clone())
            .or_default()
            .insert(rename(alias, extension, &path));
        self.counterparts
            .entry(leaf)
            .or_default()
            .insert(path.clone());
        self.paths.insert(alias.to_ascii_lowercase(), path);
    }
    /// Whether the converted file a migrated name refers to was published: a path directly, and a bare
    /// name as a converted source of the retail leaf it replaced.
    fn target_exists(&self, text: &str, target: &str, exists: impl Fn(&str) -> bool) -> bool {
        exists(target)
            || (!target.contains('/')
                && self
                    .counterparts
                    .get(&normalize(text))
                    .is_some_and(|paths| paths.iter().any(|path| exists(path))))
    }
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
        // The game looks a bare name up in the script's current directory and loaded PACK, which only the
        // running script decides. Every converted source keeps its stem in that same directory or PACK, so
        // replacing the extension names the converted file in whichever context the retail name resolved.
        let renames = self.leaves.get(&key)?;
        if renames.len() != 1 {
            return None;
        }
        match renames.first()? {
            Rename::Path(path) => Some(path.clone()),
            Rename::Extension(extension) => {
                let stem = text.rsplit_once('.').map_or(text, |(stem, _)| stem);
                Some(format!("{stem}{extension}"))
            }
        }
    }
    fn profile(&self, text: &str, character: Option<&str>) -> Option<String> {
        let key = normalize(text);
        if key.contains('/') || !key.ends_with(".cfg") {
            return None;
        }
        let stem = key.trim_end_matches(".cfg");
        if let Some(character) = character
            && self
                .profiles
                .get(&normalize(character))
                .is_some_and(|p| p.contains(stem))
        {
            return Some(stem.to_owned());
        }
        None
    }
}

fn planned_lookup(
    resources: &ResourceInventory,
    converted_aliases: &HashSet<String>,
    profiles: &BTreeMap<String, BTreeSet<String>>,
    texture_paths: &BTreeMap<String, String>,
) -> Inventory {
    let mut lookup = Inventory {
        profiles: profiles
            .iter()
            .map(|(alias, names)| {
                (
                    alias.to_ascii_lowercase(),
                    names.iter().map(|name| name.to_ascii_lowercase()).collect(),
                )
            })
            .collect(),
        ..Inventory::default()
    };
    for alias in resources.names() {
        let normalized_alias = alias.to_ascii_lowercase();
        let character_nested = normalized_alias.split('/').any(|part| {
            part.split('#')
                .next()
                .unwrap_or(part)
                .trim_end_matches('_')
                .ends_with(".chr")
        });
        let ext = alias
            .split("#entry:")
            .next()
            .unwrap_or(alias)
            .rsplit('.')
            .next()
            .unwrap_or("")
            .to_ascii_lowercase();
        let extension = match ext.as_str() {
            "chr" | "mds" | "mdt" | "mot" | "wgt" | "bbp" => Some("gltf"),
            "cfg" | "map" | "clo" | "img" | "tm2" => Some("json"),
            "snd" => Some("sound.json"),
            "pss" => Some("mp4"),
            "bin" if normalized_alias == "snd2/rev.bin" => Some("json"),
            "txt" if normalized_alias == "snd2/map_index.txt" => Some("json"),
            _ => None,
        };
        let Some(extension) = extension else { continue };
        if ext == "clo"
            || (character_nested
                && matches!(ext.as_str(), "cfg" | "map" | "img" | "tm2")
                && !super::configs::is_tracked(alias))
        {
            continue;
        }
        if !converted_aliases.contains(alias) && !super::configs::is_tracked(alias) {
            continue;
        }
        let path = texture_paths
            .get(alias)
            .cloned()
            .unwrap_or_else(|| super::configs::native_path(alias, extension));
        lookup.insert(alias, extension, path);
    }
    lookup
}

fn script_output_name(resources: &ResourceInventory, alias: &str) -> String {
    let normalized_alias = alias.to_ascii_lowercase();
    let nested_character = normalized_alias
        .split('/')
        .any(|part| part.split('#').next().unwrap_or(part).ends_with(".chr"));
    if nested_character {
        super::configs::native_path(alias, "stb")
    } else if alias.contains('/') && resources.roots().iter().all(|root| root.alias != alias) {
        super::packs::published_leaf_path(alias)
    } else {
        alias.to_owned()
    }
}

fn is_script_resource_name(alias: &str) -> bool {
    let lower_alias = alias.to_ascii_lowercase();
    lower_alias.ends_with(".stb") || lower_alias.ends_with(".stb_")
}

/// Plans final compiled-script bytes from immutable retail references and native asset names.
pub fn plan_jobs(
    resources: &ResourceInventory,
    converted_aliases: &HashSet<String>,
    profiles: &BTreeMap<String, BTreeSet<String>>,
    texture_paths: &BTreeMap<String, String>,
    publish_scripts: bool,
) -> CoreResult<(HashSet<String>, Vec<Job>)> {
    let lookup = Arc::new(planned_lookup(
        resources,
        converted_aliases,
        profiles,
        texture_paths,
    ));
    let mut consumed = HashSet::new();
    let mut jobs = Vec::new();
    for resource in resources.leaves() {
        let alias = resource.alias.clone();
        let valid_script = is_valid_script(&resource.bytes());
        if !publish_scripts && valid_script {
            if !is_script_resource_name(&alias) {
                return Err(CoreError(format!(
                    "compiled script resource has an unsupported alias: {alias}"
                )));
            }
            consumed.insert(alias);
            continue;
        }
        if !publish_scripts && is_script_resource_name(&alias) {
            consumed.insert(alias);
            continue;
        }
        if !valid_script {
            continue;
        }
        let name = script_output_name(resources, &alias);
        consumed.insert(alias.clone());
        let resource = resource.clone();
        let lookup = lookup.clone();
        jobs.push(Job {
            category: Category::Script,
            label: alias,
            work: resource.len().max(1) as u64,
            estimated_bytes: resource.len().saturating_mul(2),
            run: Box::new(move |sink: &AssetSink| {
                let (bytes, _) = migrate(&resource.bytes(), &lookup)?;
                sink.submit(name, ArtifactBytes::Owned(bytes), Category::Script)
            }),
        });
    }
    Ok((consumed, jobs))
}

pub fn verify_archive_work(resources: &ResourceInventory, publish_scripts: bool) -> u64 {
    if !publish_scripts {
        return 0;
    }
    resources
        .leaves()
        .iter()
        .filter(|resource| is_valid_script(&resource.bytes()))
        .count() as u64
}

/// Recomputes every retained compiled script and checks its final archived bytes.
pub fn verify_archive(
    resources: &ResourceInventory,
    converted_aliases: &HashSet<String>,
    profiles: &BTreeMap<String, BTreeSet<String>>,
    texture_paths: &BTreeMap<String, String>,
    reader: &dyn super::publish::ArchiveReader,
    phase: &Phase,
    publish_scripts: bool,
) -> CoreResult<()> {
    let lookup = planned_lookup(resources, converted_aliases, profiles, texture_paths);
    let mut character_scripts = BTreeMap::<String, BTreeSet<String>>::new();
    for resource in resources.leaves() {
        if !publish_scripts || !is_valid_script(&resource.bytes()) {
            continue;
        }
        let alias = &resource.alias;
        let name = script_output_name(resources, alias);
        let actual = reader.read(&name)?;
        let (expected, _) = migrate_checked(&resource.bytes(), &lookup, |text, path| {
            if lookup.target_exists(text, path, |candidate| reader.contains(candidate)) {
                Ok(())
            } else {
                Err(fail(format!(
                    "script resource target is absent: {alias} -> {path}"
                )))
            }
        })?;
        if actual != expected || !is_valid_script(&actual) {
            return Err(fail(format!(
                "native VM script differs from source migration: {alias}"
            )));
        }
        let parts: Vec<_> = alias.split('/').collect();
        if let Some(parent) = (0..parts.len().saturating_sub(1)).rev().find(|index| {
            parts[*index]
                .to_ascii_lowercase()
                .split('#')
                .next()
                .unwrap_or_default()
                .trim_end_matches('_')
                .ends_with(".chr")
        }) {
            let character = parts[..=parent].join("/");
            if !character_scripts.contains_key(&character) {
                let game_path = super::configs::native_path(&character, "game.json");
                let game: serde_json::Value = serde_json::from_slice(&reader.read(&game_path)?)?;
                let scripts = game["resources"]["scripts"]
                    .as_array()
                    .ok_or_else(|| fail(format!("character omits script list: {character}")))?
                    .iter()
                    .filter_map(|script| script["path"].as_str().map(str::to_owned))
                    .collect();
                character_scripts.insert(character.clone(), scripts);
            }
            let native = super::configs::native_path(alias, "stb");
            if !character_scripts[&character].contains(&native) {
                return Err(fail(format!(
                    "character omits native VM script resource: {alias}"
                )));
            }
        }
        phase.inc(1);
    }
    Ok(())
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
    migrate_checked(b, inventory, |_, _| Ok(()))
}

fn migrate_checked(
    b: &[u8],
    inventory: &Inventory,
    mut validate_resource: impl FnMut(&str, &str) -> CoreResult<()>,
) -> CoreResult<(Vec<u8>, usize)> {
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
            let (replacement, resource_target) = if profile_locations.contains(&insn.at) {
                (Some(key.trim_end_matches(".cfg").to_owned()), false)
            } else if let Some(profile) = inventory.profile(text, character) {
                (Some(profile), false)
            } else {
                (inventory.resource(text), true)
            };
            previous_character = if key.ends_with(".chr") {
                Some((insn.at, key))
            } else {
                None
            };
            if let Some(replacement) = replacement.filter(|v| v.as_bytes() != bytes) {
                if resource_target {
                    validate_resource(text, &replacement)?;
                }
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
        if assets.join(&path).is_file() || super::configs::is_tracked(alias) {
            inventory.insert(alias, extension, path);
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
pub fn convert_tree(raw: &Path, published: &Path, phase: &Phase) -> CoreResult<()> {
    convert_source(
        &super::archive::SourceTree::directory(raw)?,
        published,
        phase,
    )
}

pub fn convert_source(
    source: &super::archive::SourceTree,
    published: &Path,
    phase: &Phase,
) -> CoreResult<()> {
    let assets = published.join("data");
    let mut raw_files: Vec<&str> = source
        .names()
        .filter(|name| {
            !matches!(*name, "sound/sound.dat" | "sound/sound.hd3")
                && !name.ends_with(".pss")
                && !name.ends_with(".wav")
        })
        .collect();
    raw_files.sort();
    let mut files = Vec::new();
    if published.join("data").is_dir() {
        gather(&published.join("data"), &mut files)?;
    }
    files.retain(|path| {
        !matches!(
            path.extension().and_then(|ext| ext.to_str()),
            Some("png" | "wav" | "mp4" | "gltf" | "json")
        ) && !path
            .file_name()
            .is_some_and(|name| name.to_string_lossy().ends_with(".gltf.bin"))
    });
    let work = super::archive::compatibility_work_path(published, "");
    if work.is_dir() {
        gather(&work, &mut files)?;
    }
    files.sort();
    phase.set_total((raw_files.len() + files.len()) as u64);
    let locals: Vec<CoreResult<Inventory>> = raw_files
        .par_iter()
        .map(|p| -> CoreResult<Inventory> {
            let alias = p.to_ascii_lowercase();
            let mut local = Inventory::default();
            inventory_resource(&source.read(p)?, &alias, &assets, &mut local, 0)?;
            phase.inc(1);
            Ok(local)
        })
        .collect();
    let mut inventory = Inventory::default();
    for local in locals {
        inventory.merge(local?);
    }
    let migrated: Vec<CoreResult<usize>> = files
        .par_iter()
        .map(|p| -> CoreResult<usize> {
            let bytes = fs::read(p)?;
            if bytes.len() >= 2 << 20 {
                phase.item(&p.file_name().unwrap_or_default().to_string_lossy());
            }
            let (output, count) = migrate_container(&bytes, &inventory, 0)
                .map_err(|e| fail(format!("{}: {e}", p.display())))?;
            if count > 0 {
                fs::write(p, output)?;
            }
            phase.inc(1);
            Ok(count)
        })
        .collect();
    let (mut scripts, mut operands) = (0, 0);
    for count in migrated {
        let count = count?;
        scripts += usize::from(count > 0);
        operands += count;
    }
    phase.log(format!(
        "native compiled scripts: {operands} resource operands rewritten in {scripts} files"
    ));
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
pub fn verify_tree(raw: &Path, published: &Path, phase: &Phase) -> CoreResult<()> {
    verify_source(
        &super::archive::SourceTree::directory(raw)?,
        published,
        phase,
    )
}

pub fn verify_source(
    source: &super::archive::SourceTree,
    published: &Path,
    phase: &Phase,
) -> CoreResult<()> {
    let assets = published.join("data");
    let mut files: Vec<&str> = source
        .names()
        .filter(|name| {
            !matches!(*name, "sound/sound.dat" | "sound/sound.hd3")
                && !name.ends_with(".pss")
                && !name.ends_with(".wav")
        })
        .collect();
    files.sort();
    phase.set_total(files.len() as u64);
    let locals = files
        .par_iter()
        .map(|name| -> CoreResult<_> {
            let alias = name.to_ascii_lowercase();
            let bytes = source.read(name)?;
            let mut inventory = Inventory::default();
            inventory_resource(&bytes, &alias, &assets, &mut inventory, 0)?;
            let mut local = BTreeMap::new();
            script_sources(&bytes, &alias, &mut local, 0)?;
            phase.inc(1);
            Ok((inventory, local))
        })
        .collect::<Vec<_>>();
    let mut inventory = Inventory::default();
    let mut sources = BTreeMap::new();
    for local in locals {
        let (local_inventory, local_sources) = local?;
        inventory.merge(local_inventory);
        sources.extend(local_sources);
    }
    phase.add_total(sources.len() as u64);
    let checked: Vec<CoreResult<usize>> = sources
        .iter()
        .collect::<Vec<_>>()
        .into_par_iter()
        .map(|(alias, original)| -> CoreResult<usize> {
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
            phase.inc(1);
            Ok(count)
        })
        .collect();
    let mut operands = 0;
    for count in checked {
        operands += count?;
    }
    phase.log(format!(
        "native compiled script verification: {} scripts, {operands} migrated resource operands",
        sources.len()
    ));
    Ok(())
}

/// Event extension that loads a character pack into an event-scene actor (`_LOAD_CHARA`).
const EXT_LOAD_CHARA: i32 = 9;
/// Event extension that loads a motion pack into an event-scene actor (`_LOAD_MOTION`).
const EXT_LOAD_MOTION: i32 = 19;
/// Address mode of a script global slot (`RS_ADDR_GLOBAL`).
const ADDR_GLOBAL: i32 = 0x40;
/// One character or motion load into an event-scene actor whose pack is a constant.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ActorLoad {
    /// Offset of the first instruction of the calling function.
    pub function: usize,
    /// Offset of the extension call instruction.
    pub instruction: usize,
    /// Whether the call loads a motion pack (`_LOAD_MOTION`) rather than a character.
    pub motion: bool,
    /// Pack path exactly as the script names it, normalized to lower case with `/` separators.
    pub path: String,
    /// Event-scene actor slot the pack is loaded into.
    pub actor: i32,
    /// Global slot the actor is read from, when the script stores only constants there.
    pub global: Option<i32>,
}
/// Abstract value of one script stack slot.
#[derive(Clone, Copy)]
enum Value {
    Integer(i32),
    String(usize),
    Global(i32),
    GlobalAddress(i32),
    Unknown,
}
/// Runs the stack effect of every instruction of a script, reporting extension calls and stores.
fn simulate(
    b: &[u8],
    code: usize,
    functions: &[Vec<Instruction>],
    mut on_store: impl FnMut(i32, Value),
    mut on_ext: impl FnMut(&Vec<Instruction>, &Instruction, &[Value]) -> CoreResult<()>,
) -> CoreResult<()> {
    for function in functions {
        let mut stack = Vec::new();
        for insn in function {
            match insn.op {
                1 | 2 => {
                    if matches!(insn.a2, 2 | 4 | 16 | 32) {
                        stack.pop();
                    }
                    stack.push(match (insn.op, insn.a2) {
                        (1, ADDR_GLOBAL) => Value::Global(insn.a1),
                        (2, ADDR_GLOBAL) => Value::GlobalAddress(insn.a1),
                        _ => Value::Unknown,
                    });
                }
                3 => stack.push(match insn.a1 {
                    1 => Value::Integer(insn.a2),
                    3 => usize::try_from(insn.a2).map_or(Value::Unknown, Value::String),
                    _ => Value::Unknown,
                }),
                4 | 20 => {
                    stack.pop();
                }
                5 => {
                    let value = stack.pop().unwrap_or(Value::Unknown);
                    if let Some(Value::GlobalAddress(slot)) = stack.pop() {
                        on_store(slot, value);
                    }
                    stack.push(value);
                }
                6..=10 | 14 | 24 | 25 => {
                    stack.pop();
                    stack.pop();
                    stack.push(Value::Unknown);
                }
                11..=13 | 26 | 29 | 30 => {
                    stack.pop();
                    stack.push(Value::Unknown);
                }
                15..=18 | 27 => stack.clear(),
                19 => {
                    let block = code + offset(insn.a2)?;
                    let args = offset(word(b, block + 12)?)?;
                    stack.truncate(stack.len().saturating_sub(args));
                    stack.push(Value::Unknown);
                }
                21 => {
                    let count = offset(insn.a1)?;
                    if count > stack.len() {
                        stack.clear();
                        continue;
                    }
                    let start = stack.len() - count;
                    on_ext(function, insn, &stack[start..])?;
                    stack.truncate(start);
                }
                _ => {}
            }
        }
    }
    Ok(())
}
/// Lists the `_LOAD_CHARA` and `_LOAD_MOTION` calls of a compiled script whose pack is a constant.
///
/// An actor read from a global slot the script only ever sets to constants yields one load per
/// constant. Calls whose pack or actor is otherwise computed, or given through argument lists, are
/// not listed.
pub(crate) fn actor_loads(b: &[u8]) -> CoreResult<Vec<ActorLoad>> {
    let (code, functions) = decode(b)?;
    // Constants stored into each global slot; `None` once any other value is stored there.
    let mut globals = BTreeMap::<i32, Option<BTreeSet<i32>>>::new();
    simulate(
        b,
        code,
        &functions,
        |slot, value| {
            let entry = globals.entry(slot).or_insert_with(|| Some(BTreeSet::new()));
            match (entry.as_mut(), value) {
                (Some(values), Value::Integer(constant)) => {
                    values.insert(constant);
                }
                _ => *entry = None,
            }
        },
        |_, _, _| Ok(()),
    )?;
    let mut loads = Vec::new();
    simulate(
        b,
        code,
        &functions,
        |_, _| {},
        |function, insn, args| {
            let motion = match (args.first(), args.len()) {
                (Some(Value::Integer(EXT_LOAD_CHARA)), 5 | 6) => false,
                (Some(Value::Integer(EXT_LOAD_MOTION)), 5) => true,
                _ => return Ok(()),
            };
            let Value::String(at) = args[2] else {
                return Ok(());
            };
            let (actors, global) = match args[4] {
                Value::Integer(actor) => (BTreeSet::from([actor]), None),
                Value::Global(slot) => match globals.get(&slot) {
                    Some(Some(values)) => (values.clone(), Some(slot)),
                    _ => return Ok(()),
                },
                _ => return Ok(()),
            };
            let bytes = literal(
                b,
                code,
                &Instruction {
                    at: insn.at,
                    op: 3,
                    a1: 3,
                    a2: at as i32,
                },
            )?;
            if !bytes.is_ascii() {
                return Ok(());
            }
            let path = normalize(std::str::from_utf8(bytes).unwrap());
            for actor in actors {
                loads.push(ActorLoad {
                    function: function[0].at,
                    instruction: insn.at,
                    motion,
                    path: path.clone(),
                    actor,
                    global,
                });
            }
            Ok(())
        },
    )?;
    Ok(loads)
}
/// Lists the ASCII string literals of a compiled script, normalized like resource paths.
pub(crate) fn string_literals(b: &[u8]) -> CoreResult<BTreeSet<String>> {
    let (code, functions) = decode(b)?;
    let mut out = BTreeSet::new();
    for insn in functions.iter().flatten() {
        if insn.op == 3 && insn.a1 == 3 {
            let bytes = literal(b, code, insn)?;
            if bytes.is_ascii() {
                out.insert(normalize(std::str::from_utf8(bytes).unwrap()));
            }
        }
    }
    Ok(out)
}
/// Argument of an extension call in a test script.
#[cfg(test)]
pub(crate) enum ScriptArg<'a> {
    Int(i32),
    Text(&'a str),
    /// Reads a global slot.
    Global(i32),
}
/// Pseudo extension id of a test script step that stores the constant `args[1]` into global `args[0]`.
#[cfg(test)]
pub(crate) const STORE_GLOBAL: i32 = -1;
/// Builds a compiled script whose main function makes the given extension calls in order.
#[cfg(test)]
pub(crate) fn event_script_fixture(calls: &[(i32, &[ScriptArg])]) -> Vec<u8> {
    const CODE: usize = 48;
    let mut code = Vec::new();
    let mut strings = Vec::new();
    let count: usize = calls
        .iter()
        .map(|(ext, args)| match *ext {
            STORE_GLOBAL => 4,
            _ => args.len() + 2,
        })
        .sum::<usize>()
        + 1;
    let strings_at = count * 12;
    let mut push = |op: i32, a1: i32, a2: i32| {
        for value in [op, a1, a2] {
            code.extend(value.to_le_bytes());
        }
    };
    for (ext, args) in calls {
        if let (STORE_GLOBAL, [ScriptArg::Int(slot), ScriptArg::Int(value)]) = (*ext, args) {
            push(2, *slot, ADDR_GLOBAL);
            push(3, 1, *value);
            push(5, 0, 0);
            push(4, 0, 0);
            continue;
        }
        push(3, 1, *ext);
        for arg in *args {
            match arg {
                ScriptArg::Int(value) => push(3, 1, *value),
                ScriptArg::Global(slot) => push(1, *slot, ADDR_GLOBAL),
                ScriptArg::Text(text) => {
                    push(3, 3, (strings_at + strings.len()) as i32);
                    strings.extend(text.as_bytes());
                    strings.push(0);
                }
            }
        }
        push(21, args.len() as i32 + 1, 0);
    }
    push(15, 0, 0);
    let mut bytes = vec![0; CODE];
    bytes[..4].copy_from_slice(b"STB\0");
    for (at, value) in [(4, 28i32), (8, CODE as i32), (12, 28), (16, 0)] {
        bytes[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    bytes.extend(code);
    bytes.extend(strings);
    while !bytes.len().is_multiple_of(4) {
        bytes.push(0);
    }
    bytes
}
#[cfg(test)]
#[path = "script_reference_audit.rs"]
mod reference_audit;
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn luau_only_rejects_valid_scripts_with_unmapped_aliases() {
        let temporary = tempfile::tempdir().unwrap();
        let raw = temporary.path().join("raw");
        fs::create_dir_all(&raw).unwrap();
        fs::write(raw.join("valid-script.bin"), ambiguous_script_fixture()).unwrap();
        let source = super::super::archive::SourceTree::directory(&raw).unwrap();
        let resources = ResourceInventory::build(&source).unwrap();
        let error = match plan_jobs(
            &resources,
            &HashSet::new(),
            &BTreeMap::new(),
            &BTreeMap::new(),
            false,
        ) {
            Ok(_) => panic!("valid script with an unmapped alias was accepted"),
            Err(error) => error,
        };
        assert!(error.to_string().contains("unsupported alias"));
    }

    #[test]
    fn luau_only_consumes_nested_pack_scripts_and_keeps_neighbor_resources() {
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
        fs::create_dir_all(&raw).unwrap();
        let script = ambiguous_script_fixture();
        let nested = pack(&[("c02.stb_", &script), ("palette.bin", b"keep")]);
        fs::write(raw.join("scripts.pak"), pack(&[("nested.pak", &nested)])).unwrap();
        fs::write(raw.join("loose.stb"), &script).unwrap();
        let source = super::super::archive::SourceTree::directory(&raw).unwrap();
        let resources = ResourceInventory::build(&source).unwrap();
        let (consumed, jobs) = plan_jobs(
            &resources,
            &HashSet::new(),
            &BTreeMap::new(),
            &BTreeMap::new(),
            false,
        )
        .unwrap();
        assert!(jobs.is_empty());
        assert!(consumed.contains("loose.stb"));
        assert!(consumed.contains("scripts.pak/nested.pak/c02.stb_"));
        assert!(!consumed.contains("scripts.pak/nested.pak/palette.bin"));
        assert_eq!(resources.leaves().len(), 3);
    }

    #[test]
    fn script_publication_recognizes_loose_and_trailing_underscore_aliases() {
        for alias in ["map/evt.stb", "mainchr/c02.stb_", "PACK.PAK/script.STB"] {
            assert!(is_script_resource_name(alias));
        }
        for alias in ["map/evt.stb.bak", "map/evt.stb1", "map/evt"] {
            assert!(!is_script_resource_name(alias));
        }
    }

    #[test]
    fn bare_migrated_targets_resolve_to_published_source_paths() {
        let mut inventory = Inventory::default();
        inventory.insert(
            "event/e100/e110/e110_mints.chr",
            "gltf",
            "event/e100/e110/e110_mints.gltf".into(),
        );
        inventory.insert(
            "event/e100/e110/_e110_data.pak/e110_mints.chr",
            "gltf",
            "event/e100/e110/_e110_data.pak/e110_mints.gltf".into(),
        );
        let published = BTreeSet::from(["event/e100/e110/e110_mints.gltf".to_owned()]);
        let exists = |path: &str| published.contains(path);
        assert!(inventory.target_exists("e110_mints.chr", "e110_mints.gltf", exists));
        assert!(!inventory.target_exists("missing.chr", "missing.gltf", exists));
        assert!(!inventory.target_exists(
            "elsewhere/e110_mints.chr",
            "elsewhere/e110_mints.gltf",
            exists
        ));
    }

    #[test]
    fn bare_names_keep_their_spelling_and_change_only_the_extension() {
        // Underground Channel's boss scene loads "e110_Halloween.chr" in event/e100/e110/, which holds a
        // loose e110_halloween.chr and the _e110_data.pak member e110_Halloween.chr.
        let mut inventory = Inventory::default();
        for (alias, extension, path) in [
            (
                "event/e100/e110/e110_halloween.chr",
                "gltf",
                "event/e100/e110/e110_halloween.gltf",
            ),
            (
                "event/e100/e110/_e110_data.pak/e110_Halloween.chr",
                "gltf",
                "event/e100/e110/_e110_data.pak/e110_Halloween.gltf",
            ),
            // The loose image keeps its name below textures/; the PACK member's document takes a hash
            // suffix, and its pack.json names it e110telop01.json.
            (
                "event/e100/e110/e110telop01.img",
                "json",
                "textures/event/e100/e110/e110telop01.json",
            ),
            (
                "event/e100/e110/_e110_data.pak/e110telop01.img",
                "json",
                "textures/event/e100/e110/e110telop01-60b3624cea5e-0.json",
            ),
            (
                "dungeon/sphida/sphida_bar_1.img",
                "json",
                "textures/dungeon/sphida/sphida_bar_1.json",
            ),
            (
                "snd2/sp/SP_001.snd",
                "sound.json",
                "snd2/sp/SP_001.sound.json",
            ),
            (
                "menu/0/inv0_bg.pac/sp_001.snd",
                "sound.json",
                "menu/0/inv0_bg.pac/sp_001.sound.json",
            ),
            ("movie/OPEN.pss", "mp4", "movie/OPEN.mp4"),
            ("chara/a.mds", "gltf", "chara/a.gltf"),
            ("etc/gsys.pak/a.mds", "gltf", "etc/gsys.pak/a.model.gltf"),
        ] {
            inventory.insert(alias, extension, path.into());
        }
        for (text, converted) in [
            ("e110_Halloween.chr", Some("e110_Halloween.gltf")),
            ("E110_HALLOWEEN.CHR", Some("E110_HALLOWEEN.gltf")),
            ("e110telop01.img", Some("e110telop01.json")),
            ("sphida_bar_1.img", Some("sphida_bar_1.json")),
            ("sp_001.snd", Some("sp_001.sound.json")),
            ("open.pss", Some("movie/OPEN.mp4")),
            // Sources converted to different extensions stay unconverted.
            ("a.mds", None),
            ("missing.chr", None),
            (
                "event/e100/e110/e110telop01.img",
                Some("textures/event/e100/e110/e110telop01.json"),
            ),
        ] {
            assert_eq!(inventory.resource(text).as_deref(), converted, "{text}");
        }
        let mut bytes = fixture();
        bytes.truncate(84);
        bytes.resize(84 + 2 * 12, 0);
        let relative = (bytes.len() - 28) as i32;
        put(&mut bytes, 84, 3);
        put(&mut bytes, 88, 3);
        put(&mut bytes, 92, relative);
        bytes.extend(b"e110_Halloween.chr\0");
        put(&mut bytes, 84 + 12, 15);
        let (converted, count) = migrate(&bytes, &inventory).unwrap();
        assert_eq!(count, 1);
        let (code, functions) = decode(&converted).unwrap();
        assert_eq!(
            literal(&converted, code, &functions[0][0]).unwrap(),
            b"e110_Halloween.gltf"
        );
    }

    #[test]
    fn actor_loads_follow_constant_actors_and_globals_set_only_to_constants() {
        let load = |ext: i32, path: &'static str, actor: ScriptArg<'static>| {
            (
                ext,
                vec![
                    ScriptArg::Int(0),
                    ScriptArg::Text(path),
                    ScriptArg::Text("info.cfg"),
                    actor,
                ],
            )
        };
        let steps = [
            (STORE_GLOBAL, vec![ScriptArg::Int(6), ScriptArg::Int(0)]),
            (STORE_GLOBAL, vec![ScriptArg::Int(6), ScriptArg::Int(32)]),
            load(EXT_LOAD_CHARA, r"chara\C01_base.chr", ScriptArg::Int(32)),
            load(EXT_LOAD_MOTION, "e280_mints.chr", ScriptArg::Global(6)),
            // An actor read from a global the script never sets is not listed.
            load(EXT_LOAD_MOTION, "skipped.chr", ScriptArg::Global(7)),
        ];
        let calls: Vec<(i32, &[ScriptArg])> = steps
            .iter()
            .map(|(ext, args)| (*ext, args.as_slice()))
            .collect();
        let loads = actor_loads(&event_script_fixture(&calls)).unwrap();
        let summary: Vec<_> = loads
            .iter()
            .map(|l| (l.motion, l.path.as_str(), l.actor, l.global))
            .collect();
        assert_eq!(
            summary,
            [
                (false, "chara/c01_base.chr", 32, None),
                (true, "e280_mints.chr", 0, Some(6)),
                (true, "e280_mints.chr", 32, Some(6)),
            ]
        );
        assert!(
            loads[1].function == loads[0].function && loads[1].instruction > loads[0].instruction
        );
        assert!(
            string_literals(&event_script_fixture(&calls))
                .unwrap()
                .contains("e280_mints.chr")
        );
    }
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
        let models = super::super::models::convert_tree(
            &raw,
            &published,
            &textures.documents,
            &Phase::noop(),
        )
        .unwrap();
        super::super::models::strip_data(&raw, &published, &models, &Phase::noop()).unwrap();
        super::super::configs::convert_tree(&raw, &published, &Phase::noop()).unwrap();
        convert_tree(&raw, &published, &Phase::noop()).unwrap();
        super::super::packs::convert_tree(&raw, &published, &Phase::noop()).unwrap();
        verify_tree(&raw, &published, &Phase::noop()).unwrap();
        assert_eq!(fs::read(published.join("data/test.stb")).unwrap(), script);
        let game: serde_json::Value =
            serde_json::from_slice(&fs::read(published.join("data/effects.game.json")).unwrap())
                .unwrap();
        assert_eq!(game["resources"]["scripts"].as_array().unwrap().len(), 23);
        assert_eq!(
            game["profiles"]["info"]["commands"][0]["args"][0]["kind"],
            "script"
        );
        assert!(!game["profiles"]["info"]["commands"][0]["args"][0]["index"].is_null());
        fs::remove_file(published.join("data/test.stb")).unwrap();
        fs::create_dir_all(published.join("data/test.stb")).unwrap();
        fs::write(
            published.join("data/test.stb/pack.json"),
            b"{\"schema_version\":1,\"kind\":\"asset_directory\"}",
        )
        .unwrap();
        assert!(verify_tree(&raw, &published, &Phase::noop()).is_err());
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
            &Phase::noop(),
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
            counterparts: BTreeMap::new(),
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
