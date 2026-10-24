#pragma once
// Xbox 360 disc images (XDVDFS; plain, XGD2 and XGD3 layouts), read in place: the game's
// files are served straight from the .iso (tools/extract_iso.py is the same format).
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tz {

class DiscImage {
public:
    struct Entry {
        uint64_t offset = 0;  // byte offset in the image
        uint64_t size = 0;
        bool directory = false;
    };

    // Opens and indexes an image; null (with a reason in `error`) if it is not one.
    static std::shared_ptr<DiscImage> open(const std::filesystem::path& path, std::string& error);
    ~DiscImage();

    // Paths relative to the disc root, '/' or '\' separated, any case; "" is the root.
    const Entry* find(const std::string& path) const;
    // Reads up to `length` bytes of a file at `position`; returns the bytes read, or -1 when
    // the image cannot be read.
    int64_t read(const Entry& entry, uint64_t position, void* buffer, uint32_t length) const;
    bool readFile(const std::string& path, std::vector<uint8_t>& data) const;
    const std::filesystem::path& path() const { return path_; }
    FILETIME fileTime() const { return time_; }

private:
    DiscImage() = default;
    bool index(uint64_t base, uint32_t sector, uint32_t size, const std::string& prefix, int depth);
    bool readAt(uint64_t offset, void* buffer, uint32_t length) const;

    std::filesystem::path path_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    FILETIME time_{};
    uint64_t size_ = 0;
    std::map<std::string, Entry> entries_;  // lower-case '/' paths
};

}  // namespace tz
