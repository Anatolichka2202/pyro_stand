#pragma once
#include <chrono>

#ifdef _WIN32
#include <windows.h>

inline std::chrono::system_clock::time_point now_utc_precise() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr uint64_t epochShift = 116444736000000000ULL; // 1601 -> 1970 в 100 нс
    t -= epochShift;
    // 1 tick = 100 нс, переводим в наносекунды
    auto duration = std::chrono::nanoseconds(t * 100);
    // Явное преобразование через duration_cast
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(duration)
        );
}

#else
#include <time.h>

inline std::chrono::system_clock::time_point now_utc_precise() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    auto duration = std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(duration)
        );
}
#endif
