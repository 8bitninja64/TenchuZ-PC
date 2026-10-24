#pragma once
// Live control of a running game for automated testing.
// TENCHU_CONTROL=<file>: lines appended to the file are executed in order:
//   press <buttons> [ms]   tap buttons ('+'-separated names incl. lt/rt, default 150 ms)
//   hold <buttons>         keep buttons down until release
//   release                let go of everything (buttons and sticks)
//   stick <lx> <ly> [<rx> <ry>]   stick positions in -1..1
//   wait <ms>              delay the following commands
//   shot <file.bmp>        save the next presented frame
//   burst <count> <dir>    save the next count frames as dir/burst_NNNNN.bmp
//   traceframe             log every draw of the next frame (like TENCHU_TRACE_FRAME)
//   dumpframe              save the next frame's resolved textures to build/resolved
//   profile                start the host profiler window (TENCHU_TRACE=hostprof)
//   peek [*]addr[+off] [n] log n guest words at addr ('*': through the pointer at addr)
//                          (or a pointer chain addr>off>off: each '>' loads, then adds off)
//   list <head> <next> [n] walk a linked list from *head (next pointer at +next), n words each
//   findf <f> [<f>...]     log guest addresses holding these consecutive floats
//   countstart / countstop <file>  count guest function entries in between, write them (builds
//                          configured with -DTENCHU_ENTRY_COUNTS=ON)
//   watchw <addr>          hardware watchpoint: log the guest code writing those 4 bytes
//   clock                  log host milliseconds (timing measurements)
//   quit                   exit the process
#include <cstdint>
#include <string>

namespace tz::control {

struct PadState {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
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
