# Packet muxing only. The recorder keeps Media Foundation encoders;
# no FFmpeg executable, decoder or software encoder is used by the app.
set(LLCV_RECORDING_FFMPEG_ROOT "" CACHE PATH "Pinned FFmpeg SDK prefix for HEVC/AV1 MP4 recording")
if(LLCV_RECORDING AND NOT LLCV_RECORDING_FFMPEG_ROOT)
    message(FATAL_ERROR "Recording requires LLCV_RECORDING_FFMPEG_ROOT. See tools/Build-RecordingFFmpeg.ps1, or configure -DLLCV_RECORDING=OFF for a playback-only build.")
endif()
if(LLCV_RECORDING_FFMPEG_ROOT)
    file(GLOB llcv_recording_resample_dll "${LLCV_RECORDING_FFMPEG_ROOT}/bin/swresample-*.dll")
    if(llcv_recording_resample_dll OR EXISTS "${LLCV_RECORDING_FFMPEG_ROOT}/licenses/dolby-ffmpeg-NOTICE.txt")
        message(FATAL_ERROR "Use the mux-only FFmpeg SDK for LLCV_RECORDING_FFMPEG_ROOT, not the Dolby preview SDK. Use LLCV_DOLBY_SDK for a Dolby preview.")
    endif()
    foreach(component avformat avcodec avutil)
        if(NOT EXISTS "${LLCV_RECORDING_FFMPEG_ROOT}/include/lib${component}/${component}.h" OR
           NOT EXISTS "${LLCV_RECORDING_FFMPEG_ROOT}/lib/${component}.lib")
            message(FATAL_ERROR "Incomplete recording FFmpeg SDK: ${component}")
        endif()
        file(GLOB component_dlls "${LLCV_RECORDING_FFMPEG_ROOT}/bin/${component}-*.dll")
        list(LENGTH component_dlls component_count)
        if(NOT component_count EQUAL 1)
            message(FATAL_ERROR "Expected one ${component} runtime DLL in the pinned SDK")
        endif()
        list(GET component_dlls 0 component_dll)
        add_library(LLCV_${component} SHARED IMPORTED)
        set_target_properties(LLCV_${component} PROPERTIES
            IMPORTED_IMPLIB "${LLCV_RECORDING_FFMPEG_ROOT}/lib/${component}.lib"
            IMPORTED_LOCATION "${component_dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${LLCV_RECORDING_FFMPEG_ROOT}/include")
        list(APPEND llcv_recording_runtime_dlls "${component_dll}")
    endforeach()
    # The mux-only runtime excludes resampling and all FFmpeg tools.
    add_library(LLCV_RecordingMux STATIC src/recording/FragmentedMp4Sink.cpp)
    target_include_directories(LLCV_RecordingMux PRIVATE src)
    target_compile_definitions(LLCV_RecordingMux PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(LLCV_RecordingMux PRIVATE /W4 /EHsc /permissive- /utf-8)
    target_link_libraries(LLCV_RecordingMux PUBLIC LLCV_RecordingStorage LLCV_avformat LLCV_avcodec LLCV_avutil
        ole32 mfplat mfuuid)
endif()

function(llcv_enable_recording_mux target)
    target_link_libraries(${target} PRIVATE LLCV_RecordingStorage)
    if(LLCV_RECORDING_FFMPEG_ROOT)
        target_compile_definitions(${target} PRIVATE LLCV_RECORDING_MODERN_MP4)
        target_link_libraries(${target} PRIVATE LLCV_RecordingMux)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${llcv_recording_runtime_dlls} "$<TARGET_FILE_DIR:${target}>"
            COMMAND_EXPAND_LISTS VERBATIM)
    endif()
endfunction()
