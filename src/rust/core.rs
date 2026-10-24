use progress::{Phase, Reporter, Stage};
use sha2::{Digest, Sha256};
use std::fmt;
use std::io::Read;
use std::path::{Path, PathBuf};

#[path = "actions.rs"]
pub mod actions;
#[path = "archive.rs"]
pub mod archive;
#[path = "artifacts.rs"]
pub mod artifacts;
#[path = "audio.rs"]
pub mod audio;
#[path = "configs.rs"]
pub mod configs;
#[path = "configs_native.rs"]
pub mod configs_native;
#[path = "inventory.rs"]
pub mod inventory;
#[path = "models.rs"]
pub mod models;
#[path = "native_scripts.rs"]
pub mod native_scripts;
#[path = "packs.rs"]
pub mod packs;
#[path = "progress.rs"]
pub mod progress;
#[path = "publish.rs"]
pub mod publish;
#[path = "spill.rs"]
pub(crate) mod spill;
#[path = "textures.rs"]
pub mod textures;
#[path = "zip_archive.rs"]
pub mod zip_archive;

#[derive(Debug)]
pub struct CoreError(pub String);

impl fmt::Display for CoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

impl std::error::Error for CoreError {}

impl From<std::io::Error> for CoreError {
    fn from(error: std::io::Error) -> Self {
        Self(error.to_string())
    }
}

impl From<serde_json::Error> for CoreError {
    fn from(error: serde_json::Error) -> Self {
        Self(error.to_string())
    }
}

pub type CoreResult<T> = Result<T, CoreError>;

pub fn data_version() -> u64 {
    let value: serde_json::Value = serde_json::from_str(include_str!("../../data_version.json"))
        .expect("invalid data_version.json");
    assert_eq!(value["schema_version"], 1, "invalid shared schema version");
    value["data_version"]
        .as_u64()
        .expect("missing shared data version")
}

#[derive(Debug, Default)]
pub struct ExtractReport {
    pub files: u64,
    pub bytes: u64,
    pub texture_sources: u64,
    pub pngs: u64,
}

#[derive(Debug, Default)]
pub struct CheckReport {
    pub textures: u64,
}

/// Controls whether converted retail script files remain in the published archive.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ScriptPublicationPolicy {
    /// Preserve compiled script bytes for compatibility with existing callers.
    #[default]
    Retain,
    /// Omit compiled script resources while retaining logical references for loose Luau modules.
    LuauOnly,
}

fn source_fingerprint(source: &Path, phase: &Phase) -> CoreResult<String> {
    fn hash_file(path: &Path, hash: &mut Sha256, phase: &Phase) -> CoreResult<()> {
        const CHUNK: usize = 4 << 20;
        let mut file = std::fs::File::open(path)?;
        let (full, filled) = std::sync::mpsc::sync_channel::<std::io::Result<Vec<u8>>>(2);
        let (empty, recycled) = std::sync::mpsc::sync_channel::<Vec<u8>>(3);
        for _ in 0..3 {
            empty.send(vec![0; CHUNK]).expect("fingerprint buffers");
        }
        std::thread::scope(|scope| {
            scope.spawn(move || {
                while let Ok(mut buffer) = recycled.recv() {
                    buffer.resize(CHUNK, 0);
                    let read = file.read(&mut buffer).map(|count| {
                        buffer.truncate(count);
                        buffer
                    });
                    let end = read.as_ref().map_or(true, Vec::is_empty);
                    if full.send(read).is_err() || end {
                        break;
                    }
                }
            });
            while let Ok(buffer) = filled.recv() {
                let buffer = buffer?;
                if buffer.is_empty() {
                    break;
                }
                hash.update(&buffer);
                phase.inc(buffer.len() as u64);
                let _ = empty.send(buffer);
            }
            Ok(())
        })
    }
    fn gather(dir: &Path, out: &mut Vec<PathBuf>) -> CoreResult<()> {
        for entry in std::fs::read_dir(dir)? {
            let path = entry?.path();
            let metadata = std::fs::symlink_metadata(&path)?;
            if metadata.file_type().is_symlink() {
                return Err(CoreError(format!(
                    "source symlink unsupported: {}",
                    path.display()
                )));
            }
            out.push(path.clone());
            if metadata.is_dir() {
                gather(&path, out)?;
            }
        }
        Ok(())
    }
    let mut hash = Sha256::new();
    if source.is_file() {
        hash.update(b"kazarov-source-file-v1\0");
        let length = std::fs::metadata(source)?.len();
        phase.set_total(length);
        hash.update(length.to_le_bytes());
        hash_file(source, &mut hash, phase)?;
    } else if source.is_dir() {
        hash.update(b"kazarov-source-tree-v1\0");
        let mut paths = Vec::new();
        gather(source, &mut paths)?;
        paths.sort_by_key(|p| {
            p.strip_prefix(source)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/")
        });
        let mut entries = Vec::with_capacity(paths.len());
        for path in paths {
            let length = if path.is_dir() {
                None
            } else {
                Some(std::fs::metadata(&path)?.len())
            };
            phase.add_total(length.unwrap_or(0));
            entries.push((path, length));
        }
        for (path, length) in entries {
            let relative = path
                .strip_prefix(source)
                .map_err(|e| CoreError(e.to_string()))?
                .to_string_lossy()
                .replace('\\', "/");
            let bytes = relative.as_bytes();
            hash.update((bytes.len() as u64).to_le_bytes());
            hash.update(bytes);
            match length {
                None => hash.update(b"D"),
                Some(length) => {
                    hash.update(b"F");
                    hash.update(length.to_le_bytes());
                    phase.item(&relative);
                    hash_file(&path, &mut hash, phase)?;
                }
            }
        }
    } else {
        return Err(CoreError(format!(
            "source does not exist: {}",
            source.display()
        )));
    }
    Ok(hash
        .finalize()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect())
}

fn source_state(source: &Path) -> CoreResult<serde_json::Value> {
    use std::time::UNIX_EPOCH;
    fn modified(path: &Path) -> CoreResult<u128> {
        Ok(std::fs::metadata(path)?
            .modified()?
            .duration_since(UNIX_EPOCH)
            .map_err(|e| CoreError(e.to_string()))?
            .as_nanos())
    }
    let canonical = source.canonicalize()?;
    let path = canonical.to_string_lossy().into_owned();
    let entries = if canonical.is_file() {
        vec![
            serde_json::json!({"path":"","type":"file","size":std::fs::metadata(&canonical)?.len(),"mtime_ns":modified(&canonical)?.to_string()}),
        ]
    } else if canonical.is_dir() {
        fn gather(root: &Path, dir: &Path, out: &mut Vec<serde_json::Value>) -> CoreResult<()> {
            for entry in std::fs::read_dir(dir)? {
                let path = entry?.path();
                let metadata = std::fs::symlink_metadata(&path)?;
                if metadata.file_type().is_symlink() {
                    return Err(CoreError(format!(
                        "source symlink unsupported: {}",
                        path.display()
                    )));
                }
                let relative = path
                    .strip_prefix(root)
                    .map_err(|e| CoreError(e.to_string()))?
                    .to_string_lossy()
                    .replace('\\', "/");
                out.push(serde_json::json!({"path":relative,"type":if metadata.is_dir(){"directory"}else{"file"},
                    "size":if metadata.is_file(){metadata.len()}else{0},"mtime_ns":modified(&path)?.to_string()}));
                if metadata.is_dir() {
                    gather(root, &path, out)?;
                }
            }
            Ok(())
        }
        let mut entries = Vec::new();
        gather(&canonical, &canonical, &mut entries)?;
        entries.sort_by(|a, b| a["path"].as_str().cmp(&b["path"].as_str()));
        entries
    } else {
        return Err(CoreError(format!(
            "source does not exist: {}",
            source.display()
        )));
    };
    let serialized = serde_json::to_vec(&entries)?;
    let fingerprint: String = Sha256::digest(&serialized)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    Ok(serde_json::json!({"path":path,"fingerprint":fingerprint}))
}

pub fn extract(source: &Path, data_out: &Path, jobs: usize) -> CoreResult<ExtractReport> {
    extract_with_policy(source, data_out, jobs, ScriptPublicationPolicy::Retain)
}

/// Extracts data using an explicit compiled-script publication policy.
pub fn extract_with_policy(
    source: &Path,
    data_out: &Path,
    jobs: usize,
    script_policy: ScriptPublicationPolicy,
) -> CoreResult<ExtractReport> {
    Reporter::new(&|_, _| {})
        .with_console()
        .run(|reporter| extract_stages(source, data_out, jobs, reporter, script_policy))
}

/// Recognizes a complete, verified archive staged for the unchanged source.
#[cfg(test)]
fn completed_stage_pngs(
    published: &Path,
    state: &serde_json::Value,
    source_sha256: &str,
    phase: &Phase,
) -> Option<u64> {
    let marker = publish::read_marker(published).ok()?;
    if marker["source_state"] != *state || marker["source_sha256"].as_str() != Some(source_sha256) {
        return None;
    }
    publish::verify_with_progress(published, phase).ok()
}

/// Extracts and publishes data, reporting weighted stage progress from a fixed-cadence ticker.
/// Callbacks can run concurrently; 100% is emitted only after successful publication.
pub fn extract_with_progress(
    source: &Path,
    data_out: &Path,
    jobs: usize,
    progress: &(dyn Fn(f32, &'static str) + Sync),
) -> CoreResult<ExtractReport> {
    extract_with_progress_policy(
        source,
        data_out,
        jobs,
        progress,
        ScriptPublicationPolicy::Retain,
    )
}

/// Extracts and publishes data under an explicit compiled-script publication policy.
pub fn extract_with_progress_policy(
    source: &Path,
    data_out: &Path,
    jobs: usize,
    progress: &(dyn Fn(f32, &'static str) + Sync),
    script_policy: ScriptPublicationPolicy,
) -> CoreResult<ExtractReport> {
    Reporter::new(progress)
        .run(|reporter| extract_stages(source, data_out, jobs, reporter, script_policy))
}

const WORKER_STACK: usize = 16 << 20;

#[cfg(windows)]
fn system_available_memory() -> Option<u64> {
    #[repr(C)]
    struct MemoryStatus {
        length: u32,
        memory_load: u32,
        total_physical: u64,
        available_physical: u64,
        total_page_file: u64,
        available_page_file: u64,
        total_virtual: u64,
        available_virtual: u64,
        available_extended_virtual: u64,
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GlobalMemoryStatusEx(status: *mut MemoryStatus) -> i32;
    }
    let mut status: MemoryStatus = unsafe { std::mem::zeroed() };
    status.length = std::mem::size_of::<MemoryStatus>() as u32;
    (unsafe { GlobalMemoryStatusEx(&mut status) } != 0).then_some(status.available_physical)
}

#[cfg(target_os = "linux")]
fn system_available_memory() -> Option<u64> {
    let pages = unsafe { libc::sysconf(libc::_SC_AVPHYS_PAGES) };
    let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    (pages > 0 && page_size > 0).then(|| (pages as u64).saturating_mul(page_size as u64))
}

// What the process may still allocate before iOS ends it, which is less than the device's free memory.
#[cfg(target_os = "ios")]
fn system_available_memory() -> Option<u64> {
    unsafe extern "C" {
        fn os_proc_available_memory() -> usize;
    }
    let available = unsafe { os_proc_available_memory() } as u64;
    (available > 0).then_some(available)
}

#[cfg(not(any(windows, target_os = "linux", target_os = "ios")))]
fn system_available_memory() -> Option<u64> {
    None
}

// DC2DATA_AVAILABLE_MEMORY_MB stands in for the system's figure, to try a smaller device's budget.
fn available_memory() -> Option<u64> {
    if let Some(megabytes) = std::env::var("DC2DATA_AVAILABLE_MEMORY_MB")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
    {
        return Some(megabytes << 20);
    }
    system_available_memory()
}

fn conversion_memory_budget() -> usize {
    const GIB: u64 = 1 << 30;
    available_memory()
        .map(|available| available.saturating_sub(2 * GIB) / 4)
        .unwrap_or(GIB / 2)
        .clamp(128 << 20, 2 * GIB) as usize
}

fn expand_model_memory_budget(sink: &artifacts::AssetSink) -> CoreResult<()> {
    const GIB: u64 = 1 << 30;
    let Some(available) = available_memory() else {
        return Ok(());
    };
    let current = sink.conversion_limit()? as u64;
    let queue = sink.queue_limit()? as u64;
    let headroom = available.saturating_sub(2 * GIB + queue);
    let target = current.saturating_add(headroom / 2).min(6 * GIB);
    sink.raise_conversion_limit(target as usize)?;
    Ok(())
}

fn category_stage(category: artifacts::Category) -> Stage {
    use artifacts::Category;
    match category {
        Category::Texture => Stage::Textures,
        Category::Model => Stage::Models,
        Category::Audio => Stage::Audio,
        Category::Movie => Stage::Movies,
        Category::Script => Stage::Scripts,
        Category::Pack => Stage::Packs,
        Category::Preserved => Stage::Preserved,
        Category::Gameplay => Stage::Configs,
    }
}

fn interleave_jobs(jobs: Vec<artifacts::Job>) -> Vec<artifacts::Job> {
    let mut categories: [Vec<artifacts::Job>; 8] = std::array::from_fn(|_| Vec::new());
    for job in jobs {
        categories[job.category as usize].push(job);
    }
    for category in &mut categories {
        category.sort_by_key(|job| job.work);
    }
    let mut ordered = Vec::with_capacity(categories.iter().map(Vec::len).sum());
    loop {
        let before = ordered.len();
        for category in &mut categories {
            if let Some(job) = category.pop() {
                ordered.push(job);
            }
        }
        if before == ordered.len() {
            return ordered;
        }
    }
}

fn execute_jobs(
    jobs: Vec<artifacts::Job>,
    pool: &rayon::ThreadPool,
    sink: &artifacts::AssetSink,
    phases: &[Option<Phase>; 8],
    remaining: &[std::sync::atomic::AtomicU64; 8],
) -> CoreResult<()> {
    use rayon::prelude::*;
    use std::sync::atomic::Ordering;
    pool.install(|| {
        interleave_jobs(jobs)
            .into_par_iter()
            .with_max_len(1)
            .try_for_each(|job| {
                let category = job.category as usize;
                let work = job.work;
                let phase = phases[category]
                    .as_ref()
                    .ok_or_else(|| CoreError("unplanned asset category".into()))?;
                phase.item(&job.label);
                job.execute(sink)?;
                phase.inc(work);
                if remaining[category].fetch_sub(work, Ordering::AcqRel) == work {
                    phase.done();
                }
                Ok(())
            })
    })
}

#[cfg(test)]
fn immutable_after_cleanup(published: &Path) -> CoreResult<Vec<zip_archive::Input>> {
    Ok(publish::tree_inputs(published)?
        .into_iter()
        .filter(|input| {
            [".png", ".gltf", ".gltf.bin", ".game.json"]
                .iter()
                .any(|extension| input.name.ends_with(extension))
        })
        .collect())
}

fn summary(report: &ExtractReport, marker: &serde_json::Value) -> String {
    format!(
        "{} source files ({}) · {} texture sources · {} PNGs · data.zip {} in {} entries",
        progress::group(report.files),
        progress::size(report.bytes),
        progress::group(report.texture_sources),
        progress::group(report.pngs),
        progress::size(marker["archive_size"].as_u64().unwrap_or(0)),
        progress::group(marker["archive_entries"].as_u64().unwrap_or(0)),
    )
}

fn remove_private_stage(path: &Path, stage: &Path, root: &Path) -> CoreResult<()> {
    let stage = stage.canonicalize()?;
    let target = path.canonicalize()?;
    if stage.parent() != Some(root.canonicalize()?.as_path())
        || !stage
            .file_name()
            .is_some_and(|name| name.to_string_lossy().starts_with(".dc2data-stage-"))
        || !target.starts_with(&stage)
    {
        return Err(CoreError(
            "refusing cleanup outside the private extraction stage".into(),
        ));
    }
    std::fs::remove_dir_all(target)?;
    Ok(())
}

fn extract_stages(
    source: &Path,
    data_out: &Path,
    jobs: usize,
    reporter: &Reporter<'_>,
    script_policy: ScriptPublicationPolicy,
) -> CoreResult<ExtractReport> {
    use std::collections::HashSet;
    use std::sync::{Arc, atomic::AtomicU64};
    if data_out.file_name().is_none_or(|name| name != "data") {
        return Err(CoreError("output must be <game-root>/data".into()));
    }
    let root = data_out
        .parent()
        .ok_or_else(|| CoreError("missing game root".into()))?;
    std::fs::create_dir_all(root)?;
    if !(1..=1024).contains(&jobs) {
        return Err(CoreError("jobs must be between 1 and 1024".into()));
    }
    let source = source.canonicalize()?;
    if source.is_dir() && root.canonicalize()?.starts_with(&source) {
        return Err(CoreError(
            "The output must be outside the source disc folder.".into(),
        ));
    }
    let gameplay_documents = configs::gameplay_documents(Some(root))?;
    configs::initialize_character_plans(&gameplay_documents)?;
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(jobs)
        .stack_size(WORKER_STACK)
        .thread_name(|index| format!("dc2data-worker-{index}"))
        .build()
        .map_err(|error| CoreError(format!("cannot start conversion workers: {error}")))?;
    let phase = reporter.phase(Stage::Fingerprint);
    let initial_state = source_state(&source)?;
    // The owned source image is shared by hashing, indexing, every converter and final validation.
    let source_tree = Arc::new(archive::SourceTree::open_with_progress(&source, &phase)?);
    let source_sha256 = if source.is_file() {
        source_tree.fingerprint_sha256()?
    } else {
        source_fingerprint(&source, &phase)?
    };
    reporter.done(Stage::Fingerprint);
    let phase = reporter.phase(Stage::Extract);
    let (files, bytes) = source_tree.totals();
    let inventory = inventory::Inventory::build(&source_tree)?;
    phase.set_total(files);
    reporter.done(Stage::Extract);
    let gameplay_plan_sha256 = configs::asset_plan_fingerprint();
    let gameplay_source_fingerprint = configs::source_fingerprint();
    let metadata = serde_json::json!({
        "schema_version":1,"data_version":data_version(),"source":source.to_string_lossy(),
        "source_sha256":source_sha256,"script_publication_policy":format!("{script_policy:?}"),"gameplay_plan_sha256":gameplay_plan_sha256,
        "gameplay_source_fingerprint":gameplay_source_fingerprint,"jobs":jobs,
    });
    let stage = publish::acquire_stage(root, &metadata)?;
    let published = stage.path().join("published");
    if published.exists() {
        remove_private_stage(&published, stage.path(), root)?;
    }
    std::fs::create_dir_all(&published)?;
    reporter.phase(Stage::Planning);
    let (texture_plan, audio_plan) = pool.install(|| {
        rayon::join(
            || textures::plan_source(&inventory),
            || audio::plan_stream(&inventory),
        )
    });
    let texture_plan = texture_plan?;
    let audio_plan = audio_plan?;
    let texture_paths: std::collections::BTreeMap<String, String> = texture_plan
        .documents
        .iter()
        .map(|document| {
            (
                document.source_alias.clone(),
                document.document_path.clone(),
            )
        })
        .collect();
    let texture_sources = texture_plan.documents.len() as u64;
    let pngs = texture_plan.png_names.len() as u64;
    let mut model_plan = pool.install(|| {
        models::plan_source(
            source_tree.clone(),
            &inventory,
            Arc::new(texture_plan.documents.clone()),
            Arc::new(texture_plan.png_names.clone()),
        )
    })?;
    let profiles = models::planned_profile_names(&inventory)?;
    let mut consumed: HashSet<String> = texture_plan.consumed.iter().cloned().collect();
    consumed.extend(audio_plan.consumed);
    consumed.extend(model_plan.consumed.iter().cloned());
    consumed.extend(configs::gameplay_source_aliases(&inventory));
    let converted_aliases = consumed.clone();
    let (script_consumed, script_jobs) = native_scripts::plan_jobs(
        &inventory,
        &converted_aliases,
        &profiles,
        &texture_paths,
        script_policy == ScriptPublicationPolicy::Retain,
    )?;
    consumed.extend(script_consumed);
    let texture_members = texture_plan
        .documents
        .iter()
        .filter(|document| {
            !model_plan
                .embedded_texture_sources
                .contains(&document.source_alias)
                && !model_plan.consumed.contains(&document.source_alias)
        })
        .map(|document| {
            (
                document.source_alias.clone(),
                document.document_path.clone(),
            )
        })
        .collect();
    let mut all_jobs = texture_plan
        .into_jobs_without_documents(&model_plan.embedded_texture_sources, &model_plan.consumed);
    all_jobs.extend(audio_plan.jobs);
    all_jobs.extend(model_plan.into_initial_jobs());
    all_jobs.extend(script_jobs);
    all_jobs.extend(packs::plan_jobs(&inventory, &consumed, &texture_members)?);
    for chunk in gameplay_documents.chunks(128) {
        let documents = chunk.to_vec();
        all_jobs.push(artifacts::Job {
            category: artifacts::Category::Gameplay,
            label: documents[0].0.to_owned(),
            work: documents.len() as u64,
            estimated_bytes: documents.iter().map(|(_, bytes)| bytes.len()).sum(),
            run: Box::new(move |sink| {
                for (path, bytes) in documents {
                    sink.submit(
                        path,
                        artifacts::ArtifactBytes::Owned(bytes),
                        artifacts::Category::Gameplay,
                    )?;
                }
                Ok(())
            }),
        });
    }
    let mut totals = [0u64; 8];
    for job in &all_jobs {
        totals[job.category as usize] += job.work;
    }
    totals[artifacts::Category::Model as usize] += model_plan.deferred_work;
    let categories = [
        artifacts::Category::Texture,
        artifacts::Category::Model,
        artifacts::Category::Audio,
        artifacts::Category::Movie,
        artifacts::Category::Script,
        artifacts::Category::Pack,
        artifacts::Category::Preserved,
        artifacts::Category::Gameplay,
    ];
    let phases: [Option<Phase>; 8] = std::array::from_fn(|i| {
        let stage = category_stage(categories[i]);
        if totals[i] == 0 {
            reporter.skip(stage);
            None
        } else {
            let phase = reporter.phase(stage);
            phase.set_total(totals[i]);
            Some(phase)
        }
    });
    let remaining = std::array::from_fn(|i| AtomicU64::new(totals[i]));
    reporter.done(Stage::Planning);
    let archive_phase = reporter.phase(Stage::Seal);
    let sink = publish::begin_stream(
        &published,
        conversion_memory_budget(),
        archive_phase,
        totals.iter().sum(),
    )?;
    let (model_jobs, other_jobs): (Vec<_>, Vec<_>) = all_jobs
        .into_iter()
        .partition(|job| job.category == artifacts::Category::Model);
    let (other_conversion, model_conversion) = pool.install(|| {
        rayon::join(
            || {
                execute_jobs(other_jobs, &pool, &sink, &phases, &remaining)?;
                expand_model_memory_budget(&sink)
            },
            || {
                execute_jobs(model_jobs, &pool, &sink, &phases, &remaining)?;
                let deferred = model_plan.finish_jobs(&sink, &|work, label| {
                    let category = artifacts::Category::Model as usize;
                    if let Some(phase) = &phases[category] {
                        phase.item(label);
                        phase.inc(work);
                        remaining[category].fetch_sub(work, std::sync::atomic::Ordering::AcqRel);
                    }
                })?;
                execute_jobs(deferred, &pool, &sink, &phases, &remaining)
            },
        )
    });
    let conversion = other_conversion.and(model_conversion);
    if let Err(error) = conversion {
        let error = sink.first_failure().unwrap_or(error);
        let _ = sink.abort();
        return Err(error);
    }
    let marker = serde_json::json!({
        "schema_version":1,"data_version":data_version(),"source_state":initial_state,
        "source_sha256":source_sha256,"gameplay_plan_sha256":gameplay_plan_sha256,
        "gameplay_source_fingerprint":gameplay_source_fingerprint,
        "script_publication_policy":format!("{script_policy:?}"),
        "texture_sources":texture_sources,
    });
    publish::seal_stream(sink, &published, marker)?;
    reporter.done(Stage::Seal);
    // No validation begins until every converter and the streaming writer have completed.
    let phase = reporter.phase(Stage::VerifyArchive);
    pool.install(|| {
        publish::verify_stream_with_progress(
            &published,
            &inventory,
            &converted_aliases,
            &profiles,
            &texture_paths,
            &phase,
            script_policy == ScriptPublicationPolicy::Retain,
        )
    })?;
    if source_state(&source)? != initial_state {
        return Err(CoreError("source changed during extraction".into()));
    }
    reporter.done(Stage::VerifyArchive);
    reporter.phase(Stage::Publish);
    let marker = publish::read_marker(&published)?;
    publish::publish_verified(&published, root)?;
    let stage_path = stage.path().to_path_buf();
    drop(stage);
    remove_private_stage(&stage_path, &stage_path, root)?;
    reporter.done(Stage::Publish);
    let report = ExtractReport {
        files,
        bytes,
        texture_sources,
        pngs,
    };
    reporter.footer(summary(&report, &marker));
    Ok(report)
}

pub fn list(source: &Path) -> CoreResult<String> {
    archive::list_text(source)
}

pub fn sound(source: &Path) -> CoreResult<String> {
    archive::sound_text(source)
}

pub fn check(data_root: &Path) -> CoreResult<CheckReport> {
    if data_root.file_name().is_none_or(|name| name != "data") {
        return Err(CoreError("check path must be <game-root>/data".into()));
    }
    let root = data_root
        .parent()
        .ok_or_else(|| CoreError("missing game root".into()))?;
    Ok(CheckReport {
        textures: publish::verify(root)?,
    })
}

pub fn current(source: &Path, data_root: &Path) -> CoreResult<bool> {
    current_with_policy(source, data_root, ScriptPublicationPolicy::Retain)
}

/// Checks whether the published archive matches a specific script policy.
pub fn current_with_policy(
    source: &Path,
    data_root: &Path,
    script_policy: ScriptPublicationPolicy,
) -> CoreResult<bool> {
    if data_root.file_name().is_none_or(|name| name != "data") {
        return Err(CoreError("current path must be <game-root>/data".into()));
    }
    let Some(root) = data_root.parent() else {
        return Ok(false);
    };
    let Ok(marker) = publish::read_marker(root) else {
        return Ok(false);
    };
    if !publish::archive_present(root, &marker) {
        return Ok(false);
    }
    let state = match source_state(source) {
        Ok(state) => state,
        Err(_) => return Ok(false),
    };
    let marker_policy = match marker.get("script_publication_policy") {
        None => ScriptPublicationPolicy::Retain,
        Some(value) => match value.as_str() {
            Some("Retain") => ScriptPublicationPolicy::Retain,
            Some("LuauOnly") => ScriptPublicationPolicy::LuauOnly,
            _ => return Ok(false),
        },
    };
    if marker_policy != script_policy {
        return Ok(false);
    }
    Ok(marker["source_state"] == state
        && marker["source_sha256"].as_str().is_some()
        && marker["gameplay_plan_sha256"].as_str()
            == Some(configs::asset_plan_fingerprint().as_str())
        && marker["gameplay_source_fingerprint"].as_str()
            == Some(configs::source_fingerprint().as_str()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use zip_archive::Archive;

    #[test]
    fn immutable_archive_batch_includes_bulk_model_buffers() {
        let temp = tempfile::tempdir().unwrap();
        let data = temp.path().join("data");
        std::fs::create_dir(&data).unwrap();
        for name in [
            "model.gltf",
            "model.gltf.bin",
            "model.game.json",
            "picture.png",
            "script.json",
            "raw.bin",
        ] {
            std::fs::write(data.join(name), b"ready").unwrap();
        }
        let names: std::collections::BTreeSet<_> = immutable_after_cleanup(temp.path())
            .unwrap()
            .into_iter()
            .map(|input| input.name)
            .collect();
        assert_eq!(
            names,
            std::collections::BTreeSet::from([
                "model.gltf".to_owned(),
                "model.gltf.bin".to_owned(),
                "model.game.json".to_owned(),
                "picture.png".to_owned(),
            ])
        );
    }

    #[test]
    fn retry_rebuilds_incomplete_conversion_without_publishing_stale_assets() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("disc");
        std::fs::create_dir_all(source.join("MOVIE")).unwrap();
        let mut hd4 = vec![0u8; 33];
        hd4[0] = 24;
        hd4[4] = 4;
        hd4[12] = 32;
        hd4[24..33].copy_from_slice(b"FOO.BIN\0\0");
        std::fs::write(source.join("DATA.HD4"), hd4).unwrap();
        std::fs::write(source.join("DATA.DAT"), b"DATA").unwrap();
        let mut hd3 = vec![0u8; 42];
        hd3[0] = 32;
        hd3[4] = 3;
        hd3[12] = 1;
        hd3[32..42].copy_from_slice(b"BGM_A.STR\0");
        std::fs::write(source.join("SOUND.HD3"), hd3).unwrap();
        std::fs::write(source.join("SOUND.DAT"), b"SND").unwrap();
        let source = std::fs::canonicalize(source).unwrap();
        let root = temp.path().join("run");
        std::fs::create_dir_all(root.join("data")).unwrap();
        std::fs::create_dir_all(root.join("data")).unwrap();
        let metadata = serde_json::json!({
            "schema_version":1,"data_version":data_version(),"source":source.to_string_lossy(),
            "source_sha256":source_fingerprint(&source, &Phase::noop()).unwrap(),
            "gameplay_plan_sha256":configs::asset_plan_fingerprint(),
            "gameplay_source_fingerprint":configs::source_fingerprint(),"jobs":2,
        });
        let stage = publish::acquire_stage(&root, &metadata).unwrap();
        std::fs::create_dir_all(stage.path().join("published/data")).unwrap();
        std::fs::write(stage.path().join("published/data/stale.img"), b"old").unwrap();
        std::fs::create_dir_all(stage.path().join("raw")).unwrap();
        std::fs::write(stage.path().join("raw/foo.bin"), b"DATA").unwrap();
        drop(stage);
        extract(&source, &root.join("data"), 2).unwrap();
        let archive = Archive::open(&root.join(publish::ARCHIVE)).unwrap();
        assert!(!(0..archive.len()).any(|i| archive.name(i).contains("stale")));
        let index = (0..archive.len())
            .find(|&i| archive.name(i) == "foo.bin")
            .unwrap();
        assert_eq!(archive.read(index).unwrap(), b"DATA");
        drop(archive);
        check(&root.join("data")).unwrap();
        assert!(current(&source, &root.join("data")).unwrap());
        // Only the archive and its marker remain; legacy trees and the stage are gone.
        let mut left: Vec<_> = std::fs::read_dir(&root)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        left.sort();
        assert_eq!(left, ["data.json", "data.zip"]);
    }

    #[test]
    fn luau_only_policy_omits_loose_stb_and_stb_while_default_retains_them() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("disc");
        std::fs::create_dir_all(source.join("MOVIE")).unwrap();
        let mut hd4 = vec![0u8; 54];
        hd4[0..4].copy_from_slice(&36u32.to_le_bytes());
        hd4[4..8].copy_from_slice(&4u32.to_le_bytes());
        hd4[8..12].copy_from_slice(&0u32.to_le_bytes());
        hd4[12..16].copy_from_slice(&44u32.to_le_bytes());
        hd4[16..20].copy_from_slice(&4u32.to_le_bytes());
        hd4[20..24].copy_from_slice(&1u32.to_le_bytes());
        hd4[24..28].copy_from_slice(&53u32.to_le_bytes());
        hd4[36..44].copy_from_slice(b"FOO.STB\0");
        hd4[44..53].copy_from_slice(b"BAR.STB_\0");
        std::fs::write(source.join("DATA.HD4"), hd4).unwrap();
        let mut data = vec![0u8; 2052];
        data[..4].copy_from_slice(b"DATA");
        data[2048..].copy_from_slice(b"MORE");
        std::fs::write(source.join("DATA.DAT"), data).unwrap();
        let mut hd3 = vec![0u8; 42];
        hd3[0..4].copy_from_slice(&32u32.to_le_bytes());
        hd3[4..8].copy_from_slice(&3u32.to_le_bytes());
        hd3[8..12].copy_from_slice(&0u32.to_le_bytes());
        hd3[12..16].copy_from_slice(&1u32.to_le_bytes());
        hd3[32..42].copy_from_slice(b"BGM_A.STR\0");
        std::fs::write(source.join("SOUND.HD3"), hd3).unwrap();
        std::fs::write(source.join("SOUND.DAT"), b"SND").unwrap();

        let source = std::fs::canonicalize(source).unwrap();
        let retain_root = temp.path().join("retain");
        extract(&source, &retain_root.join("data"), 2).unwrap();
        let retained = Archive::open(&retain_root.join(publish::ARCHIVE)).unwrap();
        let retained_names: Vec<_> = (0..retained.len()).map(|i| retained.name(i)).collect();
        assert!(retained_names.contains(&"foo.stb"));
        assert!(retained_names.contains(&"bar.stb_"));

        let luau_root = temp.path().join("luau-only");
        extract_with_progress_policy(
            &source,
            &luau_root.join("data"),
            2,
            &|_, _| {},
            ScriptPublicationPolicy::LuauOnly,
        )
        .unwrap();
        let published = Archive::open(&luau_root.join(publish::ARCHIVE)).unwrap();
        assert!((0..published.len()).all(|i| {
            let name = published.name(i).to_ascii_lowercase();
            !name.ends_with(".stb") && !name.ends_with(".stb_")
        }));
        let marker: serde_json::Value =
            serde_json::from_slice(&std::fs::read(luau_root.join(publish::MARKER)).unwrap())
                .unwrap();
        assert_eq!(marker["script_publication_policy"], "LuauOnly");
    }

    fn sealed_stage(published: &Path, state: &serde_json::Value) {
        std::fs::create_dir_all(published.join("data")).unwrap();
        std::fs::write(published.join("data/example.bin"), b"payload").unwrap();
        for (name, bytes) in configs::gameplay_documents(None).unwrap() {
            let path = published.join("data").join(name);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, bytes).unwrap();
        }
        publish::finalize_assets(published).unwrap();
        publish::seal(
            published,
            serde_json::json!({
                "schema_version":1,"data_version":data_version(),"source_state":state,
                "source_sha256":"source-content", "gameplay_plan_sha256":configs::asset_plan_fingerprint(),
                "gameplay_source_fingerprint":configs::source_fingerprint(),
            }),
            &Phase::noop(),
        )
        .unwrap();
    }

    #[test]
    fn completed_stage_requires_source_identity_and_intact_archive() {
        let temp = tempfile::tempdir().unwrap();
        let published = temp.path().join("published");
        let state = serde_json::json!({"fingerprint":"source-state"});
        sealed_stage(&published, &state);
        assert_eq!(
            completed_stage_pngs(&published, &state, "source-content", &Phase::noop()),
            Some(0)
        );
        assert_eq!(
            completed_stage_pngs(&published, &state, "different-source", &Phase::noop()),
            None
        );
        assert_eq!(
            completed_stage_pngs(
                &published,
                &serde_json::json!({}),
                "source-content",
                &Phase::noop()
            ),
            None
        );
        let zip = published.join(publish::ARCHIVE);
        let mut bytes = std::fs::read(&zip).unwrap();
        let at = bytes
            .windows(7)
            .position(|window| window == b"example")
            .unwrap();
        bytes[at] = b'E';
        std::fs::write(&zip, bytes).unwrap();
        assert_eq!(
            completed_stage_pngs(&published, &state, "source-content", &Phase::noop()),
            None
        );
    }

    #[test]
    fn current_detects_nested_directory_change_and_stage_reuses_only_matching_source() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        std::fs::create_dir_all(source.join("nested")).unwrap();
        std::fs::write(source.join("nested/item.bin"), b"one").unwrap();
        let root = temp.path().join("run");
        let state = source_state(&source).unwrap();
        sealed_stage(&root, &state);
        assert!(current(&source, &root.join("data")).unwrap());
        let metadata = serde_json::json!({"schema_version":1,"source_sha256":source_fingerprint(&source, &Phase::noop()).unwrap()});
        let guard = publish::acquire_stage(&root, &metadata).unwrap();
        let reused = guard.path().to_path_buf();
        assert_ne!(
            publish::acquire_stage(&root, &metadata).unwrap().path(),
            reused
        );
        drop(guard);
        assert_eq!(
            publish::acquire_stage(&root, &metadata).unwrap().path(),
            reused
        );
        std::fs::write(source.join("nested/item.bin"), b"changed contents").unwrap();
        assert!(!current(&source, &root.join("data")).unwrap());
    }

    #[test]
    fn current_requires_the_requested_script_publication_policy() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        std::fs::create_dir_all(&source).unwrap();
        std::fs::write(source.join("item.bin"), b"one").unwrap();
        let root = temp.path().join("run");
        let state = source_state(&source).unwrap();
        sealed_stage(&root, &state);

        assert!(current(&source, &root.join("data")).unwrap());
        assert!(
            current_with_policy(&source, &root.join("data"), ScriptPublicationPolicy::Retain)
                .unwrap()
        );
        assert!(
            !current_with_policy(
                &source,
                &root.join("data"),
                ScriptPublicationPolicy::LuauOnly
            )
            .unwrap()
        );

        let marker_path = root.join(publish::MARKER);
        let mut marker: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&marker_path).unwrap()).unwrap();
        marker["script_publication_policy"] = serde_json::json!("LuauOnly");
        std::fs::write(&marker_path, serde_json::to_vec(&marker).unwrap()).unwrap();

        assert!(!current(&source, &root.join("data")).unwrap());
        assert!(
            !current_with_policy(&source, &root.join("data"), ScriptPublicationPolicy::Retain)
                .unwrap()
        );
        assert!(
            current_with_policy(
                &source,
                &root.join("data"),
                ScriptPublicationPolicy::LuauOnly
            )
            .unwrap()
        );

        marker["script_publication_policy"] = serde_json::json!(17);
        std::fs::write(&marker_path, serde_json::to_vec(&marker).unwrap()).unwrap();
        assert!(!current(&source, &root.join("data")).unwrap());
        assert!(
            !current_with_policy(&source, &root.join("data"), ScriptPublicationPolicy::Retain)
                .unwrap()
        );
        assert!(
            !current_with_policy(
                &source,
                &root.join("data"),
                ScriptPublicationPolicy::LuauOnly
            )
            .unwrap()
        );
    }

    #[test]
    fn current_rejects_directory_layout_and_truncated_archive() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        std::fs::create_dir_all(&source).unwrap();
        std::fs::write(source.join("item.bin"), b"one").unwrap();
        let root = temp.path().join("run");
        let state = source_state(&source).unwrap();
        sealed_stage(&root, &state);
        assert!(current(&source, &root.join("data")).unwrap());
        let zip = root.join(publish::ARCHIVE);
        let bytes = std::fs::read(&zip).unwrap();
        std::fs::write(&zip, &bytes[..bytes.len() - 1]).unwrap();
        assert!(!current(&source, &root.join("data")).unwrap());
        std::fs::remove_file(&zip).unwrap();
        std::fs::create_dir_all(root.join("data")).unwrap();
        std::fs::create_dir_all(root.join("data")).unwrap();
        std::fs::write(
            root.join("data.json"),
            serde_json::to_vec(&serde_json::json!({
                "schema_version":1,"data_version":8,"source_state":state,"source_sha256":"x",
            }))
            .unwrap(),
        )
        .unwrap();
        assert!(!current(&source, &root.join("data")).unwrap());
    }
}

#[cfg(test)]
mod script_policy_tests {
    use super::ScriptPublicationPolicy;

    #[test]
    fn script_publication_defaults_to_retain() {
        assert_eq!(
            ScriptPublicationPolicy::default(),
            ScriptPublicationPolicy::Retain
        );
    }
}
