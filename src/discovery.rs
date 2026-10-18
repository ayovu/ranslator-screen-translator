//! Bounded model discovery: keep the newest pending lookup when the user changes engines.
use crate::config::Config;
use std::collections::HashSet;

#[derive(Default)]
pub struct Queue {
    active: HashSet<u64>,
    pending: Option<(u64, Config)>,
    latest: Option<u64>,
}
impl Queue {
    pub fn submit(&mut self, id: u64, config: Config) {
        self.latest = Some(id);
        self.pending = Some((id, config));
    }
    pub fn take_next(&mut self) -> Option<(u64, Config)> {
        if self.active.len() >= 2 {
            return None;
        }
        let next = self.pending.take()?;
        self.active.insert(next.0);
        Some(next)
    }
    pub fn finish(&mut self, id: u64) -> bool {
        self.active.remove(&id) && self.latest == Some(id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn switching_engines_runs_without_waiting_and_ignores_old_results() {
        let mut queue = Queue::default();
        queue.submit(
            1,
            Config {
                provider: "cli".into(),
                ..Config::default()
            },
        );
        assert_eq!(queue.take_next().unwrap().1.provider, "cli");
        queue.submit(
            2,
            Config {
                provider: "ollama".into(),
                ..Config::default()
            },
        );
        assert_eq!(queue.take_next().unwrap().1.provider, "ollama");
        assert!(queue.finish(2));
        assert!(!queue.finish(1));
    }
    #[test]
    fn rapid_changes_keep_only_the_latest_pending_request() {
        let mut queue = Queue::default();
        for id in 1..=2 {
            queue.submit(id, Config::default());
            assert!(queue.take_next().is_some());
        }
        for id in 3..=100 {
            queue.submit(id, Config::default());
            assert!(queue.take_next().is_none());
        }
        assert!(!queue.finish(1));
        assert_eq!(queue.take_next().unwrap().0, 100);
        assert!(!queue.finish(2));
        assert!(queue.finish(100));
        assert!(queue.take_next().is_none());
    }
}
