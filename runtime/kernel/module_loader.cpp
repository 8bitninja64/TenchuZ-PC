#include "module_loader.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

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

const GameModule* loadGameModule(const std::filesystem::path& path, std::string& error,
                                 const std::string& xexSha256) {
    if (boundModule) return boundModule;
#ifdef _WIN32
    HMODULE library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) {
        error = "cannot load " + utf8(path) + " (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    auto release = [library] { FreeLibrary(library); };
    auto init = reinterpret_cast<ModuleInitFn>(GetProcAddress(library, kModuleInitSymbol));
#else
    void* library = dlopen(utf8(path).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const char* why = dlerror();
        error = "cannot load " + utf8(path) + " (" + (why ? why : "unknown error") + ")";
        return nullptr;
    }
    auto release = [library] { dlclose(library); };
    auto init = reinterpret_cast<ModuleInitFn>(dlsym(library, kModuleInitSymbol));
#endif
    auto fail = [&](std::string why) -> const GameModule* {
        release();
        error = utf8(path) + ": " + why;
        return nullptr;
    };
    if (!init) return fail("not a Tenchu Z game module");
    const GameModule* module = init(&kApi);
    if (!module || module->abiVersion != kModuleAbiVersion || module->contextSize != sizeof(PPCContext))
        return fail("built for a different version of the runtime");
    if (module->imageBase != PPC_IMAGE_BASE || module->imageSize != PPC_IMAGE_SIZE ||
        module->codeBase != PPC_CODE_BASE || module->codeSize != PPC_CODE_SIZE)
        return fail("image layout " + hex(module->imageBase) + "+" + hex(module->imageSize) + " / code " +
                    hex(module->codeBase) + "+" + hex(module->codeSize) + " differs from the runtime's");
    if (!xexSha256.empty() && xexSha256 != module->xexSha256)
        return fail("translated from another default.xex (" + std::string(module->xexSha256) + ")");

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
    logf("[module] %s: %u functions, %u host functions, built from default.xex %.16s...", utf8(path).c_str(),
         module->functionCount, module->hostCount, module->xexSha256);
    return module;
}

}  // namespace tz
