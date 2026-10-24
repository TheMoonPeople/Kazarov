//! First-run desktop setup, invoked before the host game creates its window.

use crate::core::{self, CoreError, CoreResult, ScriptPublicationPolicy};
use eframe::egui;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// Latest worker progress, coalesced so large archives cannot flood the UI queue.
#[derive(Default)]
struct Progress {
    fraction: f32,
    message: &'static str,
}

impl Progress {
    /// Parallel completions can arrive out of order; the display never goes backwards.
    fn advance(&mut self, fraction: f32, message: &'static str) {
        if fraction >= self.fraction {
            self.fraction = fraction;
            self.message = message;
        }
    }
}

/// Owns the source editor and the extraction worker until publication finishes.
struct Setup {
    source: String,
    output: PathBuf,
    script_policy: ScriptPublicationPolicy,
    progress: Arc<Mutex<Progress>>,
    worker: Option<JoinHandle<CoreResult<core::ExtractReport>>>,
    error: Option<String>,
    completed_at: Option<Instant>,
    succeeded: Arc<std::sync::atomic::AtomicBool>,
}

impl Setup {
    /// Starts one extraction without blocking the native event loop.
    fn start(&mut self) {
        let mut source = PathBuf::from(&self.source);
        if let Some(image) = lone_disc_image(&source) {
            self.source = image.to_string_lossy().into_owned();
            source = image;
        }
        if !source.is_dir() && !source.is_file() {
            self.error = Some("Choose an existing disc folder or enter a disc image path.".into());
            return;
        }
        self.error = None;
        *self.progress.lock().unwrap() = Progress::default();
        let progress = self.progress.clone();
        let output = self.output.clone();
        let script_policy = self.script_policy;
        let jobs = std::thread::available_parallelism().map_or(1, usize::from);
        match std::thread::Builder::new()
            .name("game-data-extraction".into())
            .stack_size(16 * 1024 * 1024)
            .spawn(move || {
                core::extract_with_progress_policy(
                    &source,
                    &output,
                    jobs,
                    &|fraction, message| {
                        progress.lock().unwrap().advance(fraction, message);
                    },
                    script_policy,
                )
            }) {
            Ok(worker) => self.worker = Some(worker),
            Err(error) => self.error = Some(format!("Could not start extraction: {error}")),
        }
    }

    /// Receives completion and preserves errors for correction and retry.
    fn poll(&mut self) {
        if self
            .worker
            .as_ref()
            .is_some_and(|worker| worker.is_finished())
        {
            match self.worker.take().unwrap().join() {
                Ok(Ok(_)) => {
                    self.succeeded
                        .store(true, std::sync::atomic::Ordering::Release);
                    self.completed_at = Some(Instant::now());
                }
                Ok(Err(error)) => self.error = Some(error.to_string()),
                Err(_) => {
                    self.error = Some("Extraction stopped unexpectedly. Please try again.".into())
                }
            }
        }
    }
}

/// The disc image a folder stands for: its only `.iso` file, when the folder holds no `DATA.DAT` of its own.
fn lone_disc_image(folder: &Path) -> Option<PathBuf> {
    let mut disc_files = false;
    let mut images = Vec::new();
    for path in std::fs::read_dir(folder)
        .ok()?
        .filter_map(|entry| Some(entry.ok()?.path()))
    {
        disc_files |= path
            .file_name()
            .is_some_and(|name| name.eq_ignore_ascii_case("DATA.DAT"));
        if path.is_file()
            && path
                .extension()
                .is_some_and(|ext| ext.eq_ignore_ascii_case("iso"))
        {
            images.push(path);
        }
    }
    match images.as_slice() {
        [image] if !disc_files => Some(image.clone()),
        _ => None,
    }
}

impl eframe::App for Setup {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.poll();
        let busy = self.worker.is_some();
        if busy && ctx.input(|input| input.viewport().close_requested()) {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
        }
        egui::CentralPanel::default().show(ui, |ui| {
            ui.heading("Set up game data");
            ui.add_space(10.0);
            ui.label("Choose the folder containing your disc files, or a disc image.");
            ui.label("The game will start automatically when extraction is complete.");
            ui.add_space(12.0);
            ui.add_enabled_ui(!busy && self.completed_at.is_none(), |ui| {
                ui.label("Disc folder or image");
                ui.horizontal(|ui| {
                    ui.add(egui::TextEdit::singleline(&mut self.source)
                        .desired_width((ui.available_width() - 235.0).max(100.0)));
                    if ui.button("Choose image").clicked() {
                        let mut dialog = rfd::FileDialog::new()
                            .set_title("Choose game disc image")
                            .add_filter("Disc image", &["iso"]);
                        if let Some(folder) = Path::new(&self.source).parent().filter(|folder| folder.is_dir()) {
                            dialog = dialog.set_directory(folder);
                        }
                        if let Some(path) = dialog.pick_file() {
                            if let Some(path) = path.to_str() {
                                self.source = path.into();
                                self.error = None;
                            } else {
                                self.error = Some("This path cannot be displayed as Unicode. Please choose another image.".into());
                            }
                        }
                    }
                    if ui.button("Choose folder").clicked() {
                        let mut dialog = rfd::FileDialog::new().set_title("Choose game disc folder");
                        if Path::new(&self.source).is_dir() {
                            dialog = dialog.set_directory(&self.source);
                        }
                        if let Some(path) = dialog.pick_folder() {
                            if let Some(path) = path.to_str() {
                                self.source = path.into();
                                self.error = None;
                            } else {
                                self.error = Some("This path cannot be displayed as Unicode. Please choose another folder.".into());
                            }
                        }
                    }
                });
            });
            ui.add_space(12.0);
            let progress = self.progress.lock().unwrap();
            ui.add(egui::ProgressBar::new(progress.fraction).show_percentage());
            ui.label(if progress.message.is_empty() { "Ready to extract" } else { progress.message });
            drop(progress);
            if let Some(error) = &self.error {
                egui::ScrollArea::vertical().max_height(70.0).show(ui, |ui| {
                    ui.colored_label(ui.visuals().error_fg_color, error);
                });
            }
            ui.add_space(8.0);
            if ui.add_enabled(!busy && self.completed_at.is_none() && !self.source.is_empty(),
                egui::Button::new("Okay")).clicked() {
                self.start();
            }
            if busy {
                ui.small("Please keep this window open while the data is prepared.");
            }
        });
        if let Some(finished) = self.completed_at {
            // Paint the completed bar before returning to normal game startup.
            if finished.elapsed() >= Duration::from_millis(300) {
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
        }
        if busy || self.completed_at.is_some() {
            ctx.request_repaint_after(Duration::from_millis(100));
        }
    }
}

impl Drop for Setup {
    fn drop(&mut self) {
        // A graphics/event-loop error must not leave a detached publishing worker.
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

/// The Moon People's two moons, the setup window's icon in every game: a game's own icon is drawn from its
/// disc, which is what this window is extracting.
const ICON_PNG: &[u8] = include_bytes!("../../assets/two-moons-256.png");

fn icon() -> Option<egui::IconData> {
    let mut reader = png::Decoder::new(std::io::Cursor::new(ICON_PNG))
        .read_info()
        .ok()?;
    let mut rgba = vec![0; reader.output_buffer_size()?];
    let frame = reader.next_frame(&mut rgba).ok()?;
    if frame.color_type != png::ColorType::Rgba || frame.bit_depth != png::BitDepth::Eight {
        return None;
    }
    rgba.truncate(frame.buffer_size());
    Some(egui::IconData {
        rgba,
        width: frame.width,
        height: frame.height,
    })
}

/// Runs on the main thread and returns only after setup closes or succeeds.
pub fn run(title: &str, output: &Path) -> CoreResult<bool> {
    run_with_policy(title, output, ScriptPublicationPolicy::Retain)
}

/// Runs first-run setup with an explicit compiled-script publication policy.
pub fn run_with_policy(
    title: &str,
    output: &Path,
    script_policy: ScriptPublicationPolicy,
) -> CoreResult<bool> {
    let succeeded = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let app = Setup {
        source: String::new(),
        output: output.into(),
        script_policy,
        progress: Arc::new(Mutex::new(Progress::default())),
        worker: None,
        error: None,
        completed_at: None,
        succeeded: succeeded.clone(),
    };
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([640.0, 340.0])
            .with_min_inner_size([520.0, 340.0])
            .with_icon(icon().unwrap_or_default()),
        centered: true,
        run_and_return: true,
        ..Default::default()
    };
    eframe::run_native(title, options, Box::new(move |_| Ok(Box::new(app))))
        .map_err(|error| CoreError(format!("Could not open data setup: {error}")))?;
    Ok(succeeded.load(std::sync::atomic::Ordering::Acquire))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_setup_window_has_the_two_moons_icon() {
        let icon = icon().expect("the embedded icon decodes");
        assert_eq!((icon.width, icon.height), (256, 256));
        assert_eq!(icon.rgba.len(), 256 * 256 * 4);
    }

    fn test_setup(script_policy: ScriptPublicationPolicy) -> Setup {
        Setup {
            source: String::new(),
            output: PathBuf::from("unused/data"),
            script_policy,
            progress: Arc::new(Mutex::new(Progress::default())),
            worker: None,
            error: None,
            completed_at: None,
            succeeded: Arc::new(std::sync::atomic::AtomicBool::new(false)),
        }
    }

    #[test]
    fn invalid_path_stays_open_for_correction() {
        let mut app = test_setup(ScriptPublicationPolicy::Retain);
        app.start();
        assert!(app.worker.is_none());
        assert!(app.error.is_some());
        assert!(app.completed_at.is_none());
    }

    #[test]
    fn folder_holding_one_disc_image_stands_for_it() {
        let temp = tempfile::tempdir().expect("temporary folder");
        assert_eq!(lone_disc_image(temp.path()), None);
        let image = temp.path().join("Game (PAL).ISO");
        std::fs::write(&image, b"").expect("image");
        assert_eq!(lone_disc_image(temp.path()), Some(image));
        std::fs::write(temp.path().join("Other.iso"), b"").expect("second image");
        assert_eq!(lone_disc_image(temp.path()), None);
        std::fs::remove_file(temp.path().join("Other.iso")).expect("remove second image");
        std::fs::write(temp.path().join("data.dat"), b"").expect("disc file");
        assert_eq!(lone_disc_image(temp.path()), None);
    }

    #[test]
    fn worker_failure_does_not_launch_game_and_success_does() {
        let mut app = test_setup(ScriptPublicationPolicy::Retain);
        app.worker = Some(std::thread::spawn(|| Err(CoreError("invalid disc".into()))));
        while app.worker.is_some() {
            app.poll();
            std::thread::yield_now();
        }
        assert_eq!(app.error.as_deref(), Some("invalid disc"));
        assert!(app.completed_at.is_none());
        assert!(!app.succeeded.load(std::sync::atomic::Ordering::Acquire));
        app.error = None;
        app.worker = Some(std::thread::spawn(|| Ok(core::ExtractReport::default())));
        while app.worker.is_some() {
            app.poll();
            std::thread::yield_now();
        }
        assert!(app.completed_at.is_some());
        assert!(app.succeeded.load(std::sync::atomic::Ordering::Acquire));
    }

    #[test]
    fn setup_worker_publishes_using_the_selected_script_policy() {
        let temp = tempfile::tempdir().expect("temporary folder");
        let source = temp.path().join("disc");
        std::fs::create_dir_all(source.join("MOVIE")).expect("disc folder");

        let mut hd4 = vec![0u8; 54];
        hd4[0..4].copy_from_slice(&36u32.to_le_bytes());
        hd4[4..8].copy_from_slice(&4u32.to_le_bytes());
        hd4[12..16].copy_from_slice(&44u32.to_le_bytes());
        hd4[16..20].copy_from_slice(&4u32.to_le_bytes());
        hd4[20..24].copy_from_slice(&1u32.to_le_bytes());
        hd4[24..28].copy_from_slice(&53u32.to_le_bytes());
        hd4[36..44].copy_from_slice(b"FOO.STB\0");
        hd4[44..53].copy_from_slice(b"BAR.STB_\0");
        std::fs::write(source.join("DATA.HD4"), hd4).expect("data index");
        let mut data = vec![0u8; 2052];
        data[..4].copy_from_slice(b"DATA");
        data[2048..].copy_from_slice(b"MORE");
        std::fs::write(source.join("DATA.DAT"), data).expect("data archive");
        let mut hd3 = vec![0u8; 42];
        hd3[0..4].copy_from_slice(&32u32.to_le_bytes());
        hd3[4..8].copy_from_slice(&3u32.to_le_bytes());
        hd3[12..16].copy_from_slice(&1u32.to_le_bytes());
        hd3[32..42].copy_from_slice(b"BGM_A.STR\0");
        std::fs::write(source.join("SOUND.HD3"), hd3).expect("sound index");
        std::fs::write(source.join("SOUND.DAT"), b"SND").expect("sound archive");
        let source = std::fs::canonicalize(source).expect("canonical disc path");

        for (name, policy) in [
            ("retain", ScriptPublicationPolicy::Retain),
            ("luau-only", ScriptPublicationPolicy::LuauOnly),
        ] {
            let output_root = temp.path().join(name);
            let mut app = test_setup(policy);
            app.source = source.to_string_lossy().into_owned();
            app.output = output_root.join("data");
            app.start();
            assert!(app.error.is_none(), "worker starts: {:?}", app.error);

            let deadline = Instant::now() + Duration::from_secs(30);
            while app.worker.is_some() && Instant::now() < deadline {
                app.poll();
                std::thread::sleep(Duration::from_millis(5));
            }
            assert!(app.worker.is_none(), "extraction worker finishes");
            assert!(app.error.is_none(), "extraction succeeds: {:?}", app.error);
            assert!(app.succeeded.load(std::sync::atomic::Ordering::Acquire));

            let archive =
                core::zip_archive::Archive::open(&output_root.join(core::publish::ARCHIVE))
                    .expect("published archive");
            let names: Vec<_> = (0..archive.len())
                .map(|index| archive.name(index))
                .collect();
            match policy {
                ScriptPublicationPolicy::Retain => {
                    assert!(names.contains(&"foo.stb"));
                    assert!(names.contains(&"bar.stb_"));
                }
                ScriptPublicationPolicy::LuauOnly => assert!(names.iter().all(|name| {
                    let name = name.to_ascii_lowercase();
                    !name.ends_with(".stb") && !name.ends_with(".stb_")
                })),
            }
            let marker: serde_json::Value = serde_json::from_slice(
                &std::fs::read(output_root.join(core::publish::MARKER)).expect("policy marker"),
            )
            .expect("valid policy marker");
            let marker_policy = match policy {
                ScriptPublicationPolicy::Retain => "Retain",
                ScriptPublicationPolicy::LuauOnly => "LuauOnly",
            };
            assert_eq!(marker["script_publication_policy"], marker_policy);
        }
    }

    #[test]
    fn parallel_progress_never_regresses() {
        let mut progress = Progress::default();
        progress.advance(0.5, "Converting");
        progress.advance(0.2, "Extracting");
        assert_eq!(progress.fraction, 0.5);
        assert_eq!(progress.message, "Converting");
    }
}
