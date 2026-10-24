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
    int frameRateLimit = 60;      // 30 = original pacing, 0 = unlimited
    bool showFps = false;         // frame rate counter in the top left corner
    // Game
    std::string disc;             // the disc image chosen on the first start (UTF-8)
    bool updateCheck = true;      // look for a newer release on GitHub at startup (F1 > Update)
    // Audio
    int volume = 100;             // master volume, percent
    bool mute = false;
    // Input
    float mouseSensitivity = 1.0f;
    bool invertMouseY = false;
    // Keyboard bindings: controller control -> key names ("w", "space", "mouse1", ...).
    std::map<std::string, std::string> keys;
    // Controller remapping: game control -> the physical button driving it (Xbox names).
    std::map<std::string, std::string> pad;
};

const Settings& settings();
// The same settings for the F1 menu (command processor thread), and writing them
// back to TenchuZ.ini: changed values replace their lines, comments are kept.
Settings& editableSettings();
void saveSettings();
// Restores the [keyboard] bindings of the default settings file.
void resetKeyboardDefaults();
// Restores the [controller] mapping of the default settings file (every button as itself).
void resetControllerDefaults();
std::string settingsFilePath();

// The frame rate limit in effect: frameRateLimit, except 30 while the front end (title
// screen and main menu, whose animations advance per frame) is showing.
int activeFrameRateLimit();
// Called by the front end's per-frame code.
void noteFrontEndFrame();

}  // namespace tz::host
