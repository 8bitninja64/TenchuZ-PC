#pragma once
// Live control of a running game for automated testing.
// TENCHU_CONTROL=<file>: lines appended to the file are executed in order:
//   press <buttons> [ms]   tap buttons ('+'-separated names, default 150 ms)
//   hold <buttons>         keep buttons down until release
//   release                let go of everything (buttons and sticks)
//   stick <lx> <ly> [<rx> <ry>]   stick positions in -1..1
//   wait <ms>              delay the following commands
//   shot <file.bmp>        save the next presented frame
//   traceframe             log every draw of the next frame (like TENCHU_TRACE_FRAME)
//   dumpframe              save the next frame's resolved textures to build/resolved
//   profile                start the host profiler window (TENCHU_TRACE=hostprof)
//   quit                   exit the process
#include <cstdint>
#include <string>

namespace tz::control {

struct PadState {
    uint16_t buttons = 0;
    int16_t thumbs[4] = {};  // LX, LY, RX, RY
};

// True when TENCHU_CONTROL is set; the pad state then replaces controller 0.
bool active();
PadState pad();
// Returns the path of a pending screenshot request and clears it.
std::string takeScreenshotRequest();
// True once after a command of that name (traceframe, dumpframe).
bool takeFlag(const std::string& name);

}  // namespace tz::control
