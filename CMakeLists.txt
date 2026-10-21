cmake_minimum_required(VERSION 3.16)
project(pyro_stand VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network SerialPort Test Concurrent)
qt_standard_project_setup()


set(PYRO_SOURCES
    main.cpp
    integration.cpp
    integration.h
    integration_window.cpp
    integration_window.h
    event_canvas.cpp
    event_canvas.h
    timeline_widget.cpp
    timeline_widget.h
    command_catalog.h
    types.h
    free_recording.cpp free_recording.h free_recording_dialog.cpp
)

add_executable(pyro_stand ${PYRO_SOURCES})
target_link_libraries(pyro_stand PRIVATE Qt6::Core Qt6::Widgets Qt6::Network Qt6::SerialPort Qt6::Concurrent)
target_include_directories(pyro_stand PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    set_target_properties(pyro_stand PROPERTIES WIN32_EXECUTABLE TRUE)
    target_compile_definitions(pyro_stand PRIVATE PYRO_WINDOWS)
elseif(UNIX)
    target_compile_definitions(pyro_stand PRIVATE PYRO_LINUX)
endif()

enable_testing()

# Проверки протоколов, аппаратных тиков и жизненного цикла без оборудования.
add_executable(tst_integration tests/tst_integration.cpp integration.cpp integration.h command_catalog.h)
target_include_directories(tst_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tst_integration PRIVATE STAND_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
target_link_libraries(tst_integration PRIVATE Qt6::Core Qt6::Test Qt6::Network Qt6::SerialPort)
add_test(NAME tst_integration COMMAND tst_integration)

# Проверки действующего интерфейса, в том числе полного плана и живых входов.
option(BUILD_GUI_TESTS "Собирать проверки операторского интерфейса Qt" ON)
if(BUILD_GUI_TESTS)
    add_executable(tst_operator_gui
        tests/tst_operator_gui.cpp
        integration.cpp integration.h
        integration_window.cpp integration_window.h
        event_canvas.cpp event_canvas.h
        timeline_widget.cpp timeline_widget.h
        command_catalog.h types.h
        free_recording.cpp free_recording.h free_recording_dialog.cpp
    )
    target_include_directories(tst_operator_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(tst_operator_gui PRIVATE STAND_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    target_link_libraries(tst_operator_gui PRIVATE Qt6::Core Qt6::Widgets Qt6::Test Qt6::Network Qt6::SerialPort Qt6::Concurrent)
    add_test(NAME tst_operator_gui COMMAND tst_operator_gui)
    set_tests_properties(tst_operator_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

qt_add_executable(tst_free_recording tests/tst_free_recording.cpp free_recording.cpp free_recording.h integration.cpp integration.h)
target_include_directories(tst_free_recording PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_free_recording PRIVATE Qt6::Core Qt6::Test Qt6::Network Qt6::SerialPort)
add_test(NAME tst_free_recording COMMAND tst_free_recording)

# Пассивный диагностический исполняемый файл не запускается автоматически.
option(BUILD_HARDWARE_PROBE "Собирать пассивную проверку связи с приборами" OFF)
if(BUILD_HARDWARE_PROBE)
    add_executable(live_io_probe tests/live_io_probe.cpp integration.cpp integration.h)
    target_include_directories(live_io_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(live_io_probe PRIVATE Qt6::Core Qt6::Network Qt6::SerialPort)
endif()

# Виртуальная сквозная проверка: UDP + локальный Debug + QSerialPort через PTY.
add_executable(tst_virtual_io tests/tst_virtual_io.cpp integration.cpp integration.h)
target_include_directories(tst_virtual_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tst_virtual_io PRIVATE Qt6::Core Qt6::Network Qt6::SerialPort Qt6::Test)
add_test(NAME tst_virtual_io COMMAND tst_virtual_io)

# Проверка запуска именно оконного исполняемого файла без аппаратуры.
add_test(NAME stand_offscreen_startup
         COMMAND pyro_stand --tester udp --board-ip 127.0.0.1
                 --bind-ip 127.0.0.1 --board-port 59031
                 --debug-socket pyro-ci-no-debug-server)
set_tests_properties(stand_offscreen_startup PROPERTIES
    TIMEOUT 15
    ENVIRONMENT "PYRO_STAND_SMOKE_TEST=1;QT_QPA_PLATFORM=offscreen")
