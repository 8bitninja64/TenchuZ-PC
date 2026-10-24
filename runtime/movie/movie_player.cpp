// Full-screen movie playback.
//
// The title's own player (a software WMV9/WMA Pro decoder whose clock follows
// audio output) runs in real time now that the audio driver and XMA decoder
// exist, so movies play as on the console. TENCHU_SKIP_MOVIES=1 skips them at
// the game's blocking play routine, which saves time in automated runs.
#include "kernel/kernel.h"
#include "guest/host.h"

using namespace tz;


// int PlayMovie(const char* path, ...): returns 1 when playback completed and
// 0 when the movie could not be opened.
TZ_OVERRIDE(82296FF8) {
    static const bool skip = [] {
        char value[8]{};
        return GetEnvironmentVariableA("TENCHU_SKIP_MOVIES", value, sizeof(value)) && value[0] == '1';
    }();
    if (skip) {
        logf("[movie] skipping %s", loadString(ctx.r3.u32).c_str());
        ctx.r3.u64 = 1;
        return;
    }
    TZ_TRACE("movie", "[movie] playing %s", loadString(ctx.r3.u32).c_str());
    __imp__sub_82296FF8(ctx, base);
}
