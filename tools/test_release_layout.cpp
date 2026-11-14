#include "host/release_layout.h"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2 || !argv[1][0]) return 2;
    namespace fs = std::filesystem;
    const fs::path root = argv[1];
    if (fs::exists(root) && !fs::is_empty(root)) return 2;
    fs::create_directories(root / "tools");
    const auto error = [&](bool windows) { return tz::host::update::releaseLayoutError(root, windows); };
    if (error(true).find("TenchuZ.exe") == std::string::npos) return 1;
    std::ofstream(root / "TenchuZ.exe").put('x');
    if (error(true).find("tools/make_module.ps1") == std::string::npos) return 1;
    std::ofstream(root / "tools/make_module.ps1").put('x');
    if (!error(true).empty() || error(false).empty()) return 1;
    std::ofstream(root / "TenchuZ").put('x');
    if (error(false).find("tools/make_module.sh") == std::string::npos) return 1;
    std::ofstream(root / "tools/make_module.sh").put('x');
    fs::remove(root / "tools/make_module.ps1");
    if (!error(false).empty() || error(true).empty()) return 1;
    fs::create_directory(root / "tools/make_module.ps1");
    if (error(true).empty()) return 1;  // directories are not build scripts
    std::cout << "Windows and Linux release layout checks passed.\n";
}
