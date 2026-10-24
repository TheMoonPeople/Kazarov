use super::artifacts::AssetSink;
use super::inventory::Inventory;
use super::progress::Phase;
use super::zip_archive::{self, Archive, Input, ProgressiveWriter, content_digest};
use super::{CoreError, CoreResult, textures};
use rayon::prelude::*;
use serde_json::Value;
use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::collections::HashSet;
use std::fs::{self, File, OpenOptions};
use std::path::{Component, Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}

/// Read-only named asset access used by final cross-document validators.
pub trait ArchiveReader: Sync {
    fn names(&self) -> Vec<String>;
    fn contains(&self, name: &str) -> bool;
    fn read(&self, name: &str) -> CoreResult<Vec<u8>>;
}

/// Indexed view of one complete ZIP archive without a staged asset tree.
pub struct ArchiveView<'a> {
    archive: &'a Archive,
    names: BTreeMap<String, usize>,
}

impl<'a> ArchiveView<'a> {
    pub fn new(archive: &'a Archive) -> Self {
        Self {
            archive,
            names: (0..archive.len())
                .map(|index| (archive.name(index).to_owned(), index))
                .collect(),
        }
    }
}

impl ArchiveReader for ArchiveView<'_> {
    fn names(&self) -> Vec<String> {
        self.names.keys().cloned().collect()
    }

    fn contains(&self, name: &str) -> bool {
        self.names.contains_key(name)
    }

    fn read(&self, name: &str) -> CoreResult<Vec<u8>> {
        let index = self
            .names
            .get(name)
            .ok_or_else(|| fail(format!("archive asset missing: {name}")))?;
        self.archive.read(*index)
    }
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
        "data/catalog.json",
        "data/audio_catalog.json",
        "data/model_catalog.json",
        "data/cfg_catalog.json",
    ] {
        let path = root.join(name);
        if path.is_file() {
            fs::remove_file(path)?;
        }
    }
    Ok(())
}

/// Starts the normal archive stream without creating a staged asset directory.
pub fn begin_stream(
    root: &Path,
    budget_bytes: usize,
    phase: Phase,
    total_work: u64,
) -> CoreResult<AssetSink> {
    if root.join("data").exists() || root.join("port_data").exists() {
        return Err(fail("streaming publication cannot use a staged asset tree"));
    }
    AssetSink::start_with_progress(root, budget_bytes, phase, total_work)
}

/// Completes a streamed ZIP and records its content identity beside the archive.
pub fn seal_stream(
    sink: AssetSink,
    root: &Path,
    mut marker: Value,
) -> CoreResult<zip_archive::Written> {
    let written = sink.finish()?;
    marker["layout"] = LAYOUT.into();
    marker["archive_size"] = written.size.into();
    marker["archive_entries"] = written.entries.into();
    marker["output_sha256"] = written.content_sha256.clone().into();
    let mut bytes = serde_json::to_vec_pretty(&marker)?;
    bytes.push(b'\n');
    fs::write(root.join(MARKER), bytes)?;
    Ok(written)
}

fn check_asset(
    exists: &dyn Fn(&str) -> bool,
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
                    return Err(fail("texture asset escapes data"));
                }
            }
            Component::CurDir => {}
            _ => return Err(fail("invalid texture asset component")),
        }
    }
    let normalized = parts.join("/");
    if !expected.contains(&normalized) || !exists(&normalized) {
        return Err(fail(format!(
            "missing texture asset: {document} -> {reference}"
        )));
    }
    Ok(())
}
#[cfg(test)]
fn verify_catalog(port: &Path, catalog: &Value, expected: &BTreeSet<String>) -> CoreResult<()> {
    verify_catalog_with_document(
        &|name| Ok(fs::read(port.join(name))?),
        &|name| port.join(name).is_file(),
        catalog,
        expected,
        None,
    )
}

fn verify_catalog_with_document(
    read: &dyn Fn(&str) -> CoreResult<Vec<u8>>,
    exists: &dyn Fn(&str) -> bool,
    catalog: &Value,
    expected: &BTreeSet<String>,
    loaded: Option<&Value>,
) -> CoreResult<()> {
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
            || !path.starts_with("textures/")
            || !path.ends_with(".json")
            || !expected.contains(path)
        {
            return Err(fail(format!(
                "invalid or colliding texture document: {alias} -> {path}"
            )));
        }
        let owned;
        let document = if let Some(document) = loaded {
            document
        } else {
            owned = serde_json::from_slice::<Value>(&read(path)?)?;
            &owned
        };
        if document["schema_version"] != 3
            || !keys(document, &["schema_version", "groups", "entries"])
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
                    check_levels(exists, path, levels, expected)?;
                }
                if let Some(canvas) = picture.get("canvas")
                    && (!keys(canvas, &["width", "height"])
                        || canvas["width"].as_u64().is_none_or(|n| n == 0)
                        || canvas["height"].as_u64().is_none_or(|n| n == 0))
                {
                    return Err(fail("invalid texture canvas"));
                }
                if let Some(animation) = picture.get("animation")
                    && !matches!(animation.as_str(), Some("draw" | "copy" | "copy_variant"))
                {
                    return Err(fail("invalid texture animation"));
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
                        check_levels(exists, path, &variant["levels"], expected)?;
                    }
                }
            }
        }
    }
    Ok(())
}

fn verify_texture_value(
    port: &Path,
    path: &str,
    document: &Value,
    expected: &BTreeSet<String>,
) -> CoreResult<()> {
    verify_catalog_with_document(
        &|name| Ok(fs::read(port.join(name))?),
        &|name| port.join(name).is_file(),
        &serde_json::json!({"schema_version":3,"sources":{(path):path}}),
        expected,
        Some(document),
    )
}

fn verify_texture_archive_value(
    reader: &dyn ArchiveReader,
    path: &str,
    document: &Value,
    expected: &BTreeSet<String>,
) -> CoreResult<()> {
    verify_catalog_with_document(
        &|name| reader.read(name),
        &|name| reader.contains(name),
        &serde_json::json!({"schema_version":3,"sources":{(path):path}}),
        expected,
        Some(document),
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
    exists: &dyn Fn(&str) -> bool,
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
        check_asset(exists, document, reference, expected)?;
    }
    Ok(())
}
/// Validates the unified staged `data` tree before it is archived.
pub fn verify_trees(root: &Path, phase: &Phase) -> CoreResult<u64> {
    let data = root.join("data");
    let mut data_files = BTreeSet::new();
    files(&data, &data, &mut data_files)?;
    phase.set_total(
        data_files.len() as u64
            + super::models::verify_work(&data_files)
            + super::audio::verify_work(&data_files)
            + 1,
    );
    let data_checks = data_files
        .par_iter()
        .map(|relative| -> CoreResult<()> {
            let path = data.join(relative);
            if fs::metadata(&path)?.len() <= 128 << 20 {
                let bytes = fs::read(path)?;
                audit_entry(relative, &bytes)?;
                if relative.ends_with(".json") {
                    let document: Value = serde_json::from_slice(&bytes)?;
                    if document["schema_version"] == 3
                        && document.get("groups").is_some()
                        && document.get("entries").is_some()
                    {
                        verify_texture_value(&data, relative, &document, &data_files)?;
                    }
                    if relative.ends_with("/pack.json") {
                        verify_pack_textures(&document, &|path| Ok(fs::read(data.join(path))?))?;
                    }
                }
            } else {
                let lower = relative.to_ascii_lowercase();
                if external_config_document(relative)
                    || lower.ends_with(".chr")
                    || OBSOLETE_INDEXES.contains(&relative.as_str())
                    || matches!(lower.as_str(), "snd2/rev.bin" | "snd2/map_index.txt")
                {
                    return Err(fail(format!("obsolete asset remains: {relative}")));
                }
                if !CONVERTED_EXTENSIONS.iter().any(|ext| lower.ends_with(ext))
                    && !textures::large_known_non_texture(relative)
                {
                    return Err(fail(format!("unrecognized large data file: {relative}")));
                }
                // Large converted assets can still carry proprietary containers.
                audit_entry(relative, &fs::read(&path)?)?;
            }
            phase.inc(1);
            Ok(())
        })
        .collect::<Vec<CoreResult<()>>>();
    data_checks.into_iter().collect::<CoreResult<()>>()?;
    let ((models, audio), configs) = rayon::join(
        || {
            rayon::join(
                || super::models::verify_files(root, &data_files, phase),
                || super::audio::verify_files(root, &data_files, phase),
            )
        },
        || {
            let result = super::configs::verify_loose(root);
            phase.inc(1);
            result
        },
    );
    models?;
    audio?;
    configs?;
    Ok(data_files
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

fn stage_metadata_matches(prior: &Value, current: &Value) -> bool {
    if prior == current {
        return true;
    }
    let Some(current_policy) = current
        .get("script_publication_policy")
        .and_then(Value::as_str)
    else {
        return false;
    };
    if current_policy != "Retain" || prior.get("script_publication_policy").is_some() {
        return false;
    }
    let Some(prior_object) = prior.as_object() else {
        return false;
    };
    let mut normalized = prior_object.clone();
    normalized.insert(
        "script_publication_policy".into(),
        Value::String("Retain".into()),
    );
    Value::Object(normalized) == *current
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
        if !stage_metadata_matches(&prior, metadata) {
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

/// Lists files in the unified staged `data` tree.
pub fn tree_inputs(root: &Path) -> CoreResult<Vec<Input>> {
    let inputs = zip_archive::directory_inputs(&root.join("data"))?;
    for input in &inputs {
        if external_config_document(&input.name) {
            return Err(fail(format!(
                "external config document staged inside data: {}",
                input.name
            )));
        }
    }
    Ok(inputs)
}

fn external_config_document(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    if super::configs::gameplay_digest(&lower).is_some() {
        return false;
    }
    lower.starts_with("gameplay/")
        || super::configs::is_tracked_gameplay(&lower)
        || lower.ends_with(".cfg.json")
        || lower.ends_with(".map.json")
        || lower.ends_with(".character.json")
        || lower.ends_with(".sound/config.json")
        || lower.ends_with(".sound/volume.json")
        || matches!(lower.as_str(), "snd2/rev.json" | "snd2/map_index.json")
}

/// Builds an archive from finished files while conversion continues elsewhere.
pub struct ProgressiveArchive {
    writer: ProgressiveWriter,
    appended_sources: Vec<(PathBuf, u64, SystemTime)>,
    progress_units: u64,
}

const ARCHIVE_PROGRESS_UNITS: u64 = 10_000;

fn archive_progress(start: u64, end: u64, done: u64, total: u64) -> u64 {
    if total == 0 {
        return end;
    }
    start + ((end - start) as u128 * done.min(total) as u128 / total as u128) as u64
}

impl ProgressiveArchive {
    /// Creates an unpublished partial archive in the stage directory.
    pub fn begin(root: &Path) -> CoreResult<Self> {
        fs::create_dir_all(root)?;
        Ok(Self {
            writer: ProgressiveWriter::new(&root.join(ARCHIVE))?,
            appended_sources: Vec::new(),
            progress_units: 0,
        })
    }

    /// Adds only caller-confirmed immutable files from the staged data tree.
    pub fn append_inputs(
        &mut self,
        inputs: Vec<Input>,
        phase: &Phase,
        progress_end: u64,
    ) -> CoreResult<()> {
        let progress_start = self.progress_units;
        if progress_end < progress_start || progress_end > ARCHIVE_PROGRESS_UNITS {
            return Err(fail("archive progress batch is out of order"));
        }
        phase.set_total(ARCHIVE_PROGRESS_UNITS);
        for input in &inputs {
            if external_config_document(&input.name) {
                return Err(fail(format!(
                    "external config document staged inside data: {}",
                    input.name
                )));
            }
        }
        let sources = inputs
            .iter()
            .map(|input| input.source.clone())
            .collect::<Vec<_>>();
        self.writer.append(inputs, &|done, total| {
            phase.set_done(archive_progress(progress_start, progress_end, done, total));
        })?;
        self.progress_units = progress_end;
        for source in sources {
            let metadata = fs::metadata(&source)?;
            self.appended_sources
                .push((source, metadata.len(), metadata.modified()?));
        }
        Ok(())
    }

    /// Appends all remaining files, completes the archive and writes its marker.
    /// Call this only after conversion and staged-tree verification have finished.
    pub fn seal(
        mut self,
        root: &Path,
        mut marker: Value,
        phase: &Phase,
    ) -> CoreResult<zip_archive::Written> {
        for (source, size, modified) in &self.appended_sources {
            let metadata = fs::metadata(source)?;
            if metadata.len() != *size || metadata.modified()? != *modified {
                return Err(fail(format!(
                    "source changed after progressive archive append: {}",
                    source.display()
                )));
            }
        }
        let remaining: Vec<Input> = tree_inputs(root)?
            .into_iter()
            .filter(|input| !self.writer.contains_exact_name(&input.name))
            .collect();
        self.append_inputs(remaining, phase, ARCHIVE_PROGRESS_UNITS)?;
        let written = self.writer.finish()?;
        phase.add_bytes(written.size);
        marker["layout"] = LAYOUT.into();
        marker["archive_size"] = written.size.into();
        marker["archive_entries"] = written.entries.into();
        marker["output_sha256"] = written.content_sha256.clone().into();
        let mut bytes = serde_json::to_vec_pretty(&marker)?;
        bytes.push(b'\n');
        fs::write(root.join(MARKER), bytes)?;
        Ok(written)
    }
}

/// Removes the verified asset tree after the completed archive no longer needs its sources.
pub fn remove_staged_data(stage: &Path, published: &Path, phase: &Phase) -> CoreResult<()> {
    phase.item("data");
    let stage_name = stage
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or("");
    if !stage_name.starts_with(".dc2data-stage-") {
        return Err(fail(
            "staged data cleanup requires a dc2data stage directory",
        ));
    }
    let stage = fs::canonicalize(stage)?;
    let published = fs::canonicalize(published)?;
    if published != stage.join("published") {
        return Err(fail("staged data cleanup target is outside its stage"));
    }
    let data = published.join("data");
    let metadata = match fs::symlink_metadata(&data) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            phase.set_total(1);
            phase.inc(1);
            return Ok(());
        }
        Err(error) => return Err(error.into()),
    };
    if metadata.file_type().is_symlink() || !metadata.is_dir() || fs::canonicalize(&data)? != data {
        return Err(fail("staged data cleanup target is not a direct directory"));
    }
    let mut children = Vec::new();
    let mut parents = Vec::new();
    for entry in fs::read_dir(&data)? {
        let entry = entry?;
        let path = entry.path();
        let metadata = fs::symlink_metadata(&path)?;
        if metadata.file_type().is_symlink()
            || fs::canonicalize(&path)?.parent() != Some(data.as_path())
        {
            return Err(fail(format!(
                "staged data cleanup child escapes data: {}",
                path.display()
            )));
        }
        if metadata.is_dir() {
            for child in fs::read_dir(&path)? {
                let child = child?;
                let child_path = child.path();
                let child_metadata = fs::symlink_metadata(&child_path)?;
                if child_metadata.file_type().is_symlink()
                    || fs::canonicalize(&child_path)?.parent() != Some(path.as_path())
                {
                    return Err(fail(format!(
                        "staged data cleanup child escapes data: {}",
                        child_path.display()
                    )));
                }
                children.push((child_path, child_metadata.is_dir()));
            }
            parents.push(path);
        } else {
            children.push((path, false));
        }
    }
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(4)
        .build()
        .map_err(|error| fail(format!("cannot start staged data cleanup: {error}")))?;
    phase.set_total((children.len() + parents.len() + 1) as u64);
    let removed: Vec<CoreResult<()>> = pool.install(|| {
        children
            .par_iter()
            .map(|(path, directory)| {
                if *directory {
                    fs::remove_dir_all(path)?;
                } else {
                    fs::remove_file(path)?;
                }
                phase.inc(1);
                Ok(())
            })
            .collect()
    });
    removed.into_iter().collect::<CoreResult<()>>()?;
    for parent in parents {
        fs::remove_dir(parent)?;
        phase.inc(1);
    }
    fs::remove_dir(data)?;
    phase.inc(1);
    Ok(())
}

/// Packs a verified staged tree in one pass when no progressive writer exists.
pub fn seal(root: &Path, marker: Value, phase: &Phase) -> CoreResult<zip_archive::Written> {
    ProgressiveArchive::begin(root)?.seal(root, marker, phase)
}

/// Applies the published-asset rules that need only one entry's name and bytes.
fn audit_entry(name: &str, bytes: &[u8]) -> CoreResult<()> {
    let lower = name.to_ascii_lowercase();
    if super::configs::gameplay_digest(&lower).is_some() {
        return super::configs::verify_gameplay_document(&lower, bytes);
    }
    if external_config_document(name) {
        return Err(fail(format!(
            "external config document in data archive: {name}"
        )));
    }
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
    let texture_document = lower.ends_with(".json")
        && serde_json::from_slice::<Value>(bytes).is_ok_and(|value| {
            value["schema_version"] == 3
                && value.get("groups").is_some()
                && value.get("entries").is_some()
        });
    if (lower.ends_with(".png") || texture_document)
        && (!name.starts_with("textures/")
            || name
                .rsplit_once('/')
                .unwrap()
                .0
                .split('/')
                .any(|part| part.contains('.')))
    {
        return Err(fail(format!(
            "texture outside flattened texture hierarchy: {name}"
        )));
    }
    super::packs::audit_entry(name, bytes)?;
    super::models::audit_payload(name, bytes)?;
    if bytes.len() as u64 <= 128 << 20 {
        super::configs::audit_payload(bytes, name, 0)?;
    }
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
    if marker["gameplay_source_fingerprint"].as_str()
        != Some(super::configs::source_fingerprint().as_str())
    {
        return Err(fail(
            "data.json gameplay source fingerprint does not match the extractor",
        ));
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
    verify_with_progress(root, &Phase::noop())
}

/// Fully verifies a published archive against its marker, reporting verified entries.
pub fn verify_with_progress(root: &Path, phase: &Phase) -> CoreResult<u64> {
    let marker = read_marker(root)?;
    if !archive_present(root, &marker) {
        return Err(fail(
            "data.zip is missing or its size does not match data.json",
        ));
    }
    let archive = Archive::open(&root.join(ARCHIVE))?;
    phase.set_total(archive.len() as u64);
    verify_open_archive(&archive, &marker, phase)
}

fn verify_pack_textures(
    document: &Value,
    read: &dyn Fn(&str) -> CoreResult<Vec<u8>>,
) -> CoreResult<()> {
    if let Some(members) = document["texture_members"].as_array() {
        for member in members {
            let path = member["path"]
                .as_str()
                .ok_or_else(|| fail("invalid PACK texture path"))?;
            let texture: Value = serde_json::from_slice(&read(path)?)?;
            if texture["schema_version"] != 3
                || !texture["groups"].is_array()
                || !texture["entries"].is_array()
            {
                return Err(fail(format!(
                    "PACK member is not a texture document: {path}"
                )));
            }
        }
    }
    Ok(())
}

fn verify_open_archive(archive: &Archive, marker: &Value, phase: &Phase) -> CoreResult<u64> {
    let view = ArchiveView::new(archive);
    let names: BTreeSet<String> = view.names().into_iter().collect();
    for &(name, _) in super::configs::gameplay_manifest() {
        if !names.contains(name) {
            return Err(fail(format!("gameplay document missing: {name}")));
        }
    }
    let mut records = archive.verify_entries_with_progress(
        &|name, bytes| {
            audit_entry(name, bytes)?;
            if name.ends_with(".json") && bytes.len() <= 128 << 20 {
                let document: Value = serde_json::from_slice(bytes)?;
                if document["schema_version"] == 3
                    && document.get("groups").is_some()
                    && document.get("entries").is_some()
                {
                    verify_texture_archive_value(&view, name, &document, &names)?;
                }
                if name.ends_with("/pack.json") {
                    verify_pack_textures(&document, &|path| view.read(path))?;
                }
            }
            Ok(())
        },
        &|n| phase.inc(n),
    )?;
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

/// Verifies a completed stream and its source-derived scripts without a staged asset tree.
pub fn verify_stream_with_progress(
    root: &Path,
    resources: &Inventory,
    converted_aliases: &HashSet<String>,
    profiles: &BTreeMap<String, BTreeSet<String>>,
    texture_paths: &BTreeMap<String, String>,
    phase: &Phase,
    publish_scripts: bool,
) -> CoreResult<u64> {
    let marker = read_marker(root)?;
    if !archive_present(root, &marker) {
        return Err(fail(
            "data.zip is missing or its size does not match data.json",
        ));
    }
    let archive = Archive::open(&root.join(ARCHIVE))?;
    let view = ArchiveView::new(&archive);
    let archive_names = view.names();
    let names: BTreeSet<String> = archive_names.iter().cloned().collect();
    phase.set_total(
        archive.len() as u64
            + super::models::verify_archive_work(&archive_names)
            + super::audio::verify_work(&names)
            + super::native_scripts::verify_archive_work(resources, publish_scripts)
            + 1,
    );
    let pngs = verify_open_archive(&archive, &marker, phase)?;
    let ((models, audio), scripts) = rayon::join(
        || {
            rayon::join(
                || super::models::verify_archive_with_script_policy(&view, phase, publish_scripts),
                || super::audio::verify_archive(&view, phase),
            )
        },
        || {
            super::native_scripts::verify_archive(
                resources,
                converted_aliases,
                profiles,
                texture_paths,
                &view,
                phase,
                publish_scripts,
            )
        },
    );
    models?;
    audio?;
    scripts?;
    super::configs::verify_inventory_paths(resources)?;
    phase.inc(1);
    Ok(pngs)
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
    #[test]
    fn old_stage_metadata_defaults_to_retain_but_not_luau_only() {
        let old = serde_json::json!({"schema_version": 1, "jobs": 2});
        let retain = serde_json::json!({
            "schema_version": 1,
            "jobs": 2,
            "script_publication_policy": "Retain"
        });
        let luau_only = serde_json::json!({
            "schema_version": 1,
            "jobs": 2,
            "script_publication_policy": "LuauOnly"
        });
        assert!(super::stage_metadata_matches(&old, &retain));
        assert!(!super::stage_metadata_matches(&old, &luau_only));
    }
    use super::*;

    #[test]
    fn archive_batch_progress_has_a_fixed_monotone_total() {
        let mut reported = Vec::new();
        for (start, end, total) in [(0, 3_800, 101), (3_800, 9_600, 53), (9_600, 10_000, 7)] {
            for done in 0..=total {
                reported.push(archive_progress(start, end, done, total));
            }
        }
        assert_eq!(reported.first(), Some(&0));
        assert_eq!(reported.last(), Some(&ARCHIVE_PROGRESS_UNITS));
        assert!(reported.windows(2).all(|window| window[0] <= window[1]));
    }

    #[test]
    fn texture_verifier_rejects_old_fields_and_missing_png() {
        let temp = tempfile::tempdir().unwrap();
        let port = temp.path();
        fs::create_dir_all(port.join("textures")).unwrap();
        let catalog =
            serde_json::json!({"schema_version":3,"sources":{"foo.img":"textures/foo.json"}});
        let assets = BTreeSet::from([
            "textures/foo.json".to_owned(),
            "textures/foo_0.png".to_owned(),
        ]);
        fs::write(port.join("textures/foo_0.png"), b"png").unwrap();
        let mut doc = serde_json::json!({"schema_version":3,"groups":[0],"entries":[
            {"name":"foo","pictures":[{"levels":["foo_0.png"]}]}
        ]});
        fs::write(
            port.join("textures/foo.json"),
            serde_json::to_vec(&doc).unwrap(),
        )
        .unwrap();
        verify_catalog(port, &catalog, &assets).unwrap();
        doc["original"] = serde_json::json!({"bytes":"00"});
        fs::write(
            port.join("textures/foo.json"),
            serde_json::to_vec(&doc).unwrap(),
        )
        .unwrap();
        assert!(verify_catalog(port, &catalog, &assets).is_err());
        doc.as_object_mut().unwrap().remove("original");
        doc["entries"][0]["pictures"][0]["levels"][0] = "missing.png".into();
        fs::write(
            port.join("textures/foo.json"),
            serde_json::to_vec(&doc).unwrap(),
        )
        .unwrap();
        assert!(verify_catalog(port, &catalog, &assets).is_err());
    }

    #[test]
    fn texture_verifier_accepts_script_only_empty_groups() {
        let temp = tempfile::tempdir().unwrap();
        let port = temp.path();
        fs::create_dir_all(port.join("textures")).unwrap();
        let catalog =
            serde_json::json!({"schema_version":3,"sources":{"cfg.img":"textures/cfg.json"}});
        let assets = BTreeSet::from(["textures/cfg.json".to_owned()]);
        let doc = serde_json::json!({"schema_version":3,"groups":[],"entries":[
            {"name":"#texanime","script":[{"command":"TEX_ANIME_DATA_END","args":[]}]}
        ]});
        fs::write(
            port.join("textures/cfg.json"),
            serde_json::to_vec(&doc).unwrap(),
        )
        .unwrap();
        verify_catalog(port, &catalog, &assets).unwrap();
    }

    fn staged_tree(root: &Path) {
        let data = root.join("data");
        fs::create_dir_all(data.join("audio")).unwrap();
        fs::write(data.join("script.stb"), b"unrelated script").unwrap();
        fs::write(data.join("audio/sample.wav"), b"RIFF converted PCM").unwrap();
        fs::create_dir_all(data.join("textures/audio")).unwrap();
        fs::write(data.join("textures/audio/sample.png"), b"png").unwrap();
        fs::write(data.join("manifest.json"), b"stale manifest").unwrap();
        for (name, bytes) in super::super::configs::gameplay_documents(None).unwrap() {
            let path = data.join(name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, bytes).unwrap();
        }
    }

    fn marker() -> Value {
        serde_json::json!({"schema_version":1,"data_version":super::super::data_version(),
            "gameplay_source_fingerprint":super::super::configs::source_fingerprint()})
    }

    #[test]
    fn sealed_archive_contains_unified_tree_and_detects_changes() {
        let temp = tempfile::tempdir().unwrap();
        let stage = temp.path().join(".dc2data-stage-test");
        let root = stage.join("published");
        staged_tree(&root);
        finalize_assets(&root).unwrap();
        assert!(!root.join("data/manifest.json").exists());
        let written = seal(&root, marker(), &Phase::noop()).unwrap();
        assert!(root.join("data").exists());
        assert_eq!(
            written.entries,
            3 + super::super::configs::gameplay_manifest().len() as u64
        );
        assert_eq!(verify(&root).unwrap(), 1);
        assert!(remove_staged_data(temp.path(), &root, &Phase::noop()).is_err());
        assert!(root.join("data").exists());
        remove_staged_data(&stage, &root, &Phase::noop()).unwrap();
        assert!(!root.join("data").exists());
        let archive = Archive::open(&root.join(ARCHIVE)).unwrap();
        let names: Vec<&str> = (0..archive.len())
            .map(|i| archive.name(i))
            .filter(|name| super::super::configs::gameplay_digest(name).is_none())
            .collect();
        assert_eq!(
            names,
            [
                "audio/sample.wav",
                "script.stb",
                "textures/audio/sample.png"
            ]
        );
        drop(archive);
        let mut changed = read_marker(&root).unwrap();
        changed
            .as_object_mut()
            .unwrap()
            .remove("gameplay_source_fingerprint");
        fs::write(root.join(MARKER), serde_json::to_vec(&changed).unwrap()).unwrap();
        assert!(
            verify(&root)
                .unwrap_err()
                .to_string()
                .contains("gameplay source fingerprint")
        );
        changed["gameplay_source_fingerprint"] = super::super::configs::source_fingerprint().into();
        changed["output_sha256"] = "0".repeat(64).into();
        fs::write(root.join(MARKER), serde_json::to_vec(&changed).unwrap()).unwrap();
        assert!(verify(&root).unwrap_err().to_string().contains("changed"));
        changed["data_version"] = 8.into();
        fs::write(root.join(MARKER), serde_json::to_vec(&changed).unwrap()).unwrap();
        assert!(verify(&root).is_err());
    }

    #[test]
    fn gameplay_documents_are_required_and_match_content_digests() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("published");
        staged_tree(&root);
        finalize_assets(&root).unwrap();
        let Some((name, bytes)) = super::super::configs::gameplay_documents(None)
            .unwrap()
            .into_iter()
            .next()
        else {
            return;
        };
        let document = root.join("data").join(name);
        let mut changed = bytes.to_vec();
        changed.push(b' ');
        fs::write(&document, changed).unwrap();
        seal(&root, marker(), &Phase::noop()).unwrap();
        assert!(
            verify(&root)
                .unwrap_err()
                .to_string()
                .contains("gameplay document changed")
        );
        fs::remove_file(&document).unwrap();
        seal(&root, marker(), &Phase::noop()).unwrap();
        assert!(
            verify(&root)
                .unwrap_err()
                .to_string()
                .contains("gameplay document missing")
        );
    }

    #[test]
    fn publish_replaces_legacy_directories_with_archive() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("run");
        let staged = temp.path().join("stage");
        staged_tree(&staged);
        finalize_assets(&staged).unwrap();
        seal(&staged, marker(), &Phase::noop()).unwrap();
        fs::create_dir_all(root.join("data")).unwrap();
        fs::create_dir_all(root.join("data/old")).unwrap();
        fs::write(root.join("data/old/file.bin"), b"old").unwrap();
        fs::write(root.join(MARKER), b"{\"data_version\":8}").unwrap();
        publish(&staged, &root).unwrap();
        assert!(!root.join("data").exists());
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
        let original = temp.path().join("data");
        let backup = temp.path().join(".dc2data-data-backup-test");
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
        let staged = temp.path().join(".dc2data-stage-test/published/data");
        let destination = temp.path().join("data");
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
