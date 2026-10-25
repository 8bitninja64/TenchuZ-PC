#pragma once
// Linux: the game window is an SDL3 window (DXVK-native presents to it; its HWND is the
// SDL_Window*). Its events are translated into the Win32 messages the host code already
// handles (keys as virtual-key codes, mouse buttons, focus, size), so input.cpp and the F1
// menu work unchanged; PostMessageW queues a message as an SDL user event.
#ifndef _WIN32
#include <SDL3/SDL.h>

namespace tz::platform {

using WindowProcedure = void (*)(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// Window thread: passes the messages an event stands for to the procedure. While they are
// handled, currentEvent() is the event (the F1 menu hands it to ImGui's SDL3 backend; mouse
// motion reads its relative movement).
void dispatchEvent(HWND window, const SDL_Event& event, WindowProcedure procedure);
const SDL_Event* currentEvent();

// The virtual-key code of an SDL key (0 when there is none).
int virtualKey(SDL_Keycode key);

}  // namespace tz::platform
#endif
