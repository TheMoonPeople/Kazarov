//! Audits a published archive against the retail disc it was extracted from: every converted source has
//! its counterpart where the game looks for it, and a name a script reads from the disc in a directory or
//! PACK still reads that same source's counterpart there.
use super::super::archive::SourceTree;
use super::super::inventory::Inventory as ResourceInventory;
use super::super::zip_archive::Archive;
use super::{Instruction, decode, is_valid_script, literal, script_output_name, word};
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

fn fold(path: &str) -> String {
    let mut out = String::with_capacity(path.len());
    for character in path.replace('\\', "/").trim_start_matches('/').chars() {
        if !(character == '/' && out.ends_with('/')) {
            out.push(character.to_ascii_lowercase());
        }
    }
    out
}

/// Where the game looks a script name up: below a directory, or among the members of a loaded PACK.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Context {
    Directory(String),
    Pack(String),
}

/// Files and PACK members as the game finds them by name, folding ASCII case.
#[derive(Default)]
struct Namespace {
    files: BTreeSet<String>,
    /// PACK path to its member names, each with the file it reads.
    packs: BTreeMap<String, BTreeMap<String, String>>,
    /// A texture document outside any PACK is published below `textures/`, where the game finds a
    /// relative name it misses beside its retail directory.
    textures: bool,
    /// On the disc, the physical document the texture converter plans for each texture source.
    documents: BTreeMap<String, String>,
}
impl Namespace {
    fn file(&self, path: &str) -> Option<String> {
        let path = fold(path);
        if self.files.contains(&path) || self.packs.contains_key(&path) {
            return Some(path);
        }
        let relocated = format!("textures/{path}");
        (self.textures && path.ends_with(".json") && self.files.contains(&relocated))
            .then_some(relocated)
    }
    /// The file a script name reads in one context. A root-anchored name ignores the directory.
    fn read(&self, text: &str, context: &Context) -> Option<String> {
        match context {
            Context::Directory(_) if text.starts_with('/') => self.file(text),
            Context::Directory(directory) => self.file(&format!("{directory}{text}")),
            Context::Pack(pack) if !text.contains(['/', '\\']) => {
                // A member reads the file its marker names, which must exist (asset_directory.cpp).
                let path = self.packs.get(pack)?.get(&fold(text))?;
                (self.files.contains(path) || self.packs.contains_key(path)).then(|| path.clone())
            }
            Context::Pack(_) => None,
        }
    }
}

fn retail(inventory: &ResourceInventory) -> Namespace {
    let mut namespace = Namespace::default();
    for alias in inventory.names() {
        namespace.files.insert(fold(alias));
        if let Some(members) = inventory.members(alias) {
            let members = members
                .iter()
                .rev()
                .map(|member| (fold(&member.name), fold(&member.alias)))
                .collect();
            namespace.packs.insert(fold(alias), members);
        }
    }
    namespace
}

fn published(archive: &Archive) -> (Namespace, BTreeMap<String, usize>) {
    let mut namespace = Namespace {
        textures: true,
        ..Namespace::default()
    };
    let mut entries = BTreeMap::new();
    for index in 0..archive.len() {
        let name = fold(archive.name(index));
        // Tracked gameplay documents are found at their asset paths.
        if let Some(asset) = name.strip_prefix("gameplay/") {
            namespace.files.insert(asset.to_owned());
        }
        namespace.files.insert(name.clone());
        entries.insert(name, index);
    }
    for name in entries.keys() {
        if let Some(pack) = name.strip_suffix("/pack.json") {
            let marker: serde_json::Value =
                serde_json::from_slice(&archive.read(entries[name]).unwrap()).unwrap();
            let mut members = BTreeMap::new();
            for member in marker["texture_members"].as_array().into_iter().flatten() {
                members.insert(
                    fold(member["name"].as_str().unwrap()),
                    fold(member["path"].as_str().unwrap()),
                );
            }
            let prefix = format!("{pack}/");
            for member in entries.range(prefix.clone()..) {
                let Some(leaf) = member.0.strip_prefix(&prefix) else {
                    break;
                };
                if !leaf.contains('/') && leaf != "pack.json" {
                    members.insert(leaf.to_owned(), member.0.clone());
                }
            }
            namespace.packs.insert(pack.to_owned(), members);
        }
    }
    (namespace, entries)
}

/// The name of a source's converted file in its own directory or PACK, by the converters' naming, which
/// also covers the standalone models named `.model.gltf` beside a same-stem character.
fn converted_name(source: &str) -> Option<String> {
    let leaf = source.rsplit('/').next().unwrap_or(source);
    let (_, extension) = leaf.rsplit_once('.')?;
    let extension = extension.to_ascii_lowercase();
    let path = match extension.as_str() {
        "chr" | "mds" | "mdt" | "mot" | "wgt" | "bbp" => {
            super::super::configs::native_path(source, "gltf")
        }
        "img" | "tm2" => super::super::textures::legacy_document_path(source).ok()?,
        "snd" => super::super::configs::native_path(source, "sound.json"),
        "pss" => super::super::configs::native_path(source, "mp4"),
        "cfg" | "map" => super::super::configs::native_path(source, "json"),
        _ => return None,
    };
    Some(path.rsplit('/').next().unwrap_or(&path).to_owned())
}

/// Where the archive publishes a disc source's converted file, read like the source in its own PACK or
/// directory. A texture document can sit at another physical path, so it must be the very document the
/// texture converter planned from this source, not whatever a PACK marker names.
fn converted(disc: &Namespace, data: &Namespace, source: &str) -> Option<String> {
    let name = converted_name(source)?;
    let texture = source.ends_with(".img") || source.ends_with(".tm2");
    let parent = source.rsplit_once('/').map_or("", |(parent, _)| parent);
    let context = if disc.packs.contains_key(parent) {
        Context::Pack(parent.to_owned())
    } else if parent.is_empty() {
        Context::Directory(String::new())
    } else {
        Context::Directory(format!("{parent}/"))
    };
    let found = data.read(&name, &context)?;
    if texture && disc.documents.get(source) != Some(&found) {
        return None;
    }
    Some(found)
}

/// Retail extensions whose sources are always converted, so must have a converted counterpart.
const CONVERTED: [&str; 10] = [
    "chr", "mds", "mdt", "mot", "wgt", "bbp", "img", "tm2", "snd", "pss",
];

/// Counts the always-converted sources outside characters and lists those without a counterpart.
fn missing_counterparts<'a>(
    sources: impl IntoIterator<Item = &'a str>,
    disc: &Namespace,
    data: &Namespace,
) -> (usize, Vec<String>) {
    let (mut checked, mut missing) = (0, Vec::new());
    for alias in sources {
        let (parent, leaf) = alias.rsplit_once('/').unwrap_or(("", alias));
        let extension = leaf.rsplit_once('.').map_or("", |(_, extension)| extension);
        // Character members become part of the character's own model, and duplicate PACK names are
        // unreachable by name.
        let character = parent.split('/').any(|part| {
            let part = part.split('#').next().unwrap().to_ascii_lowercase();
            part.ends_with(".chr") || part.ends_with(".chr_")
        });
        if !CONVERTED.contains(&extension.to_ascii_lowercase().as_str())
            || character
            || alias.contains('#')
        {
            continue;
        }
        checked += 1;
        if converted(disc, data, &fold(alias)).is_none() {
            missing.push(alias.to_owned());
        }
    }
    (checked, missing)
}

/// The directory `_LOAD_SCRIPT` reads a map script name from, after it applies the "_1" language marker.
fn map_script(text: &str) -> Option<String> {
    if !text.to_ascii_lowercase().ends_with(".stb") {
        return None;
    }
    let (map, rest) = text.split_once('/')?;
    let rest = if text.contains("_1") {
        rest.to_owned()
    } else {
        format!("{}_1.stb", rest.strip_suffix(".stb")?)
    };
    let mut directory = format!("map/{}/", map.get(..1)?);
    for range in [0..3, 3..6] {
        if let Some(part) = map.get(range) {
            directory.push_str(part);
            directory.push('/');
        }
    }
    Some(format!("{directory}{rest}"))
}

/// The name a retail language marker selects for PAL language `code`, 1 to 5.
fn language(text: &str, code: u32) -> Option<String> {
    if code == 1 {
        return Some(text.to_owned());
    }
    let at = text.rfind("_1.")?;
    Some(format!("{}_{code}{}", &text[..at], &text[at + 2..]))
}

/// The names a script name reads in one PAL language, each with the contexts it is looked up in: the
/// name itself in the script's contexts, the map script `_LOAD_SCRIPT` names, and a movie.
fn lookups(text: &str, code: u32, contexts: &[Context]) -> Vec<(String, Vec<Context>)> {
    let root = vec![Context::Directory(String::new())];
    let mut out = Vec::new();
    if let Some(name) = language(text, code) {
        if fold(&name).ends_with(".pss") {
            out.push((format!("movie/{name}"), root.clone()));
        }
        out.push((name, contexts.to_vec()));
    }
    if let Some(path) = map_script(text).and_then(|path| language(&path, code)) {
        out.push((path, root));
    }
    out
}

/// Checks one script name in one PAL language: for every context the disc reads it in, the migrated name
/// must read that source's counterpart in the same context, or the source itself where it stays
/// unconverted. Returns how many disc lookups it checked, and each one the archive lost.
fn check(
    disc: &Namespace,
    data: &Namespace,
    contexts: &[Context],
    before: &str,
    after: &str,
    code: u32,
) -> (usize, Vec<String>) {
    let (mut checked, mut lost) = (0, Vec::new());
    let migrated = lookups(after, code, contexts);
    for (name, contexts) in lookups(before, code, contexts) {
        for context in contexts {
            let Some(source) = disc.read(&name, &context) else {
                continue;
            };
            checked += 1;
            // The migrated name, or the movie or map script name the game composes from it.
            let found = migrated
                .iter()
                .filter(|(_, contexts)| contexts.contains(&context))
                .find_map(|(name, _)| data.read(name, &context));
            let expected = [converted(disc, data, &source), data.file(&source)];
            if found.is_none() || !expected.contains(&found) {
                lost.push(format!("{context:?}: {source} -> {found:?}"));
            }
        }
    }
    (checked, lost)
}

/// One script's string operands, paired by instruction: retail text and published text.
fn operands(original: &[u8], migrated: &[u8]) -> Vec<(usize, String, String)> {
    let (code, functions) = decode(original).unwrap();
    let mut out = Vec::new();
    for insn in functions.iter().flatten() {
        if insn.op != 3 || insn.a1 != 3 {
            continue;
        }
        let published = Instruction {
            a2: word(migrated, insn.at + 8).unwrap(),
            ..*insn
        };
        let (Ok(before), Ok(after)) = (
            literal(original, code, insn),
            literal(migrated, code, &published),
        ) else {
            continue;
        };
        if let (Ok(before), Ok(after)) = (std::str::from_utf8(before), std::str::from_utf8(after))
            && before.is_ascii()
        {
            out.push((insn.at, before.to_owned(), after.to_owned()));
        }
    }
    out.sort();
    out
}

/// Profile names a converted character declares in its gameplay document.
fn profiles(archive: &Archive, entries: &BTreeMap<String, usize>, model: &str) -> BTreeSet<String> {
    let Some(index) = model
        .strip_suffix(".gltf")
        .and_then(|stem| entries.get(&format!("gameplay/{stem}.character.json")))
    else {
        return BTreeSet::new();
    };
    let document: serde_json::Value =
        serde_json::from_slice(&archive.read(*index).unwrap()).unwrap();
    match &document["profiles"] {
        serde_json::Value::Array(names) => names
            .iter()
            .filter_map(|name| name.as_str().map(fold))
            .collect(),
        serde_json::Value::Object(profiles) => profiles.keys().map(|name| fold(name)).collect(),
        _ => BTreeSet::new(),
    }
}

/// Retail extensions of names the disc is expected to resolve, for reporting the names it cannot.
const ASSET_EXTENSIONS: [&str; 17] = [
    "chr", "mds", "mdt", "mot", "wgt", "bbp", "img", "tm2", "snd", "pss", "cfg", "map", "stb",
    "txt", "pak", "pac", "ipk",
];

#[test]
#[ignore = "requires the retail disc in KZ_RETAIL_SOURCE and its extraction's data.zip in KZ_RETAIL_DATA"]
fn published_archive_resolves_every_retail_reference() {
    let source = SourceTree::open(Path::new(&std::env::var("KZ_RETAIL_SOURCE").unwrap())).unwrap();
    let inventory = ResourceInventory::build(&source).unwrap();
    let archive = Archive::open(Path::new(&std::env::var("KZ_RETAIL_DATA").unwrap())).unwrap();
    let mut disc = retail(&inventory);
    disc.documents = super::super::textures::plan_source(&inventory)
        .unwrap()
        .documents
        .iter()
        .map(|document| (fold(&document.source_alias), fold(&document.document_path)))
        .collect();
    let (data, entries) = published(&archive);

    let (sources, missing) = missing_counterparts(inventory.names(), &disc, &data);
    eprintln!(
        "{sources} converted sources, {} without a counterpart",
        missing.len()
    );

    let (mut checked, mut dynamic, mut lost) = (0, 0, Vec::new());
    let mut unresolved = BTreeMap::<String, usize>::new();
    for resource in inventory.leaves() {
        if !is_valid_script(&resource.bytes()) {
            continue;
        }
        let alias = &resource.alias;
        let name = fold(&script_output_name(&inventory, alias));
        let migrated = archive.read(entries[&name]).unwrap();
        let operands = operands(&resource.bytes(), &migrated);
        // The script's contexts: the root, its own directory, every directory it changes to, and every
        // PACK it names. A character is a PACK on the disc, but its members are read through the
        // character, not by name.
        let mut contexts = BTreeSet::from([
            Context::Directory(String::new()),
            Context::Directory(
                alias
                    .rsplit_once('/')
                    .map_or(String::new(), |(parent, _)| format!("{}/", fold(parent))),
            ),
        ]);
        for (_, text, _) in &operands {
            if text.ends_with('/') && !text.starts_with('/') {
                contexts.insert(Context::Directory(fold(text)));
            }
        }
        let directories: Vec<_> = contexts.iter().cloned().collect();
        for (_, text, _) in &operands {
            let lower = fold(text);
            if lower.ends_with(".chr") || lower.ends_with(".chr_") {
                continue;
            }
            for context in &directories {
                if let Some(path) = disc.read(text, context)
                    && disc.packs.contains_key(&path)
                {
                    contexts.insert(Context::Pack(path));
                }
            }
        }
        let contexts: Vec<_> = contexts.into_iter().collect();
        // A profile name follows the character it is read from.
        let mut previous: Option<(usize, &str, &str)> = None;
        for (at, before, after) in &operands {
            let lower = fold(before);
            let character = previous
                .take()
                .filter(|(character, _, _)| character + 12 == *at)
                .map(|(_, before, after)| (before, after));
            if lower.ends_with(".chr") {
                previous = Some((*at, before, after));
            }
            if lower.ends_with('/') {
                continue;
            }
            if let Some((character_before, character_after)) =
                character.filter(|_| lower.ends_with(".cfg") && !lower.contains('/'))
            {
                // On the disc a profile is a member of the character's PACK; in the archive, a profile
                // of the character's converted model in the same context.
                let mut resolved = false;
                for context in &contexts {
                    let Some(pack) = disc.read(character_before, context) else {
                        continue;
                    };
                    if !disc
                        .packs
                        .get(&pack)
                        .is_some_and(|members| members.contains_key(&lower))
                    {
                        continue;
                    }
                    resolved = true;
                    checked += 1;
                    let model = data
                        .read(character_after, context)
                        .filter(|model| converted(&disc, &data, &pack).as_ref() == Some(model));
                    if !model.is_some_and(|model| {
                        profiles(&archive, &entries, &model).contains(&fold(after))
                    }) {
                        lost.push(format!("{alias} {at:#x} {context:?}: {before} -> {after}"));
                    }
                }
                if resolved {
                    continue;
                }
            }
            for code in 1..=5 {
                if code > 1 && lookups(before, code, &contexts).is_empty() {
                    break;
                }
                let (count, misses) = check(&disc, &data, &contexts, before, after, code);
                checked += count;
                for miss in misses {
                    lost.push(format!(
                        "{alias} {at:#x} language {code}: {before} -> {after}: {miss}"
                    ));
                }
                if count > 0 {
                    continue;
                }
                let extension = lower
                    .rsplit_once('.')
                    .map_or("", |(_, extension)| extension);
                if extension == "cfg" && !lower.contains('/') && character.is_none() {
                    // A profile of a character the script does not name beside it.
                    dynamic += 1;
                } else if ASSET_EXTENSIONS.contains(&extension) {
                    let name = language(before, code).unwrap_or_else(|| before.clone());
                    *unresolved.entry(format!("{alias}\t{name}")).or_default() += 1;
                }
            }
        }
    }
    eprintln!(
        "{checked} disc lookups of script names, {} lost; {} names the disc does not resolve in any \
         context; {dynamic} profile names without an adjacent character",
        lost.len(),
        unresolved.values().sum::<usize>()
    );
    for (name, count) in &unresolved {
        eprintln!("not resolved on the disc ({count}): {name}");
    }
    assert!(
        missing.is_empty() && lost.is_empty(),
        "converted sources without a counterpart:\n{}\nscript names lost:\n{}",
        missing.join("\n"),
        lost.join("\n")
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    fn namespace(files: &[&str], packs: &[(&str, &[(&str, &str)])], textures: bool) -> Namespace {
        Namespace {
            files: files.iter().map(|file| fold(file)).collect(),
            packs: packs
                .iter()
                .map(|(pack, members)| {
                    let members = members
                        .iter()
                        .map(|(name, path)| (fold(name), fold(path)))
                        .collect();
                    (fold(pack), members)
                })
                .collect(),
            textures,
            documents: BTreeMap::new(),
        }
    }

    #[test]
    fn a_standalone_model_needs_its_own_counterpart_beside_a_same_stem_character() {
        let sources = ["etc/gsys.pak/a_mu.chr", "etc/gsys.pak/a_mu.mds"];
        let disc = namespace(
            &sources,
            &[(
                "etc/gsys.pak",
                &[("a_mu.chr", sources[0]), ("a_mu.mds", sources[1])],
            )],
            false,
        );
        let character = ("a_mu.gltf", "etc/gsys.pak/a_mu.gltf");
        let model = ("a_mu.model.gltf", "etc/gsys.pak/a_mu.model.gltf");
        let both = namespace(
            &[character.1, model.1],
            &[("etc/gsys.pak", &[character, model])],
            true,
        );
        assert_eq!(missing_counterparts(sources, &disc, &both), (2, vec![]));
        // Only the character's glTF remains: the model's own counterpart is missing.
        let character_only = namespace(&[character.1], &[("etc/gsys.pak", &[character])], true);
        assert_eq!(
            missing_counterparts(sources, &disc, &character_only),
            (2, vec![sources[1].to_owned()])
        );
    }

    #[test]
    fn a_name_must_read_its_own_source_in_the_directory_the_disc_reads_it_from() {
        // d01b01_1.stb changes to both event directories; the Halloween operand is read in e110.
        let disc = namespace(
            &[
                "event/e100/e110/e110_halloween.chr",
                "event/e100/e111/e111_halloween.chr",
            ],
            &[],
            false,
        );
        let data = namespace(
            &[
                "event/e100/e110/e110_halloween.gltf",
                "event/e100/e111/e111_halloween.gltf",
            ],
            &[],
            true,
        );
        let contexts = [
            Context::Directory(String::new()),
            Context::Directory("event/e100/e110/".into()),
            Context::Directory("event/e100/e111/".into()),
        ];
        let checked = |after| check(&disc, &data, &contexts, "e110_Halloween.chr", after, 1);
        assert_eq!(checked("e110_Halloween.gltf"), (1, vec![]));
        // The e111 model exists in another of the script's directories, but not where e110's is read.
        let (count, lost) = checked("e111_Halloween.gltf");
        assert_eq!(count, 1);
        assert_eq!(lost.len(), 1, "{lost:?}");
        // A name that reads another source's counterpart in the same directory is lost too.
        let data = namespace(
            &[
                "event/e100/e110/e110_halloween.gltf",
                "event/e100/e110/e110_mints.gltf",
            ],
            &[],
            true,
        );
        let (count, lost) = check(
            &disc,
            &data,
            &contexts,
            "e110_Halloween.chr",
            "e110_mints.gltf",
            1,
        );
        assert_eq!((count, lost.len()), (1, 1), "{lost:?}");
    }

    #[test]
    fn a_name_must_read_the_pack_member_where_the_disc_reads_the_pack() {
        // A loose title card and a PACK member of the same name, as in event/e100/e110.
        let loose = "event/e100/e110/e110telop01.img";
        let member = "event/e100/e110/_e110_data.pak/e110telop01.img";
        let pack = "event/e100/e110/_e110_data.pak";
        let mut disc = namespace(
            &[loose, member, pack],
            &[(pack, &[("e110telop01.img", member)])],
            false,
        );
        let documents = [
            "textures/event/e100/e110/e110telop01.json",
            "textures/event/e100/e110/e110telop01-60b3624cea5e-0.json",
        ];
        disc.documents = BTreeMap::from([
            (loose.to_owned(), documents[0].to_owned()),
            (member.to_owned(), documents[1].to_owned()),
        ]);
        let sources = [loose, member];
        let contexts = [
            Context::Directory("event/e100/e110/".into()),
            Context::Pack(pack.into()),
        ];
        let marker = [("e110telop01.json", documents[1])];
        let data = namespace(&documents, &[(pack, &marker)], true);
        assert_eq!(
            check(
                &disc,
                &data,
                &contexts,
                "e110telop01.img",
                "e110telop01.json",
                1
            ),
            (2, vec![])
        );
        // Without the PACK's record the loose image still resolves, but not where the PACK is read.
        let data = namespace(&documents, &[(pack, &[])], true);
        let (count, lost) = check(
            &disc,
            &data,
            &contexts,
            "e110telop01.img",
            "e110telop01.json",
            1,
        );
        assert_eq!(count, 2);
        assert_eq!(lost.len(), 1, "{lost:?}");
        assert!(lost[0].starts_with("Pack("), "{lost:?}");
        assert_eq!(missing_counterparts(sources, &disc, &data).1, [member]);
        // The marker keeps its record, but the member's own document is missing from the archive.
        let data = namespace(&documents[..1], &[(pack, &marker)], true);
        let (_, lost) = check(
            &disc,
            &data,
            &contexts,
            "e110telop01.img",
            "e110telop01.json",
            1,
        );
        assert_eq!(lost.len(), 1, "{lost:?}");
        assert_eq!(missing_counterparts(sources, &disc, &data).1, [member]);
        // The marker sends the member to another existing texture, even to the loose copy.
        for other in ["textures/event/e700/e700/e700.json", documents[0]] {
            let files = [
                documents[0],
                documents[1],
                "textures/event/e700/e700/e700.json",
            ];
            let data = namespace(&files, &[(pack, &[("e110telop01.json", other)])], true);
            let (_, lost) = check(
                &disc,
                &data,
                &contexts,
                "e110telop01.img",
                "e110telop01.json",
                1,
            );
            assert_eq!(lost.len(), 1, "{other}: {lost:?}");
            assert_eq!(missing_counterparts(sources, &disc, &data).1, [member]);
        }
    }

    #[test]
    fn language_markers_and_map_scripts_resolve_like_the_game() {
        assert_eq!(
            language("e110telop01_1.json", 3).as_deref(),
            Some("e110telop01_3.json")
        );
        assert_eq!(language("e110telop01.json", 3), None);
        assert_eq!(
            map_script("d01b01/d01b01.stb").as_deref(),
            Some("map/d/d01/b01/d01b01_1.stb")
        );
        assert_eq!(map_script("snd2/bgm/bg_193.snd"), None);
    }
}
