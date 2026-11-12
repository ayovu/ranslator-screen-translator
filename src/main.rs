mod controller;
use anyhow::{Context, Result};
use ranslator::{config::Config, ocr, provider};
use serde_json::{Value, json};
use std::{
    ffi::{CStr, CString, c_char},
    io::Read,
    sync::{Arc, OnceLock, atomic::AtomicBool},
};

static EVENTS: OnceLock<std::sync::mpsc::Sender<controller::Event>> = OnceLock::new();
#[cfg(all(target_os = "macos", not(core_only)))]
unsafe extern "C" {
    fn ranslator_vision_ocr(image: *const c_char, languages: *const c_char) -> i32;
    fn ranslator_vision_warm_up(languages: *const c_char) -> i32;
}
#[cfg(not(core_only))]
unsafe extern "C" {
    fn ranslator_ui_run(
        config: *const c_char,
        callback: extern "C" fn(*const c_char),
        background: bool,
        smoke: *const c_char,
    ) -> i32;
    fn ranslator_ui_emit(event: *const c_char);
}
pub fn emit(value: Value) {
    #[cfg(not(core_only))]
    if let Ok(text) = CString::new(value.to_string()) {
        unsafe {
            ranslator_ui_emit(text.as_ptr());
        }
    }
    #[cfg(core_only)]
    let _ = value;
}
extern "C" fn on_ui_event(raw: *const c_char) {
    // The bridge lends a NUL-terminated UTF-8 buffer only for this callback.
    if raw.is_null() {
        return;
    }
    let data = unsafe { CStr::from_ptr(raw) };
    if let Ok(value) = serde_json::from_slice(data.to_bytes()) {
        if let Some(tx) = EVENTS.get() {
            let _ = tx.send(controller::Event::Ui(value));
        }
    }
}
fn main() {
    if let Err(e) = run() {
        eprintln!("Ranslator: {e:#}");
        std::process::exit(1);
    }
}
fn run() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    #[cfg(all(target_os = "macos", not(core_only)))]
    if args.first().is_some_and(|arg| arg == ocr::VISION_OPTION) {
        let target = args
            .get(1)
            .context("Usage: ranslator --vision-ocr IMAGE|--warm-up [LANGUAGES]")?;
        let languages = CString::new(args.get(2).map_or("", String::as_str))?;
        std::process::exit(if target == ocr::WARM_UP_OPTION {
            unsafe { ranslator_vision_warm_up(languages.as_ptr()) }
        } else {
            let image = CString::new(target.as_str())?;
            unsafe { ranslator_vision_ocr(image.as_ptr(), languages.as_ptr()) }
        });
    }
    if args.iter().any(|a| a == "--help" || a == "-h") {
        println!(
            "Ranslator — native selection translation\n\n  ranslator                     Start in tray (popup mode)\n  ranslator --settings          Open settings\n  ranslator --window            Open full translator\n  ranslator --background        Start in tray\n  ranslator --translate-selection Translate the current selection (desktop keybinding command)\n  ranslator --select            Arm one selection (desktop keybinding command)\n  ranslator --listen            Toggle continuous selection\n  ranslator --snapshot          Capture screen (desktop keybinding command)\n  ranslator --translate [LANG]   Translate UTF-8 stdin\n  ranslator --image IMAGE        Open image in the OCR selection view\n  ranslator --quit               Quit the running app\n  ranslator --ocr IMAGE          Print OCR words as JSON\n  ranslator --doctor             Report dependencies and local models\n  ranslator --smoke-test PATH    Render widget to PNG, then exit\n  ranslator --design-preview DIR Render light/dark UI previews\n\nSettings: {}",
            Config::path().display()
        );
        return Ok(());
    }
    let config = Config::load()?;
    let cancel = Arc::new(AtomicBool::new(false));
    match args.first().map(String::as_str) {
        Some("--doctor") => {
            println!(
                "{}",
                json!({"platform": ranslator::platform::capabilities(), "config":Config::path(), "ocr_data":ocr::data_dir(&config), "english_ocr":ocr::data_dir(&config).join("eng.traineddata").exists(), "ocr_engine":ocr::engine(&config), "tesseract":std::process::Command::new(ocr::executable()).arg("--version").output().is_ok_and(|output| output.status.success()), "ollama_model":provider::local_model(), "wayland":std::env::var_os("WAYLAND_DISPLAY").is_some()})
            );
            return Ok(());
        }
        Some("--translate") => {
            let mut text = String::new();
            std::io::stdin().take(65_537).read_to_string(&mut text)?;
            let result = provider::translate(
                &config,
                &text,
                None,
                args.get(1).map(String::as_str),
                &cancel,
            )?;
            println!("{}", result.text);
            return Ok(());
        }
        Some("--ocr") => {
            let path = args.get(1).context("Usage: ranslator --ocr IMAGE")?;
            println!(
                "{}",
                serde_json::to_string(&ocr::recognize(
                    &config,
                    std::path::Path::new(path),
                    &cancel
                )?)?
            );
            return Ok(());
        }
        Some(
            "--settings"
            | "--window"
            | "--background"
            | "--smoke-test"
            | "--design-preview"
            | "--image"
            | "--quit"
            | "--translate-selection"
            | "--select"
            | "--listen"
            | "--snapshot",
        )
        | None => (),
        Some(other) => anyhow::bail!("Unknown option: {other}. Use --help."),
    }
    let (tx, rx) = std::sync::mpsc::channel();
    let _ = EVENTS.set(tx.clone());
    let mut startup = serde_json::to_value(&config)?;
    if args.first().is_some_and(|arg| arg == "--design-preview") {
        let directory = args
            .get(1)
            .context("Usage: ranslator --design-preview DIRECTORY")?;
        std::fs::create_dir_all(directory)?;
        startup["_design_preview"] = json!(std::fs::canonicalize(directory)?);
    }
    if args.first().is_some_and(|arg| arg == "--image") {
        startup["_image"] = json!(std::fs::canonicalize(
            args.get(1).context("Usage: ranslator --image PATH")?
        )?);
    }
    if args.first().is_some_and(|arg| arg == "--settings") {
        startup["_settings"] = json!(true);
    }
    if args.first().is_some_and(|arg| arg == "--window") {
        startup["_window"] = json!(true);
    }
    if args.first().is_some_and(|arg| arg == "--quit") {
        startup["_quit"] = json!(true);
    }
    match args.first().map(String::as_str) {
        Some("--translate-selection") => startup["_action"] = json!("translate_selection"),
        Some("--select") => startup["_action"] = json!("once"),
        Some("--listen") => startup["_action"] = json!("continuous"),
        Some("--snapshot") => startup["_action"] = json!("snapshot"),
        _ => (),
    }
    let settings = CString::new(startup.to_string())?;
    let smoke = CString::new(
        if args
            .first()
            .is_some_and(|s| s == "--smoke-test" || s == "--design-preview")
        {
            args.get(1).context("Provide a PNG output path")?.as_str()
        } else {
            ""
        },
    )?;
    let worker = std::thread::spawn(move || controller::run(config, tx, rx));
    #[cfg(not(core_only))]
    let status = unsafe {
        ranslator_ui_run(
            settings.as_ptr(),
            on_ui_event,
            args.iter().any(|a| a == "--background"),
            smoke.as_ptr(),
        )
    };
    #[cfg(core_only)]
    let status = {
        let _ = (settings, smoke);
        eprintln!("UI disabled in core-only build");
        1
    };
    let _ = EVENTS
        .get()
        .unwrap()
        .send(controller::Event::Ui(json!({"type":"quit"})));
    let _ = worker.join();
    if status != 0 {
        anyhow::bail!("Desktop UI exited with status {status}");
    }
    Ok(())
}
