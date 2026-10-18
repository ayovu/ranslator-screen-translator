#[cfg(target_os = "linux")]
pub mod linux;

pub fn capabilities() -> &'static str {
    if cfg!(target_os = "linux") {
        "Linux adapters: Wayland data-control or AT-SPI selection; KDE, X11, optional GNOME companion or portal capture/shortcuts. Availability depends on the compositor and permissions; see docs/PORTABILITY.md."
    } else if cfg!(target_os = "macos") {
        "Experimental macOS adapters: AX selection, ScreenCaptureKit, Carbon shortcuts and AppKit material. Screen Recording/Accessibility permissions required; native Mac validation pending."
    } else {
        "This OS has no supported global capture/selection adapter. Portable UI and providers alone do not establish platform support."
    }
}
