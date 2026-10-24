#pragma once
// Host functions the game module calls, registered by name (see module_abi.h):
//   TZ_IMPORT(XamInputGetState) { ... }   a kernel/XAM import ("__imp__XamInputGetState")
//   TZ_OVERRIDE(823D4440) { ... }         replaces a translated game function ("sub_823D4440");
//                                         __imp__sub_823D4440(ctx, base) calls the original
//   TZ_MIDASM_HOOK1(Name) / TZ_MIDASM_HOOK2(Name)   after a mid-asm hook's definition
// The module names the overrides it forwards in config/overrides.txt and the hooks in
// config/tenchuz.toml.
#include "ppc_context.template.h"

#include <map>
#include <string>

namespace tz::host {

struct Function {
    void* address;
    PPCFunc** original;  // overrides: set to the translated function when the module loads
};

struct Registrar {
    Registrar(const char* name, void* address, PPCFunc** original = nullptr);
};

const std::map<std::string, Function>& registeredFunctions();

}  // namespace tz::host

#define TZ_IMPORT(name)                                                                          \
    PPC_FUNC(__imp__##name);                                                                     \
    static ::tz::host::Registrar tzRegister_##name("__imp__" #name, reinterpret_cast<void*>(&__imp__##name)); \
    PPC_FUNC(__imp__##name)

#define TZ_OVERRIDE(address)                                                                     \
    static PPCFunc* tzOriginal_##address = nullptr;                                              \
    static inline PPC_FUNC(__imp__sub_##address) { tzOriginal_##address(ctx, base); }           \
    PPC_FUNC(sub_##address);                                                                     \
    static ::tz::host::Registrar tzRegister_##address("sub_" #address,                          \
        reinterpret_cast<void*>(&sub_##address), &tzOriginal_##address);                         \
    PPC_FUNC(sub_##address)

#define TZ_MIDASM_HOOK1(name)                                                                    \
    static ::tz::host::Registrar tzRegister_##name(#name "/1",                                   \
        reinterpret_cast<void*>(static_cast<void (*)(PPCRegister&)>(&name)))
#define TZ_MIDASM_HOOK2(name)                                                                    \
    static ::tz::host::Registrar tzRegister_##name(#name "/2",                                   \
        reinterpret_cast<void*>(static_cast<void (*)(PPCRegister&, PPCRegister&)>(&name)))
