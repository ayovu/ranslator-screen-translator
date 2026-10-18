use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum ListenMode {
    #[default]
    Off,
    Once,
    Continuous,
}
#[derive(Default)]
pub struct SelectionState {
    pub mode: ListenMode,
    pending: Option<(String, Instant)>,
    last: String,
}
impl SelectionState {
    pub fn arm(&mut self, mode: ListenMode) {
        self.mode = mode;
        self.pending = None;
        self.last.clear();
    }
    pub fn offer(&mut self, text: String, now: Instant) {
        if self.mode == ListenMode::Off {
            return;
        }
        if text.trim().is_empty() {
            self.pending = None;
            return;
        }
        if text != self.last {
            self.pending = Some((text, now));
        }
    }
    pub fn deadline(&self, delay: Duration) -> Option<Instant> {
        self.pending.as_ref().map(|(_, at)| *at + delay)
    }
    pub fn settle(&mut self, now: Instant, delay: Duration) -> Option<String> {
        if self.deadline(delay).is_none_or(|at| now < at) {
            return None;
        }
        let (text, _) = self.pending.take()?;
        self.last = text.clone();
        if self.mode == ListenMode::Once {
            self.mode = ListenMode::Off;
        }
        Some(text)
    }
}
#[derive(Default)]
pub struct TapCounter {
    last: Option<Instant>,
    count: u8,
}
impl TapCounter {
    pub fn press(&mut self, now: Instant, required: u8, window: Duration) -> bool {
        if self
            .last
            .is_none_or(|last| now.duration_since(last) > window)
        {
            self.count = 0;
        }
        self.last = Some(now);
        self.count += 1;
        if self.count >= required {
            self.count = 0;
            self.last = None;
            true
        } else {
            false
        }
    }
}
/// KDE keystate v5 exposes modifier state only. Emit one chord on release, ignore holds.
#[derive(Default)]
pub struct ModifierChord {
    down: u8,
    peak: u8,
    started: Option<Instant>,
}
impl ModifierChord {
    pub fn update(&mut self, key: u32, pressed: bool, now: Instant) -> Option<String> {
        let bit = match key {
            3 => 1,
            4 => 2,
            5 => 4,
            6 => 8,
            7 => 16,
            _ => return None,
        };
        if pressed {
            if self.down == 0 {
                self.started = Some(now);
                self.peak = 0;
            }
            self.down |= bit;
            self.peak |= bit;
            None
        } else {
            self.down &= !bit;
            if self.down != 0 {
                return None;
            }
            let started = self.started.take()?;
            if now.duration_since(started) > Duration::from_millis(400) || self.peak & 16 != 0 {
                return None;
            }
            let mut keys = Vec::new();
            for (mask, name) in [(2, "Ctrl"), (1, "Alt"), (4, "Shift"), (8, "Meta")] {
                if self.peak & mask != 0 {
                    keys.push(name);
                }
            }
            Some(keys.join("+"))
        }
    }
}
pub fn canonical_modifier(sequence: &str) -> Option<String> {
    let parts: Vec<_> = sequence
        .split('+')
        .map(|s| s.trim().to_ascii_lowercase())
        .collect();
    if parts.is_empty()
        || parts
            .iter()
            .any(|s| !["ctrl", "control", "alt", "shift", "meta", "super"].contains(&s.as_str()))
    {
        return None;
    }
    Some(
        [
            ("Ctrl", ["ctrl", "control"]),
            ("Alt", ["alt", "alt"]),
            ("Shift", ["shift", "shift"]),
            ("Meta", ["meta", "super"]),
        ]
        .into_iter()
        .filter(|(_, names)| parts.iter().any(|s| names.contains(&s.as_str())))
        .map(|(name, _)| name)
        .collect::<Vec<_>>()
        .join("+"),
    )
}
