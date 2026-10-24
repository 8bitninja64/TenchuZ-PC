#include "host/overlay.h"

#include "host/gamepad.h"
#include "host/input.h"
#include "host/settings.h"
#include "host/update_check.h"
#include "kernel/kernel.h"
#include "kernel/control.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace tz::audio {
void setOutputVolume(float volume);
}

namespace tz::host::overlay {
namespace {

// ImGui is driven from two threads: the window thread feeds it messages, the
// command processor thread builds and draws frames. This lock serializes them. It is
// recursive: ImGui's message handler re-enters the window procedure (ReleaseCapture on a
// mouse button release sends WM_CAPTURECHANGED), which locked it twice and threw.
std::recursive_mutex g_mutex;
std::atomic<bool> g_open{false};
std::atomic<bool> g_holdInput{false};  // after closing: pads stay away from the game until released
std::atomic<bool> g_initialized{false};
HWND g_window = nullptr;
ID3D11DeviceContext* g_context = nullptr;

// Key capture for rebinding: the control waiting for a key (-1 none).
std::atomic<int> g_capture{-1};
// Controller capture: the game control waiting for a controller button (-1 none). The
// button that chose "Change" must be released first; Esc or 8 seconds cancel.
std::atomic<int> g_padCapture{-1};
bool g_padCaptureArmed = false;
ULONGLONG g_padCaptureSince = 0;
std::atomic<bool> g_popupOpen{false};

enum Section { kDisplay, kAudio, kControls, kMissions, kUpdate, kAbout, kSectionCount };
const char* kSectionNames[kSectionCount] = {"Display", "Audio", "Controls", "Missions", "Update", "About"};
int g_section = 0;
// Two panes: choosing a section moves the highlight into its options; B
// (Backspace) moves it back to the list. Only F1, Esc or Back+Start close the menu.
enum class Focus { None, Sections, Content };
Focus g_focusTo = Focus::Sections;

// Tenchu colours: charcoal panels, crimson accents, gold highlights.
const ImVec4 kCrimson(0.72f, 0.09f, 0.12f, 1.0f);
const ImVec4 kSteel(0.33f, 0.40f, 0.52f, 1.0f);
const ImVec4 kGold(0.95f, 0.76f, 0.30f, 1.0f);
ImFont* g_fontBody = nullptr;
ImFont* g_fontTitle = nullptr;
constexpr float kFontPx = 40.0f;  // rasterized large, scaled down to the window (stays sharp)

void setOpen(bool open) {
    if (g_open.exchange(open) == open) return;
    logf("[menu] %s", open ? "opened" : "closed");
    g_focusTo = Focus::Sections;
    g_capture = -1;
    g_padCapture = -1;
    g_holdInput = true;  // the game gets the pads back once everything is released
    if (g_initialized) {
        std::lock_guard lock(g_mutex);
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
    }
    if (g_window) PostMessageW(g_window, kMessageMenu, open, 0);
}

void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10;
    s.ChildRounding = 8;
    s.FrameRounding = 6;
    s.GrabRounding = 6;
    s.PopupRounding = 6;
    s.WindowBorderSize = 0;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(18, 16);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 9);
    s.ScrollbarSize = 14;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.07f, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.11f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.95f, 0.94f, 0.92f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.58f, 0.58f, 0.62f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.17f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.30f, 0.35f, 1.0f);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = kCrimson;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.92f, 0.20f, 0.22f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.24f, 1.0f);
    c[ImGuiCol_ButtonHovered] = kSteel;
    c[ImGuiCol_ButtonActive] = kCrimson;
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.27f, 1.0f);
    c[ImGuiCol_HeaderHovered] = kSteel;
    c[ImGuiCol_HeaderActive] = kCrimson;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.30f, 0.36f, 1.0f);
    c[ImGuiCol_NavHighlight] = kGold;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
}

// The system UI font (nothing to ship), else ImGui's built-in one.
void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    auto load = [&](std::initializer_list<const char*> paths, float px) -> ImFont* {
        for (const char* path : paths)
            if (std::filesystem::exists(path))
                if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, px)) return font;
        return nullptr;
    };
    g_fontBody = load({"C:/Windows/Fonts/segoeui.ttf"}, kFontPx);
    if (!g_fontBody) {
        ImFontConfig config;
        config.SizePixels = 13;
        g_fontBody = io.Fonts->AddFontDefault(&config);
    }
    g_fontTitle = load({"C:/Windows/Fonts/seguibl.ttf", "C:/Windows/Fonts/segoeuib.ttf"}, kFontPx * 1.4f);
    if (!g_fontTitle) g_fontTitle = g_fontBody;
    io.FontDefault = g_fontBody;
}

// ---- Controllers --------------------------------------------------------------------
// Pads drive the menu as ImGui keyboard navigation; Back + Start toggles it.
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
                   kShoulderL = 0x0100, kShoulderR = 0x0200, kA = 0x1000, kB = 0x2000;
uint16_t g_padHeld = 0, g_padPressed = 0;

// Assigns a controller button to a game control; the control that had it takes the old one.
void assignPadButton(int control, int input) {
    int count = 0;
    const char* const* names = controllerControls(count);
    if (control < 0 || control >= count || input < 0 || input >= count) return;
    auto& pad = editableSettings().pad;
    auto current = [&](int c) {
        const auto it = pad.find(names[c]);
        return it != pad.end() ? it->second : std::string(names[c]);
    };
    const std::string previous = current(control);
    for (int other = 0; other < count; ++other)
        if (other != control && current(other) == names[input]) pad[names[other]] = previous;
    pad[names[control]] = names[input];
    reloadControllerMapping();
    saveSettings();
}

// While capturing, the first controller button pressed after all were released.
void pollPadCapture(uint32_t pressedInputs) {
    const int control = g_padCapture;
    if (control < 0) return;
    if (GetTickCount64() - g_padCaptureSince > 8000) {
        g_padCapture = -1;
        return;
    }
    if (!g_padCaptureArmed) {
        g_padCaptureArmed = pressedInputs == 0;
        return;
    }
    for (int input = 0; input < 32; ++input)
        if (pressedInputs & (1u << input)) {
            assignPadButton(control, input);
            g_padCapture = -1;
            return;
        }
}

void pollPads() {
    uint16_t held = 0;
    // Automated runs drive controller 1 from the control file.
    if (control::active()) {
        const auto pad = control::pad();
        held = pad.buttons;
        if (pad.thumbs[1] > 16000) held |= kUp;
        if (pad.thumbs[1] < -16000) held |= kDown;
        if (pad.thumbs[0] < -16000) held |= kLeft;
        if (pad.thumbs[0] > 16000) held |= kRight;
    }
    // The menu reads the physical buttons: a remapping cannot lock it out.
    uint32_t inputs = 0;
    if (control::active()) {
        const auto pad = control::pad();
        VirtualPad scripted;
        scripted.buttons = pad.buttons;
        scripted.leftTrigger = pad.leftTrigger;
        scripted.rightTrigger = pad.rightTrigger;
        inputs |= controllerInputs(scripted);
    }
    for (uint32_t user = 0; user < 4; ++user) {
        VirtualPad pad;
        if (!gamepadRawState(user, pad)) continue;
        inputs |= controllerInputs(pad);
        uint16_t buttons = pad.buttons;
        // The left stick navigates like the D-pad.
        if (pad.thumbs[1] > 16000) buttons |= kUp;
        if (pad.thumbs[1] < -16000) buttons |= kDown;
        if (pad.thumbs[0] < -16000) buttons |= kLeft;
        if (pad.thumbs[0] > 16000) buttons |= kRight;
        held |= buttons;
    }
    // A button being assigned does not navigate, nor does it after the assignment until
    // every button is released (else the press also counted as B = back).
    static bool waitRelease = false;
    if (g_padCapture >= 0) {
        pollPadCapture(inputs);
        if (g_padCapture < 0) waitRelease = true;
        held = 0;
    } else if (waitRelease) {
        if (!held && !inputs) waitRelease = false;
        held = 0;
    }
    static uint16_t last = 0;
    const uint16_t pressed = held & ~last;
    last = held;
    if (g_holdInput && !held) g_holdInput = false;
    // Opening is handled where the game reads the pads (filterGamePad): the game must
    // not see the Start of the combo. Closing happens here, while the game is paused.
    if (g_open && (held & (kBack | kStart)) == (kBack | kStart) && (pressed & (kBack | kStart))) {
        setOpen(false);
        g_padHeld = 0;
        return;
    }
    g_padPressed = g_open ? pressed : 0;
    g_padHeld = g_open ? held : 0;
}

void feedNavigation() {
    static uint16_t previous = 0;
    ImGuiIO& io = ImGui::GetIO();
    const uint16_t changed = g_padHeld ^ previous;
    auto key = [&](uint16_t bit, ImGuiKey imguiKey) {
        if (changed & bit) io.AddKeyEvent(imguiKey, (g_padHeld & bit) != 0);
    };
    key(kUp, ImGuiKey_UpArrow);
    key(kDown, ImGuiKey_DownArrow);
    key(kLeft, ImGuiKey_LeftArrow);
    key(kRight, ImGuiKey_RightArrow);
    key(kA, ImGuiKey_Space);   // activate / toggle
    key(kB, ImGuiKey_Escape);  // cancel
    previous = g_padHeld;
}

// ---- Sections ------------------------------------------------------------------------
// Entering a section's options focuses its first control: called right before it (text
// cannot take focus, so a request at the top of a page that starts with text was lost).
void focusFirstOption() {
    if (g_focusTo != Focus::Content) return;
    ImGui::SetKeyboardFocusHere();
    g_focusTo = Focus::None;
}
void drawFps(float fps) {
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##fps", nullptr, flags)) ImGui::Text("%.0f fps", fps);
    ImGui::End();
}

bool drawDisplay(Settings& st) {
    bool changed = false;
    bool fullscreen = st.fullscreen;
    focusFirstOption();
    if (ImGui::Checkbox("Fullscreen (Alt+Enter / F11)", &fullscreen)) {
        st.fullscreen = fullscreen;
        PostMessageW(g_window, kMessageFullscreen, fullscreen, 0);
        changed = true;
    }
    static const std::pair<int, const char*> rates[] = {{30, "30 (original)"}, {60, "60"},   {120, "120"},
                                                        {144, "144"},          {165, "165"}, {240, "240"},
                                                        {0, "Unlimited"}};
    const char* current = "Custom";
    for (const auto& [value, label] : rates)
        if (value == st.frameRateLimit) current = label;
    static const int startRate = st.frameRateLimit;
    if (ImGui::BeginCombo("Frame rate limit", current)) {
        for (const auto& [value, label] : rates)
            if (ImGui::Selectable(label, value == st.frameRateLimit)) {
                st.frameRateLimit = value;
                changed = true;
            }
        ImGui::EndCombo();
    }
    if ((startRate == 30) != (st.frameRateLimit == 30))
        ImGui::TextColored(kGold, "Switching to or from 30 fps applies on the next start.");
    changed |= ImGui::Checkbox("VSync", &st.vsync);
    changed |= ImGui::Checkbox("Show FPS counter", &st.showFps);
    // HDR10: switches live; needs Windows HDR ("Use HDR" in Display settings) on this monitor.
    changed |= ImGui::Checkbox("HDR", &st.hdr);
    if (st.hdr && !hdrAvailable())
        ImGui::TextColored(kGold, "This monitor is not in HDR mode: turn on \"Use HDR\" in Windows display settings.");
    if (st.hdr) {
        changed |= ImGui::SliderInt("Peak brightness (nits)", &st.hdrPeakNits, 400, 4000);
        changed |= ImGui::SliderInt("Paper white (nits)", &st.hdrPaperWhiteNits, 80, 400);
        if (st.hdrPaperWhiteNits > st.hdrPeakNits) st.hdrPaperWhiteNits = st.hdrPeakNits;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Applied on the next start:");
    static const std::pair<int, const char*> scales[] = {
        {1, "720p (1x)"}, {2, "1440p (2x)"}, {3, "4K (3x)"}, {4, "5K (4x)"}};
    const char* scale = scales[0].second;
    for (const auto& [value, label] : scales)
        if (value == st.resolutionScale) scale = label;
    if (ImGui::BeginCombo("Internal resolution", scale)) {
        for (const auto& [value, label] : scales)
            if (ImGui::Selectable(label, value == st.resolutionScale)) {
                st.resolutionScale = value;
                changed = true;
            }
        ImGui::EndCombo();
    }
    static const int levels[] = {1, 2, 4, 8, 16};
    char anisotropy[16];
    snprintf(anisotropy, sizeof(anisotropy), st.anisotropy <= 1 ? "Off" : "%dx", st.anisotropy);
    if (ImGui::BeginCombo("Anisotropic filtering", anisotropy)) {
        for (int level : levels) {
            char label[16];
            snprintf(label, sizeof(label), level <= 1 ? "Off" : "%dx", level);
            if (ImGui::Selectable(label, level == st.anisotropy)) {
                st.anisotropy = level;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool drawAudio(Settings& st) {
    bool changed = false;
    focusFirstOption();
    if (ImGui::SliderInt("Volume", &st.volume, 0, 100, "%d%%")) changed = true;
    if (ImGui::Checkbox("Mute", &st.mute)) changed = true;
    if (changed) audio::setOutputVolume(st.mute ? 0.0f : st.volume / 100.0f);
    return changed;
}

const char* controlLabel(const char* name) {
    static const std::pair<const char*, const char*> labels[] = {
        {"move_up", "Move forward"}, {"move_down", "Move back"}, {"move_left", "Move left"},
        {"move_right", "Move right"}, {"walk", "Walk (hold)"}, {"a", "A (jump)"}, {"b", "B (draw sword)"},
        {"x", "X (attack)"}, {"y", "Y (reset view)"}, {"lb", "LB (item)"}, {"rb", "RB (crouch)"},
        {"lt", "LT (aid)"}, {"rt", "RT (strafe)"}, {"start", "Start"}, {"back", "Back"},
        {"dpad_up", "D-pad up"}, {"dpad_down", "D-pad down"}, {"dpad_left", "D-pad left"},
        {"dpad_right", "D-pad right"}, {"ls", "Left stick click"}, {"rs", "Right stick click"}};
    for (const auto& [key, label] : labels)
        if (!std::strcmp(key, name)) return label;
    return name;
}

bool drawControls(Settings& st) {
    bool changed = false;
    bool anyPad = false;
    for (uint32_t i = 0; i < 4; ++i) {
        const std::string name = gamepadName(i);
        if (name.empty()) continue;
        ImGui::Text("Controller %u: %s", i + 1, name.c_str());
        anyPad = true;
    }
    if (!anyPad) ImGui::TextDisabled("No controller connected (Xbox, PlayStation, Switch and most others work).");
    // Controller buttons (all controllers), by position on an Xbox pad.
    ImGui::TextUnformatted("Controller buttons:");
    int padCount = 0;
    const char* const* padControls = controllerControls(padCount);
    // Tables without their own scrolling: the pane scrolls, and every Change button is
    // a plain D-pad stop (a scrolling table is a child window the pad must enter first).
    if (ImGui::BeginTable("pad", 3, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("button", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("change", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 0; i < padCount; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(controlLabel(padControls[i]));
            ImGui::TableSetColumnIndex(1);
            if (g_padCapture == i) {
                ImGui::TextColored(kGold, "press a controller button (Esc cancels)");
            } else {
                const auto it = st.pad.find(padControls[i]);
                const std::string input = it != st.pad.end() ? it->second : padControls[i];
                int index = i;
                for (int j = 0; j < padCount; ++j)
                    if (input == padControls[j]) index = j;
                ImGui::TextUnformatted(controllerInputName(index));
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(1000 + i);
            if (i == 0) focusFirstOption();
            if (ImGui::SmallButton("Change") && g_padCapture < 0) {
                g_padCaptureArmed = false;
                g_padCaptureSince = GetTickCount64();
                g_padCapture = i;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset controller to defaults")) {
        resetControllerDefaults();
        reloadControllerMapping();
        changed = true;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard and mouse (player 1):");
    int count = 0;
    const char* const* controls = keyboardControls(count);
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("change", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 0; i < count; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(controlLabel(controls[i]));
            ImGui::TableSetColumnIndex(1);
            const auto it = st.keys.find(controls[i]);
            if (g_capture == i) ImGui::TextColored(kGold, "press a key (Esc cancels)");
            else ImGui::TextUnformatted(it != st.keys.end() && !it->second.empty() ? it->second.c_str() : "-");
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(i);
            if (ImGui::SmallButton("Change")) g_capture = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset keyboard to defaults")) {
        resetKeyboardDefaults();
        reloadKeyBindings();
        changed = true;
    }
    changed |= ImGui::SliderFloat("Mouse sensitivity", &st.mouseSensitivity, 0.2f, 3.0f, "%.2f");
    changed |= ImGui::Checkbox("Invert mouse Y", &st.invertMouseY);
    ImGui::TextDisabled("F1 / Esc: this menu   Alt+Enter / F11: fullscreen   Controller: Back + Start");
    ImGui::TextDisabled("The menu itself always uses the controller's own buttons.");
    return changed;
}

// Missions: the same edit as tools/save_unlock.py, on the save the game uses.
std::string g_missionMessage;

std::filesystem::path savePath() {
    char appData[MAX_PATH]{};
    GetEnvironmentVariableA("APPDATA", appData, sizeof(appData));
    return std::filesystem::path(appData) / "TenchuZRecomp" / "content" / "00000001" / "TENCHU_Z" / "tenchu.bin";
}

void unlockMissions() {
    const auto path = savePath();
    std::vector<uint8_t> data;
    if (std::ifstream file{path, std::ios::binary}) data.assign(std::istreambuf_iterator<char>(file), {});
    if (data.size() != 0x1460) {
        g_missionMessage = "No save found. Save the game once (finish the first mission), then try again.";
        return;
    }
    auto be32 = [&](size_t offset) {
        return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 | uint32_t(data[offset + 2]) << 8 |
               data[offset + 3];
    };
    const auto backup = std::filesystem::path(path).concat(".bak");
    std::error_code error;
    if (!std::filesystem::exists(backup)) std::filesystem::copy_file(path, backup, error);
    uint32_t marked = 0;
    // 8 chapters of 488 bytes at +0x500: u32 chapter, u32 count, then 48-byte mission
    // records whose first difficulty status word (+12) gets bits 0|1 (cleared).
    for (size_t chapter = 0; chapter < 8; ++chapter) {
        const size_t base = 0x500 + chapter * 488;
        const uint32_t count = (std::min)(be32(base + 4), uint32_t((488 - 8) / 48));
        for (uint32_t i = 0; i < count; ++i) {
            data[base + 8 + i * 48 + 12 + 3] |= 3;
            ++marked;
        }
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    char message[160];
    snprintf(message, sizeof(message), "Done: %u missions marked cleared. Load the save again (title screen > "
                                       "Continue) to choose any of them.", marked);
    g_missionMessage = message;
}

void drawMissions() {
    ImGui::TextWrapped("Mission Select offers the missions your save has cleared. This marks every one of the 50 "
                       "missions cleared (on the first difficulty), so all of them can be chosen.");
    ImGui::Spacing();
    focusFirstOption();
    if (ImGui::Button("Unlock all missions")) unlockMissions();
    if (!g_missionMessage.empty()) ImGui::TextWrapped("%s", g_missionMessage.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("The original save is kept next to it as tenchu.bin.bak.");
}

// Update: this version, the latest GitHub release, and installing it (update_check.h).
bool drawUpdate(Settings& st) {
    namespace update = tz::host::update;
    bool changed = false;
    const update::Release release = update::checkStatus();
    const update::Install install = update::installStatus();
    const ImVec4 kGreen(0.4f, 0.9f, 0.5f, 1);
    ImGui::Text("This version: %s", update::version());
    ImGui::Spacing();
    switch (release.state) {
    case update::Release::Idle: ImGui::TextDisabled("Not checked yet."); break;
    case update::Release::Checking: ImGui::Text("Checking GitHub for a new release..."); break;
    case update::Release::UpToDate: ImGui::TextColored(kGreen, "Up to date (latest release: %s).", release.latest.c_str()); break;
    case update::Release::NoReleases: ImGui::TextDisabled("No release has been published yet."); break;
    case update::Release::Failed: ImGui::TextColored(kCrimson, "Could not check: %s", release.error.c_str()); break;
    case update::Release::Available: ImGui::TextColored(kGold, "%s is available.", release.latest.c_str()); break;
    }
    ImGui::Spacing();
    const bool busy = install.state == update::Install::Downloading || install.state == update::Install::Verifying ||
                      install.state == update::Install::Extracting || install.state == update::Install::Installing;
    const bool done = install.state == update::Install::Done;
    // One action button whose label follows the state (one ID, so the controller's highlight
    // stays on it from "Download and install" to "Restart now").
    // canInstall probes the folder with a file: once per release found, not every frame.
    static std::string probedFor, why;
    static bool canInstall = false;
    const bool available = release.state == update::Release::Available && !done;
    if (available && probedFor != release.latest) {
        canInstall = update::canInstall(release, why);
        probedFor = release.latest;
    }
    const bool installable = available && canInstall;
    if (available && !installable) ImGui::TextWrapped("It cannot be installed here (%s).", why.c_str());
    const std::string label = done          ? std::string("Restart now")
                              : installable ? "Download and install " + release.latest
                              : available   ? std::string("Open the release page")
                                            : std::string("Check now");
    const bool disabled = busy || release.state == update::Release::Checking;
    ImGui::BeginDisabled(disabled);
    if (!disabled) focusFirstOption();  // a disabled button cannot take the focus: the checkbox does
    if (ImGui::Button((label + "###update_action").c_str())) {
        if (done) {
            if (update::restart()) ExitProcess(0);
        } else if (installable) {
            update::installStart(release);
        } else if (available) {
            update::openPage(release.page);
        } else {
            update::checkStart();
        }
    }
    ImGui::EndDisabled();
    if (available && !busy) {
        ImGui::SameLine();
        if (ImGui::Button("Check again")) update::checkStart();
    }
    if (busy) {
        const char* step = install.state == update::Install::Downloading  ? "Downloading"
                           : install.state == update::Install::Verifying  ? "Checking the download"
                           : install.state == update::Install::Extracting ? "Unpacking"
                                                                          : "Installing";
        ImGui::Text("%s...", step);
        ImGui::ProgressBar(install.state == update::Install::Downloading ? install.fraction : 1.0f, ImVec2(-1, 0));
    }
    if (install.state == update::Install::Failed) ImGui::TextColored(kCrimson, "The update failed: %s", install.error.c_str());
    if (done) ImGui::TextColored(kGreen, "%s is installed. Restart to play it.", release.latest.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    focusFirstOption();
    changed |= ImGui::Checkbox("Check for updates when the game starts", &st.updateCheck);
    if (!release.notes.empty() && release.state == update::Release::Available) {
        ImGui::Spacing();
        ImGui::TextDisabled("Release notes");
        ImGui::TextWrapped("%s", release.notes.c_str());
    }
    return changed;
}

void drawAbout() {
    ImGui::TextWrapped("Tenchu Z for PC: the Xbox 360 executable from your own disc, statically recompiled to "
                       "native code, with its graphics on Direct3D 11.");
    ImGui::Spacing();
    ImGui::TextWrapped("Settings: %s", settingsFilePath().c_str());
    ImGui::TextWrapped("Save: %s", tz::utf8(savePath()).c_str());
}

void hint(const char* button, ImVec4 color, const char* action) {
    ImGui::TextColored(color, "%s", button);
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("%s", action);
    ImGui::SameLine(0, 22);
}

void drawMenu() {
    Settings& st = editableSettings();
    ImGuiIO& io = ImGui::GetIO();
    // Any controller button shows the navigation cursor: A only activates where it is
    // visible, and moving focus from code (SetKeyboardFocusHere) does not show it.
    if (g_padPressed) ImGui::SetNavCursorVisible(true);
    // LB/RB switch sections (the highlight follows); B (Backspace) returns to the list.
    if (g_padPressed & (kShoulderL | kShoulderR)) {
        g_section = (g_section + ((g_padPressed & kShoulderL) ? kSectionCount - 1 : 1)) % kSectionCount;
        g_focusTo = Focus::Sections;
    }
    const bool back = (g_padPressed & kB) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    if (back && !g_popupOpen && !ImGui::IsAnyItemActive() && g_capture < 0) g_focusTo = Focus::Sections;

    const ImVec2 size(io.DisplaySize.x * 0.72f, io.DisplaySize.y * 0.80f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (!ImGui::Begin("##settings", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    // Header: title over a crimson/steel stripe.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float stripe = ImGui::GetFontSize() / 3.0f;
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + stripe), ImGui::GetColorU32(kCrimson), 10,
                        ImDrawFlags_RoundCornersTop);
    draw->AddRectFilled(ImVec2(origin.x, origin.y + stripe), ImVec2(origin.x + size.x, origin.y + stripe * 1.6f),
                        ImGui::GetColorU32(kSteel));
    ImGui::Dummy(ImVec2(0, stripe));
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(ImVec4(1, 1, 1, 1), "TENCHU Z");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetTextLineHeight() * 0.35f);
    ImGui::TextDisabled("SETTINGS  -  game paused");
    ImGui::Spacing();

    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
    bool changed = false;
    // Both panes take part in the window's navigation, so a pad or the arrow keys also
    // reach the Resume and Quit buttons below them.
    ImGui::BeginChild("##sections", ImVec2(size.x * 0.22f, -footer), ImGuiChildFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.1f, 0.5f));
    for (int i = 0; i < kSectionCount; ++i) {
        const bool selected = g_section == i;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Header, kCrimson);
        if (selected && g_focusTo == Focus::Sections) {
            ImGui::SetKeyboardFocusHere();
            g_focusTo = Focus::None;
        }
        if (ImGui::Selectable(kSectionNames[i], selected, 0, ImVec2(0, ImGui::GetTextLineHeight() * 1.8f))) {
            g_section = i;
            g_focusTo = Focus::Content;
        }
        if (selected) ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##content", ImVec2(0, -footer),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kGold, "%s", kSectionNames[g_section]);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    switch (g_section) {
    case kDisplay: changed |= drawDisplay(st); break;
    case kAudio: changed |= drawAudio(st); break;
    case kControls: changed |= drawControls(st); break;
    case kMissions: drawMissions(); break;
    case kUpdate: changed |= drawUpdate(st); break;
    case kAbout: drawAbout(); break;
    }
    ImGui::PopItemWidth();
    if (g_focusTo == Focus::Content) g_focusTo = Focus::None;  // a page without controls (About)
    ImGui::EndChild();
    // Footer: controller hints, Resume and Quit game.
    ImGui::Spacing();
    hint("A", ImVec4(0.3f, 0.85f, 0.4f, 1), "select");
    hint("B", kCrimson, "back");
    hint("LB / RB", kSteel, "section");
    hint("F1 / Back+Start", kGold, "close");
    const ImGuiStyle& style = ImGui::GetStyle();
    auto width = [&](const char* text) { return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2; };
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width("Resume") - width("Quit game") - style.ItemSpacing.x);
    if (ImGui::Button("Resume")) setOpen(false);
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) ImGui::OpenPopup("Quit?");
    if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("Quit Tenchu Z?");
        ImGui::TextDisabled("Progress since your last save is lost.");
        ImGui::Spacing();
        if (ImGui::Button("Quit", ImVec2(width("Cancel") * 1.6f, 0))) ExitProcess(0);
        ImGui::SameLine();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button("Cancel", ImVec2(width("Cancel") * 1.6f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    g_popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::End();
    if (changed) saveSettings();
}

// A key or mouse button for the control waiting in g_capture (window thread).
void finishCapture(int virtualKey) {
    const int control = g_capture.exchange(-1);
    if (control < 0 || virtualKey == VK_ESCAPE) return;
    const std::string name = keyName(virtualKey);
    if (name.empty()) return;
    int count = 0;
    const char* const* controls = keyboardControls(count);
    if (control >= count) return;
    {
        std::lock_guard lock(g_mutex);  // the menu reads the bindings on the other thread
        editableSettings().keys[controls[control]] = name;
    }
    reloadKeyBindings();
    saveSettings();
}

}  // namespace

bool isOpen() { return g_open; }

bool filterGamePad(uint16_t buttons) {
    static bool comboHeld = false;
    // Back + Start on any pad's physical buttons (or the merged, mapped ones).
    bool combo = (buttons & (kBack | kStart)) == (kBack | kStart);
    for (uint32_t user = 0; user < 4 && !combo; ++user) {
        VirtualPad pad;
        if (gamepadRawState(user, pad)) combo = (pad.buttons & (kBack | kStart)) == (kBack | kStart);
    }
    if (combo && !comboHeld && !g_open) setOpen(true);
    comboHeld = combo;
    return combo || ownsInput();
}
bool ownsInput() { return g_open || g_holdInput; }

bool handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    g_window = window;
    if (message == kMessageMenu) {
        if (wParam) releaseMouse();  // free the cursor for the menu
        return true;
    }
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool repeat = keyDown && (lParam & (1 << 30));
    if (keyDown && wParam == VK_F1) {
        if (!repeat) setOpen(!g_open);
        return true;
    }
    if (!g_open) return false;
    // Rebinding: the next key or mouse button.
    if (g_capture >= 0) {
        int key = 0;
        if (keyDown && !repeat) key = int(wParam);
        else if (message == WM_LBUTTONDOWN) key = VK_LBUTTON;
        else if (message == WM_RBUTTONDOWN) key = VK_RBUTTON;
        else if (message == WM_MBUTTONDOWN) key = VK_MBUTTON;
        else if (message == WM_XBUTTONDOWN) key = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        if (key) {
            // A click on "Change" itself arrives first: only take a click after a short moment.
            static ULONGLONG armedAt = 0;
            static int armedFor = -1;
            if (armedFor != g_capture) {
                armedFor = g_capture;
                armedAt = GetTickCount64();
            }
            if (keyDown || GetTickCount64() - armedAt > 250) {
                finishCapture(key);
                armedFor = -1;
            }
            return true;
        }
    }
    if (keyDown && wParam == VK_ESCAPE && g_padCapture >= 0) {
        g_padCapture = -1;
        return true;
    }
    if (keyDown && wParam == VK_ESCAPE && !repeat && !g_popupOpen) {
        setOpen(false);
        return true;
    }
    if (g_initialized) {
        std::lock_guard lock(g_mutex);
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
    }
    // Keyboard and mouse belong to the menu while it is open.
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST);
}

void initialize(HWND window, ID3D11Device* device, ID3D11DeviceContext* context) {
    std::lock_guard lock(g_mutex);
    g_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    loadFonts();
    applyStyle();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, context);
    g_context = context;
    g_initialized = true;
}

void render(ID3D11RenderTargetView* target, float fps) {
    if (!g_initialized) return;
    pollPads();
    const bool showFps = settings().showFps;
    if (!g_open && !showFps) return;
    std::lock_guard lock(g_mutex);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // The fonts are rasterized at kFontPx: scale them to ~18 px per 540 lines of window.
    ImGuiIO& io = ImGui::GetIO();
    float px = 18.0f * io.DisplaySize.y / 540.0f;
    px = px < 15.0f ? 15.0f : px > 72.0f ? 72.0f : px;
    io.FontGlobalScale = px / (g_fontBody && g_fontBody->FontSize > 20 ? kFontPx : 13.0f);
    if (g_open) feedNavigation();
    ImGui::NewFrame();
    if (showFps) drawFps(fps);
    if (g_open) drawMenu();
    ImGui::Render();
    g_context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_padPressed = 0;
}

}  // namespace tz::host::overlay
