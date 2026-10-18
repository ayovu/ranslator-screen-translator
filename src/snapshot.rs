//! Independent, bounded work for the cards on a frozen screenshot.
use crate::{config::Config, ocr, process::Cancellation, provider};
use anyhow::{Result, bail};
use serde::Deserialize;
use serde_json::{Value, json};
use std::{
    collections::{HashMap, VecDeque},
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

#[derive(Clone, Debug, Deserialize, Eq, Hash, PartialEq)]
pub struct Key {
    pub session: String,
    pub id: u64,
}

#[derive(Clone, Debug, Deserialize)]
pub struct Request {
    #[serde(flatten)]
    pub key: Key,
    #[serde(default)]
    pub text: String,
    #[serde(default)]
    pub path: Option<PathBuf>,
    #[serde(default)]
    pub offset: [i32; 2],
    #[serde(default)]
    pub rect: [f64; 4],
    #[serde(default)]
    pub click: bool,
    #[serde(default)]
    pub source: String,
    pub target: String,
}

#[derive(Default)]
pub struct Queue {
    pending: VecDeque<(Config, Request)>,
    active: HashMap<Key, Cancellation>,
}

impl Queue {
    pub fn enqueue(&mut self, config: &Config, request: Request) -> Result<()> {
        if self.active.contains_key(&request.key)
            || self
                .pending
                .iter()
                .any(|(_, queued)| queued.key == request.key)
        {
            bail!("This translation is already queued.");
        }
        if self.pending.len() + self.active.len() >= 32 {
            bail!("The translation queue is full. Retry after another card finishes.");
        }
        self.pending.push_back((config.clone(), request));
        Ok(())
    }

    pub fn take_next(&mut self) -> Option<(Config, Request, Cancellation)> {
        // Bound both CLI processes and network work; new regions never replace older ones.
        if self.active.len() >= 2 {
            return None;
        }
        let (config, request) = self.pending.pop_front()?;
        let cancel = Arc::new(AtomicBool::new(false));
        self.active.insert(request.key.clone(), cancel.clone());
        Some((config, request, cancel))
    }

    pub fn accepts(&self, key: &Key) -> bool {
        self.active
            .get(key)
            .is_some_and(|c| !c.load(Ordering::Relaxed))
    }

    pub fn finish(&mut self, key: &Key) {
        self.active.remove(key);
    }

    pub fn cancel(&mut self, session: &str, id: Option<u64>) {
        let matches = |key: &Key| key.session == session && id.is_none_or(|id| key.id == id);
        self.pending.retain(|(_, r)| !matches(&r.key));
        for (key, cancel) in &self.active {
            if matches(key) {
                cancel.store(true, Ordering::Relaxed);
            }
        }
    }

    pub fn stop(&mut self) {
        self.pending.clear();
        for cancel in self.active.values() {
            cancel.store(true, Ordering::Relaxed);
        }
    }
}

fn selected_words(mut words: Vec<ocr::Word>, request: &Request) -> Vec<ocr::Word> {
    let [x, y, width, height] = request.rect;
    for word in &mut words {
        word.x += request.offset[0];
        word.y += request.offset[1];
    }
    words.retain(|word| {
        let (left, top) = (f64::from(word.x), f64::from(word.y));
        let (right, bottom) = (left + f64::from(word.width), top + f64::from(word.height));
        if request.click {
            x >= left - 3.0 && x <= right + 3.0 && y >= top - 3.0 && y <= bottom + 3.0
        } else {
            x < right && x + width > left && y < bottom && y + height > top
        }
    });
    words
}

pub fn run(
    config: &Config,
    request: &Request,
    cancel: &Cancellation,
    source_ready: impl FnOnce(Value),
) -> Result<Value> {
    if cancel.load(Ordering::Relaxed) {
        bail!("Cancelled");
    }
    let mut text = request.text.clone();
    if text.trim().is_empty() {
        let Some(path) = &request.path else {
            bail!("Select some text to translate.")
        };
        let words = selected_words(ocr::recognize(config, path, cancel)?, request);
        let mut line = String::new();
        for word in &words {
            if !text.is_empty() {
                text.push(if line == word.line { ' ' } else { '\n' });
            }
            text.push_str(&word.text);
            line.clone_from(&word.line);
        }
        if text.trim().is_empty() {
            bail!("No text found in this region. Select a larger area or change the OCR language.");
        }
        source_ready(json!({"phase":"source", "text":text, "words":words}));
    }
    if cancel.load(Ordering::Relaxed) {
        bail!("Cancelled");
    }
    let translated = provider::translate(
        config,
        &text,
        Some(&request.source),
        Some(&request.target),
        cancel,
    )?;
    Ok(
        json!({"phase":"translation", "text":translated.text, "source":translated.source,
        "target":translated.target, "provider":translated.provider}),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(session: &str, id: u64) -> Request {
        serde_json::from_value(json!({"session":session,"id":id,"text":"Hello","target":"auto"}))
            .unwrap()
    }

    #[test]
    fn regions_queue_without_replacing_and_complete_out_of_order() {
        let mut queue = Queue::default();
        for id in 1..=3 {
            queue.enqueue(&Config::default(), request("a", id)).unwrap();
        }
        let (_, first, _) = queue.take_next().unwrap();
        let (_, second, _) = queue.take_next().unwrap();
        assert!(queue.take_next().is_none());
        queue.finish(&second.key);
        assert!(queue.accepts(&first.key));
        let (_, third, _) = queue.take_next().unwrap();
        assert_eq!(third.key.id, 3);
        assert!(!queue.accepts(&second.key));
    }

    #[test]
    fn queue_bounds_work_and_can_accept_more_after_a_card_is_closed() {
        let mut queue = Queue::default();
        for id in 1..=32 {
            queue.enqueue(&Config::default(), request("a", id)).unwrap();
        }
        assert!(queue.enqueue(&Config::default(), request("a", 33)).is_err());
        queue.cancel("a", Some(12));
        assert!(queue.enqueue(&Config::default(), request("a", 33)).is_ok());
        assert!(queue.enqueue(&Config::default(), request("a", 1)).is_err());
        queue.stop();
        assert!(queue.take_next().is_none());
    }

    #[test]
    fn cancelled_region_does_not_start_ocr_or_a_provider() {
        let cancel = Arc::new(AtomicBool::new(true));
        let result = run(&Config::default(), &request("a", 1), &cancel, |_| {
            panic!("A cancelled region cannot publish source text")
        });
        assert_eq!(result.unwrap_err().to_string(), "Cancelled");
    }

    #[test]
    fn closing_card_or_session_cancels_only_matching_work_and_rejects_late_results() {
        let mut queue = Queue::default();
        for id in 1..=3 {
            queue.enqueue(&Config::default(), request("a", id)).unwrap();
        }
        let (_, first, cancel) = queue.take_next().unwrap();
        let (_, second, _) = queue.take_next().unwrap();
        queue.cancel("a", Some(first.key.id));
        assert!(cancel.load(Ordering::Relaxed));
        assert!(!queue.accepts(&first.key));
        assert!(queue.accepts(&second.key));
        queue.cancel("a", None);
        queue.enqueue(&Config::default(), request("b", 1)).unwrap();
        // A cancelled process still occupies its slot until it actually stops.
        assert!(queue.take_next().is_none());
        queue.finish(&first.key);
        let (_, next, _) = queue.take_next().unwrap();
        assert_eq!(next.key.session, "b");
        assert!(!queue.accepts(&second.key));
    }

    #[test]
    fn cropped_ocr_respects_original_selection_and_offsets() {
        let words = vec![
            ocr::Word {
                text: "inside".into(),
                x: 10,
                y: 20,
                width: 40,
                height: 15,
                line: "1".into(),
            },
            ocr::Word {
                text: "padding".into(),
                x: 70,
                y: 20,
                width: 40,
                height: 15,
                line: "1".into(),
            },
        ];
        let mut request = request("a", 1);
        request.offset = [100, 200];
        request.rect = [108.0, 218.0, 45.0, 20.0];
        let selected = selected_words(words.clone(), &request);
        assert_eq!(selected.len(), 1);
        assert_eq!(selected[0].x, 110);
        request.click = true;
        request.rect = [180.0, 225.0, 0.0, 0.0];
        assert_eq!(selected_words(words, &request)[0].text, "padding");
    }
}
