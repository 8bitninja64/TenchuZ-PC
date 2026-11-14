#pragma once

#include <algorithm>
#include <cmath>

namespace tz::host {

// Title-thread frame timing, independent of the guest memory and host clock so
// loading stalls and pause/resume can be checked without running the game.
class FrameDeltaSmoother {
public:
    float update(float measured, int limit, bool paused) {
        if (paused && average_ > 0.0) {
            measured = float(average_);
            owed_ = 0.0;
        }
        if (!std::isfinite(measured) || !(measured > 0.0f) || measured > 0.1f) {
            // Preserve the title's handling of this frame, but never seed the
            // running average with a loading stall. Otherwise subsequent frames
            // advance too far and accumulate a deficit that slows gameplay.
            average_ = owed_ = 0.0;
            return measured;
        }
        average_ = average_ > 0.0 ? average_ + (measured - average_) * 0.1 : measured;
        double step = average_;
        if (limit > 0) {
            const double period = 1.0 / limit;
            if (std::abs(average_ - period) < period * 0.1) step = period;
        }
        owed_ += measured;
        const double given = std::clamp(step + (owed_ - step) * 0.1, step * 0.5, step * 1.5);
        owed_ -= given;
        return float(given);
    }

private:
    double average_ = 0.0;
    double owed_ = 0.0;
};

}  // namespace tz::host
