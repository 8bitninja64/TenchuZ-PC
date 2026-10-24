#include "module_loader.h"

#include <windows.h>

#include <set>

#include "guest/host.h"
#include "kernel.h"

namespace tz {
namespace {

const GameModule* boundModule = nullptr;

const RuntimeApi kApi = {kModuleAbiVersion, uint32_t(sizeof(PPCContext)), &PPCRecompFailure, &PPCQueryTimebase,
                         &PPCMmioRead32, &PPCMmioWrite32};

std::string hex(uint64_t value) {
    char text[24];
    snprintf(text, sizeof(text), "0x%llX", (unsigned long long)value);
    return text;
}

}  // namespace

const GameModule* gameModule() { return boundModule; }

const GameModule* loadGameModule(const std::filesystem::path& path, std::string& error) {
    if (boundModule) return boundModule;
    HMODULE library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) {
        error = "cannot load " + path.string() + " (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    auto fail = [&](std::string why) -> const GameModule* {
        FreeLibrary(library);
        error = path.string() + ": " + why;
        return nullptr;
    };
    auto init = reinterpret_cast<ModuleInitFn>(GetProcAddress(library, kModuleInitSymbol));
    if (!init) return fail("not a Tenchu Z game module");
    const GameModule* module = init(&kApi);
    if (!module || module->abiVersion != kModuleAbiVersion || module->contextSize != sizeof(PPCContext))
        return fail("built for a different version of the runtime");
    if (module->imageBase != PPC_IMAGE_BASE || module->imageSize != PPC_IMAGE_SIZE ||
        module->codeBase != PPC_CODE_BASE || module->codeSize != PPC_CODE_SIZE)
        return fail("image layout " + hex(module->imageBase) + "+" + hex(module->imageSize) + " / code " +
                    hex(module->codeBase) + "+" + hex(module->codeSize) + " differs from the runtime's");

    // Bind every host function the module calls before any guest code runs.
    const auto& registered = host::registeredFunctions();
    std::string missing;
    for (uint32_t i = 0; i < module->hostCount; ++i) {
        const auto found = registered.find(module->hostNames[i]);
        if (found == registered.end()) {
            missing += missing.empty() ? "" : ", ";
            missing += module->hostNames[i];
        }
    }
    if (!missing.empty()) return fail("the runtime lacks " + missing);
    std::set<std::string> requested;
    for (uint32_t i = 0; i < module->hostCount; ++i) {
        const auto& function = registered.at(module->hostNames[i]);
        module->hostSlots[i] = function.address;
        if (function.original) *function.original = module->originals[i];
        requested.insert(module->hostNames[i]);
    }
    for (const auto& [name, function] : registered)
        if (!requested.count(name))
            logf("[module] %s is registered but the module does not call it (config/overrides.txt?)", name.c_str());

    boundModule = module;
    logf("[module] %s: %u functions, %u host functions, built from default.xex %.16s...", path.string().c_str(),
         module->functionCount, module->hostCount, module->xexSha256);
    return module;
}

}  // namespace tz
