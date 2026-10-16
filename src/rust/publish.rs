use super::zip_archive::{self, Archive, Input, content_digest};
use super::{CoreError, CoreResult, textures};
use serde_json::Value;
use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::path::{Component, Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}
fn files(root: &Path, dir: &Path, out: &mut BTreeSet<String>) -> CoreResult<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if fs::symlink_metadata(&path)?.file_type().is_symlink() {
            return Err(fail(format!(
                "asset symlink unsupported: {}",
                path.display()
            )));
        }
        if path.file_name().is_some_and(|name| {
            name.to_string_lossy()
                .to_ascii_lowercase()
                .ends_with(".img")
        }) {
            return Err(fail(format!(
                "legacy .img path remains: {}",
                path.display()
            )));
        }
        if path.is_dir() {
            files(root, &path, out)?;
        } else if path.is_file() {
            out.insert(
                path.strip_prefix(root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/"),
            );
        }
    }
    Ok(())
}

/// Removes obsolete indexes so published assets are addressed only by physical paths.
pub fn finalize_assets(root: &Path) -> CoreResult<()> {
    for name in [
        "data/manifest.json",
        "port_data/manifest.json",
        "port_data/catalog.json",
        "port_data/audio_catalog.json",
        "port_data/model_catalog.json",
        "port_data/cfg_catalog.json",
    ] {
        let path = root.join(name);
        if path.is_file() {
            fs::remove_file(path)?;
        }
    }
    Ok(())
}

fn check_asset(
    port: &Path,
    document: &str,
    reference: &str,
    expected: &BTreeSet<String>,
) -> CoreResult<()> {
    let reference_path = Path::new(reference);
    if reference.is_empty()
        || reference_path.is_absolute()
        || reference.chars().any(|c| matches!(c, '\\' | ':' | '\0'))
    {
        return Err(fail(format!("invalid texture asset path: {reference}")));
    }
    let base = Path::new(document)
        .parent()
        .ok_or_else(|| fail("document has no parent"))?;
    let mut parts = Vec::new();
    for component in base.join(reference_path).components() {
        match component {
            Component::Normal(part) => parts.push(part.to_string_lossy().into_owned()),
            Component::ParentDir => {
                if parts.pop().is_none() {
                    return Err(fail("texture asset escapes port_data"));
                }
            }
            Component::CurDir => {}
            _ => return Err(fail("invalid texture asset component")),
        }
    }
    let normalized = parts.join("/");
    if !expected.contains(&normalized) || !port.join(&normalized).is_file() {
        return Err(fail(format!(
            "missing texture asset: {document} -> {reference}"
        )));
    }
    Ok(())
}
fn verify_catalog(port: &Path, catalog: &Value, expected: &BTreeSet<String>) -> CoreResult<()> {
    if catalog["schema_version"] != 3 || !keys(catalog, &["schema_version", "sources"]) {
        return Err(fail("unsupported texture catalog version"));
    }
    let sources = catalog["sources"]
        .as_object()
        .ok_or_else(|| fail("invalid catalog sources"))?;
    let mut paths = BTreeSet::new();
    let mut aliases = BTreeSet::new();
    for (alias, source) in sources {
        let path = source
            .as_str()
            .ok_or_else(|| fail("catalog document path missing"))?;
        if !aliases.insert(alias.to_ascii_lowercase())
            || !paths.insert(path.to_ascii_lowercase())
            || textures::document_path(alias)? != path
            || !path.ends_with(".json")
            || !expected.contains(path)
        {
            return Err(fail(format!(
                "invalid or colliding texture document: {alias} -> {path}"
            )));
        }
        let document: Value = serde_json::from_slice(&fs::read(port.join(path))?)?;
        if document["schema_version"] != 3
            || !keys(&document, &["schema_version", "groups", "entries"])
        {
            return Err(fail(format!("invalid texture document: {path}")));
        }
        let groups = document["groups"]
            .as_array()
            .ok_or_else(|| fail("texture groups missing"))?;
        let mut prior = None;
        for group in groups {
            let number = group
                .as_u64()
                .ok_or_else(|| fail("invalid texture group"))?;
            if prior.is_some_and(|p| number <= p) {
                return Err(fail("texture groups not strictly increasing"));
            }
            prior = Some(number);
        }
        let entries = document["entries"]
            .as_array()
            .ok_or_else(|| fail("texture entries missing"))?;
        for entry in entries {
            if entry.get("script").is_some() {
                if !keys(entry, &["name", "script"])
                    || !entry["name"]
                        .as_str()
                        .is_some_and(|name| name.starts_with('#'))
                {
                    return Err(fail("invalid texture animation entry"));
                }
                let script = entry["script"]
                    .as_array()
                    .ok_or_else(|| fail("invalid texture script"))?;
                for statement in script {
                    if !keys(statement, &["command", "args"])
                        || statement["command"].as_str().is_none()
                    {
                        return Err(fail("invalid texture script statement"));
                    }
                    let args = statement["args"]
                        .as_array()
                        .ok_or_else(|| fail("invalid texture script arguments"))?;
                    for arg in args {
                        if !(keys(arg, &["text"]) && arg["text"].is_string()
                            || keys(arg, &["number"]) && arg["number"].is_number())
                        {
                            return Err(fail("invalid texture script argument"));
                        }
                    }
                }
                continue;
            }
            if !keys_optional(entry, &["name", "pictures"], &["block"])
                || entry["name"].as_str().is_none()
            {
                return Err(fail("invalid texture entry"));
            }
            let block = entry
                .get("block")
                .map(|v| v.as_u64().ok_or_else(|| fail("invalid texture block")))
                .transpose()?
                .unwrap_or(0);
            if !groups.iter().any(|g| g.as_u64() == Some(block)) {
                return Err(fail("texture block outside groups"));
            }
            let pictures = entry["pictures"]
                .as_array()
                .ok_or_else(|| fail("texture pictures missing"))?;
            if pictures.is_empty() {
                return Err(fail("texture entry has no pictures"));
            }
            for picture in pictures {
                if !keys_optional(
                    picture,
                    &[],
                    &["levels", "canvas", "variants", "sampler", "animation"],
                ) || picture.get("levels").is_some() == picture.get("canvas").is_some()
                {
                    return Err(fail("invalid texture picture"));
                }
                if let Some(levels) = picture.get("levels") {
                    check_levels(port, path, levels, expected)?;
                }
                if let Some(canvas) = picture.get("canvas") {
                    if !keys(canvas, &["width", "height"])
                        || canvas["width"].as_u64().is_none_or(|n| n == 0)
                        || canvas["height"].as_u64().is_none_or(|n| n == 0)
                    {
                        return Err(fail("invalid texture canvas"));
                    }
                }
                if let Some(animation) = picture.get("animation") {
                    if !matches!(animation.as_str(), Some("draw" | "copy" | "copy_variant")) {
                        return Err(fail("invalid texture animation"));
                    }
                }
                if let Some(sampler) = picture.get("sampler") {
                    if !keys_optional(sampler, &[], &["wrap_u", "wrap_v", "filter"]) {
                        return Err(fail("invalid texture sampler"));
                    }
                    for key in ["wrap_u", "wrap_v"] {
                        if sampler
                            .get(key)
                            .is_some_and(|v| !matches!(v.as_str(), Some("repeat" | "clamp")))
                        {
                            return Err(fail("invalid texture wrap"));
                        }
                    }
                    if sampler
                        .get("filter")
                        .is_some_and(|v| !matches!(v.as_str(), Some("linear" | "nearest")))
                    {
                        return Err(fail("invalid texture filter"));
                    }
                }
                if let Some(variants) = picture.get("variants") {
                    let variants = variants
                        .as_array()
                        .ok_or_else(|| fail("invalid texture variants"))?;
                    if variants.is_empty() {
                        return Err(fail("empty texture variants"));
                    }
                    let mut selectors = BTreeSet::new();
                    for variant in variants {
                        if !keys(variant, &["selector", "levels"]) {
                            return Err(fail("invalid texture variant"));
                        }
                        let selector = variant["selector"]
                            .as_str()
                            .filter(|s| !s.is_empty())
                            .ok_or_else(|| fail("invalid texture selector"))?;
                        if selector == entry["name"].as_str().unwrap()
                            || !selectors.insert(selector)
                        {
                            return Err(fail("duplicate texture selector"));
                        }
                        check_levels(port, path, &variant["levels"], expected)?;
                    }
                }
            }
        }
    }
    Ok(())
}

/// Validates one directly addressed texture document and its PNG dependencies.
pub(crate) fn verify_texture_document(
    port: &Path,
    path: &str,
    expected: &BTreeSet<String>,
) -> CoreResult<()> {
    verify_catalog(
        port,
        &serde_json::json!({"schema_version":3,"sources":{(path):path}}),
        expected,
    )
}

fn keys(value: &Value, required: &[&str]) -> bool {
    keys_optional(value, required, &[])
}
fn keys_optional(value: &Value, required: &[&str], optional: &[&str]) -> bool {
    value.as_object().is_some_and(|map| {
        required.iter().all(|key| map.contains_key(*key))
            && map
                .keys()
                .all(|key| required.contains(&key.as_str()) || optional.contains(&key.as_str()))
    })
}
fn check_levels(
    port: &Path,
    document: &str,
    value: &Value,
    expected: &BTreeSet<String>,
) -> CoreResult<()> {
    let levels = value
        .as_array()
        .filter(|v| !v.is_empty())
        .ok_or_else(|| fail("texture levels missing"))?;
    for level in levels {
        let reference = level
            .as_str()
            .ok_or_else(|| fail("invalid texture level path"))?;
        if !reference.to_ascii_lowercase().ends_with(".png") {
            return Err(fail("texture level is not PNG"));
        }
        check_asset(port, document, reference, expected)?;
    }
    Ok(())
}
/// Validates staged `data` and `port_data` trees before they are packed.
pub fn verify_trees(root: &Path) -> CoreResult<u64> {
    let port = root.join("port_data");
    let data = root.join("data");
    let mut port_files = BTreeSet::new();
    files(&port, &port, &mut port_files)?;
    let mut data_files = BTreeSet::new();
    files(&data, &data, &mut data_files)?;
    for relative in &data_files {
        let path = data.join(relative);
        if relative.to_ascii_lowercase().ends_with(".chr") {
            return Err(fail(format!(
                "legacy character container remains: {relative}"
            )));
        }
        if fs::metadata(&path)?.len() <= 128 << 20 {
            textures::audit_payload(&fs::read(path)?, relative)?;
        } else if !textures::large_known_non_texture(relative) {
            return Err(fail(format!("unrecognized large data file: {relative}")));
        }
    }
    for relative in &port_files {
        if matches!(
            relative.as_str(),
            "catalog.json"
                | "audio_catalog.json"
                | "model_catalog.json"
                | "cfg_catalog.json"
                | "manifest.json"
        ) {
            return Err(fail(format!("obsolete runtime index remains: {relative}")));
        }
        if relative.ends_with(".json") {
            let document: Value = serde_json::from_slice(&fs::read(port.join(relative))?)?;
            if document["schema_version"] == 3
                && document.get("groups").is_some()
                && document.get("entries").is_some()
            {
                verify_texture_document(&port, relative, &port_files)?;
            }
        }
    }
    super::models::verify(root)?;
    super::audio::verify(root)?;
    super::configs::verify(root)?;
    super::packs::verify(root)?;
    Ok(port_files
        .iter()
        .filter(|name| name.ends_with(".png"))
        .count() as u64)
}
fn unique(root: &Path, kind: &str) -> PathBuf {
    let ticks = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    root.join(format!(".dc2data-{kind}-{}-{ticks}", std::process::id()))
}
pub fn create_stage(root: &Path) -> CoreResult<PathBuf> {
    let stage = unique(root, "stage");
    fs::create_dir(&stage)?;
    Ok(stage)
}
pub struct StageGuard {
    path: PathBuf,
    _lock: File,
}
impl StageGuard {
    pub fn path(&self) -> &Path {
        &self.path
    }
}
fn lock_stage(path: &Path) -> CoreResult<StageGuard> {
    let lock = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(path.join(".lock"))?;
    lock.try_lock().map_err(|e| fail(e.to_string()))?;
    Ok(StageGuard {
        path: path.to_path_buf(),
        _lock: lock,
    })
}

pub fn acquire_stage(root: &Path, metadata: &Value) -> CoreResult<StageGuard> {
    fs::create_dir_all(root)?;
    // Directory order is unspecified (APFS differs from ext4 and NTFS); try
    // matching stages in name order so reuse does not depend on the host.
    let mut paths = fs::read_dir(root)?
        .map(|entry| entry.map(|entry| entry.path()))
        .collect::<Result<Vec<_>, _>>()?;
    paths.sort();
    for path in paths {
        if !path.is_dir()
            || !path
                .file_name()
                .is_some_and(|n| n.to_string_lossy().starts_with(".dc2data-stage-"))
        {
            continue;
        }
        let prior = match fs::read(path.join("stage.json"))
            .ok()
            .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        {
            Some(prior) => prior,
            None => continue,
        };
        if &prior != metadata {
            continue;
        }
        if let Ok(stage) = lock_stage(&path) {
            return Ok(stage);
        }
    }
    let path = create_stage(root)?;
    let guard = lock_stage(&path)?;
    let mut bytes = serde_json::to_vec_pretty(metadata)?;
    bytes.push(b'\n');
    fs::write(guard.path.join("stage.json"), bytes)?;
    Ok(guard)
}
#[cfg(windows)]
fn rename_no_replace(from: &Path, to: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn MoveFileExW(from: *const u16, to: *const u16, flags: u32) -> i32;
    }
    let from: Vec<u16> = from.as_os_str().encode_wide().chain([0]).collect();
    let to: Vec<u16> = to.as_os_str().encode_wide().chain([0]).collect();
    if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), 0) } == 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}

#[cfg(not(windows))]
fn rename_no_replace(from: &Path, to: &Path) -> std::io::Result<()> {
    if to.exists() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::AlreadyExists,
            "publication destination already exists",
        ));
    }
    fs::rename(from, to)
}

fn rename_retry(from: &Path, to: &Path) -> CoreResult<()> {
    fn state(path: &Path) -> String {
        match fs::symlink_metadata(path) {
            Ok(metadata) if metadata.file_type().is_dir() => "directory".into(),
            Ok(metadata) if metadata.file_type().is_file() => "file".into(),
            Ok(metadata) if metadata.file_type().is_symlink() => "symlink".into(),
            Ok(_) => "other".into(),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => "absent".into(),
            Err(error) => format!("metadata error: {error}"),
        }
    }
    for attempt in 0..30 {
        match rename_no_replace(from, to) {
            Ok(()) => return Ok(()),
            Err(error)
                if attempt < 29
                    && matches!(
                        error.kind(),
                        std::io::ErrorKind::PermissionDenied | std::io::ErrorKind::InvalidInput
                    ) =>
            {
                std::thread::sleep(Duration::from_secs(1));
            }
            Err(error) => {
                return Err(fail(format!(
                    "rename {} -> {}: {error} (source: {}; destination: {})",
                    from.display(),
                    to.display(),
                    state(from),
                    state(to)
                )));
            }
        }
    }
    unreachable!()
}
/// File name of the published asset archive beside the marker.
pub const ARCHIVE: &str = "data.zip";
/// File name of the published marker.
pub const MARKER: &str = "data.json";
/// Published layout recorded in the marker.
pub const LAYOUT: &str = "zip";
/// Root-level index files that runtime lookups no longer use.
const OBSOLETE_INDEXES: [&str; 5] = [
    "manifest.json",
    "catalog.json",
    "audio_catalog.json",
    "model_catalog.json",
    "cfg_catalog.json",
];
/// Converted formats that cannot carry proprietary texture payloads.
const CONVERTED_EXTENSIONS: [&str; 5] = [".png", ".mp4", ".wav", ".gltf", ".json"];

/// Lists the staged `data` and `port_data` files as one merged archive namespace.
pub fn tree_inputs(root: &Path) -> CoreResult<Vec<Input>> {
    let mut inputs = Vec::new();
    for tree in ["data", "port_data"] {
        let base = root.join(tree);
        if base.is_dir() {
            inputs.extend(zip_archive::directory_inputs(&base)?);
        }
    }
    Ok(inputs)
}

/// Packs verified staged trees into `data.zip`, deleting each file once it is written.
pub fn pack_trees(
    root: &Path,
    progress: &(dyn Fn(u64, u64) + Sync),
) -> CoreResult<zip_archive::Written> {
    let written = zip_archive::write(tree_inputs(root)?, &root.join(ARCHIVE), true, progress)?;
    for tree in ["data", "port_data"] {
        let base = root.join(tree);
        if base.is_dir() {
            fs::remove_dir_all(base)?;
        }
    }
    Ok(written)
}

/// Packs staged trees and writes the marker describing the archive.
pub fn seal(
    root: &Path,
    mut marker: Value,
    progress: &(dyn Fn(u64, u64) + Sync),
) -> CoreResult<zip_archive::Written> {
    let written = pack_trees(root, progress)?;
    marker["layout"] = LAYOUT.into();
    marker["archive_size"] = written.size.into();
    marker["archive_entries"] = written.entries.into();
    marker["output_sha256"] = written.content_sha256.clone().into();
    let mut bytes = serde_json::to_vec_pretty(&marker)?;
    bytes.push(b'\n');
    fs::write(root.join(MARKER), bytes)?;
    Ok(written)
}

/// Applies the published-asset rules that need only one entry's name and bytes.
fn audit_entry(name: &str, bytes: &[u8]) -> CoreResult<()> {
    let lower = name.to_ascii_lowercase();
    if lower.split('/').any(|part| part.ends_with(".img")) {
        return Err(fail(format!("legacy .img path remains: {name}")));
    }
    if lower.ends_with(".chr") {
        return Err(fail(format!("legacy character container remains: {name}")));
    }
    if OBSOLETE_INDEXES.contains(&name) {
        return Err(fail(format!("obsolete runtime index remains: {name}")));
    }
    if matches!(lower.as_str(), "snd2/rev.bin" | "snd2/map_index.txt") {
        return Err(fail(format!(
            "proprietary scene audio table published: {name}"
        )));
    }
    super::packs::audit_entry(name, bytes)?;
    if !CONVERTED_EXTENSIONS.iter().any(|ext| lower.ends_with(ext)) {
        if bytes.len() as u64 <= 128 << 20 {
            textures::audit_payload(bytes, name)?;
        } else if !textures::large_known_non_texture(name) {
            return Err(fail(format!("unrecognized large data file: {name}")));
        }
    }
    Ok(())
}

/// Reads a marker describing the current data version and archive layout.
pub fn read_marker(root: &Path) -> CoreResult<Value> {
    let marker: Value = serde_json::from_slice(&fs::read(root.join(MARKER))?)?;
    if marker["schema_version"] != 1 || marker["data_version"] != super::data_version() {
        return Err(fail("data.json has an unsupported data version"));
    }
    if marker["layout"] != LAYOUT {
        return Err(fail("data.json does not describe a data.zip layout"));
    }
    Ok(marker)
}

/// Confirms that `data.zip` exists with the size recorded in the marker.
pub fn archive_present(root: &Path, marker: &Value) -> bool {
    marker["archive_size"].as_u64().is_some_and(|size| {
        fs::metadata(root.join(ARCHIVE)).is_ok_and(|meta| meta.is_file() && meta.len() == size)
    })
}

/// Fully verifies a published archive against its marker, returning its PNG count.
pub fn verify(root: &Path) -> CoreResult<u64> {
    let marker = read_marker(root)?;
    if !archive_present(root, &marker) {
        return Err(fail(
            "data.zip is missing or its size does not match data.json",
        ));
    }
    let archive = Archive::open(&root.join(ARCHIVE))?;
    let mut records = archive.verify_entries(&audit_entry)?;
    if marker["archive_entries"].as_u64() != Some(records.len() as u64) {
        return Err(fail("data.zip entry count does not match data.json"));
    }
    if Some(content_digest(&mut records).as_str()) != marker["output_sha256"].as_str() {
        return Err(fail("published output changed"));
    }
    Ok(records
        .iter()
        .filter(|record| record.name.ends_with(".png"))
        .count() as u64)
}

/// Verifies a staged archive and publishes it.
pub fn publish(staged: &Path, root: &Path) -> CoreResult<()> {
    verify(staged)?;
    publish_verified(staged, root)
}

/// Moves a verified staged archive and marker into place, replacing earlier data of any layout.
pub fn publish_verified(staged: &Path, root: &Path) -> CoreResult<()> {
    fs::create_dir_all(root)?;
    let staged_archive = staged.join(ARCHIVE);
    let staged_marker = staged.join(MARKER);
    let archive = root.join(ARCHIVE);
    let marker = root.join(MARKER);
    let legacy_data = root.join("data");
    let legacy_port = root.join("port_data");
    let mut displaced: Vec<(PathBuf, PathBuf)> = Vec::new();
    let mut placed: Vec<(PathBuf, PathBuf)> = Vec::new();
    let result = (|| -> CoreResult<()> {
        for (original, kind) in [
            (&marker, "marker-backup"),
            (&archive, "archive-backup"),
            (&legacy_data, "data-backup"),
            (&legacy_port, "port-data-backup"),
        ] {
            if fs::symlink_metadata(original).is_ok() {
                let backup = unique(root, kind);
                rename_retry(original, &backup)?;
                displaced.push((original.clone(), backup));
            }
        }
        for (from, to) in [(&staged_archive, &archive), (&staged_marker, &marker)] {
            rename_retry(from, to)?;
            placed.push((from.clone(), to.clone()));
        }
        let published = read_marker(root)?;
        if !archive_present(root, &published) {
            return Err(fail("published data.zip size does not match data.json"));
        }
        Archive::open(&archive)?;
        Ok(())
    })();
    if let Err(error) = result {
        let mut rollback = Vec::new();
        for (from, to) in placed.iter().rev() {
            if let Err(e) = rename_retry(to, from) {
                rollback.push(e.to_string());
            }
        }
        for (original, backup) in displaced.iter().rev() {
            if let Err(e) = rename_retry(backup, original) {
                rollback.push(e.to_string());
            }
        }
        return Err(if rollback.is_empty() {
            error
        } else {
            fail(format!("{error}; rollback: {}", rollback.join("; ")))
        });
    }
    for (_, backup) in displaced {
        if backup.is_dir() {
            fs::remove_dir_all(backup)?;
        } else {
            fs::remove_file(backup)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn texture_verifier_rejects_old_fields_and_missing_png() {
        let temp = tempfile::tempdir().unwrap();
        let port = temp.path();
        let catalog = serde_json::json!({"schema_version":3,"sources":{"foo.img":"foo.json"}});
        let assets = BTreeSet::from(["foo.json".to_owned(), "foo_0.png".to_owned()]);
        fs::write(port.join("foo_0.png"), b"png").unwrap();
        let mut doc = serde_json::json!({"schema_version":3,"groups":[0],"entries":[
            {"name":"foo","pictures":[{"levels":["foo_0.png"]}]}
        ]});
        fs::write(port.join("foo.json"), serde_json::to_vec(&doc).unwrap()).unwrap();
        verify_catalog(port, &catalog, &assets).unwrap();
        doc["original"] = serde_json::json!({"bytes":"00"});
        fs::write(port.join("foo.json"), serde_json::to_vec(&doc).unwrap()).unwrap();
        assert!(verify_catalog(port, &catalog, &assets).is_err());
        doc.as_object_mut().unwrap().remove("original");
        doc["entries"][0]["pictures"][0]["levels"][0] = "missing.png".into();
        fs::write(port.join("foo.json"), serde_json::to_vec(&doc).unwrap()).unwrap();
        assert!(verify_catalog(port, &catalog, &assets).is_err());
    }

    #[test]
    fn texture_verifier_accepts_script_only_empty_groups() {
        let temp = tempfile::tempdir().unwrap();
        let port = temp.path();
        let catalog = serde_json::json!({"schema_version":3,"sources":{"cfg.img":"cfg.json"}});
        let assets = BTreeSet::from(["cfg.json".to_owned()]);
        let doc = serde_json::json!({"schema_version":3,"groups":[],"entries":[
            {"name":"#texanime","script":[{"command":"TEX_ANIME_DATA_END","args":[]}]}
        ]});
        fs::write(port.join("cfg.json"), serde_json::to_vec(&doc).unwrap()).unwrap();
        verify_catalog(port, &catalog, &assets).unwrap();
    }

    fn staged_trees(root: &Path) {
        let data = root.join("data");
        let port = root.join("port_data");
        fs::create_dir_all(&data).unwrap();
        fs::create_dir_all(port.join("audio")).unwrap();
        fs::write(data.join("script.stb"), b"unrelated script").unwrap();
        fs::write(port.join("audio/sample.wav"), b"RIFF converted PCM").unwrap();
        fs::write(port.join("audio/sample.png"), b"png").unwrap();
        fs::write(port.join("manifest.json"), b"stale manifest").unwrap();
    }

    fn marker() -> Value {
        serde_json::json!({"schema_version":1,"data_version":super::super::data_version()})
    }

    #[test]
    fn sealed_archive_merges_trees_and_detects_changes() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        staged_trees(root);
        finalize_assets(root).unwrap();
        assert!(!root.join("port_data/manifest.json").exists());
        let written = seal(root, marker(), &|_, _| {}).unwrap();
        assert!(!root.join("data").exists() && !root.join("port_data").exists());
        assert_eq!(written.entries, 3);
        assert_eq!(verify(root).unwrap(), 1);
        let archive = Archive::open(&root.join(ARCHIVE)).unwrap();
        let names: Vec<&str> = (0..archive.len()).map(|i| archive.name(i)).collect();
        assert_eq!(
            names,
            ["audio/sample.png", "audio/sample.wav", "script.stb"]
        );
        drop(archive);
        let mut changed = read_marker(root).unwrap();
        changed["output_sha256"] = "0".repeat(64).into();
        fs::write(root.join(MARKER), serde_json::to_vec(&changed).unwrap()).unwrap();
        assert!(verify(root).unwrap_err().to_string().contains("changed"));
        changed["data_version"] = 8.into();
        fs::write(root.join(MARKER), serde_json::to_vec(&changed).unwrap()).unwrap();
        assert!(verify(root).is_err());
    }

    #[test]
    fn publish_replaces_legacy_directories_with_archive() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("run");
        let staged = temp.path().join("stage");
        staged_trees(&staged);
        finalize_assets(&staged).unwrap();
        seal(&staged, marker(), &|_, _| {}).unwrap();
        fs::create_dir_all(root.join("data")).unwrap();
        fs::create_dir_all(root.join("port_data/old")).unwrap();
        fs::write(root.join("port_data/old/file.bin"), b"old").unwrap();
        fs::write(root.join(MARKER), b"{\"data_version\":8}").unwrap();
        publish(&staged, &root).unwrap();
        assert!(!root.join("data").exists() && !root.join("port_data").exists());
        assert!(root.join(ARCHIVE).is_file());
        assert!(!staged.join(ARCHIVE).exists());
        assert_eq!(verify(&root).unwrap(), 1);
        let leftovers: Vec<_> = fs::read_dir(&root)
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        assert_eq!(leftovers.len(), 2, "{leftovers:?}");
    }

    #[test]
    fn audit_rejects_proprietary_and_obsolete_entries() {
        assert!(audit_entry("catalog.json", b"{}").is_err());
        assert!(audit_entry("models/foo.img/a.json", b"{}").is_err());
        assert!(audit_entry("snd2/rev.bin", b"").is_err());
        assert!(audit_entry("x/pack.json", b"{}").is_err());
        assert!(
            audit_entry(
                "x/pack.json",
                br#"{"schema_version":1,"kind":"asset_directory"}"#
            )
            .is_ok()
        );
        assert!(audit_entry("script.stb", b"plain").is_ok());
    }

    #[test]
    fn rename_existing_nonempty_directory_to_absent_backup() {
        let temp = tempfile::tempdir().unwrap();
        let original = temp.path().join("port_data");
        let backup = temp.path().join(".dc2data-port-data-backup-test");
        fs::create_dir(&original).unwrap();
        fs::write(original.join("catalog.json"), b"test").unwrap();
        rename_retry(&original, &backup).unwrap();
        assert_eq!(fs::read(backup.join("catalog.json")).unwrap(), b"test");
        rename_retry(&backup, &original).unwrap();
        assert_eq!(fs::read(original.join("catalog.json")).unwrap(), b"test");
    }

    #[test]
    fn rename_staged_directory_to_ancestor_sibling() {
        let temp = tempfile::tempdir().unwrap();
        let staged = temp.path().join(".dc2data-stage-test/published/port_data");
        let destination = temp.path().join("port_data");
        fs::create_dir_all(&staged).unwrap();
        fs::write(staged.join("catalog.json"), b"staged").unwrap();
        rename_no_replace(&staged, &destination).unwrap();
        assert_eq!(
            fs::read(destination.join("catalog.json")).unwrap(),
            b"staged"
        );
        assert!(!staged.exists());
    }

    #[test]
    fn rename_never_overwrites_existing_destination() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        let destination = temp.path().join("destination");
        fs::create_dir(&source).unwrap();
        fs::create_dir(&destination).unwrap();
        fs::write(source.join("source.txt"), b"source data").unwrap();
        fs::write(destination.join("destination.txt"), b"existing data").unwrap();

        assert!(rename_no_replace(&source, &destination).is_err());
        assert_eq!(fs::read(source.join("source.txt")).unwrap(), b"source data");
        assert_eq!(
            fs::read(destination.join("destination.txt")).unwrap(),
            b"existing data"
        );
        let message = rename_retry(&source, &destination).unwrap_err().to_string();
        assert!(message.contains("source: directory; destination: directory"));
    }
}
