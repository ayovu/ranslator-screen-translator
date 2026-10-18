# Building and sharing releases

All builds run locally. There are no GitHub Actions workflows.

## Artifacts

Keep artifacts from different platforms separate; never rename an Arch binary as
a portable Linux or macOS build. Native host packages go to
`dist/native-linux-<architecture>/`; portable packages go to `dist/portable/`.

The generic `dist/Ranslator-<architecture>.AppImage` and `dist/Ranslator.flatpak`
paths are compatibility symlinks to the latest verified portable artifacts.
Update these aliases and their root checksums only after release checks pass.
Earlier root packages are preserved under `dist/archive/pre-source-theme/`;
`dist/stable/` remains the separate preserved Arch/KDE snapshot. Packaging scripts
refuse to overwrite release aliases.

| Purpose | Command or location |
| --- | --- |
| Installed build on the original Arch/KDE machine | `~/.local/bin/ranslator` |
| Native Linux AppImage using the builder's libraries | `scripts/package-appimage.sh` |
| Portable Linux baseline and distro checks | `scripts/package-portable.sh` |
| Flatpak from the staged Linux package | `scripts/package-flatpak.sh` |
| Native Mac build and `.app` archive | `scripts/package-macos.sh` |
| Source archive for a friend or GitHub release | `python3 scripts/package-source.py` |

Source packages are written to `dist/source/`, include the lockfile and build
instructions, and exclude generated builds and local configuration. Extract one
and run the relevant build script on the target system. macOS instructions and
signing options are in [packaging/macos](../packaging/macos/README.md).

The portable Linux recipe builds on an older glibc baseline and checks every
bundled ELF library. Container rendering/OCR checks validate library compatibility;
they do not validate a desktop compositor. Publish the actual validation report
alongside a release and clearly label experimental platforms.

## Before publishing

Run `scripts/check-build.sh` and the applicable isolated integration scripts.
Verify capture, selection, shortcuts, focus, multiple monitors, and permissions on
each advertised desktop. Preserve the previous release until these checks pass.

Include checksums, source archive, license notices, minimum OS requirements, and
known limitations. macOS source adapters have not been built or exercised on a
real Mac in this Linux environment; do not label a Mac release stable until the
target-machine checklist passes.

Confirm AppStream metadata and maintainer details. Configure private vulnerability
reporting. Review `git status` and the staged diff for private data before committing
or publishing. Compiling or creating an archive never publishes it automatically.

## Uploading a GitHub release manually

GitHub Actions are not required. Commit and push the reviewed source first, then
open **Releases → Draft a new release** in your repository. Create a version tag
such as `v0.1.0` against that source commit, describe the changes and limitations,
and attach the versioned AppImage, Flatpak, optional portable tarball, matching
source archive, `LICENSE`, installation notes and `SHA256SUMS`. Review the draft
before publishing. Mark an unvalidated initial release as a prerelease.

Build output belongs in release attachments, not in Git source history. Include
the complete corresponding source and bundled dependency notices with each GPL v3
binary release. GitHub's automatic source archive reflects the selected tag;
verify that it matches the source used to build your attachments. See GitHub's
[manual release instructions](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).

## AUR

Not published yet. AUR hosts installation recipes, not release binaries. After a
public tagged GitHub release, a proposed `ranslator-bin` package can download a
versioned Linux artifact with a fixed checksum; a source-built `ranslator` package
is another option. Confirm name availability before submission.

Prepare and test `PKGBUILD`, check dependencies and licenses in a clean Arch build,
run `namcap`, and generate `.SRCINFO` with `makepkg --printsrcinfo`. Submit through
the maintainer's own AUR account and SSH key. Each release needs updated metadata
and checksums. See the [AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines)
and [.SRCINFO documentation](https://wiki.archlinux.org/title/.SRCINFO).

## Flathub

Not published yet. Our `.flatpak` and `packaging/flatpak/` manifest are for local
distribution; the manifest currently references a local AppDir. They are not a
reviewed Flathub submission.

Before submission, choose an app ID matching the owned repository, migrate affected
desktop/D-Bus/configuration identities, complete AppStream metadata and screenshots,
and test a reproducible release manifest. The current `io.github.ranslator.Ranslator`
ID does not identify the `ayovu/ranslator-screen-translator` repository. Host CLI
execution requests `org.freedesktop.Flatpak`, which needs a justified linter
exception. Retest that integration with the reviewed permissions.

As checked on 2026-10-18, Flathub requires meaningful development/release history,
disclosure of AI-generated application material, and a human-authored manifest
without AI assistance. AI agents must not submit or generate submission/review
interactions. Acceptance is discretionary. Read the current
[requirements](https://docs.flathub.org/docs/for-app-authors/requirements),
[permission exceptions](https://docs.flathub.org/docs/for-app-authors/linter#finish-args-flatpak-spawn-access)
and [submission process](https://docs.flathub.org/docs/for-app-authors/submission).

Add store badges and installation links only after the corresponding package is
actually published. A local Flatpak build does not require Flathub publication.
