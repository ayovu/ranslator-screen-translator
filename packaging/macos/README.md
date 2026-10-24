# Local macOS packaging

Build natively on the intended Mac architecture. Apple Silicon produces an
`arm64` application; an Intel Mac produces `x86_64`. There is no hosted build
service or Linux-to-macOS cross-build dependency.

Before changing the port, read the [agent and platform contribution guide](../../docs/AGENT_GUIDE.md).
Work through one concrete failure from the [Mac handoff checklist](../../docs/PORTABILITY.md#mac-handoff)
per focused change. Keep common translation/UI behavior shared and put OS-specific
fixes in the existing adapters; attach measurements for optimization claims.

Install Xcode and its command line tools, then:

```sh
brew install rust cmake qt tesseract pkg-config python librsvg
scripts/package-macos.sh
```

The script runs Rust tests, builds a release, assembles
`dist/macos-<architecture>/Ranslator.app`, copies Qt/Tesseract and their dependent
libraries, and checks every Mach-O architecture and load path. It verifies the
signature, renders a widget and runs bundled OCR before creating a ZIP. Qt 6.12
requires macOS 14.4 or newer; the resulting `Info.plist` is raised further if any
bundled library has a newer minimum OS requirement.

Snapshot uses Apple Vision for OCR, linked from the system frameworks. Tesseract
and its English model remain in the bundle for the **Tesseract** engine choice in
Settings → Screen text. Vision compiles its recognizer on each app's first use
after installation or a macOS update (about a minute), so the app prepares it in
the background at launch; the first test run on a new machine is slower for the
same reason.

By default the bundle is ad-hoc signed for local testing. For distribution with a
Developer ID, set `RANSLATOR_SIGN_IDENTITY`. Set `RANSLATOR_NOTARY_PROFILE` to an
existing `notarytool` keychain profile to notarize and staple the resulting app.
Signing credentials remain in the builder's keychain.

`RANSLATOR_QT_PREFIX` selects a non-Homebrew Qt installation.
`RANSLATOR_DIST_DIR` changes the output directory. Packages and intermediate
objects for other platforms or architectures are preserved.

Building and packaging are separate from validating desktop behavior. Before
sharing a macOS release, verify screen-recording and Accessibility permissions,
global shortcuts, single and multiple monitor capture, text selection, popup
focus, local CLI discovery, cancellation, and model switching on a real Mac.
