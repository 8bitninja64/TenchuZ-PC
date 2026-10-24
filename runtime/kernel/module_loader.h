#pragma once
// Loads the game module (tenchu_game.dll) and binds it to the runtime (guest/module_abi.h).
#include <filesystem>
#include <string>

#include "guest/ppc_context.template.h"
#include "guest/module_abi.h"

namespace tz {

// Loads, validates and binds a module translated from the default.xex with SHA-256
// `xexSha256` (any, if empty); on failure returns null and describes why in `error`. A
// module that was bound stays loaded for the life of the process.
const GameModule* loadGameModule(const std::filesystem::path& path, std::string& error,
                                 const std::string& xexSha256 = {});
// The bound module (null before loadGameModule succeeds).
const GameModule* gameModule();

}  // namespace tz
