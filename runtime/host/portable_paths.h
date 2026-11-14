#pragma once

#include <filesystem>

namespace tz::host {

inline bool portableMode(const std::filesystem::path& executableDirectory) {
    std::error_code error;
    return std::filesystem::is_regular_file(executableDirectory / "portable.txt", error);
}

inline std::filesystem::path resolveDiscPath(const std::filesystem::path& saved,
                                             const std::filesystem::path& executableDirectory) {
    if (saved.empty() || saved.is_absolute()) return saved;
    return (executableDirectory / saved).lexically_normal();
}

inline std::filesystem::path rememberDiscPath(const std::filesystem::path& disc,
                                              const std::filesystem::path& executableDirectory) {
    const auto absolute = std::filesystem::absolute(disc).lexically_normal();
    if (portableMode(executableDirectory)) {
        const auto relative = absolute.lexically_relative(executableDirectory.lexically_normal());
        if (!relative.empty()) return relative;
    }
    // Different Windows drives cannot be represented by a relative path.
    return absolute;
}

}  // namespace tz::host
