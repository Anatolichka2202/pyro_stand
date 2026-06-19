cmake_minimum_required(VERSION 3.16)
project(pyro_stand VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network SerialPort)

# ── Основное приложение ──────────────────────────────────────────────────────

add_executable(pyro_stand
    main.cpp
    mainwindow.cpp
    mainwindow.h
    stand.cpp
    stand.h
    session_logger.cpp
    session_logger.h
    types.h
    timeline_widget.cpp
    timeline_widget.h
)

target_link_libraries(pyro_stand
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::SerialPort
)

# ── Тесты ────────────────────────────────────────────────────────────────────

enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Test)

function(add_stand_test name src)
    add_executable(${name} ${src} stand.cpp stand.h types.h serial_port.h real_serial_port.h)
    target_link_libraries(${name}
        Qt6::Core Qt6::Test Qt6::Network Qt6::SerialPort)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_stand_test(tst_analysis    tests/tst_analysis.cpp)
add_stand_test(tst_cyclogram   tests/tst_cyclogram.cpp)
add_stand_test(tst_reading     tests/tst_reading.cpp)
