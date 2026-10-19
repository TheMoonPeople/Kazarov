use super::{CoreError, CoreResult};
use gpt_disk_io::{BlockIoAdapter, gpt_disk_types::BlockSize};
use iso9660::{DirectoryIterator, FileEntry, find_file, mount};
use rayon::prelude::*;
use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

const SECTOR: u64 = 2048;
const CHUNK: usize = 4 << 20;

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

fn open_image(image: &Path) -> CoreResult<Archive> {
    let block_size =
        BlockSize::new(SECTOR as u32).ok_or_else(|| error("invalid ISO sector size"))?;
    let mut io = BlockIoAdapter::new(File::open(image)?, block_size);
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
    let mut movies = Vec::new();
    for entry in DirectoryIterator::new(&mut io, movies_dir.extent_lba, movies_dir.data_length) {
        let entry = entry
            .map_err(|cause| error(format!("{}: ISO /MOVIE entry: {cause}", image.display())))?;
        if !entry.flags.directory && entry.name.to_ascii_lowercase().ends_with(".pss") {
            movies.push(NamedExtent {
                name: entry.name.as_bytes().to_vec(),
                extent: image_extent(image, &entry)?,
            });
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
    })
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
    let mut movies = Vec::new();
    for entry in fs::read_dir(movies_dir.path())? {
        let entry = entry?;
        let name = entry.file_name().to_string_lossy().as_bytes().to_vec();
        if entry.file_type()?.is_file() && folded(&name).ends_with(b".pss") {
            movies.push(NamedExtent {
                name,
                extent: Extent {
                    file: entry.path(),
                    offset: 0,
                    size: entry.metadata()?.len(),
                },
            });
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
    parse_data_index(&archive.hd4.read()?, archive.dat.size)
}

fn read_sound_index(archive: &Archive) -> CoreResult<SoundIndex> {
    parse_sound_index(&archive.sound_hd3.read()?, archive.sound_dat.size)
}

#[derive(Clone)]
struct Planned {
    rel: String,
    extent: Extent,
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
    extract_raw_with_progress(source, raw_out, jobs, &|_, _| {})
}

/// Reports each raw file after copying or verifying its cached contents.
pub fn extract_raw_with_progress(
    source: &Path,
    raw_out: &Path,
    jobs: usize,
    progress: &(dyn Fn(usize, usize) + Sync),
) -> CoreResult<(u64, u64)> {
    if jobs == 0 || jobs > 256 {
        return Err(error("jobs must be in 1..=256"));
    }
    let archive = open_archive(source)?;
    let (plan, files, bytes) = plan_archive(&archive)?;
    eprintln!(
        "dc2data: extracting {} raw files ({} bytes) with {} workers",
        plan.len(),
        bytes,
        jobs
    );
    fs::create_dir_all(raw_out)?;
    let completed = AtomicUsize::new(0);
    let skipped = AtomicUsize::new(0);
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(jobs)
        .build()
        .map_err(|err| error(format!("cannot start extraction workers: {err}")))?;
    pool.install(|| {
        plan.par_iter().try_for_each(|item| -> CoreResult<()> {
            let target = raw_out.join(&item.rel);
            if extent_matches(&item.extent, &target)
                .map_err(|err| error(format!("{}: resume comparison: {}", item.rel, err.0)))?
            {
                skipped.fetch_add(1, Ordering::Relaxed);
            } else {
                copy_extent(&item.extent, &target)
                    .map_err(|err| error(format!("{}: {}", item.rel, err.0)))?;
            }
            let done = completed.fetch_add(1, Ordering::Relaxed) + 1;
            progress(done, plan.len());
            if done.is_multiple_of(500) || done == plan.len() {
                eprintln!(
                    "dc2data: raw files {done}/{} ({} verified and reused)",
                    plan.len(),
                    skipped.load(Ordering::Relaxed)
                );
            }
            Ok(())
        })
    })?;
    if completed.load(Ordering::Relaxed) != plan.len() {
        return Err(error("incomplete raw extraction"));
    }
    eprintln!(
        "dc2data: raw extraction complete; {} files reused after byte verification",
        skipped.load(Ordering::Relaxed)
    );
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
        let mut iso = vec![0u8; 25 * SECTOR as usize];
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
        iso[19 * SECTOR as usize..19 * SECTOR as usize + movie_entries.len()]
            .copy_from_slice(&movie_entries);
        for (sector, payload) in [
            (20, index()),
            (21, b"DATA".to_vec()),
            (22, sound_index()),
            (23, b"SND".to_vec()),
            (24, b"PSS".to_vec()),
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
        ] {
            assert_eq!(
                fs::read(directory_out.join(rel))?,
                fs::read(iso_out.join(rel))?,
                "{rel}"
            );
        }
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
        assert!(!archive.movies.is_empty());
        assert!(!read_sound_index(&archive)?.entries.is_empty());
        Ok(())
    }
}
