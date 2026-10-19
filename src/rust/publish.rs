use super::{CoreError, CoreResult, textures};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}
fn digest(path: &Path) -> CoreResult<String> {
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
    let mut out = String::with_capacity(64);
    for byte in hasher.finalize() {
        out.push_str(&format!("{byte:02x}"));
    }
    Ok(out)
}
fn files(root: &Path, dir: &Path, out: &mut BTreeSet<String>) -> CoreResult<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
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
pub fn verify(root: &Path) -> CoreResult<u64> {
    let marker: Value = serde_json::from_slice(&fs::read(root.join("data.json"))?)?;
    if marker["schema_version"] != 1 || marker["data_version"] != super::data_version() {
        return Err(fail("data.json has an unsupported data version"));
    }
    let port = root.join("port_data");
    let data = root.join("data");
    let manifest: Value = serde_json::from_slice(&fs::read(port.join("manifest.json"))?)?;
    if manifest["schema_version"] != 1 {
        return Err(fail("unsupported port_data manifest version"));
    }
    let port_files = manifest["files"]
        .as_object()
        .ok_or_else(|| fail("invalid port_data manifest files"))?;
    let data_files = manifest["data_files"]
        .as_object()
        .ok_or_else(|| fail("invalid data manifest files"))?;
    for (relative, expected) in port_files {
        if digest(&port.join(relative))? != expected.as_str().unwrap_or("") {
            return Err(fail(format!("port_data asset changed: {relative}")));
        }
    }
    for (relative, expected) in data_files {
        let path = data.join(relative);
        if digest(&path)? != expected.as_str().unwrap_or("") {
            return Err(fail(format!("extracted data changed: {relative}")));
        }
        if relative != "manifest.json" {
            if fs::metadata(&path)?.len() <= 128 << 20 {
                textures::audit_payload(&fs::read(path)?, relative)?;
            } else if !textures::large_known_non_texture(relative) {
                return Err(fail(format!("unrecognized large data file: {relative}")));
            }
        }
    }
    let data_manifest: Value = serde_json::from_slice(&fs::read(data.join("manifest.json"))?)?;
    let mut expected_data = data_files.clone();
    expected_data.remove("manifest.json");
    if data_manifest["schema_version"] != 1
        || data_manifest["files"] != Value::Object(expected_data)
    {
        return Err(fail("data manifest disagrees with port_data manifest"));
    }
    let catalog: Value = serde_json::from_slice(&fs::read(port.join("catalog.json"))?)?;
    if catalog["schema_version"] != 1 {
        return Err(fail("unsupported texture catalog version"));
    }
    let mut actual_port = BTreeSet::new();
    files(&port, &port, &mut actual_port)?;
    let mut expected_port: BTreeSet<String> = port_files.keys().cloned().collect();
    expected_port.insert("manifest.json".into());
    if actual_port != expected_port {
        return Err(fail("unmanifested or missing port_data files"));
    }
    let mut actual_data = BTreeSet::new();
    files(&data, &data, &mut actual_data)?;
    let expected_data: BTreeSet<String> = data_files.keys().cloned().collect();
    if actual_data != expected_data {
        return Err(fail("unmanifested or missing data files"));
    }
    Ok(port_files
        .keys()
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
    fs::write(guard.path.join("stage.json"), serde_json::to_vec(metadata)?)?;
    Ok(guard)
}
fn rename_retry(from: &Path, to: &Path) -> CoreResult<()> {
    for attempt in 0..30 {
        match fs::rename(from, to) {
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
                    "rename {} -> {}: {error}",
                    from.display(),
                    to.display()
                )));
            }
        }
    }
    unreachable!()
}
pub fn publish(staged: &Path, root: &Path) -> CoreResult<()> {
    verify(staged)?;
    fs::create_dir_all(root)?;
    let data = root.join("data");
    let port = root.join("port_data");
    let marker = root.join("data.json");
    let staged_data = staged.join("data");
    let staged_port = staged.join("port_data");
    let staged_marker = staged.join("data.json");
    if port.join("manifest.json").is_file()
        && marker.is_file()
        && fs::read(port.join("manifest.json"))? == fs::read(staged_port.join("manifest.json"))?
        && fs::read(&marker)? == fs::read(&staged_marker)?
        && verify(root).is_ok()
    {
        return Ok(());
    }
    let backup_data = unique(root, "data-backup");
    let backup_port = unique(root, "port-data-backup");
    let backup_marker = unique(root, "marker-backup");
    let mut old_data = false;
    let mut old_port = false;
    let mut old_marker = false;
    let mut new_data = false;
    let mut new_port = false;
    let mut new_marker = false;
    let result = (|| -> CoreResult<()> {
        if marker.is_file() {
            rename_retry(&marker, &backup_marker)?;
            old_marker = true;
        }
        if data.exists() {
            rename_retry(&data, &backup_data)?;
            old_data = true;
        }
        if port.exists() {
            rename_retry(&port, &backup_port)?;
            old_port = true;
        }
        rename_retry(&staged_data, &data)?;
        new_data = true;
        rename_retry(&staged_port, &port)?;
        new_port = true;
        rename_retry(&staged_marker, &marker)?;
        new_marker = true;
        verify(root)?;
        Ok(())
    })();
    if let Err(error) = result {
        let mut rollback = Vec::new();
        if new_data && let Err(e) = rename_retry(&data, &staged_data) {
            rollback.push(e.to_string());
        }
        if new_port && let Err(e) = rename_retry(&port, &staged_port) {
            rollback.push(e.to_string());
        }
        if new_marker && let Err(e) = rename_retry(&marker, &staged_marker) {
            rollback.push(e.to_string());
        }
        if old_data && let Err(e) = rename_retry(&backup_data, &data) {
            rollback.push(e.to_string());
        }
        if old_port && let Err(e) = rename_retry(&backup_port, &port) {
            rollback.push(e.to_string());
        }
        if old_marker && let Err(e) = rename_retry(&backup_marker, &marker) {
            rollback.push(e.to_string());
        }
        return Err(if rollback.is_empty() {
            error
        } else {
            fail(format!("{error}; rollback: {}", rollback.join("; ")))
        });
    }
    if old_data {
        fs::remove_dir_all(backup_data)?;
    }
    if old_port {
        fs::remove_dir_all(backup_port)?;
    }
    if old_marker {
        fs::remove_file(backup_marker)?;
    }
    Ok(())
}
