//! First-run desktop setup, invoked before the host game creates its window.

use crate::core::{self, CoreError, CoreResult};
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
    progress: Arc<Mutex<Progress>>,
    worker: Option<JoinHandle<CoreResult<core::ExtractReport>>>,
    error: Option<String>,
    completed_at: Option<Instant>,
    succeeded: Arc<std::sync::atomic::AtomicBool>,
}

impl Setup {
    /// Starts one extraction without blocking the native event loop.
    fn start(&mut self) {
        let source = PathBuf::from(&self.source);
        if !source.is_dir() && !source.is_file() {
            self.error = Some("Choose an existing disc folder or enter a disc image path.".into());
            return;
        }
        self.error = None;
        *self.progress.lock().unwrap() = Progress::default();
        let progress = self.progress.clone();
        let output = self.output.clone();
        let jobs = std::thread::available_parallelism()
            .map_or(1, usize::from)
            .min(32);
        match std::thread::Builder::new()
            .name("game-data-extraction".into())
            .stack_size(16 * 1024 * 1024)
            .spawn(move || {
                core::extract_with_progress(&source, &output, jobs, &|fraction, message| {
                    progress.lock().unwrap().advance(fraction, message);
                })
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
            ui.label("Choose the folder containing your disc files, or enter a disc image path.");
            ui.label("The game will start automatically when extraction is complete.");
            ui.add_space(12.0);
            ui.add_enabled_ui(!busy && self.completed_at.is_none(), |ui| {
                ui.label("Disc folder or image");
                ui.horizontal(|ui| {
                    ui.add(egui::TextEdit::singleline(&mut self.source)
                        .desired_width((ui.available_width() - 125.0).max(100.0)));
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

/// Runs on the main thread and returns only after setup closes or succeeds.
pub fn run(title: &str, output: &Path) -> CoreResult<bool> {
    let succeeded = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let app = Setup {
        source: String::new(),
        output: output.into(),
        progress: Arc::new(Mutex::new(Progress::default())),
        worker: None,
        error: None,
        completed_at: None,
        succeeded: succeeded.clone(),
    };
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([640.0, 340.0])
            .with_min_inner_size([520.0, 340.0]),
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

    fn test_setup() -> Setup {
        Setup {
            source: String::new(),
            output: PathBuf::from("unused/data"),
            progress: Arc::new(Mutex::new(Progress::default())),
            worker: None,
            error: None,
            completed_at: None,
            succeeded: Arc::new(std::sync::atomic::AtomicBool::new(false)),
        }
    }

    #[test]
    fn invalid_path_stays_open_for_correction() {
        let mut app = test_setup();
        app.start();
        assert!(app.worker.is_none());
        assert!(app.error.is_some());
        assert!(app.completed_at.is_none());
    }

    #[test]
    fn worker_failure_does_not_launch_game_and_success_does() {
        let mut app = test_setup();
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
    fn parallel_progress_never_regresses() {
        let mut progress = Progress::default();
        progress.advance(0.5, "Converting");
        progress.advance(0.2, "Extracting");
        assert_eq!(progress.fraction, 0.5);
        assert_eq!(progress.message, "Converting");
    }
}
