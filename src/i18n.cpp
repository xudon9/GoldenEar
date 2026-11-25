#include "i18n.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <vector>

namespace {

constexpr std::wstring_view strings[][3] {
#define GE_TEXT(id, en, zh, es) {en, zh, es},
#include "translations.inc"
#undef GE_TEXT
};
static_assert(std::size(strings) == static_cast<std::size_t>(Text::Count));

constexpr std::array<std::string_view, 4> codes {"auto", "en", "zh-Hans", "es"};
constexpr std::array<std::wstring_view, 3> names {L"English", L"简体中文", L"Español"};

bool equals(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

Language selectLanguage(std::span<const std::wstring_view> preferences) {
    for (const auto tag : preferences) {
        const auto separator = tag.find(L'-');
        const auto base = tag.substr(0, separator);
        if (equals(base, L"en")) return Language::English;
        if (equals(base, L"es")) return Language::Spanish;
        if (equals(base, L"zh")) {
            const auto suffix = separator == tag.npos ? std::wstring_view{} : tag.substr(separator + 1);
            const auto first = suffix.substr(0, suffix.find(L'-'));
            if (first.empty() || equals(first, L"Hans") || equals(first, L"CN") || equals(first, L"SG")) {
                return Language::SimplifiedChinese;
            }
        }
    }
    return Language::English;
}

Language resolveLanguage(LanguageSetting setting) {
    switch (setting) {
    case LanguageSetting::English: return Language::English;
    case LanguageSetting::SimplifiedChinese: return Language::SimplifiedChinese;
    case LanguageSetting::Spanish: return Language::Spanish;
    case LanguageSetting::Auto: break;
    }
    ULONG count = 0;
    ULONG size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0) return Language::English;
    std::vector<wchar_t> buffer(size);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)) return Language::English;
    std::vector<std::wstring_view> preferences;
    preferences.reserve(count);
    for (const wchar_t* tag = buffer.data(); *tag;) {
        const std::wstring_view name(tag);
        preferences.push_back(name);
        tag += name.size() + 1;
    }
    return selectLanguage(preferences);
}

std::string_view languageCode(LanguageSetting setting) {
    return codes[static_cast<std::size_t>(setting)];
}

LanguageSetting parseLanguageCode(std::string_view code) {
    const auto match = std::find(codes.begin(), codes.end(), code);
    return match == codes.end() ? LanguageSetting::Auto : static_cast<LanguageSetting>(match - codes.begin());
}

std::wstring_view nativeName(Language language) {
    return names[static_cast<std::size_t>(language)];
}

std::wstring_view Localizer::get(Text id) const {
    return strings[static_cast<std::size_t>(id)][static_cast<std::size_t>(current)];
}

std::wstring Localizer::sampleRate(int hz) const {
    std::wstring value = std::to_wstring(hz / 1000);
    if (const int fraction = hz % 1000; fraction != 0) {
        value += current == Language::Spanish ? L',' : L'.';
        auto digits = std::to_wstring(1000 + fraction).substr(1);
        digits.erase(digits.find_last_not_of(L'0') + 1);
        value += digits;
    }
    return value + L" kHz";
}

std::wstring Localizer::pValue(double value) const {
    auto label = value < 0.001 ? std::wstring(L"p < 0.001") : value > 0.05 && value < 0.0505 ?
        std::wstring(L"p > 0.050") : std::format(L"p = {:.3f}", value);
    if (current == Language::Spanish) std::replace(label.begin(), label.end(), L'.', L',');
    return label;
}
