//! Deterministic classic ZIP of stored entries for readers that accept nothing newer.
use super::zip_archive::{Input, directory_inputs};
use super::{CoreError, CoreResult};
use rayon::prelude::*;
use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{BufWriter, Read, Write};
use std::path::{Path, PathBuf};

const LOCAL_HEADER: u32 = 0x0403_4b50;
const CENTRAL_HEADER: u32 = 0x0201_4b50;
const END_OF_DIRECTORY: u32 = 0x0605_4b50;
const UTF8_NAME: u16 = 1 << 11;
const DOS_DATE: u16 = 0x0021;
const MAX_ENTRY: u64 = i32::MAX as u64;
const COPY_BUFFER: usize = 1 << 20;

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}

/// What a finished archive holds.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct PackReport {
    pub files: u64,
    pub bytes: u64,
}

struct Entry {
    input: Input,
    size: u32,
    crc: u32,
    offset: u32,
}

fn safe_relative(name: &str) -> bool {
    !name.is_empty()
        && !name.ends_with('/')
        && name
            .split('/')
            .all(|part| !part.is_empty() && part != "." && part != ".." && !part.contains(':'))
}

fn crc_of(path: &Path) -> CoreResult<u32> {
    let mut file = File::open(path)?;
    let mut hasher = crc32fast::Hasher::new();
    let mut buffer = vec![0u8; COPY_BUFFER];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            return Ok(hasher.finalize());
        }
        hasher.update(&buffer[..count]);
    }
}

fn offset_of(position: u64) -> CoreResult<u32> {
    u32::try_from(position).map_err(|_| fail("archive exceeds the 4 GiB classic ZIP limit"))
}

fn write_entry(archive: &mut impl Write, entry: &Entry) -> CoreResult<()> {
    let name = entry.input.name.as_bytes();
    archive.write_all(&LOCAL_HEADER.to_le_bytes())?;
    for field in [20u16, UTF8_NAME, 0, 0, DOS_DATE] {
        archive.write_all(&field.to_le_bytes())?;
    }
    for field in [entry.crc, entry.size, entry.size] {
        archive.write_all(&field.to_le_bytes())?;
    }
    archive.write_all(&(name.len() as u16).to_le_bytes())?;
    archive.write_all(&0u16.to_le_bytes())?;
    archive.write_all(name)?;
    let mut input = File::open(&entry.input.source)?;
    let mut hasher = crc32fast::Hasher::new();
    let mut buffer = vec![0u8; COPY_BUFFER];
    let mut written = 0u64;
    loop {
        let count = input.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
        archive.write_all(&buffer[..count])?;
        written += count as u64;
    }
    if written != u64::from(entry.size) || hasher.finalize() != entry.crc {
        return Err(fail(format!(
            "{} changed while the archive was written",
            entry.input.source.display()
        )));
    }
    Ok(())
}

fn write_central(archive: &mut impl Write, entry: &Entry) -> CoreResult<()> {
    let name = entry.input.name.as_bytes();
    archive.write_all(&CENTRAL_HEADER.to_le_bytes())?;
    for field in [20u16, 20, UTF8_NAME, 0, 0, DOS_DATE] {
        archive.write_all(&field.to_le_bytes())?;
    }
    for field in [entry.crc, entry.size, entry.size] {
        archive.write_all(&field.to_le_bytes())?;
    }
    archive.write_all(&(name.len() as u16).to_le_bytes())?;
    for field in [0u16, 0, 0, 0] {
        archive.write_all(&field.to_le_bytes())?;
    }
    archive.write_all(&0u32.to_le_bytes())?;
    archive.write_all(&entry.offset.to_le_bytes())?;
    archive.write_all(name)?;
    Ok(())
}

fn entries_of(source: &Path) -> CoreResult<Vec<Entry>> {
    let mut entries = Vec::new();
    for input in directory_inputs(source)? {
        if !safe_relative(&input.name) || input.name.len() > 0xFFFF {
            return Err(fail(format!(
                "unsafe or too-long data path: {}",
                input.name
            )));
        }
        let size = fs::metadata(&input.source)?.len();
        if size > MAX_ENTRY {
            return Err(fail(format!(
                "{} exceeds the supported 2 GiB file limit",
                input.name
            )));
        }
        entries.push(Entry {
            input,
            size: size as u32,
            crc: 0,
            offset: 0,
        });
    }
    if entries.is_empty() {
        return Err(fail(format!("{} contains no files", source.display())));
    }
    if entries.len() >= 0xFFFF {
        return Err(fail(format!(
            "{} has too many files for classic ZIP",
            entries.len()
        )));
    }
    entries.sort_by(|left, right| left.input.name.as_bytes().cmp(right.input.name.as_bytes()));
    let mut folded = HashSet::new();
    for entry in &entries {
        if !folded.insert(entry.input.name.to_ascii_lowercase()) {
            return Err(fail(format!(
                "data paths differ only by ASCII case: {}",
                entry.input.name
            )));
        }
    }
    Ok(entries)
}

fn output_path(output: &Path) -> CoreResult<PathBuf> {
    let parent = match output.parent() {
        Some(parent) if !parent.as_os_str().is_empty() => parent.to_path_buf(),
        _ => std::env::current_dir()?,
    };
    fs::create_dir_all(&parent)?;
    let name = output
        .file_name()
        .ok_or_else(|| fail("archive output has no file name"))?;
    Ok(parent.canonicalize()?.join(name))
}

fn write_archive(temporary: &Path, entries: &mut [Entry]) -> CoreResult<()> {
    let mut archive = BufWriter::with_capacity(COPY_BUFFER, File::create(temporary)?);
    let mut position = 0u64;
    for entry in entries.iter_mut() {
        entry.offset = offset_of(position)?;
        write_entry(&mut archive, entry)?;
        position += 30 + entry.input.name.len() as u64 + u64::from(entry.size);
    }
    let central_offset = offset_of(position)?;
    let mut central = 0u64;
    for entry in entries.iter() {
        write_central(&mut archive, entry)?;
        central += 46 + entry.input.name.len() as u64;
    }
    offset_of(position + central)?;
    let count = entries.len() as u16;
    archive.write_all(&END_OF_DIRECTORY.to_le_bytes())?;
    for field in [0u16, 0, count, count] {
        archive.write_all(&field.to_le_bytes())?;
    }
    archive.write_all(&(central as u32).to_le_bytes())?;
    archive.write_all(&central_offset.to_le_bytes())?;
    archive.write_all(&0u16.to_le_bytes())?;
    archive.flush()?;
    archive
        .into_inner()
        .map_err(|error| fail(error.to_string()))?
        .sync_all()?;
    Ok(())
}

/// Packs the files under `source` into a classic ZIP of stored entries sorted by name.
///
/// Checksums are computed on `workers` threads; the archive appears at `output` only once
/// complete, and an existing `output` is never replaced.
pub fn pack_directory_stored(
    source: &Path,
    output: &Path,
    workers: usize,
) -> CoreResult<PackReport> {
    if !source.is_dir() {
        return Err(fail(format!(
            "{} is not a data directory",
            source.display()
        )));
    }
    let source_absolute = source.canonicalize()?;
    let output_absolute = output_path(output)?;
    if output_absolute.starts_with(&source_absolute) {
        return Err(fail(
            "archive output must be outside the source data directory",
        ));
    }
    if output_absolute.exists() {
        return Err(fail(format!(
            "{} already exists; choose a new output path",
            output.display()
        )));
    }
    let mut entries = entries_of(source)?;
    let pool = super::jobs::worker_pool(workers, "pack", 8 << 20)?;
    let crcs: Vec<u32> = pool.install(|| {
        entries
            .par_iter()
            .map(|entry| crc_of(&entry.input.source))
            .collect::<CoreResult<_>>()
    })?;
    for (entry, crc) in entries.iter_mut().zip(crcs) {
        entry.crc = crc;
    }
    let mut temporary = output_absolute.clone().into_os_string();
    temporary.push(".tmp");
    let temporary = PathBuf::from(temporary);
    if temporary.exists() {
        return Err(fail(format!(
            "temporary output {} already exists",
            temporary.display()
        )));
    }
    if let Err(error) = write_archive(&temporary, &mut entries) {
        let _ = fs::remove_file(&temporary);
        return Err(error);
    }
    fs::rename(&temporary, &output_absolute).inspect_err(|_| {
        let _ = fs::remove_file(&temporary);
    })?;
    Ok(PackReport {
        files: entries.len() as u64,
        bytes: entries.iter().map(|entry| u64::from(entry.size)).sum(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packs_sorted_stored_entries_readable_by_the_archive_reader() {
        let root = tempfile::tempdir().unwrap();
        let data = root.path().join("data");
        fs::create_dir_all(data.join("b")).unwrap();
        fs::write(data.join("b/two.bin"), [2u8; 70]).unwrap();
        fs::write(data.join("a.bin"), [1u8; 5]).unwrap();
        fs::write(data.join("empty"), []).unwrap();
        let archive = root.path().join("data.zip");
        let report = pack_directory_stored(&data, &archive, 2).unwrap();
        assert_eq!(
            report,
            PackReport {
                files: 3,
                bytes: 75
            }
        );
        let opened = super::super::zip_archive::Archive::open(&archive).unwrap();
        let names: Vec<_> = (0..opened.len())
            .map(|i| opened.name(i).to_owned())
            .collect();
        assert_eq!(names, ["a.bin", "b/two.bin", "empty"]);
        assert_eq!(opened.read(1).unwrap(), vec![2u8; 70]);
        assert!(pack_directory_stored(&data, &archive, 2).is_err());
    }

    #[test]
    fn rejects_output_inside_the_source() {
        let root = tempfile::tempdir().unwrap();
        fs::write(root.path().join("file"), [0u8]).unwrap();
        assert!(pack_directory_stored(root.path(), &root.path().join("out.zip"), 1).is_err());
    }
}
