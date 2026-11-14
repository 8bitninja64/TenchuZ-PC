// clang-cl /EHsc /std:c++20 /Iruntime tools/test_portable_paths.cpp
// /Febuild/test_portable_paths.exe /Fobuild/test_portable_paths.obj
#include "host/portable_paths.h"
#include <cstdlib>
#include <fstream>
#include <iostream>

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    using namespace tz::host;
    if (argc != 2) return 2;  // caller supplies a fresh scratch directory
    const auto root = fs::absolute(argv[1]);
    const auto original = root / "original [game]";
    const auto moved = root / "moved [game]";
    fs::create_directories(original);
    check(!portableMode(original), "normal install remains nonportable");
    const auto disc = original / "disc/game.iso";
    check(rememberDiscPath(disc, original) == disc, "normal install stores absolute path");
    std::ofstream(original / "portable.txt") << "";
    check(portableMode(original), "marker enables portable mode");
    const auto saved = rememberDiscPath(disc, original);
    check(saved == fs::path("disc/game.iso"), "portable disc is relative");
    check(resolveDiscPath(saved, moved) == moved / "disc/game.iso", "moved installation resolves disc locally");
    check(resolveDiscPath({}, moved).empty(), "empty setting remains empty");
    check(resolveDiscPath(disc, moved) == disc, "absolute settings remain valid");
    const auto sibling = root / "discs/game.iso";
    check(resolveDiscPath(rememberDiscPath(sibling, original), moved) == sibling,
          "disc beside installation survives directory move");
#ifdef _WIN32
    check(rememberDiscPath("Z:/game.iso", original).is_absolute(), "different drive stays absolute");
    check(resolveDiscPath(saved, "Z:/Portable") == fs::path("Z:/Portable/disc/game.iso"),
          "drive-letter change retains disc path");
#endif
    fs::create_directories(root / "directory-marker/portable.txt");
    check(!portableMode(root / "directory-marker"), "marker must be a file");
    std::cout << "Portable path tests passed.\n";
}
