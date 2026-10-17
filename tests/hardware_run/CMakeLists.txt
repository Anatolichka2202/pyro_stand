cmake_minimum_required(VERSION 3.21)
project(pyro_hardware_run LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Network SerialPort Concurrent)
qt_standard_project_setup(REQUIRES 6.8)

# Используем действующие библиотеки КАСУПП; его исходники не изменяем.
set(KASU_SOURCE "" CACHE PATH "Локальные исходники КАСУПП")
if(NOT EXISTS "${KASU_SOURCE}/src/core/kasu_controller.cpp")
    message(FATAL_ERROR "Укажите KASU_SOURCE")
endif()
set(KASU_BUILD_PRODUCT OFF CACHE BOOL "" FORCE)
set(KASU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KASU_BUILD_BCVM_DEBUG_MODULE ON CACHE BOOL "" FORCE)
add_subdirectory("${KASU_SOURCE}" kasu-build)
qt_add_executable(pyro_hardware_run
    main.cpp ../../integration.cpp ../../integration.h
    ../../integration_window.cpp ../../integration_window.h
    ../../event_canvas.cpp ../../event_canvas.h ../../timeline_widget.cpp ../../timeline_widget.h
    ${KASU_SOURCE}/src/debug/bcvm_debug_module.cpp
    ${KASU_SOURCE}/src/debug/bcvm_debug_module.h)
target_include_directories(pyro_hardware_run PRIVATE ../.. ${KASU_SOURCE}/src)
target_compile_definitions(pyro_hardware_run PRIVATE KASU_ENABLE_BCVM_DEBUG_MODULE=1)
target_link_libraries(pyro_hardware_run PRIVATE kasu_core kasu_bus kasu_runtime
    Qt6::Core Qt6::Widgets Qt6::Network Qt6::SerialPort Qt6::Concurrent)
# Намеренно не регистрируем этот запуск в CTest.
add_executable(pyro_uart_loopback uart_loopback.cpp)
target_link_libraries(pyro_uart_loopback PRIVATE Qt6::Core Qt6::SerialPort)
