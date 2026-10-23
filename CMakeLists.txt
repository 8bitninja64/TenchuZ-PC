cmake_minimum_required(VERSION 3.24)
project(TenchuZRecomp LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Tenchu Z Recomp requires 64-bit Windows")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Use the ClangCL toolset (-T ClangCL); generated code relies on Clang extensions")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/MP)

set(TENCHU_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated" CACHE PATH "XenonRecomp output (tools/recompile.py)")
set(TENCHU_SIMDE_DIR "${CMAKE_SOURCE_DIR}/../_Tools/XenonRecomp/thirdparty/simde" CACHE PATH "simde headers")
if(NOT EXISTS "${TENCHU_GENERATED_DIR}/sources.cmake")
    message(FATAL_ERROR "Run tools/recompile.py first to generate ${TENCHU_GENERATED_DIR}")
endif()
include("${TENCHU_GENERATED_DIR}/sources.cmake")

# Translated game code.
add_library(TenchuPPC STATIC ${TENCHU_PPC_SOURCES})
target_include_directories(TenchuPPC PUBLIC "${TENCHU_GENERATED_DIR}" "${TENCHU_SIMDE_DIR}")
target_compile_definitions(TenchuPPC PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# Exact floating point; SSSE3 byte shuffles are exact and fast.
target_compile_options(TenchuPPC PRIVATE /fp:strict /clang:-mssse3 /clang:-msse4.1 -Wno-everything)
target_compile_options(TenchuPPC PUBLIC /EHsc)

# Kernel, devices and host integration.
add_library(TenchuRuntime STATIC
    runtime/kernel/memory.cpp
    runtime/kernel/xex.cpp
    runtime/kernel/kernel.cpp
    runtime/kernel/control.cpp
    runtime/kernel/host_profiler.cpp
    runtime/host/settings.cpp
    runtime/host/input.cpp
    runtime/kernel/kernel_data.cpp
    runtime/kernel/format.cpp
    runtime/kernel/imports_ke.cpp
    runtime/kernel/imports_rtl.cpp
    runtime/kernel/imports_io.cpp
    runtime/kernel/imports_xam.cpp
    runtime/kernel/imports_net.cpp
    runtime/kernel/diagnostics.cpp
    runtime/gpu/null_gpu.cpp
    runtime/gpu/d3d11/renderer.cpp
    runtime/gpu/d3d11/xenos_backend.cpp
    runtime/gpu/d3d11/texture_cache.cpp
    runtime/audio/audio_driver.cpp
    runtime/audio/xma.cpp
    runtime/movie/movie_player.cpp)
target_include_directories(TenchuRuntime PUBLIC "${CMAKE_SOURCE_DIR}/runtime")
target_compile_options(TenchuRuntime PRIVATE /clang:-msse4.1 -Wno-unused-parameter /Z7)
target_link_libraries(TenchuRuntime PUBLIC TenchuPPC TenchuXenos bcrypt xinput winmm shell32 ole32 d3d11 dxgi d3dcompiler xaudio2 dbghelp)

# FFmpeg (XMA and WMV decoding), built by tools/build_xma_codec.py.
set(TENCHU_FFMPEG "${CMAKE_SOURCE_DIR}/build/deps/ffmpeg")
if(NOT EXISTS "${TENCHU_FFMPEG}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg is missing: run python tools/build_xma_codec.py")
endif()
target_include_directories(TenchuRuntime PUBLIC "${TENCHU_FFMPEG}/include")
target_link_libraries(TenchuRuntime PUBLIC
    "${TENCHU_FFMPEG}/lib/avcodec-tenchu.lib" "${TENCHU_FFMPEG}/lib/avutil-tenchu.lib"
    "${TENCHU_FFMPEG}/lib/avformat-tenchu.lib")
file(GLOB TENCHU_FFMPEG_DLLS "${TENCHU_FFMPEG}/bin/*.dll")

add_executable(TenchuZ app/main.cpp)
target_link_libraries(TenchuZ PRIVATE TenchuRuntime)
add_custom_command(TARGET TenchuZ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TENCHU_FFMPEG_DLLS} "$<TARGET_FILE_DIR:TenchuZ>")
# Translated code recurses deeply on the host stack.
target_link_options(TenchuZ PRIVATE /STACK:16777216 /DEBUG /OPT:REF /OPT:NOICF)

# Shader microcode tools.
add_library(TenchuXenos STATIC runtime/gpu/xenos/ucode.cpp runtime/gpu/xenos/translator.cpp)
target_include_directories(TenchuXenos PUBLIC "${CMAKE_SOURCE_DIR}/runtime")
add_executable(XenosDisasm tools/xenos_disasm/main.cpp)
target_link_libraries(XenosDisasm PRIVATE TenchuXenos d3dcompiler)
