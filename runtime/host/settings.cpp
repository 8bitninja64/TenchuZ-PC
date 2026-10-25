#include "host/settings.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>
#include <vector>

namespace tz::host {
namespace {

const char* kDefaultFile = R"(; Tenchu Z Recomp settings. Delete this file to restore the defaults.

[game]
; Your Tenchu Z (USA, Europe) disc image, chosen on the first start.
disc =
; Look for a newer release on GitHub when the game starts (F1 > Update installs it).
update_check = true

[display]
; Borderless fullscreen (Alt+Enter or F11 toggles it while playing).
fullscreen = false
window_width = 1280
window_height = 720
vsync = true
; Internal rendering resolution as a multiple of the original 1280x720
; (1 = 720p, 2 = 1440p, 3 = 4K). Higher values need much more video memory.
resolution_scale = 1
; Anisotropic texture filtering: 1 (off), 2, 4, 8 or 16.
anisotropic_filtering = 16
; Frame rate limit. 30 is the original console pacing; higher values (60, 120,
; 144, 240, ...) or 0 for unlimited unlock it. Game speed does not change.
frame_rate_limit = 60
; Frame rate counter in the top left corner (also in the F1 menu).
show_fps = false
; HDR10 output on monitors with Windows HDR turned on: highlights are expanded up to
; hdr_peak_nits (0 = the display's own peak); mid tones and the F1 menu sit at
; hdr_paper_white_nits (0 = Windows' "SDR content brightness").
hdr = false
hdr_peak_nits = 0
hdr_paper_white_nits = 0
; How strongly highlights of the 3D scene are expanded, in percent (0 = none: the SDR
; look at paper white; above 100 brightens mid tones too). The HUD stays at paper white.
hdr_highlights = 100

[graphics]
; Ambient occlusion: soft shadows in corners, under eaves and around objects standing on
; the ground, from the depth of the 3D scene (the HUD is not affected). Strength in
; percent, radius in game units (about centimeters).
ambient_occlusion = false
ao_strength = 100
ao_radius = 80
; Contrast-adaptive sharpening of the 3D scene in percent (0 = off).
sharpening = 0
; At resolution_scale 2 and above, bloom samples the scene averaged to 720p like the
; original, instead of sparkling on small highlights.
stable_bloom = true
; Multisample anti-aliasing of the 3D scene: 2 (the original), 4 or 8 samples. Applied
; at the next start; more samples need more video memory (at most 4 from resolution_scale 3).
msaa = 2

[audio]
; Master volume in percent, and mute.
volume = 100
mute = false

[input]
mouse_sensitivity = 1.0
invert_mouse_y = false

; Controller buttons: each game control (left) is driven by the controller button on
; the right, named by position on an Xbox pad: a, b, x, y, lb, rb, lt, rt, start, back,
; ls, rs (stick clicks), dpad_up, dpad_down, dpad_left, dpad_right. The F1 menu edits them.
[controller]
a = a
b = b
x = x
y = y
lb = lb
rb = rb
lt = lt
rt = rt
start = start
back = back
ls = ls
rs = rs
dpad_up = dpad_up
dpad_down = dpad_down
dpad_left = dpad_left
dpad_right = dpad_right

; Keyboard and mouse bindings for controller 1. Keys: a-z, 0-9, space, enter,
; escape, tab, shift, ctrl, alt, up, down, left, right, f1-f12, mouse1-mouse5.
; Several keys can be listed separated by spaces. The mouse moves the camera
; (right stick); click the window to capture the cursor, Escape releases it.
[keyboard]
move_up = w
move_down = s
move_left = a
move_right = d
walk = alt
a = space
b = c
x = mouse1
y = mouse2
lb = q
rb = e
lt = f
rt = r
start = enter p
back = tab
dpad_up = up 1
dpad_down = down 2
dpad_left = left 3
dpad_right = right 4
ls = z
rs = mouse3 v
)";

std::filesystem::path settingsPath() {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    return std::filesystem::path(executable).parent_path() / "TenchuZ.ini";
}

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// A setting line without its comment: comments are whole lines starting with ';' or '#'
// (values such as paths may contain ';').
std::string content(const std::string& line) {
    const std::string text = trim(line);
    return !text.empty() && (text.front() == ';' || text.front() == '#') ? std::string() : text;
}

bool parseBool(const std::string& value) { return value == "true" || value == "1" || value == "yes" || value == "on"; }

// A value where 0 (or less) means automatic, else clamped to [low, high].
int automatic(int value, int low, int high) { return value <= 0 ? 0 : std::clamp(value, low, high); }

void apply(Settings& settings, const std::string& section, const std::string& key, const std::string& value) {
    try {
        if (section == "game") {
            if (key == "disc") settings.disc = value;
            else if (key == "update_check") settings.updateCheck = parseBool(value);
        } else if (section == "display") {
            if (key == "fullscreen") settings.fullscreen = parseBool(value);
            else if (key == "window_width") settings.windowWidth = (std::max)(320, std::stoi(value));
            else if (key == "window_height") settings.windowHeight = (std::max)(180, std::stoi(value));
            else if (key == "vsync") settings.vsync = parseBool(value);
            else if (key == "resolution_scale") settings.resolutionScale = std::clamp(std::stoi(value), 1, 4);
            else if (key == "anisotropic_filtering") settings.anisotropy = std::clamp(std::stoi(value), 1, 16);
            else if (key == "frame_rate_limit") settings.frameRateLimit = (std::max)(0, std::stoi(value));
            else if (key == "show_fps") settings.showFps = parseBool(value);
            else if (key == "hdr") settings.hdr = parseBool(value);
            else if (key == "hdr_peak_nits") settings.hdrPeakNits = automatic(std::stoi(value), 80, 10000);
            else if (key == "hdr_paper_white_nits") settings.hdrPaperWhiteNits = automatic(std::stoi(value), 80, 1000);
            else if (key == "hdr_highlights") settings.hdrHighlights = std::clamp(std::stoi(value), 0, 200);
        } else if (section == "graphics") {
            if (key == "ambient_occlusion") settings.ambientOcclusion = parseBool(value);
            else if (key == "ao_strength") settings.aoStrength = std::clamp(std::stoi(value), 0, 200);
            else if (key == "ao_radius") settings.aoRadius = std::clamp(std::stoi(value), 10, 400);
            else if (key == "sharpening") settings.sharpening = std::clamp(std::stoi(value), 0, 100);
            else if (key == "stable_bloom") settings.stableBloom = parseBool(value);
            else if (key == "msaa") {
                const int samples = std::stoi(value);
                settings.msaa = samples >= 8 ? 8 : samples >= 4 ? 4 : 2;
            }
        } else if (section == "audio") {
            if (key == "volume") settings.volume = std::clamp(std::stoi(value), 0, 100);
            else if (key == "mute") settings.mute = parseBool(value);
        } else if (section == "input") {
            if (key == "mouse_sensitivity") settings.mouseSensitivity = std::stof(value);
            else if (key == "invert_mouse_y") settings.invertMouseY = parseBool(value);
        } else if (section == "keyboard") {
            settings.keys[key] = value;
        } else if (section == "controller") {
            settings.pad[key] = value;
        }
    } catch (...) {
        // Malformed values keep their defaults.
    }
}

void parse(Settings& settings, std::istream& input) {
    std::string line, section;
    while (std::getline(input, line)) {
        line = content(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = trim(line.substr(0, equals));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        apply(settings, section, key, trim(line.substr(equals + 1)));
    }
}


Settings& instance() {
    static Settings instance = [] {
        Settings result;
        std::istringstream defaults(kDefaultFile);
        parse(result, defaults);
        const auto path = settingsPath();
        if (std::ifstream file{path}) {
            parse(result, file);
        } else {
            std::ofstream out(path, std::ios::binary);
            out << kDefaultFile;
        }
        // TENCHU_RESOLUTION_SCALE overrides the file (testing).
        char value[16]{};
        if (GetEnvironmentVariableA("TENCHU_RESOLUTION_SCALE", value, sizeof(value)))
            apply(result, "display", "resolution_scale", value);
        if (GetEnvironmentVariableA("TENCHU_FRAME_RATE", value, sizeof(value)))
            apply(result, "display", "frame_rate_limit", value);
        return result;
    }();
    return instance;
}

std::string text(bool value) { return value ? "true" : "false"; }

}  // namespace

const Settings& settings() { return instance(); }
Settings& editableSettings() { return instance(); }
std::string settingsFilePath() {
    const std::u8string text = settingsPath().u8string();
    return std::string(text.begin(), text.end());
}

namespace {
std::atomic<int64_t> frontEndFrameAt{INT64_MIN / 2};  // steady clock, ms
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

void noteFrontEndFrame() { frontEndFrameAt.store(nowMs(), std::memory_order_relaxed); }

namespace {
std::atomic<bool> hdrDisplay{false};
std::atomic<int> hdrAutoPeak{1000}, hdrAutoPaperWhite{200};
}
bool hdrAvailable() { return hdrDisplay.load(std::memory_order_relaxed); }
void setHdrAvailable(bool available) { hdrDisplay.store(available, std::memory_order_relaxed); }
void setHdrAutoNits(int peak, int paperWhite) {
    hdrAutoPeak.store(peak, std::memory_order_relaxed);
    hdrAutoPaperWhite.store(paperWhite, std::memory_order_relaxed);
}
int hdrAutoPeakNits() { return hdrAutoPeak.load(std::memory_order_relaxed); }
int hdrAutoPaperWhiteNits() { return hdrAutoPaperWhite.load(std::memory_order_relaxed); }

int activeFrameRateLimit() {
    const int limit = settings().frameRateLimit;
    // TENCHU_FRONTEND_UNCAPPED: no cap (testing).
    static const bool uncapped = GetEnvironmentVariableA("TENCHU_FRONTEND_UNCAPPED", nullptr, 0) != 0;
    if ((limit <= 0 || limit > 30) && !uncapped && nowMs() - frontEndFrameAt.load(std::memory_order_relaxed) < 250)
        return 30;
    return limit;
}

void resetKeyboardDefaults() {
    Settings defaults;
    std::istringstream file(kDefaultFile);
    parse(defaults, file);
    instance().keys = defaults.keys;
}

void resetControllerDefaults() {
    Settings defaults;
    std::istringstream file(kDefaultFile);
    parse(defaults, file);
    instance().pad = defaults.pad;
}

void saveSettings() {
    const Settings& s = instance();
    std::vector<std::tuple<std::string, std::string, std::string>> values = {
        {"game", "disc", s.disc},
        {"game", "update_check", text(s.updateCheck)},
        {"display", "fullscreen", text(s.fullscreen)},
        {"display", "vsync", text(s.vsync)},
        {"display", "resolution_scale", std::to_string(s.resolutionScale)},
        {"display", "anisotropic_filtering", std::to_string(s.anisotropy)},
        {"display", "frame_rate_limit", std::to_string(s.frameRateLimit)},
        {"display", "show_fps", text(s.showFps)},
        {"display", "hdr", text(s.hdr)},
        {"display", "hdr_peak_nits", std::to_string(s.hdrPeakNits)},
        {"display", "hdr_paper_white_nits", std::to_string(s.hdrPaperWhiteNits)},
        {"display", "hdr_highlights", std::to_string(s.hdrHighlights)},
        {"graphics", "ambient_occlusion", text(s.ambientOcclusion)},
        {"graphics", "ao_strength", std::to_string(s.aoStrength)},
        {"graphics", "ao_radius", std::to_string(s.aoRadius)},
        {"graphics", "sharpening", std::to_string(s.sharpening)},
        {"graphics", "stable_bloom", text(s.stableBloom)},
        {"graphics", "msaa", std::to_string(s.msaa)},
        {"audio", "volume", std::to_string(s.volume)},
        {"audio", "mute", text(s.mute)},
        {"input", "mouse_sensitivity", std::to_string(s.mouseSensitivity)},
        {"input", "invert_mouse_y", text(s.invertMouseY)},
    };
    for (const auto& [key, value] : s.keys) values.emplace_back("keyboard", key, value);
    for (const auto& [key, value] : s.pad) values.emplace_back("controller", key, value);

    std::vector<std::string> lines;
    if (std::ifstream file{settingsPath()}) {
        for (std::string line; std::getline(file, line);) lines.push_back(line);
    }
    std::vector<bool> written(values.size());
    std::string section;
    std::map<std::string, size_t> sectionEnd;  // line after a section's last setting
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string line = content(lines[i]);
        if (line.size() > 1 && line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            sectionEnd[section] = i + 1;
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        sectionEnd[section] = i + 1;
        std::string key = trim(line.substr(0, equals));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (size_t v = 0; v < values.size(); ++v)
            if (!written[v] && std::get<0>(values[v]) == section && std::get<1>(values[v]) == key) {
                lines[i] = key + " = " + std::get<2>(values[v]);
                written[v] = true;
            }
    }
    // Settings the file did not have yet go at the end of their section.
    for (size_t v = values.size(); v-- > 0;) {
        if (written[v]) continue;
        const auto& [sectionName, key, value] = values[v];
        auto it = sectionEnd.find(sectionName);
        if (it == sectionEnd.end()) {
            lines.push_back("");
            lines.push_back("[" + sectionName + "]");
            it = sectionEnd.emplace(sectionName, lines.size()).first;
        }
        lines.insert(lines.begin() + std::ptrdiff_t(it->second), key + " = " + value);
        for (auto& [name, end] : sectionEnd)
            if (end >= it->second && name != sectionName) ++end;
        ++it->second;
    }
    std::ofstream out(settingsPath(), std::ios::binary);
    for (const auto& line : lines) out << line << '\n';
}

}  // namespace tz::host
