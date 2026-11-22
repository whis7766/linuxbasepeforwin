cmake_minimum_required(VERSION 3.16)
project(lbpe-gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WIMLIB REQUIRED IMPORTED_TARGET wimlib)
pkg_check_modules(HIVEX REQUIRED IMPORTED_TARGET hivex)

add_executable(lbpe-gui
    main.cpp
    MainWindow.cpp
    Backend.cpp
)

target_link_libraries(lbpe-gui PRIVATE
    Qt6::Widgets Qt6::Concurrent
    PkgConfig::WIMLIB PkgConfig::HIVEX
)

install(TARGETS lbpe-gui RUNTIME DESTINATION bin)