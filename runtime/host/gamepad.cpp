#include "host/gamepad.h"
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

struct Pad {
    SDL_Gamepad* handle = nullptr;
    SDL_JoystickID id = 0;
    VirtualPad state;
    uint16_t rumble[2] = {};
    bool rumbleDirty = false;
};

struct Gamepads {
    std::mutex mutex;
    std::array<Pad, kPads> pads;
    bool started = false;
    bool available = false;
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
        const auto path = (directory / name).string();
        if (std::filesystem::exists(path)) {
            const int added = SDL_AddGamepadMappingsFromFile(path.c_str());
            logf("[input] %s: %d controller layouts", name, added);
        }
    }
    {
        std::lock_guard lock(g.mutex);
        g.available = true;
    }
    auto nextRefresh = std::chrono::steady_clock::now();
    for (;;) {
        SDL_PumpEvents();
        SDL_Event event;
        bool devicesChanged = false;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED) devicesChanged = true;
        std::lock_guard lock(g.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (devicesChanged || now >= nextRefresh) {
            refresh(g);
            nextRefresh = now + std::chrono::seconds(1);
        }
        for (auto& pad : g.pads) {
            if (!pad.handle) continue;
            pad.state = read(pad.handle);
            if (pad.rumbleDirty) {
                SDL_RumbleGamepad(pad.handle, pad.rumble[0], pad.rumble[1], 0xFFFFFFFFu);
                pad.rumbleDirty = false;
            }
        }
        // Poll faster than the title's 60 Hz reads.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void start() {
    auto& g = gamepads();
    std::lock_guard lock(g.mutex);
    if (g.started) return;
    g.started = true;
    std::thread(inputMain).detach();
}

}  // namespace

bool gamepadState(uint32_t user, VirtualPad& out) {
    start();
    auto& g = gamepads();
    std::lock_guard lock(g.mutex);
    if (user >= kPads || !g.pads[user].handle) return false;
    out = g.pads[user].state;
    return true;
}

bool gamepadRumble(uint32_t user, uint16_t left, uint16_t right) {
    start();
    auto& g = gamepads();
    std::lock_guard lock(g.mutex);
    if (user >= kPads || !g.pads[user].handle) return false;
    auto& pad = g.pads[user];
    if (pad.rumble[0] != left || pad.rumble[1] != right) {
        pad.rumble[0] = left;
        pad.rumble[1] = right;
        pad.rumbleDirty = true;
    }
    return true;
}

}  // namespace tz::host
