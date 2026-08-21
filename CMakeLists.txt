cmake_minimum_required(VERSION 3.20)
project(ScreenLiveStreamWindows CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus /EHsc)
    add_compile_options(/await:strict /MP)
endif()

# ======================== 激进性能与链接时优化 (LTO / LTCG) ========================
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    message(STATUS "[Optimize] Link-Time Code Generation (LTO / LTCG) Enabled for Release")
endif()

# =================================================================================

set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")

# Automatically fetch FFmpeg prebuilt binaries if not present locally
if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h" OR NOT EXISTS "${FFMPEG_DIR}/lib/avcodec.lib")
    message(STATUS "FFmpeg prebuilt dependencies not found in ${FFMPEG_DIR}. Fetching automatically...")
    
    set(FFMPEG_ZIP_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip")
    set(FFMPEG_ZIP_PATH "${CMAKE_BINARY_DIR}/ffmpeg-download.zip")
    set(FFMPEG_EXTRACT_DIR "${CMAKE_BINARY_DIR}/ffmpeg_tmp_extract")
    
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}")
    file(DOWNLOAD "${FFMPEG_ZIP_URL}" "${FFMPEG_ZIP_PATH}"
        SHOW_PROGRESS
        STATUS DL_STATUS
    )
    list(GET DL_STATUS 0 DL_CODE)
    if(NOT DL_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download FFmpeg prebuilts: ${DL_STATUS}. You can manually download from ${FFMPEG_ZIP_URL} and extract to ${FFMPEG_DIR}")
    endif()

    message(STATUS "Extracting FFmpeg prebuilts...")
    file(MAKE_DIRECTORY "${FFMPEG_EXTRACT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${FFMPEG_ZIP_PATH}" DESTINATION "${FFMPEG_EXTRACT_DIR}")
    
    file(GLOB SUB_ITEMS "${FFMPEG_EXTRACT_DIR}/*")
    list(GET SUB_ITEMS 0 EXTRACTED_ROOT)
    
    file(MAKE_DIRECTORY "${FFMPEG_DIR}")
    file(COPY "${EXTRACTED_ROOT}/" DESTINATION "${FFMPEG_DIR}")
    
    file(REMOVE_RECURSE "${FFMPEG_EXTRACT_DIR}")
    file(REMOVE "${FFMPEG_ZIP_PATH}")
    message(STATUS "FFmpeg dependencies ready at ${FFMPEG_DIR}")
endif()

include_directories(
    src
    src/common
    src/capture
    src/encoder
    src/decoder
    src/render
    src/net
    src/ui
    ${FFMPEG_DIR}/include
)

link_directories(
    ${FFMPEG_DIR}/lib
)

file(GLOB_RECURSE SRC_FILES
    "src/*.cpp"
    "src/*.h"
)

add_executable(ScreenLiveStreamWindows
    WIN32
    ${SRC_FILES}
)

target_link_libraries(ScreenLiveStreamWindows PRIVATE
    avcodec
    avutil
    swscale
    avformat
    d3d11
    dxgi
    d3dcompiler
    mfplat
    mfuuid
    mfreadwrite
    wmcodecdspuuid
    avrt
    ws2_32
    iphlpapi
    windowsapp
    comctl32
    dwmapi
    uxtheme
    gdiplus
)

set_target_properties(ScreenLiveStreamWindows PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if(MSVC)
    # 宏定义与 SSE4.2 支持声明
    target_compile_definitions(ScreenLiveStreamWindows PRIVATE
        __SSE4_2__=1
    )

    # Release 模式激进优化选项
    target_compile_options(ScreenLiveStreamWindows PRIVATE
        $<$<CONFIG:Release>:
            /O2             # 最大速度
            /Ob3            # 极度激进内联 (VS2019+)
            /Oi             # 启用内置内部函数 (Intrinsic functions)
            /Ot             # 偏向代码执行速度
            /Oy             # 省略帧指针 (释放通用寄存器)
            /fp:fast        # 快速浮点计算 (放宽 IEEE 754，允许向量化重排)
            /Gw             # 全局数据优化
            /Gy             # 函数级链接 (支持死代码消除)
            /GF             # 字符串池优化
            /Zc:inline      # 剔除未引用的内联函数
            /GS-            # 关闭栈安全检查 (消除高频函数调用压栈/检查开销)
        >
    )

    # Release 模式链接器激进优化
    target_link_options(ScreenLiveStreamWindows PRIVATE
        $<$<CONFIG:Release>:
            /OPT:REF        # 剔除未引用的函数和数据 (Dead-code elimination)
            /OPT:ICF        # 折叠完全相同的代码段 (Identical COMDAT Folding)
        >
    )
endif()

# Copy FFmpeg DLLs to output directory
add_custom_command(TARGET ScreenLiveStreamWindows POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${FFMPEG_DIR}/bin"
    "$<TARGET_FILE_DIR:ScreenLiveStreamWindows>"
)

