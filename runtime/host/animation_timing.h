#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tz::host {

// A zero-delay texture atlas advances one image per 30 Hz tick. Keep fractional
// ticks on the material so presentation rate does not change animation speed.
inline bool advanceFrameAtlas(float& fraction, uint32_t& frame, double ticks, uint32_t count) {
    if (!count || !std::isfinite(ticks) || ticks <= 0.0) return false;
    const double total = (std::isfinite(fraction) ? fraction : 0.0) + ticks;
    const double whole = std::floor(total + 1e-6);
    fraction = float((std::max)(0.0, total - whole));
    if (whole < 1.0) return false;
    frame = uint32_t((uint64_t(frame) + uint32_t(std::fmod(whole, count))) % count);
    return true;
}

}  // namespace tz::host
