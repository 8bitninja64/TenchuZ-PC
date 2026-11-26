#pragma once
// Waiting for the emulated GPU without spinning. The title's D3D library waits for the GPU
// (fences, command buffer space) in a busy loop, as a hardware thread of the Xbox 360 can
// afford; on a PC it takes a whole core from the command processor and the graphics driver
// it is waiting for (and power from an integrated GPU). The command processor counts what
// a waiter may be waiting for (read pointer, memory and scratch writes, interrupts, vblank);
// a waiter sleeps until the count changes (kernel/native_overrides.cpp, sub_82425790).
#include <cstdint>

namespace tz::gpu {

uint32_t progress();
// Command processor and vblank threads: something a waiter may check changed.
void noteProgress();
// Sleeps until progress() differs from `seen` or about `microseconds` pass.
void waitProgress(uint32_t seen, uint32_t microseconds);

}  // namespace tz::gpu
