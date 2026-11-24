#include "host/animation_timing.h"
#include "host/settings.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

int main() {
    using tz::host::advanceFrameAtlas;
    for (int fps : {15, 30, 60, 120, 144, 240}) {
        float fraction = 0;
        uint32_t frame = 0;
        for (int i = 0; i < fps * 10; ++i)
            advanceFrameAtlas(fraction, frame, 30.0 / fps, 1000);
        assert(frame == 300);
        assert(std::abs(fraction) < 0.001f);
    }
    float fraction = 0;
    uint32_t frame = 15;
    assert(!advanceFrameAtlas(fraction, frame, 0.25, 16));
    assert(frame == 15);
    assert(advanceFrameAtlas(fraction, frame, 2.75, 16));
    assert(frame == 2 && fraction == 0);
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        assert(!advanceFrameAtlas(fraction, frame, invalid, 16));
        assert(frame == 2 && fraction == 0);
    }
    assert(!advanceFrameAtlas(fraction, frame, 1, 0));

    // A cinematic temporarily limits presentation without overwriting the user's
    // gameplay setting. Gameplay resumes after the player stops reporting frames.
    using namespace tz::host;
    editableSettings().frameRateLimit = 120;
    assert(activeFrameRateLimit() == 120);
    for (int limit : {0, 30, 60, 120, 144, 240}) {
        editableSettings().frameRateLimit = limit;
        noteCutsceneFrame();
        assert(activeFrameRateLimit() == 30);
        assert(settings().frameRateLimit == limit);
    }
    editableSettings().frameRateLimit = 120;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(activeFrameRateLimit() == 120);
}
