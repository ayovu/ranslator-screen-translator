# Native integration guidelines

Read [the shared agent and platform guide](../docs/AGENT_GUIDE.md) before editing.
Keep the Qt interface and interaction model shared. Put macOS behavior in the
existing Objective-C++ adapters and Linux behavior in its existing integrations.
Preserve the Rust/native protocol and other platforms' paths. A porting task does
not authorize unrelated features or a second UI implementation. Add meaningful
regressions; report runtime checks that require another machine.
