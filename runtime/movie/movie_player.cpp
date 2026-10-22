// Full-screen movie playback.
//
// The title plays WMV9/WMA Pro movies with a software decoder whose clock is
// driven by audio output, which the runtime does not provide yet. Movies are
// intercepted at the game's blocking play routine; host playback (Media
// Foundation decodes both codecs natively) belongs here once a renderer exists.
#include "kernel/kernel.h"
#include "ppc_recomp_shared.h"

using namespace tz;

// int PlayMovie(const char* path, ...): returns 1 when playback completed and
// 0 when the movie could not be opened.
PPC_FUNC(sub_82296FF8) {
    logf("[movie] skipping %s (host movie playback not implemented yet)", loadString(ctx.r3.u32).c_str());
    ctx.r3.u64 = 1;
}
