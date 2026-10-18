# Planned features

These items are design proposals, not available features. Platform parity, reliable capture, packaging, and current rendering defects take priority.

## Alternatives for a selected word

When the selection is a single word, offer alternative translations with short distinctions in meaning, part of speech, and usage. Keep the primary translation prominent. Do not label invented alternatives as dictionary entries; identify whether the provider supplied dictionary data or generated suggestions. Support providers without structured output through an explicit capability check.

## Experimental translation with context

Provide an opt-in setting, disabled by default. Keep the selected text separate from context, and ask the provider to translate only the selection.

Prefer the containing paragraph or a user-selected surrounding region. Do not send every visible application indiscriminately. Show the exact context to be sent and allow removal before submission. OCR remains local; sending extracted context follows the selected provider's network behavior. Keep context requests bounded and cancel stale results when the selection or provider changes.

Evaluate ambiguous words, multiple windows, unrelated nearby text, mixed languages, sensitive fields, and providers that do not support a separate context field before enabling this feature outside experiments.
