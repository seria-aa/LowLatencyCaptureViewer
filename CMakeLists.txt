cmake_minimum_required(VERSION 3.24)
project(LowLatencyCaptureViewer VERSION 1.2.4 LANGUAGES CXX)

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
    src/ui/WindowGeometry.cpp
    src/update/UpdateChecker.cpp
    src/video/DirectShowVideoFormat.cpp
    src/video/MjpegDecoder.cpp
    src/video/VideoColor.cpp
)

target_include_directories(LowLatencyCaptureViewer PRIVATE
    src
    third_party/asio/common
    third_party/asio/host
    third_party/asio/host/pc
)

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
endif()

install(TARGETS LowLatencyCaptureViewer RUNTIME DESTINATION .)
install(FILES LICENSE README.md README.ko.md DESIGN.txt BUILD_INFO.txt DEPENDENCIES.txt
        "실행안내.txt" GITHUB_PREP.md DESTINATION .)
install(FILES third_party/asio/LICENSE.txt DESTINATION . RENAME ASIO-SDK-LICENSE.txt)
install(DIRECTORY docs DESTINATION .)
