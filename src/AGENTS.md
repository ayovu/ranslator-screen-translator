# Shared Rust guidelines

Read [the shared agent and platform guide](../docs/AGENT_GUIDE.md) before editing.
Configuration, providers, request state, cancellation and OCR scheduling remain
shared across platforms. Keep OS-specific access behind adapters. Preserve saved
settings and message compatibility. Explain and test any shared behavior changed
by a platform fix; do not replace general logic with a machine-specific workaround.
