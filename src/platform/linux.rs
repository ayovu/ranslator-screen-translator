use anyhow::Result;
use std::{
    io::Read,
    os::fd::AsRawFd,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant},
};
use wl_clipboard_rs::{
    paste::{MimeType, Seat, select_mime_type},
    watch::{ClipboardEvent, ClipboardType, Watcher},
};

pub struct SelectionWatch {
    cancel: wl_clipboard_rs::watch::CancelHandle,
    stopped: Arc<AtomicBool>,
}
impl Drop for SelectionWatch {
    fn drop(&mut self) {
        self.stopped.store(true, Ordering::Relaxed);
        self.cancel.cancel();
    }
}
pub fn watch_selection(
    clipboard: bool,
    callback: impl Fn(Result<String>) + Send + 'static,
) -> Result<SelectionWatch> {
    let mut watcher = Watcher::new(
        if clipboard {
            ClipboardType::Both
        } else {
            ClipboardType::Primary
        },
        Seat::Unspecified,
    )?;
    let cancel = watcher.cancel_handle();
    let stopped = Arc::new(AtomicBool::new(false));
    let stop = stopped.clone();
    thread::spawn(move || {
        let mut initial = std::collections::HashSet::new();
        loop {
            let event = match watcher.next_event() {
                Ok(Some(event)) => event,
                Ok(None) => break,
                Err(error) => {
                    callback(Err(error.into()));
                    break;
                }
            };
            if stop.load(Ordering::Relaxed) {
                break;
            }
            match event {
                ClipboardEvent::Changed {
                    clipboard,
                    mime_types,
                    mut offer,
                } => {
                    // Never translate the selection that existed before the listener was armed.
                    if initial.insert(clipboard) {
                        continue;
                    }
                    let Some(mime) = select_mime_type(mime_types, MimeType::Text) else {
                        continue;
                    };
                    let Ok(pipe) = offer.receive(&mime) else {
                        continue;
                    };
                    let bytes = read_offer(pipe, &stop);
                    if let Some(bytes) = bytes.filter(|_| !stop.load(Ordering::Relaxed)) {
                        callback(Ok(String::from_utf8_lossy(&bytes).into_owned()));
                    }
                }
                ClipboardEvent::Cleared { clipboard } => {
                    initial.insert(clipboard);
                    callback(Ok(String::new()));
                }
            }
        }
    });
    Ok(SelectionWatch { cancel, stopped })
}

/// Reads a selection offer for up to two seconds; `None` when it is incomplete or stopped.
fn read_offer(mut pipe: impl Read + AsRawFd, stop: &AtomicBool) -> Option<Vec<u8>> {
    let fd = pipe.as_raw_fd();
    unsafe {
        libc::fcntl(
            fd,
            libc::F_SETFL,
            libc::fcntl(fd, libc::F_GETFL) | libc::O_NONBLOCK,
        );
    }
    let start = Instant::now();
    let mut bytes = Vec::new();
    while start.elapsed() < Duration::from_secs(2) && !stop.load(Ordering::Relaxed) {
        let mut buf = [0u8; 4096];
        match pipe.read(&mut buf) {
            Ok(0) => return Some(bytes),
            Ok(n) => {
                bytes.extend_from_slice(&buf[..n]);
                if bytes.len() > 65536 {
                    return None;
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                let mut poll = libc::pollfd {
                    fd,
                    events: libc::POLLIN,
                    revents: 0,
                };
                unsafe {
                    libc::poll(&mut poll, 1, 50);
                }
            }
            Err(_) => return None,
        }
    }
    None
}

/// Reads the current primary selection once, for the Translate selection shortcut. Data-control
/// reports the existing selection first, so this is the event the watcher above skips.
pub fn read_primary() -> Result<String> {
    let mut watcher = Watcher::new(ClipboardType::Primary, Seat::Unspecified)?;
    let cancel = watcher.cancel_handle();
    let finished = Arc::new(AtomicBool::new(false));
    let timer = finished.clone();
    thread::spawn(move || {
        thread::sleep(Duration::from_secs(2));
        if !timer.load(Ordering::Acquire) {
            cancel.cancel();
        }
    });
    let running = AtomicBool::new(false);
    let result = match watcher.next_event() {
        Ok(Some(ClipboardEvent::Changed {
            mime_types,
            mut offer,
            ..
        })) => match select_mime_type(mime_types, MimeType::Text) {
            None => Ok(String::new()),
            Some(mime) => offer
                .receive(&mime)
                .ok()
                .and_then(|pipe| read_offer(pipe, &running))
                .map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
                .ok_or_else(|| anyhow::anyhow!("The selected text could not be read in time.")),
        },
        Ok(Some(ClipboardEvent::Cleared { .. })) => Ok(String::new()),
        Ok(None) => Err(anyhow::anyhow!(
            "The selected text could not be read in time."
        )),
        Err(error) => Err(error.into()),
    };
    finished.store(true, Ordering::Release);
    result
}

#[allow(non_snake_case)]
mod protocol {
    use wayland_client;
    pub mod __interfaces {

        wayland_scanner::generate_interfaces!("assets/protocols/keystate.xml");
    }
    use self::__interfaces::*;
    wayland_scanner::generate_client_code!("assets/protocols/keystate.xml");
}
use protocol::org_kde_kwin_keystate::{self, OrgKdeKwinKeystate};
use wayland_client::{
    Connection, Dispatch, QueueHandle,
    globals::{GlobalListContents, registry_queue_init},
    protocol::wl_registry::WlRegistry,
};
struct KeyState<F> {
    callback: F,
    chord: crate::state::ModifierChord,
}
impl<F: Fn(String)> Dispatch<WlRegistry, GlobalListContents> for KeyState<F> {
    fn event(
        _: &mut Self,
        _: &WlRegistry,
        _: wayland_client::protocol::wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}
impl<F: Fn(String)> Dispatch<OrgKdeKwinKeystate, ()> for KeyState<F> {
    fn event(
        state: &mut Self,
        _: &OrgKdeKwinKeystate,
        event: org_kde_kwin_keystate::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let org_kde_kwin_keystate::Event::StateChanged {
            key,
            state: key_state,
        } = event;
        {
            if let Some(chord) = state.chord.update(key, key_state == 3, Instant::now()) {
                (state.callback)(chord);
            }
        }
    }
}
pub fn watch_modifiers(
    callback: impl Fn(String) + Send + 'static,
    error: impl Fn(String) + Send + 'static,
) {
    thread::spawn(move || {
        let result = (|| -> Result<()> {
            let conn = Connection::connect_to_env()?;
            let (globals, mut queue) = registry_queue_init::<KeyState<_>>(&conn)?;
            let handle = queue.handle();
            let keys: OrgKdeKwinKeystate = globals.bind(&handle, 5..=5, ())?;
            let mut state = KeyState {
                callback,
                chord: Default::default(),
            };
            keys.fetchStates();
            loop {
                queue.blocking_dispatch(&mut state)?;
            }
        })();
        if let Err(e) = result {
            error(format!(
                "Modifier-only shortcuts need KWin keystate v5: {e}"
            ));
        }
    });
}
