# Preserve native motion ownership gaps

Process each eligible motion sample immediately on the existing inactive/activity
and moving/interactive schedules. Preserve the native FFT window and suspend
sampling while sound or IR owns foreground execution and shared workspace.
Ordinary menus and games continue sampling.

These ownership gaps are part of the original application's behavior. Deferred
FIFO batching would introduce a new scheduler and require chronological replay
of input and ownership events; it is outside this design.
See the [screen-off wake policy](0002-m-only-dark-wake.md).
