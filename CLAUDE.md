# Instructions for agents working on pyro_stand

## Current implementation

Read [README.md](README.md) and [CMakeLists.txt](CMakeLists.txt) first. The production executable uses `main.cpp`, `integration.cpp` and `integration_window.cpp`. It observes the KASU PP debug contract and receives measurement data; the current application does not upload a cyclogram to BCVM.

## Legacy code

The earlier `Stand` / `MainWindow` implementation remains for historical code and legacy tests. Its design and operating instructions are described in [docs/LEGACY_README.md](docs/LEGACY_README.md). Do not treat legacy transport, command, timing or channel assumptions as requirements for the current integration implementation.

## Changes

- Preserve the current contract and approved measurement mapping when performing repository cleanup.
- Reuse existing parsers, adapters and analysis before adding another implementation.
- Keep UI, session integration and device transport responsibilities explicit.
- Use mock/fake tests for software verification; do not claim hardware acceptance from them.
- Select available tests from the current CMake targets. Build outputs and personal IDE settings must not be committed.
