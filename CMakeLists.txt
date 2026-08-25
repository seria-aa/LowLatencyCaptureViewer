cmake_minimum_required(VERSION 3.24)
project(LowLatencyCaptureViewer VERSION 1.2.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WIN32_EXECUTABLE ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(LowLatencyCaptureViewer
    src/main.cpp
    src/app.rc
    src/audio/AsioOutput.cpp
    src/audio/AudioMix.cpp
    src/audio/CaptureAudioFormat.cpp
    src/diagnostics/Logger.cpp
    src/ui/AudioOsdLayout.cpp
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
endif()

install(TARGETS LowLatencyCaptureViewer RUNTIME DESTINATION .)
install(FILES LICENSE README.md README.ko.md DESIGN.txt BUILD_INFO.txt DEPENDENCIES.txt
        "실행안내.txt" GITHUB_PREP.md DESTINATION .)
install(FILES third_party/asio/LICENSE.txt DESTINATION . RENAME ASIO-SDK-LICENSE.txt)
install(DIRECTORY docs DESTINATION .)
