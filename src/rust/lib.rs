pub mod core;
mod setup;

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
