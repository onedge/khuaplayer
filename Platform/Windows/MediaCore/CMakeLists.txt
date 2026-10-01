# The Windows media core. Core/ and Bridge/ started as copies of the portable
# part of Modules/MediaCore and are now maintained here, independently of the
# Mac app, which keeps its own code under Modules/. See ../README.md.
find_package(FFMPEG REQUIRED)
find_package(ZLIB REQUIRED)

# The renderer's shaders, compiled at build time by the Windows SDK's fxc
# (on PATH in a Visual Studio developer environment) into bytecode headers.
find_program(KHUA_FXC fxc REQUIRED)
set(KHUA_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/Output/Shaders/Video.hlsl)
set(KHUA_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${KHUA_SHADER_DIR})
function(khua_compile_shader output profile entry variable)
    add_custom_command(
        OUTPUT ${KHUA_SHADER_DIR}/${output}
        COMMAND ${KHUA_FXC} /nologo /WX /O3 /Qstrip_reflect /T ${profile} /E ${entry}
                /Vn ${variable} /Fh ${KHUA_SHADER_DIR}/${output} ${KHUA_SHADER_SOURCE}
        DEPENDS ${KHUA_SHADER_SOURCE}
        COMMENT "Compiling ${entry} (${profile})"
        VERBATIM)
endfunction()
khua_compile_shader(SPVideoVS.h vs_5_0 videoVertex g_videoVS)
khua_compile_shader(SPVideoPS.h ps_5_0 videoFragment g_videoPS)

add_library(khua_media_portable STATIC
    Core/Platform/SPFileSystemWin32.cpp
    Core/Platform/SPThreadWin32.cpp
    Core/Player/SPExecutor.cpp
    Core/Player/SPTasks.cpp
    Core/Player/SPThreadTasks.cpp
    Core/Player/SPAVFrameRef.cpp
    Core/Audio/SPAudioEngine.cpp
    Core/Demuxer.cpp
    Core/TsRapScan.cpp
    Output/SPWasapiAudioSink.cpp
    Output/SPD3D11Device.cpp
    Output/SPD3D11Renderer.cpp
    Output/SPOffscreenRenderTarget.cpp
    Decode/SPFFmpegVideoDecoder.cpp
    ${KHUA_SHADER_DIR}/SPVideoVS.h
    ${KHUA_SHADER_DIR}/SPVideoPS.h
    Bridge/SPDoviRPU.cpp
    Bridge/SPFrameSelectionPolicy.cpp
    Bridge/SPSubtitleCompositor.cpp
)

target_include_directories(khua_media_portable
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Core
        ${CMAKE_CURRENT_SOURCE_DIR}/Bridge
        ${CMAKE_CURRENT_SOURCE_DIR}/Output
        ${CMAKE_CURRENT_SOURCE_DIR}/Decode
        ${FFMPEG_INCLUDE_DIRS}
    PRIVATE
        ${KHUA_SHADER_DIR}
)

target_link_libraries(khua_media_portable
    PUBLIC
        ${FFMPEG_LIBRARIES}
        ZLIB::ZLIB
        # WASAPI output: COM activation and MMCSS thread priority.
        ole32
        avrt
        # Video output and D3D11VA decoding (dxguid: the decoder profile GUIDs).
        d3d11
        dxgi
        dxguid
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
