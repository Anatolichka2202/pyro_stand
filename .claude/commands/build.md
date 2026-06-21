Build the project. Usage: /build [stand|demo|all|tests]

- stand (default): production GUI
- demo: pyro_demo (no hardware)
- all: both stand + demo
- tests: unit + integration tests only

```bash
mkdir -p build && cd build
# stand
cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . --target pyro_stand
# demo
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=ON && cmake --build . --target pyro_demo
# tests
cmake .. -DCMAKE_BUILD_TYPE=Debug && cmake --build . && ctest --output-on-failure -V
```

On Linux run build.sh, on Windows run build.bat.
