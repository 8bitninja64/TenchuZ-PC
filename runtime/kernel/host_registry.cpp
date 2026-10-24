// Host functions registered by name for the game module (runtime/guest/host.h).
#include "guest/host.h"

#include <cstdio>
#include <cstdlib>

namespace tz::host {
namespace {
std::map<std::string, Function>& functions() {
    static std::map<std::string, Function> map;  // filled by static constructors
    return map;
}
}  // namespace

Registrar::Registrar(const char* name, void* address, PPCFunc** original) {
    if (!functions().emplace(name, Function{address, original}).second) {
        std::fprintf(stderr, "host function %s registered twice\n", name);
        std::abort();
    }
}

const std::map<std::string, Function>& registeredFunctions() { return functions(); }

}  // namespace tz::host
