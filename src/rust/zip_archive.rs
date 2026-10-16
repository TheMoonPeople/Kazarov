//! Deterministic ZIP64 asset archives holding stored or Zstandard entries.
//!
//! The format is specified in `docs/PORT_ARCHIVE_FORMAT.md` of the game repository.
use super::{CoreError, CoreResult};
use rayon::prelude::*;
use sha2::{Digest, Sha256};
use std::borrow::Cow;
use std::cell::RefCell;
use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{BufWriter, Read, Write};
use std::path::{Path, PathBuf};

/// Alignment of every entry's data within the archive.
pub const ALIGNMENT: u64 = 64;
/// Zstandard level used for compressed entries.
pub const ZSTD_LEVEL: i32 = 9;
/// Uncompressed entry stored as-is.
pub const METHOD_STORED: u16 = 0;
/// Zstandard-compressed entry.
pub const METHOD_ZSTD: u16 = 93;

const LOCAL_SIGNATURE: u32 = 0x0403_4b50;
const CENTRAL_SIGNATURE: u32 = 0x0201_4b50;
const END_SIGNATURE: u32 = 0x0605_4b50;
const ZIP64_END_SIGNATURE: u32 = 0x0606_4b50;
const ZIP64_LOCATOR_SIGNATURE: u32 = 0x0706_4b50;
const ZIP64_EXTRA: u16 = 0x0001;
/// Extensible data field used only to pad local headers to the data alignment.
const PADDING_EXTRA: u16 = 0xd935;
const FLAG_ENCRYPTED: u16 = 1;
const FLAG_STRONG_ENCRYPTION: u16 = 1 << 6;
const FLAG_UTF8: u16 = 1 << 11;
const FLAG_MASKED_HEADER: u16 = 1 << 13;
/// 1980-01-01 00:00:00 in MS-DOS format.
const DOS_TIME: u16 = 0;
const DOS_DATE: u16 = (1 << 5) | 1;
const VERSION_ZIP64: u16 = 45;
const VERSION_ZSTD: u16 = 63;
const LOCAL_HEADER: usize = 30;
const CENTRAL_HEADER: usize = 46;
const END_RECORD: usize = 22;
const ZIP64_END_RECORD: usize = 56;
const ZIP64_LOCATOR: usize = 20;
const MAX_COMMENT: usize = 0xffff;
/// Compressed entries are batched until their inputs reach this many bytes.
const BATCH_BYTES: u64 = 512 << 20;
const BATCH_ENTRIES: usize = 4096;
const COPY_CHUNK: usize = 4 << 20;

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}

fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
}

fn u64_at(bytes: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(bytes[at..at + 8].try_into().unwrap())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// Validates an entry name: relative, '/'-separated, without empty, '.' or '..' components.
pub fn validate_name(name: &str) -> CoreResult<()> {
    let valid = !name.is_empty()
        && name.len() <= 0xffff
        && !name
            .chars()
            .any(|c| matches!(c, '\\' | ':' | '\0') || c.is_control())
        && name
            .split('/')
            .all(|part| !part.is_empty() && part != "." && part != "..");
    if valid {
        Ok(())
    } else {
        Err(fail(format!("invalid archive entry name: {name:?}")))
    }
}

/// Returns whether an entry of this name is stored rather than Zstandard-compressed.
pub fn stored_by_name(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    lower.ends_with(".png") || lower.ends_with(".mp4")
}

/// Rejects names that are equal ignoring ASCII case.
fn check_unique<'a>(names: impl Iterator<Item = &'a str>) -> CoreResult<()> {
    let mut seen = HashSet::new();
    for name in names {
        if !seen.insert(name.to_ascii_lowercase()) {
            return Err(fail(format!(
                "archive entry names collide ignoring case: {name}"
            )));
        }
    }
    Ok(())
}

/// Content identity of one entry, independent of its compression.
pub struct ContentRecord<'a> {
    pub name: &'a str,
    pub size: u64,
    pub sha256: [u8; 32],
}

/// Digests entry names and uncompressed contents in name order.
pub fn content_digest(records: &mut [ContentRecord<'_>]) -> String {
    records.sort_by(|a, b| a.name.cmp(b.name));
    let mut hash = Sha256::new();
    hash.update(b"kazarov-archive-content-v1\0");
    for record in records.iter() {
        hash.update((record.name.len() as u64).to_le_bytes());
        hash.update(record.name.as_bytes());
        hash.update(record.size.to_le_bytes());
        hash.update(record.sha256);
    }
    hex(&hash.finalize())
}

/// One parsed central directory entry.
#[derive(Debug, Clone)]
struct Entry {
    name: String,
    method: u16,
    crc: u32,
    compressed: u64,
    size: u64,
    data: u64,
}

enum Backing {
    Mapped(memmap2::Mmap),
    File(File),
}

/// Immutable, thread-safe view of a ZIP archive.
pub struct Archive {
    path: PathBuf,
    backing: Backing,
    length: u64,
    entries: Vec<Entry>,
}

#[cfg(windows)]
fn read_at(file: &File, buffer: &mut [u8], offset: u64) -> std::io::Result<usize> {
    std::os::windows::fs::FileExt::seek_read(file, buffer, offset)
}

#[cfg(unix)]
fn read_at(file: &File, buffer: &mut [u8], offset: u64) -> std::io::Result<usize> {
    std::os::unix::fs::FileExt::read_at(file, buffer, offset)
}

fn read_exact_at(file: &File, mut buffer: &mut [u8], mut offset: u64) -> std::io::Result<()> {
    while !buffer.is_empty() {
        match read_at(file, buffer, offset) {
            Ok(0) => return Err(std::io::ErrorKind::UnexpectedEof.into()),
            Ok(count) => {
                buffer = &mut buffer[count..];
                offset += count as u64;
            }
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

thread_local! {
    static DECOMPRESSOR: RefCell<Option<zstd::bulk::Decompressor<'static>>> =
        const { RefCell::new(None) };
}

/// Decompresses one complete Zstandard payload into exactly `out`.
fn decompress_exact(source: &[u8], out: &mut [u8]) -> CoreResult<()> {
    let written = DECOMPRESSOR.with(|cell| -> CoreResult<usize> {
        let mut slot = cell.borrow_mut();
        if slot.is_none() {
            *slot = Some(zstd::bulk::Decompressor::new()?);
        }
        Ok(slot.as_mut().unwrap().decompress_to_buffer(source, out)?)
    })?;
    if written != out.len() {
        return Err(fail("decompressed size mismatch"));
    }
    Ok(())
}

impl Archive {
    /// Opens and validates an archive's central directory, preferring a memory map.
    pub fn open(path: &Path) -> CoreResult<Archive> {
        let context = |error: CoreError| fail(format!("{}: {error}", path.display()));
        let file = File::open(path).map_err(|e| context(e.into()))?;
        let length = file.metadata().map_err(|e| context(e.into()))?.len();
        // SAFETY: archives are treated as immutable while open; concurrent truncation
        // by another process is outside the supported contract.
        let backing = match unsafe { memmap2::Mmap::map(&file) } {
            Ok(map) if map.len() as u64 == length => Backing::Mapped(map),
            _ => Backing::File(file),
        };
        let mut archive = Archive {
            path: path.to_path_buf(),
            backing,
            length,
            entries: Vec::new(),
        };
        archive.entries = archive.parse().map_err(context)?;
        Ok(archive)
    }

    /// Returns `length` bytes at `offset`, borrowed from the map when possible.
    fn bytes(&self, offset: u64, length: u64) -> CoreResult<Cow<'_, [u8]>> {
        let end = offset
            .checked_add(length)
            .filter(|end| *end <= self.length)
            .ok_or_else(|| fail("archive range outside file"))?;
        match &self.backing {
            Backing::Mapped(map) => Ok(Cow::Borrowed(&map[offset as usize..end as usize])),
            Backing::File(file) => {
                let mut buffer = vec![0; length as usize];
                read_exact_at(file, &mut buffer, offset)?;
                Ok(Cow::Owned(buffer))
            }
        }
    }

    fn parse(&self) -> CoreResult<Vec<Entry>> {
        if self.length < END_RECORD as u64 {
            return Err(fail("not a ZIP archive"));
        }
        let tail_length = self.length.min((END_RECORD + MAX_COMMENT) as u64);
        let tail_start = self.length - tail_length;
        let tail = self.bytes(tail_start, tail_length)?;
        let end = (0..=tail.len() - END_RECORD)
            .rev()
            .find(|&at| {
                u32_at(&tail, at) == END_SIGNATURE
                    && at + END_RECORD + u16_at(&tail, at + 20) as usize == tail.len()
            })
            .ok_or_else(|| fail("ZIP end of central directory not found"))?;
        let record = &tail[end..end + END_RECORD];
        let end_offset = tail_start + end as u64;
        let mut disk = u16_at(record, 4) as u32;
        let mut directory_disk = u16_at(record, 6) as u32;
        let mut disk_entries = u16_at(record, 8) as u64;
        let mut count = u16_at(record, 10) as u64;
        let mut directory_size = u32_at(record, 12) as u64;
        let mut directory_offset = u32_at(record, 16) as u64;
        let mut directory_limit = end_offset;
        if end_offset >= ZIP64_LOCATOR as u64 {
            let locator = self.bytes(end_offset - ZIP64_LOCATOR as u64, ZIP64_LOCATOR as u64)?;
            if u32_at(&locator, 0) == ZIP64_LOCATOR_SIGNATURE {
                if u32_at(&locator, 4) != 0 || u32_at(&locator, 16) != 1 {
                    return Err(fail("multi-disk ZIP archives are unsupported"));
                }
                let zip64_offset = u64_at(&locator, 8);
                if zip64_offset + ZIP64_END_RECORD as u64 > end_offset - ZIP64_LOCATOR as u64 {
                    return Err(fail("invalid ZIP64 end record offset"));
                }
                let zip64 = self.bytes(zip64_offset, ZIP64_END_RECORD as u64)?;
                if u32_at(&zip64, 0) != ZIP64_END_SIGNATURE {
                    return Err(fail("ZIP64 end record not found"));
                }
                disk = u32_at(&zip64, 16);
                directory_disk = u32_at(&zip64, 20);
                disk_entries = u64_at(&zip64, 24);
                count = u64_at(&zip64, 32);
                directory_size = u64_at(&zip64, 40);
                directory_offset = u64_at(&zip64, 48);
                directory_limit = zip64_offset;
            } else if count == 0xffff
                || directory_size == 0xffff_ffff
                || directory_offset == 0xffff_ffff
            {
                return Err(fail("ZIP64 locator missing"));
            }
        }
        if disk != 0 || directory_disk != 0 || disk_entries != count {
            return Err(fail("multi-disk ZIP archives are unsupported"));
        }
        if directory_offset
            .checked_add(directory_size)
            .is_none_or(|end| end > directory_limit)
        {
            return Err(fail("central directory outside archive"));
        }
        // Each central header is at least 46 bytes, which bounds the allocation.
        if count > directory_size / CENTRAL_HEADER as u64 {
            return Err(fail("central directory entry count too large"));
        }
        let directory = self.bytes(directory_offset, directory_size)?;
        let mut entries = Vec::with_capacity(count as usize);
        let mut at = 0usize;
        for _ in 0..count {
            if at + CENTRAL_HEADER > directory.len() || u32_at(&directory, at) != CENTRAL_SIGNATURE
            {
                return Err(fail("invalid central directory header"));
            }
            let header = &directory[at..at + CENTRAL_HEADER];
            let flags = u16_at(header, 8);
            let method = u16_at(header, 10);
            let crc = u32_at(header, 16);
            let mut compressed = u32_at(header, 20) as u64;
            let mut size = u32_at(header, 24) as u64;
            let name_length = u16_at(header, 28) as usize;
            let extra_length = u16_at(header, 30) as usize;
            let comment_length = u16_at(header, 32) as usize;
            let mut start_disk = u16_at(header, 34) as u32;
            let mut local = u32_at(header, 42) as u64;
            let variable = at + CENTRAL_HEADER;
            let next = variable + name_length + extra_length + comment_length;
            if next > directory.len() {
                return Err(fail("truncated central directory"));
            }
            let raw_name = &directory[variable..variable + name_length];
            let name = std::str::from_utf8(raw_name)
                .map_err(|_| fail("archive entry name is not UTF-8"))?
                .to_owned();
            validate_name(&name)?;
            if flags & (FLAG_ENCRYPTED | FLAG_STRONG_ENCRYPTION | FLAG_MASKED_HEADER) != 0 {
                return Err(fail(format!("encrypted archive entry: {name}")));
            }
            if method != METHOD_STORED && method != METHOD_ZSTD {
                return Err(fail(format!(
                    "unsupported compression method {method}: {name}"
                )));
            }
            let mut extra =
                &directory[variable + name_length..variable + name_length + extra_length];
            while extra.len() >= 4 {
                let id = u16_at(extra, 0);
                let length = u16_at(extra, 2) as usize;
                if 4 + length > extra.len() {
                    return Err(fail(format!("truncated extra field: {name}")));
                }
                if id == ZIP64_EXTRA {
                    let mut field = &extra[4..4 + length];
                    let mut take = |value: &mut u64| -> CoreResult<()> {
                        if field.len() < 8 {
                            return Err(fail(format!("truncated ZIP64 field: {name}")));
                        }
                        *value = u64_at(field, 0);
                        field = &field[8..];
                        Ok(())
                    };
                    if size == 0xffff_ffff {
                        take(&mut size)?;
                    }
                    if compressed == 0xffff_ffff {
                        take(&mut compressed)?;
                    }
                    if local == 0xffff_ffff {
                        take(&mut local)?;
                    }
                    if start_disk == 0xffff {
                        if field.len() < 4 {
                            return Err(fail(format!("truncated ZIP64 field: {name}")));
                        }
                        start_disk = u32_at(field, 0);
                    }
                }
                extra = &extra[4 + length..];
            }
            if start_disk != 0 {
                return Err(fail("multi-disk ZIP archives are unsupported"));
            }
            if method == METHOD_STORED && compressed != size {
                return Err(fail(format!("stored entry size mismatch: {name}")));
            }
            let local_header = self.bytes(local, LOCAL_HEADER as u64)?;
            if u32_at(&local_header, 0) != LOCAL_SIGNATURE || u16_at(&local_header, 8) != method {
                return Err(fail(format!("invalid local header: {name}")));
            }
            let local_name_length = u16_at(&local_header, 26) as u64;
            let local_extra_length = u16_at(&local_header, 28) as u64;
            if self
                .bytes(local + LOCAL_HEADER as u64, local_name_length)?
                .as_ref()
                != raw_name
            {
                return Err(fail(format!("local header name mismatch: {name}")));
            }
            let data = local + LOCAL_HEADER as u64 + local_name_length + local_extra_length;
            if data
                .checked_add(compressed)
                .is_none_or(|end| end > directory_offset)
            {
                return Err(fail(format!("entry data outside archive: {name}")));
            }
            entries.push(Entry {
                name,
                method,
                crc,
                compressed,
                size,
                data,
            });
            at = next;
        }
        check_unique(entries.iter().map(|entry| entry.name.as_str()))?;
        Ok(entries)
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    fn entry(&self, index: usize) -> CoreResult<&Entry> {
        self.entries
            .get(index)
            .ok_or_else(|| fail(format!("archive entry index out of range: {index}")))
    }

    /// Name of an entry; panics when the index is out of range.
    pub fn name(&self, index: usize) -> &str {
        &self.entries[index].name
    }

    pub fn size(&self, index: usize) -> u64 {
        self.entries[index].size
    }

    pub fn crc(&self, index: usize) -> u32 {
        self.entries[index].crc
    }

    pub fn stored(&self, index: usize) -> bool {
        self.entries[index].method == METHOD_STORED
    }

    pub fn data_offset(&self, index: usize) -> u64 {
        self.entries[index].data
    }

    /// Reads one entry's uncompressed bytes into `out` and verifies its CRC-32.
    pub fn read_into(&self, index: usize, out: &mut [u8]) -> CoreResult<()> {
        let entry = self.entry(index)?;
        if out.len() as u64 != entry.size {
            return Err(fail(format!(
                "buffer size {} does not match entry size {}: {}",
                out.len(),
                entry.size,
                entry.name
            )));
        }
        let source = self.bytes(entry.data, entry.compressed)?;
        if entry.method == METHOD_STORED {
            out.copy_from_slice(&source);
        } else {
            decompress_exact(&source, out)
                .map_err(|e| fail(format!("{}: {}: {e}", self.path.display(), entry.name)))?;
        }
        if crc32fast::hash(out) != entry.crc {
            return Err(fail(format!(
                "{}: CRC mismatch: {}",
                self.path.display(),
                entry.name
            )));
        }
        Ok(())
    }

    /// Reads one entry's uncompressed bytes and verifies its CRC-32.
    pub fn read(&self, index: usize) -> CoreResult<Vec<u8>> {
        let mut out = vec![0; self.entry(index)?.size as usize];
        self.read_into(index, &mut out)?;
        Ok(out)
    }

    /// Calls `visit` with each entry's verified bytes in parallel, returning content records.
    pub fn verify_entries(
        &self,
        visit: &(dyn Fn(&str, &[u8]) -> CoreResult<()> + Sync),
    ) -> CoreResult<Vec<ContentRecord<'_>>> {
        (0..self.entries.len())
            .into_par_iter()
            .map(|index| {
                let entry = &self.entries[index];
                let bytes = if entry.method == METHOD_STORED {
                    let bytes = self.bytes(entry.data, entry.size)?;
                    if crc32fast::hash(&bytes) != entry.crc {
                        return Err(fail(format!("CRC mismatch: {}", entry.name)));
                    }
                    bytes
                } else {
                    Cow::Owned(self.read(index)?)
                };
                visit(&entry.name, &bytes)?;
                Ok(ContentRecord {
                    name: &entry.name,
                    size: entry.size,
                    sha256: Sha256::digest(&bytes).into(),
                })
            })
            .collect()
    }
}

/// Summary of a written archive.
#[derive(Debug, Clone)]
pub struct Written {
    pub size: u64,
    pub entries: u64,
    pub content_sha256: String,
}

/// One file to place in an archive.
pub struct Input {
    pub name: String,
    pub source: PathBuf,
}

struct Prepared {
    method: u16,
    crc: u32,
    size: u64,
    sha256: [u8; 32],
    /// Compressed bytes; stored entries are copied from the source while writing.
    payload: Option<Vec<u8>>,
}

fn prepare(input: &Input) -> CoreResult<Prepared> {
    let context = |error: std::io::Error| fail(format!("{}: {error}", input.source.display()));
    if stored_by_name(&input.name) {
        let mut file = File::open(&input.source).map_err(context)?;
        let mut crc = crc32fast::Hasher::new();
        let mut sha = Sha256::new();
        let mut size = 0u64;
        let mut buffer = vec![0; COPY_CHUNK];
        loop {
            let count = file.read(&mut buffer).map_err(context)?;
            if count == 0 {
                break;
            }
            crc.update(&buffer[..count]);
            sha.update(&buffer[..count]);
            size += count as u64;
        }
        return Ok(Prepared {
            method: METHOD_STORED,
            crc: crc.finalize(),
            size,
            sha256: sha.finalize().into(),
            payload: None,
        });
    }
    let bytes = fs::read(&input.source).map_err(context)?;
    let payload = zstd::bulk::compress(&bytes, ZSTD_LEVEL).map_err(context)?;
    Ok(Prepared {
        method: METHOD_ZSTD,
        crc: crc32fast::hash(&bytes),
        size: bytes.len() as u64,
        sha256: Sha256::digest(&bytes).into(),
        payload: Some(payload),
    })
}

struct CentralEntry {
    name: String,
    method: u16,
    crc: u32,
    compressed: u64,
    size: u64,
    local: u64,
}

fn version_needed(method: u16) -> u16 {
    if method == METHOD_ZSTD {
        VERSION_ZSTD
    } else {
        VERSION_ZIP64
    }
}

struct CountingWriter<W: Write> {
    inner: W,
    position: u64,
}

impl<W: Write> CountingWriter<W> {
    fn put(&mut self, bytes: &[u8]) -> CoreResult<()> {
        self.inner.write_all(bytes)?;
        self.position += bytes.len() as u64;
        Ok(())
    }
}

fn local_header(name: &str, method: u16, crc: u32, compressed: u64, size: u64, at: u64) -> Vec<u8> {
    let fixed = LOCAL_HEADER + name.len() + 4 + 16;
    let misalignment = (at + fixed as u64) % ALIGNMENT;
    let mut padding = if misalignment == 0 {
        0
    } else {
        (ALIGNMENT - misalignment) as usize
    };
    if padding != 0 && padding < 4 {
        padding += ALIGNMENT as usize;
    }
    let extra_length = 20 + padding;
    let mut header = Vec::with_capacity(fixed + padding);
    header.extend(LOCAL_SIGNATURE.to_le_bytes());
    header.extend(version_needed(method).to_le_bytes());
    header.extend(FLAG_UTF8.to_le_bytes());
    header.extend(method.to_le_bytes());
    header.extend(DOS_TIME.to_le_bytes());
    header.extend(DOS_DATE.to_le_bytes());
    header.extend(crc.to_le_bytes());
    header.extend(0xffff_ffffu32.to_le_bytes());
    header.extend(0xffff_ffffu32.to_le_bytes());
    header.extend((name.len() as u16).to_le_bytes());
    header.extend((extra_length as u16).to_le_bytes());
    header.extend(name.as_bytes());
    header.extend(ZIP64_EXTRA.to_le_bytes());
    header.extend(16u16.to_le_bytes());
    header.extend(size.to_le_bytes());
    header.extend(compressed.to_le_bytes());
    if padding != 0 {
        header.extend(PADDING_EXTRA.to_le_bytes());
        header.extend(((padding - 4) as u16).to_le_bytes());
        header.resize(header.len() + padding - 4, 0);
    }
    debug_assert_eq!((at + header.len() as u64) % ALIGNMENT, 0);
    header
}

fn central_header(entry: &CentralEntry) -> Vec<u8> {
    let mut header = Vec::with_capacity(CENTRAL_HEADER + entry.name.len() + 28);
    header.extend(CENTRAL_SIGNATURE.to_le_bytes());
    header.extend(VERSION_ZSTD.to_le_bytes());
    header.extend(version_needed(entry.method).to_le_bytes());
    header.extend(FLAG_UTF8.to_le_bytes());
    header.extend(entry.method.to_le_bytes());
    header.extend(DOS_TIME.to_le_bytes());
    header.extend(DOS_DATE.to_le_bytes());
    header.extend(entry.crc.to_le_bytes());
    header.extend(0xffff_ffffu32.to_le_bytes());
    header.extend(0xffff_ffffu32.to_le_bytes());
    header.extend((entry.name.len() as u16).to_le_bytes());
    header.extend(28u16.to_le_bytes());
    header.extend(0u16.to_le_bytes());
    header.extend(0u16.to_le_bytes());
    header.extend(0u16.to_le_bytes());
    header.extend(0u32.to_le_bytes());
    header.extend(0xffff_ffffu32.to_le_bytes());
    header.extend(entry.name.as_bytes());
    header.extend(ZIP64_EXTRA.to_le_bytes());
    header.extend(24u16.to_le_bytes());
    header.extend(entry.size.to_le_bytes());
    header.extend(entry.compressed.to_le_bytes());
    header.extend(entry.local.to_le_bytes());
    header
}

fn end_records(count: u64, directory_offset: u64, directory_size: u64) -> Vec<u8> {
    let zip64_offset = directory_offset + directory_size;
    let mut tail = Vec::with_capacity(ZIP64_END_RECORD + ZIP64_LOCATOR + END_RECORD);
    tail.extend(ZIP64_END_SIGNATURE.to_le_bytes());
    tail.extend(((ZIP64_END_RECORD - 12) as u64).to_le_bytes());
    tail.extend(VERSION_ZSTD.to_le_bytes());
    tail.extend(VERSION_ZIP64.to_le_bytes());
    tail.extend(0u32.to_le_bytes());
    tail.extend(0u32.to_le_bytes());
    tail.extend(count.to_le_bytes());
    tail.extend(count.to_le_bytes());
    tail.extend(directory_size.to_le_bytes());
    tail.extend(directory_offset.to_le_bytes());
    tail.extend(ZIP64_LOCATOR_SIGNATURE.to_le_bytes());
    tail.extend(0u32.to_le_bytes());
    tail.extend(zip64_offset.to_le_bytes());
    tail.extend(1u32.to_le_bytes());
    tail.extend(END_SIGNATURE.to_le_bytes());
    tail.extend(0u16.to_le_bytes());
    tail.extend(0u16.to_le_bytes());
    tail.extend(0xffffu16.to_le_bytes());
    tail.extend(0xffffu16.to_le_bytes());
    tail.extend(0xffff_ffffu32.to_le_bytes());
    tail.extend(0xffff_ffffu32.to_le_bytes());
    tail.extend(0u16.to_le_bytes());
    tail
}

/// Copies a stored source and confirms it still has the prepared size and CRC-32.
fn copy_stored<W: Write>(
    input: &Input,
    prepared: &Prepared,
    out: &mut CountingWriter<W>,
) -> CoreResult<()> {
    let mut file = File::open(&input.source)?;
    let mut crc = crc32fast::Hasher::new();
    let mut size = 0u64;
    let mut buffer = vec![0; COPY_CHUNK];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        crc.update(&buffer[..count]);
        size += count as u64;
        out.put(&buffer[..count])?;
    }
    if size != prepared.size || crc.finalize() != prepared.crc {
        return Err(fail(format!(
            "source changed while packing: {}",
            input.source.display()
        )));
    }
    Ok(())
}

/// Writes a deterministic archive of `inputs`, optionally deleting each source once written.
///
/// The archive is written beside `output` and renamed over it only when complete.
pub fn write(
    mut inputs: Vec<Input>,
    output: &Path,
    remove_sources: bool,
    progress: &(dyn Fn(u64, u64) + Sync),
) -> CoreResult<Written> {
    for input in &inputs {
        validate_name(&input.name)?;
    }
    inputs.sort_by(|a, b| a.name.cmp(&b.name));
    check_unique(inputs.iter().map(|input| input.name.as_str()))?;
    let lengths = inputs
        .iter()
        .map(|input| Ok(fs::metadata(&input.source)?.len()))
        .collect::<CoreResult<Vec<u64>>>()?;
    let total: u64 = lengths.iter().sum();
    let mut partial_name = output
        .file_name()
        .ok_or_else(|| fail("archive output has no file name"))?
        .to_os_string();
    partial_name.push(".partial");
    let partial = output.with_file_name(partial_name);
    if partial.exists() {
        fs::remove_file(&partial)?;
    }
    let file = File::create(&partial)?;
    let mut out = CountingWriter {
        inner: BufWriter::with_capacity(COPY_CHUNK, file),
        position: 0,
    };
    let mut central = Vec::with_capacity(inputs.len());
    let mut hash = Sha256::new();
    hash.update(b"kazarov-archive-content-v1\0");
    let mut done = 0u64;
    let mut start = 0;
    while start < inputs.len() {
        let mut end = start;
        let mut batch_bytes = 0u64;
        while end < inputs.len()
            && end - start < BATCH_ENTRIES
            && (end == start || batch_bytes < BATCH_BYTES)
        {
            if !stored_by_name(&inputs[end].name) {
                batch_bytes += lengths[end];
            }
            end += 1;
        }
        let prepared = inputs[start..end]
            .par_iter()
            .map(prepare)
            .collect::<CoreResult<Vec<_>>>()?;
        for (input, prepared) in inputs[start..end].iter().zip(prepared) {
            let compressed = prepared
                .payload
                .as_ref()
                .map_or(prepared.size, |payload| payload.len() as u64);
            let local = out.position;
            out.put(&local_header(
                &input.name,
                prepared.method,
                prepared.crc,
                compressed,
                prepared.size,
                local,
            ))?;
            match &prepared.payload {
                Some(payload) => out.put(payload)?,
                None => copy_stored(input, &prepared, &mut out)?,
            }
            hash.update((input.name.len() as u64).to_le_bytes());
            hash.update(input.name.as_bytes());
            hash.update(prepared.size.to_le_bytes());
            hash.update(prepared.sha256);
            central.push(CentralEntry {
                name: input.name.clone(),
                method: prepared.method,
                crc: prepared.crc,
                compressed,
                size: prepared.size,
                local,
            });
            if remove_sources {
                fs::remove_file(&input.source)?;
            }
            done += prepared.size;
            progress(done, total);
        }
        start = end;
    }
    let directory_offset = out.position;
    for entry in &central {
        out.put(&central_header(entry))?;
    }
    let directory_size = out.position - directory_offset;
    out.put(&end_records(
        central.len() as u64,
        directory_offset,
        directory_size,
    ))?;
    let size = out.position;
    let file = out
        .inner
        .into_inner()
        .map_err(|error| CoreError(error.error().to_string()))?;
    file.sync_all()?;
    drop(file);
    fs::rename(&partial, output)?;
    Ok(Written {
        size,
        entries: central.len() as u64,
        content_sha256: hex(&hash.finalize()),
    })
}

/// Lists every regular file under `root` as archive inputs named by relative path.
pub fn directory_inputs(root: &Path) -> CoreResult<Vec<Input>> {
    fn walk(root: &Path, directory: &Path, out: &mut Vec<Input>) -> CoreResult<()> {
        for entry in fs::read_dir(directory)? {
            let path = entry?.path();
            let metadata = fs::symlink_metadata(&path)?;
            if metadata.file_type().is_symlink() {
                return Err(fail(format!("symlink unsupported: {}", path.display())));
            }
            if metadata.is_dir() {
                walk(root, &path, out)?;
            } else if metadata.is_file() {
                let relative = path.strip_prefix(root).unwrap();
                let name = relative
                    .to_str()
                    .ok_or_else(|| fail(format!("path is not Unicode: {}", path.display())))?
                    .replace('\\', "/");
                out.push(Input { name, source: path });
            }
        }
        Ok(())
    }
    let mut inputs = Vec::new();
    walk(root, root, &mut inputs)?;
    Ok(inputs)
}

/// Packs a directory tree into a deterministic archive.
pub fn pack_directory(source: &Path, output: &Path) -> CoreResult<Written> {
    if !source.is_dir() {
        return Err(fail(format!(
            "pack source is not a directory: {}",
            source.display()
        )));
    }
    let source = source.canonicalize()?;
    let parent = output
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    if parent.canonicalize()?.starts_with(&source) {
        return Err(fail(
            "the archive must be written outside the packed directory",
        ));
    }
    write(directory_inputs(&source)?, output, false, &|_, _| {})
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tree(root: &Path, files: &[(&str, &[u8])]) {
        for (name, bytes) in files {
            let path = root.join(name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, bytes).unwrap();
        }
    }

    fn sample() -> Vec<(&'static str, Vec<u8>)> {
        vec![
            ("a/texture.png", vec![0x89; 1000]),
            ("a/movie.MP4", (0..70_000u32).map(|i| i as u8).collect()),
            ("b/script.stb", b"script ".repeat(5000)),
            ("b/empty.bin", Vec::new()),
            ("z.json", b"{\"schema_version\":1}".to_vec()),
        ]
    }

    fn packed(temp: &Path) -> (PathBuf, Vec<(&'static str, Vec<u8>)>) {
        let source = temp.join("source");
        let files = sample();
        let borrowed: Vec<(&str, &[u8])> = files.iter().map(|(n, b)| (*n, b.as_slice())).collect();
        tree(&source, &borrowed);
        let output = temp.join("out.zip");
        pack_directory(&source, &output).unwrap();
        (output, files)
    }

    #[test]
    fn round_trip_is_aligned_deterministic_and_zip64() {
        let temp = tempfile::tempdir().unwrap();
        let (output, files) = packed(temp.path());
        let archive = Archive::open(&output).unwrap();
        assert_eq!(archive.len(), files.len());
        let names: Vec<&str> = (0..archive.len()).map(|i| archive.name(i)).collect();
        let mut sorted = names.clone();
        sorted.sort();
        assert_eq!(names, sorted);
        let bytes = fs::read(&output).unwrap();
        for index in 0..archive.len() {
            let name = archive.name(index);
            let expected = &files.iter().find(|(n, _)| *n == name).unwrap().1;
            assert_eq!(archive.size(index), expected.len() as u64);
            assert_eq!(archive.crc(index), crc32fast::hash(expected));
            assert_eq!(archive.read(index).unwrap(), *expected);
            assert_eq!(archive.data_offset(index) % ALIGNMENT, 0);
            assert_eq!(archive.stored(index), stored_by_name(name));
            if archive.stored(index) {
                let at = archive.data_offset(index) as usize;
                assert_eq!(&bytes[at..at + expected.len()], expected.as_slice());
            }
        }
        // ZIP64 records are always present and the classic fields are saturated.
        let end = bytes.len() - END_RECORD;
        assert_eq!(u32_at(&bytes, end), END_SIGNATURE);
        assert_eq!(u16_at(&bytes, end + 10), 0xffff);
        assert_eq!(u32_at(&bytes, end + 16), 0xffff_ffff);
        assert_eq!(u32_at(&bytes, end - ZIP64_LOCATOR), ZIP64_LOCATOR_SIGNATURE);
        assert_eq!(u32_at(&bytes, 0), LOCAL_SIGNATURE);
        assert_eq!(u32_at(&bytes, 18), 0xffff_ffff);
        assert_eq!(u16_at(&bytes, 10), DOS_TIME);
        assert_eq!(u16_at(&bytes, 12), DOS_DATE);
        // Rewriting the same tree produces identical bytes.
        let again = temp.path().join("again.zip");
        pack_directory(&temp.path().join("source"), &again).unwrap();
        assert_eq!(fs::read(again).unwrap(), bytes);
    }

    #[test]
    fn zstd_entries_compress_and_digest_matches_reader() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        let text = b"repetitive text ".repeat(10_000);
        tree(&source, &[("big.txt", &text), ("image.png", &text)]);
        let output = temp.path().join("out.zip");
        let written = pack_directory(&source, &output).unwrap();
        let archive = Archive::open(&output).unwrap();
        let text_index = (0..archive.len())
            .find(|&i| archive.name(i) == "big.txt")
            .unwrap();
        assert!(!archive.stored(text_index));
        let entry = &archive.entries[text_index];
        assert_eq!(entry.method, METHOD_ZSTD);
        assert!(entry.compressed < entry.size / 10);
        let mut records = archive.verify_entries(&|_, _| Ok(())).unwrap();
        assert_eq!(content_digest(&mut records), written.content_sha256);
        assert_eq!(written.size, fs::metadata(&output).unwrap().len());
    }

    #[test]
    fn reader_rejects_crc_mismatch() {
        let temp = tempfile::tempdir().unwrap();
        let (output, _) = packed(temp.path());
        let archive = Archive::open(&output).unwrap();
        let index = (0..archive.len())
            .find(|&i| archive.name(i) == "a/texture.png")
            .unwrap();
        let at = archive.data_offset(index) as usize;
        drop(archive);
        let mut bytes = fs::read(&output).unwrap();
        bytes[at] ^= 0xff;
        fs::write(&output, bytes).unwrap();
        let archive = Archive::open(&output).unwrap();
        let message = archive.read(index).unwrap_err().to_string();
        assert!(message.contains("CRC mismatch"), "{message}");
        assert!(archive.verify_entries(&|_, _| Ok(())).is_err());
    }

    #[test]
    fn writer_and_reader_reject_case_duplicates_and_bad_names() {
        let temp = tempfile::tempdir().unwrap();
        tree(temp.path(), &[("one.bin", b"1"), ("two.bin", b"2")]);
        let inputs = vec![
            Input {
                name: "Dir/File.bin".into(),
                source: temp.path().join("one.bin"),
            },
            Input {
                name: "dir/file.BIN".into(),
                source: temp.path().join("two.bin"),
            },
        ];
        let output = temp.path().join("out.zip");
        assert!(
            write(inputs, &output, false, &|_, _| {})
                .unwrap_err()
                .to_string()
                .contains("collide")
        );
        for name in ["", "/abs", "a\\b", "c:/x", "a/../b", "a//b", "./a", "a/"] {
            assert!(validate_name(name).is_err(), "{name}");
        }
        // A forged archive whose second name differs only by case is rejected on open.
        let forged = temp.path().join("forged.zip");
        write(
            vec![
                Input {
                    name: "dir/aaaa.bin".into(),
                    source: temp.path().join("one.bin"),
                },
                Input {
                    name: "dir/bbbb.bin".into(),
                    source: temp.path().join("two.bin"),
                },
            ],
            &forged,
            false,
            &|_, _| {},
        )
        .unwrap();
        let mut bytes = fs::read(&forged).unwrap();
        let mut patched = 0;
        let mut at = 0;
        while let Some(found) = bytes[at..]
            .windows(12)
            .position(|window| window == b"dir/bbbb.bin")
        {
            bytes[at + found..at + found + 12].copy_from_slice(b"DIR/AAAA.bin");
            at += found + 12;
            patched += 1;
        }
        assert_eq!(patched, 2);
        fs::write(&forged, bytes).unwrap();
        assert!(
            Archive::open(&forged)
                .err()
                .unwrap()
                .to_string()
                .contains("collide")
        );
    }

    #[test]
    fn writer_removes_sources_when_requested() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        tree(&source, &[("x.bin", b"x"), ("y/z.png", b"z")]);
        let output = temp.path().join("out.zip");
        write(
            directory_inputs(&source).unwrap(),
            &output,
            true,
            &|_, _| {},
        )
        .unwrap();
        assert!(!source.join("x.bin").exists());
        assert!(!source.join("y/z.png").exists());
        let archive = Archive::open(&output).unwrap();
        assert_eq!(archive.len(), 2);
    }

    #[test]
    fn reader_falls_back_to_positional_reads() {
        let temp = tempfile::tempdir().unwrap();
        let (output, files) = packed(temp.path());
        let mut archive = Archive::open(&output).unwrap();
        archive.backing = Backing::File(File::open(&output).unwrap());
        archive.entries = archive.parse().unwrap();
        for index in 0..archive.len() {
            let expected = &files
                .iter()
                .find(|(n, _)| *n == archive.name(index))
                .unwrap()
                .1;
            assert_eq!(archive.read(index).unwrap(), *expected);
        }
    }
}
