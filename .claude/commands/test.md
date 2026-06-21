Run all tests. Builds in Debug, runs ctest.

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build .
ctest --output-on-failure -V
```

Test targets:
- tst_analysis   — Stand::analyzeEvents() pure function
- tst_cyclogram  — cyclogram.ini parser
- tst_reading    — readingThread with MockSerial (all events, missed, port drop, stop)
- tst_logformat  — SessionLogger timestamp derivation, session folders

GUI tests (needs display or Xvfb):
```bash
cmake .. -DBUILD_GUI_TESTS=ON
cmake --build . --target tst_gui
QT_QPA_PLATFORM=offscreen ./tst_gui
```
