# Portable media-core build. The Mac app is still built by Apps/Mac/Scripts/build.sh
# (XcodeGen); this project compiles the platform-neutral C++ sources for the
# Windows port and for host-side unit tests. See Platform/Windows/README.md.
cmake_minimum_required(VERSION 3.25)

project(KhuaCore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KHUA_BUILD_TESTS "Build portable media-core unit tests" ON)

add_subdirectory(Modules/MediaCore)

if(KHUA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Platform/Windows/Tests/MediaCore)
endif()
