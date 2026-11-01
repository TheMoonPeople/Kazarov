//! Runs asset-class jobs written by the host program on the shared worker pool.
use super::core::jobs::{self, ClassJob};
use super::core::{CoreError, CoreResult};
use cxx::UniquePtr;
use ffi::ExtractJob;

// The C++ half stays out of unit tests, which link no C++ bridge.
#[cxx::bridge(namespace = "kazarov")]
mod ffi {
    unsafe extern "C++" {
        include!("kazarov/extract_job.hpp");

        /// One asset-class job implemented by the host; `run` reports failure by throwing.
        type ExtractJob;
        fn run(self: &ExtractJob) -> Result<()>;
    }

    extern "Rust" {
        /// Jobs of several asset classes that run together on one worker pool.
        type JobSet;
        fn new_job_set() -> Box<JobSet>;
        /// Queues a job of asset class `kind`; `work` orders the largest jobs of each class first.
        fn job_set_add(
            set: &mut JobSet,
            kind: u8,
            label: &str,
            work: u64,
            body: UniquePtr<ExtractJob>,
        );
        /// Runs the jobs on `workers` threads, interleaving classes; the first failure stops the rest.
        fn job_set_run(set: Box<JobSet>, workers: usize) -> Result<()>;
    }
}

// The host's job bodies only touch state it synchronises itself.
unsafe impl Send for ExtractJob {}

const WORKER_STACK: usize = 16 << 20;

struct HostJob {
    class: usize,
    work: u64,
    label: String,
    body: UniquePtr<ExtractJob>,
}

impl ClassJob for HostJob {
    fn class(&self) -> usize {
        self.class
    }

    fn work(&self) -> u64 {
        self.work
    }
}

/// Jobs collected by the host before they run together.
#[derive(Default)]
pub struct JobSet {
    jobs: Vec<HostJob>,
}

pub fn new_job_set() -> Box<JobSet> {
    Box::default()
}

pub fn job_set_add(
    set: &mut JobSet,
    kind: u8,
    label: &str,
    work: u64,
    body: UniquePtr<ExtractJob>,
) {
    set.jobs.push(HostJob {
        class: usize::from(kind),
        work,
        label: label.to_owned(),
        body,
    });
}

/// Runs every job on `workers` threads, interleaving classes; the first failure stops the rest.
// The host hands the set over through the bridge as the Box it owns, so the Box is the signature.
#[allow(clippy::boxed_local)]
pub fn job_set_run(set: Box<JobSet>, workers: usize) -> CoreResult<()> {
    if !(1..=1024).contains(&workers) {
        return Err(CoreError("workers must be between 1 and 1024".into()));
    }
    let classes = set.jobs.iter().map(|job| job.class + 1).max().unwrap_or(0);
    let pool = jobs::worker_pool(workers, "extract", WORKER_STACK)?;
    jobs::run_interleaved(&pool, set.jobs, classes, |job| {
        let body = job
            .body
            .as_ref()
            .ok_or_else(|| CoreError(format!("{}: missing job", job.label)))?;
        body.run()
            .map_err(|error| CoreError(format!("{}: {error}", job.label)))
    })
}
