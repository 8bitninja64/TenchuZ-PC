#include "xdvdfs.h"

#include <algorithm>
#include <cstring>

namespace tz {
namespace {

constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr uint32_t kSector = 2048;
// Game partition offsets for plain XDVDFS, XGD2, XGD1-style and XGD3 images.
constexpr uint64_t kPartitions[] = {0x0, 0xFD90000, 0x18300000, 0x2080000};
constexpr uint8_t kAttributeDirectory = 0x10;

std::string normalize(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(tolower(c)); });
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

}  // namespace

std::shared_ptr<DiscImage> DiscImage::open(const std::filesystem::path& path, std::string& error) {
    std::shared_ptr<DiscImage> disc(new DiscImage);
    disc->path_ = path;
    disc->handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (disc->handle_ == INVALID_HANDLE_VALUE) {
        error = "Cannot open the disc image";
        return nullptr;
    }
    GetFileTime(disc->handle_, nullptr, nullptr, &disc->time_);
    for (const uint64_t base : kPartitions) {
        char magic[sizeof(kMagic) - 1];
        uint32_t root[2];
        if (!disc->readAt(base + 32 * kSector, magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(magic)) != 0)
            continue;
        if (!disc->readAt(base + 32 * kSector + sizeof(magic), root, sizeof(root))) break;
        disc->entries_[""] = Entry{0, 0, true};
        if (!disc->index(base, root[0], root[1], "", 0)) break;
        return disc;
    }
    error = "This is not an Xbox 360 disc image";
    return nullptr;
}

DiscImage::~DiscImage() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

bool DiscImage::readAt(uint64_t offset, void* buffer, uint32_t length) const {
    OVERLAPPED overlapped{};
    overlapped.Offset = uint32_t(offset);
    overlapped.OffsetHigh = uint32_t(offset >> 32);
    DWORD read = 0;
    return ReadFile(handle_, buffer, length, &read, &overlapped) && read == length;
}

// A directory is a binary tree of entries: {left, right (in 4-byte units), start sector,
// size, attributes, name length, name}.
bool DiscImage::index(uint64_t base, uint32_t sector, uint32_t size, const std::string& prefix, int depth) {
    if (size == 0) return true;
    if (depth > 32 || size > (64u << 20)) return false;
    std::vector<uint8_t> table(size);
    if (!readAt(base + uint64_t(sector) * kSector, table.data(), size)) return false;
    std::vector<uint32_t> pending{0};
    std::vector<bool> seen(size / 4 + 1);
    while (!pending.empty()) {
        const uint32_t offset = pending.back();
        pending.pop_back();
        if (offset + 14 > size || seen[offset / 4]) continue;
        seen[offset / 4] = true;
        uint16_t left, right;
        uint32_t start, length;
        std::memcpy(&left, &table[offset], 2);
        std::memcpy(&right, &table[offset + 2], 2);
        std::memcpy(&start, &table[offset + 4], 4);
        std::memcpy(&length, &table[offset + 8], 4);
        const uint8_t attributes = table[offset + 12], nameLength = table[offset + 13];
        if (left == 0xFFFF || offset + 14 + nameLength > size) continue;
        if (left) pending.push_back(uint32_t(left) * 4);
        if (right) pending.push_back(uint32_t(right) * 4);
        const std::string name(reinterpret_cast<const char*>(&table[offset + 14]), nameLength);
        const std::string path = normalize(prefix.empty() ? name : prefix + "/" + name);
        const uint64_t position = base + uint64_t(start) * kSector;
        if (attributes & kAttributeDirectory) {
            entries_[path] = Entry{position, 0, true};
            if (!index(base, start, length, path, depth + 1)) return false;
        } else {
            entries_[path] = Entry{position, length, false};
        }
    }
    return true;
}

const DiscImage::Entry* DiscImage::find(const std::string& path) const {
    const auto found = entries_.find(normalize(path));
    return found == entries_.end() ? nullptr : &found->second;
}

uint32_t DiscImage::read(const Entry& entry, uint64_t position, void* buffer, uint32_t length) const {
    if (entry.directory || position >= entry.size) return 0;
    const uint32_t count = uint32_t((std::min)(uint64_t(length), entry.size - position));
    OVERLAPPED overlapped{};
    const uint64_t offset = entry.offset + position;
    overlapped.Offset = uint32_t(offset);
    overlapped.OffsetHigh = uint32_t(offset >> 32);
    DWORD read = 0;
    ReadFile(handle_, buffer, count, &read, &overlapped);  // positioned reads are thread-safe
    return read;
}

bool DiscImage::readFile(const std::string& path, std::vector<uint8_t>& data) const {
    const Entry* entry = find(path);
    if (!entry || entry->directory || entry->size > (1ull << 31)) return false;
    data.resize(size_t(entry->size));
    return read(*entry, 0, data.data(), uint32_t(data.size())) == data.size();
}

}  // namespace tz
