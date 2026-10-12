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
- The primary table displays events explicitly mapped in the approved
  action-to-input configuration. Multichannel actions may have indented
  per-input subrows. **Never add a 96-channel diagnostic table**, tab, or panel.
- Planned T0 UTC originates from the cyclogram published by KASU PP.
  PC UTC countdown is only a planning indication; it is not measured T0.
- Measured relative time = board hardware timestamp minus the physical
  KP timestamp on the same board counter; delta = measured - planned.
  Default action tolerance: +/- 1 ms; an approved mapping may override.
- Do not invent a measurement when mapping, confirmed upload, execution
  permission, reset/empty frame, physical KP, or an input timestamp is missing.
  Missing evidence is not the same as an observed hardware failure.
- Do not restore legacy GUI controls that upload to BCVM, set BCVM time,
  stop the launch program, or interpret an old one-byte-per-ms stream.
- Keep `IntegrationSession`, validation, reset and evidence/report handling
  intact when refactoring presentation.

## Validation

`ctest --test-dir build/stand --output-on-failure`
For GUI regressions build with `-DBUILD_GUI_TESTS=ON` and run
`tst_operator_gui` offscreen. Verify hardware wiring and the board firmware
protocol separately on the real stand.
