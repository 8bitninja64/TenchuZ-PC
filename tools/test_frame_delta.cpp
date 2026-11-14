// Standalone regression test: clang-cl /EHsc /std:c++20 /Iruntime
// tools/test_frame_delta.cpp /Febuild/test_frame_delta.exe /Fobuild/test_frame_delta.obj
#include "host/frame_delta.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    for (int limit : {0, 30, 60, 120, 240}) {
        const float frame = 1.0f / (limit ? limit : 144);
        for (float stall : {0.101f, 1.0f, 16.0f, 60.0f}) {
            tz::host::FrameDeltaSmoother smoother;
            for (int i = 0; i < 120; ++i) smoother.update(frame, limit, false);
            check(smoother.update(stall, limit, false) == stall, "stall retains guest handling");
            // A mission must immediately resume normal speed without opening the menu.
            for (int i = 0; i < 1200; ++i)
                check(std::abs(smoother.update(frame, limit, false) - frame) < 0.000001f,
                      "loading stall must not affect subsequent frame times");
            check(std::abs(smoother.update(30.0f, limit, true) - frame) < 0.000001f,
                  "menu pause advances by one ordinary frame");
        }
    }

    tz::host::FrameDeltaSmoother jitter;
    double elapsed = 0, given = 0;
    for (int i = 0; i < 10000; ++i) {
        const float frame = i % 2 ? 0.010f : 0.006666667f;
        elapsed += frame;
        const float output = jitter.update(frame, 120, false);
        given += output;
        check(output > 0 && output < 0.012f, "jitter remains bounded");
    }
    check(std::abs(elapsed - given) < 0.02, "smoothing preserves elapsed game time");

    // Sustained rendering below the 60 FPS target (for example at 4K) must
    // advance game time at the measured rate, then recover when the load drops.
    for (int slowRate : {20, 30, 45}) {
        tz::host::FrameDeltaSmoother loaded;
        double realTime = 0.0, gameTime = 0.0;
        for (int i = 0; i < 2400; ++i) {
            const float frame = 1.0f / (i >= 600 && i < 1200 ? slowRate : 60);
            realTime += frame;
            const float output = loaded.update(frame, 60, false);
            gameTime += output;
            check(std::isfinite(output) && output > 0.0f, "sustained slow frames stay valid");
            if (i >= 1800)
                check(std::abs(output - 1.0f / 60) < 0.000001f,
                      "rendering recovery must not require a menu pause");
        }
        check(std::abs(realTime - gameTime) < 0.02, "sustained slow rendering preserves game speed");
    }

    for (float bad : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::quiet_NaN()}) {
        jitter.update(bad, 60, false);
        check(std::abs(jitter.update(1.0f / 60, 60, false) - 1.0f / 60) < 0.000001f,
              "invalid sample cannot poison subsequent frames");
    }
    std::puts("Frame delta regression tests passed.");
}
