# Publish Ranslator on AUR

The `ranslator-bin/` directory contains a recipe for the published **v0.1.0
prerelease, x86_64 only**. AUR publication is a separate, manual step. GitHub
Actions are not required.

**Publication paused (2026-10-19):** AUR currently blocks new account registration.
The registration notice says there is no manual registration queue. Keep this
recipe for later; the account and upload instructions below apply once
registration reopens or an existing maintainer account is available. Follow
[aur-general](https://lists.archlinux.org/archives/list/aur-general@lists.archlinux.org/)
or [Arch news](https://archlinux.org/news/) for updates; do not automate retries.
Ranslator is not listed in AUR, so `yay -S ranslator-bin` is not available yet.
GitHub release downloads and local package installation remain available.

The package downloads the versioned portable archive from GitHub, checks its
SHA-256, and installs it under `/usr/lib/ranslator`. The launcher, desktop entry,
icon and license notices are registered under `/usr`. Qt, Tesseract and English
OCR data are bundled; other OCR packs and local models remain optional downloads.
No login startup, GNOME extension or user preferences are enabled by installation.

## Build and check locally

On an up-to-date Arch Linux system, install `base-devel`, `git` and `namcap`.
Run `makepkg` as your ordinary user, never with `sudo`:

```sh
mkdir -p build/aur-local
cp packaging/aur/ranslator-bin/{PKGBUILD,.SRCINFO,LICENSE} build/aur-local/
cd build/aur-local
makepkg --syncdeps --cleanbuild
namcap PKGBUILD ranslator-bin-*.pkg.tar.zst
makepkg --printsrcinfo > .SRCINFO
```

For installation, run `sudo pacman -U ranslator-bin-*.pkg.tar.zst`.
Use `/usr/bin/ranslator --doctor` to check the packaged launcher. An older manual
installation in `~/.local/bin` can take priority in `PATH`; `type -a ranslator`
shows which command will run. Existing user desktop entries and autostart entries
can likewise point to a previous installation. Review those paths before replacing
your normal installation; package installation does not rewrite personal files.

Bundled third-party libraries retain their upstream build flags and license files.
`namcap` reports unstripped binaries and missing FULL RELRO hardening in some
upstream libraries; this recipe does not rebuild them. It also reports shell and
D-Bus dependencies as unused, although the launcher and desktop integrations need
them. Review warnings alongside runtime library-resolution tests. Real compositor,
shortcut and multi-monitor testing
still follows [the platform checklist](../../docs/PORTABILITY.md).

## Register an account and key

For new maintainers, wait until the registration pause described above ends.

1. Open [AUR registration](https://aur.archlinux.org/register), register your own
   username and email, complete the displayed verification and follow the account
   activation instructions. A GitHub account does not create an AUR account.
2. Create a dedicated SSH key:

   ```sh
   mkdir -p ~/.ssh
   chmod 700 ~/.ssh
   ssh-keygen -t ed25519 -C "AUR" -f ~/.ssh/id_ed25519_aur
   ```

   Choose a passphrase when prompted. If that filename already exists, do not
   overwrite it; use the existing AUR key or choose another filename throughout
   these commands. The private key is `~/.ssh/id_ed25519_aur`; its public counterpart
   is `~/.ssh/id_ed25519_aur.pub`.
3. Run `cat ~/.ssh/id_ed25519_aur.pub`. Copy the **complete public line** into
   **My Account → SSH Public Key** on AUR and save. Never upload the private file.
4. Test authentication:

   ```sh
   ssh -i ~/.ssh/id_ed25519_aur -o IdentitiesOnly=yes -T aur@aur.archlinux.org
   ```

   On first connection, compare the host fingerprint with the fingerprints on
   [the AUR homepage](https://aur.archlinux.org/) before accepting it. A successful
   response identifies your AUR account and says interactive shells are disabled;
   that message is normal. The SSH user is always `aur`, not your account name.

## Publish the recipe

Check that [ranslator-bin](https://aur.archlinux.org/packages/ranslator-bin) is
still available before the first upload. AUR accepts the `master` branch. Keep
its small packaging repository separate from the application repository:

```sh
mkdir -p ~/aur
git -c init.defaultBranch=master \
  -c core.sshCommand='ssh -i ~/.ssh/id_ed25519_aur -o IdentitiesOnly=yes' \
  clone ssh://aur@aur.archlinux.org/ranslator-bin.git ~/aur/ranslator-bin
cd ~/aur/ranslator-bin
git config core.sshCommand 'ssh -i ~/.ssh/id_ed25519_aur -o IdentitiesOnly=yes'
```

An empty-repository warning is expected for a new package. Replace the example
identity below with **your own public commit identity**; these settings apply to
this repository only:

```sh
git config user.name "YOUR_NAME"
git config user.email "YOUR_PUBLIC_EMAIL"
```

Copy `PKGBUILD`, `.SRCINFO` and `LICENSE` from this project's
`packaging/aur/ranslator-bin/` into `~/aur/ranslator-bin/`. You may add a
`# Maintainer: Your name <your@email>` comment to `PKGBUILD`. Then:

```sh
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO LICENSE
git diff --cached
git commit -m "Add ranslator-bin 0.1.0"
git push -u origin master
```

Do not upload the application sources, binary archive or generated package to
AUR. After the push succeeds and the package page shows version `0.1.0-1`, other
users can install it with `yay -S ranslator-bin`. Add an AUR link to the main
README only after that publication is verified.

## Update a release

Publish the corresponding GitHub assets first. Change `pkgver`, reset `pkgrel`
to `1`, and update the archive checksum from the actual downloaded asset.
For a packaging-only correction, increment `pkgrel`. Build, install and validate
again, regenerate `.SRCINFO`, then commit and push the recipe. A GitHub release
does not update AUR automatically. Keep the in-project recipe synchronized too.

See the official [submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines)
and [.SRCINFO documentation](https://wiki.archlinux.org/title/.SRCINFO).
