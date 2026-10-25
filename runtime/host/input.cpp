#include "host/input.h"
#include "host/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include "platform/window_sdl.h"
#endif

namespace tz::host {
namespace {

using Clock = std::chrono::steady_clock;

// Virtual key codes; mouse buttons use 0x01-0x06 like Windows (VK_LBUTTON...).
int keyCode(const std::string& name) {
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        return std::toupper(static_cast<unsigned char>(name[0]));
    static const std::pair<const char*, int> names[] = {
        {"space", VK_SPACE}, {"enter", VK_RETURN}, {"escape", VK_ESCAPE}, {"tab", VK_TAB},
        {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"up", VK_UP}, {"down", VK_DOWN},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"backspace", VK_BACK}, {"mouse1", VK_LBUTTON},
        {"mouse2", VK_RBUTTON}, {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1}, {"mouse5", VK_XBUTTON2}};
    for (const auto& [label, code] : names)
        if (name == label) return code;
    if (name.size() >= 2 && name[0] == 'f') {
        const int number = std::atoi(name.c_str() + 1);
        if (number >= 1 && number <= 12) return VK_F1 + number - 1;
    }
    return 0;
}

enum Control {
    MoveUp, MoveDown, MoveLeft, MoveRight, Walk, ButtonA, ButtonB, ButtonX, ButtonY, ShoulderL, ShoulderR,
    TriggerL, TriggerR, Start, Back, DpadUp, DpadDown, DpadLeft, DpadRight, ThumbL, ThumbR, ControlCount
};
const char* kControlNames[ControlCount] = {"move_up", "move_down", "move_left", "move_right", "walk", "a", "b", "x",
                                           "y", "lb", "rb", "lt", "rt", "start", "back", "dpad_up", "dpad_down",
                                           "dpad_left", "dpad_right", "ls", "rs"};

struct State {
    std::mutex mutex;
    std::array<bool, 256> keys{};
    std::array<std::vector<int>, ControlCount> bindings;
    double mouseX = 0, mouseY = 0;  // accumulated raw motion since the last poll
    Clock::time_point lastPoll = Clock::now();
    float stickX = 0, stickY = 0;   // smoothed camera stick
    bool captured = false;
    std::atomic<bool> active{false};
    HWND window = nullptr;
};

void loadBindings(State& s) {
    for (int control = 0; control < ControlCount; ++control) {
        s.bindings[control].clear();
        const auto it = settings().keys.find(kControlNames[control]);
        if (it == settings().keys.end()) continue;
        std::istringstream names(it->second);
        std::string name;
        while (names >> name)
            if (const int code = keyCode(name)) s.bindings[control].push_back(code);
    }
}

State& state() {
    static State* instance = [] {
        auto* s = new State;
        loadBindings(*s);
        return s;
    }();
    return *instance;
}

bool down(const State& s, Control control) {
    for (int code : s.bindings[control])
        if (s.keys[code & 0xFF]) return true;
    return false;
}

void setCapture(State& s, bool capture) {
    if (s.captured == capture) return;
    s.captured = capture;
#ifndef _WIN32
    SDL_SetWindowRelativeMouseMode(static_cast<SDL_Window*>(s.window), capture);
#else
    if (capture) {
        RECT client;
        GetClientRect(s.window, &client);
        MapWindowPoints(s.window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        ClipCursor(&client);
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
#endif
}

int16_t axis(float value) { return int16_t(std::clamp(value, -1.0f, 1.0f) * 32767.0f); }

}  // namespace

void initializeInput(HWND window) {
    auto& s = state();
    s.window = window;
#ifdef _WIN32
    RAWINPUTDEVICE device{0x01, 0x02, 0, window};  // generic desktop, mouse
    RegisterRawInputDevices(&device, 1, sizeof(device));
#endif
}

bool handleInputMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam == VK_ESCAPE && s.captured) {
            setCapture(s, false);
            return true;
        }
        s.keys[wParam & 0xFF] = true;
        s.active = true;
        return message == WM_KEYDOWN;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        s.keys[wParam & 0xFF] = false;
        return message == WM_KEYUP;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
        if (!s.captured) {
            // The first click only captures the cursor.
            setCapture(s, true);
            s.active = true;
            return true;
        }
        const int code = message == WM_LBUTTONDOWN ? VK_LBUTTON : message == WM_RBUTTONDOWN ? VK_RBUTTON
                       : message == WM_MBUTTONDOWN ? VK_MBUTTON
                       : (GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
        s.keys[code] = true;
        return true;
    }
    case WM_LBUTTONUP: s.keys[VK_LBUTTON] = false; return true;
    case WM_RBUTTONUP: s.keys[VK_RBUTTON] = false; return true;
    case WM_MBUTTONUP: s.keys[VK_MBUTTON] = false; return true;
    case WM_XBUTTONUP: s.keys[GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2] = false; return true;
    case WM_INPUT: {
#ifndef _WIN32
        if (const SDL_Event* event = platform::currentEvent(); event && event->type == SDL_EVENT_MOUSE_MOTION && s.captured) {
            s.mouseX += event->motion.xrel;
            s.mouseY += event->motion.yrel;
        }
        return false;
#else
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                UINT(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && s.captured) {
            s.mouseX += raw.data.mouse.lLastX;
            s.mouseY += raw.data.mouse.lLastY;
        }
        return false;
#endif
    }
    case WM_KILLFOCUS:
        setCapture(s, false);
        s.keys.fill(false);
        return false;
    case WM_SIZE:
    case WM_MOVE:
        if (s.captured) {
            setCapture(s, false);
            setCapture(s, true);
        }
        return false;
    default:
        return false;
    }
}

bool keyboardMouseActive() { return state().active; }

void reloadKeyBindings() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    loadBindings(s);
}

void releaseMouse() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    setCapture(s, false);
    s.keys.fill(false);
}

std::string keyName(int code) {
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::string(1, char(std::tolower(code)));
    static const std::pair<const char*, int> names[] = {
        {"space", VK_SPACE}, {"enter", VK_RETURN}, {"escape", VK_ESCAPE}, {"tab", VK_TAB},
        {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL}, {"alt", VK_MENU}, {"up", VK_UP}, {"down", VK_DOWN},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"backspace", VK_BACK}, {"mouse1", VK_LBUTTON},
        {"mouse2", VK_RBUTTON}, {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1}, {"mouse5", VK_XBUTTON2}};
    for (const auto& [label, value] : names)
        if (code == value) return label;
    if (code >= VK_F1 && code <= VK_F12) return "f" + std::to_string(code - VK_F1 + 1);
    // Left/right variants report as the generic modifier.
    if (code == VK_LSHIFT || code == VK_RSHIFT) return "shift";
    if (code == VK_LCONTROL || code == VK_RCONTROL) return "ctrl";
    if (code == VK_LMENU || code == VK_RMENU) return "alt";
    return {};
}

const char* const* keyboardControls(int& count) {
    count = ControlCount;
    return kControlNames;
}

VirtualPad keyboardMouseState() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    VirtualPad pad;
    static const std::pair<Control, uint16_t> buttons[] = {
        {DpadUp, 0x0001}, {DpadDown, 0x0002}, {DpadLeft, 0x0004}, {DpadRight, 0x0008}, {Start, 0x0010},
        {Back, 0x0020}, {ThumbL, 0x0040}, {ThumbR, 0x0080}, {ShoulderL, 0x0100}, {ShoulderR, 0x0200},
        {ButtonA, 0x1000}, {ButtonB, 0x2000}, {ButtonX, 0x4000}, {ButtonY, 0x8000}};
    for (const auto& [control, mask] : buttons)
        if (down(s, control)) pad.buttons |= mask;
    pad.leftTrigger = down(s, TriggerL) ? 255 : 0;
    pad.rightTrigger = down(s, TriggerR) ? 255 : 0;

    float x = float(down(s, MoveRight)) - float(down(s, MoveLeft));
    float y = float(down(s, MoveUp)) - float(down(s, MoveDown));
    if (x && y) {
        x *= 0.7071f;
        y *= 0.7071f;
    }
    if (down(s, Walk)) {
        x *= 0.5f;
        y *= 0.5f;
    }
    pad.thumbs[0] = axis(x);
    pad.thumbs[1] = axis(y);

    // Mouse speed (counts per second) maps to camera stick deflection; a short
    // smoothing keeps polls between mouse reports from dropping to zero.
    const auto now = Clock::now();
    const float seconds = (std::max)(0.001f, std::chrono::duration<float>(now - s.lastPoll).count());
    s.lastPoll = now;
    const float scale = 0.0012f * settings().mouseSensitivity;
    const float targetX = float(s.mouseX) / seconds * scale;
    const float targetY = float(s.mouseY) / seconds * scale * (settings().invertMouseY ? 1.0f : -1.0f);
    s.mouseX = s.mouseY = 0;
    const float blend = (std::min)(1.0f, seconds / 0.03f);
    s.stickX += (targetX - s.stickX) * blend;
    s.stickY += (targetY - s.stickY) * blend;
    pad.thumbs[2] = axis(s.stickX);
    pad.thumbs[3] = axis(s.stickY);
    return pad;
}

void mergePad(VirtualPad& into, const VirtualPad& other) {
    into.buttons |= other.buttons;
    into.leftTrigger = (std::max)(into.leftTrigger, other.leftTrigger);
    into.rightTrigger = (std::max)(into.rightTrigger, other.rightTrigger);
    for (int i = 0; i < 4; ++i)
        into.thumbs[i] = int16_t(std::clamp(int(into.thumbs[i]) + int(other.thumbs[i]), -32768, 32767));
}

}  // namespace tz::host
