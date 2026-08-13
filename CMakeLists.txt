cmake_minimum_required(VERSION 3.24)
project(LowLatencyCaptureViewer VERSION 0.16.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WIN32_EXECUTABLE ON)

add_executable(LowLatencyCaptureViewer
    src/main.cpp
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
)

if (MSVC)
    target_compile_options(LowLatencyCaptureViewer PRIVATE /W4 /EHsc /permissive- /utf-8)
endif()

install(TARGETS LowLatencyCaptureViewer RUNTIME DESTINATION .)
install(FILES README.md DESIGN.txt BUILD_INFO.txt DEPENDENCIES.txt
        "실행안내.txt" GITHUB_PREP.md settings.ini DESTINATION .)
