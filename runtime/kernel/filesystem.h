#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace tz {

// Guest device roots ("game:", "save:") resolve to host directories.
void mountDevice(const std::string& root, const std::filesystem::path& directory);
void unmountDevice(const std::string& root);
// Returns false if the path names no known device or escapes its root.
bool resolveGuestPath(const std::string& guestPath, std::filesystem::path& hostPath);

}  // namespace tz
