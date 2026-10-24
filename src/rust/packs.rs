//! Publishes ordinary retail PACK containers as directly readable asset directories.
use super::artifacts::{ArtifactBytes, AssetSink, Category, Job};
use super::inventory::Inventory;
use super::progress::Phase;
use super::{CoreError, CoreResult, models};
use rayon::prelude::*;
use serde_json::{Value, json};
use std::collections::HashSet;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// Final treatment of one retail leaf or wholly converted specialized container.
pub enum RetainedEdit {
    Remove,
    Replace(Vec<u8>),
}

/// Thread-safe scrub decisions collected before surviving assets are submitted.
#[derive(Default)]
pub struct RetainedEdits(Mutex<BTreeMap<String, RetainedEdit>>);

impl RetainedEdits {
    pub fn record(&self, alias: impl Into<String>, edit: RetainedEdit) -> CoreResult<()> {
        let alias = alias.into();
        let mut edits = self
            .0
            .lock()
            .map_err(|_| fail("retained edits lock poisoned"))?;
        if edits.contains_key(&alias) {
            return Err(fail(format!("conflicting retained edits: {alias}")));
        }
        edits.insert(alias, edit);
        Ok(())
    }

    pub fn contains(&self, alias: &str) -> bool {
        self.0.lock().is_ok_and(|edits| edits.contains_key(alias))
    }

    fn take(&self, alias: &str) -> CoreResult<Option<RetainedEdit>> {
        Ok(self
            .0
            .lock()
            .map_err(|_| fail("retained edits lock poisoned"))?
            .remove(alias))
    }
}

/// Flattens surviving PACK members and borrowed standalone bytes into final archive entries.
pub fn assemble_retained(
    inventory: &Inventory,
    edits: &RetainedEdits,
    sink: &AssetSink,
) -> CoreResult<()> {
    fn submit(
        inventory: &Inventory,
        edits: &RetainedEdits,
        sink: &AssetSink,
        alias: &str,
        nested: bool,
    ) -> CoreResult<()> {
        let edit = edits.take(alias)?;
        if matches!(edit.as_ref(), Some(RetainedEdit::Remove)) {
            return Ok(());
        }
        let resource = inventory
            .get(alias)
            .ok_or_else(|| fail(format!("missing retail resource: {alias}")))?;
        let members = inventory.members(alias);
        if members.is_some() || empty_archive(resource.bytes(), alias) {
            if consumed_container(alias) {
                return Ok(());
            }
            if matches!(edit.as_ref(), Some(RetainedEdit::Replace(_))) {
                return Err(fail(format!("replace PACK members individually: {alias}")));
            }
            let marker = format!(
                "{}/pack.json",
                folder_path(alias).to_string_lossy().replace('\\', "/")
            );
            sink.submit(
                marker,
                ArtifactBytes::Owned(serde_json::to_vec(
                    &json!({"schema_version":1,"kind":"asset_directory"}),
                )?),
                Category::Pack,
            )?;
            if let Some(members) = members {
                for member in members {
                    if member.resource.is_empty()
                        && !empty_archive(member.resource.bytes(), &member.alias)
                    {
                        continue;
                    }
                    submit(inventory, edits, sink, &member.alias, true)?;
                }
            }
            return Ok(());
        }
        if nested && resource.is_empty() {
            return Ok(());
        }
        let path = if nested {
            leaf_path(alias)
        } else {
            PathBuf::from(alias)
        };
        let name = path.to_string_lossy().replace('\\', "/");
        let bytes = match edit {
            Some(RetainedEdit::Replace(bytes)) => ArtifactBytes::Owned(bytes),
            Some(RetainedEdit::Remove) => return Ok(()),
            None => ArtifactBytes::SourceRange(resource.clone()),
        };
        sink.submit(name, bytes, Category::Preserved)
    }
    for root in inventory.roots() {
        submit(inventory, edits, sink, &root.alias, false)?;
    }
    Ok(())
}

/// Plans independent ZIP jobs for source bytes not claimed by a converter.
pub fn plan_jobs(
    inventory: &Inventory,
    consumed: &HashSet<String>,
    texture_documents: &BTreeMap<String, String>,
) -> CoreResult<Vec<Job>> {
    fn visit(
        inventory: &Inventory,
        consumed: &HashSet<String>,
        alias: &str,
        nested: bool,
        jobs: &mut Vec<Job>,
        texture_documents: &BTreeMap<String, String>,
    ) -> CoreResult<()> {
        if consumed.contains(alias) {
            return Ok(());
        }
        let resource = inventory
            .get(alias)
            .ok_or_else(|| fail(format!("missing retail resource: {alias}")))?;
        let members = inventory.members(alias);
        if members.is_some() || empty_archive(resource.bytes(), alias) {
            if consumed_container(alias) {
                return Ok(());
            }
            let name = format!(
                "{}/pack.json",
                folder_path(alias).to_string_lossy().replace('\\', "/")
            );
            let bytes =
                serde_json::to_vec(&directory_marker(inventory, alias, texture_documents)?)?;
            jobs.push(Job {
                category: Category::Pack,
                label: name.clone(),
                work: 1,
                estimated_bytes: bytes.len(),
                run: Box::new(move |sink| {
                    sink.submit(name, ArtifactBytes::Owned(bytes), Category::Pack)
                }),
            });
            if let Some(members) = members {
                for member in members {
                    if member.resource.is_empty()
                        && !empty_archive(member.resource.bytes(), &member.alias)
                    {
                        continue;
                    }
                    visit(
                        inventory,
                        consumed,
                        &member.alias,
                        true,
                        jobs,
                        texture_documents,
                    )?;
                }
            }
            return Ok(());
        }
        if nested && resource.is_empty() {
            return Ok(());
        }
        let path = if nested {
            leaf_path(alias)
        } else {
            PathBuf::from(alias)
        };
        let name = path.to_string_lossy().replace('\\', "/");
        let resource = resource.clone();
        jobs.push(Job {
            category: Category::Preserved,
            label: name.clone(),
            work: resource.len().max(1) as u64,
            estimated_bytes: resource.len(),
            run: Box::new(move |sink| {
                sink.submit(
                    name,
                    ArtifactBytes::SourceRange(resource),
                    Category::Preserved,
                )
            }),
        });
        Ok(())
    }
    let mut jobs = Vec::new();
    for root in inventory.roots() {
        visit(
            inventory,
            consumed,
            &root.alias,
            false,
            &mut jobs,
            texture_documents,
        )?;
    }
    Ok(jobs)
}

/// Records only relocated direct texture members belonging to this PACK.
fn directory_marker(
    inventory: &Inventory,
    alias: &str,
    texture_documents: &BTreeMap<String, String>,
) -> CoreResult<Value> {
    let mut document = json!({"schema_version":1,"kind":"asset_directory"});
    let mut textures = BTreeMap::new();
    for member in inventory.members(alias).unwrap_or_default() {
        if let Some(path) = texture_documents.get(&member.alias) {
            let logical = super::textures::legacy_document_path(&member.alias)?;
            let name = logical.rsplit('/').next().unwrap_or(&logical);
            if textures
                .insert(name.to_ascii_lowercase(), json!({"name":name,"path":path}))
                .is_some()
            {
                return Err(fail(format!(
                    "duplicate logical texture member: {alias}/{name}"
                )));
            }
        }
    }
    if !textures.is_empty() {
        document["texture_members"] = Value::Array(textures.into_values().collect());
    }
    Ok(document)
}

/// Accepts safe direct logical names and explicit physical texture paths.
fn valid_marker(document: &Value) -> bool {
    let Some(object) = document.as_object() else {
        return false;
    };
    if object.get("schema_version") != Some(&json!(1))
        || object.get("kind") != Some(&json!("asset_directory"))
        || object
            .keys()
            .any(|key| !matches!(key.as_str(), "schema_version" | "kind" | "texture_members"))
    {
        return false;
    }
    let Some(members) = object.get("texture_members") else {
        return true;
    };
    let Some(members) = members.as_array() else {
        return false;
    };
    let mut names = BTreeSet::new();
    let safe = |value: &str| {
        !value.is_empty()
            && !value.contains(['\\', ':', '\0'])
            && value
                .split('/')
                .all(|part| !matches!(part, "" | "." | ".."))
    };
    members.iter().all(|member| {
        let Some(object) = member.as_object() else {
            return false;
        };
        let (Some(name), Some(path)) = (
            object.get("name").and_then(Value::as_str),
            object.get("path").and_then(Value::as_str),
        ) else {
            return false;
        };
        object.len() == 2
            && safe(name)
            && !name.contains('/')
            && name.to_ascii_lowercase().ends_with(".json")
            && safe(path)
            && path.starts_with("textures/")
            && path.to_ascii_lowercase().ends_with(".json")
            && names.insert(name.to_ascii_lowercase())
    })
}

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

/// Returns the final flattened path for a retained member of a PACK directory.
pub(crate) fn published_leaf_path(alias: &str) -> String {
    leaf_path(alias).to_string_lossy().replace('\\', "/")
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
    let target = published.join("data").join(folder_path(alias));
    fs::create_dir_all(&target)?;
    let marker = target.join("pack.json");
    if marker.is_file() {
        let document: Value = serde_json::from_slice(&fs::read(&marker)?)?;
        if !valid_marker(&document) {
            return Err(fail("PACK marker collides with an existing asset"));
        }
    }
    fs::write(
        marker,
        serde_json::to_vec(&json!({"schema_version":1,"kind":"asset_directory"}))?,
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
        let target = published.join("data").join(leaf_path(&child));
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

fn publish_source(
    source_tree: &super::archive::SourceTree,
    published: &Path,
    alias: &str,
    phase: &Phase,
) -> CoreResult<()> {
    let bytes = source_tree.read(alias)?;
    if bytes.len() >= 2 << 20 {
        phase.item(alias);
    }
    let target = published.join("data").join(alias);
    let work = super::archive::compatibility_work_path(published, alias);
    let current_path = if work.is_file() {
        work
    } else if target.is_file() {
        fs::create_dir_all(
            work.parent()
                .ok_or_else(|| fail("PACK work path has no parent"))?,
        )?;
        fs::rename(&target, &work)?;
        work
    } else {
        target.clone()
    };
    let current = if current_path.is_file() {
        Some(fs::read(&current_path)?)
    } else {
        None
    };
    if publish_pack(&bytes, current.as_deref(), alias, published, 0)? && current_path.is_file() {
        fs::remove_file(current_path)?;
    }
    phase.inc(1);
    Ok(())
}
fn disjoint_folders(folders: &[&str]) -> bool {
    let nested = |ancestor: &str, path: &str| {
        path.strip_prefix(ancestor)
            .is_some_and(|rest| rest.starts_with('/'))
    };
    folders.iter().enumerate().all(|(i, a)| {
        folders
            .iter()
            .enumerate()
            .all(|(j, b)| i == j || (a != b && !nested(a, b) && !nested(b, a)))
    })
}

/// Converts every ordinary PACK to a directory while copying only surviving published member payloads.
pub fn convert_tree(raw: &Path, published: &Path, phase: &Phase) -> CoreResult<()> {
    convert_source(
        &super::archive::SourceTree::directory(raw)?,
        published,
        phase,
    )
}

pub fn convert_source(
    source_tree: &super::archive::SourceTree,
    published: &Path,
    phase: &Phase,
) -> CoreResult<()> {
    let mut files: Vec<&str> = source_tree.names().collect();
    files.sort();
    phase.set_total(files.len() as u64);
    let scanned: Vec<CoreResult<Option<(String, String)>>> = files
        .par_iter()
        .map(|alias| -> CoreResult<Option<(String, String)>> {
            let bytes = source_tree.read(alias)?;
            if models::pack(&bytes).is_none() && !empty_archive(&bytes, alias) {
                phase.inc(1);
                return Ok(None);
            }
            let folder = folder_path(alias)
                .to_string_lossy()
                .replace('\\', "/")
                .to_lowercase();
            Ok(Some((alias.to_string(), folder)))
        })
        .collect();
    let mut packs = Vec::new();
    for (_, entry) in files.iter().zip(scanned) {
        if let Some((alias, folder)) = entry? {
            packs.push((alias, folder));
        }
    }
    let folders: Vec<&str> = packs.iter().map(|(_, folder)| folder.as_str()).collect();
    if disjoint_folders(&folders) {
        let results: Vec<CoreResult<()>> = packs
            .par_iter()
            .map(|(alias, _)| publish_source(source_tree, published, alias, phase))
            .collect();
        for result in results {
            result?;
        }
    } else {
        for (alias, _) in &packs {
            publish_source(source_tree, published, alias, phase)?;
        }
    }
    let native = published.join("data");
    if native.is_dir() {
        clean_empty_directories(&native)?;
    }
    Ok(())
}

/// Rejects binary PACK containers and validates the minimal marker of every asset directory.
pub fn verify(root: &Path) -> CoreResult<()> {
    for name in ["data"] {
        let directory = root.join(name);
        if !directory.is_dir() {
            continue;
        }
        let mut files = Vec::new();
        collect(&directory, &mut files)?;
        files.sort();
        let results: Vec<CoreResult<()>> = files
            .par_iter()
            .map(|path| -> CoreResult<()> {
                audit_entry(&path.to_string_lossy(), &fs::read(path)?)
            })
            .collect();
        for result in results {
            result?;
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
        if !valid_marker(&document) {
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
    fn marker_preserves_only_direct_texture_members() {
        let temp = tempfile::tempdir().unwrap();
        let nested = pack(&[("other.img", b"image")]);
        fs::write(
            temp.path().join("one.pac"),
            pack(&[("owned.img", b"image"), ("nested.pac", &nested)]),
        )
        .unwrap();
        fs::write(
            temp.path().join("two.pac"),
            pack(&[("unrelated.img", b"image")]),
        )
        .unwrap();
        let source = super::super::archive::SourceTree::directory(temp.path()).unwrap();
        let inventory = Inventory::build(&source).unwrap();
        let documents = BTreeMap::from([
            (
                "one.pac/owned.img".into(),
                "textures/shared/owned.json".into(),
            ),
            (
                "one.pac/nested.pac/other.img".into(),
                "textures/shared/other.json".into(),
            ),
            (
                "two.pac/unrelated.img".into(),
                "textures/shared/unrelated.json".into(),
            ),
        ]);
        let marker = directory_marker(&inventory, "one.pac", &documents).unwrap();
        assert_eq!(
            marker["texture_members"],
            json!([{"name":"owned.json","path":"textures/shared/owned.json"}])
        );
        assert!(valid_marker(&marker));
        audit_entry("one.pac/pack.json", &serde_json::to_vec(&marker).unwrap()).unwrap();
        for members in [
            json!([{"name":"../bad.json","path":"textures/a.json"}]),
            json!([{"name":"a.json","path":"textures/../a.json"}]),
            json!([{"name":"a.json","path":"data/a.json"}]),
            json!([{"name":"a.json","path":"textures/a.json"},{"name":"A.json","path":"textures/b.json"}]),
        ] {
            assert!(!valid_marker(
                &json!({"schema_version":1,"kind":"asset_directory","texture_members":members})
            ));
        }
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
        let work = super::super::archive::compatibility_work_path(&output, "outer.pcp");
        fs::create_dir_all(work.parent().unwrap()).unwrap();
        fs::write(&work, current).unwrap();
        convert_tree(&raw, &output, &Phase::noop()).unwrap();
        verify(&output).unwrap();
        assert_eq!(
            fs::read(output.join("data/outer.pcp/keep.bin")).unwrap(),
            b"current keep"
        );
        assert!(output.join("data/outer.pcp/nested.pac/pack.json").is_file());
        assert!(!output.join("data/outer.pcp/model.mds").exists());
        assert!(!work.exists());
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
        convert_tree(&raw, &output, &Phase::noop()).unwrap();
        assert_eq!(fs::read(output.join("data/dup.pac/a.bin")).unwrap(), b"one");
        assert_eq!(
            fs::read(output.join("data/dup.pac/a.entry_1.bin")).unwrap(),
            b"two"
        );
        assert!(output.join("data/empty.pac/pack.json").is_file());
        assert!(output.join("data/zero.pcp/pack.json").is_file());
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
