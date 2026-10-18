# Build and validation guidelines

Read [the shared agent and platform guide](../docs/AGENT_GUIDE.md) before editing.
Keep platform artifacts separate and preserve previous verified releases. Package
the shared application, with explicit dependency and permission checks. Use
temporary profiles and isolated desktop sessions for tests. Never remove a failing
platform check merely to make a port pass; report the limitation. Keep local builds
reproducible without introducing GitHub Actions.
