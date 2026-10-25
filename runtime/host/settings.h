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
    bool hdr = false;             // HDR10 output when Windows HDR is on for the monitor
    int hdrPeakNits = 0;          // brightest highlights (0 = the display's peak brightness)
    int hdrPaperWhiteNits = 0;    // mid tones and the menu (0 = Windows' SDR content brightness)
    int hdrHighlights = 100;      // highlight expansion of the 3D scene, percent (0 = none)
    // Graphics: effects the original does not have, on the 3D scene only (post_effects.h)
    bool ambientOcclusion = false;  // screen-space ambient occlusion
    int aoStrength = 100;         // percent
    int aoRadius = 80;            // game units (about centimeters)
    int sharpening = 0;           // contrast-adaptive sharpening, percent (0 = off)
    int msaa = 2;                 // samples of the title's 2x MSAA targets: 2, 4 or 8 (next start)
    bool stableBloom = true;      // bloom from the 720p footprint at resolution scales above 1
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

// Whether Windows HDR is on for the game window's monitor (the renderer updates it).
bool hdrAvailable();
void setHdrAvailable(bool available);
// What the HDR levels' "auto" (0) stands for on that monitor, in nits (the renderer updates it).
void setHdrAutoNits(int peak, int paperWhite);
int hdrAutoPeakNits();
int hdrAutoPaperWhiteNits();

}  // namespace tz::host
