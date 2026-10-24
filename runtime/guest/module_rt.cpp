// Compiled into the game module (tenchu_game.dll) with the generated code: the services
// the generated code calls, forwarded to the runtime, and the module's entry point.
#include "ppc_context.h"
#include "module_abi.h"

#include <cstdlib>

namespace {
const tz::RuntimeApi* g_api = nullptr;
}  // namespace

[[noreturn]] void PPCRecompFailure(const PPCContext& ctx, uint32_t address, const char* reason) {
    g_api->recompFailure(ctx, address, reason);
    std::abort();
}

uint64_t PPCQueryTimebase() { return g_api->queryTimebase(); }
uint32_t PPCMmioRead32(uint32_t address) { return g_api->mmioRead32(address); }
void PPCMmioWrite32(uint32_t address, uint32_t value) { g_api->mmioWrite32(address, value); }

#ifdef TENCHU_ENTRY_COUNTS
extern "C" uint32_t* volatile PPCEntryCounts = nullptr;
uint32_t* volatile* tz::moduleEntryCounts() { return &PPCEntryCounts; }
#else
uint32_t* volatile* tz::moduleEntryCounts() { return nullptr; }
#endif

extern "C" __declspec(dllexport) const tz::GameModule* tenchu_module_init(const tz::RuntimeApi* api) {
    if (!api || api->abiVersion != tz::kModuleAbiVersion || api->contextSize != sizeof(PPCContext)) return nullptr;
    g_api = api;
    return tz::builtinModule();
}
