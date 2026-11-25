#pragma once

#include <format>
#include <span>
#include <string>
#include <string_view>

enum class Language { English, SimplifiedChinese, Spanish };
enum class LanguageSetting { Auto, English, SimplifiedChinese, Spanish };

enum class Text {
#define GE_TEXT(id, en, zh, es) id,
#include "translations.inc"
#undef GE_TEXT
    Count
};

Language selectLanguage(std::span<const std::wstring_view> preferences);
Language resolveLanguage(LanguageSetting setting);
std::string_view languageCode(LanguageSetting setting);
LanguageSetting parseLanguageCode(std::string_view code);
std::wstring_view nativeName(Language language);

class Localizer {
public:
    explicit Localizer(Language language = Language::English) : current(language) {}

    Language language() const { return current; }
    std::wstring_view get(Text id) const;

    template <typename... Args>
    std::wstring format(Text id, const Args&... args) const {
        return std::vformat(get(id), std::make_wformat_args(args...));
    }

    std::wstring sampleRate(int hz) const;
    std::wstring pValue(double value) const;

private:
    Language current;
};
