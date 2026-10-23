#include "control.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

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
    uint16_t held = 0, tapped = 0;
    int16_t thumbs[4] = {};
    std::string screenshot;
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

uint16_t parseButtons(const std::string& names) {
    static const std::pair<const char*, uint16_t> buttons[] = {
        {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010},
        {"back", 0x0020}, {"ls", 0x0040}, {"rs", 0x0080}, {"lb", 0x0100}, {"rb", 0x0200},
        {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}};
    uint16_t mask = 0;
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
        } else if (command == "traceframe" || command == "dumpframe") {
            c.flags.insert(command);
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
    state.buttons = c.held | c.tapped;
    for (int i = 0; i < 4; ++i) state.thumbs[i] = c.thumbs[i];
    return state;
}

std::string takeScreenshotRequest() {
    if (!active()) return {};
    poll();
    auto& c = controller();
    std::lock_guard lock(c.mutex);
    return std::exchange(c.screenshot, std::string{});
}

bool takeFlag(const std::string& name) {
    if (!active()) return false;
    auto& c = controller();
    std::lock_guard lock(c.mutex);
    return c.flags.erase(name) != 0;
}

}  // namespace tz::control
