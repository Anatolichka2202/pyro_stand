#pragma once

// Platform-specific hardware defaults.
// Pass explicit values to Stand's constructor to override.
#ifdef _WIN32
  static constexpr const char* DEFAULT_SERIAL_PORT = "COM7";
#else
  static constexpr const char* DEFAULT_SERIAL_PORT = "/dev/ttyUSB0";
#endif
