cmake_minimum_required(VERSION 3.20)
project(io_uring_udp_bench LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "io_uring_udp_bench is Linux-only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing)
    if(LIBURING_FOUND AND LIBURING_VERSION VERSION_LESS "2.4")
        message(FATAL_ERROR "liburing >= 2.4 is required (found ${LIBURING_VERSION})")
    endif()
endif()

add_executable(udp_bench
    src/main.cpp
)

target_compile_definitions(udp_bench PRIVATE _GNU_SOURCE)

target_compile_options(udp_bench PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)

target_link_libraries(udp_bench PRIVATE Threads::Threads)

if(TARGET PkgConfig::LIBURING)
    target_link_libraries(udp_bench PRIVATE PkgConfig::LIBURING)
else()
    find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)
    find_library(LIBURING_LIBRARY NAMES uring)

    if(NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
        message(FATAL_ERROR
            "liburing was not found. Install the liburing development package.\n"
            "Arch/CachyOS/SteamOS dev environment: sudo pacman -S liburing\n"
            "Debian/Ubuntu: sudo apt install liburing-dev")
    endif()

    target_include_directories(udp_bench PRIVATE ${LIBURING_INCLUDE_DIR})
    target_link_libraries(udp_bench PRIVATE ${LIBURING_LIBRARY})
endif()
