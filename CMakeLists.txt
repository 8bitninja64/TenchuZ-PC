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
if(NOT EXISTS "${TENCHU_SIMDE_DIR}/x86/avx.h")
    message(FATAL_ERROR "simde is missing at ${TENCHU_SIMDE_DIR}: run tools/build.ps1 (or python tools/setup_generator.py)")
endif()
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
add_compile_options(/EHsc)

# Translated game code: the game module (tenchu_game.dll, runtime/guest/module_abi.h),
# built when tools/recompile.py has generated its sources. Players build it on first start
# with tools/make_module.ps1, which must use the same flags.
if(EXISTS "${TENCHU_GENERATED_DIR}/sources.cmake")
    include("${TENCHU_GENERATED_DIR}/sources.cmake")
    add_library(tenchu_game SHARED ${TENCHU_PPC_SOURCES} runtime/guest/module_rt.cpp)
    target_include_directories(tenchu_game PRIVATE "${TENCHU_GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/runtime/guest"
                               "${TENCHU_SIMDE_DIR}")
    # Debugging: per-function entry counts (control commands countstart/countstop). Costs a
    # test in every guest call, so it is off by default.
    option(TENCHU_ENTRY_COUNTS "Count guest function entries (debugging)" OFF)
    if(TENCHU_ENTRY_COUNTS)
        target_compile_definitions(tenchu_game PRIVATE TENCHU_ENTRY_COUNTS)
    endif()
    # Exact floating point; SSSE3 byte shuffles are exact and fast.
    target_compile_options(tenchu_game PRIVATE /fp:strict /clang:-mssse3 /clang:-msse4.1 -Wno-everything /Z7)
    target_link_options(tenchu_game PRIVATE /DEBUG /OPT:REF /OPT:NOICF)
endif()

# Kernel, devices and host integration. An object library: the host functions register
# themselves by name (runtime/guest/host.h), so no object may be dropped.
add_library(TenchuRuntime OBJECT
    runtime/kernel/memory.cpp
    runtime/kernel/xex.cpp
    runtime/kernel/kernel.cpp
    runtime/kernel/control.cpp
    runtime/kernel/host_profiler.cpp
    runtime/host/settings.cpp
    runtime/host/input.cpp
    runtime/host/gamepad.cpp
    runtime/host/overlay.cpp
    runtime/kernel/kernel_data.cpp
    runtime/kernel/format.cpp
    runtime/kernel/imports_ke.cpp
    runtime/kernel/imports_rtl.cpp
    runtime/kernel/imports_io.cpp
    runtime/kernel/imports_xam.cpp
    runtime/kernel/imports_net.cpp
    runtime/kernel/diagnostics.cpp
    runtime/kernel/native_overrides.cpp
    runtime/kernel/mmio_fault.cpp
    runtime/kernel/host_registry.cpp
    runtime/kernel/module_loader.cpp
    runtime/gpu/null_gpu.cpp
    runtime/gpu/d3d11/renderer.cpp
    runtime/gpu/d3d11/xenos_backend.cpp
    runtime/gpu/d3d11/texture_cache.cpp
    runtime/audio/audio_driver.cpp
    runtime/audio/xma.cpp
    runtime/movie/movie_player.cpp)
target_include_directories(TenchuRuntime PUBLIC "${CMAKE_SOURCE_DIR}/runtime" "${TENCHU_SIMDE_DIR}")
target_compile_options(TenchuRuntime PRIVATE /clang:-msse4.1 -Wno-unused-parameter /Z7)
target_link_libraries(TenchuRuntime PUBLIC TenchuXenos bcrypt xinput winmm shell32 ole32 d3d11 dxgi d3dcompiler xaudio2 dbghelp)

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

# Dear ImGui (F1 menu), fetched by tools/fetch_imgui.py.
set(TENCHU_IMGUI "${CMAKE_SOURCE_DIR}/build/deps/imgui")
if(NOT EXISTS "${TENCHU_IMGUI}/imgui.h")
    message(FATAL_ERROR "Dear ImGui is missing: run python tools/fetch_imgui.py")
endif()
add_library(TenchuImGui STATIC
    ${TENCHU_IMGUI}/imgui.cpp ${TENCHU_IMGUI}/imgui_draw.cpp ${TENCHU_IMGUI}/imgui_tables.cpp
    ${TENCHU_IMGUI}/imgui_widgets.cpp ${TENCHU_IMGUI}/backends/imgui_impl_win32.cpp
    ${TENCHU_IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(TenchuImGui PUBLIC "${TENCHU_IMGUI}")
# The menu reads controllers itself (SDL); the Win32 backend's XInput polling stays out.
target_compile_definitions(TenchuImGui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_compile_options(TenchuImGui PRIVATE -Wno-everything)
target_link_libraries(TenchuRuntime PUBLIC TenchuImGui)

# SDL3 (game controllers), fetched by tools/fetch_sdl.py.
set(TENCHU_SDL "${CMAKE_SOURCE_DIR}/build/deps/SDL3")
if(NOT EXISTS "${TENCHU_SDL}/include/SDL3/SDL.h")
    message(FATAL_ERROR "SDL3 is missing: run python tools/fetch_sdl.py")
endif()
target_include_directories(TenchuRuntime PUBLIC "${TENCHU_SDL}/include")
target_link_libraries(TenchuRuntime PUBLIC "${TENCHU_SDL}/lib/x64/SDL3.lib")
list(APPEND TENCHU_FFMPEG_DLLS "${TENCHU_SDL}/lib/x64/SDL3.dll" "${CMAKE_SOURCE_DIR}/assets/gamecontrollerdb.txt")

add_executable(TenchuZ app/main.cpp)
target_link_libraries(TenchuZ PRIVATE TenchuRuntime)
if(TARGET tenchu_game)
    add_dependencies(TenchuZ tenchu_game)
endif()
add_custom_command(TARGET TenchuZ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TENCHU_FFMPEG_DLLS} "$<TARGET_FILE_DIR:TenchuZ>")
# Translated code recurses deeply on the host stack.
target_link_options(TenchuZ PRIVATE /STACK:16777216 /DEBUG /OPT:REF /OPT:NOICF)

# Shader microcode tools.
add_library(TenchuXenos STATIC runtime/gpu/xenos/ucode.cpp runtime/gpu/xenos/translator.cpp)
target_include_directories(TenchuXenos PUBLIC "${CMAKE_SOURCE_DIR}/runtime")
add_executable(XenosDisasm tools/xenos_disasm/main.cpp)
target_link_libraries(XenosDisasm PRIVATE TenchuXenos d3dcompiler)
