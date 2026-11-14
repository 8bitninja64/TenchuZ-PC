// Build alongside runtime/host/settings.cpp in a dedicated scratch directory.
// Each invocation exercises one startup setting, saving, and restart-only behavior.
#include "host/settings.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main(int argc, char** argv) {
    using namespace tz::host;
    if (argc != 3) return 2;
    const auto path = std::filesystem::path(settingsFilePath());
    // This test writes only beside its dedicated test executable.
    { std::ofstream file(path); file << "; keep this comment\n[game]\nlanguage = " << argv[1] << '\n'; }
    const auto expected = uint32_t(std::stoul(argv[2]));
    check(uint32_t(settings().language) == expected, "language setting parsed incorrectly");
    check(gameLanguage() == expected, "incorrect Xbox startup language ID");
    saveSettings();
    {
        std::ifstream file(path);
        const std::string text((std::istreambuf_iterator<char>(file)), {});
        check(text.find("; keep this comment") != std::string::npos, "saving removed a comment");
        check(text.find(std::string("language = ") + languageOption(settings().language).code + "\n") != std::string::npos,
              "language not saved using its canonical code");
    }
    editableSettings().language = expected == 4 ? GameLanguage::German : GameLanguage::French;
    saveSettings();
    check(gameLanguage() == expected, "menu edit changed the running game's language");
    GameLanguage value = GameLanguage::French;
    check(!parseGameLanguage("unsupported", value) && value == GameLanguage::French,
          "invalid language changed an existing setting");
    std::cout << "Language settings passed: " << argv[1] << " -> " << expected << '\n';
}
