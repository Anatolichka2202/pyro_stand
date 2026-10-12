# Instructions for agents working on pyro_stand

## Three separate products — do not mix their responsibilities

1. **KASU PP (C++/Qt)** controls loading the flight cyclogram to BCVM and the
   launch controller. Its isolated Debug module publishes events and cyclogram
   evidence over `kasu.pp.debug.stand.v1`. Pyro Stand observes that module;
   it does not send launch commands to KASU PP.
2. **Pyro Tester (Python/PySide)** is the tester-board service/diagnostic app.
   Its 12 YPS / 96-channel state tables, oscillograms, raw signal tools,
   serial/UDP diagnostics belong there, NOT in Pyro Stand.
3. **Pyro Stand (this C++/Qt repo)** is an operator HMI for the flight task:
   the planned monitored events, physical measurements relative to KP,
   planned vs actual times and signed deltas, tolerances, and traceable reports.

## Operator GUI contract

- Preserve the original summer `MainWindow` **visual hierarchy**: large timing
  block, connection/status card, event timeline, operator event table,
  summary strip, and event/system journals. Current implementation is
  `IntegrationWindow`; old `Stand` is only a UX reference, not the backend.
- The primary table displays events from the built-in БКД-А-1 / БКД-Б configuration.
  Never require an external JSON wiring file. Multichannel actions may have indented
  per-input subrows. **Never add a 96-channel diagnostic table**, tab, or panel.
- Planned T0 UTC originates from the cyclogram published by KASU PP.
  PC UTC countdown is only a planning indication; it is not measured T0.
- Support both testers: new AA55/CRC8 frames over UDP or RS-485, and old
  COM masks (one byte = one hardware millisecond, bits 1..8, КП on bit 8).
  Measured relative time = hardware/sample timestamp minus physical KP on the
  same counter; delta = measured - planned. UART chunk arrival time is not measurement time.
  Support one old COM with selected БКД, or two COMs for А-1 and Б concurrently.
  Two old COMs require КП8 in each stream; never subtract independent byte counters.
  New tester's YPS assignments are selected by physical wiring in the UI; do not guess them.
  Owner confirmed 2026-10-12: А-1 item 3 -> channels 1,2; item 5 -> 3,4,5;
  item 16 -> 6,7. Б item 20 -> 1,2; item 21 -> 3,4,5; item 22 -> 6,7.
  These are original action ordinals, not milliseconds or display row numbers.
  Validate action meaning too: an incompatible numbered item must not receive a false measurement.
  Default action tolerance: +/- 1 ms; an approved mapping may override.
- Do not invent a measurement when mapping, confirmed upload, execution
  permission, reset/empty frame, physical KP, or an input timestamp is missing.
  Missing evidence is not the same as an observed hardware failure.
- Do not restore legacy GUI controls that upload to BCVM, set BCVM time,
  or stop the launch program. Restore old COM acquisition without restoring those controls.
- Keep `IntegrationSession`, validation, reset and evidence/report handling
  intact when refactoring presentation.
- Treat a new KASU run/upload, tester reconfiguration, and explicit new
  collection/reset as **evidence boundaries**. Rotate `collection_id`, discard
  prior collection frames/counters, and keep only relevant Debug evidence in the
  new report. Refuse report export when raw evidence could not be written.
- With two legacy COMs, track connection/receiving and KP8 **per stream**.
  The overall healthy/green indicator requires every configured stream AND a
  valid armed collection; one missing stream invalidates measurements until a
  new collection. A recovered UART feed must not auto-rearm the collection.
- Keep GUI `objectName` values stable for regression tests and avoid assigning
  a second name to the same widget.

## Validation

`ctest --test-dir build/stand --output-on-failure`
For GUI regressions build with `-DBUILD_GUI_TESTS=ON` and run
`tst_operator_gui` offscreen. Verify hardware wiring and the board firmware
protocol separately on the real stand.
