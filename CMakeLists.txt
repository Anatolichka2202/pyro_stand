cmake_minimum_required(VERSION 3.16)
project(pyro_stand VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network SerialPort)

set(SOURCES
    main.cpp
    mainwindow.cpp
    stand.cpp
)

set(HEADERS
    mainWindow.h
    stand.h
)

add_executable(pyro_stand ${SOURCES} ${HEADERS}
    cyclogram_data.h
)
target_link_libraries(pyro_stand
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::SerialPort
)

