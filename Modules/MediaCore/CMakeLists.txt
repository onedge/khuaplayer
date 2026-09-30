# Platform-neutral part of KhuaPlayerMediaCore. Objective-C++ bridge files and
# Platform/macOS stay in the Xcode project.
find_package(FFMPEG REQUIRED)
find_package(ZLIB REQUIRED)

add_library(khua_media_portable STATIC
    Core/Platform/SPFileSystemPosix.cpp
    Core/Platform/SPFileSystemWin32.cpp
    Core/Platform/SPThreadPosix.cpp
    Core/Platform/SPThreadWin32.cpp
    Core/Demuxer.cpp
    Core/TsRapScan.cpp
    Bridge/SPDoviRPU.cpp
    Bridge/SPFrameSelectionPolicy.cpp
    Bridge/SPSubtitleCompositor.cpp
)

target_include_directories(khua_media_portable
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Core
        ${CMAKE_CURRENT_SOURCE_DIR}/Bridge
        ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(khua_media_portable
    PUBLIC
        ${FFMPEG_LIBRARIES}
        ZLIB::ZLIB
)

# Matches GCC_PREPROCESSOR_DEFINITIONS in Apps/Mac/project.yml.
target_compile_definitions(khua_media_portable PUBLIC SP_ENHANCE=1)

if(MSVC)
    # Keep <windows.h> from defining min/max and the rarely used APIs. The
    # portable sources use the standard C functions (fopen, getenv) on purpose.
    target_compile_definitions(khua_media_portable
        PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(khua_media_portable PUBLIC /utf-8)
endif()
