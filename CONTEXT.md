# Stickwalker terms

Stickwalker is firmware for recreating a Pokéwalker on the M5Stack Stick S3.
The reconstructed firmware owns game and protocol behavior.
The Stick adapters provide hardware services. Use these distinctions when
changing the scheduler or interpreting validation results.

| Term | Meaning |
| --- | --- |
| Native firmware | The original Pokéwalker program represented by the matching H8 reconstruction. |
| Experience fidelity | Recreating gameplay, progression, screen availability, responsiveness, walking and game compatibility on the Stick. It does not require identical chip settings. |
| Native UI cadence | The application's schedule for rendering and visual state. Ordinary refresh opportunities occur every quarter second; individual animations may advance more slowly. |
| Panel scan rate | The TFT controller's internal rate for driving its stored image, independent of application cadence and processor image transfers. |
| Stationary carry | Screen-off operation detecting whether meaningful movement has begun. |
| Moving carry | Screen-off operation collecting and processing acceleration for walking progression. |
| Interactive use | Viewing or controlling the UI, including idle-on time after the last action. |
| Native screen availability | The original rules for waking, remaining visible and turning the display off, with foreground-specific exceptions. |
| Motion processing equivalence | Agreement in native motion/game results from the same chronological samples, external events and initial state. This detects scheduling regressions. |
| Motion accuracy | Agreement between detected steps and actual walking, affected by sensor input and the estimator. Processing equivalence alone does not establish it. |
| Native sampling gap | An interval where sound or IR owns foreground execution and shared state, suspending native motion sampling. Ordinary menus and games continue sampling. |
| IR session | A native protocol exchange with its peer, retaining foreground ownership and timing requirements. |
