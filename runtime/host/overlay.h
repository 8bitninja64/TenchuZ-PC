#pragma once
// F1 settings menu (Dear ImGui), modeled on the Blitz 2003 port's: sections on the
// left, options on the right, keyboard/mouse and controller navigation. While it is
// open the game is paused (its frame thread waits in VdSwap) and gets no input.
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace tz::host::overlay {

// Window thread. True when the message was the menu's (F1, or input while open).
bool handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// Command processor thread (renderer).
void initialize(HWND window, ID3D11Device* device, ID3D11DeviceContext* context);
// Draws the menu and the FPS counter into the back buffer before Present.
// `fps` is the rate of new game frames.
void render(ID3D11RenderTargetView* target, float fps);

// Any thread.
bool isOpen();
// True once after the menu paused the game (the next frame's time excludes the pause).
bool takePause();
bool ownsInput();  // the game sees neutral controls (open, or buttons still held from closing)
// Game's controller read: opens the menu on Back + Start; true when the game must
// see neutral controls instead (the combo itself never reaches it).
bool filterGamePad(uint16_t buttons);

// Window messages the menu posts to the window thread.
constexpr UINT kMessageMenu = WM_APP + 2;        // menu opened or closed
constexpr UINT kMessageFullscreen = WM_APP + 3;  // wParam: fullscreen on/off

}  // namespace tz::host::overlay
