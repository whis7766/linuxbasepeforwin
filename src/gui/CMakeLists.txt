cmake_minimum_required(VERSION 3.16)
project(lbpe-gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WIMLIB REQUIRED IMPORTED_TARGET wimlib)
pkg_check_modules(HIVEX REQUIRED IMPORTED_TARGET hivex)
pkg_check_modules(LIBBDE QUIET IMPORTED_TARGET libbde)
if(LIBBDE_FOUND)
    message(STATUS "libbde found: BitLocker support enabled")
endif()

add_executable(lbpe-gui
    main.cpp
    MainWindow.cpp
    Backend.cpp
)

target_link_libraries(lbpe-gui PRIVATE
    Qt6::Widgets Qt6::Concurrent
    PkgConfig::WIMLIB PkgConfig::HIVEX
)
if(LIBBDE_FOUND)
    target_compile_definitions(lbpe-gui PRIVATE LBPE_HAVE_LIBBDE=1)
    target_link_libraries(lbpe-gui PRIVATE PkgConfig::LIBBDE)
endif()

install(TARGETS lbpe-gui RUNTIME DESTINATION bin)

# 宿主 Linux 功能自检（不走 Buildroot）：cmake -DBUILD_SELFTEST=ON
option(BUILD_SELFTEST "Build host-side backend self test" OFF)
if(BUILD_SELFTEST)
    add_executable(lbpe-selftest
        tests/selftest.cpp
        Backend.cpp
        MainWindow.cpp
    )
    target_include_directories(lbpe-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(lbpe-selftest PRIVATE
        Qt6::Widgets Qt6::Concurrent
        PkgConfig::WIMLIB PkgConfig::HIVEX
    )
    if(LIBBDE_FOUND)
        target_compile_definitions(lbpe-selftest PRIVATE LBPE_HAVE_LIBBDE=1)
        target_link_libraries(lbpe-selftest PRIVATE PkgConfig::LIBBDE)
    endif()
endif()