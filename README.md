# Ranslator

Translate selected text or words on your screen, then keep reading where you were.
A native Linux app with a Rust core, OCR and a floating translation popup.

![Snapshot: select a passage, then keep multiple translations open and arrange their cards](assets/screenshots/snapshot-demo.gif)

*Actual Snapshot recording: English → Spanish with local Ollama. Hold Shift for multiple selections, then drag the cards. [Recording notes](assets/screenshots/README.md).*

- **Select or capture.** Translate text selections, images and otherwise unselectable screen content.
- **Keep several translations open.** Select multiple regions, move and resize their cards, or expand one for comfortable reading.
- **Choose your engine.** Ollama, OpenAI-compatible APIs, CLI runtimes, LibreTranslate or MyMemory.
- **Make it yours.** Preferred and fallback languages, editable source language, shortcuts, light/dark themes, OCR packs and a start-at-login toggle.

## Get it

[Releases](https://github.com/ayovu/ranslator-screen-translator/releases) · [Build from source](docs/USER_GUIDE.md#build-and-verify) · [User guide](docs/USER_GUIDE.md)

The first public release is being prepared. Linux packages are produced as **AppImage** and **Flatpak**, with Qt, Tesseract and English OCR included. Additional OCR packs include Ukrainian, Japanese, Chinese and Arabic. No LLM weights are downloaded automatically.

Automatic translation uses an available local Ollama model, otherwise the online MyMemory service. Choose Ollama explicitly for local-only translation.

Developed on **Arch Linux / KDE Plasma Wayland**. Other Wayland desktops and X11 use their available integrations; shortcuts, monitor capture and blur can differ. **macOS is experimental and still needs testing on a real Mac.** See the [platform validation notes](docs/PORTABILITY.md).

AUR and Flathub publication is planned; the app is not listed there yet. [Packaging and release details](docs/RELEASING.md).

## Build

Install the [platform dependencies](docs/USER_GUIDE.md#build-and-verify), then:

```sh
cargo build --release --locked
cargo run --release --locked -- --window
```

On Linux, `scripts/install.sh` installs to `~/.local`. For macOS, follow the [Mac build guide](packaging/macos/README.md).

## Contribute

Bug reports, focused pull requests and platform testing are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md); platform ports and coding agents should also follow the [shared architecture guide](docs/AGENT_GUIDE.md).

Want to translate the interface? Start with the [translation guide](docs/TRANSLATING.md). English is the source language; community catalogs are separate from OCR packs.

[GNU GPL v3](LICENSE). Bundled components retain their own licenses.
