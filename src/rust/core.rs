use sha2::{Digest, Sha256};
use std::fmt;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

#[path = "archive.rs"]
pub mod archive;
#[path = "audio.rs"]
pub mod audio;
#[path = "configs.rs"]
pub mod configs;
#[path = "models.rs"]
pub mod models;
#[path = "native_scripts.rs"]
pub mod native_scripts;
#[path = "packs.rs"]
pub mod packs;
#[path = "publish.rs"]
pub mod publish;
#[path = "textures.rs"]
pub mod textures;
#[path = "zip_archive.rs"]
pub mod zip_archive;

#[derive(Debug)]
pub struct CoreError(pub String);

impl fmt::Display for CoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

impl std::error::Error for CoreError {}

impl From<std::io::Error> for CoreError {
    fn from(error: std::io::Error) -> Self {
        Self(error.to_string())
    }
}

impl From<serde_json::Error> for CoreError {
    fn from(error: serde_json::Error) -> Self {
        Self(error.to_string())
    }
}

pub type CoreResult<T> = Result<T, CoreError>;

pub fn data_version() -> u64 {
    let value: serde_json::Value = serde_json::from_str(include_str!("../../data_version.json"))
        .expect("invalid data_version.json");
    assert_eq!(value["schema_version"], 1, "invalid shared schema version");
    value["data_version"]
        .as_u64()
        .expect("missing shared data version")
}

#[derive(Debug, Default)]
pub struct ExtractReport {
    pub files: u64,
    pub bytes: u64,
    pub texture_sources: u64,
    pub pngs: u64,
}

#[derive(Debug, Default)]
pub struct CheckReport {
    pub textures: u64,
}

fn source_fingerprint(source: &Path) -> CoreResult<String> {
    fn hash_file(path: &Path, hash: &mut Sha256) -> CoreResult<()> {
        let mut file = std::fs::File::open(path)?;
        let mut buffer = vec![0u8; 1024 * 1024];
        loop {
            let count = file.read(&mut buffer)?;
            if count == 0 {
                break;
            }
            hash.update(&buffer[..count]);
        }
        Ok(())
    }
    fn gather(dir: &Path, out: &mut Vec<PathBuf>) -> CoreResult<()> {
        for entry in std::fs::read_dir(dir)? {
            let path = entry?.path();
            let metadata = std::fs::symlink_metadata(&path)?;
            if metadata.file_type().is_symlink() {
                return Err(CoreError(format!(
                    "source symlink unsupported: {}",
                    path.display()
                )));
            }
            out.push(path.clone());
            if metadata.is_dir() {
                gather(&path, out)?;
            }
        }
        Ok(())
    }
    let mut hash = Sha256::new();
    if source.is_file() {
        hash.update(b"kazarov-source-file-v1\0");
        hash.update(std::fs::metadata(source)?.len().to_le_bytes());
        hash_file(source, &mut hash)?;
    } else if source.is_dir() {
        hash.update(b"kazarov-source-tree-v1\0");
        let mut paths = Vec::new();
        gather(source, &mut paths)?;
        paths.sort_by_key(|p| {
            p.strip_prefix(source)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/")
        });
        for path in paths {
            let relative = path
                .strip_prefix(source)
                .map_err(|e| CoreError(e.to_string()))?
                .to_string_lossy()
                .replace('\\', "/");
            let bytes = relative.as_bytes();
            hash.update((bytes.len() as u64).to_le_bytes());
            hash.update(bytes);
            if path.is_dir() {
                hash.update(b"D");
            } else {
                hash.update(b"F");
                hash.update(std::fs::metadata(&path)?.len().to_le_bytes());
                hash_file(&path, &mut hash)?;
            }
        }
    } else {
        return Err(CoreError(format!(
            "source does not exist: {}",
            source.display()
        )));
    }
    Ok(hash
        .finalize()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect())
}

fn source_state(source: &Path) -> CoreResult<serde_json::Value> {
    use std::time::UNIX_EPOCH;
    fn modified(path: &Path) -> CoreResult<u128> {
        Ok(std::fs::metadata(path)?
            .modified()?
            .duration_since(UNIX_EPOCH)
            .map_err(|e| CoreError(e.to_string()))?
            .as_nanos())
    }
    let canonical = source.canonicalize()?;
    let path = canonical.to_string_lossy().into_owned();
    let entries = if canonical.is_file() {
        vec![
            serde_json::json!({"path":"","type":"file","size":std::fs::metadata(&canonical)?.len(),"mtime_ns":modified(&canonical)?.to_string()}),
        ]
    } else if canonical.is_dir() {
        fn gather(root: &Path, dir: &Path, out: &mut Vec<serde_json::Value>) -> CoreResult<()> {
            for entry in std::fs::read_dir(dir)? {
                let path = entry?.path();
                let metadata = std::fs::symlink_metadata(&path)?;
                if metadata.file_type().is_symlink() {
                    return Err(CoreError(format!(
                        "source symlink unsupported: {}",
                        path.display()
                    )));
                }
                let relative = path
                    .strip_prefix(root)
                    .map_err(|e| CoreError(e.to_string()))?
                    .to_string_lossy()
                    .replace('\\', "/");
                out.push(serde_json::json!({"path":relative,"type":if metadata.is_dir(){"directory"}else{"file"},
                    "size":if metadata.is_file(){metadata.len()}else{0},"mtime_ns":modified(&path)?.to_string()}));
                if metadata.is_dir() {
                    gather(root, &path, out)?;
                }
            }
            Ok(())
        }
        let mut entries = Vec::new();
        gather(&canonical, &canonical, &mut entries)?;
        entries.sort_by(|a, b| a["path"].as_str().cmp(&b["path"].as_str()));
        entries
    } else {
        return Err(CoreError(format!(
            "source does not exist: {}",
            source.display()
        )));
    };
    let serialized = serde_json::to_vec(&entries)?;
    let fingerprint: String = Sha256::digest(&serialized)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    Ok(serde_json::json!({"path":path,"fingerprint":fingerprint}))
}

/// Coalesces parallel progress callbacks for stderr; the bar never moves backwards.
#[derive(Default)]
struct ExtractCliProgress {
    coalesced_fraction: f32,
    coalesced_message: &'static str,
    printed_fraction: f32,
    printed_message: &'static str,
}

impl ExtractCliProgress {
    fn advance(&mut self, fraction: f32, message: &'static str) {
        if fraction >= self.coalesced_fraction {
            self.coalesced_fraction = fraction.clamp(0.0, 1.0);
            self.coalesced_message = message;
        }
    }

    fn maybe_eprint(&mut self) {
        let message_changed = self.coalesced_message != self.printed_message;
        let step = self.coalesced_fraction - self.printed_fraction >= 0.01
            || self.coalesced_fraction >= 0.99;
        if !message_changed && !step {
            return;
        }
        eprintln!(
            "dc2data: [{:>3.0}%] {}",
            self.coalesced_fraction * 100.0,
            self.coalesced_message
        );
        self.printed_fraction = self.coalesced_fraction;
        self.printed_message = self.coalesced_message;
    }
}

fn extract_cli_progress() -> impl Fn(f32, &'static str) + Sync {
    let state = Mutex::new(ExtractCliProgress::default());
    move |fraction, message| {
        let mut progress = state.lock().expect("extract progress lock");
        progress.advance(fraction, message);
        progress.maybe_eprint();
    }
}

pub fn extract(source: &Path, data_out: &Path, jobs: usize) -> CoreResult<ExtractReport> {
    extract_with_progress(source, data_out, jobs, &extract_cli_progress())
}

/// Recognizes a complete, verified archive staged for the unchanged source.
fn completed_stage_pngs(
    published: &Path,
    state: &serde_json::Value,
    source_sha256: &str,
) -> Option<u64> {
    let marker = publish::read_marker(published).ok()?;
    if marker["source_state"] != *state || marker["source_sha256"].as_str() != Some(source_sha256) {
        return None;
    }
    publish::verify(published).ok()
}

/// Extracts and publishes data, reporting completed work in weighted phases.
/// Callbacks can run concurrently; 100% is emitted only after successful publication.
pub fn extract_with_progress(
    source: &Path,
    data_out: &Path,
    jobs: usize,
    progress: &(dyn Fn(f32, &'static str) + Sync),
) -> CoreResult<ExtractReport> {
    if data_out.file_name().is_none_or(|name| name != "data") {
        return Err(CoreError("output must be <game-root>/data".into()));
    }
    let root = data_out
        .parent()
        .ok_or_else(|| CoreError("missing game root".into()))?;
    std::fs::create_dir_all(root)?;
    if !(1..=64).contains(&jobs) {
        return Err(CoreError("jobs must be between 1 and 64".into()));
    }
    let source = source.canonicalize()?;
    if source.is_dir() && root.canonicalize()?.starts_with(&source) {
        return Err(CoreError("The output must be outside the source disc folder. Choose the folder containing the original disc files.".into()));
    }
    progress(0.0, "Checking source files…");
    let initial_state = source_state(&source)?;
    let source_sha256 = source_fingerprint(&source)?;
    let metadata = serde_json::json!({
        "schema_version":1,"data_version":data_version(),"source":source.to_string_lossy(),
        "source_sha256":source_sha256,"jobs":jobs,
    });
    let stage = publish::acquire_stage(root, &metadata)?;
    let raw = stage.path().join("raw");
    let published = stage.path().join("published");
    progress(0.02, "Checking saved conversion…");
    if let Some(pngs) = completed_stage_pngs(&published, &initial_state, &source_sha256) {
        let (files, bytes) = archive::extraction_totals(&source)?;
        let marker = publish::read_marker(&published)?;
        let report = ExtractReport {
            files,
            bytes,
            texture_sources: marker["texture_sources"].as_u64().unwrap_or(0),
            pngs,
        };
        if source_state(&source)? != initial_state {
            return Err(CoreError("source changed during extraction".into()));
        }
        progress(0.99, "Publishing verified data…");
        publish::publish_verified(&published, root)?;
        let stage_path = stage.path().to_path_buf();
        drop(stage);
        std::fs::remove_dir_all(stage_path)?;
        progress(1.0, "Ready — starting game…");
        return Ok(report);
    }
    if published.is_dir() {
        std::fs::remove_dir_all(&published)?;
    }
    progress(0.05, "Extracting files…");
    let (files, bytes) =
        archive::extract_raw_with_progress(&source, &raw, jobs, &|done, total| {
            progress(
                0.05 + 0.25 * done as f32 / total.max(1) as f32,
                "Extracting files…",
            );
        })?;
    progress(0.30, "Converting textures…");
    let converted =
        textures::convert_tree_with_progress(&raw, &published, jobs, &|done, total| {
            progress(
                0.30 + 0.38 * done as f32 / total.max(1) as f32,
                "Converting textures…",
            );
        })?;
    progress(0.68, "Converting audio…");
    audio::convert_tree(&raw, &published, jobs)?;
    progress(0.70, "Converting models and animation…");
    let converted_models = models::convert_tree(&raw, &published, jobs, &converted.documents)?;
    textures::remove_embedded(
        &published,
        &converted.documents,
        &converted_models.consumed_texture_sources,
        &converted_models.referenced_pngs,
    )?;
    progress(0.71, "Converting configuration…");
    configs::convert_tree(&raw, &published, jobs)?;
    native_scripts::convert_tree(&raw, &published)?;
    packs::convert_tree(&raw, &published)?;
    native_scripts::verify_tree(&raw, &published)?;
    publish::finalize_assets(&published)?;
    progress(0.72, "Verifying converted data…");
    publish::verify_trees(&published)?;
    if source_state(&source)? != initial_state {
        return Err(CoreError("source changed during extraction".into()));
    }
    // The raw tree only feeds conversion; removing it before packing lowers peak disk use.
    std::fs::remove_dir_all(&raw)?;
    progress(0.75, "Packing data…");
    publish::seal(
        &published,
        serde_json::json!({
            "schema_version":1,"data_version":data_version(),"source_state":initial_state,
            "source_sha256":source_sha256,"texture_sources":converted.sources,
        }),
        &|done, total| {
            progress(
                0.75 + 0.19 * done as f32 / total.max(1) as f32,
                "Packing data…",
            );
        },
    )?;
    progress(0.94, "Verifying packed data…");
    publish::verify(&published)?;
    progress(0.99, "Publishing data…");
    publish::publish_verified(&published, root)?;
    let stage_path = stage.path().to_path_buf();
    drop(stage);
    std::fs::remove_dir_all(stage_path)?;
    progress(1.0, "Ready — starting game…");
    Ok(ExtractReport {
        files,
        bytes,
        texture_sources: converted.sources,
        pngs: converted.pngs,
    })
}

pub fn list(source: &Path) -> CoreResult<String> {
    archive::list_text(source)
}

pub fn sound(source: &Path) -> CoreResult<String> {
    archive::sound_text(source)
}

pub fn check(data_root: &Path) -> CoreResult<CheckReport> {
    if data_root.file_name().is_none_or(|name| name != "data") {
        return Err(CoreError("check path must be <game-root>/data".into()));
    }
    let root = data_root
        .parent()
        .ok_or_else(|| CoreError("missing game root".into()))?;
    Ok(CheckReport {
        textures: publish::verify(root)?,
    })
}

pub fn current(source: &Path, data_root: &Path) -> CoreResult<bool> {
    if data_root.file_name().is_none_or(|name| name != "data") {
        return Err(CoreError("current path must be <game-root>/data".into()));
    }
    let Some(root) = data_root.parent() else {
        return Ok(false);
    };
    let Ok(marker) = publish::read_marker(root) else {
        return Ok(false);
    };
    if !publish::archive_present(root, &marker) {
        return Ok(false);
    }
    let state = match source_state(source) {
        Ok(state) => state,
        Err(_) => return Ok(false),
    };
    Ok(marker["source_state"] == state && marker["source_sha256"].as_str().is_some())
}

#[cfg(test)]
mod tests {
    use super::*;
    use zip_archive::Archive;

    #[test]
    fn retry_rebuilds_incomplete_conversion_without_publishing_stale_assets() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("disc");
        std::fs::create_dir_all(source.join("MOVIE")).unwrap();
        let mut hd4 = vec![0u8; 33];
        hd4[0] = 24;
        hd4[4] = 4;
        hd4[12] = 32;
        hd4[24..33].copy_from_slice(b"FOO.BIN\0\0");
        std::fs::write(source.join("DATA.HD4"), hd4).unwrap();
        std::fs::write(source.join("DATA.DAT"), b"DATA").unwrap();
        let mut hd3 = vec![0u8; 42];
        hd3[0] = 32;
        hd3[4] = 3;
        hd3[12] = 1;
        hd3[32..42].copy_from_slice(b"BGM_A.STR\0");
        std::fs::write(source.join("SOUND.HD3"), hd3).unwrap();
        std::fs::write(source.join("SOUND.DAT"), b"SND").unwrap();
        let source = std::fs::canonicalize(source).unwrap();
        let root = temp.path().join("run");
        std::fs::create_dir_all(root.join("data")).unwrap();
        std::fs::create_dir_all(root.join("port_data")).unwrap();
        let metadata = serde_json::json!({
            "schema_version":1,"data_version":data_version(),"source":source.to_string_lossy(),
            "source_sha256":source_fingerprint(&source).unwrap(),"jobs":2,
        });
        let stage = publish::acquire_stage(&root, &metadata).unwrap();
        std::fs::create_dir_all(stage.path().join("published/port_data")).unwrap();
        std::fs::write(stage.path().join("published/port_data/stale.img"), b"old").unwrap();
        std::fs::create_dir_all(stage.path().join("raw")).unwrap();
        std::fs::write(stage.path().join("raw/foo.bin"), b"DATA").unwrap();
        drop(stage);
        extract(&source, &root.join("data"), 2).unwrap();
        let archive = Archive::open(&root.join(publish::ARCHIVE)).unwrap();
        assert!(!(0..archive.len()).any(|i| archive.name(i).contains("stale")));
        let index = (0..archive.len())
            .find(|&i| archive.name(i) == "foo.bin")
            .unwrap();
        assert_eq!(archive.read(index).unwrap(), b"DATA");
        drop(archive);
        check(&root.join("data")).unwrap();
        assert!(current(&source, &root.join("data")).unwrap());
        // Only the archive and its marker remain; legacy trees and the stage are gone.
        let mut left: Vec<_> = std::fs::read_dir(&root)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        left.sort();
        assert_eq!(left, ["data.json", "data.zip"]);
    }

    fn sealed_stage(published: &Path, state: &serde_json::Value) {
        std::fs::create_dir_all(published.join("data")).unwrap();
        std::fs::write(published.join("data/example.bin"), b"payload").unwrap();
        publish::finalize_assets(published).unwrap();
        publish::seal(
            published,
            serde_json::json!({
                "schema_version":1,"data_version":data_version(),"source_state":state,
                "source_sha256":"source-content",
            }),
            &|_, _| {},
        )
        .unwrap();
    }

    #[test]
    fn completed_stage_requires_source_identity_and_intact_archive() {
        let temp = tempfile::tempdir().unwrap();
        let published = temp.path().join("published");
        let state = serde_json::json!({"fingerprint":"source-state"});
        sealed_stage(&published, &state);
        assert_eq!(
            completed_stage_pngs(&published, &state, "source-content"),
            Some(0)
        );
        assert_eq!(
            completed_stage_pngs(&published, &state, "different-source"),
            None
        );
        assert_eq!(
            completed_stage_pngs(&published, &serde_json::json!({}), "source-content"),
            None
        );
        let zip = published.join(publish::ARCHIVE);
        let mut bytes = std::fs::read(&zip).unwrap();
        let at = bytes
            .windows(7)
            .position(|window| window == b"example")
            .unwrap();
        bytes[at] = b'E';
        std::fs::write(&zip, bytes).unwrap();
        assert_eq!(
            completed_stage_pngs(&published, &state, "source-content"),
            None
        );
    }

    #[test]
    fn current_detects_nested_directory_change_and_stage_reuses_only_matching_source() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        std::fs::create_dir_all(source.join("nested")).unwrap();
        std::fs::write(source.join("nested/item.bin"), b"one").unwrap();
        let root = temp.path().join("run");
        let state = source_state(&source).unwrap();
        sealed_stage(&root, &state);
        assert!(current(&source, &root.join("data")).unwrap());
        let metadata = serde_json::json!({"schema_version":1,"source_sha256":source_fingerprint(&source).unwrap()});
        let guard = publish::acquire_stage(&root, &metadata).unwrap();
        let reused = guard.path().to_path_buf();
        assert_ne!(
            publish::acquire_stage(&root, &metadata).unwrap().path(),
            reused
        );
        drop(guard);
        assert_eq!(
            publish::acquire_stage(&root, &metadata).unwrap().path(),
            reused
        );
        std::fs::write(source.join("nested/item.bin"), b"changed contents").unwrap();
        assert!(!current(&source, &root.join("data")).unwrap());
    }

    #[test]
    fn current_rejects_directory_layout_and_truncated_archive() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        std::fs::create_dir_all(&source).unwrap();
        std::fs::write(source.join("item.bin"), b"one").unwrap();
        let root = temp.path().join("run");
        let state = source_state(&source).unwrap();
        sealed_stage(&root, &state);
        assert!(current(&source, &root.join("data")).unwrap());
        let zip = root.join(publish::ARCHIVE);
        let bytes = std::fs::read(&zip).unwrap();
        std::fs::write(&zip, &bytes[..bytes.len() - 1]).unwrap();
        assert!(!current(&source, &root.join("data")).unwrap());
        std::fs::remove_file(&zip).unwrap();
        std::fs::create_dir_all(root.join("data")).unwrap();
        std::fs::create_dir_all(root.join("port_data")).unwrap();
        std::fs::write(
            root.join("data.json"),
            serde_json::to_vec(&serde_json::json!({
                "schema_version":1,"data_version":8,"source_state":state,"source_sha256":"x",
            }))
            .unwrap(),
        )
        .unwrap();
        assert!(!current(&source, &root.join("data")).unwrap());
    }
}
