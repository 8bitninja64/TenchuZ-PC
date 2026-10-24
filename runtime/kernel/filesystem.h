#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace tz {

class DiscImage;

// Guest device roots ("game:", "save:") resolve to host directories, or to a disc image
// read in place (read-only).
void mountDevice(const std::string& root, const std::filesystem::path& directory);
void mountDevice(const std::string& root, std::shared_ptr<DiscImage> disc);
void unmountDevice(const std::string& root);
// Returns false if the path names no known device or escapes its root.
bool resolveGuestPath(const std::string& guestPath, std::filesystem::path& hostPath);

}  // namespace tz
