#include "host/gamepad.h"
#include "host/settings.h"
#include "kernel/kernel.h"

#include <SDL3/SDL.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace tz::host {
namespace {

constexpr int kPads = 4;

// Open devices: owned by the input thread, which also does all SDL calls (opening a
// pad, reading it or sending rumble can block on the device for milliseconds).
struct Pad {
    SDL_Gamepad* handle = nullptr;
    SDL_JoystickID id = 0;
};

// What the title's threads see: a snapshot per slot and pending rumble, behind a
// mutex that is only ever held for a copy.
struct Published {
    bool connected = false;
    VirtualPad state;  // mapped
    VirtualPad raw;    // physical
    char name[64] = {};
};
struct Rumble {
    uint16_t left = 0, right = 0;
    bool dirty = false;
};

struct Gamepads {
    std::array<Pad, kPads> pads;  // input thread only
    std::mutex mutex;
    std::array<Published, kPads> published;
    std::array<Rumble, kPads> rumble;
    std::once_flag started;
};

Gamepads& gamepads() {
    static Gamepads* instance = new Gamepads;
    return *instance;
}

int16_t stickAxis(Sint16 value, bool invert) {
    const int v = invert ? -int(value) - 1 : int(value);
    return int16_t(std::clamp(v, -32768, 32767));
}

uint8_t triggerAxis(Sint16 value) { return uint8_t(std::clamp(int(value), 0, 32767) * 255 / 32767); }

// ---- Remapping -------------------------------------------------------------------------
// Inputs and game controls, by the same index: XInput button bits, or a trigger.
struct Input {
    const char* name;
    const char* label;
    uint16_t bit;  // 0 for the triggers
};
constexpr Input kInputs[] = {
    {"a", "A", 0x1000}, {"b", "B", 0x2000}, {"x", "X", 0x4000}, {"y", "Y", 0x8000},
    {"lb", "LB", 0x0100}, {"rb", "RB", 0x0200}, {"lt", "LT", 0}, {"rt", "RT", 0},
    {"start", "Start", 0x0010}, {"back", "Back", 0x0020}, {"ls", "Left stick click", 0x0040},
    {"rs", "Right stick click", 0x0080}, {"dpad_up", "D-pad up", 0x0001}, {"dpad_down", "D-pad down", 0x0002},
    {"dpad_left", "D-pad left", 0x0004}, {"dpad_right", "D-pad right", 0x0008}};
constexpr int kInputCount = int(std::size(kInputs));
constexpr int kLeftTrigger = 6, kRightTrigger = 7;
constexpr uint8_t kTriggerPressed = 30;  // the title treats a trigger above 30 as pressed

// source[control] = the input that drives it (written by the menu, read by the input thread).
// Filled by reloadControllerMapping before the input thread starts.
std::array<std::atomic<uint8_t>, kInputCount> source{};

uint8_t inputValue(const VirtualPad& raw, int input) {
    if (input == kLeftTrigger) return raw.leftTrigger;
    if (input == kRightTrigger) return raw.rightTrigger;
    return (raw.buttons & kInputs[input].bit) ? 255 : 0;
}

VirtualPad remap(const VirtualPad& raw) {
    VirtualPad mapped = raw;
    mapped.buttons = 0;
    mapped.leftTrigger = mapped.rightTrigger = 0;
    for (int control = 0; control < kInputCount; ++control) {
        const uint8_t value = inputValue(raw, source[control].load(std::memory_order_relaxed));
        if (control == kLeftTrigger) mapped.leftTrigger = (std::max)(mapped.leftTrigger, value);
        else if (control == kRightTrigger) mapped.rightTrigger = (std::max)(mapped.rightTrigger, value);
        else if (value > kTriggerPressed) mapped.buttons |= kInputs[control].bit;
    }
    return mapped;
}

VirtualPad read(SDL_Gamepad* pad) {
    VirtualPad state;
    static const std::pair<SDL_GamepadButton, uint16_t> buttons[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
        // Face buttons by position, as on an Xbox pad.
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000}, {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    for (const auto& [button, mask] : buttons)
        if (SDL_GetGamepadButton(pad, button)) state.buttons |= mask;
    state.leftTrigger = triggerAxis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    state.rightTrigger = triggerAxis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    // SDL's Y axes point down; the 360's point up.
    state.thumbs[0] = stickAxis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), false);
    state.thumbs[1] = stickAxis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), true);
    state.thumbs[2] = stickAxis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), false);
    state.thumbs[3] = stickAxis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), true);
    return state;
}

// Keeps pads[] in sync with the connected gamepads, in connection order.
void refresh(Gamepads& g) {
    for (auto& pad : g.pads)
        if (pad.handle && !SDL_GamepadConnected(pad.handle)) {
            logf("[input] controller disconnected: %s", SDL_GetGamepadName(pad.handle));
            SDL_CloseGamepad(pad.handle);
            pad = Pad{};
        }
    // Close gaps so the first connected pad is player 1.
    std::stable_partition(g.pads.begin(), g.pads.end(), [](const Pad& pad) { return pad.handle != nullptr; });
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        const bool open = std::any_of(g.pads.begin(), g.pads.end(), [&](const Pad& pad) { return pad.handle && pad.id == ids[i]; });
        if (open) continue;
        auto free = std::find_if(g.pads.begin(), g.pads.end(), [](const Pad& pad) { return !pad.handle; });
        if (free == g.pads.end()) break;
        if (SDL_Gamepad* handle = SDL_OpenGamepad(ids[i])) {
            free->handle = handle;
            free->id = ids[i];
            logf("[input] controller %d: %s", int(free - g.pads.begin()) + 1, SDL_GetGamepadName(handle));
        }
    }
    SDL_free(ids);
}

void inputMain() {
    auto& g = gamepads();
    // No SDL window exists: accept input whatever window has focus (as XInput does).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        logf("[input] SDL gamepad support unavailable: %s", SDL_GetError());
        return;
    }
    // Community layouts for pads SDL does not know (SDL_GameControllerDB, zlib license),
    // next to the executable; a user's own gamecontrollerdb_user.txt is loaded last.
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto directory = std::filesystem::path(executable).parent_path();
    for (const char* name : {"gamecontrollerdb.txt", "gamecontrollerdb_user.txt"}) {
        const auto path = tz::utf8(directory / name);
        if (std::filesystem::exists(path)) {
            const int added = SDL_AddGamepadMappingsFromFile(path.c_str());
            logf("[input] %s: %d controller layouts", name, added);
        }
    }
    auto nextRefresh = std::chrono::steady_clock::now();
    for (;;) {
        bool devicesChanged = false;
#ifdef _WIN32
        SDL_PumpEvents();
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED) devicesChanged = true;
#else
        // The game window's thread owns the SDL event queue here: update the pads directly
        // (new and removed pads are found by the refresh every second).
        SDL_UpdateGamepads();
#endif
        const auto now = std::chrono::steady_clock::now();
        if (devicesChanged || now >= nextRefresh) {
            refresh(g);
            nextRefresh = now + std::chrono::seconds(1);
        }
        std::array<Published, kPads> snapshot;
        for (int i = 0; i < kPads; ++i)
            if (g.pads[i].handle) {
                snapshot[i].connected = true;
                snapshot[i].raw = read(g.pads[i].handle);
                snapshot[i].state = remap(snapshot[i].raw);
                if (const char* name = SDL_GetGamepadName(g.pads[i].handle))
                    strncpy_s(snapshot[i].name, name, _TRUNCATE);
            }
        std::array<Rumble, kPads> pending;
        {
            std::lock_guard lock(g.mutex);
            g.published = snapshot;
            pending = g.rumble;
            for (auto& rumble : g.rumble) rumble.dirty = false;
        }
        for (int i = 0; i < kPads; ++i)
            if (pending[i].dirty && g.pads[i].handle)
                SDL_RumbleGamepad(g.pads[i].handle, pending[i].left, pending[i].right, 0xFFFFFFFFu);
        // Poll faster than the title's 60 Hz reads.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void start() {
    auto& g = gamepads();
    std::call_once(g.started, [] {
        reloadControllerMapping();
        std::thread(inputMain).detach();
    });
}

}  // namespace

bool gamepadState(uint32_t user, VirtualPad& out) {
    start();
    auto& g = gamepads();
    std::lock_guard lock(g.mutex);
    if (user >= kPads || !g.published[user].connected) return false;
    out = g.published[user].state;
    return true;
}

bool gamepadRawState(uint32_t user, VirtualPad& out) {
    start();
    auto& g = gamepads();
    std::lock_guard lock(g.mutex);
    if (user >= kPads || !g.published[user].connected) return false;
    out = g.published[user].raw;
    return true;
}

const char* const* controllerControls(int& count) {
    static const auto names = [] {
        std::array<const char*, kInputCount> list{};
        for (int i = 0; i < kInputCount; ++i) list[i] = kInputs[i].name;
        return list;
    }();
    count = kInputCount;
    return names.data();
}

const char* controllerInputName(int input) { return input >= 0 && input < kInputCount ? kInputs[input].label : "?"; }

uint32_t controllerInputs(const VirtualPad& raw) {
    uint32_t pressed = 0;
    for (int input = 0; input < kInputCount; ++input)
        if (inputValue(raw, input) > kTriggerPressed) pressed |= 1u << input;
    return pressed;
}

void reloadControllerMapping() {
    for (int control = 0; control < kInputCount; ++control) {
        int input = control;
        const auto found = settings().pad.find(kInputs[control].name);
        if (found != settings().pad.end())
            for (int i = 0; i < kInputCount; ++i)
                if (found->second == kInputs[i].name) input = i;
        source[control] = uint8_t(input);
    }
}

std::string gamepadName(uint32_t user) {
    start();
    auto& g = gamepads();
    std::lock_guard lock(g.mutex);
    if (user >= kPads || !g.published[user].connected) return {};
    return g.published[user].name[0] ? g.published[user].name : "Controller";
}

bool gamepadRumble(uint32_t user, uint16_t left, uint16_t right) {
    start();
    auto& g = gamepads();
    std::lock_guard lock(g.mutex);
    if (user >= kPads || !g.published[user].connected) return false;
    auto& rumble = g.rumble[user];
    if (rumble.left != left || rumble.right != right) {
        rumble = {left, right, true};
    }
    return true;
}

}  // namespace tz::host
