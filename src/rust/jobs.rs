//! Game-independent scheduling of independent asset classes onto one worker pool.
use super::{CoreError, CoreResult};

/// A unit of conversion work that belongs to one asset class.
pub trait ClassJob: Send {
    /// Index of the asset class, below the class count given to [`run_interleaved`].
    fn class(&self) -> usize;
    /// Estimated cost, used to start the largest jobs of each class first.
    fn work(&self) -> u64;
}

/// Orders jobs round-robin across classes, the largest job of each class first.
///
/// Every class has a job near the head of the queue, so independent classes run side by side
/// instead of one class draining before the next starts.
pub fn interleave<T: ClassJob>(jobs: Vec<T>, classes: usize) -> Vec<T> {
    let mut queues: Vec<Vec<T>> = (0..classes).map(|_| Vec::new()).collect();
    for job in jobs {
        let class = job.class();
        queues[class].push(job);
    }
    for queue in &mut queues {
        queue.sort_by_key(|job| job.work());
    }
    let mut ordered = Vec::with_capacity(queues.iter().map(Vec::len).sum());
    loop {
        let before = ordered.len();
        for queue in &mut queues {
            if let Some(job) = queue.pop() {
                ordered.push(job);
            }
        }
        if before == ordered.len() {
            return ordered;
        }
    }
}

/// Runs the jobs interleaved across classes on `pool`, one job per rayon task.
///
/// The first error stops further jobs from starting and is returned.
pub fn run_interleaved<T, F>(
    pool: &rayon::ThreadPool,
    jobs: Vec<T>,
    classes: usize,
    run: F,
) -> CoreResult<()>
where
    T: ClassJob,
    F: Fn(T) -> CoreResult<()> + Sync + Send,
{
    use rayon::prelude::*;
    pool.install(|| {
        interleave(jobs, classes)
            .into_par_iter()
            .with_max_len(1)
            .try_for_each(run)
    })
}

/// Builds a pool of `workers` threads whose stacks fit deep converter call paths.
pub fn worker_pool(
    workers: usize,
    name: &'static str,
    stack: usize,
) -> CoreResult<rayon::ThreadPool> {
    rayon::ThreadPoolBuilder::new()
        .num_threads(workers)
        .stack_size(stack)
        .thread_name(move |index| format!("{name}-worker-{index}"))
        .build()
        .map_err(|error| CoreError(format!("cannot start conversion workers: {error}")))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    struct Probe(usize, u64);

    impl ClassJob for Probe {
        fn class(&self) -> usize {
            self.0
        }
        fn work(&self) -> u64 {
            self.1
        }
    }

    #[test]
    fn interleaves_classes_largest_first() {
        let jobs = vec![
            Probe(0, 1),
            Probe(0, 9),
            Probe(1, 5),
            Probe(2, 2),
            Probe(1, 7),
        ];
        let order: Vec<(usize, u64)> = interleave(jobs, 3)
            .into_iter()
            .map(|job| (job.0, job.1))
            .collect();
        assert_eq!(order, [(0, 9), (1, 7), (2, 2), (0, 1), (1, 5)]);
    }

    #[test]
    fn stops_at_the_first_error() {
        let pool = worker_pool(1, "test", 1 << 20).unwrap();
        let seen = Mutex::new(Vec::new());
        let result = run_interleaved(
            &pool,
            vec![Probe(0, 3), Probe(0, 2), Probe(0, 1)],
            1,
            |job| {
                seen.lock().unwrap().push(job.1);
                if job.1 == 2 {
                    Err(CoreError("stop".into()))
                } else {
                    Ok(())
                }
            },
        );
        assert!(result.is_err());
        assert_eq!(*seen.lock().unwrap(), [3, 2]);
    }
}
