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
    runtime/kernel/kernel_data.cpp
    runtime/kernel/format.cpp
    runtime/kernel/imports_ke.cpp
    runtime/kernel/imports_rtl.cpp
    runtime/kernel/imports_io.cpp
    runtime/kernel/imports_xam.cpp
    runtime/kernel/imports_net.cpp
    runtime/gpu/null_gpu.cpp
    runtime/audio/null_audio.cpp)
target_include_directories(TenchuRuntime PUBLIC "${CMAKE_SOURCE_DIR}/runtime")
target_compile_options(TenchuRuntime PRIVATE /clang:-msse4.1 -Wno-unused-parameter)
target_link_libraries(TenchuRuntime PUBLIC TenchuPPC bcrypt xinput winmm shell32 ole32)

add_executable(TenchuZ app/main.cpp)
target_link_libraries(TenchuZ PRIVATE TenchuRuntime)
# Translated code recurses deeply on the host stack.
target_link_options(TenchuZ PRIVATE /STACK:16777216 /DEBUG)
