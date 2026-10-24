//! Bytes that wait in a temporary file instead of memory.
//!
//! The model pass keeps every file's packed resources and artifacts from analysis until publication, over a
//! gigabyte for the whole disc. iOS ends an app long before a desktop runs short, so there they are written to
//! one file in the temporary directory and read back by the job that publishes them.

use super::CoreResult;
use std::borrow::Cow;
use std::fs::File;
use std::sync::{Mutex, OnceLock};

/// Whether waiting bytes go to the temporary file: on iOS, and where tests ask for the iOS source path.
const SPILL: bool = cfg!(any(target_os = "ios", kazarov_mapped_source));

pub(crate) enum SpilledBytes {
    Memory(Vec<u8>),
    File { offset: u64, length: usize },
}

impl SpilledBytes {
    pub(crate) fn new(bytes: Vec<u8>) -> CoreResult<Self> {
        if !SPILL || bytes.is_empty() {
            return Ok(Self::Memory(bytes));
        }
        let offset = append(&bytes)?;
        Ok(Self::File {
            offset,
            length: bytes.len(),
        })
    }

    pub(crate) fn read(&self) -> CoreResult<Cow<'_, [u8]>> {
        match self {
            Self::Memory(bytes) => Ok(Cow::Borrowed(bytes)),
            Self::File { offset, length } => {
                let mut bytes = vec![0u8; *length];
                read_at(&spill()?.file, &mut bytes, *offset)?;
                Ok(Cow::Owned(bytes))
            }
        }
    }
}

struct Spill {
    file: File,
    /// Serialises appends; reads use positioned I/O and need no lock.
    end: Mutex<u64>,
}

fn spill() -> CoreResult<&'static Spill> {
    static SPILL_FILE: OnceLock<Result<Spill, String>> = OnceLock::new();
    SPILL_FILE
        .get_or_init(|| open().map_err(|error| format!("spill file: {error}")))
        .as_ref()
        .map_err(|error| super::CoreError(error.clone()))
}

fn open() -> std::io::Result<Spill> {
    let path = std::env::temp_dir().join(format!("kazarov-spill-{}.bin", std::process::id()));
    let mut options = std::fs::OpenOptions::new();
    options.read(true).write(true).create(true).truncate(true);
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        // FILE_FLAG_DELETE_ON_CLOSE: the file goes with the process.
        options.custom_flags(0x0400_0000);
    }
    let file = options.open(&path)?;
    // Unlinked while open, so it goes with the process however that ends.
    #[cfg(unix)]
    std::fs::remove_file(&path)?;
    Ok(Spill {
        file,
        end: Mutex::new(0),
    })
}

fn append(bytes: &[u8]) -> CoreResult<u64> {
    let spill = spill()?;
    let mut end = spill
        .end
        .lock()
        .map_err(|_| super::CoreError("spill file lock poisoned".into()))?;
    let offset = *end;
    write_at(&spill.file, bytes, offset)?;
    *end += bytes.len() as u64;
    Ok(offset)
}

#[cfg(unix)]
fn write_at(file: &File, bytes: &[u8], offset: u64) -> std::io::Result<()> {
    use std::os::unix::fs::FileExt;
    file.write_all_at(bytes, offset)
}

#[cfg(unix)]
fn read_at(file: &File, bytes: &mut [u8], offset: u64) -> std::io::Result<()> {
    use std::os::unix::fs::FileExt;
    file.read_exact_at(bytes, offset)
}

#[cfg(windows)]
fn write_at(file: &File, mut bytes: &[u8], mut offset: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !bytes.is_empty() {
        let written = file.seek_write(bytes, offset)?;
        if written == 0 {
            return Err(std::io::ErrorKind::WriteZero.into());
        }
        bytes = &bytes[written..];
        offset += written as u64;
    }
    Ok(())
}

#[cfg(windows)]
fn read_at(file: &File, mut bytes: &mut [u8], mut offset: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !bytes.is_empty() {
        let read = file.seek_read(bytes, offset)?;
        if read == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        bytes = &mut bytes[read..];
        offset += read as u64;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spilled_bytes_read_back_whole() {
        let first = SpilledBytes::new(vec![1, 2, 3]).unwrap();
        let second = SpilledBytes::new((0..=255).collect()).unwrap();
        assert_eq!(&*first.read().unwrap(), &[1, 2, 3]);
        assert_eq!(second.read().unwrap().len(), 256);
        assert_eq!(second.read().unwrap()[255], 255);
    }
}
