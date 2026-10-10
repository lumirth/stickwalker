# Process motion samples on the original schedule

Stickwalker processes each acceleration sample as it arrives, using the
Pokéwalker's inactive and walking schedules and its 64-sample FFT window.
Sound and infrared pause sampling while they use the shared working memory.
Menus and games continue sampling.

This preserves the order in which the original program receives movement and
input. Buffering samples for later processing would also require replaying
button events and task changes in that order. The port therefore keeps
immediate processing.

See the [screen-off wake policy](0002-m-only-dark-wake.md) for how buttons affect
this schedule.
