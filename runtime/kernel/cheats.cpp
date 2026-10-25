// Trainer cheats (cheats.h). The addresses come from the translated code (see CLAUDE.md,
// "Cheats"): characters keep life at +0x118C (max +0x1188); the player is *0x82B58A58
// (vtable 0x82023BC0); the game-data manager MGR = *0x82B57814 holds the profile
// (MGR+0xA0: money, skills, costumes, mission stats) and the inventory (MGR+0x470).
#include "kernel/cheats.h"

#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "guest/host.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace tz::cheats {
namespace {

enum Cheat {
    kInfiniteHealth, kOneHitKills, kInvisible, kSilent, kInfiniteItems, kCurePoison, kUnbreakableGuard,
    kSuperJump, kFastRun, kLowGravity, kFastGrapple, kLongGrapple, kSoftLanding, kFreeShopping, kBestRank,
    kPerfectStealth,
    kMaxMoney, kUnlockSkills, kFillStock, kAllCostumes,
    kCount
};

const Info kInfo[kCount] = {
    {"health", "Infinite health", "You take no damage from attacks or poison (falling into a death zone still kills).", false},
    {"onehit", "One-hit kills", "Any hit by you, your weapons or your items kills an enemy (scripted boss phases may differ).", false},
    {"invisible", "Invisible to enemies", "Guards never see you or notice you bumping into them. They still react to noise, bodies and alarms.", false},
    {"silent", "Silent ninja", "Enemies hear no noise at all (yours, items, and their own calls for help).", false},
    {"items", "Infinite items", "Using an item does not use it up.", false},
    {"poison", "Cure poison", "Poison wears off at once.", false},
    {"guard", "Unbreakable guard", "Blocking never breaks your guard (the gauge stays full).", false},
    {"jump", "Super jump", "Jumps go twice as high (combine with Soft landing).", false},
    {"run", "Fast run", "You run 75% faster.", false},
    {"gravity", "Low gravity", "Falls are half as fast, for everyone.", false},
    {"zip", "Fast grapple", "The grappling hook pulls you twice as fast.", false},
    {"grapple", "Long grapple", "The grappling hook reaches twice as far (2200 instead of 1100).", false},
    {"landing", "Soft landing", "No stagger after landing from a great height.", false},
    {"shopping", "Free shopping", "Buying items, skills and costumes costs nothing (you still need the money to afford them).", false},
    {"rank", "Grand Master rank", "Every mission ends with at least the top score (900). It is saved as the mission's best.", false},
    {"stealth", "Perfect stealth record", "Detections and combat time do not count against your mission score.", false},
    {"money", "Max money", "Sets your money to 99,999,999.", true},
    {"skills", "Unlock all skills", "Makes every skill available in the skill shop.", true},
    {"stock", "Fill item stock", "Puts 99 of every shop item in your stock (carried ones included).", true},
    {"costumes", "Unlock all costumes", "Owns and lists every costume part.", true},
};

std::atomic<bool> g_enabled[kCount];
std::atomic<bool> g_requested[kCount];
std::mutex g_messageMutex;
std::string g_message;

void setMessage(const std::string& text) {
    std::lock_guard lock(g_messageMutex);
    g_message = text;
    logf("[cheats] %s", text.c_str());
}

float loadFloat(uint32_t address) {
    const uint32_t bits = load32(address);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
void storeFloat(uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    store32(address, bits);
}
uint8_t load8(uint32_t address) { return *memory->host<uint8_t>(address); }

// The local player while one exists (hideout or mission), else 0.
uint32_t player() {
    const uint32_t p = load32(0x82B58A58);
    return p && load32(p) == 0x82023BC0 ? p : 0;
}
// Players are types 0x1000-0x1003 (the damage routine's own test).
bool playerType(uint32_t object) { return object && (load32(object + 0xA0) & 0x1000); }
// The game-data manager while its first profile is active (sub_8229C290), else 0.
uint32_t manager() {
    const uint32_t m = load32(0x82B57814);
    if (!m || load32(m + 0x90) != m + 0xA0 || load32(m + 0xA0 + 0x14) != 1) return 0;
    return m;
}
uint32_t inventory(uint32_t m) { return m + (load8(m + 0xE20) ? 0x948 : 0x470); }

// Data patches: game constants rewritten while a cheat is on, restored when it is off.
struct Patch {
    uint32_t address;
    float scale;       // new value = original * scale
    uint32_t original = 0;
    bool saved = false;
};
Patch g_jumpPatches[] = {{0x829D49B4, 2.0f}, {0x829D49F4, 2.0f}, {0x829D4A34, 2.0f}, {0x829D4A54, 2.0f}};  // rows 0/2/4/5 height
Patch g_gravityPatches[] = {{0x8200E600, 0.5f}};
Patch g_zipPatches[] = {{0x820242D4, 2.0f}};
Patch g_grapplePatches[] = {{0x820242BC, 4.0f}};  // 1100^2 -> 2200^2: hit distance from the hand
bool g_patched[kCount];

template <size_t N> void applyPatches(int cheat, Patch (&patches)[N]) {
    const bool on = g_enabled[cheat].load(std::memory_order_relaxed);
    if (on == g_patched[cheat]) return;
    for (Patch& patch : patches) {
        if (!patch.saved) {
            patch.original = load32(patch.address);
            patch.saved = true;
        }
        if (on) {
            float value;
            std::memcpy(&value, &patch.original, 4);
            storeFloat(patch.address, value * patch.scale);
        } else {
            store32(patch.address, patch.original);
        }
    }
    g_patched[cheat] = on;
}

// Fast run: the player's top speed (+0x14DC) is computed by sub_8233F240 at spawn (and every
// frame in the hideout); the override scales its result and records whether the stored speed
// is scaled, so toggling while playing applies at once without scaling twice.
constexpr float kRunScale = 1.75f;
std::atomic<bool> g_runApplied{false};

void runAction(int cheat) {
    // Only with a loaded game (hideout or mission): before that the profile is a placeholder
    // that loading a save overwrites.
    const uint32_t m = player() ? manager() : 0;
    if (!m) {
        setMessage(std::string(kInfo[cheat].name) + ": go to the hideout or a mission first.");
        return;
    }
    switch (cheat) {
    case kMaxMoney:
        store32(m + 0x1E8, 99999999);
        setMessage("Money set to 99,999,999.");
        break;
    case kUnlockSkills:
        store32(m + 0x440, load32(m + 0x440) | 0x03FFFFFF);  // shop unlock mask: tab 1 bits 0-16, tab 2 bits 17-25
        store32(m + 0x444, load32(m + 0x444) | 0x03FFFFFF);  // "new" badges
        setMessage("Every skill is in the skill shop now.");
        break;
    case kFillStock: {
        // Entries {id 0x10000+i, stock, flags (bit 0 obtained)} at INV+8; carried items are
        // separate (ids INV+536, counts INV+708, 43 slots) and the save adds both: keep the
        // sum at 99, the game's maximum.
        const uint32_t inv = inventory(m);
        for (uint32_t i = 2; i < 32; ++i) {
            uint32_t carried = 0;
            for (uint32_t slot = 0; slot < 43; ++slot)
                if (load32(inv + 536 + slot * 4) == 0x10000 + i) carried += load32(inv + 708 + slot * 4);
            store32(inv + 12 + 12 * i, carried < 99 ? 99 - carried : 0);
            store32(inv + 16 + 12 * i, load32(inv + 16 + 12 * i) | 1);
        }
        store32(inv + 1228, load32(inv + 1228) | 0xFFFFFFFC);  // shop availability, items 2-31
        setMessage("99 of every shop item are in stock.");
        break;
    }
    case kAllCostumes: {
        // Owned (+0x1F0) and shop-listed (+0x230) part masks per category, from the part
        // lists at 0x82ABC970 (both genders).
        static const uint64_t kMasks[8] = {0x0000000000000FFFull, 0x00000FFFFFFFFFFFull, 0x0001FFFFFFFFFFFFull,
                                           0x000000007FFFFFFFull, 0x00000001FFFFFFFFull, 0x0000000001FFFFFFull,
                                           0x00000000000007FFull, 0x3FFFFFFFFFFFFFFFull};
        for (uint32_t c = 0; c < 8; ++c) {
            store64(m + 0x1F0 + 8 * c, load64(m + 0x1F0 + 8 * c) | kMasks[c]);
            store64(m + 0x230 + 8 * c, load64(m + 0x230 + 8 * c) | kMasks[c]);
        }
        setMessage("Every costume part is yours.");
        break;
    }
    default:
        break;
    }
}

}  // namespace

int count() { return kCount; }
const Info& info(int cheat) { return kInfo[cheat]; }
int find(const std::string& id) {
    for (int i = 0; i < kCount; ++i)
        if (id == kInfo[i].id) return i;
    return -1;
}
bool enabled(int cheat) { return g_enabled[cheat].load(std::memory_order_relaxed); }

void set(int cheat, bool on) {
    if (cheat < 0 || cheat >= kCount) return;
    if (kInfo[cheat].action) {
        if (on) g_requested[cheat] = true;
        return;
    }
    if (g_enabled[cheat].exchange(on) != on) logf("[cheats] %s %s", kInfo[cheat].name, on ? "on" : "off");
}

std::string message() {
    std::lock_guard lock(g_messageMutex);
    return g_message;
}

void applyFrame() {
    for (int i = 0; i < kCount; ++i)
        if (kInfo[i].action && g_requested[i].exchange(false)) runAction(i);
    applyPatches(kSuperJump, g_jumpPatches);
    applyPatches(kLowGravity, g_gravityPatches);
    applyPatches(kFastGrapple, g_zipPatches);
    applyPatches(kLongGrapple, g_grapplePatches);

    const uint32_t p = player();
    if (!p) return;
    if (enabled(kInfiniteHealth)) {
        // Also undoes life spent by power-ups; not while dying (death zones set life 0).
        const float life = loadFloat(p + 0x118C), max = loadFloat(p + 0x1188);
        if (life >= 1.0f && life < max) storeFloat(p + 0x118C, max);
    }
    if (enabled(kCurePoison) && load32(p + 0x11D8)) storeFloat(p + 0x11CC, 0.0f);  // duration: the game cures it
    if (const bool run = enabled(kFastRun); run != g_runApplied) {
        const float speed = loadFloat(p + 0x14DC);
        storeFloat(p + 0x14DC, run ? speed * kRunScale : speed / kRunScale);
        g_runApplied = run;
    }
    if (enabled(kPerfectStealth)) {
        // Mission stats in the profile: bonus flags, combat ms, kill and detection counts.
        // A detection clears bonus bits 0x1 (undetected) and 0x40 (which kills also clear).
        if (const uint32_t m = manager()) {
            const uint32_t stats = m + 0xA0 + 0x360;
            bool kills = load8(stats + 0xF) != 0;
            for (uint32_t i = 8; i < 0xE; ++i) kills |= load8(stats + i) != 0;
            store32(stats, load32(stats) | (kills ? 0x1 : 0x41));
            store32(stats + 4, 0);
            *memory->host<uint8_t>(stats + 0xE) = 0;
        }
    }
}

}  // namespace tz::cheats

using namespace tz;
using namespace tz::cheats;

// The damage routine every hit and poison tick goes through: r3 target, f1 damage, r5
// attacker (or the item/projectile, whose owner id is at +0xCC). The game itself turns some
// player hits into 10000 (stealth kills); one-hit kills do that for every positive hit.
TZ_OVERRIDE(822D8C20) {
    const uint32_t target = ctx.r3.u32;
    const double damage = ctx.f1.f64;
    if (target && (enabled(kInfiniteHealth) || enabled(kOneHitKills))) {
        if (playerType(target)) {
            if (enabled(kInfiniteHealth)) ctx.f1.f64 = 0.0;
        } else if (enabled(kOneHitKills) && ctx.f1.f64 > 0.0) {
            const uint32_t attacker = ctx.r5.u32, p = load32(0x82B58A58);
            const uint32_t type = attacker ? load32(attacker + 0xA0) : 0;
            const bool byPlayer = (type & 0x1000) || ((type & 0x10000) && p && load32(attacker + 0xCC) == load32(p + 0x9C));
            if (byPlayer) ctx.f1.f64 = 10000.0;
        }
    }
    TZ_TRACE("cheats", "[cheats] damage %.1f -> %.1f to %08X (type %X) by %08X (type %X)", damage, ctx.f1.f64, target,
             target ? load32(target + 0xA0) : 0, ctx.r5.u32, ctx.r5.u32 ? load32(ctx.r5.u32 + 0xA0) : 0);
    __imp__sub_822D8C20(ctx, base);
}

// An AI character's sight check (r3 AI): 0 = the player is not seen.
TZ_OVERRIDE(82309910) {
    if (enabled(kInvisible)) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__sub_82309910(ctx, base);
}

// AI think (r3 AI): a large negative contact timer (+0x1458) keeps bumping into it from
// alerting it; the game decays it back to 0 afterwards.
TZ_OVERRIDE(822F2988) {
    if (enabled(kInvisible) && ctx.r3.u32 && !playerType(ctx.r3.u32)) store32(ctx.r3.u32 + 0x1458, 0xCE6E6B28);  // -1e9
    __imp__sub_822F2988(ctx, base);
}

// Noise events (r3 position, r4 loudness...): no return value.
TZ_OVERRIDE(8228F3C0) {
    if (enabled(kSilent)) return;
    __imp__sub_8228F3C0(ctx, base);
}

// Using the selected item: returns its id after taking one from the carried list.
TZ_OVERRIDE(82363458) {
    if (enabled(kInfiniteItems)) {
        ctx.r3.u64 = load32(0x833A0EC4 + 64 * load32(0x8339BEF8));  // sub_8238BE38: the selected item
        return;
    }
    __imp__sub_82363458(ctx, base);
}

// The player's top run speed, stored by the callers at +0x14DC.
TZ_OVERRIDE(8233F240) {
    __imp__sub_8233F240(ctx, base);
    const bool run = enabled(kFastRun);
    if (run) ctx.f1.f64 *= kRunScale;
    g_runApplied = run;
}

// Spending money (r3 profile, r4 amount): the three shops.
TZ_OVERRIDE(8229A940) {
    if (enabled(kFreeShopping)) return;
    __imp__sub_8229A940(ctx, base);
}

// A mission's total score (also stored at 0x8340BF64); 900 is the top rank's threshold.
TZ_OVERRIDE(82402AF8) {
    __imp__sub_82402AF8(ctx, base);
    if (enabled(kBestRank) && int32_t(ctx.r3.u32) < 900) {
        ctx.r3.u64 = 900;
        store32(0x8340BF64, 900);
    }
}

// The grapple's ray length (1400, shared with AI sight) after its load in sub_82363860.
void GrappleRayLengthMidAsmHook(PPCRegister& f0) {
    if (enabled(kLongGrapple)) f0.f64 = 2800.0;
}
TZ_MIDASM_HOOK1(GrappleRayLengthMidAsmHook);

// The player's guard gauge after a blocked hit (sub_822DDF68: r31 the character, f0 the gauge
// minus the hit's guard damage; not above 0 breaks the guard).
void GuardBreakMidAsmHook(PPCRegister& f0, PPCRegister& r31) {
    if (enabled(kUnbreakableGuard) && playerType(r31.u32)) f0.f64 = 100.0;
}
TZ_MIDASM_HOOK2(GuardBreakMidAsmHook);

// Fall height above which the player lands with a stagger (600, shared constant).
void SoftLandingMidAsmHook(PPCRegister& f13) {
    if (enabled(kSoftLanding)) f13.f64 = 1e9;
}
TZ_MIDASM_HOOK1(SoftLandingMidAsmHook);
