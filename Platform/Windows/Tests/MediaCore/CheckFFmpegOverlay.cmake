# cmake -DKHUA_ROOT=<repo> -P CheckFFmpegOverlay.cmake
#
# Fails when the Windows FFmpeg overlay port drifts from the Mac build:
#   - the component whitelists in Scripts/build_ffmpeg_min.sh, and
#   - the patches in Scripts/patches.
if(NOT KHUA_ROOT)
    message(FATAL_ERROR "KHUA_ROOT is required")
endif()
set(port "${KHUA_ROOT}/Platform/Windows/vcpkg/ports/ffmpeg")

# Whitelists: every variable the Mac configure line passes to --enable-<kind>=
# must have a KHUA_FFMPEG_<NAME> counterpart with the same value, so a list
# added there fails here until it is mirrored.
include("${port}/khua-components.cmake")
set(script "${KHUA_ROOT}/Scripts/build_ffmpeg_min.sh")
file(STRINGS "${script}" enable_lines REGEX "--enable-[a-z]+=\"\\$")
set(names)
foreach(line IN LISTS enable_lines)
    string(REGEX MATCHALL "\\$[A-Z_]+" refs "${line}")
    foreach(ref IN LISTS refs)
        string(SUBSTRING "${ref}" 1 -1 name)
        list(APPEND names ${name})
    endforeach()
endforeach()
list(REMOVE_DUPLICATES names)
set(lists 0)
foreach(name IN LISTS names)
    file(STRINGS "${script}" definition REGEX "^${name}=")
    list(LENGTH definition count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "expected one ${name}= line in Scripts/build_ffmpeg_min.sh, found ${count}")
    endif()
    string(REGEX REPLACE "^${name}=" "" mac "${definition}")
    if(NOT DEFINED KHUA_FFMPEG_${name})
        message(FATAL_ERROR "Scripts/build_ffmpeg_min.sh defines ${name}, which "
                            "khua-components.cmake does not mirror as KHUA_FFMPEG_${name}")
    endif()
    if(NOT mac STREQUAL KHUA_FFMPEG_${name})
        message(FATAL_ERROR
            "${name} differs from Scripts/build_ffmpeg_min.sh\n"
            "  Mac:     ${mac}\n"
            "  Windows: ${KHUA_FFMPEG_${name}}\n"
            "Update ${port}/khua-components.cmake.")
    endif()
    math(EXPR lists "${lists} + 1")
endforeach()
if(lists LESS 6)
    message(FATAL_ERROR "found only ${lists} whitelist variables in Scripts/build_ffmpeg_min.sh")
endif()

# Patches. The overlay copy of the mov patch additionally ends with the blank
# context line (a single space) that git apply requires; see README.md.
file(GLOB mac_patches "${KHUA_ROOT}/Scripts/patches/*.patch")
set(patches 0)
foreach(mac_patch IN LISTS mac_patches)
    get_filename_component(name "${mac_patch}" NAME)
    if(NOT EXISTS "${port}/${name}")
        message(FATAL_ERROR "Scripts/patches/${name} is missing from the overlay port")
    endif()
    file(READ "${mac_patch}" expected)
    if(name STREQUAL "ffmpeg-mov-multistsd-seek.patch")
        string(APPEND expected " \n")
    endif()
    file(READ "${port}/${name}" actual)
    if(NOT expected STREQUAL actual)
        message(FATAL_ERROR "${port}/${name} differs from Scripts/patches/${name}")
    endif()
    file(STRINGS "${port}/portfile.cmake" listed REGEX "^ +${name}$")
    if(NOT listed)
        message(FATAL_ERROR "${name} is not applied by ${port}/portfile.cmake")
    endif()
    math(EXPR patches "${patches} + 1")
endforeach()

message(STATUS "FFmpeg overlay matches the Mac build (${lists} lists, ${patches} patches)")
