#include "control.h"
#include "cheats.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#include "kernel.h"

namespace tz::control {
namespace {

using Clock = std::chrono::steady_clock;

struct Controller {
    std::mutex mutex;
    std::string path;
    std::streamoff offset = 0;
    Clock::time_point nextPoll{};
    std::deque<std::string> queue;  // commands waiting for a delay to pass
    Clock::time_point resumeAt{};
    Clock::time_point releaseAt{};  // end of a tap
    uint32_t held = 0, tapped = 0;  // buttons; bit 16 LT, bit 17 RT (fully pressed)
    int16_t thumbs[4] = {};
    std::string screenshot;
    int burstRemaining = 0, burstIndex = 0;  // consecutive frames still to save
    std::string burstDirectory;
    std::set<std::string> flags;
};

Controller& controller() {
    static Controller* instance = [] {
        auto* result = new Controller;
        char value[MAX_PATH]{};
        if (GetEnvironmentVariableA("TENCHU_CONTROL", value, sizeof(value))) result->path = value;
        return result;
    }();
    return *instance;
}

uint32_t parseButtons(const std::string& names) {
    static const std::pair<const char*, uint32_t> buttons[] = {
        {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010},
        {"back", 0x0020}, {"ls", 0x0040}, {"rs", 0x0080}, {"lb", 0x0100}, {"rb", 0x0200},
        {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}, {"lt", 0x10000}, {"rt", 0x20000}};
    uint32_t mask = 0;
    std::stringstream stream(names);
    std::string name;
    while (std::getline(stream, name, '+'))
        for (const auto& [label, bit] : buttons)
            if (name == label) mask |= bit;
    return mask;
}

int16_t axis(double value) {
    if (value > 1) value = 1;
    if (value < -1) value = -1;
    return int16_t(value * 32767);
}

// Address expressions for peek/watchw: hex, '*addr+off' (through the pointer at addr), or
// pointer chains 'addr>off>off...' (each '>' loads the word at the current address, then
// adds the hex offset).
uint32_t guestAddress(const std::string& text) {
    const bool deref = !text.empty() && text[0] == '*';
    char* end = nullptr;
    uint32_t address = uint32_t(strtoul(text.c_str() + (deref ? 1 : 0), &end, 16));
    if (deref) address = tz::load32(address);
    while (end && *end) {
        if (*end == '>') address = tz::load32(address);
        else if (*end != '+') break;
        address += uint32_t(strtoul(end + 1, &end, 16));
    }
    return address;
}

bool closeTo(float actual, float wanted) {
    return std::fabs(actual - wanted) <= 1e-4f * (std::max)(1.0f, std::fabs(wanted));
}

// Scans committed guest memory (heap, stacks, image, physical) for big-endian floats.
void findFloats(const std::vector<float>& values) {
    const uint8_t* base = tz::memory->base();
    int found = 0;
    for (const auto [begin, end] : {std::pair{0x40000000u, 0x7F000000u}, std::pair{0x80000000u, 0xA0000000u},
                                    std::pair{0xA0000000u, 0xC0000000u}}) {
        for (uint64_t address = begin; address < end && found < 32;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(base + address, &info, sizeof(info))) break;
            const uint64_t regionEnd = (std::min)(uint64_t(end), uint64_t(static_cast<const uint8_t*>(info.BaseAddress) +
                                                                          info.RegionSize - base));
            const bool readable = info.State == MEM_COMMIT &&
                                  (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                                  !(info.Protect & PAGE_GUARD);
            if (readable) {
                for (uint64_t at = address; at + values.size() * 4 <= regionEnd && found < 32; at += 4) {
                    size_t i = 0;
                    for (; i < values.size(); ++i) {
                        const uint32_t bits = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + at + i * 4));
                        float value;
                        std::memcpy(&value, &bits, 4);
                        if (!closeTo(value, values[i])) break;
                    }
                    if (i == values.size()) {
                        logf("[control] findf match at %08X", uint32_t(at));
                        ++found;
                    }
                }
            }
            address = regionEnd;
        }
    }
    logf("[control] findf done: %d matches", found);
}

// Runs queued commands until one asks for a delay. Called with the mutex held.
void execute(Controller& c, Clock::time_point now) {
    while (!c.queue.empty() && now >= c.resumeAt && now >= c.releaseAt) {
        const std::string line = c.queue.front();
        std::stringstream stream(line);
        std::string command;
        stream >> command;
        // One screenshot per presented frame: later commands wait for a pending one.
        if (!c.screenshot.empty()) break;
        c.queue.pop_front();
        if (command.empty() || command[0] == '#') continue;
        logf("[control] %s", line.c_str());
        if (command == "press") {
            std::string names;
            int ms = 150;
            stream >> names >> ms;
            c.tapped = parseButtons(names);
            c.releaseAt = now + std::chrono::milliseconds(ms);
            // Leave the pad idle briefly so consecutive taps register as separate presses.
            c.resumeAt = c.releaseAt + std::chrono::milliseconds(100);
        } else if (command == "hold") {
            std::string names;
            stream >> names;
            c.held |= parseButtons(names);
        } else if (command == "release") {
            c.held = c.tapped = 0;
            for (auto& thumb : c.thumbs) thumb = 0;
        } else if (command == "stick") {
            double values[4] = {};
            for (auto& value : values) stream >> value;
            for (int i = 0; i < 4; ++i) c.thumbs[i] = axis(values[i]);
        } else if (command == "wait") {
            int ms = 0;
            stream >> ms;
            c.resumeAt = now + std::chrono::milliseconds(ms);
        } else if (command == "shot") {
            stream >> c.screenshot;
        } else if (command == "burst") {
            stream >> c.burstRemaining >> c.burstDirectory;
            c.burstIndex = 0;
        } else if (command == "cheat") {
            // cheat <id> [on|off]: F1 > Cheats (kernel/cheats.h); actions run with "on".
            std::string id, state = "on";
            stream >> id >> state;
            const int cheat = cheats::find(id);
            if (cheat < 0) logf("[control] unknown cheat %s", id.c_str());
            else cheats::set(cheat, state != "off");
        } else if (command == "traceframe" || command == "dumpframe" || command == "profile") {
            c.flags.insert(command);
        } else if (command == "peek") {
            // peek [*]<hex address>[+<hex offset>] [words]: logs guest words; '*' reads a pointer first.
            std::string where;
            int words = 4;
            stream >> where >> words;
            const uint32_t address = guestAddress(where);
            for (int i = 0; i < words; i += 8) {
                std::string text;
                for (int j = i; j < words && j < i + 8; ++j) {
                    char word[12];
                    snprintf(word, sizeof(word), " %08X", tz::load32(address + uint32_t(j) * 4));
                    text += word;
                }
                logf("[control] peek %08X:%s", address + uint32_t(i) * 4, text.c_str());
            }
        } else if (command == "list") {
            // list <hex head> <hex next offset> [words]: walks a linked list from *head,
            // logging each node's first words.
            std::string head, next;
            int words = 16;
            stream >> head >> next >> words;
            const uint32_t nextOffset = uint32_t(strtoul(next.c_str(), nullptr, 16));
            uint32_t node = tz::load32(uint32_t(strtoul(head.c_str(), nullptr, 16)));
            for (int count = 0; node && count < 64; ++count, node = tz::load32(node + nextOffset)) {
                std::string text;
                for (int j = 0; j < words; ++j) {
                    char word[12];
                    snprintf(word, sizeof(word), " %08X", tz::load32(node + uint32_t(j) * 4));
                    text += word;
                }
                logf("[control] node %08X:%s", node, text.c_str());
            }
        } else if (command == "findf") {
            // findf <float> [<float>...]: logs guest addresses holding these consecutive floats.
            std::vector<float> values;
            for (float value; stream >> value;) values.push_back(value);
            if (!values.empty()) findFloats(values);
        } else if (command == "countstart") {
            tz::startEntryCounts();
        } else if (command == "countstop") {
            std::string path;
            stream >> path;
            tz::stopEntryCounts(path);
        } else if (command == "watchw") {
            // watchw <hex address>: logs the guest code writing those 4 bytes.
            std::string where;
            stream >> where;
            tz::watchGuestWrites(guestAddress(where));
        } else if (command == "clock") {
            // clock: logs host milliseconds since the first command (timing measurements).
            static const Clock::time_point start = now;
            logf("[control] clock %.1f", std::chrono::duration<double, std::milli>(now - start).count());
        } else if (command == "quit") {
            logf("[control] quitting");
            fflush(nullptr);
            ExitProcess(0);
        } else {
            logf("[control] unknown command '%s'", command.c_str());
        }
    }
    if (now >= c.releaseAt) c.tapped = 0;
}

void poll() {
    auto& c = controller();
    if (c.path.empty()) return;
    std::lock_guard lock(c.mutex);
    const auto now = Clock::now();
    if (now >= c.nextPoll) {
        c.nextPoll = now + std::chrono::milliseconds(30);
        std::ifstream file(c.path, std::ios::binary);
        if (file) {
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < c.offset) c.offset = 0;  // file was recreated
            // Only whole lines are consumed.
            file.seekg(c.offset);
            std::string text(size_t(size - c.offset), '\0');
            file.read(text.data(), std::streamsize(text.size()));
            const size_t last = text.rfind('\n');
            if (last != std::string::npos) {
                std::stringstream lines(text.substr(0, last));
                std::string line;
                while (std::getline(lines, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    c.queue.push_back(line);
                }
                c.offset += std::streamoff(last + 1);
            }
        }
    }
    execute(c, now);
}

}  // namespace

bool active() { return !controller().path.empty(); }

PadState pad() {
    poll();
    auto& c = controller();
    std::lock_guard lock(c.mutex);
    PadState state;
    const uint32_t pressed = c.held | c.tapped;
    state.buttons = uint16_t(pressed);
    state.leftTrigger = (pressed & 0x10000) ? 255 : 0;
    state.rightTrigger = (pressed & 0x20000) ? 255 : 0;
    for (int i = 0; i < 4; ++i) state.thumbs[i] = c.thumbs[i];
    return state;
}

std::string takeScreenshotRequest() {
    if (!active()) return {};
    poll();
    auto& c = controller();
    std::lock_guard lock(c.mutex);
    if (c.burstRemaining > 0) {
        --c.burstRemaining;
        char name[32];
        snprintf(name, sizeof(name), "/burst_%05d.bmp", c.burstIndex++);
        return c.burstDirectory + name;
    }
    return std::exchange(c.screenshot, std::string{});
}

bool takeFlag(const std::string& name) {
    if (!active()) return false;
    auto& c = controller();
    std::lock_guard lock(c.mutex);
    return c.flags.erase(name) != 0;
}

}  // namespace tz::control
