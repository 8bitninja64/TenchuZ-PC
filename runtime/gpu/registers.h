#pragma once
// Xenos register indices (dword offsets; MMIO address = 0x7FC80000 + index * 4).
#include <cstdint>

namespace tz::gpu::reg {

// Render backend surfaces
constexpr uint32_t RB_SURFACE_INFO = 0x2000;   // pitch 0-13, msaa samples 16-17
constexpr uint32_t RB_COLOR_INFO = 0x2001;     // edram base 0-11, format 16-19
constexpr uint32_t RB_DEPTH_INFO = 0x2002;     // edram base 0-11, format 16 (0 D24S8, 1 D24FS8)
constexpr uint32_t RB_COLOR1_INFO = 0x2003;
constexpr uint32_t RB_COLOR2_INFO = 0x2004;
constexpr uint32_t RB_COLOR3_INFO = 0x2005;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200E;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR = 0x200F;
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080;     // x 0-14, y 16-30 (signed)
constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL = 0x2081;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR = 0x2082;

// Vertex grouper
constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100;
constexpr uint32_t VGT_MIN_VTX_INDX = 0x2101;
constexpr uint32_t VGT_INDX_OFFSET = 0x2102;
constexpr uint32_t VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103;

constexpr uint32_t RB_COLOR_MASK = 0x2104;           // 4 bits per render target
constexpr uint32_t RB_BLEND_RED = 0x2105;            // .. RB_BLEND_ALPHA 0x2108 (floats)
constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C;
constexpr uint32_t RB_STENCILREFMASK = 0x210D;       // ref 0-7, mask 8-15, write mask 16-23
constexpr uint32_t RB_ALPHA_REF = 0x210E;            // float
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;      // then XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET

constexpr uint32_t PA_CL_UCP_0_X = 0x2388;           // six user clip planes, xyzw floats each

// Sequencer
constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180;
constexpr uint32_t SQ_CONTEXT_MISC = 0x2181;
constexpr uint32_t SQ_INTERPOLATOR_CNTL = 0x2182;

constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;
constexpr uint32_t RB_COLORCONTROL = 0x2202;         // alpha func 0-2, alpha test enable 3
constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204;         // user planes 0-5, clip disable 16, UCP cull only 17
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;      // cull front 0, cull back 1, face 2, window offset enable 16
constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;
constexpr uint32_t RB_MODECONTROL = 0x2208;          // edram mode 0-2: 4 color+depth, 5 depth only, 6 copy
constexpr uint32_t RB_BLENDCONTROL1 = 0x2209;
constexpr uint32_t RB_BLENDCONTROL2 = 0x220A;
constexpr uint32_t RB_BLENDCONTROL3 = 0x220B;

constexpr uint32_t PA_SU_POINT_SIZE = 0x2280;
constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;          // pixel center 0 (0 = D3D, 1 = OpenGL)
constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380;

// Resolve (EDRAM to memory copies)
constexpr uint32_t RB_COPY_CONTROL = 0x2318;         // source 0-2, sample 4-6, color clear 8, depth clear 9, command 20-21
constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;      // pitch 0-13, height 16-29
constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;       // endian 0-2, format 7-12
constexpr uint32_t RB_DEPTH_CLEAR = 0x231D;
constexpr uint32_t RB_COLOR_CLEAR = 0x231E;
constexpr uint32_t RB_COLOR_CLEAR_LO = 0x231F;

// Constants
constexpr uint32_t SHADER_CONSTANT_000_X = 0x4000;   // 512 float4: vertex 0-255, pixel 256-511
constexpr uint32_t SHADER_CONSTANT_FETCH_00_0 = 0x4800;  // 32 texture x 6 dwords = 96 vertex x 2 dwords
constexpr uint32_t SHADER_CONSTANT_BOOL_000_031 = 0x4900;
constexpr uint32_t SHADER_CONSTANT_LOOP_00 = 0x4908;

}  // namespace tz::gpu::reg
