cmake_minimum_required(VERSION 3.24)
project(LowLatencyCaptureViewer VERSION 1.2.10 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WIN32_EXECUTABLE ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(LowLatencyCaptureViewer
    src/main.cpp
    src/app.rc
    src/audio/AsioOutput.cpp
    src/audio/AudioMix.cpp
    src/audio/AudioDeviceCapabilities.cpp
    src/audio/WasapiOutput.cpp
    src/audio/CaptureAudioFormat.cpp
    src/audio/PcmPipeline.cpp
    src/capture/DirectShowDevices.cpp
    src/capture/AudioSampleGrabber.cpp
    src/capture/DirectShowGraphResources.cpp
    src/capture/LatestVideoSample.cpp
    src/diagnostics/Logger.cpp
    src/diagnostics/AudioErrorHistory.cpp
    src/settings/SettingsStore.cpp
    src/ui/AudioOsdLayout.cpp
    src/ui/PresentationModeUi.cpp
    src/ui/SettingsView.cpp
    src/ui/SettingsDialogControls.cpp
    src/ui/UiText.cpp
    src/ui/WindowGeometry.cpp
    src/update/UpdateChecker.cpp
    src/video/DirectShowVideoFormat.cpp
    src/video/MjpegDecoder.cpp
    src/video/VideoColor.cpp
    src/video/HdrDisplay.cpp
)

target_include_directories(LowLatencyCaptureViewer PRIVATE
    src
    third_party/asio/common
    third_party/asio/host
    third_party/asio/host/pc
)

option(LLCV_HDR_FRAME_AUDIT "Build a private F8 single-frame HDR diagnostic (not for release)" OFF)
option(LLCV_HDR_SCRGB_PROTOTYPE "Private direct P010 shader/scRGB comparison (not for release)" OFF)
option(LLCV_EXPERIMENTAL_HARDWARE_TONEMAP "Private vendor HDR control experiment (not for release)" OFF)
if (LLCV_HDR_SCRGB_PROTOTYPE)
    set(LLCV_HDR_FRAME_AUDIT ON)
    target_compile_definitions(LowLatencyCaptureViewer PRIVATE LLCV_HDR_SCRGB_PROTOTYPE)
endif()
if (LLCV_EXPERIMENTAL_HARDWARE_TONEMAP)
    if (NOT LLCV_HDR_FRAME_AUDIT)
        message(FATAL_ERROR "Experimental vendor HDR control requires LLCV_HDR_FRAME_AUDIT=ON; it must not enter a release build.")
    endif()
    target_sources(LowLatencyCaptureViewer PRIVATE src/capture/HardwareToneMapping.cpp)
    target_compile_definitions(LowLatencyCaptureViewer PRIVATE LLCV_EXPERIMENTAL_HARDWARE_TONEMAP)
endif()
if (LLCV_HDR_FRAME_AUDIT)
    target_compile_definitions(LowLatencyCaptureViewer PRIVATE LLCV_HDR_FRAME_AUDIT)
    set_target_properties(LowLatencyCaptureViewer PROPERTIES OUTPUT_NAME "LowLatencyCaptureViewer_HDR_Audit")
endif()
if (LLCV_HDR_SCRGB_PROTOTYPE)
    set_target_properties(LowLatencyCaptureViewer PROPERTIES OUTPUT_NAME "LowLatencyCaptureViewer_HDR_Compare")
endif()

# Steinberg's host-side ASIO helper is compiled into the optional backend.
# Driver DLLs are not bundled; installed ASIO drivers are discovered at runtime.
target_sources(LowLatencyCaptureViewer PRIVATE
    third_party/asio/common/asio.cpp
    third_party/asio/host/asiodrivers.cpp
    third_party/asio/host/pc/asiolist.cpp
)
set_source_files_properties(
    third_party/asio/common/asio.cpp
    third_party/asio/host/asiodrivers.cpp
    third_party/asio/host/pc/asiolist.cpp
    PROPERTIES COMPILE_OPTIONS "/UUNICODE;/U_UNICODE"
)

target_compile_definitions(LowLatencyCaptureViewer PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(LowLatencyCaptureViewer PRIVATE
    ole32
    oleaut32
    strmiids
    uuid
    avrt
    d3d11
    dxgi
    d2d1
    dwrite
    d3dcompiler
    shcore
    comctl32
    mfplat
    mfuuid
    advapi32
    winhttp
)

if (MSVC)
    target_compile_options(LowLatencyCaptureViewer PRIVATE /W4 /EHsc /permissive- /utf-8)
endif()

include(CTest)
if (BUILD_TESTING)
    add_executable(SurroundAudioTests tests/SurroundAudioTests.cpp
        src/capture/AudioSampleGrabber.cpp
        src/audio/CaptureAudioFormat.cpp src/audio/PcmPipeline.cpp src/audio/AudioMix.cpp)
    target_include_directories(SurroundAudioTests PRIVATE src)
    target_compile_definitions(SurroundAudioTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(SurroundAudioTests PRIVATE strmiids ole32)
    set_target_properties(SurroundAudioTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(SurroundAudioTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME SurroundAudioTests COMMAND SurroundAudioTests)
    add_executable(HardwareToneMappingTests tests/HardwareToneMappingTests.cpp
        src/capture/HardwareToneMapping.cpp)
    target_include_directories(HardwareToneMappingTests PRIVATE src)
    target_compile_definitions(HardwareToneMappingTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(HardwareToneMappingTests PRIVATE strmiids uuid)
    set_target_properties(HardwareToneMappingTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(HardwareToneMappingTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME HardwareToneMappingTests COMMAND HardwareToneMappingTests)
    add_executable(HdrPolicyTests tests/HdrPolicyTests.cpp)
    target_include_directories(HdrPolicyTests PRIVATE src)
    target_compile_definitions(HdrPolicyTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(HdrPolicyTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME HdrPolicyTests COMMAND HdrPolicyTests)
    add_executable(OutputTransitionStateTests tests/OutputTransitionStateTests.cpp)
    target_include_directories(OutputTransitionStateTests PRIVATE src)
    set_target_properties(OutputTransitionStateTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME OutputTransitionStateTests COMMAND OutputTransitionStateTests)

    add_executable(SettingsViewTests tests/SettingsViewTests.cpp
        src/ui/SettingsView.cpp src/ui/PresentationModeUi.cpp
        src/ui/SettingsDialogControls.cpp src/ui/UiText.cpp)
    target_include_directories(SettingsViewTests PRIVATE src)
    target_compile_definitions(SettingsViewTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_link_libraries(SettingsViewTests PRIVATE comctl32)
    set_target_properties(SettingsViewTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(OutputTransitionStateTests PRIVATE /W4 /EHsc /permissive- /utf-8)
        target_compile_options(SettingsViewTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME SettingsViewTests COMMAND SettingsViewTests)
    # Native control creation is substantially slower than pure geometry tests,
    # especially when other replay tests run concurrently on CI.
    set_tests_properties(SettingsViewTests PROPERTIES TIMEOUT 120)
    add_executable(LatestVideoSampleTests tests/LatestVideoSampleTests.cpp
        src/capture/LatestVideoSample.cpp
        src/capture/DirectShowGraphResources.cpp)
    target_include_directories(LatestVideoSampleTests PRIVATE src)
    target_compile_definitions(LatestVideoSampleTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(LatestVideoSampleTests PRIVATE ole32 strmiids uuid)
    set_target_properties(LatestVideoSampleTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(LatestVideoSampleTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME LatestVideoSampleTests COMMAND LatestVideoSampleTests)
    set_tests_properties(LatestVideoSampleTests PROPERTIES TIMEOUT 15)
    # Exercise application callbacks and the ASIO notification protocol without
    # enumerating/opening hardware. This target is never installed.
    # These test translation units include the implementation directly. Keep
    # that dependency explicit even when a localized MSVC include scan misses it.
    set_property(SOURCE tests/AudioCallbackTests.cpp tests/HdrPipelineTests.cpp
        tests/MonitorMoveTests.cpp tests/SharedReplayTests.cpp
        APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    get_target_property(audio_callback_sources LowLatencyCaptureViewer SOURCES)
    list(FILTER audio_callback_sources EXCLUDE REGEX "src/(main.cpp|app.rc|audio/AsioOutput.cpp)$")
    add_executable(AudioCallbackTests tests/AudioCallbackTests.cpp ${audio_callback_sources})
    target_include_directories(AudioCallbackTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
    target_compile_definitions(AudioCallbackTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>)
    target_link_libraries(AudioCallbackTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,LINK_LIBRARIES>)
    set_target_properties(AudioCallbackTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(AudioCallbackTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME AudioCallbackTests COMMAND AudioCallbackTests)

    add_executable(HdrPipelineTests tests/HdrPipelineTests.cpp ${audio_callback_sources})
    target_include_directories(HdrPipelineTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
    target_compile_definitions(HdrPipelineTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>)
    target_link_libraries(HdrPipelineTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,LINK_LIBRARIES>)
    set_target_properties(HdrPipelineTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(HdrPipelineTests PRIVATE /W4 /EHsc /permissive- /utf-8)
        target_compile_options(HdrPolicyTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME HdrPipelineTests COMMAND HdrPipelineTests)
    set_tests_properties(HdrPipelineTests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS GPU)

    add_executable(MonitorMoveTests tests/MonitorMoveTests.cpp
        ${audio_callback_sources} src/audio/AsioOutput.cpp)
    target_include_directories(MonitorMoveTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
    target_compile_definitions(MonitorMoveTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>)
    target_link_libraries(MonitorMoveTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,LINK_LIBRARIES>)
    set_target_properties(MonitorMoveTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(MonitorMoveTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME MonitorMoveTests COMMAND MonitorMoveTests)
    set_tests_properties(MonitorMoveTests PROPERTIES TIMEOUT 30)

    add_executable(SharedReplayTests tests/SharedReplayTests.cpp
        ${audio_callback_sources} src/audio/AsioOutput.cpp)
    target_include_directories(SharedReplayTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
    target_compile_definitions(SharedReplayTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>)
    target_link_libraries(SharedReplayTests PRIVATE
        $<TARGET_PROPERTY:LowLatencyCaptureViewer,LINK_LIBRARIES>)
    set_target_properties(SharedReplayTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(SharedReplayTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME SharedReplayTests COMMAND SharedReplayTests)
    add_test(NAME SharedSurroundReplay COMMAND SharedReplayTests --surround)
    set_tests_properties(SharedSurroundReplay PROPERTIES TIMEOUT 240)
    set_tests_properties(SharedReplayTests PROPERTIES TIMEOUT 180)
    add_test(NAME SharedReplayExtended COMMAND SharedReplayTests --extended)
    set_tests_properties(SharedReplayExtended PROPERTIES TIMEOUT 240)
    add_test(NAME SharedReplayTarget25 COMMAND SharedReplayTests --target25)
    set_tests_properties(SharedReplayTarget25 PROPERTIES TIMEOUT 240)

    add_executable(AudioModuleTests
        tests/AudioModuleTests.cpp
        src/audio/AudioMix.cpp
        src/audio/CaptureAudioFormat.cpp
        src/audio/PcmPipeline.cpp
        src/ui/AudioOsdLayout.cpp
    )
    set_target_properties(AudioModuleTests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_include_directories(AudioModuleTests PRIVATE src)
    target_link_libraries(AudioModuleTests PRIVATE strmiids)
    target_compile_definitions(AudioModuleTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    if (MSVC)
        target_compile_options(AudioModuleTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME AudioModuleTests COMMAND AudioModuleTests)

    add_executable(AudioDeviceCapabilitiesTests
        tests/AudioDeviceCapabilitiesTests.cpp
        src/audio/AudioDeviceCapabilities.cpp
    )
    target_include_directories(AudioDeviceCapabilitiesTests PRIVATE src)
    target_compile_definitions(AudioDeviceCapabilitiesTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(AudioDeviceCapabilitiesTests PRIVATE
        ole32
        oleaut32
        uuid
        avrt
    )
    set_target_properties(AudioDeviceCapabilitiesTests PROPERTIES
        WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(AudioDeviceCapabilitiesTests PRIVATE
            /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME AudioDeviceCapabilitiesTests
             COMMAND AudioDeviceCapabilitiesTests)

    add_executable(LoggerTests
        tests/LoggerTests.cpp
        src/diagnostics/Logger.cpp
    )
    target_include_directories(LoggerTests PRIVATE src)
    target_compile_definitions(LoggerTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    set_target_properties(LoggerTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(LoggerTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME LoggerTests COMMAND LoggerTests)

    add_executable(VideoColorTests
        tests/VideoColorTests.cpp
        src/video/VideoColor.cpp
    )
    target_include_directories(VideoColorTests PRIVATE src)
    target_compile_definitions(VideoColorTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    set_target_properties(VideoColorTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(VideoColorTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME VideoColorTests COMMAND VideoColorTests)

    add_executable(DirectShowVideoFormatTests
        tests/DirectShowVideoFormatTests.cpp
        src/video/DirectShowVideoFormat.cpp
    )
    target_include_directories(DirectShowVideoFormatTests PRIVATE src)
    target_compile_definitions(DirectShowVideoFormatTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(DirectShowVideoFormatTests PRIVATE
        ole32
        strmiids
        mfuuid
    )
    set_target_properties(DirectShowVideoFormatTests PROPERTIES
        WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(DirectShowVideoFormatTests PRIVATE
            /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME DirectShowVideoFormatTests COMMAND DirectShowVideoFormatTests)

    add_executable(DirectShowDevicesTests
        tests/DirectShowDevicesTests.cpp
        src/capture/DirectShowDevices.cpp
    )
    target_include_directories(DirectShowDevicesTests PRIVATE src)
    target_compile_definitions(DirectShowDevicesTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(DirectShowDevicesTests PRIVATE
        ole32
        oleaut32
        strmiids
    )
    set_target_properties(DirectShowDevicesTests PROPERTIES
        WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(DirectShowDevicesTests PRIVATE
            /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME DirectShowDevicesTests COMMAND DirectShowDevicesTests)

    add_executable(SettingsStoreTests
        tests/SettingsStoreTests.cpp
        src/settings/SettingsStore.cpp
    )
    target_include_directories(SettingsStoreTests PRIVATE src)
    target_compile_definitions(SettingsStoreTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    set_target_properties(SettingsStoreTests PROPERTIES
        WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(SettingsStoreTests PRIVATE
            /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME SettingsStoreTests COMMAND SettingsStoreTests)

    add_executable(UpdateCheckerTests
        tests/UpdateCheckerTests.cpp
        src/update/UpdateChecker.cpp
    )
    target_include_directories(UpdateCheckerTests PRIVATE src)
    target_compile_definitions(UpdateCheckerTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(UpdateCheckerTests PRIVATE winhttp)
    set_target_properties(UpdateCheckerTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(UpdateCheckerTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME UpdateCheckerTests COMMAND UpdateCheckerTests)

    add_executable(WindowGeometryTests
        tests/WindowGeometryTests.cpp
        src/ui/WindowGeometry.cpp
    )
    target_include_directories(WindowGeometryTests PRIVATE src)
    target_compile_definitions(WindowGeometryTests PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    set_target_properties(WindowGeometryTests PROPERTIES
        WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(WindowGeometryTests PRIVATE
            /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME WindowGeometryTests COMMAND WindowGeometryTests)

    add_executable(AudioErrorHistoryTests
        tests/AudioErrorHistoryTests.cpp
        src/diagnostics/AudioErrorHistory.cpp
    )
    target_include_directories(AudioErrorHistoryTests PRIVATE src)
    set_target_properties(AudioErrorHistoryTests PROPERTIES
        WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(AudioErrorHistoryTests PRIVATE
            /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME AudioErrorHistoryTests COMMAND AudioErrorHistoryTests)

    # Keep the formerly failing driver/transition cases in normal regression runs.
    add_test(NAME VideoFormatFaults COMMAND DirectShowVideoFormatTests --faults)
    add_test(NAME VideoTransitionFaults COMMAND AudioCallbackTests --transition-faults)
    add_test(NAME VideoLayoutStress COMMAND DirectShowVideoFormatTests --stress)
    add_test(NAME VideoMailboxStress COMMAND LatestVideoSampleTests --stress)
    add_test(NAME VideoTransitionStress COMMAND AudioCallbackTests --transition-stress)
    set_tests_properties(VideoFormatFaults VideoTransitionFaults VideoLayoutStress
        VideoMailboxStress VideoTransitionStress PROPERTIES TIMEOUT 30)
endif()

install(TARGETS LowLatencyCaptureViewer RUNTIME DESTINATION .)
install(FILES LICENSE README.md README.ko.md DESIGN.txt BUILD_INFO.txt DEPENDENCIES.txt
        "실행안내.txt" GITHUB_PREP.md DESTINATION .)
install(FILES third_party/asio/LICENSE.txt DESTINATION . RENAME ASIO-SDK-LICENSE.txt)
install(FILES third_party/asio/HOST-LICENSE.txt DESTINATION . RENAME ASIO-HOST-LICENSE.txt)
install(DIRECTORY docs DESTINATION .)
