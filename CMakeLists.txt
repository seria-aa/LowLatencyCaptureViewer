cmake_minimum_required(VERSION 3.24)
project(LowLatencyCaptureViewer VERSION 2.1.5 LANGUAGES CXX)

include(cmake/MsvcDependencies.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WIN32_EXECUTABLE ON)
add_compile_definitions(LLCV_VSR_FEATURE)
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
    src/capture/HardwareToneMapping.cpp
    src/capture/ElgatoHidToneMapping.cpp
    src/diagnostics/Logger.cpp
    src/diagnostics/AudioErrorHistory.cpp
    src/settings/SettingsStore.cpp
    src/screenshot/ScreenshotPixels.cpp
    src/screenshot/ScreenshotService.cpp
    src/ui/AudioOsdLayout.cpp
    src/ui/AudioOnlyView.cpp
    src/ui/PresentationModeUi.cpp
    src/ui/SettingsView.cpp
    src/ui/SettingsCompactLayout.cpp
    src/ui/SettingsTheme.cpp
    src/ui/SettingsDialogControls.cpp
    src/ui/UiText.cpp
    src/ui/ViewerHelpWindow.cpp
    src/ui/WindowGeometry.cpp
    src/update/UpdateChecker.cpp
    src/video/DirectShowVideoFormat.cpp
    src/video/MjpegDecoder.cpp
    src/video/VideoColor.cpp
    src/video/HdrDisplay.cpp
    src/video/VsrExperiment.cpp
)

target_include_directories(LowLatencyCaptureViewer PRIVATE
    src
    third_party/asio/common
    third_party/asio/host
    third_party/asio/host/pc
)

set_property(SOURCE tests/VsrExperimentTests.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/VsrTimingValidation.inl")

option(LLCV_HDR_FRAME_AUDIT "Build a private F8 single-frame HDR diagnostic (not for release)" OFF)
option(LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC "Private audio-input watchdog/retry prototype (not for release)" OFF)
option(LLCV_RECORDING "Enable SDR video and stereo audio recording" ON)
if(LLCV_RECORDING)
    target_compile_definitions(LowLatencyCaptureViewer PRIVATE LLCV_RECORDING)
    target_sources(LowLatencyCaptureViewer PRIVATE src/recording/Recorder.cpp src/recording/RecordingControls.cpp
        src/recording/RecordingGpuIndicator.cpp
        src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp src/recording/RecordingSettingsView.cpp)
    target_link_libraries(LowLatencyCaptureViewer PRIVATE mfreadwrite)
endif()
option(LLCV_HDR_SCRGB_PROTOTYPE "Private direct P010 shader/scRGB comparison (not for release)" OFF)
option(LLCV_VSR_EXPERIMENT "Private NVIDIA VSR A/B experiment (not for release)" OFF)
if(LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC)
    if(LLCV_HDR_FRAME_AUDIT OR LLCV_HDR_SCRGB_PROTOTYPE OR LLCV_VSR_EXPERIMENT)
        message(FATAL_ERROR "Build audio-input recovery and HDR/VSR diagnostics separately")
    endif()
    target_compile_definitions(LowLatencyCaptureViewer PRIVATE LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC)
    set_target_properties(LowLatencyCaptureViewer PROPERTIES
        OUTPUT_NAME "LowLatencyCaptureViewer_AudioRecovery_Diagnostic")
    # A prior regular EXE may remain in a reused build directory. Packaging must
    # reject the directory as well as the diagnostic executable's version/name.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/llcv-audio-recovery-diagnostic.txt"
        "Private audio-input recovery build; do not package as a regular release.\n")
else()
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/llcv-audio-recovery-diagnostic.txt")
endif()
if(LLCV_RECORDING AND (LLCV_HDR_FRAME_AUDIT OR LLCV_HDR_SCRGB_PROTOTYPE OR LLCV_VSR_EXPERIMENT))
    message(FATAL_ERROR "Disable LLCV_RECORDING when building private HDR/VSR experiments")
endif()
if (LLCV_VSR_EXPERIMENT)
    target_compile_definitions(LowLatencyCaptureViewer PRIVATE LLCV_VSR_EXPERIMENT)
    set_property(SOURCE src/main.cpp APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/video/VsrExperiment.h")
    set_target_properties(LowLatencyCaptureViewer PROPERTIES OUTPUT_NAME "LowLatencyCaptureViewer_VSR_Test")
    if (LLCV_HDR_SCRGB_PROTOTYPE OR LLCV_HDR_FRAME_AUDIT)
        message(FATAL_ERROR "Build VSR and HDR experiments separately for unambiguous measurements")
    endif()
endif()
if (LLCV_HDR_SCRGB_PROTOTYPE)
    set(LLCV_HDR_FRAME_AUDIT ON)
    target_compile_definitions(LowLatencyCaptureViewer PRIVATE LLCV_HDR_SCRGB_PROTOTYPE)
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
    dwmapi
    d3dcompiler
    shcore
    comctl32
    mfplat
    mfuuid
    advapi32
    winhttp
    hid
    setupapi
    windowscodecs
    shell32
)

if (MSVC)
    target_compile_options(LowLatencyCaptureViewer PRIVATE /W4 /EHsc /permissive- /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(RecordingQualityBenchmark tests/RecordingQualityBenchmark.cpp src/recording/Recorder.cpp
        src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp)
    target_include_directories(RecordingQualityBenchmark PRIVATE src)
    target_compile_definitions(RecordingQualityBenchmark PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(RecordingQualityBenchmark PRIVATE ole32 oleaut32 shell32 strmiids mfplat mfreadwrite mfuuid)
    target_compile_options(RecordingQualityBenchmark PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingQualityBenchmark PROPERTIES WIN32_EXECUTABLE FALSE)
    add_executable(RecordingRateControlTests tests/RecordingRateControlTests.cpp src/recording/EncoderSupport.cpp)
    target_include_directories(RecordingRateControlTests PRIVATE src)
    target_compile_definitions(RecordingRateControlTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(RecordingRateControlTests PRIVATE ole32 oleaut32 mfplat mfreadwrite mfuuid strmiids)
    target_compile_options(RecordingRateControlTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingRateControlTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingRateControlTests COMMAND RecordingRateControlTests)
    set_tests_properties(RecordingRateControlTests PROPERTIES TIMEOUT 15)
    add_executable(RecordingEncoderProbeTests tests/RecordingEncoderProbeTests.cpp src/recording/EncoderSupport.cpp)
    target_include_directories(RecordingEncoderProbeTests PRIVATE src)
    target_compile_definitions(RecordingEncoderProbeTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(RecordingEncoderProbeTests PRIVATE ole32 oleaut32 mfplat mfreadwrite mfuuid strmiids)
    target_compile_options(RecordingEncoderProbeTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingEncoderProbeTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingEncoderProbeTests COMMAND RecordingEncoderProbeTests)
    set_tests_properties(RecordingEncoderProbeTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_executable(EncoderProbeProcessTests tests/EncoderProbeProcessTests.cpp)
    target_include_directories(EncoderProbeProcessTests PRIVATE src)
    target_compile_definitions(EncoderProbeProcessTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(EncoderProbeProcessTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(EncoderProbeProcessTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME EncoderProbeProcessTests COMMAND EncoderProbeProcessTests)
    set_tests_properties(EncoderProbeProcessTests PROPERTIES TIMEOUT 15)
    add_executable(RecordingTests tests/RecordingTests.cpp src/recording/Recorder.cpp
        src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp)
    target_include_directories(RecordingTests PRIVATE src)
    target_compile_definitions(RecordingTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(RecordingTests PRIVATE ole32 oleaut32 shell32 strmiids mfplat mfreadwrite mfuuid)
    set_target_properties(RecordingTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if(MSVC)
        target_compile_options(RecordingTests PRIVATE /W4 /EHsc /utf-8)
    endif()
    add_test(NAME RecordingTests COMMAND RecordingTests)
    if(LLCV_RECORDING_FFMPEG_ROOT)
        foreach(case mjpeg mjpeg-pause mjpeg-limited mjpeg-scale mjpeg-arrival mjpeg-invalid
                     mjpeg-color mjpeg-color-scale raw-color raw-color-scale
                     raw-yuy2-color raw-yuy2-color-scale)
            add_test(NAME Recording_${case} COMMAND RecordingTests --${case})
            set_tests_properties(Recording_${case} PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
        endforeach()
        add_test(NAME Recording_mjpeg-split COMMAND RecordingTests --mjpeg-split)
        set_tests_properties(Recording_mjpeg-split PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
        add_test(NAME Recording_mjpeg-hevc COMMAND RecordingTests --mjpeg-hevc)
        set_tests_properties(Recording_mjpeg-hevc PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
        add_test(NAME Recording_mjpeg-hd COMMAND RecordingTests --mjpeg-hd)
        set_tests_properties(Recording_mjpeg-hd PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME RecordingGainTests COMMAND RecordingTests --gain)
    add_test(NAME RecordingMuteTests COMMAND RecordingTests --mute)
    set_tests_properties(RecordingGainTests RecordingMuteTests
        PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME RecordingPauseTests COMMAND RecordingTests --pause)
    add_test(NAME RecordingPauseUnalignedTests COMMAND RecordingTests --pause-unaligned)
    set_tests_properties(RecordingPauseUnalignedTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME RecordingLargeAudioTests COMMAND RecordingTests --large-audio)
    add_test(NAME RecordingLargeAudioPauseTests COMMAND RecordingTests --large-audio-pause)
    add_test(NAME RecordingAudioPacketBoundsTests COMMAND RecordingTests --audio-bounds)
    set_tests_properties(RecordingLargeAudioTests RecordingLargeAudioPauseTests
        RecordingAudioPacketBoundsTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    if(LLCV_RECORDING_FFMPEG_ROOT)
        add_test(NAME RecordingGainSplitTests COMMAND RecordingTests --gain-split)
        set_tests_properties(RecordingGainSplitTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
        add_test(NAME RecordingSplitPauseTests COMMAND RecordingTests --split-pause)
        set_tests_properties(RecordingSplitPauseTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    endif()
    add_test(NAME RecordingScaleTests COMMAND RecordingTests --scale)
    set_tests_properties(RecordingPauseTests RecordingScaleTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME RecordingArrivalClockTests COMMAND RecordingTests --arrival)
    add_test(NAME RecordingRateLimitTests COMMAND RecordingTests --limited)
    add_test(NAME RecordingQualityTests COMMAND RecordingTests --quality)
    add_test(NAME RecordingQualityYuy2Tests COMMAND RecordingTests --quality-yuy2)
    set_tests_properties(RecordingQualityTests RecordingQualityYuy2Tests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME RecordingPaddedInputTests COMMAND RecordingTests --padded)
    add_test(NAME RecordingYuy2InputTests COMMAND RecordingTests --yuy2)
    add_test(NAME RecordingLifecycleTests COMMAND RecordingTests --lifecycle)
    set_tests_properties(RecordingLifecycleTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_executable(RecordingAdmissionTests tests/RecordingAdmissionTests.cpp)
    target_include_directories(RecordingAdmissionTests PRIVATE src)
    target_compile_definitions(RecordingAdmissionTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingAdmissionTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingAdmissionTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingAdmissionTests COMMAND RecordingAdmissionTests)
    set_tests_properties(RecordingAdmissionTests PROPERTIES TIMEOUT 15)
    add_executable(RecordingStartupOptionsTests tests/RecordingStartupOptionsTests.cpp
        src/recording/Recorder.cpp src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp)
    target_include_directories(RecordingStartupOptionsTests PRIVATE src)
    target_compile_definitions(RecordingStartupOptionsTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(RecordingStartupOptionsTests PRIVATE ole32 oleaut32 shell32 strmiids mfplat mfreadwrite mfuuid)
    target_compile_options(RecordingStartupOptionsTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingStartupOptionsTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingStartupOptionsTests COMMAND RecordingStartupOptionsTests)
    set_tests_properties(RecordingStartupOptionsTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    set_tests_properties(RecordingPaddedInputTests RecordingYuy2InputTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_executable(RecordingBufferTests tests/RecordingBufferTests.cpp)
    target_include_directories(RecordingBufferTests PRIVATE src)
    target_compile_definitions(RecordingBufferTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingBufferTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingBufferTests PRIVATE ole32 mfplat mfuuid)
    set_target_properties(RecordingBufferTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingBufferTests COMMAND RecordingBufferTests)
    set_tests_properties(RecordingBufferTests PROPERTIES TIMEOUT 20)
    set_tests_properties(RecordingRateLimitTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_executable(RecordingSettingsTests tests/RecordingSettingsTests.cpp
        src/recording/RecordingSettingsView.cpp src/recording/RecordingOptions.cpp
        src/ui/SettingsView.cpp src/ui/PresentationModeUi.cpp)
    target_include_directories(RecordingSettingsTests PRIVATE src)
    target_compile_definitions(RecordingSettingsTests PRIVATE LLCV_RECORDING UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(RecordingSettingsTests PRIVATE comctl32 ole32 shell32)
    set_target_properties(RecordingSettingsTests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_compile_options(RecordingSettingsTests PRIVATE /W4 /EHsc /utf-8)
    add_test(NAME RecordingSettingsTests COMMAND RecordingSettingsTests)
    set_tests_properties(RecordingSettingsTests PROPERTIES TIMEOUT 45)
    add_executable(RecordingShortcutTests tests/RecordingShortcutTests.cpp
        src/settings/SettingsStore.cpp src/ui/SettingsView.cpp src/ui/PresentationModeUi.cpp
        src/ui/SettingsDialogControls.cpp src/ui/UiText.cpp
        src/recording/RecordingSettingsView.cpp src/recording/RecordingOptions.cpp)
    target_include_directories(RecordingShortcutTests PRIVATE src)
    target_compile_definitions(RecordingShortcutTests PRIVATE LLCV_RECORDING UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(RecordingShortcutTests PRIVATE comctl32 ole32 shell32)
    target_compile_options(RecordingShortcutTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingShortcutTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingShortcutTests COMMAND RecordingShortcutTests)
    set_tests_properties(RecordingShortcutTests PROPERTIES TIMEOUT 60)
    add_executable(RecordingSafetyPolicyTests tests/RecordingSafetyPolicyTests.cpp)
    target_include_directories(RecordingSafetyPolicyTests PRIVATE src)
    target_compile_definitions(RecordingSafetyPolicyTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingSafetyPolicyTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingSafetyPolicyTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingSafetyPolicyTests COMMAND RecordingSafetyPolicyTests)
    set_tests_properties(RecordingSafetyPolicyTests PROPERTIES TIMEOUT 15)
    foreach(recording_test RecordingStateTests RecordingStatusTests RecordingControlsTests RecordingContainerTests)
        add_executable(${recording_test} tests/${recording_test}.cpp)
        target_include_directories(${recording_test} PRIVATE src)
        target_compile_definitions(${recording_test} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_compile_options(${recording_test} PRIVATE /W4 /EHsc /utf-8)
        set_target_properties(${recording_test} PROPERTIES WIN32_EXECUTABLE FALSE)
        add_test(NAME ${recording_test} COMMAND ${recording_test})
        set_tests_properties(${recording_test} PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    endforeach()
    target_sources(RecordingStateTests PRIVATE src/recording/Recorder.cpp
        src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp)
    target_link_libraries(RecordingStateTests PRIVATE ole32 oleaut32 shell32 strmiids mfplat mfreadwrite mfuuid)
    # The controls fixture includes the production view with a snapshot-only recorder stub.
    target_link_libraries(RecordingControlsTests PRIVATE user32 gdi32 shell32 ole32 comctl32 dwmapi)
    add_executable(RecordingGpuIndicatorTests tests/RecordingGpuIndicatorTests.cpp
        src/recording/RecordingGpuIndicator.cpp)
    target_include_directories(RecordingGpuIndicatorTests PRIVATE src)
    target_compile_definitions(RecordingGpuIndicatorTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingGpuIndicatorTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingGpuIndicatorTests PRIVATE user32 ole32 d3d11 d2d1 dwrite)
    set_target_properties(RecordingGpuIndicatorTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingGpuIndicatorTests COMMAND RecordingGpuIndicatorTests)
    add_test(NAME RecordingGpuIndicatorGpuTests COMMAND RecordingGpuIndicatorTests --gpu)
    set_tests_properties(RecordingGpuIndicatorTests RecordingGpuIndicatorGpuTests
        PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    add_executable(RecordingIndicatorPreview WIN32 tests/RecordingControlsTests.cpp)
    target_include_directories(RecordingIndicatorPreview PRIVATE src)
    target_compile_definitions(RecordingIndicatorPreview PRIVATE LLCV_RECORDING_INDICATOR_PREVIEW UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingIndicatorPreview PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingIndicatorPreview PRIVATE user32 gdi32 shell32 ole32 comctl32)
    target_sources(RecordingContainerTests PRIVATE src/recording/EncoderSupport.cpp)
    target_link_libraries(RecordingContainerTests PRIVATE ole32 oleaut32 mfplat mfreadwrite mfuuid strmiids)
    add_executable(RecordingOptionsTests tests/RecordingOptionsTests.cpp src/recording/RecordingOptions.cpp)
    target_include_directories(RecordingOptionsTests PRIVATE src)
    target_compile_definitions(RecordingOptionsTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingOptionsTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingOptionsTests PRIVATE ole32 shell32)
    set_target_properties(RecordingOptionsTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingOptionsTests COMMAND RecordingOptionsTests)
    add_executable(RecordingProcessingTests tests/RecordingProcessingTests.cpp src/recording/RecordingOptions.cpp)
    target_include_directories(RecordingProcessingTests PRIVATE src)
    target_compile_definitions(RecordingProcessingTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingProcessingTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingProcessingTests PRIVATE mfplat mfuuid ole32 shell32)
    set_target_properties(RecordingProcessingTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingProcessingTests COMMAND RecordingProcessingTests)
    set_tests_properties(RecordingProcessingTests PROPERTIES TIMEOUT 15)
    set_tests_properties(RecordingOptionsTests PROPERTIES TIMEOUT 15)
    set_tests_properties(RecordingTests RecordingArrivalClockTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
    add_executable(PlaybackPowerRequestTests tests/PlaybackPowerRequestTests.cpp)
    target_include_directories(PlaybackPowerRequestTests PRIVATE src)
    target_compile_definitions(PlaybackPowerRequestTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    set_target_properties(PlaybackPowerRequestTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if(MSVC)
        target_compile_options(PlaybackPowerRequestTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME PlaybackPowerRequestTests COMMAND PlaybackPowerRequestTests)
    add_test(NAME PlaybackPowerNativeTests COMMAND PlaybackPowerRequestTests --native)
    set_tests_properties(PlaybackPowerRequestTests PlaybackPowerNativeTests PROPERTIES TIMEOUT 15)
    set_tests_properties(PlaybackPowerNativeTests PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(PlaybackPowerEventsTests tests/PlaybackPowerEventsTests.cpp)
    set_target_properties(PlaybackPowerEventsTests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_include_directories(PlaybackPowerEventsTests PRIVATE src)
    target_compile_definitions(PlaybackPowerEventsTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(PlaybackPowerEventsTests PRIVATE strmiids ole32 oleaut32)
    if(MSVC)
        target_compile_options(PlaybackPowerEventsTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME PlaybackPowerEventsTests COMMAND PlaybackPowerEventsTests)
    set_tests_properties(PlaybackPowerEventsTests PROPERTIES TIMEOUT 15)
    add_executable(SuspendResumeNotificationTests tests/SuspendResumeNotificationTests.cpp)
    set_target_properties(SuspendResumeNotificationTests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_include_directories(SuspendResumeNotificationTests PRIVATE src)
    target_compile_definitions(SuspendResumeNotificationTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(SuspendResumeNotificationTests PRIVATE user32)
    if(MSVC)
        target_compile_options(SuspendResumeNotificationTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME SuspendResumeNotificationTests COMMAND SuspendResumeNotificationTests)
    add_test(NAME SuspendResumeNotificationNativeTests COMMAND SuspendResumeNotificationTests --native)
    set_tests_properties(SuspendResumeNotificationTests SuspendResumeNotificationNativeTests
        PROPERTIES TIMEOUT 15)
    set_tests_properties(SuspendResumeNotificationNativeTests PROPERTIES SKIP_RETURN_CODE 77)
endif()
if (BUILD_TESTING)
    add_executable(ViewerHelpTests tests/ViewerHelpTests.cpp src/ui/ViewerHelpWindow.cpp)
    set_property(SOURCE tests/ViewerHelpTests.cpp APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ViewerHelpWindow.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ViewerShortcuts.h")
    target_include_directories(ViewerHelpTests PRIVATE src)
    target_compile_definitions(ViewerHelpTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_link_libraries(ViewerHelpTests PRIVATE user32 gdi32 dwmapi gdiplus imm32)
    set_target_properties(ViewerHelpTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(ViewerHelpTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME ViewerHelpTests COMMAND ViewerHelpTests)
    add_executable(RecordingHelpTests tests/ViewerHelpTests.cpp src/ui/ViewerHelpWindow.cpp)
    target_include_directories(RecordingHelpTests PRIVATE src)
    target_compile_definitions(RecordingHelpTests PRIVATE LLCV_RECORDING UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_link_libraries(RecordingHelpTests PRIVATE user32 gdi32 dwmapi gdiplus imm32)
    target_compile_options(RecordingHelpTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingHelpTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingHelpTests COMMAND RecordingHelpTests)
    set_tests_properties(RecordingHelpTests PROPERTIES TIMEOUT 120)
    add_test(NAME ViewerHelpImeTests COMMAND ViewerHelpTests --with-ime)
    # Each stress test opens/closes 320 native help windows. Preserve the full
    # coverage while allowing headroom alongside CPU-heavy parallel replays.
    set_tests_properties(ViewerHelpTests ViewerHelpImeTests PROPERTIES TIMEOUT 120)
    add_executable(ScreenshotTests tests/ScreenshotTests.cpp tests/HdrScreenshotReference.cpp
        src/screenshot/ScreenshotPixels.cpp src/screenshot/ScreenshotService.cpp)
    target_include_directories(ScreenshotTests PRIVATE src)
    target_compile_definitions(ScreenshotTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ScreenshotTests PRIVATE ole32 oleaut32 windowscodecs shell32 user32 gdi32 uuid)
    set_target_properties(ScreenshotTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(ScreenshotTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME ScreenshotTests COMMAND ScreenshotTests)
    set_tests_properties(ScreenshotTests PROPERTIES TIMEOUT 60)
    add_executable(MjpegDecoderTests tests/MjpegDecoderTests.cpp src/video/VideoColor.cpp)
    target_include_directories(MjpegDecoderTests PRIVATE src)
    target_compile_definitions(MjpegDecoderTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(MjpegDecoderTests PRIVATE mfplat mfuuid ole32 strmiids windowscodecs)
    set_target_properties(MjpegDecoderTests PROPERTIES WIN32_EXECUTABLE FALSE)
    set_property(SOURCE tests/MjpegDecoderTests.cpp APPEND PROPERTY
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/video/MjpegDecoder.cpp")
    if (MSVC)
        target_compile_options(MjpegDecoderTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME MjpegDecoderTests COMMAND MjpegDecoderTests)
    add_test(NAME MjpegNativeDecoderTests COMMAND MjpegDecoderTests --native)
    set_tests_properties(MjpegNativeDecoderTests PROPERTIES TIMEOUT 30)
    add_executable(WasapiInitializationTests tests/WasapiInitializationTests.cpp
        src/audio/AudioDeviceCapabilities.cpp)
    target_include_directories(WasapiInitializationTests PRIVATE src)
    target_compile_definitions(WasapiInitializationTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(WasapiInitializationTests PRIVATE ole32 oleaut32 uuid avrt)
    set_target_properties(WasapiInitializationTests PROPERTIES WIN32_EXECUTABLE FALSE)
    set_property(SOURCE tests/WasapiInitializationTests.cpp APPEND PROPERTY
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/WasapiOutput.cpp")
    set_property(SOURCE tests/AudioCallbackTests.cpp APPEND PROPERTY
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/AsioOutput.cpp")
    if (MSVC)
        target_compile_options(WasapiInitializationTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME WasapiInitializationTests COMMAND WasapiInitializationTests)
    add_executable(WasapiRenderTests tests/WasapiRenderTests.cpp
        src/audio/AudioDeviceCapabilities.cpp)
    target_include_directories(WasapiRenderTests PRIVATE src)
    target_compile_definitions(WasapiRenderTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(WasapiRenderTests PRIVATE ole32 oleaut32 uuid avrt)
    set_target_properties(WasapiRenderTests PROPERTIES WIN32_EXECUTABLE FALSE)
    set_property(SOURCE tests/WasapiRenderTests.cpp APPEND PROPERTY
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/WasapiOutput.cpp")
    if (MSVC)
        target_compile_options(WasapiRenderTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME WasapiRenderTests COMMAND WasapiRenderTests)
    set_tests_properties(WasapiRenderTests MjpegDecoderTests PROPERTIES TIMEOUT 30)
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
    # Fake COM/HID only: never open capture devices in regression tests.
    foreach(tonemap_test HardwareToneMappingTests VendorUsbToneMappingTests)
        add_executable(${tonemap_test} tests/${tonemap_test}.cpp
            src/capture/HardwareToneMapping.cpp src/capture/ElgatoHidToneMapping.cpp)
        target_include_directories(${tonemap_test} PRIVATE src)
        target_compile_definitions(${tonemap_test} PRIVATE
            UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00)
        target_link_libraries(${tonemap_test} PRIVATE strmiids uuid hid setupapi)
        set_target_properties(${tonemap_test} PROPERTIES WIN32_EXECUTABLE FALSE)
        if (MSVC)
            target_compile_options(${tonemap_test} PRIVATE /W4 /EHsc /permissive- /utf-8)
        endif()
        add_test(NAME ${tonemap_test} COMMAND ${tonemap_test})
    endforeach()
    add_executable(HdrPolicyTests tests/HdrPolicyTests.cpp)
    target_include_directories(HdrPolicyTests PRIVATE src)
    target_compile_definitions(HdrPolicyTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(HdrPolicyTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME HdrPolicyTests COMMAND HdrPolicyTests)
    add_executable(OutputTransitionStateTests tests/OutputTransitionStateTests.cpp)
    target_include_directories(OutputTransitionStateTests PRIVATE src)
    set_target_properties(OutputTransitionStateTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME OutputTransitionStateTests COMMAND OutputTransitionStateTests)
    foreach(policy_test ExclusivePresentationStateTests VideoClearPolicyTests VideoPlacementTests)
        add_executable(${policy_test} tests/${policy_test}.cpp)
        target_include_directories(${policy_test} PRIVATE src)
        target_compile_definitions(${policy_test} PRIVATE
            UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        set_target_properties(${policy_test} PROPERTIES WIN32_EXECUTABLE FALSE)
        if(MSVC)
            target_compile_options(${policy_test} PRIVATE /W4 /EHsc /permissive- /utf-8)
        endif()
        add_test(NAME ${policy_test} COMMAND ${policy_test})
        set_tests_properties(${policy_test} PROPERTIES TIMEOUT 15)
    endforeach()

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
    add_executable(SettingsModeCacheTests tests/SettingsModeCacheTests.cpp)
    target_include_directories(SettingsModeCacheTests PRIVATE src)
    set_target_properties(SettingsModeCacheTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME SettingsModeCacheTests COMMAND SettingsModeCacheTests)
    set_tests_properties(SettingsModeCacheTests PROPERTIES TIMEOUT 15)
    # Native control creation is substantially slower than pure geometry tests,
    # especially when other replay tests run concurrently on CI.
    set_tests_properties(SettingsViewTests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    add_executable(SettingsThemeTests tests/SettingsThemeTests.cpp
        src/ui/SettingsTheme.cpp src/ui/SettingsView.cpp src/ui/PresentationModeUi.cpp
        src/ui/SettingsDialogControls.cpp src/ui/UiText.cpp)
    target_include_directories(SettingsThemeTests PRIVATE src)
    target_compile_definitions(SettingsThemeTests PRIVATE UNICODE _UNICODE NOMINMAX
        WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_link_libraries(SettingsThemeTests PRIVATE comctl32 dwmapi)
    set_target_properties(SettingsThemeTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(SettingsThemeTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME SettingsThemeTests COMMAND SettingsThemeTests)
    # Hidden native fixture clicks can take 2 s each on the test host.
    # Keep all 32 lifecycle checks; allow the
    # measured ~70 s run instead of mistaking that finite work for a deadlock.
    set_tests_properties(SettingsThemeTests PROPERTIES TIMEOUT 120)
    # Local visual/keyboard QA with synthetic devices; never installed and
    # never reads or saves a user's settings or opens capture/audio hardware.
    get_target_property(settings_preview_sources SettingsThemeTests SOURCES)
    add_executable(SettingsPreview WIN32 ${settings_preview_sources})
    target_include_directories(SettingsPreview PRIVATE src)
    target_compile_definitions(SettingsPreview PRIVATE LLCV_SETTINGS_PREVIEW
        $<TARGET_PROPERTY:SettingsThemeTests,COMPILE_DEFINITIONS>)
    target_link_libraries(SettingsPreview PRIVATE comctl32 dwmapi)
    if(LLCV_RECORDING)
        target_compile_definitions(SettingsPreview PRIVATE LLCV_RECORDING)
        target_sources(SettingsPreview PRIVATE src/recording/RecordingSettingsView.cpp src/recording/RecordingOptions.cpp)
        target_link_libraries(SettingsPreview PRIVATE ole32 shell32)
        # Exercise the actual six-page / ten-shortcut theme as well as the
        # playback-only layout. Preview-only coverage cannot catch regressions.
        add_executable(RecordingThemeTests ${settings_preview_sources}
            src/recording/RecordingSettingsView.cpp src/recording/RecordingOptions.cpp)
        target_include_directories(RecordingThemeTests PRIVATE src)
        target_compile_definitions(RecordingThemeTests PRIVATE LLCV_RECORDING
            $<TARGET_PROPERTY:SettingsThemeTests,COMPILE_DEFINITIONS>)
        target_compile_options(RecordingThemeTests PRIVATE /W4 /EHsc /permissive- /utf-8)
        target_link_libraries(RecordingThemeTests PRIVATE comctl32 dwmapi ole32 shell32)
        set_target_properties(RecordingThemeTests PROPERTIES WIN32_EXECUTABLE FALSE)
        add_test(NAME RecordingThemeTests COMMAND RecordingThemeTests)
        set_tests_properties(RecordingThemeTests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
    if (MSVC)
        target_compile_options(SettingsPreview PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_executable(LatestVideoSampleTests tests/LatestVideoSampleTests.cpp
        src/capture/LatestVideoSample.cpp
        src/video/DirectShowVideoFormat.cpp
        src/capture/DirectShowGraphResources.cpp)
    target_include_directories(LatestVideoSampleTests PRIVATE src)
    target_compile_definitions(LatestVideoSampleTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(LatestVideoSampleTests PRIVATE ole32 strmiids uuid mfuuid user32)
    set_target_properties(LatestVideoSampleTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if (MSVC)
        target_compile_options(LatestVideoSampleTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME LatestVideoSampleTests COMMAND LatestVideoSampleTests)
    set_tests_properties(LatestVideoSampleTests PROPERTIES TIMEOUT 15)

    add_executable(CaptureFrameRateMonitorTests tests/CaptureFrameRateMonitorTests.cpp)
    target_include_directories(CaptureFrameRateMonitorTests PRIVATE src)
    set_target_properties(CaptureFrameRateMonitorTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if(MSVC)
        target_compile_options(CaptureFrameRateMonitorTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME CaptureFrameRateMonitorTests COMMAND CaptureFrameRateMonitorTests)
    set_tests_properties(CaptureFrameRateMonitorTests PROPERTIES TIMEOUT 15)
    # Exercise application callbacks and the ASIO notification protocol without
    # enumerating/opening hardware. This target is never installed.
    # These test translation units include the implementation directly. Keep
    # that dependency explicit even when a localized MSVC include scan misses it.
    set_property(SOURCE tests/AudioCallbackTests.cpp tests/HdrPipelineTests.cpp
        tests/MjpegRecordingPlaybackTests.cpp
        tests/MonitorMoveTests.cpp tests/SharedReplayTests.cpp
        APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ViewerHelpWindow.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ViewerShortcuts.h")
    get_target_property(audio_callback_sources LowLatencyCaptureViewer SOURCES)
    list(FILTER audio_callback_sources EXCLUDE REGEX "src/(main.cpp|app.rc|audio/AsioOutput.cpp)$")
    if (WIN32)
        add_executable(VsrExperimentTests tests/VsrExperimentTests.cpp ${audio_callback_sources})
        target_compile_definitions(VsrExperimentTests PRIVATE LLCV_SETTINGS_MODE_TEST)
        set_property(SOURCE tests/VsrExperimentTests.cpp APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/video/VsrExperiment.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/SettingsControllerRegressionTests.inl"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/CaptureFrameRateOsdTests.inl"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ViewerLifecycleTests.inl"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExclusiveViewerLifecycleTests.inl"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExclusiveRendererRegression.inl"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/VideoClearRegression.inl")
        target_include_directories(VsrExperimentTests PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
        target_compile_definitions(VsrExperimentTests PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>)
        target_link_libraries(VsrExperimentTests PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,LINK_LIBRARIES>)
        set_target_properties(VsrExperimentTests PROPERTIES WIN32_EXECUTABLE FALSE)
        if (MSVC)
            target_compile_options(VsrExperimentTests PRIVATE /W4 /EHsc /permissive- /utf-8)
        endif()
        add_test(NAME VsrExperimentTests COMMAND VsrExperimentTests)
        set_tests_properties(VsrExperimentTests PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS GPU)
        add_test(NAME VideoClearRegression COMMAND VsrExperimentTests --video-clear-regression)
        set_tests_properties(VideoClearRegression PROPERTIES
            TIMEOUT 90 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS GPU)
        add_test(NAME SettingsControllerTests COMMAND VsrExperimentTests --settings-controller)
        add_test(NAME CaptureFrameRateOsdTests COMMAND VsrExperimentTests --capture-fps-osd)
        set_tests_properties(CaptureFrameRateOsdTests PROPERTIES TIMEOUT 30)
        add_test(NAME ViewerLifecycleTests COMMAND VsrExperimentTests --viewer-lifecycle --smoke-test)
        set_tests_properties(ViewerLifecycleTests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        add_test(NAME ExclusiveViewerLifecycleTests COMMAND VsrExperimentTests --exclusive-viewer-lifecycle)
        set_tests_properties(ExclusiveViewerLifecycleTests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
        # Hidden-window interaction and worker handover timings are distorted
        # by concurrent UI/GPU replay suites. Keep the deadline, but isolate it.
        set_tests_properties(SettingsControllerTests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endif()
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
    add_test(NAME AudioInputRecoveryTests COMMAND AudioCallbackTests --audio-input-recovery)
    set_tests_properties(AudioInputRecoveryTests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

    if(LLCV_RECORDING)
        # Real viewer/recording close protocol with hidden windows and synthetic
        # media. Modal answers and render acknowledgements are injected; no GPU.
        add_executable(RecordingSettingsCancelTests tests/RecordingSettingsCancelTests.cpp
            ${audio_callback_sources})
        set_property(SOURCE tests/RecordingSettingsCancelTests.cpp APPEND PROPERTY
            OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
        target_include_directories(RecordingSettingsCancelTests PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
        target_compile_definitions(RecordingSettingsCancelTests PRIVATE LLCV_SETTINGS_MODE_TEST
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>)
        target_link_libraries(RecordingSettingsCancelTests PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,LINK_LIBRARIES>)
        set_target_properties(RecordingSettingsCancelTests PROPERTIES WIN32_EXECUTABLE FALSE)
        if(MSVC)
            target_compile_options(RecordingSettingsCancelTests PRIVATE /W4 /EHsc /permissive- /utf-8)
        endif()
        add_test(NAME RecordingSettingsCancelTests COMMAND RecordingSettingsCancelTests)
        set_tests_properties(RecordingSettingsCancelTests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        add_executable(MjpegRecordingPlaybackTests tests/MjpegRecordingPlaybackTests.cpp
            ${audio_callback_sources})
        target_include_directories(MjpegRecordingPlaybackTests PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
        target_compile_definitions(MjpegRecordingPlaybackTests PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>)
        target_link_libraries(MjpegRecordingPlaybackTests PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,LINK_LIBRARIES>)
        set_target_properties(MjpegRecordingPlaybackTests PROPERTIES WIN32_EXECUTABLE FALSE)
        if(MSVC)
            target_compile_options(MjpegRecordingPlaybackTests PRIVATE /W4 /EHsc /permissive- /utf-8)
        endif()
        add_test(NAME MjpegRecordingPlaybackTests COMMAND MjpegRecordingPlaybackTests)
        set_tests_properties(MjpegRecordingPlaybackTests PROPERTIES
            TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77 LABELS GPU)
        add_test(NAME PlaybackMuteRecordingTests COMMAND AudioCallbackTests --mute-recording)
        set_tests_properties(PlaybackMuteRecordingTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
        set_property(SOURCE tests/AudioCallbackTests.cpp APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/RecordingAudioDeviceSwitchTests.inl")
        add_test(NAME RecordingAudioDeviceSwitchTests COMMAND AudioCallbackTests --recording-audio-device-switch)
        add_test(NAME RecordingAudioInputBoundaryTests COMMAND AudioCallbackTests --recording-audio-input-boundaries)
        set_tests_properties(RecordingAudioDeviceSwitchTests RecordingAudioInputBoundaryTests
            PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()

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
    add_test(NAME HdrVsrPipelineTests COMMAND HdrPipelineTests --vsr-hdr)
    set_tests_properties(HdrVsrPipelineTests PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 LABELS GPU)
    add_test(NAME SdrPipelineTests COMMAND HdrPipelineTests --sdr-only)
    add_test(NAME OverlayUiTests COMMAND HdrPipelineTests --osd-ui)
    set_tests_properties(OverlayUiTests PROPERTIES TIMEOUT 60 LABELS GPU)
    set_tests_properties(SdrPipelineTests PROPERTIES TIMEOUT 90 LABELS GPU)
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
        src/ui/AudioOnlyView.cpp
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

    add_executable(AudioOnlyViewTests
        tests/AudioOnlyViewTests.cpp
        src/ui/AudioOnlyView.cpp)
    target_include_directories(AudioOnlyViewTests PRIVATE src)
    set_target_properties(AudioOnlyViewTests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_compile_definitions(AudioOnlyViewTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(AudioOnlyViewTests PRIVATE gdiplus gdi32 user32)
    if (MSVC)
        target_compile_options(AudioOnlyViewTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME AudioOnlyViewTests COMMAND AudioOnlyViewTests)

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
    add_test(NAME SettingsStoreSaveFailureTests COMMAND SettingsStoreTests --save-failure-probe)
    add_test(NAME SettingsStoreEmptyCacheTests COMMAND SettingsStoreTests --empty-cache-probe)
    set_tests_properties(SettingsStoreSaveFailureTests SettingsStoreEmptyCacheTests
        PROPERTIES WILL_FAIL TRUE TIMEOUT 15)

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

    add_executable(WindowCornersTests tests/WindowCornersTests.cpp)
    target_include_directories(WindowCornersTests PRIVATE src)
    target_compile_definitions(WindowCornersTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(WindowCornersTests PRIVATE dwmapi user32 gdi32)
    set_target_properties(WindowCornersTests PROPERTIES WIN32_EXECUTABLE FALSE)
    if(MSVC)
        target_compile_options(WindowCornersTests PRIVATE /W4 /EHsc /permissive- /utf-8)
    endif()
    add_test(NAME WindowCornersTests COMMAND WindowCornersTests)

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

add_library(LLCV_RecordingStorage STATIC EXCLUDE_FROM_ALL src/recording/RecordingStorage.cpp)
target_include_directories(LLCV_RecordingStorage PUBLIC src)
target_compile_definitions(LLCV_RecordingStorage PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(LLCV_RecordingStorage PRIVATE /W4 /EHsc /permissive- /utf-8)
target_link_libraries(LLCV_RecordingStorage PUBLIC ole32 mfplat mfuuid)
include(cmake/RecordingFFmpeg.cmake)
if(BUILD_TESTING)
    add_executable(RecordingStorageTests tests/RecordingStorageTests.cpp)
    target_link_libraries(RecordingStorageTests PRIVATE LLCV_RecordingStorage)
    target_compile_definitions(RecordingStorageTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingStorageTests PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(RecordingStorageTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingStorageTests COMMAND RecordingStorageTests)
    set_tests_properties(RecordingStorageTests PROPERTIES TIMEOUT 30)

    add_executable(RecordingStorageSpaceTests tests/RecordingStorageTests.cpp src/recording/RecordingStorage.cpp)
    target_include_directories(RecordingStorageSpaceTests PRIVATE src)
    target_compile_definitions(RecordingStorageSpaceTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN LLCV_RECORDING_STORAGE_SPACE_TEST)
    target_compile_options(RecordingStorageSpaceTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingStorageSpaceTests PRIVATE ole32 mfplat mfuuid)
    set_target_properties(RecordingStorageSpaceTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingStorageSpaceTests COMMAND RecordingStorageSpaceTests)
    set_tests_properties(RecordingStorageSpaceTests PROPERTIES TIMEOUT 30)

    add_executable(RecordingLowSpaceTests tests/RecordingLowSpaceTests.cpp src/recording/Recorder.cpp
        src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp src/recording/RecordingStorage.cpp)
    target_include_directories(RecordingLowSpaceTests PRIVATE src)
    target_compile_definitions(RecordingLowSpaceTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN LLCV_RECORDING_STORAGE_SPACE_TEST)
    target_compile_options(RecordingLowSpaceTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingLowSpaceTests PRIVATE ole32 oleaut32 shell32 strmiids mfplat mfreadwrite mfuuid)
    set_target_properties(RecordingLowSpaceTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingLowSpaceTests COMMAND RecordingLowSpaceTests)
    set_tests_properties(RecordingLowSpaceTests PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

    add_executable(RecordingQueueTests tests/RecordingQueueTests.cpp src/recording/Recorder.cpp
        src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp)
    target_include_directories(RecordingQueueTests PRIVATE src)
    target_compile_definitions(RecordingQueueTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingQueueTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingQueueTests PRIVATE ole32 oleaut32 shell32 strmiids mfplat mfreadwrite mfuuid)
    set_target_properties(RecordingQueueTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingQueueTests COMMAND RecordingQueueTests)
    set_tests_properties(RecordingQueueTests PROPERTIES TIMEOUT 30)

    add_executable(RecordingSpaceDelayTests tests/RecordingTests.cpp src/recording/Recorder.cpp
        src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp src/recording/RecordingStorage.cpp)
    target_include_directories(RecordingSpaceDelayTests PRIVATE src)
    target_compile_definitions(RecordingSpaceDelayTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN LLCV_RECORDING_SPACE_DELAY_TEST)
    target_compile_options(RecordingSpaceDelayTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingSpaceDelayTests PRIVATE ole32 oleaut32 shell32 strmiids mfplat mfreadwrite mfuuid)
    set_target_properties(RecordingSpaceDelayTests PROPERTIES WIN32_EXECUTABLE FALSE)
    foreach(case arrival mjpeg-arrival pause-unaligned split-pause hevc-arrival)
        if((NOT case STREQUAL "split-pause" AND NOT case STREQUAL "hevc-arrival" AND
            NOT case STREQUAL "mjpeg-arrival") OR LLCV_RECORDING_FFMPEG_ROOT)
            add_test(NAME RecordingSpaceDelay_${case} COMMAND RecordingSpaceDelayTests --${case})
            set_tests_properties(RecordingSpaceDelay_${case} PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
            if(case STREQUAL "hevc-arrival")
                set_tests_properties(RecordingSpaceDelay_${case} PROPERTIES SKIP_RETURN_CODE 77)
            endif()
        endif()
    endforeach()

    add_executable(RecordingStorageDelayTests tests/RecordingTests.cpp src/recording/Recorder.cpp
        src/recording/RecordingOptions.cpp src/recording/EncoderSupport.cpp src/recording/RecordingStorage.cpp)
    target_include_directories(RecordingStorageDelayTests PRIVATE src)
    target_compile_definitions(RecordingStorageDelayTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN LLCV_RECORDING_STORAGE_DELAY_TEST)
    target_compile_options(RecordingStorageDelayTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingStorageDelayTests PRIVATE ole32 oleaut32 shell32 strmiids mfplat mfreadwrite mfuuid)
    set_target_properties(RecordingStorageDelayTests PROPERTIES WIN32_EXECUTABLE FALSE)
    foreach(case arrival pause-unaligned split-pause)
        if(NOT case STREQUAL "split-pause" OR LLCV_RECORDING_FFMPEG_ROOT)
            add_test(NAME RecordingStorageDelay_${case} COMMAND RecordingStorageDelayTests --${case})
            set_tests_properties(RecordingStorageDelay_${case} PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
        endif()
    endforeach()
    if(LLCV_RECORDING_FFMPEG_ROOT)
        foreach(case hevc hevc-arrival)
            add_test(NAME RecordingStorageDelay_${case} COMMAND RecordingStorageDelayTests --${case})
            set_tests_properties(RecordingStorageDelay_${case} PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
        endforeach()
    endif()
endif()
if(BUILD_TESTING AND LLCV_RECORDING_FFMPEG_ROOT)
    add_executable(RecordingMuxFaultTests tests/RecordingMuxFaultTests.cpp src/recording/RecordingContainer.cpp)
    target_include_directories(RecordingMuxFaultTests PRIVATE src)
    target_compile_definitions(RecordingMuxFaultTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingMuxFaultTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingMuxFaultTests PRIVATE ole32 mf mfplat mfreadwrite mfuuid)
    set_target_properties(RecordingMuxFaultTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME RecordingMuxFaultTests COMMAND RecordingMuxFaultTests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recording/mux-hevc-flash-tone.mp4")
    set_tests_properties(RecordingMuxFaultTests PROPERTIES TIMEOUT 30)
    add_test(NAME RecordingColorNegotiationTests COMMAND RecordingMuxFaultTests --color-negotiation)
    set_tests_properties(RecordingColorNegotiationTests PROPERTIES TIMEOUT 15)
    add_test(NAME RecordingHevcSyncTests COMMAND RecordingTests --hevc)
    add_test(NAME RecordingHevcArrivalSyncTests COMMAND RecordingTests --hevc-arrival)
    set_tests_properties(RecordingHevcSyncTests RecordingHevcArrivalSyncTests PROPERTIES
        TIMEOUT 45 RUN_SERIAL TRUE LABELS GPU SKIP_RETURN_CODE 77)
    add_executable(RecordingModernContainerTests tests/RecordingModernContainerTests.cpp
        src/recording/EncoderSupport.cpp src/recording/RecordingOptions.cpp)
    target_include_directories(RecordingModernContainerTests PRIVATE src)
    target_compile_definitions(RecordingModernContainerTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(RecordingModernContainerTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingModernContainerTests PRIVATE ole32 oleaut32 mfplat mfreadwrite mfuuid strmiids)
    set_target_properties(RecordingModernContainerTests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_executable(RecordingStorageCrashTests tests/RecordingModernContainerTests.cpp
        src/recording/EncoderSupport.cpp src/recording/RecordingOptions.cpp src/recording/RecordingStorage.cpp)
    target_include_directories(RecordingStorageCrashTests PRIVATE src)
    target_compile_definitions(RecordingStorageCrashTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN LLCV_RECORDING_STORAGE_DELAY_TEST)
    target_compile_options(RecordingStorageCrashTests PRIVATE /W4 /EHsc /utf-8)
    target_link_libraries(RecordingStorageCrashTests PRIVATE ole32 oleaut32 mfplat mfreadwrite mfuuid strmiids)
    set_target_properties(RecordingStorageCrashTests PROPERTIES WIN32_EXECUTABLE FALSE)
    foreach(codec av1 hevc hevc-bframes)
        add_test(NAME RecordingModern_${codec}_Recovery COMMAND RecordingModernContainerTests
            --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recording/mux-${codec}-flash-tone.mp4")
        set_tests_properties(RecordingModern_${codec}_Recovery PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endforeach()
    # Independent decoding is test-only; these programs are never distributed
    # with the app. MF's reader normalizes some fragmented track start times,
    # so absolute PTS/DTS and visible/audible sync need a second implementation.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(LLCV_TEST_FFMPEG NAMES ffmpeg DOC "FFmpeg executable for independent recording regression tests")
    find_program(LLCV_TEST_FFPROBE NAMES ffprobe DOC "FFprobe executable for independent recording regression tests")
    if(Python3_Interpreter_FOUND AND LLCV_TEST_FFMPEG AND LLCV_TEST_FFPROBE)
        foreach(codec av1 hevc)
            add_test(NAME RecordingSegments_${codec}_Crash COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/Verify-RecordingSegments.py"
                --test-exe "$<TARGET_FILE:RecordingModernContainerTests>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recording/mux-${codec}-flash-tone.mp4"
                --ffmpeg "${LLCV_TEST_FFMPEG}" --ffprobe "${LLCV_TEST_FFPROBE}" --mode time --crash)
            set_tests_properties(RecordingSegments_${codec}_Crash PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
            add_test(NAME RecordingStorageCrash_${codec} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/Verify-RecordingSegments.py"
                --test-exe "$<TARGET_FILE:RecordingStorageCrashTests>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recording/mux-${codec}-flash-tone.mp4"
                --ffmpeg "${LLCV_TEST_FFMPEG}" --ffprobe "${LLCV_TEST_FFPROBE}" --mode time --crash)
            set_tests_properties(RecordingStorageCrash_${codec} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        endforeach()
        foreach(codec av1 hevc hevc-bframes hevc-closed-bframes)
            set(split_args)
            if(codec STREQUAL "hevc-bframes")
                set(split_args --allow-deferred)
            endif()
            foreach(mode time size)
                add_test(NAME RecordingSegments_${codec}_${mode} COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/Verify-RecordingSegments.py"
                    --test-exe "$<TARGET_FILE:RecordingModernContainerTests>"
                    --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recording/mux-${codec}-flash-tone.mp4"
                    --ffmpeg "${LLCV_TEST_FFMPEG}" --ffprobe "${LLCV_TEST_FFPROBE}" --mode ${mode} ${split_args})
                set_tests_properties(RecordingSegments_${codec}_${mode} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
            endforeach()
        endforeach()
        foreach(case av1 hevc hevc-bframes audio-offset video-offset)
            set(fixture_codec "${case}")
            set(offset_args)
            if(case STREQUAL "audio-offset")
                set(fixture_codec hevc)
                set(offset_args --audio-offset-ms 10)
            elseif(case STREQUAL "video-offset")
                set(fixture_codec av1)
                set(offset_args --video-offset-ms 15)
            endif()
            add_test(NAME RecordingIndependent_${case} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/Verify-RecordingMux.py"
                --test-exe "$<TARGET_FILE:RecordingModernContainerTests>"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recording/mux-${fixture_codec}-flash-tone.mp4"
                --ffmpeg "${LLCV_TEST_FFMPEG}" --ffprobe "${LLCV_TEST_FFPROBE}" ${offset_args})
            set_tests_properties(RecordingIndependent_${case} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
        endforeach()
    else()
        message(STATUS "Independent recording media tests disabled: set LLCV_TEST_FFMPEG/LLCV_TEST_FFPROBE and install Python 3.")
    endif()
endif()

get_property(llcv_build_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(llcv_target IN LISTS llcv_build_targets)
    get_target_property(llcv_sources ${llcv_target} SOURCES)
    if("tests/RecordingTests.cpp" IN_LIST llcv_sources)
        target_link_libraries(${llcv_target} PRIVATE windowscodecs)
    endif()
    if("src/recording/Recorder.cpp" IN_LIST llcv_sources)
        foreach(llcv_decode_source src/video/MjpegDecoder.cpp src/video/VideoColor.cpp)
            if(NOT "${llcv_decode_source}" IN_LIST llcv_sources)
                target_sources(${llcv_target} PRIVATE ${llcv_decode_source})
            endif()
        endforeach()
    endif()
    if("src/recording/EncoderSupport.cpp" IN_LIST llcv_sources AND
       NOT "src/recording/RecordingContainer.cpp" IN_LIST llcv_sources)
        target_sources(${llcv_target} PRIVATE src/recording/RecordingContainer.cpp)
    endif()
    if("src/recording/EncoderSupport.cpp" IN_LIST llcv_sources OR
       "src/recording/RecordingContainer.cpp" IN_LIST llcv_sources)
        llcv_enable_recording_mux(${llcv_target})
    endif()
    if("src/ui/SettingsView.cpp" IN_LIST llcv_sources OR
       "src/ui/ViewerHelpWindow.cpp" IN_LIST llcv_sources OR
       "src/ui/AudioOnlyView.cpp" IN_LIST llcv_sources OR
       "tests/RecordingControlsTests.cpp" IN_LIST llcv_sources)
        target_sources(${llcv_target} PRIVATE src/ui/SettingsFonts.cpp src/ui/SettingsFonts.rc)
    endif()
    if("src/ui/SettingsTheme.cpp" IN_LIST llcv_sources)
        target_link_libraries(${llcv_target} PRIVATE uxtheme)
    endif()
    if("src/ui/SettingsView.cpp" IN_LIST llcv_sources AND
       NOT "src/ui/SettingsCompactLayout.cpp" IN_LIST llcv_sources)
        target_sources(${llcv_target} PRIVATE src/ui/SettingsCompactLayout.cpp)
    endif()
endforeach()

if(BUILD_TESTING)
    add_test(NAME RendererResizeTests COMMAND HdrPipelineTests --resize)
    set_tests_properties(RendererResizeTests PROPERTIES LABELS GPU SKIP_RETURN_CODE 77 TIMEOUT 120)
    find_program(LLCV_POWERSHELL NAMES pwsh powershell)
    if(LLCV_POWERSHELL)
        if(LLCV_RECORDING_FFMPEG_ROOT AND LLCV_TEST_FFPROBE)
            add_test(NAME RecordingMjpegCrashRecovery COMMAND "${LLCV_POWERSHELL}"
                -NoProfile -ExecutionPolicy Bypass -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/Test-MjpegRecordingRecovery.ps1"
                -Recorder "$<TARGET_FILE:RecordingTests>" -Probe "${LLCV_TEST_FFPROBE}")
            set_tests_properties(RecordingMjpegCrashRecovery PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
            foreach(raw_input Nv12 Yuy2)
                add_test(NAME Recording${raw_input}CrashRecovery COMMAND "${LLCV_POWERSHELL}"
                    -NoProfile -ExecutionPolicy Bypass -File
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/Test-MjpegRecordingRecovery.ps1"
                    -Recorder "$<TARGET_FILE:RecordingTests>" -Probe "${LLCV_TEST_FFPROBE}"
                    -InputFormat ${raw_input})
                set_tests_properties(Recording${raw_input}CrashRecovery PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
            endforeach()
        endif()
        add_test(NAME PackageDocsTests COMMAND "${LLCV_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/PackageDocsTests.ps1")
        set_tests_properties(PackageDocsTests PROPERTIES TIMEOUT 60)
        if(LLCV_RECORDING AND NOT LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC)
            add_test(NAME ReleasePackageTests COMMAND "${LLCV_POWERSHELL}"
                -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/ReleasePackageTests.ps1"
                -BuildRoot "${CMAKE_CURRENT_BINARY_DIR}" -SourceRoot "${CMAKE_CURRENT_SOURCE_DIR}"
                -Version "${PROJECT_VERSION}" -Configuration "$<CONFIG>")
            set_tests_properties(ReleasePackageTests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        endif()
    endif()
endif()

if(BUILD_TESTING AND MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    add_test(NAME BuildDependencyTests COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-test"
        "-DCOMPILER=${CMAKE_CXX_COMPILER}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildDependencyTests.cmake")
    set_tests_properties(BuildDependencyTests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(BUILD_TESTING AND MSVC)
    # Verify preprocessor exclusion, not merely optimizer removal of idle code.
    set(llcv_recovery_rsp "/nologo\n/TP\n/std:c++20\n/utf-8\n")
    string(APPEND llcv_recovery_rsp "/D\"$<JOIN:$<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>,\"\n/D\">\"\n")
    string(APPEND llcv_recovery_rsp "/I\"$<JOIN:$<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>,\"\n/I\">\"\n")
    set(llcv_recovery_system_includes $ENV{INCLUDE} ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
    list(REMOVE_DUPLICATES llcv_recovery_system_includes)
    foreach(llcv_recovery_include IN LISTS llcv_recovery_system_includes)
        if(NOT llcv_recovery_include STREQUAL "")
            file(TO_CMAKE_PATH "${llcv_recovery_include}" llcv_recovery_include)
            string(APPEND llcv_recovery_rsp "/I\"${llcv_recovery_include}\"\n")
        endif()
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/audio-recovery-boundary/$<CONFIG>/main.rsp"
        CONTENT "${llcv_recovery_rsp}")
    add_test(NAME AudioRecoveryBuildBoundaryTests COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER}"
        "-DARGUMENTS_FILE=${CMAKE_CURRENT_BINARY_DIR}/audio-recovery-boundary/$<CONFIG>/main.rsp"
        "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/audio-recovery-boundary/$<CONFIG>/results"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/AudioRecoveryBuildBoundaryTests.cmake")
    set_tests_properties(AudioRecoveryBuildBoundaryTests PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE LABELS Build)

    # The private branches live in main.cpp and are invisible to normal tests.
    # Compile that translation unit in each supported mode without linking or
    # opening hardware. Keep the extra objects out of ordinary application builds.
    set(llcv_optional_build_targets)
    foreach(llcv_optional_mode VSR_EXPERIMENT HDR_FRAME_AUDIT HDR_SCRGB_PROTOTYPE AUDIO_INPUT_RECOVERY_DIAGNOSTIC)
        set(llcv_optional_target "OptionalBuild_${llcv_optional_mode}")
        add_library(${llcv_optional_target} OBJECT EXCLUDE_FROM_ALL src/main.cpp)
        target_include_directories(${llcv_optional_target} PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
        target_compile_options(${llcv_optional_target} PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_OPTIONS>)
        target_compile_features(${llcv_optional_target} PRIVATE
            $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_FEATURES>)
        # An experimental parent build must not leak its mode into other variants.
        target_compile_definitions(${llcv_optional_target} PRIVATE
            "$<FILTER:$<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>,EXCLUDE,^LLCV_(VSR_EXPERIMENT|HDR_FRAME_AUDIT|HDR_SCRGB_PROTOTYPE|AUDIO_INPUT_RECOVERY_DIAGNOSTIC)(=.*)?$>"
            "LLCV_${llcv_optional_mode}")
        if(llcv_optional_mode STREQUAL "HDR_SCRGB_PROTOTYPE")
            target_compile_definitions(${llcv_optional_target} PRIVATE LLCV_HDR_FRAME_AUDIT)
        endif()
        list(APPEND llcv_optional_build_targets ${llcv_optional_target})
    endforeach()
    add_custom_target(OptionalBuildSyntaxTests DEPENDS ${llcv_optional_build_targets})
    add_test(NAME OptionalBuildSyntaxTests COMMAND "${CMAKE_COMMAND}"
        --build "${CMAKE_CURRENT_BINARY_DIR}" --config "$<CONFIG>"
        --target OptionalBuildSyntaxTests --parallel 3)
    set_tests_properties(OptionalBuildSyntaxTests PROPERTIES
        TIMEOUT 180 RUN_SERIAL TRUE LABELS Build)
endif()

if(LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC)
    install(CODE "message(FATAL_ERROR \"Audio-input recovery diagnostics cannot be installed as a regular release\")")
endif()
install(TARGETS LowLatencyCaptureViewer RUNTIME DESTINATION .)
if(LLCV_RECORDING)
    if(NOT EXISTS "${LLCV_RECORDING_FFMPEG_ROOT}/licenses/COPYING.LGPLv2.1")
        message(FATAL_ERROR "Recording distribution needs the pinned SDK licenses directory")
    endif()
    add_custom_command(TARGET LowLatencyCaptureViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${LLCV_RECORDING_FFMPEG_ROOT}/licenses"
            "$<TARGET_FILE_DIR:LowLatencyCaptureViewer>/ffmpeg-licenses" VERBATIM)
    install(FILES ${llcv_recording_runtime_dlls} DESTINATION .)
    install(DIRECTORY "${LLCV_RECORDING_FFMPEG_ROOT}/licenses/" DESTINATION ffmpeg-licenses)
endif()
install(FILES LICENSE README.md README.ko.md DEPENDENCIES.txt
        "실행안내.txt" DESTINATION .)
install(FILES third_party/asio/LICENSE.txt DESTINATION . RENAME ASIO-SDK-LICENSE.txt)
install(FILES third_party/asio/HOST-LICENSE.txt DESTINATION . RENAME ASIO-HOST-LICENSE.txt)
install(FILES third_party/pretendard/LICENSE.txt DESTINATION . RENAME PRETENDARD-LICENSE.txt)
# Keep exclusions in sync with Copy-PackageDocs.ps1 and the installer.
install(DIRECTORY docs DESTINATION .
        PATTERN "viewer.png" EXCLUDE
        PATTERN "rounded-corners-validation.md" EXCLUDE
        PATTERN "TEST-BLT.ko.md" EXCLUDE
        PATTERN "vsr-all-formats-validation.md" EXCLUDE
        PATTERN "vsr-experiment.md" EXCLUDE
        PATTERN "vsr-hdr-validation.md" EXCLUDE)
