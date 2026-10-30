use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Path, PathBuf};

/// Builds the small distribution archive using aligned Zstandard ZIP entries.
fn gameplay_archive(documents: &[(String, Vec<u8>)]) -> Vec<u8> {
    let mut archive = Vec::new();
    let mut directory = Vec::new();
    for (name, bytes) in documents {
        let compressed = zstd::bulk::compress(bytes, 1).expect("compress gameplay document");
        let offset = u32::try_from(archive.len()).expect("gameplay archive exceeds ZIP32 limit");
        let size = u32::try_from(bytes.len()).expect("gameplay document exceeds ZIP32 limit");
        let compressed_size = u32::try_from(compressed.len()).expect("compressed gameplay size");
        let name_size = u16::try_from(name.len()).expect("gameplay path exceeds ZIP limit");
        let crc = crc32fast::hash(bytes);
        let mut padding = (64 - (archive.len() + 30 + name.len()) % 64) % 64;
        if padding > 0 && padding < 4 {
            padding += 64;
        }
        archive.extend(0x0403_4b50u32.to_le_bytes());
        for field in [63u16, 1 << 11, 93, 0, 33] {
            archive.extend(field.to_le_bytes());
        }
        for field in [crc, compressed_size, size] {
            archive.extend(field.to_le_bytes());
        }
        archive.extend(name_size.to_le_bytes());
        archive.extend((padding as u16).to_le_bytes());
        archive.extend(name.as_bytes());
        if padding > 0 {
            archive.extend(0xd935u16.to_le_bytes());
            archive.extend((padding as u16 - 4).to_le_bytes());
            archive.resize(archive.len() + padding - 4, 0);
        }
        archive.extend(compressed);
        directory.extend(0x0201_4b50u32.to_le_bytes());
        for field in [63u16, 63, 1 << 11, 93, 0, 33] {
            directory.extend(field.to_le_bytes());
        }
        for field in [crc, compressed_size, size] {
            directory.extend(field.to_le_bytes());
        }
        for field in [name_size, 0, 0, 0, 0] {
            directory.extend(field.to_le_bytes());
        }
        directory.extend(0u32.to_le_bytes());
        directory.extend(offset.to_le_bytes());
        directory.extend(name.as_bytes());
    }
    let count = u16::try_from(documents.len()).expect("too many gameplay documents for ZIP32");
    let offset = u32::try_from(archive.len()).expect("gameplay archive exceeds ZIP32 limit");
    let size = u32::try_from(directory.len()).expect("gameplay directory exceeds ZIP32 limit");
    archive.extend(directory);
    archive.extend(0x0605_4b50u32.to_le_bytes());
    for field in [0u16, 0, count, count] {
        archive.extend(field.to_le_bytes());
    }
    archive.extend(size.to_le_bytes());
    archive.extend(offset.to_le_bytes());
    archive.extend(0u16.to_le_bytes());
    archive
}

/// Writes generated files only when their content changes.
fn write_generated(path: &Path, bytes: &[u8]) {
    if fs::read(path).is_ok_and(|existing| existing == bytes) {
        return;
    }
    fs::create_dir_all(path.parent().expect("generated file directory"))
        .expect("create generated directory");
    fs::write(path, bytes).expect("write generated gameplay data");
}

/// Publishes a gameplay-only seed without touching the user's extracted archive.
fn publish_archive(out: &Path, documents: &[(String, Vec<u8>)]) {
    let archive = gameplay_archive(documents);
    write_generated(&out.join("data.zip"), &archive);
    if let Some(destination) = std::env::var_os("KAZAROV_GAMEPLAY_ARCHIVE") {
        let destination = PathBuf::from(destination);
        println!("cargo:rerun-if-changed={}", destination.display());
        write_generated(&destination, &archive);
    }
}

fn collect(root: &Path, directory: &Path, files: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(directory).expect("read tracked config directory") {
        let path = entry.expect("read tracked config entry").path();
        if path.is_dir() {
            collect(root, &path, files);
        } else if path.is_file() {
            assert!(path.starts_with(root));
            files.push(path);
        }
    }
}

fn main() {
    println!("cargo:rustc-check-cfg=cfg(kazarov_ffmpeg)");
    // Maps the disc image a resource at a time, as on iOS, for testing that path on other hosts.
    println!("cargo:rustc-check-cfg=cfg(kazarov_mapped_source)");
    println!("cargo:rerun-if-env-changed=KAZAROV_FFMPEG_INCLUDE");
    println!("cargo:rerun-if-env-changed=KAZAROV_FFMPEG_LIB");
    println!("cargo:rerun-if-changed=src/c/movie_mux.c");
    if let Some(include) = std::env::var_os("KAZAROV_FFMPEG_INCLUDE") {
        cc::Build::new()
            .file("src/c/movie_mux.c")
            .include(include)
            .compile("kazarov_movie_mux");
        println!("cargo:rustc-cfg=kazarov_ffmpeg");
        for name in ["avformat", "avcodec", "avutil"] {
            println!("cargo:rustc-link-lib=dylib={name}");
        }
        if let Some(lib) = std::env::var_os("KAZAROV_FFMPEG_LIB") {
            println!(
                "cargo:rustc-link-search=native={}",
                PathBuf::from(lib).display()
            );
        }
    }
    let configured_root = std::env::var_os("KAZAROV_GAMEPLAY_ROOT");
    let root = configured_root
        .as_ref()
        .map(|value| PathBuf::from(value.as_os_str()))
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../port/gameplay"));
    println!("cargo:rerun-if-env-changed=KAZAROV_GAMEPLAY_ROOT");
    println!("cargo:rerun-if-env-changed=KAZAROV_GAMEPLAY_ARCHIVE");
    // Cargo treats a missing rerun-if-changed path as changed, which would rebuild on every run.
    if root.exists() {
        println!("cargo:rerun-if-changed={}", root.display());
    }
    if configured_root.is_none() && !root.join("character_clips.json").is_file() {
        let out = PathBuf::from(std::env::var_os("OUT_DIR").expect("OUT_DIR"));
        fs::write(
            out.join("tracked_configs.rs"),
            "pub(crate) static TRACKED_GAMEPLAY_PATHS: &[&str] = &[];\n\
             pub(crate) static TRACKED_CONFIG_PATHS: &[&str] = &[];\n\
             pub(crate) static GAMEPLAY_DOCUMENTS: &[(&str, [u8; 32])] = &[];\n\
             pub(crate) const TRACKED_CONFIG_FINGERPRINT: &str = \"0000000000000000\";\n",
        )
        .expect("write empty gameplay index");
        publish_archive(&out, &[]);
        return;
    }
    assert!(
        root.is_dir(),
        "tracked gameplay directory missing: {}",
        root.display()
    );
    let mut files = Vec::new();
    collect(&root, &root, &mut files);
    files.sort();
    let mut generated = String::from("pub(crate) static TRACKED_GAMEPLAY_PATHS: &[&str] = &[\n");
    let mut gameplay_paths = Vec::new();
    let mut documents = Vec::new();
    let mut fingerprint: u64 = 0xcbf29ce484222325;
    for file in files {
        let relative = file
            .strip_prefix(&root)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        assert!(
            relative.ends_with(".json"),
            "non-JSON gameplay file: {relative}"
        );
        println!("cargo:rerun-if-changed={}", file.display());
        let bytes = fs::read(&file).expect("read tracked config");
        let json: serde_json::Value =
            serde_json::from_slice(&bytes).expect("parse tracked config JSON");
        let standalone = relative.ends_with(".cfg.json") || relative.ends_with(".map.json");
        assert!(
            json["schema_version"] == if standalone { 2 } else { 1 },
            "unsupported gameplay schema: {relative}"
        );
        assert!(
            json["kind"].is_string() && (!standalone || json.get("commands").is_none()),
            "invalid gameplay document: {relative}"
        );
        for byte in relative.bytes().chain([0]).chain(bytes.iter().copied()) {
            fingerprint ^= u64::from(byte);
            fingerprint = fingerprint.wrapping_mul(0x100000001b3);
        }
        let path = relative.to_ascii_lowercase();
        let compact = serde_json::to_vec(&json).expect("serialize tracked config JSON");
        documents.push((format!("gameplay/{path}"), compact));
        gameplay_paths.push(path);
    }
    gameplay_paths.sort();
    let before = gameplay_paths.len();
    gameplay_paths.dedup();
    assert_eq!(
        gameplay_paths.len(),
        before,
        "gameplay paths collide ignoring case"
    );
    for path in gameplay_paths {
        generated.push_str(&format!("    {path:?},\n"));
    }
    generated.push_str("];\n");
    documents.sort_by(|a, b| a.0.cmp(&b.0));
    generated.push_str("pub(crate) static GAMEPLAY_DOCUMENTS: &[(&str, [u8; 32])] = &[\n");
    for (path, bytes) in &documents {
        let digest: [u8; 32] = Sha256::digest(bytes).into();
        generated.push_str(&format!("    ({path:?}, {digest:?}),\n"));
    }
    generated.push_str("];\n");
    generated.push_str("pub(crate) static TRACKED_CONFIG_PATHS: &[&str] = &[\n");
    let mut standalone_paths = Vec::new();
    collect(&root, &root, &mut standalone_paths);
    standalone_paths.sort();
    let mut standalone_names = Vec::new();
    for file in standalone_paths {
        let relative = file
            .strip_prefix(&root)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        if relative.ends_with(".cfg.json") || relative.ends_with(".map.json") {
            standalone_names.push(relative.to_ascii_lowercase());
        }
    }
    standalone_names.sort();
    standalone_names.dedup();
    for path in standalone_names {
        generated.push_str(&format!("    {path:?},\n"));
    }
    generated.push_str("];\n");
    generated.push_str(&format!(
        "pub(crate) const TRACKED_CONFIG_FINGERPRINT: &str = \"{fingerprint:016x}\";\n"
    ));
    let out = PathBuf::from(std::env::var_os("OUT_DIR").expect("OUT_DIR"));
    write_generated(&out.join("tracked_configs.rs"), generated.as_bytes());
    let plan: serde_json::Value = serde_json::from_slice(
        &fs::read(root.join("character_clips.json")).expect("read character clip plan"),
    )
    .expect("parse character clip plan");
    assert!(
        plan["schema_version"] == 1
            && plan["kind"] == "character_clips"
            && plan["characters"].is_object(),
        "invalid character clip plan"
    );
    publish_archive(&out, &documents);
}
