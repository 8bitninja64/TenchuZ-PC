#include "platform/window_sdl.h"

#include <cctype>

namespace tz::platform {
namespace {

thread_local const SDL_Event* g_current = nullptr;

Uint32 messageEventType() {
    static const Uint32 type = SDL_RegisterEvents(1);
    return type;
}

}  // namespace

const SDL_Event* currentEvent() { return g_current; }

int virtualKey(SDL_Keycode key) {
    if (key >= 'a' && key <= 'z') return std::toupper(int(key));
    if (key >= '0' && key <= '9') return int(key);
    if (key >= SDLK_F1 && key <= SDLK_F12) return VK_F1 + int(key - SDLK_F1);
    switch (key) {
    case SDLK_SPACE: return VK_SPACE;
    case SDLK_RETURN: case SDLK_KP_ENTER: return VK_RETURN;
    case SDLK_ESCAPE: return VK_ESCAPE;
    case SDLK_TAB: return VK_TAB;
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return VK_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return VK_CONTROL;
    case SDLK_LALT: case SDLK_RALT: return VK_MENU;
    case SDLK_UP: return VK_UP;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    default: return 0;
    }
}

void dispatchEvent(HWND window, const SDL_Event& event, WindowProcedure procedure) {
    g_current = &event;
    auto send = [&](UINT message, WPARAM wParam = 0, LPARAM lParam = 0) { procedure(window, message, wParam, lParam); };
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        send(WM_CLOSE);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        // lParam as Windows has it: bit 29 Alt held, bit 30 auto-repeat.
        const bool alt = event.key.mod & SDL_KMOD_ALT;
        const LPARAM lParam = (alt ? LPARAM(1) << 29 : 0) | (event.key.repeat ? LPARAM(1) << 30 : 0);
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const bool system = alt || event.key.key == SDLK_LALT || event.key.key == SDLK_RALT;
        send(down ? (system ? WM_SYSKEYDOWN : WM_KEYDOWN) : (system ? WM_SYSKEYUP : WM_KEYUP),
             WPARAM(virtualKey(event.key.key)), lParam);
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        send(WM_CHAR);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        send(WM_MOUSEMOVE);
        send(WM_INPUT);  // relative motion for the camera (input.cpp reads the event)
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        switch (event.button.button) {
        case SDL_BUTTON_LEFT: send(down ? WM_LBUTTONDOWN : WM_LBUTTONUP); break;
        case SDL_BUTTON_RIGHT: send(down ? WM_RBUTTONDOWN : WM_RBUTTONUP); break;
        case SDL_BUTTON_MIDDLE: send(down ? WM_MBUTTONDOWN : WM_MBUTTONUP); break;
        case SDL_BUTTON_X1: send(down ? WM_XBUTTONDOWN : WM_XBUTTONUP, WPARAM(XBUTTON1) << 16); break;
        case SDL_BUTTON_X2: send(down ? WM_XBUTTONDOWN : WM_XBUTTONUP, WPARAM(XBUTTON2) << 16); break;
        default: break;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        send(WM_MOUSEWHEEL);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        send(WM_SETFOCUS);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        send(WM_KILLFOCUS);
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        send(WM_SIZE);
        break;
    case SDL_EVENT_WINDOW_MOVED:
        send(WM_MOVE);
        break;
    default:
        if (event.type == messageEventType())
            send(UINT(event.user.code), WPARAM(reinterpret_cast<uintptr_t>(event.user.data1)),
                 LPARAM(reinterpret_cast<intptr_t>(event.user.data2)));
        break;
    }
    g_current = nullptr;
}

}  // namespace tz::platform

BOOL PostMessageW(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    SDL_Event event{};
    event.type = tz::platform::messageEventType();
    event.user.windowID = window ? SDL_GetWindowID(static_cast<SDL_Window*>(window)) : 0;
    event.user.code = Sint32(message);
    event.user.data1 = reinterpret_cast<void*>(uintptr_t(wParam));
    event.user.data2 = reinterpret_cast<void*>(intptr_t(lParam));
    return SDL_PushEvent(&event) ? TRUE : FALSE;
}
