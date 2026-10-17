pub mod core;
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

    struct CheckReport {
        textures: u64,
    }

    extern "Rust" {
        fn bridge_smoke(value: &str) -> Result<String>;
        fn extract(source: &str, data_out: &str, jobs: usize) -> Result<ExtractReport>;
        fn list(source: &str) -> Result<String>;
        fn sound(source: &str) -> Result<String>;
        fn check(data_root: &str) -> Result<CheckReport>;
        fn current(source: &str, data_root: &str) -> Result<bool>;
        /// Runs first-run setup on the main thread; false means the user closed it.
        fn setup_data(title: &str, data_root: &str) -> Result<bool>;

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
        /// Decodes a PNG of exactly `width` by `height` pixels into `out` as 8-bit RGBA (`width * height * 4`
        /// bytes). Safe to call from any thread.
        fn png_decode_rgba(png: &[u8], width: u32, height: u32, out: &mut [u8]) -> Result<()>;
    }
}

fn setup_data(title: &str, data_root: &str) -> Result<bool, core::CoreError> {
    setup::run(title, data_root.as_ref())
}

fn bridge_smoke(value: &str) -> Result<String, core::CoreError> {
    if value == "error" {
        return Err(core::CoreError("bridge smoke error".into()));
    }
    Ok(value.to_owned())
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

fn list(source: &str) -> Result<String, core::CoreError> {
    core::list(source.as_ref())
}

fn sound(source: &str) -> Result<String, core::CoreError> {
    core::sound(source.as_ref())
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
