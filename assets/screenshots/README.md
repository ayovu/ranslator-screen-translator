# Demo media

`snapshot-demo.gif` is a 28-second recording of the running application, using an
English excerpt from the project owner's supplied
`endowment-calculator-parameters.pdf`. The original PDF is not distributed here.

- Actual screen capture, English OCR and local Ollama `qwen2.5:3b` responses.
- One selection, Shift to keep Snapshot open, three translations, dragging and
  bringing an overlapping card forward.
- Recorded on an isolated X11 display with a compositor and a cropped PDF viewer.
  Captions, a cursor trail and click highlights were added afterward; the sequence
  runs at its original speed. It is a workflow demonstration, not a latency benchmark:
  the model was already loaded and the repeated first passage uses the app's cache.
- Window blur and transparency depend on the desktop. This recording does not
  validate Wayland, macOS, monitor selection or translation accuracy in general.

The other PNG previews use the Chinese → English Adventure Time example.
