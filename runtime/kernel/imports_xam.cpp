// XAM imports: user profile, content (saves), system UI, notifications, input.
// One local profile is signed in on controller 0; online features are absent.
#include "kernel.h"
#include "filesystem.h"
#include "ppc_recomp_shared.h"
#include <chrono>
#include <fstream>
#include <xinput.h>

using namespace tz;

namespace {

constexpr uint64_t kXuid = 0xE000000054454E43ull;  // offline-style XUID
constexpr uint32_t kHardDrive = 1;                 // the only storage device
constexpr uint32_t kContentDataSize = 0x134;       // XCONTENT_DATA

// XOVERLAPPED: result +0, length +4, context +8, event handle +0xC,
// completion routine +0x10, routine context +0x14, extended error +0x18.
uint32_t complete(uint32_t overlapped, uint32_t result, uint32_t length = 0) {
    if (!overlapped) return result;
    store32(overlapped + 0x00, result);
    store32(overlapped + 0x04, length);
    store32(overlapped + 0x18, result);
    if (const uint32_t event = load32(overlapped + 0x0C)) {
        auto object = lookupHandle(event);
        if (object && object->dispatcher) setEvent(object->dispatcher);
    }
    if (const uint32_t routine = load32(overlapped + 0x10))
        queueApc(currentThread(), Apc{routine, {result, length, overlapped}});
    return kErrorIoPending;
}

uint32_t stackArgument(const PPCContext& ctx, int index) {
    // Ninth and later arguments: low word of each 8-byte parameter slot.
    return load32(ctx.r1.u32 + 0x54 + uint32_t(index - 8) * 8);
}

void writeWide(uint32_t address, const std::u16string& text, uint32_t capacity) {
    const uint32_t count = (std::min)(uint32_t(text.size()), capacity - 1);
    for (uint32_t i = 0; i < count; ++i) store16(address + i * 2, uint16_t(text[i]));
    store16(address + count * 2, 0);
}

std::string hex32(uint32_t value) {
    char text[9];
    snprintf(text, sizeof(text), "%08X", value);
    return text;
}

// ---- Content ----------------------------------------------------------------
// Content packages are folders: <user>/content/<type>/<file name>.

std::filesystem::path contentRoot(uint32_t contentType) {
    return userDirectory() / "content" / hex32(contentType);
}

std::string contentFileName(uint32_t contentData) {
    return std::string(memory->host<const char>(contentData + 0x108),
                       strnlen(memory->host<const char>(contentData + 0x108), 42));
}

struct EnumeratorObject : KernelObject {
    EnumeratorObject() : KernelObject(Kind::Enumerator) {}
    std::vector<std::vector<uint8_t>> items;  // raw guest records
    size_t cursor = 0;
    uint32_t itemSize = 0;
};

// ---- Notifications ----------------------------------------------------------

struct ListenerObject : KernelObject {
    ListenerObject() : KernelObject(Kind::NotifyListener) {}
    std::mutex mutex;
    std::deque<std::pair<uint32_t, uint32_t>> queue;
    uint64_t areas = 0;
};
std::mutex listenerMutex;
std::vector<std::weak_ptr<ListenerObject>> listeners;

void postNotification(ListenerObject& listener, uint32_t id, uint32_t parameter) {
    std::lock_guard lock(listener.mutex);
    listener.queue.emplace_back(id, parameter);
    setEvent(listener.dispatcher);
}

// ---- Profile settings ----------------------------------------------------------
// Setting ids encode type (bits 28-31) and size (bits 16-27).

std::filesystem::path settingPath(uint32_t id) { return userDirectory() / "profile" / (hex32(id) + ".bin"); }

bool loadSetting(uint32_t id, std::vector<uint8_t>& data) {
    std::ifstream file(settingPath(id), std::ios::binary);
    if (!file) return false;
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool defaultSetting(uint32_t id, uint32_t& value) {
    switch (id) {
    case 0x10040002: value = 0; return true;  // Y axis inversion
    case 0x10040003: value = 3; return true;  // controller vibration on
    case 0x10040012: value = 0; return true;  // gamer zone
    case 0x10040015: value = 0; return true;  // difficulty
    case 0x10040018: value = 0; return true;  // control sensitivity
    case 0x1004001D: value = 1; return true;  // auto aim
    case 0x1004001E: value = 1; return true;  // auto center
    case 0x1004001F: value = 0; return true;  // movement control
    case 0x10040024: value = 0; return true;  // camera inversion
    case 0x10040034: value = 0; return true;  // car transmission
    default: return false;
    }
}

}  // namespace

// ---- Memory -------------------------------------------------------------------------

PPC_FUNC(__imp__XamAlloc) {
    // (flags, size, address*)
    const uint32_t address = memory->allocateVirtual((std::max)(ctx.r4.u32, 1u), 0x1000);
    store32(ctx.r5.u32, address);
    ctx.r3.u64 = address ? 0 : 0x8007000E;  // E_OUTOFMEMORY
}
PPC_FUNC(__imp__XamFree) { memory->free(ctx.r3.u32); ctx.r3.u64 = 0; }

// ---- Users --------------------------------------------------------------------------

PPC_FUNC(__imp__XamUserGetSigninState) { ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0; }  // signed in locally

PPC_FUNC(__imp__XamUserGetXUID) {
    // (user, typeMask, xuid*)
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = kErrorNoSuchUser; return; }
    store64(ctx.r5.u32, kXuid);
    ctx.r3.u64 = kErrorSuccess;
}

PPC_FUNC(__imp__XamUserGetName) {
    // (user, buffer, length)
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = kErrorNoSuchUser; return; }
    const char name[] = "Player";
    const uint32_t count = (std::min<uint32_t>)(ctx.r5.u32, sizeof(name));
    std::memcpy(memory->host(ctx.r4.u32), name, count);
    if (count) *memory->host(ctx.r4.u32 + count - 1) = 0;
    ctx.r3.u64 = kErrorSuccess;
}

PPC_FUNC(__imp__XamUserCheckPrivilege) {
    // (user, privilege, result*). Online privileges are not available.
    store32(ctx.r5.u32, 0);
    ctx.r3.u64 = ctx.r3.u32 == 0 ? kErrorSuccess : kErrorNoSuchUser;
}

PPC_FUNC(__imp__XamUserAreUsersFriends) {
    // (user, xuids*, count, result*, overlapped)
    if (ctx.r6.u32) store32(ctx.r6.u32, 0);
    ctx.r3.u64 = complete(ctx.r7.u32, kErrorSuccess);
}

PPC_FUNC(__imp__XamUserCreateStatsEnumerator) {
    // Leaderboards need Xbox Live.
    ctx.r3.u64 = kErrorFunctionFailed;
}

PPC_FUNC(__imp__XamUserReadProfileSettings) {
    // (titleId, user, xuidCount, xuids*, settingCount, settingIds*, bufferSize*, buffer, overlapped)
    const uint32_t user = ctx.r4.u32, count = ctx.r7.u32, ids = ctx.r8.u32;
    const uint32_t sizePtr = ctx.r9.u32, buffer = ctx.r10.u32, overlapped = stackArgument(ctx, 8);
    if (user != 0) { ctx.r3.u64 = complete(overlapped, kErrorNoSuchUser); return; }
    uint32_t required = 8 + count * 0x28;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = load32(ids + i * 4);
        const uint32_t type = id >> 28;
        if (type == 4 || type == 6) required += (id >> 16) & 0xFFF;
    }
    if (!buffer || load32(sizePtr) < required) {
        store32(sizePtr, required);
        ctx.r3.u64 = kErrorInsufficientBuffer;
        return;
    }
    std::memset(memory->host(buffer), 0, required);
    // XUSER_READ_PROFILE_SETTING_RESULT { count; settings* } then settings.
    store32(buffer, count);
    const uint32_t settings = buffer + 8;
    store32(buffer + 4, settings);
    uint32_t data = settings + count * 0x28;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = load32(ids + i * 4), entry = settings + i * 0x28;
        const uint32_t type = id >> 28, size = (id >> 16) & 0xFFF;
        store32(entry + 0x00, 0);            // source: none
        store32(entry + 0x04, user);
        store32(entry + 0x10, id);
        *memory->host(entry + 0x18) = uint8_t(type);
        std::vector<uint8_t> stored;
        uint32_t value;
        if ((type == 4 || type == 6) && loadSetting(id, stored)) {
            const uint32_t bytes = (std::min)(uint32_t(stored.size()), size);
            std::memcpy(memory->host(data), stored.data(), bytes);
            store32(entry + 0x00, 2);        // source: title
            store32(entry + 0x20, bytes);
            store32(entry + 0x24, data);
        } else if (type == 4 || type == 6) {
            store32(entry + 0x24, data);
        } else if (defaultSetting(id, value)) {
            store32(entry + 0x00, 1);        // source: default
            store32(entry + 0x20, value);
        }
        if (type == 4 || type == 6) data += size;
        TZ_TRACE("xam", "[xam] read profile setting %08X source %u", id, load32(entry));
    }
    ctx.r3.u64 = complete(overlapped, kErrorSuccess);
}

PPC_FUNC(__imp__XamUserWriteProfileSettings) {
    // (titleId, user, settingCount, settings*, overlapped)
    const uint32_t count = ctx.r5.u32, settings = ctx.r6.u32, overlapped = ctx.r7.u32;
    std::filesystem::create_directories(userDirectory() / "profile");
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = settings + i * 0x28;
        const uint32_t id = load32(entry + 0x10), type = *memory->host(entry + 0x18);
        if (type != 4 && type != 6) continue;
        const uint32_t size = load32(entry + 0x20), pointer = load32(entry + 0x24);
        std::ofstream file(settingPath(id), std::ios::binary | std::ios::trunc);
        file.write(memory->host<const char>(pointer), size);
        TZ_TRACE("xam", "[xam] wrote profile setting %08X (%u bytes)", id, size);
    }
    ctx.r3.u64 = complete(overlapped, kErrorSuccess);
}

// ---- Content --------------------------------------------------------------------------

PPC_FUNC(__imp__XamContentCreate) {
    // (user, rootName, contentData*, flags, disposition*, license*, overlapped)
    const std::string root = loadString(ctx.r4.u32);
    const uint32_t contentData = ctx.r5.u32, flags = ctx.r6.u32;
    const uint32_t dispositionPtr = ctx.r7.u32, licensePtr = ctx.r8.u32, overlapped = ctx.r9.u32;
    const uint32_t contentType = load32(contentData + 4);
    const auto folder = contentRoot(contentType) / std::filesystem::u8path(contentFileName(contentData));
    const bool exists = std::filesystem::is_directory(folder);
    // Low byte: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING
    const uint32_t mode = flags & 0xFF;
    uint32_t result = kErrorSuccess, disposition = exists ? 2 : 1;
    if ((mode == 3 || mode == 5) && !exists) result = kErrorPathNotFound;
    else if (mode == 1 && exists) result = kErrorAlreadyExists;
    if (result == kErrorSuccess) {
        if ((mode == 2 || mode == 5) && exists) std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder);
        mountDevice(root, folder);
        if (dispositionPtr) store32(dispositionPtr, disposition);
        if (licensePtr) store32(licensePtr, 0);
    }
    TZ_TRACE("xam", "[xam] XamContentCreate %s device %08X type %08X '%s' flags %08X -> %u (lr %08X)", root.c_str(),
             load32(contentData), contentType, contentFileName(contentData).c_str(), flags, result, uint32_t(ctx.lr));
    ctx.r3.u64 = complete(overlapped, result);
}

PPC_FUNC(__imp__XamContentClose) {
    // (rootName, overlapped)
    unmountDevice(loadString(ctx.r3.u32));
    ctx.r3.u64 = complete(ctx.r4.u32, kErrorSuccess);
}

PPC_FUNC(__imp__XamContentCreateEnumerator) {
    // (user, deviceId, contentType, flags, maxItems, bufferSize*, handle*)
    const uint32_t deviceId = ctx.r4.u32, contentType = ctx.r5.u32, maxItems = ctx.r7.u32;
    TZ_TRACE("xam", "[xam] XamContentCreateEnumerator device %08X type %08X flags %08X max %u", deviceId, contentType,
             ctx.r6.u32, maxItems);
    auto enumerator = std::make_shared<EnumeratorObject>();
    enumerator->itemSize = kContentDataSize;
    std::error_code error;
    if (deviceId == 0 || deviceId == kHardDrive) {
        for (const auto& entry : std::filesystem::directory_iterator(contentRoot(contentType), error)) {
            if (!entry.is_directory()) continue;
            std::vector<uint8_t> record(kContentDataSize, 0);
            auto put32 = [&](size_t offset, uint32_t value) {
                for (int i = 0; i < 4; ++i) record[offset + i] = uint8_t(value >> (24 - i * 8));
            };
            put32(0, kHardDrive);
            put32(4, contentType);
            const auto u8name = entry.path().filename().u8string();
            const std::string name(u8name.begin(), u8name.end());
            for (size_t i = 0; i < name.size() && i < 127; ++i) record[8 + i * 2 + 1] = uint8_t(name[i]);
            std::memcpy(record.data() + 0x108, name.data(), (std::min<size_t>)(name.size(), 41));
            enumerator->items.push_back(std::move(record));
        }
    }
    if (ctx.r8.u32) store32(ctx.r8.u32, maxItems * kContentDataSize);
    store32(ctx.r9.u32, createHandle(enumerator));
    ctx.r3.u64 = kErrorSuccess;
}

PPC_FUNC(__imp__XamEnumerate) {
    // (handle, flags, buffer, bufferSize, itemsReturned*, overlapped)
    auto enumerator = lookupHandleAs<EnumeratorObject>(ctx.r3.u32);
    if (!enumerator) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    const uint32_t buffer = ctx.r5.u32, capacity = ctx.r6.u32 / (std::max)(enumerator->itemSize, 1u);
    uint32_t returned = 0;
    while (returned < capacity && enumerator->cursor < enumerator->items.size()) {
        const auto& item = enumerator->items[enumerator->cursor++];
        std::memcpy(memory->host(buffer + returned * enumerator->itemSize), item.data(), item.size());
        ++returned;
    }
    if (ctx.r7.u32) store32(ctx.r7.u32, returned);
    const uint32_t result = returned ? kErrorSuccess : kErrorNoMoreFiles;
    TZ_TRACE("xam", "[xam] XamEnumerate handle %08X flags %08X size %X -> %u items, result %u, itemsPtr %08X overlapped %08X",
             ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, returned, result, ctx.r7.u32, ctx.r8.u32);
    ctx.r3.u64 = complete(ctx.r8.u32, result, returned);
}

PPC_FUNC(__imp__XamContentGetCreator) {
    // (user, contentData*, isCreator*, xuid*, overlapped). Games use this to test
    // whether a package exists, so missing content must fail.
    const uint32_t contentData = ctx.r4.u32;
    const auto folder = contentRoot(load32(contentData + 4)) / std::filesystem::u8path(contentFileName(contentData));
    if (!std::filesystem::is_directory(folder)) {
        TZ_TRACE("xam", "[xam] XamContentGetCreator '%s': not found", contentFileName(contentData).c_str());
        ctx.r3.u64 = complete(ctx.r7.u32, kErrorPathNotFound);
        return;
    }
    if (ctx.r5.u32) store32(ctx.r5.u32, 1);
    if (ctx.r6.u32) store64(ctx.r6.u32, kXuid);
    ctx.r3.u64 = complete(ctx.r7.u32, kErrorSuccess);
}

PPC_FUNC(__imp__XamContentGetDeviceData) {
    // (deviceId, XDEVICE_DATA*): id, type, total u64, free u64, name[28]
    TZ_TRACE("xam", "[xam] XamContentGetDeviceData %08X", ctx.r3.u32);
    if (ctx.r3.u32 != kHardDrive) { ctx.r3.u64 = kErrorDeviceNotConnected; return; }
    const uint32_t data = ctx.r4.u32;
    std::memset(memory->host(data), 0, 0x50);
    store32(data + 0, kHardDrive);
    store32(data + 4, 1);  // hard drive
    store64(data + 8, 20ull << 30);
    store64(data + 16, 10ull << 30);
    writeWide(data + 24, u"Hard Drive", 28);
    ctx.r3.u64 = kErrorSuccess;
}

PPC_FUNC(__imp__XamContentGetDeviceState) {
    // (deviceId, overlapped)
    const uint32_t result = ctx.r3.u32 == kHardDrive ? kErrorSuccess : kErrorDeviceNotConnected;
    TZ_TRACE("xam", "[xam] XamContentGetDeviceState %08X -> %u", ctx.r3.u32, result);
    ctx.r3.u64 = complete(ctx.r4.u32, result);
}

PPC_FUNC(__imp__XamContentSetThumbnail) { ctx.r3.u64 = complete(ctx.r6.u32, kErrorSuccess); }

// ---- System UI: answered immediately --------------------------------------------------

PPC_FUNC(__imp__XamShowDeviceSelectorUI) {
    // (user, contentType, flags, totalRequested u64, deviceId*, overlapped)
    TZ_TRACE("xam", "[xam] XamShowDeviceSelectorUI user %u type %08X flags %08X", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    store32(ctx.r7.u32, kHardDrive);
    ctx.r3.u64 = complete(ctx.r8.u32, kErrorSuccess);
}

PPC_FUNC(__imp__XamShowSigninUI) { ctx.r3.u64 = kErrorSuccess; }
PPC_FUNC(__imp__XamShowGamerCardUIForXUID) { ctx.r3.u64 = complete(ctx.r5.u32, kErrorSuccess); }

namespace {
void answerMessageBox(PPCContext& ctx, uint32_t overlapped) {
    // (user, title, text, buttonCount, buttons*, activeButton, flags, result*, overlapped)
    const auto text = loadWideString(ctx.r5.u32);
    std::string narrow(text.begin(), text.end());
    logf("[xam] message box answered with button %u: %s", ctx.r8.u32, narrow.c_str());
    store32(ctx.r10.u32, ctx.r8.u32);
    ctx.r3.u64 = complete(overlapped, kErrorSuccess);
}
}  // namespace

PPC_FUNC(__imp__XamShowMessageBoxUI) { answerMessageBox(ctx, stackArgument(ctx, 8)); }
PPC_FUNC(__imp__XamShowMessageBoxUIEx) { answerMessageBox(ctx, stackArgument(ctx, 8)); }

PPC_FUNC(__imp__XamShowKeyboardUI) {
    // (user, flags, defaultText, title, description, buffer, length, overlapped)
    const auto text = loadWideString(ctx.r5.u32);
    writeWide(ctx.r8.u32, text, (std::max)(ctx.r9.u32, 1u));
    ctx.r3.u64 = complete(ctx.r10.u32, kErrorSuccess);
}

PPC_FUNC(__imp__XamShowDirtyDiscErrorUI) { fatal("The game reported a disc read error (missing or damaged data files)"); }

// ---- Notifications ---------------------------------------------------------------------

PPC_FUNC(__imp__XamNotifyCreateListener) {
    // (areas u64) -> handle
    auto listener = std::make_shared<ListenerObject>();
    listener->areas = ctx.r3.u64;
    listener->dispatcher = createDispatcherObject(kNotificationEvent, 16, 0);
    // System area: the local profile is already signed in.
    if (listener->areas & 1) postNotification(*listener, 0x0000000A, 1);  // XN_SYS_SIGNINCHANGED
    {
        std::lock_guard lock(listenerMutex);
        listeners.push_back(listener);
    }
    ctx.r3.u64 = createHandle(listener);
}

PPC_FUNC(__imp__XNotifyGetNext) {
    // (handle, matchId, id*, parameter*)
    auto listener = lookupHandleAs<ListenerObject>(ctx.r3.u32);
    if (!listener) { ctx.r3.u64 = 0; return; }
    std::lock_guard lock(listener->mutex);
    const uint32_t match = ctx.r4.u32;
    for (auto it = listener->queue.begin(); it != listener->queue.end(); ++it) {
        if (match && it->first != match) continue;
        store32(ctx.r5.u32, it->first);
        if (ctx.r6.u32) store32(ctx.r6.u32, it->second);
        listener->queue.erase(it);
        if (listener->queue.empty()) resetEvent(listener->dispatcher);
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}

// ---- Input -------------------------------------------------------------------------------
// XINPUT_STATE { packet; buttons u16; triggers u8 x2; thumbs s16 x4 }

namespace {
// TENCHU_INPUT_SCRIPT="ms:buttons,..." drives controller 0 for automated runs.
// Buttons are '+'-separated names; '-' releases everything.
struct ScriptedInput {
    std::vector<std::pair<uint32_t, uint16_t>> steps;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool active = false;
};

uint16_t buttonMask(const std::string& name) {
    static const std::pair<const char*, uint16_t> buttons[] = {
        {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010},
        {"back", 0x0020}, {"ls", 0x0040}, {"rs", 0x0080}, {"lb", 0x0100}, {"rb", 0x0200},
        {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}};
    for (const auto& [label, mask] : buttons)
        if (name == label) return mask;
    return 0;
}

ScriptedInput& scriptedInput() {
    static ScriptedInput script = [] {
        ScriptedInput result;
        char text[4096]{};
        if (!GetEnvironmentVariableA("TENCHU_INPUT_SCRIPT", text, sizeof(text))) return result;
        std::string spec = text;
        size_t position = 0;
        while (position < spec.size()) {
            size_t end = spec.find(',', position);
            if (end == std::string::npos) end = spec.size();
            const std::string step = spec.substr(position, end - position);
            const size_t colon = step.find(':');
            if (colon != std::string::npos) {
                uint16_t mask = 0;
                std::string names = step.substr(colon + 1);
                size_t at = 0;
                while (at <= names.size()) {
                    size_t plus = names.find('+', at);
                    if (plus == std::string::npos) plus = names.size();
                    mask |= buttonMask(names.substr(at, plus - at));
                    at = plus + 1;
                }
                result.steps.emplace_back(uint32_t(std::stoul(step.substr(0, colon))), mask);
            }
            position = end + 1;
        }
        result.active = !result.steps.empty();
        return result;
    }();
    return script;
}

uint16_t scriptedButtons() {
    auto& script = scriptedInput();
    const auto elapsed = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - script.start).count());
    uint16_t buttons = 0;
    for (const auto& [time, mask] : script.steps)
        if (time <= elapsed) buttons = mask;
    return buttons;
}
}  // namespace

PPC_FUNC(__imp__XamInputGetState) {
    // (user, flags, state*)
    XINPUT_STATE state{};
    const bool scripted = ctx.r3.u32 == 0 && scriptedInput().active;
    TZ_TRACE("input", "[input] XamInputGetState user %u flags %X lr %08X", ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
    if (scripted) {
        static std::atomic<uint32_t> packet{0};
        state.Gamepad.wButtons = scriptedButtons();
        state.dwPacketNumber = ++packet;
    } else if (ctx.r3.u32 >= 4 || XInputGetState(ctx.r3.u32, &state) != ERROR_SUCCESS) {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    const uint32_t out = ctx.r5.u32;
    store32(out + 0, state.dwPacketNumber);
    store16(out + 4, state.Gamepad.wButtons);
    *memory->host(out + 6) = state.Gamepad.bLeftTrigger;
    *memory->host(out + 7) = state.Gamepad.bRightTrigger;
    store16(out + 8, uint16_t(state.Gamepad.sThumbLX));
    store16(out + 10, uint16_t(state.Gamepad.sThumbLY));
    store16(out + 12, uint16_t(state.Gamepad.sThumbRX));
    store16(out + 14, uint16_t(state.Gamepad.sThumbRY));
    ctx.r3.u64 = kErrorSuccess;
}

PPC_FUNC(__imp__XamInputSetState) {
    // (user, flags, vibration*) where vibration = { left u16, right u16 }
    XINPUT_VIBRATION vibration{load16(ctx.r5.u32), load16(ctx.r5.u32 + 2)};
    ctx.r3.u64 = ctx.r3.u32 < 4 && XInputSetState(ctx.r3.u32, &vibration) == ERROR_SUCCESS
                     ? kErrorSuccess : kErrorDeviceNotConnected;
}

// ---- Title and system ---------------------------------------------------------------------

PPC_FUNC(__imp__XamGetExecutionId) {
    // (info**): pointer to the XEX execution id header field
    const uint32_t header = xexHeaderAddress();
    const uint32_t count = load32(header + 20);
    for (uint32_t i = 0; i < count; ++i) {
        if (load32(header + 24 + i * 8) != 0x40006) continue;
        store32(ctx.r3.u32, header + load32(header + 28 + i * 8));
        ctx.r3.u64 = kStatusSuccess;
        return;
    }
    ctx.r3.u64 = kStatusNotSupported;
}

PPC_FUNC(__imp__XamGetSystemVersion) { ctx.r3.u64 = 0x20044B00; }
PPC_FUNC(__imp__XamEnableInactivityProcessing) { ctx.r3.u64 = kErrorSuccess; }
PPC_FUNC(__imp__XamResetInactivity) { ctx.r3.u64 = kErrorSuccess; }
PPC_FUNC(__imp__XamLoaderLaunchTitle) {
    logf("[xam] XamLoaderLaunchTitle(%s): exiting", loadString(ctx.r3.u32).c_str());
    ExitProcess(0);
}
PPC_FUNC(__imp__XamLoaderTerminateTitle) {
    logf("[xam] XamLoaderTerminateTitle: exiting");
    ExitProcess(0);
}

PPC_FUNC(__imp__XGetAVPack) { ctx.r3.u64 = 6; }        // HDMI
PPC_FUNC(__imp__XGetGameRegion) { ctx.r3.u64 = 0xFF; }  // North America
PPC_FUNC(__imp__XGetLanguage) { ctx.r3.u64 = 1; }       // English

PPC_FUNC(__imp__ExGetXConfigSetting) {
    // (category, setting, buffer, bufferSize, requiredSize*)
    const uint16_t category = uint16_t(ctx.r3.u32), setting = uint16_t(ctx.r4.u32);
    const uint32_t buffer = ctx.r5.u32, bufferSize = ctx.r6.u32, requiredPtr = ctx.r7.u32;
    uint32_t value = 0, size = 4;
    if (category == 3 && setting == 0x9) value = 1;                 // user language: English
    else if (category == 3 && setting == 0xA) value = 0x00040000;   // video flags: widescreen
    else if (category == 3 && setting == 0x10) value = 0x67;        // country: United States
    else if (category == 2 && setting == 0x2) value = 0x00001000;   // AV region: NTSC-M
    else TZ_TRACE("xam", "[xam] ExGetXConfigSetting %u/%u defaulted to zero", category, setting);
    if (requiredPtr) store16(requiredPtr, uint16_t(size));
    if (buffer) {
        if (bufferSize < size) { ctx.r3.u64 = kStatusBufferTooSmall; return; }
        store32(buffer, value);
    }
    ctx.r3.u64 = kStatusSuccess;
}

// ---- Sessions, voice and XMsg: online features are unavailable ---------------------------

namespace {
struct SessionObject : KernelObject {
    SessionObject() : KernelObject(Kind::Other) {}
};
}  // namespace

PPC_FUNC(__imp__XamSessionCreateHandle) {
    auto session = std::make_shared<SessionObject>();
    session->dispatcher = createDispatcherObject(kNotificationEvent, 16, 0);
    store32(ctx.r3.u32, createHandle(session));
    ctx.r3.u64 = kErrorSuccess;
}

PPC_FUNC(__imp__XamSessionRefObjByHandle) {
    auto session = lookupHandle(ctx.r3.u32);
    if (!session) { ctx.r3.u64 = kErrorInvalidParameter; return; }
    store32(ctx.r4.u32, session->dispatcher);
    ctx.r3.u64 = kErrorSuccess;
}

PPC_FUNC(__imp__XamVoiceCreate) { ctx.r3.u64 = kErrorDeviceNotConnected; }
PPC_FUNC(__imp__XamVoiceClose) { ctx.r3.u64 = kErrorSuccess; }
PPC_FUNC(__imp__XamVoiceHeadsetPresent) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XamVoiceSubmitPacket) { ctx.r3.u64 = kErrorDeviceNotConnected; }

PPC_FUNC(__imp__XMsgStartIORequest) {
    // (app, message, overlapped, buffer, size)
    TZ_TRACE("xam", "[xam] XMsgStartIORequest app %08X message %08X", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = complete(ctx.r5.u32, kErrorFunctionFailed);
}

PPC_FUNC(__imp__XMsgStartIORequestEx) {
    TZ_TRACE("xam", "[xam] XMsgStartIORequestEx app %08X message %08X", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = complete(ctx.r5.u32, kErrorFunctionFailed);
}

PPC_FUNC(__imp__XMsgInProcessCall) {
    TZ_TRACE("xam", "[xam] XMsgInProcessCall app %08X message %08X", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = kErrorFunctionFailed;
}

PPC_FUNC(__imp__XMsgCancelIORequest) { ctx.r3.u64 = kStatusSuccess; }
