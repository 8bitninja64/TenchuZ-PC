#pragma once
// User settings, read from TenchuZ.ini next to the executable (created with
// documented defaults on first run).
#include <cstdint>
#include <map>
#include <string>

namespace tz::host {

struct Settings {
    // Display
    bool fullscreen = false;      // borderless fullscreen on the current monitor
    int windowWidth = 1280;
    int windowHeight = 720;
    bool vsync = true;
    int resolutionScale = 1;      // internal resolution multiplier (1 = 720p, 2 = 1440p, 3 = 4K)
    int anisotropy = 16;          // texture filtering, 1 = off
    // Input
    float mouseSensitivity = 1.0f;
    bool invertMouseY = false;
    // Keyboard bindings: controller control -> key names ("w", "space", "mouse1", ...).
    std::map<std::string, std::string> keys;
};

const Settings& settings();

}  // namespace tz::host
