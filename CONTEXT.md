# Stickwalker

Stickwalker is the Stick S3 firmware port of the original Pokéwalker application.
The matching `pw` decompilation is the authority for the native application and protocol.

## Language

**Native firmware**:
The original Pokéwalker program represented by the matching `pw` decompilation.
_Avoid_: Bench firmware, browser simulator

**Experience fidelity**:
Recreating the Pokéwalker's gameplay, progression, screen availability, responsiveness, walking experience, and game compatibility using the Stick's hardware.
It does not require identical chip settings, numeric sensor rates, or incidental hardware quirks.
_Avoid_: Cycle matching, identical chip configuration

**Native UI cadence**:
The native application's schedule for rendering and advancing visual state.
Its ordinary refresh opportunity occurs every quarter second; individual animations can advance more slowly.
_Avoid_: Panel scan rate

**Panel scan rate**:
The Stick S3 TFT controller's internal rate for driving its stored image onto the panel.
It is independent of the native UI cadence and of image transfers from the processor.
_Avoid_: Game frame rate

**Stationary carry**:
Screen-off operation while detecting whether meaningful movement has begun.
_Avoid_: Off, sleep without specifying the operating state

**Moving carry**:
Screen-off operation while collecting and processing acceleration for walking progression.
_Avoid_: Stationary standby

**Interactive use**:
Operation while the user is viewing or controlling the Pokéwalker UI, including menus and games.
Its battery cost includes the screen's idle-on time after the user's last action.

**Native screen availability**:
The original firmware's rules for waking, keeping visible, and powering down the display, including foreground-specific exceptions.
_Avoid_: A fixed universal screen-off deadline

**Motion processing equivalence**:
Agreement in native motion/game results when processing the same chronological samples and external events from the same initial state.
It is a check for scheduling regressions, rather than the complete definition of experience fidelity.
_Avoid_: Identical physical step detection

**Motion accuracy**:
Agreement between detected steps and actual walking, influenced by both the sensor input and the native estimator.
_Avoid_: Processing equivalence

**Native sampling gap**:
An interval in which the original firmware suspends motion sampling while another foreground task owns execution and shared state, such as sound playback or IR.
_Avoid_: Any screen-on interval

**IR session**:
A native Pokéwalker protocol exchange with its peer, including its original foreground ownership and timing requirements.
_Avoid_: Background listening
