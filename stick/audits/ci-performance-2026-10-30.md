# Firmware workflow performance

The firmware workflows run the production Stick S3 image with synthetic storage, controls and optical peers. They do not establish physical IR, analog, cache or cartridge timing.

## Measurements

The [previous GitHub workflow](https://github.com/lumirth/stickwalker/actions/runs/41004487532) ran the full suite separately on Linux and macOS. Its Linux job took 36 minutes 50 seconds and its macOS job took 34 minutes 58 seconds, concurrently. The complete run took 36 minutes 55 seconds. Its 30 scenario intervals accounted for about 33 minutes 46 seconds on Linux; nine optical scenarios accounted for about 22 minutes 7 seconds of that. These intervals include simulation and assertions.

The [first shared-build, sharded workflow](https://github.com/lumirth/stickwalker/actions/runs/41008740650) completed in 15 minutes 56 seconds. Its six jobs used 32 minutes 10 seconds of combined job time, compared with 71 minutes 48 seconds for the two previous jobs. The new workflow runs the complete suite on Linux and a settings and optical-storage smoke check on macOS. Job time is not CPU time, and the two runs do not have identical host coverage.

One instrumented local Walker exchange took 70.7 seconds wall time and 67.3 seconds of child CPU time. The 8-second simulated startup and two 4-second waits used 48.3 seconds of wall time; waiting for peer completion used 8.2 seconds. A separate noisy upload took 135.1 seconds wall time and 131.2 seconds of child CPU time. It spent 50.7 seconds in those three waits and 56.2 seconds waiting for peer completion. These are single local measurements, not hardware timings or CI predictions. A ten-second CPU sample during the noisy exchange showed most sampled stacks inside the emulator's execution and device-timing machinery.

## Changes evaluated

- Checking peer completion every 10 simulated microseconds instead of every microsecond reduced loop iterations tenfold, but the local peer phase took 8.7 seconds instead of 8.2 seconds. The recorded peer report, flash image and final screen were identical in that run. The change was not kept.
- Reducing the startup wait from eight simulated seconds to four saved about 15 seconds in one local exchange, but changed five bytes in the decoded synthetic EEPROM and changed the optical report. Passing the existing workflow did not establish equivalent behavior, so the shorter wait was not kept.
- Enabling the simulator's interpreter-only deferred batching throughout an exchange caused a peer response timeout. Enabling it only before the peer started completed the exchange and produced the same final flash image, but changed the optical trace. Neither version was kept.
- Skipping repeated scans of an unchanged console produced identical peer, flash and screen outputs, but did not improve the local benchmark. It was not kept.

Native instruction-cost timing currently uses the interpreter: the pinned simulator's native JIT does not collect those costs. Bypassing that timing to reduce CI time would change what the optical workflows exercise.

The workflows no longer use OCR to check screen wording. They retain screenshots for visual inspection and compare observable navigation, settings persistence, connection details, saved data and optical outcomes. The settings, recovery, optical-storage and exchange checks also verify that a simulated reboot occurred.
