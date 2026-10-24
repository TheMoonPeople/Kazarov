use super::progress::Phase;
use super::{CoreError, CoreResult};
use gpt_disk_io::{BlockIoAdapter, gpt_disk_types::BlockSize};
use iso9660::{DirectoryIterator, FileEntry, find_file, mount};
use rayon::prelude::*;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashSet};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::ops::Range;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

const SECTOR: u64 = 2048;
const CHUNK: usize = 4 << 20;

struct ReadOnlyCursor<'a>(std::io::Cursor<&'a [u8]>);

impl Read for ReadOnlyCursor<'_> {
    fn read(&mut self, output: &mut [u8]) -> std::io::Result<usize> {
        self.0.read(output)
    }
}

impl Seek for ReadOnlyCursor<'_> {
    fn seek(&mut self, position: SeekFrom) -> std::io::Result<u64> {
        self.0.seek(position)
    }
}

impl Write for ReadOnlyCursor<'_> {
    fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
        Err(std::io::ErrorKind::PermissionDenied.into())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl gpt_disk_io::ReadWriteSeek for ReadOnlyCursor<'_> {}

fn error(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}

fn le32(bytes: &[u8]) -> CoreResult<u32> {
    Ok(u32::from_le_bytes(
        bytes
            .get(..4)
            .ok_or_else(|| error("truncated u32"))?
            .try_into()
            .map_err(|_| error("truncated u32"))?,
    ))
}

fn ascii_fold(value: u8) -> u8 {
    value.to_ascii_lowercase()
}

fn folded(bytes: &[u8]) -> Vec<u8> {
    bytes
        .iter()
        .map(|&b| if b == b'\\' { b'/' } else { ascii_fold(b) })
        .skip_while(|&b| b == b'/')
        .collect()
}

fn disk_name(bytes: &[u8]) -> String {
    let mut name = String::with_capacity(bytes.len());
    for &byte in bytes {
        if byte == b'\\' {
            name.push('/');
        } else if byte >= 0x80 || byte == b'%' {
            name.push_str(&format!("%{byte:02X}"));
        } else {
            name.push(ascii_fold(byte) as char);
        }
    }
    name
}

fn safe_relative(name: &str) -> bool {
    !name.is_empty()
        && !name.ends_with('/')
        && name
            .split('/')
            .all(|part| !part.is_empty() && part != "." && part != ".." && !part.contains(':'))
}

fn nul_name(bytes: &[u8], offset: usize, label: &str) -> CoreResult<Vec<u8>> {
    let tail = bytes
        .get(offset..)
        .ok_or_else(|| error(format!("{label}: name offset outside file")))?;
    let end = tail
        .iter()
        .position(|&b| b == 0)
        .ok_or_else(|| error(format!("{label}: unterminated name")))?;
    Ok(tail[..end].to_vec())
}

#[derive(Clone, Debug)]
struct Extent {
    file: PathBuf,
    offset: u64,
    size: u64,
}

impl Extent {
    fn read(&self) -> CoreResult<Vec<u8>> {
        let count = usize::try_from(self.size).map_err(|_| error("extent too large"))?;
        if count > (512 << 20) {
            return Err(error("metadata extent exceeds 512 MiB"));
        }
        let mut file = File::open(&self.file)?;
        let end = self
            .offset
            .checked_add(self.size)
            .ok_or_else(|| error("extent overflow"))?;
        if end > file.metadata()?.len() {
            return Err(error(format!("{}: truncated extent", self.file.display())));
        }
        file.seek(SeekFrom::Start(self.offset))?;
        let mut bytes = vec![0; count];
        file.read_exact(&mut bytes)?;
        Ok(bytes)
    }
}

#[derive(Clone)]
struct NamedExtent {
    name: Vec<u8>,
    extent: Extent,
}

struct Archive {
    source: PathBuf,
    image: bool,
    dat: Extent,
    hd4: Extent,
    sound_dat: Extent,
    sound_hd3: Extent,
    movies: Vec<NamedExtent>,
    image_bytes: Option<SourceBytes>,
}

impl Archive {
    fn extent_bytes(&self, extent: &Extent) -> CoreResult<Vec<u8>> {
        if let Some(image) = &self.image_bytes {
            let start =
                usize::try_from(extent.offset).map_err(|_| error("source offset overflow"))?;
            let size = usize::try_from(extent.size).map_err(|_| error("source size overflow"))?;
            let end = start
                .checked_add(size)
                .ok_or_else(|| error("source range overflow"))?;
            if end > image.len() {
                return Err(error("truncated ISO extent"));
            }
            return Ok(image.slice(start..end)?.to_vec());
        }
        extent.read()
    }
}

fn image_extent(image: &Path, entry: &FileEntry) -> CoreResult<Extent> {
    if entry.file_unit_size != 0 || entry.interleave_gap != 0 || entry.flags.not_final {
        return Err(error(format!(
            "ISO extent for {} is interleaved or multi-extent",
            entry.name
        )));
    }
    let offset = u64::from(entry.extent_lba)
        .checked_mul(SECTOR)
        .ok_or_else(|| error("ISO extent offset overflow"))?;
    let extent = Extent {
        file: image.to_path_buf(),
        offset,
        size: entry.size,
    };
    let end = extent
        .offset
        .checked_add(extent.size)
        .ok_or_else(|| error("ISO extent overflow"))?;
    if end > fs::metadata(image)?.len() {
        return Err(error("truncated ISO extent"));
    }
    Ok(extent)
}

fn open_image_bytes(image: &Path, bytes: &[u8]) -> CoreResult<Archive> {
    let block_size =
        BlockSize::new(SECTOR as u32).ok_or_else(|| error("invalid ISO sector size"))?;
    let mut io = BlockIoAdapter::new(ReadOnlyCursor(std::io::Cursor::new(bytes)), block_size);
    let volume = mount(&mut io, 0)
        .map_err(|cause| error(format!("{}: ISO mount: {cause}", image.display())))?;
    if u64::from(volume.logical_block_size) != SECTOR {
        return Err(error(format!(
            "{}: unsupported ISO logical block size",
            image.display()
        )));
    }
    let mut required = |name: &str| -> CoreResult<Extent> {
        let entry = find_file(&mut io, &volume, name)
            .map_err(|cause| error(format!("{}: ISO {name}: {cause}", image.display())))?;
        if entry.flags.directory {
            return Err(error(format!("ISO {name} is a directory")));
        }
        image_extent(image, &entry)
    };
    let dat = required("/DATA.DAT")?;
    let hd4 = required("/DATA.HD4")?;
    let sound_dat = required("/SOUND.DAT")?;
    let sound_hd3 = required("/SOUND.HD3")?;
    let movies_dir = find_file(&mut io, &volume, "/MOVIE")
        .map_err(|cause| error(format!("{}: ISO /MOVIE: {cause}", image.display())))?;
    if !movies_dir.flags.directory {
        return Err(error("ISO /MOVIE is not a directory"));
    }
    // Subdirectories hold the manual's movies (/MOVIE/TUTO and /MOVIE/TUTO2).
    let mut movies = Vec::new();
    let mut visited = HashSet::new();
    let mut pending = vec![(String::new(), movies_dir.extent_lba, movies_dir.data_length)];
    while let Some((prefix, lba, length)) = pending.pop() {
        if !visited.insert(lba) {
            continue;
        }
        for entry in DirectoryIterator::new(&mut io, lba, length) {
            let entry = entry.map_err(|cause| {
                error(format!(
                    "{}: ISO /MOVIE/{prefix} entry: {cause}",
                    image.display()
                ))
            })?;
            if entry.name == "." || entry.name == ".." {
                continue;
            }
            let name = format!("{prefix}{}", entry.name);
            if entry.flags.directory {
                pending.push((format!("{name}/"), entry.extent_lba, entry.data_length));
            } else if name.to_ascii_lowercase().ends_with(".pss") {
                movies.push(NamedExtent {
                    name: name.into_bytes(),
                    extent: image_extent(image, &entry)?,
                });
            }
        }
    }
    movies.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(Archive {
        source: image.to_path_buf(),
        image: true,
        dat,
        hd4,
        sound_dat,
        sound_hd3,
        movies,
        image_bytes: None,
    })
}

fn open_image(image: &Path) -> CoreResult<Archive> {
    open_image_bytes(image, &SourceBytes::open_iso(image)?.bytes()?)
}

fn directory_file(root: &Path, name: &str) -> CoreResult<Extent> {
    for entry in fs::read_dir(root)? {
        let entry = entry?;
        if entry.file_type()?.is_file()
            && entry
                .file_name()
                .to_string_lossy()
                .eq_ignore_ascii_case(name)
        {
            return Ok(Extent {
                file: entry.path(),
                offset: 0,
                size: entry.metadata()?.len(),
            });
        }
    }
    Err(error(format!("{} holds no {name}", root.display())))
}

fn open_directory(root: &Path) -> CoreResult<Archive> {
    let movies_dir = fs::read_dir(root)?
        .filter_map(Result::ok)
        .find(|entry| {
            entry.file_type().is_ok_and(|kind| kind.is_dir())
                && entry
                    .file_name()
                    .to_string_lossy()
                    .eq_ignore_ascii_case("MOVIE")
        })
        .ok_or_else(|| error(format!("{} holds no MOVIE directory", root.display())))?;
    // Subdirectories hold the manual's movies (MOVIE/TUTO and MOVIE/TUTO2).
    let mut movies = Vec::new();
    let mut pending = vec![(String::new(), movies_dir.path())];
    while let Some((prefix, directory)) = pending.pop() {
        for entry in fs::read_dir(directory)? {
            let entry = entry?;
            let name = format!("{prefix}{}", entry.file_name().to_string_lossy());
            let kind = entry.file_type()?;
            if kind.is_dir() {
                pending.push((format!("{name}/"), entry.path()));
            } else if kind.is_file() && folded(name.as_bytes()).ends_with(b".pss") {
                movies.push(NamedExtent {
                    name: name.into_bytes(),
                    extent: Extent {
                        file: entry.path(),
                        offset: 0,
                        size: entry.metadata()?.len(),
                    },
                });
            }
        }
    }
    movies.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(Archive {
        source: root.to_path_buf(),
        image: false,
        dat: directory_file(root, "DATA.DAT")?,
        hd4: directory_file(root, "DATA.HD4")?,
        sound_dat: directory_file(root, "SOUND.DAT")?,
        sound_hd3: directory_file(root, "SOUND.HD3")?,
        movies,
        image_bytes: None,
    })
}

fn open_archive(source: &Path) -> CoreResult<Archive> {
    if source.is_dir() {
        open_directory(source)
    } else if source.is_file() {
        open_image(source)
    } else {
        Err(error(format!(
            "{} is neither a disc image nor a directory",
            source.display()
        )))
    }
}

#[derive(Clone)]
struct DataRecord {
    name: Vec<u8>,
    path: Vec<u8>,
    size: u32,
    sector: u32,
}

fn parse_data_index(hd4: &[u8], dat_size: u64) -> CoreResult<Vec<DataRecord>> {
    let names_start =
        usize::try_from(le32(hd4)?).map_err(|_| error("DATA.HD4 name offset overflow"))?;
    if names_start == 0 || names_start % 12 != 0 || names_start > hd4.len() {
        return Err(error("DATA.HD4 invalid records/name boundary"));
    }
    let mut records = Vec::with_capacity(names_start / 12);
    for index in 0..names_start / 12 {
        let raw = &hd4[index * 12..index * 12 + 12];
        let name_offset = le32(raw)? as usize;
        if name_offset < names_start || name_offset >= hd4.len() {
            return Err(error(format!(
                "DATA.HD4 record {index}: name outside names"
            )));
        }
        let name = nul_name(hd4, name_offset, "DATA.HD4")?;
        let path = folded(&name);
        if !path.is_empty() && !safe_relative(&disk_name(&path)) {
            return Err(error(format!("DATA.HD4 record {index}: unsafe path")));
        }
        let size = le32(&raw[4..])?;
        let sector = le32(&raw[8..])?;
        let end = u64::from(sector)
            .checked_mul(SECTOR)
            .and_then(|v| v.checked_add(u64::from(size)))
            .ok_or_else(|| error("DATA.DAT extent overflow"))?;
        if end > dat_size {
            return Err(error(format!("DATA.DAT truncated at record {index}")));
        }
        records.push(DataRecord {
            name,
            path,
            size,
            sector,
        });
    }
    Ok(records)
}

#[derive(Clone)]
struct SoundRecord {
    name: Vec<u8>,
    size: u32,
    sector: u32,
    sectors: u32,
}

struct SoundIndex {
    entries: Vec<SoundRecord>,
    shadowed: Vec<SoundRecord>,
}

fn parse_sound_index(hd3: &[u8], dat_size: u64) -> CoreResult<SoundIndex> {
    let names_start =
        usize::try_from(le32(hd3)?).map_err(|_| error("SOUND.HD3 name offset overflow"))?;
    if names_start == 0 || names_start % 16 != 0 || names_start > hd3.len() {
        return Err(error("SOUND.HD3 invalid records/name boundary"));
    }
    let count = names_start / 16;
    let terminator = &hd3[(count - 1) * 16..count * 16];
    let terminator_name = le32(terminator)? as usize;
    let empty_name = terminator_name == 0
        || (terminator_name >= names_start && hd3.get(terminator_name) == Some(&0));
    if !empty_name || terminator[4..16].iter().any(|&byte| byte != 0) {
        return Err(error("SOUND.HD3 has no final terminator"));
    }
    let mut seen = HashSet::new();
    let mut entries = Vec::new();
    let mut shadowed = Vec::new();
    for index in 0..count - 1 {
        let raw = &hd3[index * 16..index * 16 + 16];
        let name_offset = le32(raw)? as usize;
        if name_offset < names_start || name_offset >= hd3.len() {
            return Err(error(format!(
                "SOUND.HD3 record {index}: name outside names"
            )));
        }
        let name = nul_name(hd3, name_offset, "SOUND.HD3")?;
        if name.contains(&b'/') || name.contains(&b'\\') || !safe_relative(&disk_name(&name)) {
            return Err(error(format!("SOUND.HD3 record {index}: unsafe name")));
        }
        let size = le32(&raw[4..])?;
        let sector = le32(&raw[8..])?;
        let sectors = le32(&raw[12..])?;
        if size == 0 || u64::from(size).div_ceil(SECTOR) != u64::from(sectors) {
            return Err(error(format!(
                "SOUND.HD3 record {index}: invalid size/sector count"
            )));
        }
        let end = u64::from(sector)
            .checked_mul(SECTOR)
            .and_then(|v| v.checked_add(u64::from(size)))
            .ok_or_else(|| error("SOUND.DAT extent overflow"))?;
        if end > dat_size {
            return Err(error(format!("SOUND.HD3 record {index}: beyond SOUND.DAT")));
        }
        let record = SoundRecord {
            name: name.clone(),
            size,
            sector,
            sectors,
        };
        if seen.insert(name) {
            entries.push(record);
        } else {
            shadowed.push(record);
        }
    }
    Ok(SoundIndex { entries, shadowed })
}

fn read_index(archive: &Archive) -> CoreResult<Vec<DataRecord>> {
    parse_data_index(&archive.extent_bytes(&archive.hd4)?, archive.dat.size)
}

fn read_sound_index(archive: &Archive) -> CoreResult<SoundIndex> {
    parse_sound_index(
        &archive.extent_bytes(&archive.sound_hd3)?,
        archive.sound_dat.size,
    )
}

#[derive(Clone)]
struct Planned {
    rel: String,
    extent: Extent,
}

/// Indexed retail files read directly from their disc extents during conversion.
#[derive(Clone)]
pub struct SourceTree {
    entries: BTreeMap<String, Extent>,
    resources: BTreeMap<String, ResourceRef>,
    files: u64,
    bytes: u64,
    image: Option<SourceBytes>,
}

/// Bytes owned for the lifetime of all references into one source file.
#[derive(Clone)]
pub struct SourceBytes(Arc<SourceData>);

enum SourceData {
    Owned(Vec<u8>),
    /// iOS gives an app about 7 GB of address space: a 4.7 GB disc image, read or mapped whole, leaves the
    /// converters too little, and an allocation fails. There each resource maps its own range of the image only
    /// while it is read.
    Mapped { file: File, length: usize },
}

/// A source's bytes, borrowed from memory or mapped from the disc image for as long as this lives.
pub enum SourceSlice<'a> {
    Borrowed(&'a [u8]),
    Mapped(memmap2::Mmap),
    Read(Vec<u8>),
}

impl std::ops::Deref for SourceSlice<'_> {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        match self {
            Self::Borrowed(bytes) => bytes,
            Self::Mapped(map) => map,
            Self::Read(bytes) => bytes,
        }
    }
}

impl SourceBytes {
    pub fn owned(bytes: Vec<u8>) -> Self {
        Self(Arc::new(SourceData::Owned(bytes)))
    }

    /// Reads an ISO once and shares its immutable buffer with all converters.
    pub fn open_iso(path: &Path) -> CoreResult<Self> {
        Self::open_iso_with_progress(path, &Phase::noop())
    }

    pub fn open_iso_with_progress(path: &Path, phase: &Phase) -> CoreResult<Self> {
        let mut file = File::open(path)?;
        let length = usize::try_from(file.metadata()?.len())
            .map_err(|_| error("ISO exceeds addressable memory"))?;
        phase.set_total(length as u64);
        if cfg!(any(target_os = "ios", kazarov_mapped_source)) {
            phase.inc(length as u64);
            phase.add_bytes(length as u64);
            return Ok(Self(Arc::new(SourceData::Mapped { file, length })));
        }
        let mut bytes = Vec::with_capacity(length);
        let mut chunk = vec![0u8; CHUNK];
        loop {
            let count = file.read(&mut chunk)?;
            if count == 0 {
                break;
            }
            bytes.extend_from_slice(&chunk[..count]);
            phase.inc(count as u64);
            phase.add_bytes(count as u64);
        }
        if bytes.len() != length {
            return Err(error("ISO changed size while being read"));
        }
        Ok(Self::owned(bytes))
    }

    pub fn len(&self) -> usize {
        match &*self.0 {
            SourceData::Owned(bytes) => bytes.len(),
            SourceData::Mapped { length, .. } => *length,
        }
    }

    /// The bytes in range, which must lie inside the source.
    pub fn slice(&self, range: Range<usize>) -> CoreResult<SourceSlice<'_>> {
        if range.start > range.end || range.end > self.len() {
            return Err(error("source range outside the source"));
        }
        match &*self.0 {
            SourceData::Owned(bytes) => Ok(SourceSlice::Borrowed(&bytes[range])),
            SourceData::Mapped { file, .. } => {
                if range.is_empty() {
                    return Ok(SourceSlice::Borrowed(&[]));
                }
                // SAFETY: the mapping is read-only, and the disc image is not changed during extraction.
                let mapped = unsafe {
                    memmap2::MmapOptions::new()
                        .offset(range.start as u64)
                        .len(range.len())
                        .map(file)
                };
                match mapped {
                    Ok(map) => Ok(SourceSlice::Mapped(map)),
                    // Without address space for a mapping there may still be memory for a copy.
                    Err(_) => {
                        let mut bytes = vec![0; range.len()];
                        read_at(file, range.start as u64, &mut bytes)?;
                        Ok(SourceSlice::Read(bytes))
                    }
                }
            }
        }
    }

    /// The whole source: a mapped image is mapped whole only for as long as the result lives.
    pub fn bytes(&self) -> CoreResult<SourceSlice<'_>> {
        self.slice(0..self.len())
    }
}

#[cfg(unix)]
fn read_at(file: &File, offset: u64, bytes: &mut [u8]) -> CoreResult<()> {
    std::os::unix::fs::FileExt::read_exact_at(file, bytes, offset)?;
    Ok(())
}

#[cfg(windows)]
fn read_at(file: &File, mut offset: u64, mut bytes: &mut [u8]) -> CoreResult<()> {
    while !bytes.is_empty() {
        let count = std::os::windows::fs::FileExt::seek_read(file, bytes, offset)?;
        if count == 0 {
            return Err(error("disc image ended early"));
        }
        offset += count as u64;
        bytes = &mut bytes[count..];
    }
    Ok(())
}

/// A checked, immutable byte range of a retail resource.
#[derive(Clone)]
pub struct ResourceRef {
    pub alias: String,
    source: SourceBytes,
    range: Range<usize>,
}

impl ResourceRef {
    /// The resource's bytes. From a mapped disc image they stay mapped only while the result lives, so a
    /// converter holds them for the one resource it is converting.
    pub fn bytes(&self) -> SourceSlice<'_> {
        self.source
            .slice(self.range.clone())
            .unwrap_or_else(|failure| panic!("{}: cannot read the source: {failure}", self.alias))
    }

    pub fn len(&self) -> usize {
        self.range.len()
    }

    pub fn is_empty(&self) -> bool {
        self.range.is_empty()
    }

    pub fn subrange(&self, alias: impl Into<String>, range: Range<usize>) -> CoreResult<Self> {
        if range.start > range.end || range.end > self.len() {
            return Err(error("resource subrange outside source bytes"));
        }
        let start = self
            .range
            .start
            .checked_add(range.start)
            .ok_or_else(|| error("resource offset overflow"))?;
        let end = self
            .range
            .start
            .checked_add(range.end)
            .ok_or_else(|| error("resource offset overflow"))?;
        Ok(Self {
            alias: alias.into(),
            source: self.source.clone(),
            range: start..end,
        })
    }
}

/// Temporary converted PACK buffer, separate from the final asset namespace.
pub fn compatibility_work_path(published: &Path, name: &str) -> PathBuf {
    published.join(".work/packs").join(name)
}

/// Finds a converted PACK buffer or a final standalone retained source file.
pub fn compatibility_path(published: &Path, name: &str) -> PathBuf {
    let work = compatibility_work_path(published, name);
    if work.is_file() {
        work
    } else {
        published.join("data").join(name)
    }
}

impl SourceTree {
    pub fn open(source: &Path) -> CoreResult<Self> {
        Self::open_with_progress(source, &Phase::noop())
    }

    /// Reads an ISO once while reporting bytes acquired for its shared resource index.
    pub fn open_with_progress(source: &Path, phase: &Phase) -> CoreResult<Self> {
        let image = source
            .is_file()
            .then(|| SourceBytes::open_iso_with_progress(source, phase))
            .transpose()?;
        let mut archive = if let Some(image) = &image {
            open_image_bytes(source, &image.bytes()?)?
        } else {
            open_archive(source)?
        };
        archive.image_bytes = image.clone();
        let (plan, files, bytes) = plan_archive(&archive)?;
        let mut resources = BTreeMap::new();
        for item in &plan {
            let source_bytes = if let Some(image) = &image {
                image.clone()
            } else {
                SourceBytes::owned(item.extent.read()?)
            };
            let start =
                usize::try_from(item.extent.offset).map_err(|_| error("source offset overflow"))?;
            let size =
                usize::try_from(item.extent.size).map_err(|_| error("source size overflow"))?;
            let range = if image.is_some() {
                start
                    ..start
                        .checked_add(size)
                        .ok_or_else(|| error("source range overflow"))?
            } else {
                0..size
            };
            if range.end > source_bytes.len() {
                return Err(error(format!("truncated source extent: {}", item.rel)));
            }
            resources.insert(
                item.rel.clone(),
                ResourceRef {
                    alias: item.rel.clone(),
                    source: source_bytes,
                    range,
                },
            );
        }
        Ok(Self {
            entries: plan
                .into_iter()
                .map(|item| (item.rel, item.extent))
                .collect(),
            resources,
            files,
            bytes,
            image,
        })
    }

    /// Opens an already unpacked source tree for standalone converter callers.
    pub fn directory(root: &Path) -> CoreResult<Self> {
        fn gather(
            root: &Path,
            directory: &Path,
            entries: &mut BTreeMap<String, Extent>,
        ) -> CoreResult<()> {
            for entry in fs::read_dir(directory)? {
                let path = entry?.path();
                if path.is_dir() {
                    gather(root, &path, entries)?;
                } else if path.is_file() {
                    let name = path
                        .strip_prefix(root)
                        .map_err(|e| error(e.to_string()))?
                        .to_string_lossy()
                        .replace('\\', "/");
                    entries.insert(
                        name,
                        Extent {
                            size: path.metadata()?.len(),
                            file: path,
                            offset: 0,
                        },
                    );
                }
            }
            Ok(())
        }
        let mut entries = BTreeMap::new();
        gather(root, root, &mut entries)?;
        let bytes = entries.values().map(|entry| entry.size).sum();
        let mut resources = BTreeMap::new();
        for (name, extent) in &entries {
            let bytes = SourceBytes::owned(extent.read()?);
            resources.insert(
                name.clone(),
                ResourceRef {
                    alias: name.clone(),
                    range: 0..bytes.len(),
                    source: bytes,
                },
            );
        }
        Ok(Self {
            files: entries.len() as u64,
            bytes,
            entries,
            resources,
            image: None,
        })
    }

    pub fn names(&self) -> impl Iterator<Item = &str> {
        self.entries.keys().map(String::as_str)
    }

    pub fn contains(&self, name: &str) -> bool {
        self.entries.contains_key(name)
    }

    pub fn len(&self, name: &str) -> CoreResult<u64> {
        self.entries
            .get(name)
            .map(|entry| entry.size)
            .ok_or_else(|| error(format!("source file missing: {name}")))
    }

    pub fn read(&self, name: &str) -> CoreResult<Vec<u8>> {
        Ok(self.resource(name)?.bytes().to_vec())
    }

    pub fn resource(&self, name: &str) -> CoreResult<ResourceRef> {
        self.resources
            .get(name)
            .cloned()
            .ok_or_else(|| error(format!("source file missing: {name}")))
    }

    /// Hashes the same ISO bytes used by the indexed resources.
    pub fn fingerprint_sha256(&self) -> CoreResult<String> {
        let image = self
            .image
            .as_ref()
            .ok_or_else(|| error("source is not an ISO"))?;
        let mut hash = Sha256::new();
        hash.update(b"kazarov-source-file-v1\0");
        hash.update((image.len() as u64).to_le_bytes());
        // A mapped image is hashed in pieces, so the whole of it is never mapped at once.
        let mut start = 0;
        while start < image.len() {
            let end = image.len().min(start + (64 << 20));
            hash.update(&*image.slice(start..end)?);
            start = end;
        }
        Ok(hash
            .finalize()
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect())
    }

    pub fn copy(&self, name: &str, destination: &Path) -> CoreResult<()> {
        let extent = self
            .entries
            .get(name)
            .ok_or_else(|| error(format!("source file missing: {name}")))?;
        copy_extent(extent, destination)
    }

    pub fn totals(&self) -> (u64, u64) {
        (self.files, self.bytes)
    }
}

fn plan_archive(archive: &Archive) -> CoreResult<(Vec<Planned>, u64, u64)> {
    let records = read_index(archive)?;
    let mut seen_game = HashSet::new();
    let mut target_names = HashSet::new();
    let mut plan = Vec::new();
    let mut files = 0u64;
    let mut bytes = 0u64;
    let mut add = |rel: String, extent: Extent| -> CoreResult<()> {
        if !safe_relative(&rel) || rel == "manifest.txt" || rel.ends_with(".dc2tmp") {
            return Err(error(format!("unsafe or reserved extracted path {rel}")));
        }
        if !target_names.insert(rel.clone()) {
            return Err(error(format!("duplicate extracted path {rel}")));
        }
        plan.push(Planned { rel, extent });
        Ok(())
    };
    for record in records {
        if record.path.is_empty() || !seen_game.insert(record.path.clone()) {
            continue;
        }
        let rel = disk_name(&record.path);
        let offset = archive
            .dat
            .offset
            .checked_add(u64::from(record.sector) * SECTOR)
            .ok_or_else(|| error("DATA.DAT offset overflow"))?;
        add(
            rel,
            Extent {
                file: archive.dat.file.clone(),
                offset,
                size: u64::from(record.size),
            },
        )?;
        files += 1;
        bytes += u64::from(record.size);
    }
    for movie in &archive.movies {
        add(
            format!("movie/{}", disk_name(&movie.name)),
            movie.extent.clone(),
        )?;
    }
    add("sound/sound.dat".into(), archive.sound_dat.clone())?;
    add("sound/sound.hd3".into(), archive.sound_hd3.clone())?;
    let sound = read_sound_index(archive)?;
    for entry in sound.entries {
        let offset = archive
            .sound_dat
            .offset
            .checked_add(u64::from(entry.sector) * SECTOR)
            .ok_or_else(|| error("SOUND.DAT offset overflow"))?;
        add(
            format!("sound/{}", disk_name(&entry.name)),
            Extent {
                file: archive.sound_dat.file.clone(),
                offset,
                size: u64::from(entry.size),
            },
        )?;
    }
    Ok((plan, files, bytes))
}

fn copy_extent(extent: &Extent, target: &Path) -> CoreResult<()> {
    fs::create_dir_all(
        target
            .parent()
            .ok_or_else(|| error("target has no parent"))?,
    )?;
    let temp = target.with_extension(format!(
        "{}.dc2tmp",
        target.extension().unwrap_or_default().to_string_lossy()
    ));
    let result = (|| -> CoreResult<()> {
        let mut input = File::open(&extent.file)?;
        let end = extent
            .offset
            .checked_add(extent.size)
            .ok_or_else(|| error("extent overflow"))?;
        if end > input.metadata()?.len() {
            return Err(error(format!(
                "{}: truncated extent",
                extent.file.display()
            )));
        }
        input.seek(SeekFrom::Start(extent.offset))?;
        let mut output = File::create(&temp)?;
        let mut buffer = vec![0u8; CHUNK];
        let mut remaining = extent.size;
        while remaining > 0 {
            let count = usize::try_from(remaining.min(CHUNK as u64))
                .map_err(|_| error("chunk overflow"))?;
            input.read_exact(&mut buffer[..count])?;
            output.write_all(&buffer[..count])?;
            remaining -= count as u64;
        }
        output.sync_all()?;
        drop(output);
        for attempt in 0..4 {
            match fs::rename(&temp, target) {
                Ok(()) => return Ok(()),
                Err(err) if attempt < 3 && matches!(err.raw_os_error(), Some(5 | 22 | 32)) => {
                    std::thread::sleep(std::time::Duration::from_millis(25 * (attempt + 1)));
                }
                Err(err) => return Err(error(format!("{}: {err}", target.display()))),
            }
        }
        Err(error(format!(
            "{}: rename retry exhausted",
            target.display()
        )))
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result
}

fn extent_matches(extent: &Extent, target: &Path) -> CoreResult<bool> {
    let target_meta = match fs::metadata(target) {
        Ok(meta) => meta,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(false),
        Err(error) => return Err(error.into()),
    };
    if !target_meta.is_file() || target_meta.len() != extent.size {
        return Ok(false);
    }
    let mut source = File::open(&extent.file)?;
    let end = extent
        .offset
        .checked_add(extent.size)
        .ok_or_else(|| error("extent overflow"))?;
    if end > source.metadata()?.len() {
        return Err(error("source extent truncated during resume"));
    }
    source.seek(SeekFrom::Start(extent.offset))?;
    let mut existing = File::open(target)?;
    let mut source_chunk = vec![0u8; 1 << 20];
    let mut existing_chunk = vec![0u8; 1 << 20];
    let mut remaining = extent.size;
    while remaining != 0 {
        let count = usize::try_from(remaining.min(source_chunk.len() as u64))
            .map_err(|_| error("extent comparison overflow"))?;
        source.read_exact(&mut source_chunk[..count])?;
        existing.read_exact(&mut existing_chunk[..count])?;
        if source_chunk[..count] != existing_chunk[..count] {
            return Ok(false);
        }
        remaining -= count as u64;
    }
    Ok(true)
}

pub fn extract_raw(source: &Path, raw_out: &Path, jobs: usize) -> CoreResult<(u64, u64)> {
    rayon::ThreadPoolBuilder::new()
        .num_threads(jobs.clamp(1, 256))
        .build()
        .map_err(|err| error(format!("cannot start extraction workers: {err}")))?
        .install(|| extract_raw_with_progress(source, raw_out, jobs, &Phase::noop()))
}

/// Reports each raw file after copying or verifying its cached contents.
/// Returns the retail data-file totals without copying extraction payloads.
pub fn extraction_totals(source: &Path) -> CoreResult<(u64, u64)> {
    let (_, files, bytes) = plan_archive(&open_archive(source)?)?;
    Ok((files, bytes))
}

pub fn extract_raw_with_progress(
    source: &Path,
    raw_out: &Path,
    jobs: usize,
    phase: &Phase,
) -> CoreResult<(u64, u64)> {
    if jobs == 0 || jobs > 256 {
        return Err(error("jobs must be in 1..=256"));
    }
    let archive = open_archive(source)?;
    let (plan, files, bytes) = plan_archive(&archive)?;
    let planned: u64 = plan.iter().map(|item| item.extent.size).sum();
    phase.log(format!(
        "extracting {} raw files ({}) with {} workers",
        plan.len(),
        super::progress::size(planned),
        jobs
    ));
    phase.set_total(planned);
    fs::create_dir_all(raw_out)?;
    let completed = AtomicUsize::new(0);
    let skipped = AtomicUsize::new(0);
    plan.par_iter().try_for_each(|item| -> CoreResult<()> {
        let target = raw_out.join(&item.rel);
        if item.extent.size >= 8 << 20 {
            phase.item(&item.rel);
        }
        if extent_matches(&item.extent, &target)
            .map_err(|err| error(format!("{}: resume comparison: {}", item.rel, err.0)))?
        {
            skipped.fetch_add(1, Ordering::Relaxed);
        } else {
            copy_extent(&item.extent, &target)
                .map_err(|err| error(format!("{}: {}", item.rel, err.0)))?;
        }
        completed.fetch_add(1, Ordering::Relaxed);
        phase.inc(item.extent.size);
        phase.add_bytes(item.extent.size);
        Ok(())
    })?;
    if completed.load(Ordering::Relaxed) != plan.len() {
        return Err(error("incomplete raw extraction"));
    }
    let skipped = skipped.load(Ordering::Relaxed);
    if skipped > 0 {
        phase.log(format!(
            "{skipped} raw files reused after byte verification"
        ));
    }
    Ok((files, bytes))
}

pub fn list_text(source: &Path) -> CoreResult<String> {
    let archive = open_archive(source)?;
    let records = read_index(&archive)?;
    let mut seen = HashSet::new();
    let mut files = 0u64;
    let mut duplicates = 0u64;
    let mut bytes = 0u64;
    let mut output = format!(
        "source: {} ({})\nDATA.DAT: {} bytes, DATA.HD4: {} bytes, SOUND.DAT: {} bytes\n",
        archive.source.display(),
        if archive.image {
            "ISO 9660 image"
        } else {
            "directory"
        },
        archive.dat.size,
        archive.hd4.size,
        archive.sound_dat.size
    );
    for record in &records {
        output.push_str(&format!(
            "{}\t{}\t{}\n",
            if record.name.is_empty() {
                "<no name>".into()
            } else {
                String::from_utf8_lossy(&record.name).into_owned()
            },
            record.size,
            record.sector
        ));
        if record.path.is_empty() {
            continue;
        }
        if seen.insert(&record.path) {
            files += 1;
            bytes += u64::from(record.size);
        } else {
            duplicates += 1;
        }
    }
    output.push_str(&format!(
        "{} entries, {} files, {} duplicates, {} bytes\n",
        records.len(),
        files,
        duplicates,
        bytes
    ));
    Ok(output)
}

pub fn sound_text(source: &Path) -> CoreResult<String> {
    let archive = open_archive(source)?;
    let index = read_sound_index(&archive)?;
    let mut bytes = 0u64;
    let mut padded = 0u64;
    let mut output = String::new();
    for (prefix, entry) in index
        .entries
        .iter()
        .map(|entry| ("", entry))
        .chain(index.shadowed.iter().map(|entry| ("shadowed\t", entry)))
    {
        output.push_str(&format!(
            "{}{name}\t{}\t{}\t{}\n",
            prefix,
            entry.size,
            entry.sector,
            entry.sectors,
            name = String::from_utf8_lossy(&entry.name)
        ));
        bytes += u64::from(entry.size);
        padded += u64::from(entry.sectors) * SECTOR;
    }
    output.push_str(&format!(
        "sound: {} entries, {} shadowed, {} bytes, {} bytes in sectors, SOUND.DAT {} bytes\n",
        index.entries.len(),
        index.shadowed.len(),
        bytes,
        padded,
        archive.sound_dat.size
    ));
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn index() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&24u32.to_le_bytes());
        bytes.extend_from_slice(&4u32.to_le_bytes());
        bytes.extend_from_slice(&0u32.to_le_bytes());
        bytes.extend_from_slice(&32u32.to_le_bytes());
        bytes.extend_from_slice(&0u32.to_le_bytes());
        bytes.extend_from_slice(&0u32.to_le_bytes());
        bytes.extend_from_slice(b"FOO.BIN\0\0");
        bytes
    }

    fn sound_index() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&32u32.to_le_bytes());
        bytes.extend_from_slice(&3u32.to_le_bytes());
        bytes.extend_from_slice(&0u32.to_le_bytes());
        bytes.extend_from_slice(&1u32.to_le_bytes());
        bytes.extend_from_slice(&[0; 16]);
        bytes.extend_from_slice(b"BGM_A.STR\0");
        bytes
    }

    fn disc_dir(root: &Path) -> CoreResult<()> {
        fs::create_dir_all(root.join("MOVIE"))?;
        fs::write(root.join("DATA.HD4"), index())?;
        fs::write(root.join("DATA.DAT"), b"DATA")?;
        fs::write(root.join("SOUND.HD3"), sound_index())?;
        fs::write(root.join("SOUND.DAT"), b"SND")?;
        fs::write(root.join("MOVIE/INTRO.PSS"), b"PSS")?;
        fs::create_dir_all(root.join("MOVIE/TUTO2"))?;
        fs::write(root.join("MOVIE/TUTO2/VTUTO24.PSS"), b"TUTO")?;
        Ok(())
    }

    fn record(name: &[u8], sector: u32, size: u32, directory: bool) -> Vec<u8> {
        let length = 33 + name.len() + usize::from(name.len().is_multiple_of(2));
        let mut bytes = vec![0u8; length];
        bytes[0] = length as u8;
        bytes[2..6].copy_from_slice(&sector.to_le_bytes());
        bytes[6..10].copy_from_slice(&sector.to_be_bytes());
        bytes[10..14].copy_from_slice(&size.to_le_bytes());
        bytes[14..18].copy_from_slice(&size.to_be_bytes());
        bytes[25] = if directory { 2 } else { 0 };
        bytes[28] = 1;
        bytes[31] = 1;
        bytes[32] = name.len() as u8;
        bytes[33..33 + name.len()].copy_from_slice(name);
        bytes
    }

    fn iso_bytes() -> Vec<u8> {
        let mut iso = vec![0u8; 27 * SECTOR as usize];
        let descriptor = 16 * SECTOR as usize;
        iso[descriptor] = 1;
        iso[descriptor + 1..descriptor + 6].copy_from_slice(b"CD001");
        iso[descriptor + 6] = 1;
        iso[descriptor + 128..descriptor + 130].copy_from_slice(&(SECTOR as u16).to_le_bytes());
        iso[descriptor + 130..descriptor + 132].copy_from_slice(&(SECTOR as u16).to_be_bytes());
        let root = record(&[0], 18, SECTOR as u32, true);
        iso[descriptor + 156..descriptor + 156 + root.len()].copy_from_slice(&root);
        let terminator = 17 * SECTOR as usize;
        iso[terminator] = 255;
        iso[terminator + 1..terminator + 6].copy_from_slice(b"CD001");
        iso[terminator + 6] = 1;
        let mut root_entries = Vec::new();
        root_entries.extend_from_slice(&record(&[0], 18, SECTOR as u32, true));
        root_entries.extend_from_slice(&record(&[1], 18, SECTOR as u32, true));
        root_entries.extend_from_slice(&record(b"DATA.HD4;1", 20, index().len() as u32, false));
        root_entries.extend_from_slice(&record(b"DATA.DAT;1", 21, 4, false));
        root_entries.extend_from_slice(&record(
            b"SOUND.HD3;1",
            22,
            sound_index().len() as u32,
            false,
        ));
        root_entries.extend_from_slice(&record(b"SOUND.DAT;1", 23, 3, false));
        root_entries.extend_from_slice(&record(b"MOVIE", 19, SECTOR as u32, true));
        iso[18 * SECTOR as usize..18 * SECTOR as usize + root_entries.len()]
            .copy_from_slice(&root_entries);
        let mut movie_entries = Vec::new();
        movie_entries.extend_from_slice(&record(&[0], 19, SECTOR as u32, true));
        movie_entries.extend_from_slice(&record(&[1], 18, SECTOR as u32, true));
        movie_entries.extend_from_slice(&record(b"INTRO.PSS;1", 24, 3, false));
        movie_entries.extend_from_slice(&record(b"TUTO2", 25, SECTOR as u32, true));
        iso[19 * SECTOR as usize..19 * SECTOR as usize + movie_entries.len()]
            .copy_from_slice(&movie_entries);
        let mut manual_entries = Vec::new();
        manual_entries.extend_from_slice(&record(&[0], 25, SECTOR as u32, true));
        manual_entries.extend_from_slice(&record(&[1], 19, SECTOR as u32, true));
        manual_entries.extend_from_slice(&record(b"VTUTO24.PSS;1", 26, 4, false));
        iso[25 * SECTOR as usize..25 * SECTOR as usize + manual_entries.len()]
            .copy_from_slice(&manual_entries);
        for (sector, payload) in [
            (20, index()),
            (21, b"DATA".to_vec()),
            (22, sound_index()),
            (23, b"SND".to_vec()),
            (24, b"PSS".to_vec()),
            (26, b"TUTO".to_vec()),
        ] {
            let offset = sector * SECTOR as usize;
            iso[offset..offset + payload.len()].copy_from_slice(&payload);
        }
        iso
    }

    #[test]
    fn extraction_progress_finishes_only_after_valid_publication() -> CoreResult<()> {
        let temp = tempfile::tempdir()?;
        let disc = temp.path().join("disc");
        disc_dir(&disc)?;
        fs::remove_file(disc.join("MOVIE/INTRO.PSS"))?;
        fs::remove_dir_all(disc.join("MOVIE/TUTO2"))?;
        let output = temp.path().join("game/data");
        let updates = std::sync::Mutex::new(Vec::new());
        super::super::extract_with_progress(&disc, &output, 2, &|fraction, _| {
            if fraction == 1.0 {
                assert!(super::super::check(&output).is_ok());
            }
            updates.lock().unwrap().push(fraction);
        })?;
        let values = updates.into_inner().unwrap();
        assert_eq!(values.last(), Some(&1.0));
        assert!(values.iter().any(|value| *value > 0.05 && *value < 0.95));

        fs::write(disc.join("DATA.HD4"), [0u8; 12])?;
        let failed = std::sync::Mutex::new(Vec::new());
        assert!(
            super::super::extract_with_progress(&disc, &output, 2, &|fraction, _| {
                failed.lock().unwrap().push(fraction);
            })
            .is_err()
        );
        assert!(
            failed
                .into_inner()
                .unwrap()
                .iter()
                .all(|value| *value < 1.0)
        );
        assert!(super::super::check(&output).is_ok());
        Ok(())
    }

    #[test]
    fn directory_and_iso_parallel_extract_match() -> CoreResult<()> {
        let temp = tempfile::tempdir()?;
        let disc = temp.path().join("disc");
        disc_dir(&disc)?;
        let iso = temp.path().join("disc.iso");
        fs::write(&iso, iso_bytes())?;
        let directory_out = temp.path().join("from-directory");
        let iso_out = temp.path().join("from-iso");
        assert_eq!(extract_raw(&disc, &directory_out, 1)?, (1, 4));
        assert_eq!(extract_raw(&iso, &iso_out, 4)?, (1, 4));
        for rel in [
            "foo.bin",
            "sound/sound.dat",
            "sound/sound.hd3",
            "sound/bgm_a.str",
            "movie/intro.pss",
            "movie/tuto2/vtuto24.pss",
        ] {
            assert_eq!(
                fs::read(directory_out.join(rel))?,
                fs::read(iso_out.join(rel))?,
                "{rel}"
            );
        }
        assert_eq!(fs::read(iso_out.join("movie/tuto2/vtuto24.pss"))?, b"TUTO");
        assert!(list_text(&iso)?.contains("1 files"));
        assert!(sound_text(&iso)?.contains("1 entries"));
        Ok(())
    }

    #[test]
    fn malformed_index_cannot_publish_raw_files() -> CoreResult<()> {
        let temp = tempfile::tempdir()?;
        let disc = temp.path().join("disc");
        disc_dir(&disc)?;
        fs::write(disc.join("DATA.HD4"), [0u8; 12])?;
        let raw = temp.path().join("raw");
        assert!(extract_raw(&disc, &raw, 4).is_err());
        assert!(!raw.join("foo.bin").exists());
        Ok(())
    }

    #[test]
    fn resume_reuses_only_exact_raw_bytes() -> CoreResult<()> {
        let temp = tempfile::tempdir()?;
        let disc = temp.path().join("disc");
        disc_dir(&disc)?;
        let raw = temp.path().join("raw");
        extract_raw(&disc, &raw, 2)?;
        let source = Extent {
            file: disc.join("DATA.DAT"),
            offset: 0,
            size: 4,
        };
        assert!(extent_matches(&source, &raw.join("foo.bin"))?);
        fs::write(raw.join("foo.bin"), b"BAD!")?;
        assert!(!extent_matches(&source, &raw.join("foo.bin"))?);
        extract_raw(&disc, &raw, 2)?;
        assert_eq!(fs::read(raw.join("foo.bin"))?, b"DATA");
        assert!(extent_matches(&source, &raw.join("foo.bin"))?);
        Ok(())
    }

    #[test]
    #[ignore = "requires KAZAROV_RETAIL_ISO pointing at the local PAL disc image"]
    fn retail_iso_index_matches_the_game_oracle() -> CoreResult<()> {
        let path = std::env::var_os("KAZAROV_RETAIL_ISO")
            .ok_or_else(|| error("KAZAROV_RETAIL_ISO is missing"))?;
        let archive = open_image(Path::new(&path))?;
        let records = read_index(&archive)?;
        assert_eq!(records.len(), 8753);
        let mut seen = HashSet::new();
        assert_eq!(
            records
                .iter()
                .filter(|record| !record.path.is_empty() && seen.insert(&record.path))
                .count(),
            8751
        );
        // 9 movies in /MOVIE and the manual's 22 in each of /MOVIE/TUTO and /MOVIE/TUTO2.
        assert_eq!(archive.movies.len(), 53);
        assert!(
            archive
                .movies
                .iter()
                .any(|movie| movie.name == b"TUTO2/VTUTO44.PSS")
        );
        assert!(!read_sound_index(&archive)?.entries.is_empty());
        Ok(())
    }
}
