Build and run demo mode (full GUI, no hardware required).

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=ON
cmake --build . --target pyro_demo
./pyro_demo        # Linux
# or Release\pyro_demo.exe  # Windows
```

Demo runs a scripted 5-second countdown with 4 pyrotechnic events via MockSerial.
MockSerial::setRealtime(true) gives real 1 ms/byte pacing for human-speed playback.
No COM port, no БЦВМ, no network needed.
