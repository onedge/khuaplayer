find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

set(KHUA_MEDIACORE_DIR ${PROJECT_SOURCE_DIR}/Modules/MediaCore)

# Each portable header must compile on its own. Headers that still include
# Apple frameworks are listed in Platform/Windows/README.md and join this list
# once they no longer do.
set(KHUA_PORTABLE_HEADERS
    Core/BoundedQueue.hpp
    Core/Demuxer.hpp
    Core/MatroskaCuesLocator.hpp
    Core/SPContainerRecovery.hpp
    Core/SPGopReadback.hpp
    Core/SPMp4InferredRecovery.hpp
    Core/SPReadSourceView.hpp
    Core/SPResilience.hpp
    Core/SPSourceGrowthPolicy.hpp
    Core/SPSourceInput.hpp
    Core/SPTrialDecode.hpp
    Core/SPTrialExecutor.hpp
    Core/SeekTargets.hpp
    Core/TsRapPolicy.hpp
    Core/TsRapScan.hpp
    Core/Platform/SPErrno.hpp
    Core/Platform/SPFileSystem.hpp
    Core/Platform/SPThread.hpp
    Core/Platform/SPWin32.hpp
    Core/Player/SPExecutor.hpp
    Core/Player/SPPlayerListener.hpp
    Core/Player/SPSync.hpp
    Core/Player/SPTasks.hpp
    Core/Player/SPVideoFrame.hpp
    Core/Recovery/RecoveryAsfRm.hpp
    Core/Recovery/RecoveryAvi.hpp
    Core/Recovery/RecoveryBase.hpp
    Core/Recovery/RecoveryFlv.hpp
    Core/Recovery/RecoveryMatroska.hpp
    Core/Recovery/RecoveryMkvContentMap.hpp
    Core/Recovery/RecoveryMp4.hpp
    Core/Recovery/RecoveryMp4ContentMap.hpp
    Core/Recovery/RecoveryMp4Window.hpp
    Core/Recovery/RecoveryMpeg.hpp
    Core/Recovery/RecoveryOgg.hpp
    Core/Recovery/RecoveryTsContentMap.hpp
    Core/Recovery/RecoveryTsPsi.hpp
    Core/Recovery/RecoveryTypes.hpp
    Core/Resilience/ResilienceAudioCodec.hpp
    Core/Resilience/ResilienceBitstream.hpp
    Core/Resilience/ResilienceDamageMap.hpp
    Core/Resilience/ResilienceOpenDiagnosis.hpp
    Core/Resilience/ResilienceVideoCodec.hpp
    Bridge/SPAnnexBStartCode.hpp
    Bridge/SPAudioChannelMap.hpp
    Bridge/SPAudioPeak.hpp
    Bridge/SPColorMetadata.hpp
    Bridge/SPAv1CatchUpSkip.hpp
    Bridge/SPContentCoverage.hpp
    Bridge/SPDoviRPU.hpp
    Bridge/SPFrameSelectionPolicy.hpp
    Bridge/SPPacketFixDispatch.hpp
    Bridge/SPPlayerSessionPolicy.hpp
    Bridge/SPResilientEndPolicy.hpp
    Bridge/SPRuntimeGates.hpp
    Bridge/SPSoftwareDecodePolicy.hpp
    Bridge/SPSubtitleCompositor.hpp
    Bridge/SPSubtitleSampleGate.hpp
    Bridge/SPSubtitleWatchdogGate.hpp
    Bridge/SPThumbBlackBorder.hpp
    Bridge/SPThumbKeyPolicy.hpp
    Bridge/SPThumbSchedulingPolicy.hpp
    Bridge/SPTimeStretch.hpp
    Bridge/SPVideoFrameQueueSizing.hpp
    Bridge/SPVideoRecoveryLane.hpp
)

set(KHUA_HEADER_CHECK_SOURCES)
foreach(header IN LISTS KHUA_PORTABLE_HEADERS)
    string(MAKE_C_IDENTIFIER ${header} id)
    set(source ${CMAKE_CURRENT_BINARY_DIR}/header_check/${id}.cpp)
    file(CONFIGURE OUTPUT ${source}
         CONTENT "#include \"${KHUA_MEDIACORE_DIR}/${header}\"\n")
    list(APPEND KHUA_HEADER_CHECK_SOURCES ${source})
endforeach()

add_library(khua_header_check OBJECT ${KHUA_HEADER_CHECK_SOURCES})
target_link_libraries(khua_header_check PRIVATE khua_media_portable)

add_executable(khua_mediacore_tests
    AnnexBStartCodeTests.cpp
    ColorAndChannelTests.cpp
    DemuxerTests.cpp
    ExecutorTests.cpp
    FFmpegConfigurationTests.cpp
    FileSystemTests.cpp
    FrameSelectionPolicyTests.cpp
    SubtitleCompositorTests.cpp
    TasksTests.cpp
    ThreadTests.cpp
    TsRapPolicyTests.cpp
    VideoFrameBridgeTests.cpp
    VideoFrameTests.cpp
)
target_link_libraries(khua_mediacore_tests
    PRIVATE
        khua_media_portable
        GTest::gtest_main
)
add_dependencies(khua_mediacore_tests khua_header_check)

gtest_discover_tests(khua_mediacore_tests DISCOVERY_MODE PRE_TEST)

add_test(NAME ffmpeg_overlay_matches_mac
         COMMAND ${CMAKE_COMMAND} -DKHUA_ROOT=${PROJECT_SOURCE_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckFFmpegOverlay.cmake)
