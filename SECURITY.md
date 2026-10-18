# Security

Selected text goes to the configured translation provider. Automatic mode can use
an online provider when Ollama is unavailable; choose Ollama explicitly for local
translation. A coding CLI may call a cloud model despite running locally.

Keep API tokens, provider profiles, private screenshots, and runtime credentials
out of issues, pull requests, and source archives. Report bugs using synthetic text
and a redacted configuration. Ranslator does not need your account tokens to
reproduce UI, OCR, capture, or process cancellation defects.

Flatpak's host-command permission lets an explicitly selected CLI run outside the
sandbox. Review that permission before installing. Host CLI execution uses your
existing login, a private temporary working directory, and bounded processes.
It does not provide a sandbox for an arbitrary custom executable.

Selection access is enabled only while a listener is armed. Screenshot recognition
is local; only the selected words go to the translation provider. Experimental
context translation is a proposal and is not implemented.

For a vulnerability involving private data or command execution, use the hosting
repository's private vulnerability reporting facility if enabled, or the
maintainer's published private contact. Do not publish secrets or an exploit
against another person's installation. No reporting address is invented here;
the project owner must configure it when publishing the repository.
