# Compile only the shipped MIT NIS and Apache-2.0 CMAA2 shaders into the binary.
# No source paths, shader compiler, or experimental/GPL kernels at runtime.
get_filename_component(llcv_sdk_bin "${CMAKE_RC_COMPILER}" DIRECTORY)
file(GLOB llcv_fxc_candidates "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/fxc.exe")
list(SORT llcv_fxc_candidates COMPARE NATURAL ORDER DESCENDING)
find_program(LLCV_FXC fxc HINTS "${llcv_sdk_bin}" "$ENV{WindowsSdkVerBinPath}/x64")
if(NOT LLCV_FXC AND llcv_fxc_candidates)
    list(GET llcv_fxc_candidates 0 LLCV_FXC)
endif()
if(NOT LLCV_FXC)
    message(FATAL_ERROR "Windows SDK fxc is required to embed upscaling shaders")
endif()
set(llcv_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/upscaling")
file(MAKE_DIRECTORY "${llcv_shader_dir}")
function(llcv_shader name file entry profile)
    cmake_parse_arguments(PARSE_ARGV 4 shader "" "" "DEPENDS")
    set(output "${llcv_shader_dir}/${name}.h")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${LLCV_FXC}" /nologo /O3 /T ${profile} /E ${entry} /Vn ${name} /Fh "${output}" ${shader_UNPARSED_ARGUMENTS} "${CMAKE_CURRENT_SOURCE_DIR}/${file}"
        DEPENDS "${file}" ${shader_DEPENDS}
        VERBATIM)
    set(llcv_shader_headers ${llcv_shader_headers} "${output}" PARENT_SCOPE)
endfunction()
llcv_shader(uyvyRepack src/video/shaders/UyvyRepack.hlsl main cs_5_0)
add_library(llcv_uyvy STATIC src/video/UyvyRepacker.cpp "${llcv_shader_dir}/uyvyRepack.h")
target_include_directories(llcv_uyvy PRIVATE "${llcv_shader_dir}")
target_compile_definitions(llcv_uyvy PRIVATE NOMINMAX)
target_compile_options(llcv_uyvy PRIVATE /W4 /EHsc /permissive- /utf-8)
target_link_libraries(llcv_uyvy PRIVATE d3d11)
set(llcv_shader_headers "")
set(transfer src/video/shaders/UpscaleTransfer.hlsl)
llcv_shader(transferVertex ${transfer} Vertex vs_5_0)
llcv_shader(copyPixel ${transfer} Copy ps_5_0)
llcv_shader(decode709 ${transfer} Decode ps_5_0)
llcv_shader(encode709 ${transfer} Encode ps_5_0)
set(cmaa_defines /D CMAA2_STATIC_QUALITY_PRESET=2 /D CMAA2_EXTRA_SHARPNESS=0
    /D CMAA2_EDGE_DETECTION_LUMA_PATH=0 /D CMAA2_USE_HALF_FLOAT_PRECISION=0
    /D CMAA2_COLLECT_EXPAND_BLEND_ITEMS=0 /D CMAA2_UAV_STORE_TYPED=1
    /D CMAA2_UAV_STORE_TYPED_UNORM_FLOAT=0 /D CMAA2_UAV_STORE_CONVERT_TO_SRGB=0)
foreach(pair IN ITEMS "cmaaEdges,EdgesColor2x2CS" "cmaaArgs,ComputeDispatchArgsCS" "cmaaCandidates,ProcessCandidatesCS" "cmaaApply,DeferredColorApply2x2CS")
    string(REPLACE "," ";" fields "${pair}")
    list(GET fields 0 name)
    list(GET fields 1 entry)
    llcv_shader(${name} third_party/upscaling/cmaa2/CMAA2.hlsl ${entry} cs_5_0
        ${cmaa_defines} /D CMAA2_SUPPORT_HDR_COLOR_RANGE=0)
endforeach()
foreach(threads IN ITEMS 128 256)
    # NIS_Config.h is host-side C++ only; the compiler tracks that include.
    # Only NIS shaders include NIS_Scaler.h, not CMAA2 or the transfer passes.
    llcv_shader(nis${threads} third_party/upscaling/nis/NIS_Main.hlsl main cs_5_0
        /D NIS_SCALER=1 /D NIS_HDR_MODE=0 /D NIS_NV12_SUPPORT=0 /D NIS_VIEWPORT_SUPPORT=1
        /D NIS_CLAMP_OUTPUT=1 /D NIS_BLOCK_WIDTH=32 /D NIS_BLOCK_HEIGHT=24 /D NIS_THREAD_GROUP_SIZE=${threads}
        DEPENDS third_party/upscaling/nis/NIS_Scaler.h)
endforeach()
add_library(llcv_upscaling STATIC src/video/CmaaNisUpscaler.cpp ${llcv_shader_headers})
target_include_directories(llcv_upscaling PRIVATE "${llcv_shader_dir}")
target_compile_definitions(llcv_upscaling PRIVATE NOMINMAX)
target_compile_options(llcv_upscaling PRIVATE /W4 /EHsc /permissive- /utf-8)
target_link_libraries(llcv_upscaling PRIVATE d3d11)
if(LLCV_UPSCALING_DIAGNOSTIC)
    add_library(llcv_upscaling_diagnostic STATIC EXCLUDE_FROM_ALL
        src/video/CmaaNisUpscaler.cpp src/video/UpscalingOverflowTelemetry.cpp ${llcv_shader_headers})
    target_include_directories(llcv_upscaling_diagnostic PRIVATE "${llcv_shader_dir}")
    # PUBLIC is required: diagnostic CmaaPipeline layout must match every consumer.
    target_compile_definitions(llcv_upscaling_diagnostic PUBLIC LLCV_UPSCALING_TELEMETRY PRIVATE NOMINMAX)
    target_compile_options(llcv_upscaling_diagnostic PRIVATE /W4 /EHsc /permissive- /utf-8)
    target_link_libraries(llcv_upscaling_diagnostic PRIVATE d3d11)
endif()
