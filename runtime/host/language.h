#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tz::host {

// Xbox system language IDs supported by the USA/Europe disc.
enum class GameLanguage : uint32_t { English = 1, German = 3, French = 4, Spanish = 5, Italian = 6 };
struct LanguageOption { GameLanguage value; const char* code; const char* name; };
inline constexpr LanguageOption kLanguages[] = {
    {GameLanguage::English, "en", "English"}, {GameLanguage::French, "fr", "French"},
    {GameLanguage::German, "de", "German"}, {GameLanguage::Spanish, "es", "Spanish"},
    {GameLanguage::Italian, "it", "Italian"},
};

inline std::string languageLower(std::string_view text) {
    std::string result(text);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

inline bool parseGameLanguage(std::string_view text, GameLanguage& result) {
    const auto value = languageLower(text);
    for (const auto& option : kLanguages) {
        if (value == option.code || value == languageLower(option.name)) {
            result = option.value;
            return true;
        }
    }
    return false;  // Keep the existing/default value for unsupported languages.
}

inline const LanguageOption& languageOption(GameLanguage value) {
    for (const auto& option : kLanguages) if (option.value == value) return option;
    return kLanguages[0];
}

}  // namespace tz::host
