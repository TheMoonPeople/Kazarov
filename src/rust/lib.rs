pub mod core;
#[cfg(not(test))]
mod host_jobs;
#[cfg(feature = "desktop-ui")]
mod setup;

use core::zip_archive::Archive;

#[cxx::bridge(namespace = "kazarov")]
mod ffi {
    struct ExtractReport {
        files: u64,
        bytes: u64,
        texture_sources: u64,
        pngs: u64,
    }

    struct PackReport {
        files: u64,
        bytes: u64,
    }

    struct CheckReport {
        textures: u64,
    }

    /// Where extract_tracked is: a fraction from 0 to 1 and the stage it is in.
    struct ExtractStatus {
        fraction: f32,
        stage: String,
    }

    enum ScriptPublicationPolicy {
        Retain,
        LuauOnly,
    }

    struct TextureConvertReport {
        sources: u64,
        pngs: u64,
    }

    extern "Rust" {
        fn bridge_smoke(value: &str) -> Result<String>;
        fn extract(source: &str, data_out: &str, jobs: usize) -> Result<ExtractReport>;
        /// extract, also publishing its progress for extract_status, for a host that draws its own
        /// progress while it runs on another thread.
        fn extract_tracked(source: &str, data_out: &str, jobs: usize) -> Result<ExtractReport>;
        fn extract_status() -> ExtractStatus;
        fn extract_with_script_policy(
            source: &str,
            data_out: &str,
            jobs: usize,
            policy: ScriptPublicationPolicy,
        ) -> Result<ExtractReport>;
        fn textures_convert(raw: &str, output: &str, jobs: usize) -> Result<TextureConvertReport>;
        fn textures_convert_dc1(
            raw: &str,
            output: &str,
            jobs: usize,
        ) -> Result<TextureConvertReport>;
        fn textures_document_map_dc1(raw: &str, output: &str, jobs: usize) -> Result<String>;
        fn textures_verify(textures: &str) -> Result<()>;
        fn list(source: &str) -> Result<String>;
        fn sound(source: &str) -> Result<String>;
        fn refresh_configs(source: &str, config_dir: &str) -> Result<usize>;
        fn audit_configs(source: &str, config_dir: &str) -> Result<String>;
        fn configs_parity(
            source: &str,
            archive: &str,
            config_dir: &str,
            fixture_dir: &str,
            texture_oracle: &str,
        ) -> Result<usize>;
        fn check(data_root: &str) -> Result<CheckReport>;
        fn current(source: &str, data_root: &str) -> Result<bool>;
        fn current_with_script_policy(
            source: &str,
            data_root: &str,
            policy: ScriptPublicationPolicy,
        ) -> Result<bool>;
        /// Runs first-run setup on the main thread; false means the user closed it.
        fn setup_data(title: &str, data_root: &str) -> Result<bool>;
        /// Runs first-run setup under an explicit compiled-script publication policy.
        fn setup_data_with_script_policy(
            title: &str,
            data_root: &str,
            policy: ScriptPublicationPolicy,
        ) -> Result<bool>;

        /// Immutable, thread-safe view of a ZIP asset archive.
        type Archive;
        /// Opens an archive and validates its central directory and entry names.
        fn open_archive(path: &str) -> Result<Box<Archive>>;
        fn archive_entry_count(archive: &Archive) -> usize;
        /// Relative '/'-separated name in its original case; the index must be in range.
        fn archive_entry_name(archive: &Archive, index: usize) -> &str;
        /// Uncompressed size in bytes.
        fn archive_entry_size(archive: &Archive, index: usize) -> u64;
        fn archive_entry_crc(archive: &Archive, index: usize) -> u32;
        /// True when the entry's bytes are stored uncompressed in the file.
        fn archive_entry_stored(archive: &Archive, index: usize) -> bool;
        /// Absolute file offset of the entry's data, 64-byte aligned.
        fn archive_entry_data_offset(archive: &Archive, index: usize) -> u64;
        /// Decompresses or copies one entry into `out` (exactly its size) and checks its CRC-32.
        fn archive_read_entry(archive: &Archive, index: usize, out: &mut [u8]) -> Result<()>;
        fn archive_path(archive: &Archive) -> String;
        /// Writes a deterministic archive of every file below `source`.
        fn pack_directory(source: &str, output: &str) -> Result<()>;
        /// Packs a directory as a classic ZIP of stored entries, checksumming on `workers` threads.
        fn pack_directory_stored(source: &str, output: &str, workers: usize) -> Result<PackReport>;
        /// Decodes a PNG of exactly `width` by `height` pixels into `out` as 8-bit RGBA (`width * height * 4`
        /// bytes). Safe to call from any thread.
        fn png_decode_rgba(png: &[u8], width: u32, height: u32, out: &mut [u8]) -> Result<()>;
    }
}

fn setup_data(title: &str, data_root: &str) -> Result<bool, core::CoreError> {
    #[cfg(not(feature = "desktop-ui"))]
    {
        let _ = (title, data_root);
        return Ok(false);
    }
    #[cfg(feature = "desktop-ui")]
    {
        setup::run(title, data_root.as_ref())
    }
}

fn setup_data_with_script_policy(
    title: &str,
    data_root: &str,
    policy: ffi::ScriptPublicationPolicy,
) -> Result<bool, core::CoreError> {
    let policy = core_script_publication_policy(policy)?;
    #[cfg(not(feature = "desktop-ui"))]
    {
        let _ = (title, data_root, policy);
        return Ok(false);
    }
    #[cfg(feature = "desktop-ui")]
    {
        setup::run_with_policy(title, data_root.as_ref(), policy)
    }
}

fn bridge_smoke(value: &str) -> Result<String, core::CoreError> {
    if value == "error" {
        return Err(core::CoreError("bridge smoke error".into()));
    }
    Ok(value.to_owned())
}

static EXTRACT_STATUS: std::sync::Mutex<(f32, &'static str)> = std::sync::Mutex::new((0.0, ""));

fn extract_tracked(
    source: &str,
    data_out: &str,
    jobs: usize,
) -> Result<ffi::ExtractReport, core::CoreError> {
    *EXTRACT_STATUS.lock().unwrap() = (0.0, "");
    let report = core::extract_with_progress(
        source.as_ref(),
        data_out.as_ref(),
        jobs,
        &|fraction, stage| {
            *EXTRACT_STATUS.lock().unwrap() = (fraction, stage);
        },
    )?;
    Ok(ffi::ExtractReport {
        files: report.files,
        bytes: report.bytes,
        texture_sources: report.texture_sources,
        pngs: report.pngs,
    })
}

fn extract_status() -> ffi::ExtractStatus {
    let (fraction, stage) = *EXTRACT_STATUS.lock().unwrap();
    ffi::ExtractStatus {
        fraction,
        stage: stage.to_owned(),
    }
}

fn extract(
    source: &str,
    data_out: &str,
    jobs: usize,
) -> Result<ffi::ExtractReport, core::CoreError> {
    let report = core::extract(source.as_ref(), data_out.as_ref(), jobs)?;
    Ok(ffi::ExtractReport {
        files: report.files,
        bytes: report.bytes,
        texture_sources: report.texture_sources,
        pngs: report.pngs,
    })
}

fn extract_with_script_policy(
    source: &str,
    data_out: &str,
    jobs: usize,
    policy: ffi::ScriptPublicationPolicy,
) -> Result<ffi::ExtractReport, core::CoreError> {
    let policy = core_script_publication_policy(policy)?;
    let report = core::extract_with_policy(source.as_ref(), data_out.as_ref(), jobs, policy)?;
    Ok(ffi::ExtractReport {
        files: report.files,
        bytes: report.bytes,
        texture_sources: report.texture_sources,
        pngs: report.pngs,
    })
}

fn textures_convert(
    raw: &str,
    output: &str,
    jobs: usize,
) -> Result<ffi::TextureConvertReport, core::CoreError> {
    let report = core::textures::convert_tree(raw.as_ref(), output.as_ref(), jobs)?;
    Ok(ffi::TextureConvertReport {
        sources: report.sources,
        pngs: report.pngs,
    })
}

#[cfg(test)]
fn script_publication_policy_from_repr(
    value: u8,
) -> Result<core::ScriptPublicationPolicy, core::CoreError> {
    match value {
        0 => Ok(core::ScriptPublicationPolicy::Retain),
        1 => Ok(core::ScriptPublicationPolicy::LuauOnly),
        _ => Err(core::CoreError(format!(
            "invalid script publication policy value: {value}"
        ))),
    }
}

fn core_script_publication_policy(
    policy: ffi::ScriptPublicationPolicy,
) -> Result<core::ScriptPublicationPolicy, core::CoreError> {
    match policy {
        ffi::ScriptPublicationPolicy::Retain => Ok(core::ScriptPublicationPolicy::Retain),
        ffi::ScriptPublicationPolicy::LuauOnly => Ok(core::ScriptPublicationPolicy::LuauOnly),
        _ => Err(core::CoreError("invalid script publication policy".into())),
    }
}

#[cfg(test)]
mod script_publication_policy_tests {
    use super::*;

    #[test]
    fn maps_both_publication_policies() {
        assert_eq!(
            core_script_publication_policy(ffi::ScriptPublicationPolicy::Retain).unwrap(),
            core::ScriptPublicationPolicy::Retain
        );
        assert_eq!(
            core_script_publication_policy(ffi::ScriptPublicationPolicy::LuauOnly).unwrap(),
            core::ScriptPublicationPolicy::LuauOnly
        );
    }

    #[test]
    fn rejects_unknown_policy_values() {
        assert!(script_publication_policy_from_repr(2).is_err());
        assert!(script_publication_policy_from_repr(u8::MAX).is_err());
    }
}

fn textures_convert_dc1(
    raw: &str,
    output: &str,
    jobs: usize,
) -> Result<ffi::TextureConvertReport, core::CoreError> {
    let report = core::textures::convert_tree_dc1(raw.as_ref(), output.as_ref(), jobs)?;
    Ok(ffi::TextureConvertReport {
        sources: report.sources,
        pngs: report.pngs,
    })
}

fn textures_document_map_dc1(
    raw: &str,
    output: &str,
    jobs: usize,
) -> Result<String, core::CoreError> {
    core::textures::document_map_tree_dc1(raw.as_ref(), output.as_ref(), jobs)
}

fn textures_verify(textures: &str) -> Result<(), core::CoreError> {
    core::textures::verify_tree(textures.as_ref())
}

fn list(source: &str) -> Result<String, core::CoreError> {
    core::list(source.as_ref())
}

fn sound(source: &str) -> Result<String, core::CoreError> {
    core::sound(source.as_ref())
}

fn generated(path: &str) -> bool {
    path.ends_with(".cfg.json")
        || path.ends_with(".map.json")
        || path.ends_with(".character.json")
        || path.ends_with(".sound/config.json")
        || path.ends_with(".sound/volume.json")
        || matches!(
            path,
            "snd2/rev.json" | "snd2/map_index.json" | "character_clips.json"
        )
}
fn preserve(
    directory: &std::path::Path,
    source: &std::path::Path,
    target: &std::path::Path,
) -> Result<(), core::CoreError> {
    for entry in std::fs::read_dir(directory)? {
        let entry = entry?;
        let path = entry.path();
        let kind = entry.file_type()?;
        if kind.is_symlink() {
            return Err(core::CoreError(format!(
                "gameplay symlink unsupported: {}",
                path.display()
            )));
        }
        if kind.is_dir() {
            preserve(&path, source, target)?;
        } else if kind.is_file() {
            let relative = path
                .strip_prefix(source)
                .map_err(|error| core::CoreError(error.to_string()))?;
            let name = relative
                .to_string_lossy()
                .replace('\\', "/")
                .to_ascii_lowercase();
            if !generated(&name) {
                let output = target.join(relative);
                std::fs::create_dir_all(output.parent().unwrap())?;
                std::fs::copy(path, output)?;
            }
        }
    }
    Ok(())
}
fn generated_files(
    root: &std::path::Path,
) -> Result<std::collections::BTreeMap<std::path::PathBuf, std::path::PathBuf>, core::CoreError> {
    fn visit(
        directory: &std::path::Path,
        root: &std::path::Path,
        files: &mut std::collections::BTreeMap<std::path::PathBuf, std::path::PathBuf>,
    ) -> Result<(), core::CoreError> {
        for entry in std::fs::read_dir(directory)? {
            let entry = entry?;
            let path = entry.path();
            let kind = entry.file_type()?;
            if kind.is_symlink() {
                return Err(core::CoreError(format!(
                    "gameplay symlink unsupported: {}",
                    path.display()
                )));
            }
            if kind.is_dir() {
                visit(&path, root, files)?;
            } else if kind.is_file() {
                let relative = path
                    .strip_prefix(root)
                    .map_err(|error| core::CoreError(error.to_string()))?
                    .to_path_buf();
                let name = relative
                    .to_string_lossy()
                    .replace('\\', "/")
                    .to_ascii_lowercase();
                if generated(&name) {
                    files.insert(relative, path);
                }
            }
        }
        Ok(())
    }
    let mut files = std::collections::BTreeMap::new();
    visit(root, root, &mut files)?;
    Ok(files)
}
fn install_filewise(
    staging: &std::path::Path,
    destination: &std::path::Path,
    backup: &std::path::Path,
) -> Result<(), core::CoreError> {
    let incoming = generated_files(staging)?;
    let old = generated_files(destination)?;
    std::fs::create_dir(backup)?;
    let mut prepared = Vec::new();
    let mut changed = Vec::new();
    let result = (|| -> Result<(), core::CoreError> {
        // Copy every original before the first replacement, including obsolete generated files.
        for (relative, source) in &old {
            let target = backup.join(relative);
            std::fs::create_dir_all(target.parent().unwrap())?;
            std::fs::copy(source, target).map_err(|error| {
                core::CoreError(format!(
                    "backup gameplay file {}: {error}",
                    source.display()
                ))
            })?;
        }
        // Prepare every replacement next to its destination, so the commit can only fail on a target lock.
        for (relative, source) in &incoming {
            let target = destination.join(relative);
            std::fs::create_dir_all(target.parent().unwrap())?;
            let temporary = target.with_file_name(format!(
                "{}.gameplay-incoming-{}",
                target.file_name().unwrap().to_string_lossy(),
                std::process::id()
            ));
            std::fs::copy(source, &temporary).map_err(|error| {
                core::CoreError(format!(
                    "prepare gameplay file {}: {error}",
                    target.display()
                ))
            })?;
            prepared.push((target, temporary));
        }
        for (target, temporary) in &prepared {
            changed.push(target.clone());
            if std::fs::rename(temporary, target).is_err() {
                // A watcher can deny replacement on Windows while still allowing writes.
                std::fs::copy(temporary, target).map_err(|error| {
                    core::CoreError(format!(
                        "replace gameplay file {}: {error}",
                        target.display()
                    ))
                })?;
            }
        }
        for relative in old
            .keys()
            .filter(|relative| !incoming.contains_key(*relative))
        {
            let target = destination.join(relative);
            changed.push(target.clone());
            std::fs::remove_file(&target).map_err(|error| {
                core::CoreError(format!(
                    "remove obsolete gameplay file {}: {error}",
                    target.display()
                ))
            })?;
        }
        Ok(())
    })();
    for (_, temporary) in prepared {
        let _ = std::fs::remove_file(temporary);
    }
    if let Err(error) = result {
        let mut rollback_errors = Vec::new();
        for target in changed.into_iter().rev() {
            let relative = target.strip_prefix(destination).unwrap();
            let original = backup.join(relative);
            let restore = if original.is_file() {
                std::fs::copy(&original, &target).map(|_| ())
            } else if target.is_file() {
                std::fs::remove_file(&target)
            } else {
                Ok(())
            };
            if let Err(restore_error) = restore {
                rollback_errors.push(format!("{}: {restore_error}", target.display()));
            }
        }
        if rollback_errors.is_empty() {
            let _ = std::fs::remove_dir_all(backup);
            return Err(error);
        }
        return Err(core::CoreError(format!(
            "{error}; rollback incomplete (originals retained at {}): {}",
            backup.display(),
            rollback_errors.join("; ")
        )));
    }
    if let Err(error) = std::fs::remove_dir_all(backup) {
        eprintln!(
            "gameplay refresh retained old-file backup {}: {error}",
            backup.display()
        );
    }
    Ok(())
}

fn refresh_configs(source: &str, config_dir: &str) -> Result<usize, core::CoreError> {
    let indexed = core::archive::SourceTree::open(source.as_ref())?;
    let destination = std::path::Path::new(config_dir);
    let parent = destination
        .parent()
        .ok_or_else(|| core::CoreError("gameplay destination has no parent".into()))?;
    let stem = destination
        .file_name()
        .ok_or_else(|| core::CoreError("gameplay destination has no name".into()))?
        .to_string_lossy();
    std::fs::create_dir_all(parent)?;
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|error| core::CoreError(error.to_string()))?
        .as_nanos();
    let staging = parent.join(format!(".{stem}.refresh-{}-{stamp}", std::process::id()));
    let backup = parent.join(format!(".{stem}.previous-{}-{stamp}", std::process::id()));
    std::fs::create_dir(&staging)?;
    let mut ready_to_install = false;
    let result = (|| -> Result<usize, core::CoreError> {
        if destination.is_dir() {
            preserve(destination, destination, &staging).map_err(|error| {
                core::CoreError(format!("preserve unrelated gameplay files: {error}"))
            })?;
        } else if destination.exists() {
            return Err(core::CoreError(
                "gameplay destination is not a directory".into(),
            ));
        }
        let configs = core::configs::export_source(&indexed, &staging)
            .map_err(|error| core::CoreError(format!("standalone gameplay export: {error}")))?;
        let audio = core::audio::export_gameplay(&indexed, &staging)
            .map_err(|error| core::CoreError(format!("audio gameplay export: {error}")))?;
        core::configs::export_character_clip_plan(&indexed, &staging.join("character_clips.json"))
            .map_err(|error| core::CoreError(format!("character clip plan export: {error}")))?;
        let characters = core::models::export_character_gameplay(&indexed, &staging)
            .map_err(|error| core::CoreError(format!("character gameplay export: {error}")))?;
        ready_to_install = true;
        if destination.exists()
            && let Err(error) = std::fs::rename(destination, &backup)
        {
            if error.kind() == std::io::ErrorKind::PermissionDenied {
                install_filewise(&staging, destination, &backup).map_err(|install_error| core::CoreError(format!(
                        "directory swap denied ({error}); filewise gameplay install failed: {install_error}; complete refresh retained at {}", staging.display()
                    )))?;
                if let Err(cleanup_error) = std::fs::remove_dir_all(&staging) {
                    eprintln!(
                        "gameplay refresh retained completed staging {}: {cleanup_error}",
                        staging.display()
                    );
                }
                return Ok(configs + audio + characters + 1);
            }
            return Err(core::CoreError(format!(
                "move existing gameplay directory {} to {}: {error}; complete refresh retained at {}",
                destination.display(),
                backup.display(),
                staging.display()
            )));
        }
        if let Err(error) = std::fs::rename(&staging, destination) {
            if backup.exists() {
                let _ = std::fs::rename(&backup, destination);
            }
            return Err(core::CoreError(format!(
                "install refreshed gameplay directory {}: {error}; complete refresh retained at {}",
                destination.display(),
                staging.display()
            )));
        }
        if backup.exists()
            && let Err(error) = std::fs::remove_dir_all(&backup)
        {
            eprintln!(
                "gameplay refresh retained previous directory {}: {error}",
                backup.display()
            );
        }
        Ok(configs + audio + characters + 1)
    })();
    if result.is_err() && !ready_to_install {
        let _ = std::fs::remove_dir_all(staging);
    }
    result
}

fn audit_configs(source: &str, config_dir: &str) -> Result<String, core::CoreError> {
    let indexed = core::archive::SourceTree::open(source.as_ref())?;
    let (checked, unused, character, audio, files) = core::configs::audit_source(
        &indexed,
        config_dir.as_ref(),
        &core::progress::Phase::noop(),
    )?;
    let sound = core::audio::audit_gameplay(&indexed, config_dir.as_ref())?;
    let clips = core::configs::audit_character_clip_plan(
        &indexed,
        &std::path::Path::new(config_dir).join("character_clips.json"),
    )?;
    let characters = core::models::audit_character_gameplay(&indexed, config_dir.as_ref())?;
    Ok(format!(
        "verified {checked} standalone CFG/MAP, {characters} character, {sound} audio gameplay documents and {clips} character clip plans against {files} source files ({unused} unused retail scripts; {character} character-owned, {audio} audio-owned)\n"
    ))
}

#[cfg(test)]
mod gameplay_refresh_tests {
    use super::install_filewise;

    #[test]
    fn filewise_refresh_preserves_unrelated_files_and_removes_obsolete_generated_files() {
        let root = std::env::temp_dir().join(format!(
            "kazarov-gameplay-refresh-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let destination = root.join("gameplay");
        let staging = root.join("staging");
        let backup = root.join("backup");
        std::fs::create_dir_all(destination.join("world")).unwrap();
        std::fs::create_dir_all(staging.join("world")).unwrap();
        std::fs::write(destination.join("world/old.cfg.json"), b"old").unwrap();
        std::fs::write(destination.join("world/obsolete.map.json"), b"obsolete").unwrap();
        std::fs::write(destination.join("README.md"), b"user note").unwrap();
        std::fs::write(staging.join("world/old.cfg.json"), b"new").unwrap();
        std::fs::write(staging.join("world/new.character.json"), b"character").unwrap();

        install_filewise(&staging, &destination, &backup).unwrap();

        assert_eq!(
            std::fs::read(destination.join("world/old.cfg.json")).unwrap(),
            b"new"
        );
        assert_eq!(
            std::fs::read(destination.join("world/new.character.json")).unwrap(),
            b"character"
        );
        assert!(!destination.join("world/obsolete.map.json").exists());
        assert_eq!(
            std::fs::read(destination.join("README.md")).unwrap(),
            b"user note"
        );
        assert!(!backup.exists());
        std::fs::remove_dir_all(root).unwrap();
    }
}

fn configs_parity(
    source: &str,
    archive: &str,
    config_dir: &str,
    fixture_dir: &str,
    texture_oracle: &str,
) -> Result<usize, core::CoreError> {
    let indexed = core::archive::SourceTree::open(source.as_ref())?;
    let configs = core::configs::export_parity_fixture(
        &indexed,
        archive.as_ref(),
        config_dir.as_ref(),
        fixture_dir.as_ref(),
        if texture_oracle.is_empty() {
            None
        } else {
            Some(texture_oracle.as_ref())
        },
    )?;
    core::audio::export_movie_parity_fixture(&indexed, archive.as_ref(), fixture_dir.as_ref())?;
    Ok(configs)
}

fn check(data_root: &str) -> Result<ffi::CheckReport, core::CoreError> {
    let report = core::check(data_root.as_ref())?;
    Ok(ffi::CheckReport {
        textures: report.textures,
    })
}

fn current(source: &str, data_root: &str) -> Result<bool, core::CoreError> {
    core::current(source.as_ref(), data_root.as_ref())
}

fn current_with_script_policy(
    source: &str,
    data_root: &str,
    policy: ffi::ScriptPublicationPolicy,
) -> Result<bool, core::CoreError> {
    let policy = core_script_publication_policy(policy)?;
    core::current_with_policy(source.as_ref(), data_root.as_ref(), policy)
}

fn open_archive(path: &str) -> Result<Box<Archive>, core::CoreError> {
    Ok(Box::new(Archive::open(path.as_ref())?))
}

fn archive_entry_count(archive: &Archive) -> usize {
    archive.len()
}

fn archive_entry_name(archive: &Archive, index: usize) -> &str {
    archive.name(index)
}

fn archive_entry_size(archive: &Archive, index: usize) -> u64 {
    archive.size(index)
}

fn archive_entry_crc(archive: &Archive, index: usize) -> u32 {
    archive.crc(index)
}

fn archive_entry_stored(archive: &Archive, index: usize) -> bool {
    archive.stored(index)
}

fn archive_entry_data_offset(archive: &Archive, index: usize) -> u64 {
    archive.data_offset(index)
}

fn archive_read_entry(
    archive: &Archive,
    index: usize,
    out: &mut [u8],
) -> Result<(), core::CoreError> {
    archive.read_into(index, out)
}

fn archive_path(archive: &Archive) -> String {
    archive.path().to_string_lossy().into_owned()
}

fn pack_directory_stored(
    source: &str,
    output: &str,
    workers: usize,
) -> Result<ffi::PackReport, core::CoreError> {
    let report =
        core::classic_zip::pack_directory_stored(source.as_ref(), output.as_ref(), workers)?;
    Ok(ffi::PackReport {
        files: report.files,
        bytes: report.bytes,
    })
}

fn pack_directory(source: &str, output: &str) -> Result<(), core::CoreError> {
    core::zip_archive::pack_directory(source.as_ref(), output.as_ref()).map(|_| ())
}

fn png_decode_rgba(png: &[u8], width: u32, height: u32, out: &mut [u8]) -> Result<(), String> {
    let mut decoder = png::Decoder::new(std::io::Cursor::new(png));
    decoder.set_transformations(
        png::Transformations::EXPAND | png::Transformations::STRIP_16 | png::Transformations::ALPHA,
    );
    let mut reader = decoder.read_info().map_err(|error| error.to_string())?;
    let info = reader.info();
    if info.width != width || info.height != height {
        return Err(format!(
            "PNG is {}x{}, expected {width}x{height}",
            info.width, info.height
        ));
    }
    let pixels = width as usize * height as usize;
    if out.len() != pixels * 4 {
        return Err("RGBA buffer has the wrong size".into());
    }
    let (color, depth) = reader.output_color_type();
    if depth != png::BitDepth::Eight {
        return Err("PNG does not decode to 8-bit samples".into());
    }
    match color {
        png::ColorType::Rgba => {
            reader.next_frame(out).map_err(|error| error.to_string())?;
        }
        png::ColorType::GrayscaleAlpha => {
            let mut gray = vec![0; pixels * 2];
            reader
                .next_frame(&mut gray)
                .map_err(|error| error.to_string())?;
            for (rgba, ga) in out
                .as_chunks_mut::<4>()
                .0
                .iter_mut()
                .zip(gray.as_chunks::<2>().0)
            {
                *rgba = [ga[0], ga[0], ga[0], ga[1]];
            }
        }
        _ => return Err(format!("unexpected PNG output colour {color:?}")),
    }
    Ok(())
}

#[cfg(test)]
mod png_tests {
    fn encode(color: png::ColorType, data: &[u8], palette: Option<&[u8]>) -> Vec<u8> {
        let mut bytes = Vec::new();
        let mut encoder = png::Encoder::new(&mut bytes, 2, 1);
        encoder.set_color(color);
        encoder.set_depth(png::BitDepth::Eight);
        if let Some(palette) = palette {
            encoder.set_palette(palette.to_vec());
        }
        encoder
            .write_header()
            .unwrap()
            .write_image_data(data)
            .unwrap();
        bytes
    }

    fn decode(png: &[u8]) -> Vec<u8> {
        let mut out = vec![0; 8];
        super::png_decode_rgba(png, 2, 1, &mut out).unwrap();
        out
    }

    #[test]
    fn decodes_every_colour_type_to_rgba() {
        assert_eq!(
            decode(&encode(
                png::ColorType::Rgba,
                &[1, 2, 3, 4, 5, 6, 7, 8],
                None
            )),
            [1, 2, 3, 4, 5, 6, 7, 8]
        );
        assert_eq!(
            decode(&encode(png::ColorType::Rgb, &[1, 2, 3, 5, 6, 7], None)),
            [1, 2, 3, 255, 5, 6, 7, 255]
        );
        assert_eq!(
            decode(&encode(png::ColorType::Grayscale, &[9, 10], None)),
            [9, 9, 9, 255, 10, 10, 10, 255]
        );
        assert_eq!(
            decode(&encode(
                png::ColorType::GrayscaleAlpha,
                &[9, 1, 10, 2],
                None
            )),
            [9, 9, 9, 1, 10, 10, 10, 2]
        );
        assert_eq!(
            decode(&encode(
                png::ColorType::Indexed,
                &[1, 0],
                Some(&[1, 2, 3, 4, 5, 6])
            )),
            [4, 5, 6, 255, 1, 2, 3, 255]
        );
        let mut out = vec![0; 8];
        assert!(
            super::png_decode_rgba(&encode(png::ColorType::Rgb, &[0; 6], None), 1, 2, &mut out)
                .is_err()
        );
    }
}
