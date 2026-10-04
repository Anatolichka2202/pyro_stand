cmake_minimum_required(VERSION 3.16)
project(pyro_stand VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network SerialPort)

# ── Основное приложение ──────────────────────────────────────────────────────

add_executable(pyro_stand
    main.cpp
    integration.cpp
    integration.h
    integration_window.cpp
    integration_window.h
)

target_link_libraries(pyro_stand
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::SerialPort
)

if(WIN32)
    target_compile_definitions(pyro_stand PRIVATE PYRO_WINDOWS)
elseif(UNIX)
    target_compile_definitions(pyro_stand PRIVATE PYRO_LINUX)
endif()

# ── Тесты ────────────────────────────────────────────────────────────────────

enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Test)

# Unit / integration tests (no GUI required)
function(add_stand_test name src)
    add_executable(${name} ${src}
        stand.cpp stand.h platform.h types.h
        serial_port.h real_serial_port.h
    )
    target_link_libraries(${name}
        Qt6::Core Qt6::Test Qt6::Network Qt6::SerialPort)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_stand_test(tst_analysis  tests/tst_analysis.cpp)
add_stand_test(tst_cyclogram tests/tst_cyclogram.cpp)
add_stand_test(tst_reading   tests/tst_reading.cpp)

add_executable(tst_integration tests/tst_integration.cpp integration.cpp integration.h)
target_include_directories(tst_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_integration Qt6::Core Qt6::Test Qt6::Network Qt6::SerialPort)
add_test(NAME tst_integration COMMAND tst_integration)

# Trace-log format tests (no GUI, no hardware)
add_executable(tst_logformat tests/tst_logformat.cpp session_logger.cpp session_logger.h)
target_link_libraries(tst_logformat Qt6::Core Qt6::Test)
add_test(NAME tst_logformat COMMAND tst_logformat)

# GUI simulation tests (require QApplication + Widgets)
option(BUILD_GUI_TESTS "Build GUI simulation tests (requires display or Xvfb)" OFF)

if(BUILD_GUI_TESTS)
    add_executable(tst_gui
        tests/tst_gui.cpp
        mainwindow.cpp
        mainwindow.h
        stand.cpp
        stand.h
        platform.h
        session_logger.cpp
        session_logger.h
        types.h
        timeline_widget.cpp
        timeline_widget.h
    )
    target_include_directories(tst_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_gui
        Qt6::Core
        Qt6::Widgets
        Qt6::Test
        Qt6::Network
        Qt6::SerialPort
    )
    add_test(NAME tst_gui COMMAND tst_gui)
    set_tests_properties(tst_gui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
