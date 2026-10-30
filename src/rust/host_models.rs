//! Converts a game's models with the configuration reader the host program supplies.
use super::core::models::{self, GameProfile};
use super::core::{CoreError, CoreResult};
use cxx::UniquePtr;
use ffi::ProfileReader;
use std::sync::Arc;

// The C++ half stays out of unit tests, which link no C++ bridge.
#[cxx::bridge(namespace = "kazarov")]
mod ffi {
    struct ModelConvertReport {
        texture_sources: u64,
        pngs: u64,
        files: u64,
        /// Motions of another bundle's skeleton that fit no model, so nothing plays them.
        unresolved_motions: u64,
    }

    unsafe extern "C++" {
        include!("kazarov/profile_reader.hpp");

        /// The host's reader of configuration and cloth files; `read` reports failure by throwing.
        type ProfileReader;
        fn read(self: &ProfileReader, alias: &str, data: &[u8]) -> Result<String>;
    }

    extern "Rust" {
        /// Converts a Dark Cloud raw tree's textures and models into `output`, reading its
        /// configuration and cloth files with `reader` and keeping motions' source frames.
        fn models_convert_dc1(
            raw: &str,
            output: &str,
            reader: UniquePtr<ProfileReader>,
            jobs: usize,
        ) -> Result<ModelConvertReport>;

        /// Empties, in a data tree `models_convert_dc1` published into, every source it converted,
        /// leaving files their sizes and packs their layout; returns how many.
        fn models_strip_dc1(data: &str) -> Result<u64>;
    }
}

// The host's reader keeps no state between calls, as its contract states.
unsafe impl Send for ProfileReader {}
unsafe impl Sync for ProfileReader {}

fn models_convert_dc1(
    raw: &str,
    output: &str,
    reader: UniquePtr<ProfileReader>,
    jobs: usize,
) -> CoreResult<ffi::ModelConvertReport> {
    if reader.is_null() {
        return Err(CoreError("missing configuration reader".into()));
    }
    let read = move |alias: &str, data: &[u8]| -> CoreResult<serde_json::Value> {
        let text = reader
            .read(alias, data)
            .map_err(|error| CoreError(format!("{alias}: {error}")))?;
        serde_json::from_str(&text).map_err(|error| CoreError(format!("{alias}: {error}")))
    };
    let game = GameProfile {
        reader: Some(Arc::new(read)),
        absolute_frames: true,
        fixed_strip_streams: true,
        keep_source_records: true,
    };
    let (textures, models) = models::convert_tree_dc1(raw.as_ref(), output.as_ref(), &game, jobs)?;
    Ok(ffi::ModelConvertReport {
        texture_sources: textures.sources,
        pngs: textures.pngs,
        files: models.files.len() as u64,
        unresolved_motions: models.unresolved.len() as u64,
    })
}

fn models_strip_dc1(data: &str) -> CoreResult<u64> {
    Ok(models::strip_tree_dc1(data.as_ref())? as u64)
}
