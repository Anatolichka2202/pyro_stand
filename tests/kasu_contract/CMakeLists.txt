cmake_minimum_required(VERSION 3.19)
project(kasu_stand_contract_check LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Реальные исходники КАСУПП выбираются явно, без путей конкретного рабочего ПК.
set(KASU_SOURCE "" CACHE PATH "Checkout каталога исходников КАСУПП")
if(NOT KASU_SOURCE OR NOT EXISTS "${KASU_SOURCE}/src/debug/bcvm_debug_module.cpp")
    message(FATAL_ERROR "Укажите -DKASU_SOURCE=<каталог-исходников-КАСУПП>")
endif()
set(KASU_BUILD_PRODUCT OFF CACHE BOOL "" FORCE)
set(KASU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KASU_BUILD_BCVM_DEBUG_MODULE ON CACHE BOOL "" FORCE)

add_subdirectory("${KASU_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/kasu-build")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network SerialPort Concurrent)

add_executable(kasu_stand_contract_check
    check.cpp
    ../../integration.cpp
    ../../integration.h
    ${KASU_SOURCE}/src/debug/bcvm_debug_module.cpp
    ${KASU_SOURCE}/src/debug/bcvm_debug_module.h
)
target_include_directories(kasu_stand_contract_check PRIVATE
    ../.. ${KASU_SOURCE}/src)
target_compile_definitions(kasu_stand_contract_check
    PRIVATE KASU_ENABLE_BCVM_DEBUG_MODULE=1)
target_link_libraries(kasu_stand_contract_check PRIVATE
    kasu_bus kasu_runtime kasu_protocol
    Qt6::Core Qt6::Network Qt6::SerialPort Qt6::Concurrent)
enable_testing()
add_test(NAME kasu_stand_real_publisher COMMAND kasu_stand_contract_check)
set_tests_properties(kasu_stand_real_publisher PROPERTIES TIMEOUT 30)
