# Explicit-only timing tools. Neither these targets nor alternate shaders are installed.
set(llcv_preserve_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/cmaa-preserve-timing")
file(MAKE_DIRECTORY "${llcv_preserve_dir}")
string(REPLACE "CMAA2_EXTRA_SHARPNESS=0" "CMAA2_EXTRA_SHARPNESS=1" llcv_preserve_defines "${cmaa_defines}")
set(llcv_preserve_headers "")
foreach(pair IN ITEMS "cmaaEdges,EdgesColor2x2CS" "cmaaArgs,ComputeDispatchArgsCS" "cmaaCandidates,ProcessCandidatesCS" "cmaaApply,DeferredColorApply2x2CS")
    string(REPLACE "," ";" fields "${pair}")
    list(GET fields 0 name)
    list(GET fields 1 entry)
    set(header "${llcv_preserve_dir}/${name}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${LLCV_FXC}" /nologo /O3 /T cs_5_0 /E ${entry} /Vn ${name} /Fh "${header}"
            ${llcv_preserve_defines} /D CMAA2_SUPPORT_HDR_COLOR_RANGE=0
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/upscaling/cmaa2/CMAA2.hlsl"
        DEPENDS third_party/upscaling/cmaa2/CMAA2.hlsl VERBATIM)
    list(APPEND llcv_preserve_headers "${header}")
endforeach()

# Interactive full-UI package uses the production selectable shader library,
# not either fixed-variant latency executable. TestProfile is added centrally.
get_target_property(llcv_ui_sources LowLatencyCaptureViewer SOURCES)
list(FILTER llcv_ui_sources EXCLUDE REGEX "src/main.cpp$")
add_executable(LLCV_Upscaling_UI_Test WIN32 EXCLUDE_FROM_ALL tests/UpscalingUiViewer.cpp ${llcv_ui_sources})
set_property(SOURCE tests/UpscalingUiViewer.cpp APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_include_directories(LLCV_Upscaling_UI_Test PRIVATE $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
target_compile_definitions(LLCV_Upscaling_UI_Test PRIVATE $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>)
target_compile_options(LLCV_Upscaling_UI_Test PRIVATE /W4 /EHsc /permissive- /utf-8)
target_link_libraries(LLCV_Upscaling_UI_Test PRIVATE $<TARGET_PROPERTY:LowLatencyCaptureViewer,LINK_LIBRARIES>)
if(COMMAND llcv_enable_recording_mux)
    llcv_enable_recording_mux(LLCV_Upscaling_UI_Test)
endif()
add_library(llcv_preserve_timing STATIC EXCLUDE_FROM_ALL src/video/CmaaNisUpscaler.cpp ${llcv_preserve_headers})
target_include_directories(llcv_preserve_timing PRIVATE "${llcv_preserve_dir}" "${llcv_shader_dir}")
target_compile_definitions(llcv_preserve_timing PRIVATE NOMINMAX)
target_compile_options(llcv_preserve_timing PRIVATE /W4 /EHsc /permissive- /utf-8)
target_link_libraries(llcv_preserve_timing PRIVATE d3d11)
add_dependencies(llcv_preserve_timing llcv_upscaling)

# Compile one identical viewer object set; only the linked CMAA shader library differs.
get_target_property(llcv_timing_sources LowLatencyCaptureViewer SOURCES)
add_library(CmaaLatencyViewerObjects OBJECT EXCLUDE_FROM_ALL ${llcv_timing_sources} src/video/SpatialUpscaler.cpp)
target_include_directories(CmaaLatencyViewerObjects PRIVATE $<TARGET_PROPERTY:LowLatencyCaptureViewer,INCLUDE_DIRECTORIES>)
target_compile_definitions(CmaaLatencyViewerObjects PRIVATE
    $<TARGET_PROPERTY:LowLatencyCaptureViewer,COMPILE_DEFINITIONS>
    LLCV_TEST_PROFILE LLCV_SPATIAL_PROTOTYPE LLCV_SPATIAL_SOURCE_ROOT=L"${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(CmaaLatencyViewerObjects PRIVATE /W4 /EHsc /permissive- /utf-8)
get_target_property(llcv_timing_links LowLatencyCaptureViewer LINK_LIBRARIES)
list(REMOVE_ITEM llcv_timing_links llcv_upscaling)
target_link_libraries(CmaaLatencyViewerObjects PRIVATE ${llcv_timing_links})
foreach(variant IN ITEMS High Preserve)
    set(target "LLCV_Cmaa${variant}Latency")
    add_executable(${target} WIN32 EXCLUDE_FROM_ALL $<TARGET_OBJECTS:CmaaLatencyViewerObjects>)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(${target} PRIVATE src)
    target_link_libraries(${target} PRIVATE ${llcv_timing_links})
    if(COMMAND llcv_enable_recording_mux)
        llcv_enable_recording_mux(${target})
    endif()
    if(variant STREQUAL "Preserve")
        target_link_libraries(${target} PRIVATE llcv_preserve_timing)
    else()
        target_link_libraries(${target} PRIVATE llcv_upscaling)
    endif()
endforeach()
