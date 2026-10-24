#pragma once
// First start and every start: choose the game (a disc image or an extracted folder),
// check that it is the supported Tenchu Z disc, and load a game module translated from
// it, building one from the disc when there is none (tools/make_module.ps1), as the NFL
// Blitz 2003 port does.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace tz {

class DiscImage;

struct GameSource {
    std::shared_ptr<DiscImage> disc;  // a disc image read in place, or
    std::filesystem::path folder;     // an extracted disc (development)
    std::vector<uint8_t> xex;         // default.xex
};

// Returns false when the player cancelled or the game cannot start (already reported).
bool prepareGame(int argc, wchar_t** argv, GameSource& source);

}  // namespace tz
