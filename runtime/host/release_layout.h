#pragma once
#include <filesystem>
#include <string>

namespace tz::host::update {
inline std::string releaseLayoutError(const std::filesystem::path& directory, bool windows) {
    const char* executable = windows ? "TenchuZ.exe" : "TenchuZ";
    const char* builder = windows ? "tools/make_module.ps1" : "tools/make_module.sh";
    for (const char* required : {executable, builder}) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(directory / required, error))
            return std::string("release files are missing (") + required +
                   "). Extract the entire release package and run the executable inside it";
    }
    return {};
}
}  // namespace tz::host::update
