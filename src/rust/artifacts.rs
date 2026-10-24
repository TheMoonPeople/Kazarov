//! Bounded in-memory asset submission to one private ZIP writer.
use super::archive::{ResourceRef, SourceSlice};
use super::progress::Phase;
use super::zip_archive::{self, PreparedEntry, StreamWriter, Written};
use super::{CoreError, CoreResult};
use std::collections::HashSet;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{SyncSender, sync_channel};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}

/// Final bytes produced by a converter or borrowed directly from the source image.
pub enum ArtifactBytes {
    Owned(Vec<u8>),
    SourceRange(ResourceRef),
}

impl ArtifactBytes {
    pub fn as_slice(&self) -> SourceSlice<'_> {
        match self {
            Self::Owned(bytes) => SourceSlice::Borrowed(bytes),
            Self::SourceRange(resource) => resource.bytes(),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Category {
    Texture,
    Model,
    Audio,
    Movie,
    Script,
    Pack,
    Preserved,
    Gameplay,
}

type JobRunner = Box<dyn FnOnce(&AssetSink) -> CoreResult<()> + Send>;

/// A conversion task admitted by byte estimate before its closure runs.
pub struct Job {
    pub category: Category,
    pub label: String,
    pub work: u64,
    pub estimated_bytes: usize,
    pub run: JobRunner,
}

impl Job {
    pub fn execute(self, sink: &AssetSink) -> CoreResult<()> {
        let _admission = sink
            .inner
            .conversion_gate
            .acquire(self.estimated_bytes)
            .map_err(|error| sink.first_failure().unwrap_or(error))?;
        let work = self.work;
        let started = Instant::now();
        let result = (self.run)(sink);
        let elapsed = started.elapsed();
        if elapsed >= Duration::from_secs(1) && std::env::var_os("DC2DATA_PROFILE_JOBS").is_some() {
            eprintln!(
                "dc2data job {:?} [{}]: {:.3}s",
                self.category,
                self.label,
                elapsed.as_secs_f64()
            );
        }
        if let Err(error) = result {
            let error = CoreError(format!("{:?} [{}]: {error}", self.category, self.label));
            sink.record_failure(&error);
            sink.cancel();
            return Err(error);
        }
        sink.complete(work)
            .map_err(|error| sink.first_failure().unwrap_or(error))
    }
}

struct GateState {
    used: usize,
    limit: usize,
}

struct MemoryGate {
    state: Mutex<GateState>,
    available: Condvar,
    cancelled: AtomicBool,
}

impl MemoryGate {
    fn new(limit: usize) -> Self {
        Self {
            state: Mutex::new(GateState {
                used: 0,
                limit: limit.max(1),
            }),
            available: Condvar::new(),
            cancelled: AtomicBool::new(false),
        }
    }

    fn acquire(self: &Arc<Self>, estimate: usize) -> CoreResult<MemoryPermit> {
        let mut state = self
            .state
            .lock()
            .map_err(|_| fail("asset memory gate poisoned"))?;
        loop {
            if self.cancelled.load(Ordering::Acquire) {
                return Err(fail("asset submission cancelled"));
            }
            let weight = estimate.clamp(1, state.limit);
            if state.used <= state.limit - weight {
                state.used += weight;
                return Ok(MemoryPermit {
                    gate: self.clone(),
                    weight,
                });
            }
            state = self
                .available
                .wait(state)
                .map_err(|_| fail("asset memory gate poisoned"))?;
        }
    }

    fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
        self.available.notify_all();
    }

    fn limit(&self) -> CoreResult<usize> {
        Ok(self
            .state
            .lock()
            .map_err(|_| fail("asset memory gate poisoned"))?
            .limit)
    }

    fn raise_limit(&self, new_limit: usize) -> CoreResult<usize> {
        let mut state = self
            .state
            .lock()
            .map_err(|_| fail("asset memory gate poisoned"))?;
        if new_limit > state.limit {
            state.limit = new_limit;
            self.available.notify_all();
        }
        Ok(state.limit)
    }
}

struct MemoryPermit {
    gate: Arc<MemoryGate>,
    weight: usize,
}

impl Drop for MemoryPermit {
    fn drop(&mut self) {
        if let Ok(mut state) = self.gate.state.lock() {
            state.used -= self.weight;
            self.gate.available.notify_all();
        }
    }
}

enum Message {
    Entry(PreparedEntry, MemoryPermit, Category),
    Completed(u64),
    Finish,
    Abort,
}

struct SinkInner {
    sender: SyncSender<Message>,
    writer: Mutex<Option<JoinHandle<CoreResult<Written>>>>,
    names: Mutex<HashSet<String>>,
    conversion_gate: Arc<MemoryGate>,
    queue_gate: Arc<MemoryGate>,
    first_failure: Arc<Mutex<Option<String>>>,
}

/// Clonable producer handle for a single bounded, unpublished ZIP stream.
#[derive(Clone)]
pub struct AssetSink {
    inner: Arc<SinkInner>,
}

impl AssetSink {
    pub fn conversion_limit(&self) -> CoreResult<usize> {
        self.inner.conversion_gate.limit()
    }

    pub fn queue_limit(&self) -> CoreResult<usize> {
        self.inner.queue_gate.limit()
    }

    /// Lets remaining conversion jobs use available memory after other producers finish.
    pub fn raise_conversion_limit(&self, new_limit: usize) -> CoreResult<usize> {
        self.inner.conversion_gate.raise_limit(new_limit)
    }

    /// Runs planning work under the same byte admission used by conversion jobs.
    pub(crate) fn with_admission<T>(
        &self,
        estimated_bytes: usize,
        body: impl FnOnce() -> CoreResult<T>,
    ) -> CoreResult<T> {
        let _admission = self.inner.conversion_gate.acquire(estimated_bytes)?;
        let result = body();
        if let Err(error) = &result {
            self.record_failure(error);
            self.cancel();
        }
        result
    }

    pub fn start(stage: &Path, budget_bytes: usize) -> CoreResult<Self> {
        Self::start_with_progress(stage, budget_bytes, Phase::noop(), 0)
    }

    pub fn start_with_progress(
        stage: &Path,
        budget_bytes: usize,
        phase: Phase,
        total_work: u64,
    ) -> CoreResult<Self> {
        std::fs::create_dir_all(stage)?;
        let writer = StreamWriter::new(&stage.join("data.zip"))?;
        if total_work != 0 {
            phase.set_total(total_work);
        }
        let (sender, receiver) = sync_channel::<Message>(64);
        let queue_gate = Arc::new(MemoryGate::new(budget_bytes));
        let conversion_gate = Arc::new(MemoryGate::new(budget_bytes));
        let worker_gate = queue_gate.clone();
        let worker_conversion_gate = conversion_gate.clone();
        let first_failure = Arc::new(Mutex::new(None));
        let worker_failure = first_failure.clone();
        let handle = std::thread::Builder::new()
            .name("dc2data-zip-writer".into())
            .spawn(move || {
                let result = (|| -> CoreResult<Written> {
                    let mut writer = writer;
                    while let Ok(message) = receiver.recv() {
                        match message {
                            Message::Entry(entry, _permit, _category) => {
                                phase.add_bytes(writer.append(entry)?);
                            }
                            Message::Completed(work) => phase.inc(work),
                            Message::Finish => {
                                let written = writer.finish()?;
                                return Ok(written);
                            }
                            Message::Abort => return Err(fail("ZIP stream aborted")),
                        }
                    }
                    Err(fail("ZIP writer stopped before finish"))
                })();
                if let Err(error) = &result {
                    if let Ok(mut first) = worker_failure.lock() {
                        first.get_or_insert_with(|| error.to_string());
                    }
                    worker_gate.cancel();
                    worker_conversion_gate.cancel();
                }
                result
            })?;
        Ok(Self {
            inner: Arc::new(SinkInner {
                sender,
                writer: Mutex::new(Some(handle)),
                names: Mutex::new(HashSet::new()),
                conversion_gate,
                queue_gate,
                first_failure,
            }),
        })
    }

    fn record_failure(&self, error: &CoreError) {
        if error.0 == "asset submission cancelled" {
            return;
        }
        if let Ok(mut first) = self.inner.first_failure.lock() {
            first.get_or_insert_with(|| error.to_string());
        }
    }

    /// First underlying producer or writer failure, retained across parallel cancellation.
    pub fn first_failure(&self) -> Option<CoreError> {
        self.inner
            .first_failure
            .lock()
            .ok()
            .and_then(|failure| failure.clone().map(CoreError))
    }

    pub fn submit(
        &self,
        name: impl Into<String>,
        bytes: ArtifactBytes,
        category: Category,
    ) -> CoreResult<()> {
        let name = name.into();
        if category == Category::Gameplay && super::configs::gameplay_digest(&name).is_none() {
            let error = fail(format!(
                "{category:?} [{name}]: gameplay document is not tracked"
            ));
            self.record_failure(&error);
            self.cancel();
            return Err(error);
        }
        zip_archive::validate_name(&name)?;
        let lower = name.to_ascii_lowercase();
        if category != Category::Gameplay
            && (lower.starts_with("gameplay/")
                || super::configs::is_tracked_gameplay(&lower)
                || lower.ends_with(".cfg.json")
                || lower.ends_with(".map.json")
                || lower.ends_with(".character.json")
                || lower.ends_with(".sound/config.json")
                || lower.ends_with(".sound/volume.json")
                || matches!(lower.as_str(), "snd2/rev.json" | "snd2/map_index.json"))
        {
            let error = fail(format!(
                "{category:?} [{name}]: gameplay config requires the Gameplay category"
            ));
            self.record_failure(&error);
            self.cancel();
            return Err(error);
        }
        {
            let mut names = self
                .inner
                .names
                .lock()
                .map_err(|_| fail("asset names lock poisoned"))?;
            if !names.insert(lower) {
                let error = fail(format!(
                    "{category:?} [{name}]: archive entry names collide ignoring case"
                ));
                self.record_failure(&error);
                self.cancel();
                return Err(error);
            }
        }
        let estimate = bytes
            .as_slice()
            .len()
            .saturating_mul(2)
            .saturating_add(1 << 16);
        let permit = self.inner.queue_gate.acquire(estimate)?;
        let entry_name = name.clone();
        let prepared = match zip_archive::prepare_artifact(name, bytes) {
            Ok(prepared) => prepared,
            Err(error) => {
                self.record_failure(&fail(format!("{category:?} [{entry_name}]: {error}")));
                self.cancel();
                return Err(error);
            }
        };
        self.inner
            .sender
            .send(Message::Entry(prepared, permit, category))
            .map_err(|_| {
                self.cancel();
                fail("ZIP writer stopped during submission")
            })
    }

    pub fn cancel(&self) {
        self.inner.conversion_gate.cancel();
        self.inner.queue_gate.cancel();
    }

    pub fn complete(&self, work: u64) -> CoreResult<()> {
        self.inner
            .sender
            .send(Message::Completed(work))
            .map_err(|_| {
                self.cancel();
                fail("ZIP writer stopped before completing a job")
            })
    }

    pub fn finish(self) -> CoreResult<Written> {
        self.inner
            .sender
            .send(Message::Finish)
            .map_err(|_| fail("ZIP writer stopped before finish"))?;
        let handle = self
            .inner
            .writer
            .lock()
            .map_err(|_| fail("ZIP writer lock poisoned"))?
            .take()
            .ok_or_else(|| fail("ZIP writer already finished"))?;
        handle.join().map_err(|_| fail("ZIP writer panicked"))?
    }

    /// Stops a failed conversion and waits until the unpublished writer releases its file.
    pub fn abort(self) -> CoreResult<()> {
        self.cancel();
        let _ = self.inner.sender.send(Message::Abort);
        if let Some(handle) = self
            .inner
            .writer
            .lock()
            .map_err(|_| fail("ZIP writer lock poisoned"))?
            .take()
        {
            let _ = handle.join().map_err(|_| fail("ZIP writer panicked"))?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::MemoryGate;
    use std::sync::Arc;
    use std::sync::mpsc::channel;
    use std::time::Duration;

    #[test]
    fn raising_conversion_limit_wakes_a_waiting_job() {
        let gate = Arc::new(MemoryGate::new(10));
        let first = gate.acquire(10).unwrap();
        let (sender, receiver) = channel();
        let waiting = gate.clone();
        let worker = std::thread::spawn(move || {
            let _permit = waiting.acquire(10).unwrap();
            sender.send(()).unwrap();
        });
        assert!(receiver.recv_timeout(Duration::from_millis(50)).is_err());
        assert_eq!(gate.raise_limit(20).unwrap(), 20);
        receiver.recv_timeout(Duration::from_secs(2)).unwrap();
        worker.join().unwrap();
        drop(first);
    }
}
