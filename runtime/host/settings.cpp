#include "host/settings.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tz::host {
namespace {

const char* kDefaultFile = R"(; Tenchu Z Recomp settings. Delete this file to restore the defaults.

[display]
; Borderless fullscreen (Alt+Enter or F11 toggles it while playing).
fullscreen = false
window_width = 1280
window_height = 720
vsync = true

[input]
mouse_sensitivity = 1.0
invert_mouse_y = false

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

bool parseBool(const std::string& value) { return value == "true" || value == "1" || value == "yes" || value == "on"; }

void apply(Settings& settings, const std::string& section, const std::string& key, const std::string& value) {
    try {
        if (section == "display") {
            if (key == "fullscreen") settings.fullscreen = parseBool(value);
            else if (key == "window_width") settings.windowWidth = (std::max)(320, std::stoi(value));
            else if (key == "window_height") settings.windowHeight = (std::max)(180, std::stoi(value));
            else if (key == "vsync") settings.vsync = parseBool(value);
        } else if (section == "input") {
            if (key == "mouse_sensitivity") settings.mouseSensitivity = std::stof(value);
            else if (key == "invert_mouse_y") settings.invertMouseY = parseBool(value);
        } else if (section == "keyboard") {
            settings.keys[key] = value;
        }
    } catch (...) {
        // Malformed values keep their defaults.
    }
}

void parse(Settings& settings, std::istream& input) {
    std::string line, section;
    while (std::getline(input, line)) {
        line = trim(line.substr(0, line.find(';')));
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

}  // namespace

const Settings& settings() {
    static const Settings instance = [] {
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
        return result;
    }();
    return instance;
}

}  // namespace tz::host
