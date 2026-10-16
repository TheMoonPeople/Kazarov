//! Publishes ordinary retail PACK containers as directly readable asset directories.
use super::{CoreError, CoreResult, models};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}
fn collect(root: &Path, output: &mut Vec<PathBuf>) -> CoreResult<()> {
    for entry in fs::read_dir(root)? {
        let path = entry?.path();
        if path.is_dir() {
            collect(&path, output)?;
        } else if path.is_file() {
            output.push(path);
        }
    }
    Ok(())
}
fn extension(alias: &str) -> &str {
    alias
        .split('#')
        .next()
        .unwrap_or(alias)
        .rsplit('.')
        .next()
        .unwrap_or("")
}
fn component(value: &str) -> String {
    if value == "." || value == ".." {
        return "%2E".repeat(value.len());
    }
    value
        .bytes()
        .map(|byte| {
            if byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-') {
                (byte as char).to_string()
            } else {
                format!("%{byte:02X}")
            }
        })
        .collect()
}
fn folder_path(alias: &str) -> PathBuf {
    alias
        .split('/')
        .map(|part| {
            let part = part.split_once("#entry:").map_or_else(
                || part.to_owned(),
                |(base, ordinal)| format!("{base}.entry_{ordinal}"),
            );
            component(&part)
        })
        .collect()
}
fn leaf_path(alias: &str) -> PathBuf {
    let mut parts: Vec<_> = alias.split('/').map(str::to_owned).collect();
    let leaf = parts.pop().unwrap();
    let leaf = if let Some((base, ordinal)) = leaf.split_once("#entry:") {
        if let Some((stem, extension)) = base.rsplit_once('.') {
            format!("{stem}.entry_{ordinal}.{extension}")
        } else {
            format!("{base}.entry_{ordinal}")
        }
    } else {
        leaf
    };
    let mut path = folder_path(&parts.join("/"));
    path.push(component(&leaf));
    path
}
fn consumed_container(alias: &str) -> bool {
    matches!(
        extension(alias).to_ascii_lowercase().trim_end_matches('_'),
        "chr" | "snd" | "img" | "tm2"
    )
}
fn empty_archive(bytes: &[u8], alias: &str) -> bool {
    matches!(
        extension(alias).to_ascii_lowercase().as_str(),
        "pcp" | "pac" | "mpk" | "ipk" | "pak" | "chr" | "snd"
    ) && (bytes.is_empty() || (bytes.len() >= 76 && bytes.iter().all(|byte| *byte == 0)))
}
fn clean_empty_directories(path: &Path) -> CoreResult<()> {
    for entry in fs::read_dir(path)? {
        let child = entry?.path();
        if child.is_dir() {
            clean_empty_directories(&child)?;
            if fs::read_dir(&child)?.next().is_none() {
                fs::remove_dir(child)?;
            }
        }
    }
    Ok(())
}
fn publish_pack(
    raw: &[u8],
    current: Option<&[u8]>,
    alias: &str,
    published: &Path,
    depth: usize,
) -> CoreResult<bool> {
    if depth > 16 {
        return Err(fail("asset directory nesting exceeds 16"));
    }
    let original = match models::pack(raw) {
        Some(members) => members,
        None if empty_archive(raw, alias) => Vec::new(),
        None => return Ok(false),
    };
    if consumed_container(alias) {
        if current.is_some_and(|bytes| {
            models::pack(bytes).is_some_and(|members| members.iter().any(|member| member.size > 0))
        }) {
            return Err(fail(format!(
                "unconverted specialized PACK remains: {alias}"
            )));
        }
        return Ok(true);
    }
    let target = published.join("port_data").join(folder_path(alias));
    fs::create_dir_all(&target)?;
    let marker = target.join("pack.json");
    if marker.is_file() {
        let document: Value = serde_json::from_slice(&fs::read(&marker)?)?;
        if document != json!({"schema_version":1,"kind":"asset_directory"}) {
            return Err(fail("PACK marker collides with an existing asset"));
        }
    }
    fs::write(
        marker,
        serde_json::to_vec_pretty(&json!({"schema_version":1,"kind":"asset_directory"}))?,
    )?;
    let mut available = BTreeMap::new();
    if let Some(bytes) = current {
        let members = if empty_archive(bytes, alias) {
            Vec::new()
        } else {
            models::pack(bytes)
                .ok_or_else(|| fail(format!("published PACK structure changed: {alias}")))?
        };
        let mut ordinals = BTreeMap::<String, usize>::new();
        for member in members {
            let ordinal = ordinals.entry(member.name.clone()).or_default();
            available.insert(
                (member.name, *ordinal),
                &bytes[member.start..member.start + member.size],
            );
            *ordinal += 1;
        }
    }
    let mut names = BTreeSet::new();
    let mut ordinals = BTreeMap::<String, usize>::new();
    for (index, member) in original.iter().enumerate() {
        let child = if names.insert(member.name.clone()) {
            format!("{alias}/{}", member.name)
        } else {
            format!("{alias}/{}#entry:{index}", member.name)
        };
        let ordinal = ordinals.entry(member.name.clone()).or_default();
        let present = available
            .get(&(member.name.clone(), *ordinal))
            .copied()
            .filter(|bytes| !bytes.is_empty());
        *ordinal += 1;
        let source = &raw[member.start..member.start + member.size];
        if (member.size > 0 || empty_archive(source, &child))
            && publish_pack(source, present, &child, published, depth + 1)?
        {
            continue;
        }
        let Some(bytes) = present else {
            continue;
        };
        let target = published.join("port_data").join(leaf_path(&child));
        if target.file_name().is_some_and(|name| name == "pack.json") {
            return Err(fail("retail member collides with reserved pack.json"));
        }
        if target.exists() {
            return Err(fail(format!(
                "PACK member overwrites a native asset: {}",
                target.display()
            )));
        }
        fs::create_dir_all(target.parent().unwrap())?;
        fs::write(target, bytes)?;
    }
    Ok(true)
}

/// Converts every ordinary PACK to a directory while copying only surviving published member payloads.
pub fn convert_tree(raw: &Path, published: &Path) -> CoreResult<()> {
    let mut files = Vec::new();
    collect(raw, &mut files)?;
    files.sort();
    for source in files {
        let alias = source
            .strip_prefix(raw)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        let bytes = fs::read(&source)?;
        if models::pack(&bytes).is_none() && !empty_archive(&bytes, &alias) {
            continue;
        }
        let target = published
            .join("data")
            .join(source.strip_prefix(raw).unwrap());
        let current = if target.is_file() {
            Some(fs::read(&target)?)
        } else {
            None
        };
        if publish_pack(&bytes, current.as_deref(), &alias, published, 0)? && target.is_file() {
            fs::remove_file(target)?;
        }
    }
    let native = published.join("port_data");
    if native.is_dir() {
        clean_empty_directories(&native)?;
    }
    Ok(())
}

/// Rejects binary PACK containers and validates the minimal marker of every asset directory.
pub fn verify(root: &Path) -> CoreResult<()> {
    for name in ["data", "port_data"] {
        let directory = root.join(name);
        if !directory.is_dir() {
            continue;
        }
        let mut files = Vec::new();
        collect(&directory, &mut files)?;
        for path in files {
            audit_entry(&path.to_string_lossy(), &fs::read(&path)?)?;
        }
    }
    Ok(())
}

/// Rejects a binary PACK container or an invalid asset directory marker in one published file.
pub(crate) fn audit_entry(name: &str, bytes: &[u8]) -> CoreResult<()> {
    if models::pack(bytes).is_some() || empty_archive(bytes, name) {
        return Err(fail(format!("binary PACK remains: {name}")));
    }
    let file_name = name.rsplit(['/', '\\']).next().unwrap_or(name);
    if file_name == "pack.json" {
        let document: Value = serde_json::from_slice(bytes)?;
        if document != json!({"schema_version":1,"kind":"asset_directory"}) {
            return Err(fail("invalid asset directory marker"));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn pack(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut output = Vec::new();
        for (name, data) in entries {
            let mut header = vec![0; 80];
            header[..name.len()].copy_from_slice(name.as_bytes());
            for (offset, value) in [
                (64, 80),
                (68, data.len() as u32),
                (72, 80 + data.len() as u32),
            ] {
                header[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
            }
            output.extend(header);
            output.extend_from_slice(data);
        }
        output.extend([0; 80]);
        output
    }
    #[test]
    fn folders_preserve_surviving_members_and_never_restore_converted_payloads() {
        let temporary = tempfile::tempdir().unwrap();
        let raw = temporary.path().join("raw");
        let output = temporary.path().join("out");
        fs::create_dir_all(&raw).unwrap();
        fs::create_dir_all(output.join("data")).unwrap();
        let original = pack(&[
            ("model.mds", b"original model"),
            ("keep.bin", b"original keep"),
            ("nested.pac", &pack(&[("consumed.cfg", b"commands")])),
        ]);
        let current = pack(&[
            ("model.mds", b""),
            ("keep.bin", b"current keep"),
            ("nested.pac", &pack(&[("consumed.cfg", b"")])),
        ]);
        fs::write(raw.join("outer.pcp"), original).unwrap();
        fs::write(output.join("data/outer.pcp"), current).unwrap();
        convert_tree(&raw, &output).unwrap();
        verify(&output).unwrap();
        assert_eq!(
            fs::read(output.join("port_data/outer.pcp/keep.bin")).unwrap(),
            b"current keep"
        );
        assert!(
            output
                .join("port_data/outer.pcp/nested.pac/pack.json")
                .is_file()
        );
        assert!(!output.join("port_data/outer.pcp/model.mds").exists());
        assert!(!output.join("data/outer.pcp").exists());
    }
    #[test]
    fn raw_inventory_retains_empty_directory_and_duplicate_member_identity() {
        let temporary = tempfile::tempdir().unwrap();
        let raw = temporary.path().join("raw");
        let output = temporary.path().join("out");
        fs::create_dir_all(&raw).unwrap();
        fs::create_dir_all(output.join("data")).unwrap();
        let bytes = pack(&[("a.bin", b"one"), ("a.bin", b"two")]);
        fs::write(raw.join("dup.pac"), &bytes).unwrap();
        fs::write(output.join("data/dup.pac"), bytes).unwrap();
        fs::write(raw.join("empty.pac"), pack(&[("old.mds", b"removed")])).unwrap();
        fs::write(raw.join("zero.pcp"), b"").unwrap();
        fs::write(output.join("data/zero.pcp"), b"").unwrap();
        convert_tree(&raw, &output).unwrap();
        assert_eq!(
            fs::read(output.join("port_data/dup.pac/a.bin")).unwrap(),
            b"one"
        );
        assert_eq!(
            fs::read(output.join("port_data/dup.pac/a.entry_1.bin")).unwrap(),
            b"two"
        );
        assert!(output.join("port_data/empty.pac/pack.json").is_file());
        assert!(output.join("port_data/zero.pcp/pack.json").is_file());
        verify(&output).unwrap();
    }
    #[test]
    fn retained_pack_and_invalid_directory_marker_are_rejected() {
        let temporary = tempfile::tempdir().unwrap();
        fs::create_dir_all(temporary.path().join("data")).unwrap();
        fs::write(
            temporary.path().join("data/archive"),
            pack(&[("keep", b"data")]),
        )
        .unwrap();
        assert!(verify(temporary.path()).is_err());
        fs::remove_file(temporary.path().join("data/archive")).unwrap();
        fs::write(
            temporary.path().join("data/pack.json"),
            b"{\"schema_version\":1,\"members\":[]}",
        )
        .unwrap();
        assert!(verify(temporary.path()).is_err());
    }
    #[test]
    fn validated_vm_script_takes_precedence_over_coincidental_pack_header() {
        let mut script = vec![0; 700];
        script[..4].copy_from_slice(b"SB2\0");
        for (at, value) in [
            (4, 80u32),
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
            script[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
        assert!(super::super::native_scripts::is_valid_script(&script));
        assert!(models::pack(&script).is_none());
        let temp = tempfile::tempdir().unwrap();
        fs::create_dir_all(temp.path().join("data")).unwrap();
        fs::write(temp.path().join("data/arbitrary.bin"), &script).unwrap();
        verify(temp.path()).unwrap();
        script[4..8].fill(0);
        assert!(!super::super::native_scripts::is_valid_script(&script));
        assert!(models::pack(&script).is_some());
        fs::write(temp.path().join("data/arbitrary.bin"), script).unwrap();
        assert!(verify(temp.path()).is_err());
        let archive = pack(&[("SB2", b"actual archive member")]);
        assert!(models::pack(&archive).is_some());
        assert!(!super::super::native_scripts::is_valid_script(&archive));
    }
}
