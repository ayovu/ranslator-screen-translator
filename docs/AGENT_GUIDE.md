# Agent and platform contribution guide

These rules apply to human and agent-assisted changes. A platform port improves
one shared application; it must not create a separate product inside the repository.
Read [CONTRIBUTING](../CONTRIBUTING.md) and the current
[validation boundaries](PORTABILITY.md) before implementation.

## Keep responsibilities in their existing layers

| Responsibility | Location |
| --- | --- |
| Configuration, providers, cancellation, translation state and OCR scheduling | `src/` |
| Shared widget layout, interaction and material painting | `native/ui.cpp`, `native/reader.hpp`, `native/design.cpp` |
| Shared interface strings and community catalogs | `native/i18n.*`, `translations/`; see [translation guide](TRANSLATING.md) |
| OS selection, capture, shortcuts and backdrop integration | Existing adapters in `native/` and `src/platform/` |
| Dependency deployment, signing and artifact generation | `scripts/`, `packaging/` |

For macOS, start with `native/macos.mm`, `native/accessibility_macos.mm`, their
interfaces and platform-gated CMake integration. Keep platform decisions at
adapter boundaries. Do not duplicate the translator, settings, provider router,
or snapshot state machine for a particular OS. Do not remove Linux integrations
to make a Mac build pass.

## Keep the requested scope

Fix the named platform problem first. Separate new features, visual redesigns
and unrelated refactoring into their own proposals and PRs. Prefer extending an
existing adapter over adding parallel implementations or a new abstraction layer.
Create a module when it has a distinct responsibility; explain that responsibility.

Shared changes are allowed when necessary: explain the cross-platform reason,
preserve existing behavior, and cover the affected contract with tests. Preserve
configuration compatibility and the Rust/native message protocol. Do not silently
reset preferences, weaken cancellation/stale-result handling, or change other
platforms' defaults for a local workaround.

Mark visible interface strings with `UiText::tr` and refresh the source catalog.
Keep English as the source/fallback. Translate complete sentences with placeholders;
never translate protocol keys, provider IDs, user content or saved shortcuts.
Interface language must remain independent of translation and OCR languages.

## Measure optimizations

Record the scenario, hardware, OS, package, baseline and result. Measure the relevant
latency, memory, CPU or package size using identical synthetic inputs. A speculative
cache, rewrite or dependency replacement is not evidence of an improvement.
Check cold/warm runs and failure/cancellation paths where relevant. Identify costs
shifted to startup, memory, another platform or another workflow.

## Validate and report honestly

Run the relevant regression tests and `scripts/check-build.sh`. For platform work,
test permissions, focus, shortcuts, capture and monitor mapping on that platform;
use [the Mac handoff checklist](PORTABILITY.md#mac-handoff). Preserve the existing
Linux checks. Report checks you could not run; do not disable them to obtain a pass.

Use synthetic text and isolated displays/profiles. Missing capabilities must produce
an actionable explanation, not silently different behavior. Keep unvalidated support
experimental. Add dependencies only with a concrete need, platform fallback,
packaging changes and license notices. No GitHub Actions are required.

## Prepare a reviewable contribution

Start a focused branch from the current upstream default branch and keep it updated.
Keep local experiments under ignored `build/`; do not commit binaries, SDKs,
credentials, personal paths or machine-specific profiles. The PR must distinguish
adapter changes from shared changes, include regression evidence and measurements
for performance claims, and state remaining platform limitations.
