# Contributing to Ranslator

Ranslator is open source under the [GNU GPL v3](LICENSE). Contributions, forks, bug reports, documentation, and platform testing are welcome.

For agent-assisted work and platform ports, follow the
[shared architecture and contribution rules](docs/AGENT_GUIDE.md). They define
adapter boundaries, scope, measurement requirements and review expectations.

To contribute interface translations without changing application code, follow
the [Qt Linguist catalog guide](docs/TRANSLATING.md).

## Start locally

Read the [user and build guide](docs/USER_GUIDE.md) for dependencies and the [platform notes](docs/PORTABILITY.md) for validation boundaries. Clone your fork, create a focused branch, and use the committed `Cargo.lock`:

```sh
cargo build --locked
cargo run --locked -- --window
cargo test --locked --all-targets
cargo clippy --locked --all-targets -- -D warnings
cargo fmt --check
```

A separate profile prevents development builds from changing your normal settings:

```sh
cp config.example.json /tmp/ranslator-development.json
RANSLATOR_CONFIG=/tmp/ranslator-development.json cargo run --locked -- --settings
```

Close the ordinary application first when testing the tray application: its single-instance connection belongs to the desktop session. Headless unit tests and isolated integration scripts do not require closing it.

Rust code in `src/` owns translation, configuration, process supervision, and OCR. Native UI and desktop integration live in `native/`. Packaging scripts live in `scripts/` and `packaging/`. Keep platform-specific behavior behind adapters and maintain the same user-facing features wherever their support is advertised.

## Validate changes

Run `scripts/check-build.sh` for the local checks it documents. See the [build guide](docs/USER_GUIDE.md#build-and-verify) for Qt interaction tests and isolated screenshot tests. Native files use `clang-format`; Rust files use `cargo fmt`.

For fixes, add a regression that fails for the original behavior. Tests must not send real text to cloud APIs, modify credentials, or capture a user's desktop without being explicitly run as a live test. Use temporary profiles, synthetic images, mock providers, and private display/bus sessions.

A passing build is not proof of desktop integration. Changes to capture, shortcuts, selection, focus, or multiple monitors require the relevant runtime checks and an explicit account of platforms not tested.

For a Mac contribution, start with one concrete failing scenario from the
[Mac handoff checklist](docs/PORTABILITY.md#mac-handoff). Fix it in the existing
adapter, run the local checks, and submit that change before unrelated features
or broad refactoring. Include before/after measurements for optimization claims.

## Submit a change

Use an imperative commit subject such as `Fix stale model responses after provider changes`. Keep unrelated refactoring separate. Explain the problem, resulting behavior, linked issue if any, and checks performed. Include before/after screenshots for visible changes, using synthetic content. Mention packaging or permission changes explicitly.

Do not commit generated builds, local profiles, credentials, downloaded SDKs, or dependency caches. Keep `AGENTS.md` intact. Document added dependencies and licenses, and make builds reproducible without GitHub Actions. Review [planned features](docs/ROADMAP.md) before starting a larger feature.
