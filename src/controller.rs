use ranslator::{
    config::Config,
    ocr, packs,
    process::Cancellation,
    provider, snapshot,
    state::{ListenMode, SelectionState, TapCounter, canonical_modifier},
};
use serde_json::{Value, json};
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Receiver, Sender},
    },
    time::{Duration, Instant},
};

pub enum Event {
    Ui(Value),
    Selection(u64, Result<String, String>),
    Modifier(String),
    Capability(String),
    Completed(u64, Value),
    OcrCompleted(u64, Value),
    RegionUpdate(snapshot::Key, Value),
    RegionCompleted(snapshot::Key, Value),
    Models(u64, Result<Vec<String>, String>),
    Packs(u64, Result<(), String>),
}
#[derive(Clone)]
enum Work {
    Translation {
        text: String,
        source: String,
        target: String,
        request_id: String,
    },
    Ocr(String),
}
struct Jobs {
    generation: u64,
    active: bool,
    cancel: Cancellation,
    pending: Option<(u64, Config, Work)>,
    tx: Sender<Event>,
    ocr: bool,
}
impl Jobs {
    fn stop(&mut self) {
        self.generation += 1;
        self.cancel.store(true, Ordering::Relaxed);
        self.pending = None;
    }
    fn submit(&mut self, config: &Config, work: Work) {
        self.stop();
        self.pending = Some((self.generation, config.clone(), work));
        crate::emit(
            json!({"type":"busy", "text": if self.active {"Waiting for previous request to stop…"} else {"Working…"}}),
        );
        self.dispatch();
    }
    fn dispatch(&mut self) {
        if self.active {
            return;
        }
        let Some((id, config, work)) = self.pending.take() else {
            return;
        };
        self.active = true;
        self.cancel = Arc::new(AtomicBool::new(false));
        let cancel = self.cancel.clone();
        let tx = self.tx.clone();
        let is_ocr = self.ocr;
        std::thread::spawn(move || {
            let request_id = match &work {
                Work::Translation { request_id, .. } => Some(request_id.clone()),
                Work::Ocr(_) => None,
            };
            let result = match work {
                Work::Translation { text, source, target, .. } => provider::translate(&config, &text, Some(&source), Some(&target), &cancel).map(|r| json!({"type":"translation", "text":r.text, "source":r.source, "target":r.target, "provider":r.provider})),
                Work::Ocr(path) => ocr::recognize(&config, std::path::Path::new(&path), &cancel).map(|words| json!({"type":"ocr", "path":path, "words":words})),
            };
            let mut value =
                result.unwrap_or_else(|e| json!({"type":"error", "text":e.to_string()}));
            if let Some(request_id) = request_id {
                value["request_id"] = json!(request_id);
            }
            let _ = tx.send(if is_ocr {
                Event::OcrCompleted(id, value)
            } else {
                Event::Completed(id, value)
            });
        });
    }
}
struct App {
    config: Config,
    resolved: Config,
    selection: SelectionState,
    listen_generation: u64,
    accessibility_selection: bool,
    #[cfg(target_os = "linux")]
    watcher: Option<ranslator::platform::linux::SelectionWatch>,
    focus: bool,
    taps: [TapCounter; 3],
    shortcut_recording: bool,
    shortcut_resume_at: Option<Instant>,
    jobs: Jobs,
    ocr_jobs: Jobs,
    regions: snapshot::Queue,
    snapshot_session: Option<String>,
    tx: Sender<Event>,
    models: ranslator::discovery::Queue,
    packs_busy: bool,
    packs_cancel: Cancellation,
}
impl App {
    fn dispatch_models(&mut self) {
        while let Some((id, config)) = self.models.take_next() {
            let tx = self.tx.clone();
            std::thread::spawn(move || {
                let result = provider::models(&config).map_err(|e| e.to_string());
                let _ = tx.send(Event::Models(id, result));
            });
        }
    }
    fn dispatch_regions(&mut self) {
        while let Some((config, request, cancel)) = self.regions.take_next() {
            let tx = self.tx.clone();
            std::thread::spawn(move || {
                let key = request.key.clone();
                let result = snapshot::run(&config, &request, &cancel, |value| {
                    let _ = tx.send(Event::RegionUpdate(key.clone(), value));
                });
                let value =
                    result.unwrap_or_else(|e| json!({"phase":"error", "text":e.to_string()}));
                let _ = tx.send(Event::RegionCompleted(key, value));
            });
        }
    }
    fn region_update(&self, key: &snapshot::Key, mut value: Value) {
        if self.regions.accepts(key) && self.snapshot_session.as_deref() == Some(&key.session) {
            value["type"] = json!("snapshot_region");
            value["session"] = json!(key.session);
            value["id"] = json!(key.id);
            crate::emit(value);
        }
    }
    fn provider_label(&self) {
        crate::emit(
            json!({"type":"provider", "text":format!("{}{}", self.resolved.provider,
            if self.resolved.model.is_empty() { String::new() } else { format!(" · {}", self.resolved.model) })}),
        );
    }
    fn arm(&mut self, mode: ListenMode) {
        self.listen_generation += 1;
        self.selection.arm(mode);
        self.accessibility_selection = cfg!(target_os = "macos");
        #[cfg(target_os = "linux")]
        {
            self.watcher = None;
        }
        if mode != ListenMode::Off {
            #[cfg(target_os = "linux")]
            if std::env::var_os("WAYLAND_DISPLAY").is_some() {
                let tx = self.tx.clone();
                let id = self.listen_generation;
                match ranslator::platform::linux::watch_selection(
                    self.config.watch_clipboard,
                    move |result| {
                        let _ = tx.send(Event::Selection(id, result.map_err(|e| e.to_string())));
                    },
                ) {
                    Ok(watcher) => self.watcher = Some(watcher),
                    Err(_) => {
                        // GNOME and other compositors may not expose data-control. Read
                        // selected text through the application's accessibility API instead.
                        self.accessibility_selection = true;
                    }
                }
            }
            crate::emit(json!({"type":"hide"}));
        }
        self.listening();
    }
    fn listening(&self) {
        crate::emit(
            json!({"type":"listening", "generation":self.listen_generation,
                "accessibility":self.accessibility_selection,
                "mode":match self.selection.mode { ListenMode::Off=>"off", ListenMode::Once=>"once", ListenMode::Continuous=>"continuous" }}),
        );
    }
    fn selection_event(&mut self, id: u64, result: Result<String, String>) {
        if id != self.listen_generation || self.selection.mode == ListenMode::Off {
            return;
        }
        match result {
            Ok(text) if !self.focus => self.selection.offer(text, Instant::now()),
            Ok(_) => (),
            Err(_) => {
                #[cfg(target_os = "linux")]
                {
                    self.watcher = None;
                }
                self.listen_generation += 1;
                self.selection.arm(self.selection.mode);
                self.accessibility_selection = true;
                self.listening();
            }
        }
    }
    fn action(&mut self, action: &str) {
        match action {
            "once" => self.arm(if self.selection.mode == ListenMode::Once {
                ListenMode::Off
            } else {
                ListenMode::Once
            }),
            "continuous" => self.arm(if self.selection.mode == ListenMode::Continuous {
                ListenMode::Off
            } else {
                ListenMode::Continuous
            }),
            "snapshot" => {
                self.arm(ListenMode::Off);
                self.jobs.stop();
                self.ocr_jobs.stop();
                self.regions.stop();
                crate::emit(json!({"type":"capture"}));
            }
            _ => (),
        }
    }
    fn selected(&mut self, text: String) {
        let request_id = format!("selection-{}", self.jobs.generation + 1);
        crate::emit(json!({"type":"selected", "text":text, "request_id":request_id}));
        self.jobs.submit(
            &self.resolved,
            Work::Translation {
                text,
                source: "auto".into(),
                target: "auto".into(),
                request_id,
            },
        );
    }
    fn set_shortcut_recording(&mut self, active: bool) {
        if self.shortcut_recording == active {
            return;
        }
        self.shortcut_recording = active;
        self.taps = Default::default();
        // D-Bus/native activation can arrive after the recorder receives the key release.
        self.shortcut_resume_at = (!active).then(|| Instant::now() + Duration::from_millis(250));
    }
    fn modifier(&mut self, chord: &str) {
        // Shift/Ctrl belong to the selection gesture while a snapshot is open.
        if self.snapshot_session.is_some() {
            return;
        }
        let action = [
            ("once", &self.config.once),
            ("continuous", &self.config.continuous),
            ("snapshot", &self.config.snapshot),
        ]
        .into_iter()
        .find(|(_, binding)| canonical_modifier(&binding.sequence).as_deref() == Some(chord))
        .map(|(action, _)| action);
        if let Some(action) = action {
            self.shortcut(action);
        }
    }
    fn shortcut(&mut self, action: &str) {
        if self.shortcut_recording
            || self
                .shortcut_resume_at
                .is_some_and(|resume| Instant::now() < resume)
        {
            return;
        }
        let (index, binding) = match action {
            "once" => (0, &self.config.once),
            "continuous" => (1, &self.config.continuous),
            "snapshot" => (2, &self.config.snapshot),
            _ => return,
        };
        // Modifier releases belong to the snapshot's multi-selection gesture on every
        // backend (X11/macOS/companion as well as KWin's separate modifier channel).
        if self.snapshot_session.is_some() && canonical_modifier(&binding.sequence).is_some() {
            return;
        }
        if self.taps[index].press(
            Instant::now(),
            binding.taps,
            Duration::from_millis(self.config.tap_window_ms),
        ) {
            self.action(action);
        }
    }
}
fn new_app(config: Config, tx: Sender<Event>) -> App {
    App {
        resolved: config.clone(),
        config,
        selection: Default::default(),
        listen_generation: 0,
        accessibility_selection: false,
        #[cfg(target_os = "linux")]
        watcher: None,
        focus: false,
        taps: Default::default(),
        shortcut_recording: false,
        shortcut_resume_at: None,
        jobs: Jobs {
            generation: 0,
            active: false,
            cancel: Arc::new(AtomicBool::new(false)),
            pending: None,
            tx: tx.clone(),
            ocr: false,
        },
        ocr_jobs: Jobs {
            generation: 0,
            active: false,
            cancel: Arc::new(AtomicBool::new(false)),
            pending: None,
            tx: tx.clone(),
            ocr: true,
        },
        tx: tx.clone(),
        regions: Default::default(),
        snapshot_session: None,
        models: Default::default(),
        packs_busy: false,
        packs_cancel: Arc::new(AtomicBool::new(false)),
    }
}

pub fn run(config: Config, tx: Sender<Event>, rx: Receiver<Event>) {
    let mut app = new_app(config, tx.clone());
    loop {
        let delay = Duration::from_millis(app.config.selection_delay_ms);
        let event = if let Some(deadline) = app.selection.deadline(delay) {
            match rx.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
                Ok(e) => Some(e),
                Err(mpsc::RecvTimeoutError::Timeout) => None,
                Err(_) => break,
            }
        } else {
            match rx.recv() {
                Ok(e) => Some(e),
                Err(_) => break,
            }
        };
        if let Some(event) = event {
            match event {
                Event::Ui(value) => {
                    let kind = value["type"].as_str().unwrap_or("");
                    match kind {
                        "ready" => {
                            app.resolved = provider::resolve_auto(&app.config);
                            app.provider_label();
                            #[cfg(target_os = "linux")]
                            if std::env::var_os("WAYLAND_DISPLAY").is_some() {
                                let keys = tx.clone();
                                let errors = tx.clone();
                                ranslator::platform::linux::watch_modifiers(
                                    move |chord| {
                                        let _ = keys.send(Event::Modifier(chord));
                                    },
                                    move |message| {
                                        let _ = errors.send(Event::Capability(message));
                                    },
                                );
                            }
                        }
                        "quit" => {
                            app.jobs.stop();
                            app.ocr_jobs.stop();
                            app.regions.stop();
                            app.packs_cancel.store(true, Ordering::Relaxed);
                            break;
                        }
                        "action" => app.action(value["action"].as_str().unwrap_or("")),
                        "shortcut" => app.shortcut(value["action"].as_str().unwrap_or("")),
                        "shortcut_recording" => {
                            app.set_shortcut_recording(value["active"].as_bool().unwrap_or(false));
                        }
                        "focus" => app.focus = value["active"].as_bool().unwrap_or(false),
                        "list_runtimes" => {
                            let mut found = ranslator::runtimes::discover();
                            found["type"] = json!("runtimes");
                            crate::emit(found);
                        }
                        "list_models" => {
                            let id = value["id"].as_u64().unwrap_or(0);
                            if let Ok(config) =
                                serde_json::from_value::<Config>(value["config"].clone())
                            {
                                app.models.submit(id, config);
                                app.dispatch_models();
                            } else {
                                crate::emit(
                                    json!({"type":"models", "id":id, "error":"Model lookup settings are invalid."}),
                                );
                            }
                        }
                        "list_packs" => {
                            let mut candidate = app.config.clone();
                            candidate.tessdata_dir =
                                value["directory"].as_str().unwrap_or("").into();
                            crate::emit(
                                json!({"type":"packs_list", "id":value["id"], "packs":packs::list(&candidate)}),
                            );
                        }
                        "cancel_packs" => app.packs_cancel.store(true, Ordering::Relaxed),
                        "install_packs" => {
                            let id = value["id"].as_u64().unwrap_or(0);
                            if app.packs_busy {
                                crate::emit(
                                    json!({"type":"packs_done", "id":id, "error":"A language download is still stopping. Try again in a moment."}),
                                );
                            } else if let Ok(codes) =
                                serde_json::from_value::<Vec<String>>(value["codes"].clone())
                            {
                                app.packs_busy = true;
                                app.packs_cancel = Arc::new(AtomicBool::new(false));
                                let cancel = app.packs_cancel.clone();
                                let tx = tx.clone();
                                std::thread::spawn(move || {
                                    let result = packs::install(&codes, &cancel, |text| {
                                        crate::emit(
                                            json!({"type":"packs_progress", "id":id, "text":text}),
                                        )
                                    })
                                    .map_err(|e| e.to_string());
                                    let _ = tx.send(Event::Packs(id, result));
                                });
                            }
                        }
                        "selection" => {
                            if !app.focus
                                && value["generation"]
                                    .as_u64()
                                    .is_none_or(|id| id == app.listen_generation)
                            {
                                app.selection.offer(
                                    value["text"].as_str().unwrap_or("").into(),
                                    Instant::now(),
                                );
                            }
                        }
                        "selection_unavailable" => {
                            if value["generation"].as_u64() == Some(app.listen_generation)
                                && app.selection.mode != ListenMode::Off
                            {
                                app.arm(ListenMode::Off);
                                crate::emit(json!({"type":"error", "text": value["text"]}));
                            }
                        }
                        "translate" => app.jobs.submit(
                            &app.resolved,
                            Work::Translation {
                                text: value["text"].as_str().unwrap_or("").into(),
                                source: value["source"].as_str().unwrap_or("auto").into(),
                                target: value["target"].as_str().unwrap_or("auto").into(),
                                request_id: value["request_id"].as_str().unwrap_or("").into(),
                            },
                        ),
                        "ocr" => app.ocr_jobs.submit(
                            &app.config,
                            Work::Ocr(value["path"].as_str().unwrap_or("").into()),
                        ),
                        "ocr_selected" => app.selected(value["text"].as_str().unwrap_or("").into()),
                        "snapshot_start" => {
                            app.regions.stop();
                            app.snapshot_session = value["session"].as_str().map(str::to_owned);
                        }
                        "snapshot_end" => {
                            let session = value["session"].as_str().unwrap_or("");
                            app.regions.cancel(session, None);
                            if app.snapshot_session.as_deref() == Some(session) {
                                app.snapshot_session = None;
                                app.ocr_jobs.stop();
                            }
                        }
                        "snapshot_cancel_region" => {
                            app.regions.cancel(
                                value["session"].as_str().unwrap_or(""),
                                value["id"].as_u64(),
                            );
                        }
                        "snapshot_request" => {
                            if let Ok(request) = serde_json::from_value::<snapshot::Request>(value)
                            {
                                if app.snapshot_session.as_deref() == Some(&request.key.session) {
                                    let key = request.key.clone();
                                    if let Err(error) = app.regions.enqueue(&app.resolved, request)
                                    {
                                        crate::emit(
                                            json!({"type":"snapshot_region", "session":key.session,
                                            "id":key.id,"phase":"error","text":error.to_string()}),
                                        );
                                    }
                                    app.dispatch_regions();
                                }
                            }
                        }
                        "dismiss_popup" => app.jobs.stop(),
                        "cancel" => {
                            app.jobs.stop();
                            app.ocr_jobs.stop();
                            app.regions.stop();
                            app.arm(ListenMode::Off);
                            crate::emit(json!({"type":"cancelled"}));
                        }
                        "save_config" => {
                            let result = serde_json::from_value::<Config>(value["config"].clone())
                                .map_err(anyhow::Error::from)
                                .and_then(|config| {
                                    config.save()?;
                                    Ok(config)
                                });
                            match result {
                                Ok(config) => {
                                    app.arm(ListenMode::Off);
                                    app.jobs.stop();
                                    app.ocr_jobs.stop();
                                    app.regions.stop();
                                    app.taps = Default::default();
                                    app.config = config;
                                    app.resolved = provider::resolve_auto(&app.config);
                                    crate::emit(
                                        json!({"type":"config_saved", "config":app.config}),
                                    );
                                    app.provider_label();
                                }
                                Err(e) => crate::emit(
                                    json!({"type":"config_error", "text":e.to_string()}),
                                ),
                            }
                        }
                        _ => (),
                    }
                }
                Event::Modifier(chord) => app.modifier(&chord),
                Event::Selection(id, result) => app.selection_event(id, result),
                Event::Capability(message) => {
                    crate::emit(json!({"type":"capability", "text":message}))
                }
                Event::Completed(id, value) => {
                    app.jobs.active = false;
                    if id == app.jobs.generation {
                        crate::emit(value);
                    }
                    app.jobs.dispatch();
                }
                Event::OcrCompleted(id, value) => {
                    app.ocr_jobs.active = false;
                    if id == app.ocr_jobs.generation {
                        crate::emit(value);
                    }
                    app.ocr_jobs.dispatch();
                }
                Event::RegionUpdate(key, value) => app.region_update(&key, value),
                Event::RegionCompleted(key, value) => {
                    app.region_update(&key, value);
                    app.regions.finish(&key);
                    app.dispatch_regions();
                }
                Event::Models(id, result) => {
                    if app.models.finish(id) {
                        crate::emit(match result {
                            Ok(models) => json!({"type":"models", "id":id, "models":models}),
                            Err(error) => json!({"type":"models", "id":id, "error":error}),
                        });
                    }
                    app.dispatch_models();
                }
                Event::Packs(id, result) => {
                    app.packs_busy = false;
                    crate::emit(json!({"type":"packs_done", "id":id, "error":result.err()}));
                }
            }
        }
        if let Some(text) = app.selection.settle(Instant::now(), delay) {
            if app.selection.mode == ListenMode::Off {
                #[cfg(target_os = "linux")]
                {
                    app.watcher = None;
                }
            }
            app.listening();
            app.selected(text);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn focused_watcher_disconnect_enables_fallback_without_accepting_own_text() {
        let (tx, _) = mpsc::channel();
        let mut app = new_app(Config::default(), tx);
        app.selection.arm(ListenMode::Continuous);
        app.listen_generation = 7;
        app.focus = true;
        app.selection_event(7, Ok("Our own popup".into()));
        assert!(app.selection.deadline(Duration::ZERO).is_none());
        app.selection_event(7, Err("Compositor disconnected".into()));
        assert_eq!(app.listen_generation, 8);
        assert!(app.accessibility_selection);
        assert_eq!(app.selection.mode, ListenMode::Continuous);
        app.selection_event(7, Err("Late duplicate".into()));
        assert_eq!(app.listen_generation, 8);
        app.selection.arm(ListenMode::Off);
        app.selection_event(8, Err("Stopped listener".into()));
        assert_eq!(app.listen_generation, 8);
    }

    #[test]
    fn snapshot_modifier_gestures_do_not_count_as_global_shortcuts() {
        let (tx, _) = mpsc::channel();
        let mut config = Config::default();
        config.once.sequence = "Shift".into();
        config.once.taps = 2;
        let mut app = new_app(config, tx);
        app.snapshot_session = Some("test-snapshot".into());
        app.shortcut("once");
        app.shortcut("once");
        assert_eq!(app.selection.mode, ListenMode::Off);
        assert!(!app.taps[0].press(Instant::now(), 2, Duration::from_millis(500)));
    }

    #[test]
    fn snapshot_ordinary_chords_still_count() {
        let (tx, _) = mpsc::channel();
        let mut config = Config::default();
        config.once.taps = 2;
        let mut app = new_app(config, tx);
        app.snapshot_session = Some("test-snapshot".into());
        app.shortcut("once");
        assert!(app.taps[0].press(Instant::now(), 2, Duration::from_millis(500)));
    }

    #[test]
    fn shortcut_recording_blocks_native_and_kwin_channels_without_carrying_partial_taps() {
        let (tx, _) = mpsc::channel();
        let mut config = Config::default();
        config.snapshot.sequence = "Shift".into();
        config.snapshot.taps = 2;
        let mut app = new_app(config, tx);
        app.modifier("Shift"); // One tap before opening the recorder.
        app.set_shortcut_recording(true);
        for _ in 0..3 {
            app.shortcut("snapshot"); // Native X11/macOS/portal callback.
            app.modifier("Shift"); // Independent KWin modifier watcher.
        }
        assert_eq!(app.jobs.generation, 0);
        assert_eq!(app.listen_generation, 0);
        app.set_shortcut_recording(false);
        let resume = app.shortcut_resume_at.unwrap();
        app.set_shortcut_recording(false); // Hide/destroy notifications must not extend the guard.
        assert_eq!(app.shortcut_resume_at, Some(resume));
        app.shortcut("snapshot");
        app.modifier("Shift");
        assert_eq!(app.jobs.generation, 0);
        std::thread::sleep(resume.saturating_duration_since(Instant::now()));
        app.modifier("Shift");
        assert_eq!(app.jobs.generation, 0); // Recording and pre-recording taps are discarded.
        app.modifier("Shift");
        assert_eq!(app.jobs.generation, 1); // The normal two-tap gesture works again.
    }

    #[test]
    fn recording_does_not_disable_explicit_controls_and_restarting_capture_resets_all_taps() {
        let (tx, _) = mpsc::channel();
        let mut app = new_app(Config::default(), tx);
        app.set_shortcut_recording(true);
        app.shortcut("once");
        app.shortcut("continuous");
        app.shortcut("snapshot");
        assert_eq!(app.selection.mode, ListenMode::Off);
        assert_eq!(app.listen_generation, 0);
        app.action("snapshot"); // Explicit tray/button actions are not global key events.
        assert_eq!(app.jobs.generation, 1);
        app.set_shortcut_recording(false);
        app.set_shortcut_recording(true); // Switch recorder before the previous guard expires.
        assert!(app.shortcut_resume_at.is_none());
        app.shortcut("snapshot");
        assert_eq!(app.jobs.generation, 1);
        for counter in &mut app.taps {
            assert!(!counter.press(Instant::now(), 2, Duration::from_secs(1)));
        }
    }

    #[test]
    fn translation_replies_preserve_client_request_identity_including_errors() {
        for (text, expected_type) in [("repository", "translation"), ("", "error")] {
            let (tx, rx) = mpsc::channel();
            let config = Config {
                provider: "cli".into(),
                cli_program: "/bin/cat".into(),
                ..Config::default()
            };
            let mut app = new_app(config.clone(), tx);
            app.jobs.submit(
                &config,
                Work::Translation {
                    text: text.into(),
                    source: "en".into(),
                    target: "ru".into(),
                    request_id: "corrected-source".into(),
                },
            );
            let Event::Completed(_, reply) = rx.recv_timeout(Duration::from_secs(3)).unwrap()
            else {
                panic!("Expected translation completion")
            };
            assert_eq!(reply["type"], expected_type);
            assert_eq!(reply["request_id"], "corrected-source");
            if expected_type == "translation" {
                assert_eq!(reply["source"], "en");
                assert_eq!(reply["target"], "ru");
                assert_eq!(reply["text"], text);
            }
        }
    }
}
