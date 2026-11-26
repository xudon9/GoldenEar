#include "audio.hpp"
#include "i18n.hpp"
#include "encoded_audio_cache.hpp"
#include "resource.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <bit>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <future>
#include <iomanip>
#include <optional>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace {

constexpr int width = 1180;
constexpr int height = 740;

struct Palette {
    COLORREF background, surface, raised, hovered, line;
    COLORREF textColor, muted, disabled, accent, successColor, errorColor;
    COLORREF primary, primaryHover, primaryText;
    COLORREF selected, selectedHover, selectedBorder, selectedText;
    COLORREF waveform, clipWaveform;
};

constexpr Palette lightColors {
    RGB(243, 243, 243), RGB(253, 253, 253), RGB(248, 248, 248), RGB(235, 235, 235), RGB(208, 208, 208),
    RGB(26, 26, 26), RGB(82, 82, 82), RGB(113, 113, 113), RGB(0, 90, 158), RGB(16, 112, 61), RGB(161, 38, 26),
    RGB(0, 103, 192), RGB(0, 86, 160), RGB(255, 255, 255),
    RGB(229, 241, 251), RGB(217, 234, 248), RGB(97, 149, 189), RGB(0, 76, 135),
    RGB(109, 109, 109), RGB(59, 115, 158)
};

constexpr Palette darkColors {
    RGB(32, 32, 32), RGB(39, 39, 39), RGB(48, 48, 48), RGB(60, 60, 60), RGB(80, 80, 80),
    RGB(246, 246, 246), RGB(187, 187, 187), RGB(139, 139, 139), RGB(153, 215, 255), RGB(128, 222, 153), RGB(255, 177, 158),
    RGB(153, 215, 255), RGB(177, 225, 255), RGB(0, 42, 69),
    RGB(27, 58, 81), RGB(34, 71, 98), RGB(113, 167, 206), RGB(173, 222, 255),
    RGB(140, 140, 140), RGB(117, 177, 218)
};

Palette contrastColors() {
    const auto window = GetSysColor(COLOR_WINDOW);
    const auto text = GetSysColor(COLOR_WINDOWTEXT);
    const auto face = GetSysColor(COLOR_BTNFACE);
    const auto highlight = GetSysColor(COLOR_HIGHLIGHT);
    const auto highlightedText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    return {window, window, face, face, GetSysColor(COLOR_BTNTEXT),
            text, text, GetSysColor(COLOR_GRAYTEXT), text, text, text,
            highlight, highlight, highlightedText,
            highlight, highlight, text, highlightedText, text, highlightedText};
}

bool windowsUsesDarkMode() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS && light == 0;
}

constexpr RECT rect(int x, int y, int w, int h) { return {x, y, x + w, y + h}; }

enum Shortcut { HearA, HearB, HearX, ToggleAB, Play, Back, Forward, AnswerA, AnswerB, KeyCount };
constexpr std::array<int, KeyCount> defaultKeys {'J', 'K', 'L', VK_OEM_1, VK_SPACE, VK_OEM_COMMA, VK_OEM_PERIOD, '1', '2'};
constexpr std::array<Text, KeyCount> shortcutNames {
    Text::ListenA, Text::ListenB, Text::ListenX, Text::ToggleAB, Text::PlayPause,
    Text::SeekBack, Text::SeekForward, Text::AnswerA, Text::AnswerB
};
constexpr std::array modeNames {Text::Abx, Text::Identify, Text::Preference};

enum Control {
    Library = 1, Preferences, Import, Start, Finish, Skip, ChooseSong, Next, Retry, Seek,
    PlayPause, Rewind, FastForward, SelectA, SelectB, SelectX, Toggle, SubmitA, SubmitB,
    PreviousPage, NextPage, ClosePanel, ResetKeys, ToggleBundled, MarkLoop, ClearLoop, NextAndPlay, TogglePractice,
    PreferenceBase = 30, ModeBase = 40, DifficultyBase = 50, KeyBase = 70,
    ThemeBase = 80, LanguageBase = 90, RemoveBase = 100,
    RestartOnFinishOption = 110, ShuffleTracksOption, BackToSetup, ClearEncodedAudioCache, RandomSong, PickTrackBase = 120,
    ProfileBase = 140, CodecBase = 160, LadderTrialCount = 170
};

enum class Panel { Main, Library, Preferences };
enum class PreferencePage { Appearance, Language, Shortcuts, Session };
constexpr std::array preferencePages {Text::Appearance, Text::Language, Text::Shortcuts, Text::SessionOptions};
constexpr std::array<std::wstring_view, codecs.size()> codecNames {L"MP3", L"AAC", L"LDAC", L"aptX HD", L"SBC"};
constexpr std::array scenarioNames {Text::AacEveryday, Text::AacHigherRate, Text::LdacMetro, Text::LdacCafe,
    Text::LdacStudy, Text::AptxHome, Text::SbcBasic, Text::SbcHigh};

std::wstring_view codecName(Codec codec) { return codecNames[static_cast<std::size_t>(codec)]; }
enum class ThemeSetting { System, Light, Dark };
enum class Font { Small, Body, Bold, Title, Hero, Sample, Mono, Chinese, Icon, Count };
enum class Style { Normal, Primary, Selected, Quiet };
enum class TransportIcon { Play, Pause, Rewind, FastForward };
// These glyphs are shared by Segoe Fluent Icons and Segoe MDL2 Assets.
enum class ActionIcon : wchar_t {
    Settings = L'\uE713', Repertoire = L'\uE90B', Import = L'\uE8B5', Play = L'\uE768', Next = L'\uE72A', Back = L'\uE72B',
    Delete = L'\uE74D', Shuffle = L'\uE8B1'
};
enum class ControlRole { Action, Checkbox, Radio };

struct Hit {
    int id;
    RECT area;
    ControlRole role = ControlRole::Action;
    bool checked = false;
    int group = 0;
};

HFONT createIconFont(const wchar_t* family) {
    const auto dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    const auto font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
    wchar_t actualFamily[LF_FACESIZE] {};
    const bool available = font && SelectObject(dc, font) &&
        GetTextFaceW(dc, static_cast<int>(std::size(actualFamily)), actualFamily) && lstrcmpiW(family, actualFamily) == 0;
    DeleteDC(dc);
    if (available) return font;
    if (font) DeleteObject(font);
    return nullptr;
}

std::wstring keyName(int key, const Localizer& locale) {
    if (key == VK_SPACE) return std::wstring(locale.get(Text::SpaceKey));
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, static_cast<wchar_t>(key));
    const UINT character = MapVirtualKeyW(key, MAPVK_VK_TO_CHAR) & 0xffff;
    if (character >= L' ' && character != 127 && key != VK_DELETE) return std::wstring(1, static_cast<wchar_t>(character));
    wchar_t name[40] {};
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const LONG packed = static_cast<LONG>((scan & 0xff) << 16) | ((scan & 0xff00) ? (1 << 24) : 0);
    if (GetKeyNameTextW(packed, name, static_cast<int>(std::size(name)))) return name;
    return locale.format(Text::KeyCode, key);
}

bool bindable(int key) {
    switch (key) {
    case VK_TAB: case VK_RETURN: case VK_ESCAPE:
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
        return false;
    default:
        return key >= VK_BACK && key <= 254 && MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX) != 0;
    }
}

wchar_t sideName(int side) { return L"ABX"[side]; }

std::wstring clockText(std::size_t frames, int rate, bool precise = false) {
    const auto seconds = frames / rate;
    auto label = std::to_wstring(seconds / 60) + L":" + (seconds % 60 < 10 ? L"0" : L"") + std::to_wstring(seconds % 60);
    if (precise) {
        const auto hundredths = frames % rate * 100 / rate;
        label.append(hundredths < 10 ? L".0" : L".").append(std::to_wstring(hundredths));
    }
    return label;
}

std::wstring percent(unsigned hits, unsigned total) {
    return std::to_wstring(static_cast<unsigned>(std::lround(100.0 * hits / total))) + L"%";
}

Text audioMessage(AudioIssue issue) {
    switch (issue) {
    case AudioIssue::Cancelled: return Text::AudioCancelled;
    case AudioIssue::PipeCreate: return Text::AudioPipeCreate;
    case AudioIssue::PipeConfigure: return Text::AudioPipeConfigure;
    case AudioIssue::FileHandles: return Text::AudioFileHandles;
    case AudioIssue::FfmpegUnavailable: return Text::FfmpegUnavailable;
    case AudioIssue::FfmpegLaunchFailed: return Text::FfmpegLaunchFailed;
    case AudioIssue::FfmpegFailed: return Text::FfmpegFailed;
    case AudioIssue::LdacFailed: return Text::LdacFailed;
    case AudioIssue::Incomplete: return Text::IncompleteAudio;
    case AudioIssue::TrackMissing: return Text::TrackMissing;
    case AudioIssue::TrackTooLong: return Text::TrackTooLong;
    case AudioIssue::Empty: return Text::NoAudio;
    case AudioIssue::Memory: return Text::AudioMemory;
    case AudioIssue::Io: return Text::AudioIo;
    case AudioIssue::OutputUnavailable: return Text::OutputUnavailable;
    case AudioIssue::OutputStopped: return Text::OutputStopped;
    case AudioIssue::None: return Text::UnexpectedError;
    }
    return Text::UnexpectedError;
}

struct BundledTrack {
    fs::path path;
    std::wstring_view title;
    std::wstring_view performer;
};

std::vector<BundledTrack> loadBundledTracks() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return {};
    executable.resize(length);
    const auto directory = fs::path(executable).parent_path() / L"tracks";

    std::vector<BundledTrack> tracks;
    const auto add = [&](const wchar_t* filename, std::wstring_view title, std::wstring_view performer) {
        auto path = directory / filename;
        std::error_code error;
        if (fs::is_regular_file(path, error)) tracks.push_back({std::move(path), title, performer});
    };
    add(L"goldberg-aria.flac", L"Bach — Goldberg Variations, Aria", L"Kimiko Ishizaka");
    add(L"goldberg-variatio-1.flac", L"Bach — Goldberg Variations, Variation 1", L"Kimiko Ishizaka");
    add(L"beethoven-op18-6-i.flac", L"Beethoven — String Quartet No. 6, I", L"Musopen String Quartet");
    return tracks;
}

struct Settings {
    std::vector<fs::path> tracks;
    std::array<int, KeyCount> keys = defaultKeys;
    Mode mode = Mode::Abx;
    Difficulty difficulty = Difficulty::Random;
    unsigned trialsPerLevel = Session::defaultTrialsPerLevel;
    Codec codec = Codec::Mp3;
    unsigned enabled = (1U << profiles.size()) - 1;
    LanguageSetting language = LanguageSetting::Auto;
    ThemeSetting theme = ThemeSetting::System;
    bool includeBundled = true;
    bool practice = false;
    bool restartOnFinish = true;
    bool shuffleTracks = true;
    fs::path file;

    void load() {
        if (const DWORD length = GetEnvironmentVariableW(L"GOLDEN_EAR_HOME", nullptr, 0); length) {
            std::wstring directory(length, L'\0');
            const DWORD copied = GetEnvironmentVariableW(L"GOLDEN_EAR_HOME", directory.data(), length);
            if (copied && copied < length) file = fs::path(directory.substr(0, copied)) / L"settings.txt";
        }
        if (file.empty()) {
            PWSTR local = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
                file = fs::path(local) / L"GoldenEar" / L"settings.txt";
            }
            CoTaskMemFree(local);
        }
        if (file.empty()) return;
        std::ifstream in(file);
        std::string version;
        if (!(in >> version)) return;
        const bool hasTrialsPerLevel = version == "GoldenEar8";
        const bool hasCodecs = hasTrialsPerLevel || version == "GoldenEar7";
        const bool hasSessionOptions = hasCodecs || version == "GoldenEar6";
        const bool hasPractice = hasSessionOptions || version == "GoldenEar5";
        const bool hasTheme = hasPractice || version == "GoldenEar4";
        if (!hasTheme && version != "GoldenEar3" && version != "GoldenEar2" && version != "GoldenEar1") return;
        int savedMode = 0;
        int savedDifficulty = 0;
        unsigned savedEnabled = 0;
        if (!(in >> savedMode >> savedDifficulty >> savedEnabled)) return;
        if (savedMode >= 0 && savedMode <= 2) mode = static_cast<Mode>(savedMode);
        if (savedDifficulty >= 0 && savedDifficulty <= 1) difficulty = static_cast<Difficulty>(savedDifficulty);
        for (const Codec candidate : codecs) {
            if (!hasCodecs && candidate != Codec::Mp3) break;
            const unsigned mask = profileMask(candidate);
            if (savedEnabled & mask) enabled = (enabled & ~mask) | (savedEnabled & mask);
        }
        if (version != "GoldenEar1") {
            std::string code;
            if (!(in >> code)) return;
            language = parseLanguageCode(code);
        }
        if (hasTheme || version == "GoldenEar3") {
            int savedIncludeBundled = 1;
            if (!(in >> savedIncludeBundled)) return;
            includeBundled = savedIncludeBundled != 0;
        }
        if (hasTheme) {
            int savedTheme = 0;
            if (!(in >> savedTheme)) return;
            if (savedTheme >= 0 && savedTheme <= static_cast<int>(ThemeSetting::Dark)) theme = static_cast<ThemeSetting>(savedTheme);
        }
        if (hasPractice && !(in >> practice)) return;
        if (hasSessionOptions && !(in >> restartOnFinish >> shuffleTracks)) return;
        if (hasCodecs) {
            int savedCodec = 0;
            if (!(in >> savedCodec)) return;
            if (savedCodec >= 0 && savedCodec < static_cast<int>(codecs.size())) codec = codecs[savedCodec];
        }
        if (hasTrialsPerLevel) {
            unsigned savedTrialsPerLevel = 0;
            if (!(in >> savedTrialsPerLevel)) return;
            if (savedTrialsPerLevel > 0 && savedTrialsPerLevel <= Session::maxTrialsPerLevel) {
                trialsPerLevel = savedTrialsPerLevel;
            }
        }
        auto savedKeys = keys;
        std::vector<int> read;
        for (auto& key : savedKeys) {
            if (!(in >> key)) return;
            if (bindable(key) && std::find(read.begin(), read.end(), key) == read.end()) read.push_back(key);
        }
        if (read.size() == keys.size()) keys = savedKeys;
        for (std::string path; in >> std::quoted(path);) {
            try {
                tracks.emplace_back(std::u8string(path.begin(), path.end()));
            } catch (const fs::filesystem_error&) {
                // A malformed path should not prevent the rest of the repertoire from loading.
            }
        }
    }

    bool save() const {
        if (file.empty()) return false;
        std::error_code error;
        fs::create_directories(file.parent_path(), error);
        if (error) return false;
        auto temp = file;
        temp += L".new";
        std::ofstream out(temp, std::ios::trunc);
        out << "GoldenEar8\n" << static_cast<int>(mode) << ' ' << static_cast<int>(difficulty) << ' ' << enabled
            << ' ' << languageCode(language) << ' ' << includeBundled << ' ' << static_cast<int>(theme) << ' ' << practice
            << ' ' << restartOnFinish << ' ' << shuffleTracks << ' ' << static_cast<int>(codec) << ' ' << trialsPerLevel << '\n';
        for (int key : keys) out << key << ' ';
        out << '\n';
        for (const auto& path : tracks) {
            const auto utf8 = path.u8string();
            out << std::quoted(std::string(utf8.begin(), utf8.end())) << '\n';
        }
        out.close();
        return out.good() && MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
};

struct Layout {
    int x;
    int y;
    int w;
    int h;

    static Layout forWindow(HWND window) {
        RECT area {};
        GetClientRect(window, &area);
        const double scale = std::min(static_cast<double>(area.right) / width, static_cast<double>(area.bottom) / height);
        const int w = std::max(1, static_cast<int>(width * scale));
        const int h = std::max(1, static_cast<int>(height * scale));
        return {(area.right - w) / 2, (area.bottom - h) / 2, w, h};
    }

    POINT point(int px, int py) const {
        return {static_cast<LONG>((px - x) * static_cast<double>(width) / w),
                static_cast<LONG>((py - y) * static_cast<double>(height) / h)};
    }
};

class Canvas {
public:
    Canvas(HDC dc, const std::array<HFONT, static_cast<std::size_t>(Font::Count)>& fonts, const Palette& colors)
        : dc(dc), fonts(fonts), colors(colors) {
        SetBkMode(dc, TRANSPARENT);
    }

    void box(RECT area, COLORREF fill, COLORREF border, int radius = 12) const {
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, border);
        RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    }

    void ellipse(RECT area, COLORREF fill, COLORREF border) const {
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, border);
        Ellipse(dc, area.left, area.top, area.right, area.bottom);
    }

    void stroke(int x1, int y1, int x2, int y2, COLORREF color) const {
        SetDCPenColor(dc, color);
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
    }

    void check(int x, int y, COLORREF ink) const {
        SetDCPenColor(dc, ink);
        const auto pen = CreatePen(PS_SOLID, 2, ink);
        const auto previous = pen ? SelectObject(dc, pen) : nullptr;
        const POINT points[] {{x - 5, y}, {x - 1, y + 4}, {x + 6, y - 4}};
        Polyline(dc, points, static_cast<int>(std::size(points)));
        if (pen) {
            SelectObject(dc, previous);
            DeleteObject(pen);
        }
    }

    void answerMark(RECT area, bool correct) const {
        const auto ink = correct ? colors.successColor : colors.errorColor;
        box(area, colors.surface, ink, 16);
        const int x = (area.left + area.right) / 2;
        const int y = (area.top + area.bottom) / 2;
        const auto pen = CreatePen(PS_SOLID, 3, ink);
        const auto previous = pen ? SelectObject(dc, pen) : nullptr;
        if (correct) {
            const POINT check[] {{x - 7, y}, {x - 2, y + 5}, {x + 7, y - 5}};
            Polyline(dc, check, static_cast<int>(std::size(check)));
        } else {
            const POINT down[] {{x - 5, y - 5}, {x + 5, y + 5}};
            const POINT up[] {{x - 5, y + 5}, {x + 5, y - 5}};
            Polyline(dc, down, static_cast<int>(std::size(down)));
            Polyline(dc, up, static_cast<int>(std::size(up)));
        }
        if (pen) {
            SelectObject(dc, previous);
            DeleteObject(pen);
        }
    }

    void transportIcon(TransportIcon icon, RECT area, COLORREF color) const {
        const int x = (area.left + area.right) / 2;
        const int y = (area.top + area.bottom) / 2;
        if (icon == TransportIcon::Pause) {
            box(rect(x - 7, y - 9, 5, 18), color, color, 1);
            box(rect(x + 2, y - 9, 5, 18), color, color, 1);
            return;
        }
        SetDCBrushColor(dc, color);
        SetDCPenColor(dc, color);
        if (icon == TransportIcon::Play) {
            const POINT triangle[] {{x - 5, y - 9}, {x + 9, y}, {x - 5, y + 9}};
            Polygon(dc, triangle, static_cast<int>(std::size(triangle)));
            return;
        }

        const int direction = icon == TransportIcon::Rewind ? -1 : 1;
        const auto point = [&](int dx, int dy) -> POINT { return {x + direction * dx, y + dy}; };
        const POINT curve[] {
            point(10, -3), point(12, 3), point(8, 11), point(0, 11),
            point(-7, 11), point(-11, 6), point(-11, 0),
            point(-11, -6), point(-6, -10), point(1, -10)
        };
        if (const auto pen = CreatePen(PS_SOLID, 2, color)) {
            const auto previous = SelectObject(dc, pen);
            PolyBezier(dc, curve, static_cast<DWORD>(std::size(curve)));
            SelectObject(dc, previous);
            DeleteObject(pen);
        } else {
            PolyBezier(dc, curve, static_cast<DWORD>(std::size(curve)));
        }
        const POINT arrow[] {point(6, -10), point(0, -14), point(0, -6)};
        Polygon(dc, arrow, static_cast<int>(std::size(arrow)));
        label(L"5", rect(x - 7, y - 7, 14, 16), Font::Small, color, DT_CENTER);
    }

    bool hasActionIcons() const { return fonts[static_cast<std::size_t>(Font::Icon)] != nullptr; }

    int textWidth(std::wstring_view value, Font font) const {
        SelectObject(dc, fonts[static_cast<std::size_t>(font)]);
        SIZE size {};
        GetTextExtentPoint32W(dc, value.data(), static_cast<int>(value.size()), &size);
        return size.cx;
    }

    void label(std::wstring_view value, RECT area, Font font, std::optional<COLORREF> color = {}, UINT align = DT_LEFT) const {
        SelectObject(dc, fonts[static_cast<std::size_t>(font)]);
        SetTextColor(dc, color.value_or(colors.textColor));
        DrawTextW(dc, value.data(), static_cast<int>(value.size()), &area,
                  align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    void paragraph(std::wstring_view value, RECT area, Font font, std::optional<COLORREF> color = {}) const {
        SelectObject(dc, fonts[static_cast<std::size_t>(font)]);
        SetTextColor(dc, color.value_or(colors.muted));
        DrawTextW(dc, value.data(), static_cast<int>(value.size()), &area, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

private:
    HDC dc;
    const std::array<HFONT, static_cast<std::size_t>(Font::Count)>& fonts;
    const Palette& colors;
};

class App {
public:
    App();
    ~App();
    int run(HINSTANCE instance, int show);

private:
    struct EncodedPreparation {
        std::size_t track;
        std::size_t profile;
        std::future<void> finished;
        std::jthread worker;
        bool replaceWhenStopped = false;
    };

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT message(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    void draw(Canvas& canvas);
    void setup(Canvas& canvas);
    void listening(Canvas& canvas);
    void results(Canvas& canvas);
    void library(Canvas& canvas);
    void preferences(Canvas& canvas);
    void appearance(Canvas& canvas);
    void shortcuts(Canvas& canvas);
    void languages(Canvas& canvas);
    void sessionOptions(Canvas& canvas);
    void button(Canvas& canvas, int id, RECT area, std::wstring_view label, Style style = Style::Normal,
                bool enabled = true, Font font = Font::Bold);
    void iconButton(Canvas& canvas, int id, RECT area, ActionIcon icon, std::wstring_view label,
                    Style style = Style::Normal, bool enabled = true, Font font = Font::Bold);
    void choice(Canvas& canvas, Hit hit, std::wstring_view label, std::wstring_view detail = {}, Font font = Font::Bold);
    COLORREF buttonFace(Canvas& canvas, int id, RECT area, Style style, bool enabled);
    void sample(Canvas& canvas, int side, RECT area);
    void transport(Canvas& canvas, Player::State state);
    void updateLanguage();
    void updateAppearance();
    void layoutLadderCountEdit();
    void readLadderCount();
    std::wstring_view t(Text id) const { return locale.get(id); }
    std::wstring profileLabel(const Profile& profile) const;
    std::wstring_view scenarioLabel(const Profile& profile) const;

    void click(int id);
    void key(int key, bool repeat);
    void navigate(bool backwards);
    void save();
    void importTracks();
    std::size_t bundledTrackCount() const { return settings.includeBundled ? bundledTracks.size() : 0; }
    std::size_t trackCount() const { return bundledTrackCount() + settings.tracks.size(); }
    const fs::path& trackPath(std::size_t index) const;
    void start();
    void advanceTrial(Advance action, bool autoplay = false, std::optional<std::size_t> track = {});
    void prepare(bool autoplay = false, bool reuseAudio = false);
    void loadTrialAudio(bool autoplay);
    void prefetchNextAudio();
    void poll();
    void finish(bool restart);
    void selectTrack(std::size_t index);
    void choose(int side);
    void movePlayhead(long long frame);
    std::size_t timelineFrame(int x) const;
    std::optional<LoopRange> draggedLoop(int x) const;
    void setLoop(std::optional<LoopRange> range);

    HWND window = nullptr;
    HWND ladderCountEdit = nullptr;
    HFONT ladderCountFont = nullptr;
    int ladderCountFontHeight = 0;
    std::array<HFONT, static_cast<std::size_t>(Font::Count)> fonts {};
    Palette colors = lightColors;
    Settings settings;
    const std::vector<BundledTrack> bundledTracks = loadBundledTracks();
    Localizer locale;
    Language systemLanguage = Language::English;
    Player player;
    std::mt19937_64 random {std::random_device{}()};
    std::optional<Session> session;
    fs::path lastTrack;
    Panel panel = Panel::Main;
    PreferencePage preferencePage = PreferencePage::Appearance;
    std::shared_ptr<PreparedAudio> audio;
    EncodedAudioCache encodedAudioCache;
    std::mutex resultMutex;
    std::optional<PreparedResult> prepared;
    std::optional<EncodedPreparation> nextEncoded;
    std::jthread preparation;
    std::vector<Hit> hits;
    AudioError error;
    std::wstring status;
    POINT pointer {-1, -1};
    int selected = 0;
    int lastAB = 0;
    int focused = 0;
    int pressed = 0;
    int dragStartX = 0;
    int dragCurrentX = 0;
    int capturing = -1;
    std::size_t page = 0;
    std::optional<std::size_t> requestedTrack;
    std::optional<LoopRange> clip;
    std::optional<std::size_t> loopStart;
    bool active = false;
    bool validLadderCount = true;
    bool restartPending = false;
    bool playWhenPrepared = false;
    bool tracking = false;
    bool dirty = true;
    bool wasPlaying = false;

    static constexpr std::size_t pageSize = 8;
    static constexpr int timelineStart = 82;
    static constexpr int timelineEnd = 702;
    static constexpr RECT seekArea = rect(timelineStart - 5, 398, timelineEnd - timelineStart + 11, 51);
    static constexpr RECT ladderCountArea = rect(635, 340, 82, 27);
};

App::App() {
    settings.load();
    updateLanguage();
}

void App::updateAppearance() {
    HIGHCONTRASTW contrast {};
    contrast.cbSize = sizeof(contrast);
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const bool dark = !highContrast && (settings.theme == ThemeSetting::Dark ||
        (settings.theme == ThemeSetting::System && windowsUsesDarkMode()));
    colors = highContrast ? contrastColors() : dark ? darkColors : lightColors;

    const BOOL darkFrame = dark;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkFrame, sizeof(darkFrame));
    const bool overrideCaption = settings.theme != ThemeSetting::System && !highContrast;
    const COLORREF caption = overrideCaption ? colors.background : DWMWA_COLOR_DEFAULT;
    const COLORREF title = overrideCaption ? colors.textColor : DWMWA_COLOR_DEFAULT;
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &title, sizeof(title));
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

void App::updateLanguage() {
    systemLanguage = resolveLanguage(LanguageSetting::Auto);
    const Language next = settings.language == LanguageSetting::Auto ? systemLanguage : resolveLanguage(settings.language);
    if (fonts[0] && next == locale.language()) return;
    locale = Localizer(next);
    for (HFONT existing : fonts) if (existing) DeleteObject(existing);
    const bool chinese = next == Language::SimplifiedChinese;
    const wchar_t* uiFamily = chinese ? L"Microsoft YaHei UI" : L"Segoe UI";
    const auto font = [&](Font type, int size, int weight, const wchar_t* family) {
        fonts[static_cast<std::size_t>(type)] = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
    };
    font(Font::Small, 13, FW_NORMAL, uiFamily);
    font(Font::Body, 16, FW_NORMAL, uiFamily);
    font(Font::Bold, 16, FW_SEMIBOLD, uiFamily);
    font(Font::Title, 27, FW_SEMIBOLD, uiFamily);
    font(Font::Hero, 35, FW_SEMIBOLD, uiFamily);
    font(Font::Sample, 48, FW_SEMIBOLD, uiFamily);
    font(Font::Mono, 14, FW_NORMAL, chinese ? uiFamily : L"Consolas");
    font(Font::Chinese, 16, FW_SEMIBOLD, L"Microsoft YaHei UI");
    auto& icons = fonts[static_cast<std::size_t>(Font::Icon)];
    icons = createIconFont(L"Segoe Fluent Icons");
    if (!icons) icons = createIconFont(L"Segoe MDL2 Assets");
}

App::~App() {
    if (preparation.joinable()) {
        preparation.request_stop();
        preparation.join();
    }
    for (HFONT font : fonts) DeleteObject(font);
    if (ladderCountFont) DeleteObject(ladderCountFont);
}

void App::layoutLadderCountEdit() {
    if (!ladderCountEdit) return;
    const auto layout = Layout::forWindow(window);
    const int fontHeight = std::max(1, MulDiv(16, layout.h, height));
    if (fontHeight != ladderCountFontHeight) {
        const auto font = CreateFontW(-fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (font) {
            SendMessageW(ladderCountEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (ladderCountFont) DeleteObject(ladderCountFont);
            ladderCountFont = font;
            ladderCountFontHeight = fontHeight;
        }
    }
    const int x = layout.x + MulDiv(ladderCountArea.left + 4, layout.w, width);
    const int y = layout.y + MulDiv(ladderCountArea.top + 4, layout.h, height);
    MoveWindow(ladderCountEdit, x, y, MulDiv(ladderCountArea.right - ladderCountArea.left - 8, layout.w, width),
               MulDiv(ladderCountArea.bottom - ladderCountArea.top - 7, layout.h, height), TRUE);
}

void App::readLadderCount() {
    wchar_t input[5] {};
    const int length = GetWindowTextW(ladderCountEdit, input, static_cast<int>(std::size(input)));
    const bool digits = length > 0 && length <= 3 && std::all_of(input, input + length,
        [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
    const auto count = digits ? std::wcstoul(input, nullptr, 10) : 0;
    validLadderCount = count > 0 && count <= Session::maxTrialsPerLevel;
    if (validLadderCount && settings.trialsPerLevel != count) {
        settings.trialsPerLevel = static_cast<unsigned>(count);
        save();
    }
    dirty = true;
}

void App::button(Canvas& canvas, int id, RECT area, std::wstring_view label, Style style, bool enabled, Font font) {
    const auto ink = buttonFace(canvas, id, area, style, enabled);
    area.left += 5;
    area.right -= 5;
    canvas.label(label, area, font, ink, DT_CENTER);
}

void App::iconButton(Canvas& canvas, int id, RECT area, ActionIcon icon, std::wstring_view label,
                     Style style, bool enabled, Font font) {
    if (!canvas.hasActionIcons()) {
        button(canvas, id, area, label, style, enabled, font);
        return;
    }
    const auto ink = buttonFace(canvas, id, area, style, enabled);
    constexpr int iconWidth = 22;
    constexpr int gap = 8;
    const int labelWidth = std::min(canvas.textWidth(label, font) + 4,
                                    static_cast<int>(area.right - area.left) - iconWidth - gap - 16);
    const int x = (area.left + area.right - iconWidth - gap - labelWidth) / 2;
    const auto glyph = static_cast<wchar_t>(icon);
    canvas.label(std::wstring_view(&glyph, 1), rect(x, area.top, iconWidth, area.bottom - area.top), Font::Icon, ink, DT_CENTER);
    canvas.label(label, rect(x + iconWidth + gap, area.top, labelWidth, area.bottom - area.top), font, ink);
}

void App::choice(Canvas& canvas, Hit hit, std::wstring_view label, std::wstring_view detail, Font font) {
    hits.push_back(hit);
    const auto area = hit.area;
    const bool hot = PtInRect(&area, pointer);
    const bool focus = focused == hit.id;
    const auto fill = hot ? colors.raised : colors.surface;
    if (hot || focus) canvas.box(area, fill, focus ? colors.accent : fill, 8);

    const int x = area.left + 18;
    const int y = detail.empty() ? (area.top + area.bottom) / 2 : area.top + 15;
    const auto marker = rect(x - 9, y - 9, 18, 18);
    if (hit.role == ControlRole::Radio) {
        canvas.ellipse(marker, fill, hit.checked ? colors.accent : colors.muted);
        if (hit.checked) canvas.ellipse(rect(x - 4, y - 4, 8, 8), colors.accent, colors.accent);
    } else {
        canvas.box(marker, hit.checked ? colors.primary : fill, hit.checked ? colors.primary : colors.muted, 4);
        if (hit.checked) canvas.check(x, y, colors.primaryText);
    }
    const int labelX = area.left + 36;
    const int labelWidth = area.right - labelX - 4;
    canvas.label(label, rect(labelX, y - 11, labelWidth, 22), font);
    if (!detail.empty()) canvas.label(detail, rect(labelX, y + 12, labelWidth, 19), Font::Small, colors.muted);
}

COLORREF App::buttonFace(Canvas& canvas, int id, RECT area, Style style, bool enabled) {
    const bool hot = enabled && PtInRect(&area, pointer);
    COLORREF fill = hot ? colors.hovered : colors.raised;
    COLORREF border = colors.line;
    COLORREF ink = enabled ? colors.textColor : colors.disabled;
    if (style == Style::Quiet) fill = hot ? colors.raised : colors.surface;
    if (style == Style::Selected && enabled) {
        fill = hot ? colors.selectedHover : colors.selected;
        border = colors.selectedBorder;
        ink = colors.selectedText;
    }
    if (style == Style::Primary && enabled) {
        fill = hot ? colors.primaryHover : colors.primary;
        border = fill;
        ink = colors.primaryText;
    }
    if (enabled && focused == id) border = colors.textColor;
    if (enabled) hits.push_back({id, area});
    canvas.box(area, fill, border, 8);
    return ink;
}

std::wstring App::profileLabel(const Profile& profile) const {
    auto bitrate = std::to_wstring(profile.bitrate) + L" kbps";
    return profile.codec == Codec::Mp3 ? locale.sampleRate(profile.sampleRate) + L" / " + bitrate : bitrate;
}

std::wstring_view App::scenarioLabel(const Profile& profile) const {
    if (profile.scenario == Scenario::None) return {};
    return t(scenarioNames[static_cast<std::size_t>(profile.scenario) - 1]);
}

void App::draw(Canvas& canvas) {
    hits.clear();
    canvas.box(rect(39, 27, 35, 35), colors.primary, colors.primary, 9);
    for (int i = 0; i < 5; ++i) {
        constexpr int amplitudes[] = {4, 8, 11, 7, 4};
        canvas.stroke(47 + 5 * i, 44 - amplitudes[i], 47 + 5 * i, 45 + amplitudes[i], colors.primaryText);
        canvas.stroke(48 + 5 * i, 44 - amplitudes[i], 48 + 5 * i, 45 + amplitudes[i], colors.primaryText);
    }
    canvas.label(L"Golden Ear", rect(86, 19, 268, 38), Font::Title);
    canvas.label(t(Text::Tagline), rect(88, 57, 268, 18), Font::Small, colors.muted);
    iconButton(canvas, Library, rect(789, 26, 170, 40), ActionIcon::Repertoire,
               t(Text::Repertoire), panel == Panel::Library ? Style::Selected : Style::Quiet);
    iconButton(canvas, Preferences, rect(971, 26, 170, 40), ActionIcon::Settings,
               t(Text::Preferences), panel == Panel::Preferences ? Style::Selected : Style::Quiet);
    canvas.stroke(39, 91, 1141, 91, colors.line);
    canvas.box(rect(39, 110, 705, 574), colors.surface, colors.line, 17);
    canvas.box(rect(762, 110, 379, 574), colors.surface, colors.line, 17);

    if (panel == Panel::Library) library(canvas);
    else if (panel == Panel::Preferences) preferences(canvas);
    else if (active) listening(canvas);
    else setup(canvas);

    results(canvas);
    if (!status.empty()) canvas.label(status, rect(40, 697, 1101, 25), Font::Small, colors.muted);
}

void App::setup(Canvas& canvas) {
    canvas.label(t(session ? Text::AnotherSession : Text::ListeningTestSetup), rect(64, 127, 654, 46), Font::Hero);
    canvas.label(t(Text::TestType), rect(65, 181, 650, 24), Font::Bold);
    for (int i = 0; i < 3; ++i) {
        choice(canvas, {ModeBase + i, rect(65 + i * 221, 207, 211, 37), ControlRole::Radio,
                       static_cast<int>(settings.mode) == i, ModeBase}, t(modeNames[i]));
    }
    constexpr std::array descriptions {Text::AbxDescription, Text::IdentifyDescription, Text::PreferenceDescription};
    canvas.label(t(descriptions[static_cast<std::size_t>(settings.mode)]), rect(65, 246, 650, 22), Font::Small, colors.muted);
    canvas.label(t(Text::Difficulty), rect(65, 275, 650, 24), Font::Bold);
    choice(canvas, {DifficultyBase, rect(65, 301, 321, 37), ControlRole::Radio,
                   settings.difficulty == Difficulty::Random, DifficultyBase}, t(Text::Random));
    choice(canvas, {DifficultyBase + 1, rect(396, 301, 321, 37), ControlRole::Radio,
                   settings.difficulty == Difficulty::Ladder, DifficultyBase}, t(Text::Ladder));
    const bool ladder = settings.difficulty == Difficulty::Ladder;
    if (ladder) {
        const auto help = validLadderCount ? std::wstring(t(Text::LadderDescription)) :
            locale.format(Text::TrialsPerLevelInvalid, Session::maxTrialsPerLevel);
        canvas.label(help, rect(65, 340, 365, 27), Font::Small, validLadderCount ? colors.muted : colors.errorColor);
        canvas.label(t(Text::TrialsPerLevel), rect(435, 340, 188, 27), Font::Body, colors.textColor, DT_RIGHT);
        const auto border = !validLadderCount ? colors.errorColor : focused == LadderTrialCount ? colors.accent : colors.line;
        canvas.box(ladderCountArea, colors.raised, border, 7);
        hits.push_back({LadderTrialCount, ladderCountArea});
    } else {
        canvas.label(t(Text::RandomDescription), rect(65, 340, 650, 22), Font::Small, colors.muted);
    }
    canvas.label(t(Text::Codec), rect(65, 369, 652, 24), Font::Bold);
    for (std::size_t i = 0; i < codecs.size(); ++i) {
        const int x = 65 + static_cast<int>(i) * 132;
        choice(canvas, {CodecBase + static_cast<int>(i), rect(x, 394, 124, 38), ControlRole::Radio,
                       settings.codec == codecs[i], CodecBase}, codecName(codecs[i]));
    }
    canvas.label(t(Text::Profiles), rect(65, 437, 652, 24), Font::Bold);
    const int profileCount = std::popcount(profileMask(settings.codec));
    const int profileWidth = 660 / profileCount;
    int x = 65;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const auto& profile = profiles[i];
        if (profile.codec != settings.codec) continue;
        const bool enabled = (settings.enabled & (1U << i)) != 0;
        const auto detail = profile.codec == Codec::Mp3 ? locale.sampleRate(profile.sampleRate) : std::wstring(scenarioLabel(profile));
        choice(canvas, {ProfileBase + static_cast<int>(i), rect(x, 465, profileWidth - 8, 49), ControlRole::Checkbox, enabled},
               std::to_wstring(profile.bitrate) + L" kbps", detail);
        x += profileWidth;
    }
    if (settings.codec != Codec::Mp3) canvas.label(t(Text::BluetoothHelp), rect(65, 516, 652, 22), Font::Small, colors.muted);
    choice(canvas, {TogglePractice, rect(65, 541, 652, 52), ControlRole::Checkbox, settings.practice},
           t(Text::PracticeMode), t(Text::PracticeHelp));
    canvas.stroke(65, 600, 717, 600, colors.line);
    const auto tracks = trackCount();
    if (tracks) {
        canvas.label(locale.format(tracks == 1 ? Text::TrackReady : Text::TracksReady, tracks),
                     rect(65, 614, 250, 47), Font::Body, colors.muted);
    } else {
        canvas.paragraph(t(Text::AddAudioToBegin), rect(65, 621, 250, 40), Font::Small, colors.muted);
    }
    iconButton(canvas, Import, rect(326, 614, 190, 47), ActionIcon::Import, t(Text::ImportAudio));
    iconButton(canvas, Start, rect(526, 614, 191, 47), ActionIcon::Play, t(Text::StartSession), Style::Primary,
               tracks != 0 && (!ladder || validLadderCount) && !preparation.joinable() && !nextEncoded);
}

void App::results(Canvas& canvas) {
    canvas.label(t(Text::SessionResults), rect(785, 132, 334, 37), Font::Title);
    const bool practice = session ? session->practice : settings.practice;
    if (session) {
        const auto& s = *session;
        std::wstring meta(t(practice ? Text::PracticeMode : modeNames[static_cast<std::size_t>(s.mode)]));
        meta.append(L"  /  ").append(t(s.difficulty == Difficulty::Random ? Text::Random : Text::Ladder));
        meta.append(L"  /  ").append(t(active && !s.complete() ? Text::InProgress : Text::Finished));
        canvas.label(meta, rect(787, 174, 330, 26), Font::Small, colors.muted);
    } else {
        canvas.label(t(practice ? Text::PracticeMode : Text::NoScores), rect(787, 174, 330, 26), Font::Small, colors.muted);
    }
    const bool preference = session ? session->mode == Mode::Preference : settings.mode == Mode::Preference;
    canvas.stroke(787, 215, 1116, 215, colors.line);
    const auto codec = session ? profiles[session->enabled.front()].codec : settings.codec;
    canvas.label(locale.format(Text::ProfileColumn, codecName(codec)), rect(787, 227, 150, 23), Font::Small, colors.muted);
    if (practice) {
        canvas.label(t(Text::AnswersColumn), rect(1016, 227, 100, 23), Font::Small, colors.muted, DT_RIGHT);
    } else {
        canvas.label(t(preference ? Text::PreferenceColumn : Text::CorrectColumn), rect(933, 227, 81, 23), Font::Small, colors.muted, DT_CENTER);
        canvas.label(t(preference ? Text::ShareColumn : Text::PValueColumn), rect(1016, 227, 100, 23), Font::Small, colors.muted, DT_RIGHT);
    }
    int y = 254;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (session && std::find(session->enabled.begin(), session->enabled.end(), i) == session->enabled.end()) continue;
        const auto& profile = profiles[i];
        if (!session && (profile.codec != codec || (settings.enabled & (1U << i)) == 0)) continue;
        canvas.stroke(787, y, 1116, y, colors.line);
        const auto scenario = scenarioLabel(profile);
        canvas.label(profileLabel(profile), rect(787, y + 3, 146, 28), Font::Body);
        if (!scenario.empty()) canvas.label(scenario, rect(787, y + 25, 146, 20), Font::Small, colors.muted);
        if (practice) {
            const auto total = session ? session->scores[i].total : 0;
            canvas.label(total ? std::to_wstring(total) : L"—", rect(1016, y + 4, 100, 30), Font::Mono,
                         total ? colors.textColor : colors.muted, DT_RIGHT);
        } else if (session && session->scores[i].total) {
            const auto& score = session->scores[i];
            const double p = score.adjustedP(session->enabled.size());
            canvas.label(std::to_wstring(score.correct) + L" / " + std::to_wstring(score.total), rect(933, y + 2, 81, 27), Font::Mono, colors.textColor, DT_CENTER);
            if (!preference) canvas.label(percent(score.correct, score.total), rect(933, y + 26, 81, 19), Font::Small, colors.muted, DT_CENTER);
            canvas.label(preference ? percent(score.correct, score.total) : locale.pValue(p), rect(1016, y + 2, 100, 27), Font::Mono,
                         !preference && p <= 0.05 ? colors.accent : colors.textColor, DT_RIGHT);
            if (!preference) canvas.label(t(p <= 0.05 ? Text::AboveChance : Text::Inconclusive), rect(1016, y + 26, 100, 19), Font::Small,
                                         p <= 0.05 ? colors.accent : colors.muted, DT_RIGHT);
        } else {
            canvas.label(L"—", rect(933, y + 4, 81, 30), Font::Mono, colors.muted, DT_CENTER);
            canvas.label(L"—", rect(1016, y + 4, 100, 30), Font::Mono, colors.muted, DT_RIGHT);
        }
        y += 49;
    }
    canvas.stroke(787, y, 1116, y, colors.line);
    if (session) {
        const auto& s = *session;
        const auto counts = locale.format(Text::TrialCounts, s.answered, s.skipped, s.replaced);
        canvas.label(counts, rect(787, 517, 330, 24), Font::Small, colors.muted);
    }
    if (!practice) canvas.paragraph(t(preference ? Text::PreferenceHelp : Text::PValueHelp),
                                   rect(787, 551, 330, 46), Font::Small);
    if (active) {
        const bool complete = session->complete();
        if (settings.restartOnFinish) {
            button(canvas, Finish, rect(787, 601, 330, 33), t(complete ? Text::RestartSession : Text::FinishAndRestart), Style::Quiet);
        }
        if (!complete || panel != Panel::Main) {
            iconButton(canvas, BackToSetup, rect(787, settings.restartOnFinish ? 641 : 613, 330, settings.restartOnFinish ? 33 : 43),
                       ActionIcon::Back, t(Text::BackToSetup), Style::Quiet);
        }
    }
}

void App::sample(Canvas& canvas, int side, RECT area) {
    const int id = SelectA + side;
    const bool enabled = audio != nullptr;
    const bool on = enabled && side == selected;
    const bool hot = enabled && PtInRect(&area, pointer);
    const COLORREF border = focused == id ? colors.textColor : on ? colors.selectedBorder : colors.line;
    canvas.box(area, on ? colors.selected : hot ? colors.hovered : colors.raised, border, 12);
    if (enabled) hits.push_back({id, area});
    canvas.label(std::wstring(1, sideName(side)), rect(area.left + 18, area.top + 10, 86, 60), Font::Sample,
                 on ? colors.selectedText : enabled ? colors.textColor : colors.muted);
    const auto key = keyName(settings.keys[HearA + side], locale);
    const int keyWidth = key.size() > 3 ? 69 : 30;
    const int keyX = area.right - keyWidth - 14;
    canvas.box(rect(keyX, area.top + 16, keyWidth, 24), on ? colors.selected : colors.surface, border, 6);
    canvas.label(key, rect(keyX + 3, area.top + 16, keyWidth - 6, 23), Font::Small, on ? colors.selectedText : colors.muted, DT_CENTER);
    std::wstring label(t(side == 2 ? Text::UnknownSample : Text::ReferenceSample));
    std::wstring_view scenario;
    const auto& trial = session->trial;
    if (session->practice) {
        const auto& profile = profiles[trial.profile];
        if (trial.source(side) == 0) {
            label = t(Text::LosslessSource);
        } else {
            label = std::wstring(codecName(profile.codec)) + L"  ·  " + profileLabel(profile);
            scenario = scenarioLabel(profile);
        }
    }
    const auto sampleInk = on ? colors.selectedText : colors.muted;
    const int labelWidth = area.right - area.left - 32;
    canvas.label(label, rect(area.left + 19, area.top + (scenario.empty() ? 73 : 67), labelWidth, 20), Font::Small, sampleInk);
    if (!scenario.empty()) canvas.label(scenario, rect(area.left + 19, area.top + 86, labelWidth, 18), Font::Small, sampleInk);
    if (trial.response >= 0 && session->mode != Mode::Preference) {
        const bool correct = side == trial.answer(session->mode);
        if (correct || side == trial.response) canvas.answerMark(rect(area.right - 46, area.top + 44, 32, 32), correct);
    }
}

void App::transport(Canvas& canvas, Player::State state) {
    canvas.box(rect(66, 388, 652, 137), colors.raised, colors.line);
    const bool ready = audio != nullptr;
    const auto frameCount = ready ? audio->frames() : 0;
    constexpr int timelineWidth = timelineEnd - timelineStart;
    constexpr int waveformTop = seekArea.top + 2;
    constexpr int waveformCenter = seekArea.top + 22;
    constexpr int progressY = seekArea.bottom - 5;
    const auto positionX = [&](std::size_t frame) {
        return timelineStart + (frameCount ? static_cast<int>(static_cast<double>(frame) * timelineWidth / frameCount) : 0);
    };
    const auto preview = ready && pressed == Seek ? draggedLoop(dragCurrentX) : std::nullopt;
    const auto range = preview ? preview : state.loop;
    const int loopLeft = range ? positionX(range->start) : 0;
    const int loopRight = range ? std::max(loopLeft + 1, positionX(range->end)) : 0;
    if (range) canvas.box(rect(loopLeft, waveformTop, loopRight - loopLeft, progressY - waveformTop + 3), colors.selected, colors.selected, 1);
    const int playhead = positionX(state.position);
    for (std::size_t i = 0; i < PreparedAudio::overviewSize; ++i) {
        const int x = timelineStart + 2 + static_cast<int>((timelineWidth - 4) * i / PreparedAudio::overviewSize);
        const int amplitude = ready ? 2 + static_cast<int>(17 * std::sqrt(audio->overview[i])) : 2;
        const bool inside = range && x >= loopLeft && x <= loopRight;
        const COLORREF waveform = inside ? (x <= playhead ? colors.selectedText : colors.clipWaveform) :
            ready && !range && x <= playhead ? colors.accent : colors.waveform;
        canvas.stroke(x, waveformCenter - amplitude, x, waveformCenter + amplitude + 1, waveform);
        canvas.stroke(x + 1, waveformCenter - amplitude, x + 1, waveformCenter + amplitude + 1, waveform);
    }
    canvas.box(rect(timelineStart, progressY - 1, timelineWidth + 1, 3), colors.line, colors.line, 1);
    if (ready) {
        hits.push_back({Seek, seekArea});
        if (range) {
            canvas.stroke(loopLeft, waveformTop, loopLeft, progressY + 3, colors.accent);
            canvas.stroke(loopRight, waveformTop, loopRight, progressY + 3, colors.accent);
        } else if (loopStart) {
            const int x = positionX(*loopStart);
            canvas.stroke(x, waveformTop, x, progressY + 3, colors.accent);
        }
        const bool insideLoop = range && playhead >= loopLeft && playhead < loopRight;
        const auto playheadColor = insideLoop ? colors.selectedText : colors.accent;
        const int progressStart = insideLoop ? loopLeft : timelineStart;
        if (playhead > progressStart) {
            canvas.box(rect(progressStart, progressY - 1, playhead - progressStart + 1, 3), playheadColor, playheadColor, 1);
        }
        canvas.stroke(playhead, waveformTop, playhead, progressY + 3, playheadColor);
        if (focused == Seek) canvas.stroke(timelineStart, seekArea.bottom, timelineEnd + 1, seekArea.bottom, colors.textColor);
    }
    canvas.label(clockText(state.position, player.sampleRate()), rect(82, 452, 69, 20), Font::Mono, colors.muted);
    canvas.label(clockText(frameCount, player.sampleRate()), rect(633, 452, 69, 20), Font::Mono, colors.muted, DT_RIGHT);
    if (ready) {
        const int rate = player.sampleRate();
        const auto loopLabel = range ? locale.format(Text::LoopTimes, clockText(range->start, rate, true), clockText(range->end, rate, true)) :
            loopStart ? locale.format(Text::LoopStartTime, clockText(*loopStart, rate, true)) : std::wstring(t(Text::DragToLoop));
        canvas.label(loopLabel, rect(151, 452, 482, 20), Font::Small, range || loopStart ? colors.accent : colors.muted, DT_CENTER);
    }

    button(canvas, Toggle, rect(82, 475, 155, 40), t(Text::ToggleAB), Style::Quiet, ready);

    struct PlaybackButton {
        Control id;
        RECT area;
        TransportIcon icon;
        Text description;
    };
    const PlaybackButton controls[] {
        {Rewind, rect(306, 475, 48, 40), TransportIcon::Rewind, Text::SeekBack},
        {PlayPause, rect(368, 473, 48, 44), state.playing ? TransportIcon::Pause : TransportIcon::Play,
            state.playing ? Text::Pause : Text::Play},
        {FastForward, rect(430, 475, 48, 40), TransportIcon::FastForward, Text::SeekForward},
    };
    const PlaybackButton* hoveredControl = nullptr;
    const PlaybackButton* focusedControl = nullptr;
    for (const auto& control : controls) {
        const auto style = control.id == PlayPause ? Style::Primary : Style::Normal;
        const auto ink = buttonFace(canvas, control.id, control.area, style, ready);
        canvas.transportIcon(control.icon, control.area, ink);
        if (ready && PtInRect(&control.area, pointer)) hoveredControl = &control;
        if (ready && focused == control.id) focusedControl = &control;
    }
    if (const auto* hint = hoveredControl ? hoveredControl : focusedControl) {
        canvas.label(t(hint->description), rect(490, 475, 212, 40), Font::Small, colors.muted);
    }

    button(canvas, MarkLoop, rect(66, 532, 224, 31), t(loopStart ? Text::SetLoopEnd : Text::SetLoopStart),
           loopStart ? Style::Selected : Style::Quiet, ready);
    button(canvas, ClearLoop, rect(298, 532, 132, 31), t(Text::ClearLoop), Style::Quiet, ready && (state.loop || loopStart));
    const bool canChangeSong = session->trackCount > 1 && !session->complete();
    button(canvas, ChooseSong, rect(445, 532, 121, 31), t(Text::ChooseSong), Style::Quiet, canChangeSong, Font::Small);
    iconButton(canvas, RandomSong, rect(574, 532, 144, 31), ActionIcon::Shuffle, t(Text::RandomSong),
               Style::Quiet, canChangeSong, Font::Small);
}

void App::listening(Canvas& canvas) {
    const auto& s = *session;
    const auto& trial = s.trial;
    auto meta = locale.format(Text::Trial, s.answered + s.skipped + s.replaced + (trial.response < 0 ? 1 : 0));
    meta.append(L"  /  ").append(t(modeNames[static_cast<std::size_t>(s.mode)]));
    if (s.practice) meta.append(L"  /  ").append(t(Text::PracticeMode));
    canvas.label(meta, rect(66, 131, 650, 24), Font::Mono, colors.accent);
    const auto bundledCount = bundledTrackCount();
    const auto title = trial.track < bundledCount ? std::wstring(bundledTracks[trial.track].title)
        : settings.tracks[trial.track - bundledCount].stem().wstring();
    canvas.label(title, rect(64, 156, 654, 52), Font::Hero);

    if (audio) {
        const auto label = locale.format(Text::PlaybackFormat, locale.sampleRate(player.sampleRate()));
        canvas.label(label, rect(66, 212, 650, 27), Font::Small, colors.muted);
    } else if (error) {
        std::wstring message(t(audioMessage(error.issue)));
        if (!error.detail.empty()) message.append(L" ").append(error.detail);
        canvas.paragraph(message, rect(66, 207, 650, 58), Font::Small, colors.errorColor);
    } else {
        canvas.label(t(Text::Preparing), rect(66, 212, 650, 27), Font::Body, colors.muted);
    }

    const int sides = s.mode == Mode::Abx ? 3 : 2;
    const int sampleWidth = sides == 3 ? 210 : 320;
    for (int side = 0; side < sides; ++side) sample(canvas, side, rect(66 + side * (sampleWidth + 11), 267, sampleWidth, 109));
    transport(canvas, player.state());
    canvas.stroke(66, 571, 718, 571, colors.line);

    if (trial.response >= 0) {
        std::wstring headline;
        auto feedbackColor = colors.accent;
        if (s.mode == Mode::Preference) {
            headline = locale.format(Text::PreferenceAnswer, sideName(trial.response), sideName(trial.lossless));
        } else {
            const bool correct = trial.response == trial.answer(s.mode);
            feedbackColor = correct ? colors.successColor : colors.errorColor;
            headline = t(correct ? Text::Correct : Text::Incorrect);
            headline += L' ';
            headline += s.mode == Mode::Abx ? locale.format(Text::AbxAnswer, sideName(trial.match), sideName(trial.lossless)) :
                locale.format(Text::IdentifyAnswer, sideName(trial.lossless));
        }
        canvas.label(headline, rect(66, 579, 648, 31), Font::Bold, feedbackColor);
        if (!s.practice) {
            const auto& profile = profiles[trial.profile];
            auto label = std::wstring(codecName(profile.codec)) + L"  ·  " + profileLabel(profile);
            if (profile.scenario != Scenario::None) label.append(L"  ·  ").append(scenarioLabel(profile));
            canvas.label(label, rect(66, 619, 426, 41), Font::Body, colors.muted);
        }
        if (s.complete()) {
            iconButton(canvas, BackToSetup, rect(506, 620, 212, 43), ActionIcon::Back, t(Text::BackToSetup), Style::Primary);
        } else {
            iconButton(canvas, Next, rect(506, 620, 212, 43), ActionIcon::Next, t(Text::NextTrial), Style::Primary,
                       !preparation.joinable());
        }
        return;
    }

    constexpr std::array questions {Text::AbxQuestion, Text::IdentifyQuestion, Text::PreferenceQuestion};
    canvas.label(t(questions[static_cast<std::size_t>(s.mode)]), rect(66, 579, 398, 28), Font::Bold);
    const bool idle = !preparation.joinable();
    if (error) {
        button(canvas, Retry, rect(66, 617, 285, 45), t(Text::Retry), Style::Primary, idle);
    } else {
        button(canvas, SubmitA, rect(66, 617, 138, 45), L"A  /  " + keyName(settings.keys[AnswerA], locale), Style::Selected, audio != nullptr);
        button(canvas, SubmitB, rect(213, 617, 138, 45), L"B  /  " + keyName(settings.keys[AnswerB], locale), Style::Selected, audio != nullptr);
    }
    button(canvas, Skip, rect(618, 617, 100, 45), t(Text::Skip), Style::Quiet, idle);
}

void App::library(Canvas& canvas) {
    const bool canChooseTrack = active && !session->complete();
    canvas.label(t(Text::Repertoire), rect(65, 128, 640, 47), Font::Hero);
    canvas.label(t(canChooseTrack ? Text::ChooseTrackHelp : Text::SourceFormats), rect(66, 182, 648, 24), Font::Body, colors.muted);
    canvas.stroke(66, 224, 718, 224, colors.line);
    const auto count = trackCount();
    const auto bundledCount = bundledTrackCount();
    if (!count) {
        canvas.label(t(Text::EmptyLibrary), rect(66, 253, 636, 35), Font::Body, colors.muted);
    }
    const auto first = page * pageSize;
    const auto end = std::min(count, first + pageSize);
    for (std::size_t i = first; i < end; ++i) {
        const int y = 230 + static_cast<int>(i - first) * 42;
        if (i < bundledCount) {
            const auto& track = bundledTracks[i];
            canvas.label(track.title, rect(66, y + 2, 489, 22), Font::Bold);
            canvas.label(track.performer, rect(66, y + 22, 489, 17), Font::Small, colors.muted);
            if (!active) canvas.label(t(Text::BundledTrack), rect(570, y + 4, 148, 31), Font::Small, colors.muted, DT_RIGHT);
        } else {
            const auto& path = settings.tracks[i - bundledCount];
            canvas.label(path.filename().wstring(), rect(66, y + 2, 489, 22), Font::Bold);
            canvas.label(path.parent_path().wstring(), rect(66, y + 22, 489, 17), Font::Small, colors.muted);
            if (!active) button(canvas, RemoveBase + static_cast<int>(i - first), rect(603, y + 4, 115, 31), t(Text::Remove), Style::Quiet);
        }
        if (active) {
            if (session->trial.track == i) {
                canvas.label(t(Text::CurrentTrack), rect(570, y + 4, 148, 31), Font::Small, colors.accent, DT_RIGHT);
            } else if (canChooseTrack) {
                button(canvas, PickTrackBase + static_cast<int>(i - first), rect(603, y + 4, 115, 31),
                       t(Text::ChooseTrack), Style::Quiet);
            }
        }
        canvas.stroke(66, y + 41, 718, y + 41, colors.line);
    }
    if (count > pageSize) {
        button(canvas, PreviousPage, rect(66, 575, 100, 33), t(Text::PreviousPage), Style::Quiet, page > 0);
        button(canvas, NextPage, rect(175, 575, 100, 33), t(Text::NextPage), Style::Quiet, end < count);
        canvas.label(locale.format(Text::PageRange, first + 1, end, count), rect(288, 575, 158, 33), Font::Small, colors.muted);
    }
    if (!active && !bundledTracks.empty()) {
        choice(canvas, {ToggleBundled, rect(66, 611, 310, 42), ControlRole::Checkbox, settings.includeBundled},
               t(Text::ShowBundledTracks));
    }
    button(canvas, ClosePanel, rect(active ? 618 : 418, 611, 100, 42), t(Text::Done), active ? Style::Primary : Style::Quiet);
    if (!active) iconButton(canvas, Import, rect(528, 611, 190, 42), ActionIcon::Import, t(Text::ImportAudio), Style::Primary);
}

void App::preferences(Canvas& canvas) {
    canvas.label(t(Text::Preferences), rect(65, 128, 640, 47), Font::Hero);
    canvas.stroke(66, 227, 718, 227, colors.line);
    for (int i = 0; i < static_cast<int>(preferencePages.size()); ++i) {
        const int id = PreferenceBase + i;
        const auto area = rect(66 + i * 165, 187, 157, 40);
        const bool open = static_cast<int>(preferencePage) == i;
        const bool hot = PtInRect(&area, pointer);
        const auto fill = hot ? colors.raised : colors.surface;
        if (hot || focused == id) canvas.box(area, fill, focused == id ? colors.accent : fill, 8);
        hits.push_back({id, area});
        canvas.label(t(preferencePages[i]), area, Font::Bold, open ? colors.accent : colors.textColor, DT_CENTER);
        if (open) canvas.box(rect(area.left + 12, area.bottom - 2, area.right - area.left - 24, 3), colors.accent, colors.accent, 1);
    }
    switch (preferencePage) {
    case PreferencePage::Appearance: appearance(canvas); break;
    case PreferencePage::Language: languages(canvas); break;
    case PreferencePage::Shortcuts: shortcuts(canvas); break;
    case PreferencePage::Session: sessionOptions(canvas); break;
    }
    button(canvas, ClosePanel, rect(602, 611, 116, 42), t(Text::Done), Style::Primary);
}

void App::sessionOptions(Canvas& canvas) {
    choice(canvas, {RestartOnFinishOption, rect(66, 263, 652, 59), ControlRole::Checkbox, settings.restartOnFinish},
           t(Text::RestartOnFinish), t(Text::RestartOnFinishHelp));
    canvas.stroke(66, 331, 718, 331, colors.line);
    choice(canvas, {ShuffleTracksOption, rect(66, 343, 652, 59), ControlRole::Checkbox, settings.shuffleTracks},
           t(Text::ShuffleTracks), t(Text::ShuffleTracksHelp));
    canvas.stroke(66, 411, 718, 411, colors.line);
    canvas.label(t(Text::ClearEncodedAudioCacheHelp), rect(75, 427, 373, 42), Font::Small, colors.muted);
    iconButton(canvas, ClearEncodedAudioCache, rect(466, 427, 252, 42), ActionIcon::Delete, t(Text::ClearEncodedAudioCache), Style::Quiet);
}

void App::appearance(Canvas& canvas) {
    canvas.label(t(Text::AppTheme), rect(66, 254, 648, 28), Font::Bold);
    constexpr std::array themeNames {Text::SystemTheme, Text::LightTheme, Text::DarkTheme};
    for (int i = 0; i < static_cast<int>(themeNames.size()); ++i) {
        choice(canvas, {ThemeBase + i, rect(66 + i * 220, 294, 212, 44), ControlRole::Radio,
                       static_cast<int>(settings.theme) == i, ThemeBase}, t(themeNames[i]));
    }
    canvas.label(t(Text::AppearanceHelp), rect(66, 348, 648, 28), Font::Body, colors.muted);
}

void App::shortcuts(Canvas& canvas) {
    canvas.label(t(Text::ShortcutHelp), rect(66, 254, 648, 24), Font::Body, colors.muted);
    for (int i = 0; i < KeyCount; ++i) {
        const int column = i / 5;
        const int y = 290 + 57 * (i % 5);
        const int x = 66 + 335 * column;
        canvas.stroke(x, y, x + 316, y, colors.line);
        canvas.label(t(shortcutNames[i]), rect(x, y + 7, 200, 44), Font::Body);
        const auto label = capturing == i ? std::wstring(t(Text::PressKey)) : keyName(settings.keys[i], locale);
        button(canvas, KeyBase + i, rect(x + 202, y + 11, 115, 36), label,
               capturing == i ? Style::Selected : Style::Normal);
    }
    button(canvas, ResetKeys, rect(66, 611, 159, 42), t(Text::ResetKeys), Style::Quiet);
}

void App::languages(Canvas& canvas) {
    canvas.label(t(Text::LanguageHelp), rect(66, 254, 648, 24), Font::Body, colors.muted);
    const int autoIndex = static_cast<int>(LanguageSetting::Auto);
    choice(canvas, {LanguageBase + autoIndex, rect(66, 294, 321, 44), ControlRole::Radio,
                   settings.language == LanguageSetting::Auto, LanguageBase},
           locale.format(Text::AutoLanguage, nativeName(systemLanguage)), {},
           systemLanguage == Language::SimplifiedChinese ? Font::Chinese : Font::Bold);
    constexpr std::array manualOptions {LanguageSetting::English, LanguageSetting::SimplifiedChinese, LanguageSetting::Spanish};
    for (std::size_t i = 0; i < manualOptions.size(); ++i) {
        const auto option = manualOptions[i];
        const auto language = resolveLanguage(option);
        choice(canvas, {LanguageBase + static_cast<int>(option), rect(66, 350 + static_cast<int>(i) * 56, 321, 44),
                       ControlRole::Radio, settings.language == option, LanguageBase}, nativeName(language), {},
               language == Language::SimplifiedChinese ? Font::Chinese : Font::Bold);
    }
}

void App::save() {
    if (!settings.save()) status = t(Text::SettingsSaveError);
}

void App::importTracks() {
    if (active) return;
    std::vector<wchar_t> filenames(65536);
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    std::wstring filter(t(Text::AudioFileFilter));
    filter += L'\0';
    filter += L"*.flac;*.wav";
    filter += L'\0';
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = filenames.data();
    dialog.nMaxFile = static_cast<DWORD>(filenames.size());
    dialog.lpstrTitle = t(Text::ImportTitle).data();
    dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError()) status = t(Text::FilePickerError);
        return;
    }
    const auto oldCount = settings.tracks.size();
    const wchar_t* part = filenames.data();
    const fs::path first(part);
    part += wcslen(part) + 1;
    const auto add = [&](const fs::path& path) {
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) { return std::towlower(ch); });
        if (extension != L".flac" && extension != L".wav") return;
        const auto matches = [&](const fs::path& existing) {
            return CompareStringOrdinal(path.c_str(), -1, existing.c_str(), -1, TRUE) == CSTR_EQUAL;
        };
        if (std::any_of(settings.tracks.begin(), settings.tracks.end(), matches)) return;
        if (std::any_of(bundledTracks.begin(), bundledTracks.end(), [&](const auto& track) { return matches(track.path); })) return;
        settings.tracks.push_back(path);
    };
    if (!*part) add(first);
    for (; *part; part += wcslen(part) + 1) add(first / part);
    const auto imported = settings.tracks.size() - oldCount;
    if (imported) {
        status = locale.format(imported == 1 ? Text::ImportedOne : Text::ImportedMany, imported);
        save();
    } else {
        status = t(Text::NoTracksAdded);
    }
}

const fs::path& App::trackPath(std::size_t index) const {
    const auto bundledCount = bundledTrackCount();
    return index < bundledCount ? bundledTracks[index].path : settings.tracks[index - bundledCount];
}

void App::start() {
    if (active || preparation.joinable() || nextEncoded) return;
    restartPending = false;
    if (settings.difficulty == Difficulty::Ladder && !validLadderCount) return;
    const auto count = trackCount();
    if (!count) return;
    std::vector<std::size_t> enabled;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].codec == settings.codec && (settings.enabled & (1U << i))) enabled.push_back(i);
    }
    if (enabled.empty()) return;
    std::optional<std::size_t> previousTrack;
    if (!lastTrack.empty()) {
        for (std::size_t i = 0; i < count; ++i) {
            if (trackPath(i) == lastTrack) {
                previousTrack = i;
                break;
            }
        }
    }
    session.emplace(settings.mode, settings.difficulty, std::move(enabled), count, random, settings.practice,
                    settings.shuffleTracks, previousTrack, settings.trialsPerLevel);
    clip.reset();
    active = true;
    status.clear();
    prepare();
}

void App::advanceTrial(Advance action, bool autoplay, std::optional<std::size_t> track) {
    const auto previous = session->trial;
    session->advance(action, track);
    const auto& trial = session->trial;
    const bool sameTrack = trial.track == previous.track;
    if (!sameTrack) clip.reset();
    bool reuseAudio = false;
    if (sameTrack && trial.profile == previous.profile && audio && audio->sourceWriteTime) {
        std::error_code error;
        const auto modified = fs::last_write_time(trackPath(trial.track), error);
        reuseAudio = !error && modified == *audio->sourceWriteTime;
    }
    prepare(autoplay, reuseAudio);
}

void App::prepare(bool autoplay, bool reuseAudio) {
    if (!active || preparation.joinable()) return;
    error = {};
    loopStart.reset();
    if (pressed == Seek && GetCapture() == window) ReleaseCapture();
    selected = lastAB = 0;
    focused = 0;
    if (!reuseAudio) {
        audio.reset();
        player.load(nullptr, 0);
    }
    const int previousRate = player.sampleRate();
    if (!player.open()) {
        audio.reset();
        error = {AudioIssue::OutputUnavailable, {}};
        return;
    }
    const int rate = player.sampleRate();
    if (audio && rate == previousRate) {
        loadTrialAudio(autoplay);
        return;
    }
    audio.reset();
    if (clip && rate != previousRate) {
        clip->start = static_cast<std::uint64_t>(clip->start) * rate / previousRate;
        clip->end = static_cast<std::uint64_t>(clip->end) * rate / previousRate;
    }
    const auto& trial = session->trial;
    const auto track = trackPath(trial.track);
    const auto profile = profiles[trial.profile];
    playWhenPrepared = autoplay;
    if (nextEncoded && (nextEncoded->track != trial.track || nextEncoded->profile != trial.profile)) nextEncoded->worker.request_stop();
    preparation = std::jthread([this, track, profile, rate](std::stop_token stop) {
        auto result = prepareAudio(encodedAudioCache, track, profile, rate, stop);
        std::scoped_lock lock(resultMutex);
        prepared = std::move(result);
    });
}

void App::loadTrialAudio(bool autoplay) {
    player.load(audio, session->trial.source(selected), clip);
    if (!audio) return;
    if (autoplay) player.toggle();
    prefetchNextAudio();
}

void App::prefetchNextAudio() {
    try {
        const auto next = session->previewNext();
        const auto& current = session->trial;
        const bool needsEncode = next && (next->track != current.track || next->profile != current.profile);
        if (nextEncoded) {
            if (needsEncode && nextEncoded->track == next->track && nextEncoded->profile == next->profile &&
                !nextEncoded->worker.get_stop_token().stop_requested()) return;
            nextEncoded->replaceWhenStopped = needsEncode;
            nextEncoded->worker.request_stop();
            if (nextEncoded->finished.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            nextEncoded.reset();
        }
        if (!needsEncode) return;
        auto track = trackPath(next->track);
        std::promise<void> done;
        auto finished = done.get_future();
        std::jthread worker([this, track = std::move(track), profile = profiles[next->profile],
                             done = std::move(done)](std::stop_token stop) mutable {
            prefetchAudio(encodedAudioCache, track, profile, stop);
            done.set_value();
        });
        nextEncoded.emplace(EncodedPreparation {next->track, next->profile, std::move(finished), std::move(worker)});
    } catch (const std::exception&) {
        // The next trial can encode on demand.
    }
}

void App::poll() {
    std::optional<PreparedResult> result;
    {
        std::scoped_lock lock(resultMutex);
        result.swap(prepared);
    }
    if (result) {
        preparation.join();
        const bool autoplay = std::exchange(playWhenPrepared, false);
        if (active && requestedTrack) {
            selectTrack(*requestedTrack);
        } else if (active) {
            audio = std::move(result->audio);
            error = std::move(result->error);
            loadTrialAudio(autoplay);
        }
        dirty = true;
    }
    if (nextEncoded && nextEncoded->finished.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool replace = nextEncoded->replaceWhenStopped;
        nextEncoded.reset();
        if (replace && active && audio) prefetchNextAudio();
        dirty = true;
    }
    if (restartPending && !preparation.joinable() && !nextEncoded) {
        start();
        dirty = true;
    }
    if (active && audio) {
        const auto state = player.state();
        if (state.failed) {
            player.load(nullptr, 0);
            audio.reset();
            loopStart.reset();
            error = {AudioIssue::OutputStopped, {}};
        }
        dirty |= state.playing || wasPlaying || state.failed;
        wasPlaying = state.playing;
    }
    if (dirty) InvalidateRect(window, nullptr, FALSE);
}

void App::finish(bool restart) {
    if (!active) return;
    lastTrack = trackPath(session->trial.track);
    restartPending = restart;
    requestedTrack.reset();
    if (preparation.joinable()) preparation.request_stop();
    if (nextEncoded) nextEncoded->worker.request_stop();
    player.load(nullptr, 0);
    audio.reset();
    clip.reset();
    loopStart.reset();
    if (pressed == Seek && GetCapture() == window) ReleaseCapture();
    error = {};
    active = false;
    capturing = -1;
    panel = Panel::Main;
    focused = 0;
}

void App::selectTrack(std::size_t index) {
    if (session->complete() || index >= session->trackCount || index == session->trial.track) return;
    panel = Panel::Main;
    if (preparation.joinable()) {
        requestedTrack = index;
        preparation.request_stop();
        return;
    }
    requestedTrack.reset();
    advanceTrial(session->trial.response >= 0 ? Advance::Next : Advance::ChangeSong, false, index);
}

void App::choose(int side) {
    if (!active || !audio || (side == 2 && session->mode != Mode::Abx)) return;
    selected = side;
    if (side != 2) lastAB = side;
    player.select(session->trial.source(side));
}

void App::movePlayhead(long long frame) {
    if (!active || !audio) return;
    player.seek(static_cast<std::size_t>(std::clamp(frame, 0LL, static_cast<long long>(audio->frames()))));
}

std::size_t App::timelineFrame(int x) const {
    const int offset = std::clamp(x, timelineStart, timelineEnd) - timelineStart;
    return static_cast<std::uint64_t>(audio->frames()) * offset / (timelineEnd - timelineStart);
}

std::optional<LoopRange> App::draggedLoop(int x) const {
    if (std::abs(x - dragStartX) < 4) return {};
    const auto start = timelineFrame(std::min(dragStartX, x));
    const auto end = timelineFrame(std::max(dragStartX, x));
    if (start == end) return {};
    return LoopRange {start, end};
}

void App::setLoop(std::optional<LoopRange> range) {
    clip = range;
    loopStart.reset();
    player.setLoop(range);
    dirty = true;
}

void App::click(int id) {
    status.clear();
    switch (id) {
    case ChooseSong: case Library:
        panel = id == Library && panel == Panel::Library ? Panel::Main : Panel::Library;
        capturing = -1;
        page = active ? session->trial.track / pageSize : 0;
        break;
    case RandomSong:
        if (active && session->trackCount > 1) selectTrack(session->randomOtherTrack());
        break;
    case Preferences:
        panel = panel == Panel::Preferences ? Panel::Main : Panel::Preferences;
        capturing = -1;
        break;
    case ClosePanel: panel = Panel::Main; capturing = -1; break;
    case Import: importTracks(); break;
    case LadderTrialCount: SetFocus(ladderCountEdit); break;
    case TogglePractice: if (!active) { settings.practice = !settings.practice; save(); } break;
    case RestartOnFinishOption:
        settings.restartOnFinish = !settings.restartOnFinish;
        if (!settings.restartOnFinish) restartPending = false;
        save();
        break;
    case ShuffleTracksOption:
        settings.shuffleTracks = !settings.shuffleTracks;
        if (active) {
            session->shuffleTracks = settings.shuffleTracks;
            if (audio) prefetchNextAudio();
        }
        save();
        break;
    case ClearEncodedAudioCache:
        if (nextEncoded) {
            nextEncoded->replaceWhenStopped = false;
            nextEncoded->worker.request_stop();
        }
        encodedAudioCache.clear();
        status = t(Text::EncodedAudioCacheCleared);
        break;
    case Start: start(); break;
    case Finish: finish(settings.restartOnFinish); break;
    case BackToSetup: finish(false); break;
    case Retry: prepare(); break;
    case Next: case NextAndPlay: case Skip: {
        if (!active || session->complete() || preparation.joinable()) break;
        const bool next = id == Next || id == NextAndPlay;
        if (next != (session->trial.response >= 0)) break;
        advanceTrial(next ? Advance::Next : Advance::Skip, id == NextAndPlay);
        break;
    }
    case Seek: if (active && audio && panel == Panel::Main) player.seek(timelineFrame(pointer.x)); break;
    case MarkLoop: {
        if (!active || !audio) break;
        const auto state = player.state();
        if (!loopStart) {
            if (state.loop) setLoop({});
            loopStart = state.position;
        } else if (*loopStart != state.position) {
            setLoop(LoopRange {std::min(*loopStart, state.position), std::max(*loopStart, state.position)});
        }
        break;
    }
    case ClearLoop: if (active && audio) setLoop({}); break;
    case PlayPause: if (active && audio) player.toggle(); break;
    case Rewind: case FastForward:
        movePlayhead(static_cast<long long>(player.state().position) + (id == Rewind ? -5LL : 5LL) * player.sampleRate());
        break;
    case SelectA: case SelectB: case SelectX: choose(id - SelectA); break;
    case Toggle: choose(1 - lastAB); break;
    case SubmitA: case SubmitB:
        if (active && audio && session->submit(id - SubmitA)) focused = session->complete() ? BackToSetup : Next;
        break;
    case PreviousPage: if (page > 0) --page; break;
    case NextPage: if ((page + 1) * pageSize < trackCount()) ++page; break;
    case ToggleBundled:
        if (!active && !bundledTracks.empty()) {
            settings.includeBundled = !settings.includeBundled;
            page = 0;
            save();
        }
        break;
    case ResetKeys: settings.keys = defaultKeys; capturing = -1; save(); break;
    default:
        if (id >= PreferenceBase && id < PreferenceBase + static_cast<int>(preferencePages.size())) {
            preferencePage = static_cast<PreferencePage>(id - PreferenceBase);
            capturing = -1;
        } else if (id >= ModeBase && id < ModeBase + 3 && !active) {
            settings.mode = static_cast<Mode>(id - ModeBase);
            save();
        } else if (id >= DifficultyBase && id < DifficultyBase + 2 && !active) {
            settings.difficulty = static_cast<Difficulty>(id - DifficultyBase);
            save();
        } else if (id >= CodecBase && id < CodecBase + static_cast<int>(codecs.size()) && !active) {
            settings.codec = codecs[id - CodecBase];
            status.clear();
            save();
        } else if (id >= ProfileBase && id < ProfileBase + static_cast<int>(profiles.size()) && !active &&
                   profiles[id - ProfileBase].codec == settings.codec) {
            const auto enabled = settings.enabled ^ (1U << (id - ProfileBase));
            if (enabled & profileMask(settings.codec)) { settings.enabled = enabled; status.clear(); save(); }
            else status = t(Text::NoProfile);
        } else if (id >= KeyBase && id < KeyBase + static_cast<int>(KeyCount)) {
            capturing = id - KeyBase;
        } else if (id >= ThemeBase && id <= ThemeBase + static_cast<int>(ThemeSetting::Dark)) {
            settings.theme = static_cast<ThemeSetting>(id - ThemeBase);
            updateAppearance();
            save();
        } else if (id >= LanguageBase && id <= LanguageBase + static_cast<int>(LanguageSetting::Spanish)) {
            settings.language = static_cast<LanguageSetting>(id - LanguageBase);
            updateLanguage();
            save();
        } else if (id >= PickTrackBase && id < PickTrackBase + static_cast<int>(pageSize) &&
                   active && panel == Panel::Library) {
            selectTrack(page * pageSize + (id - PickTrackBase));
        } else if (id >= RemoveBase && id < RemoveBase + static_cast<int>(pageSize) && !active) {
            const auto index = page * pageSize + (id - RemoveBase);
            const auto bundledCount = bundledTrackCount();
            if (index >= bundledCount && index < trackCount()) {
                settings.tracks.erase(settings.tracks.begin() + static_cast<std::ptrdiff_t>(index - bundledCount));
                if (page > 0 && page * pageSize >= trackCount()) --page;
                status = t(Text::TrackRemoved);
                save();
            }
        }
        break;
    }
    dirty = true;
}

void App::navigate(bool backwards) {
    if (hits.empty()) return;
    const auto found = std::find_if(hits.begin(), hits.end(), [&](const Hit& hit) { return hit.id == focused; });
    const auto count = hits.size();
    const auto step = backwards ? count - 1 : 1;
    auto index = found == hits.end() ? (backwards ? 0 : count - 1) : static_cast<std::size_t>(found - hits.begin());
    for (std::size_t i = 0; i < count; ++i) {
        index = (index + step) % count;
        const auto& next = hits[index];
        if (next.role == ControlRole::Radio && !next.checked) continue;
        focused = next.id;
        if (focused == LadderTrialCount) {
            SetFocus(ladderCountEdit);
            SendMessageW(ladderCountEdit, EM_SETSEL, 0, -1);
        } else if (GetFocus() == ladderCountEdit) {
            SetFocus(window);
        }
        dirty = true;
        return;
    }
}

void App::key(int code, bool repeat) {
    if (capturing >= 0) {
        if (repeat) return;
        if (code == VK_ESCAPE) {
            capturing = -1;
            status.clear();
        } else if (!bindable(code) || ((GetKeyState(VK_CONTROL) | GetKeyState(VK_SHIFT)) & 0x8000)) {
            status = t(Text::ReservedKeys);
        } else {
            const auto other = std::find(settings.keys.begin(), settings.keys.end(), code);
            if (other != settings.keys.end() && other - settings.keys.begin() != capturing) {
                status = locale.format(Text::DuplicateKey, t(shortcutNames[other - settings.keys.begin()]));
            } else {
                settings.keys[capturing] = code;
                capturing = -1;
                status.clear();
                save();
            }
        }
        dirty = true;
        return;
    }
    if (code == VK_TAB) {
        if (!repeat) navigate((GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return;
    }
    if (code == VK_ESCAPE && panel != Panel::Main) { click(ClosePanel); return; }
    if (panel == Panel::Preferences && focused >= PreferenceBase &&
        focused < PreferenceBase + static_cast<int>(preferencePages.size()) && (code == VK_LEFT || code == VK_RIGHT)) {
        const int count = static_cast<int>(preferencePages.size());
        const int step = code == VK_RIGHT ? 1 : count - 1;
        focused = PreferenceBase + (focused - PreferenceBase + step) % count;
        click(focused);
        return;
    }
    const auto current = std::find_if(hits.begin(), hits.end(), [&](const Hit& hit) { return hit.id == focused; });
    if (code == VK_RETURN) {
        if (!repeat && focused != Seek && current != hits.end()) click(focused);
        return;
    }
    if (current != hits.end() && current->role != ControlRole::Action && code == VK_SPACE) {
        if (!repeat) click(focused);
        return;
    }
    const bool arrowBack = code == VK_LEFT || code == VK_UP;
    if (current != hits.end() && current->role == ControlRole::Radio && (arrowBack || code == VK_RIGHT || code == VK_DOWN)) {
        const auto inGroup = [group = current->group](const Hit& hit) { return hit.group == group; };
        const auto first = std::find_if(hits.begin(), current, inGroup);
        const auto last = std::find_if_not(current, hits.end(), inGroup);
        const auto count = last - first;
        const auto step = arrowBack ? count - 1 : 1;
        focused = (first + (current - first + step) % count)->id;
        click(focused);
        return;
    }
    if (!active || panel != Panel::Main || ((GetKeyState(VK_CONTROL) | GetKeyState(VK_SHIFT)) & 0x8000)) return;
    constexpr std::array controls {SelectA, SelectB, SelectX, Toggle, PlayPause, Rewind, FastForward, SubmitA, SubmitB};
    for (std::size_t i = 0; i < settings.keys.size(); ++i) {
        if (settings.keys[i] == code && (!repeat || i == Back || i == Forward)) {
            click(i == Play && session->trial.response >= 0 && !session->complete() ? NextAndPlay : controls[i]);
        }
    }
}

void App::paint() {
    const bool showLadderCount = panel == Panel::Main && !active && settings.difficulty == Difficulty::Ladder;
    if (ladderCountEdit && showLadderCount != (IsWindowVisible(ladderCountEdit) != FALSE)) {
        if (!showLadderCount && GetFocus() == ladderCountEdit) SetFocus(window);
        ShowWindow(ladderCountEdit, showLadderCount ? SW_SHOWNA : SW_HIDE);
    }
    PAINTSTRUCT paintInfo {};
    HDC target = BeginPaint(window, &paintInfo);
    RECT client {};
    GetClientRect(window, &client);
    HDC memory = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    if (memory && bitmap) {
        SelectObject(memory, bitmap);
        SelectObject(memory, GetStockObject(DC_BRUSH));
        SelectObject(memory, GetStockObject(DC_PEN));
        SetDCBrushColor(memory, colors.background);
        FillRect(memory, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const auto layout = Layout::forWindow(window);
        SaveDC(memory);
        SetMapMode(memory, MM_ANISOTROPIC);
        SetWindowExtEx(memory, width, height, nullptr);
        SetViewportExtEx(memory, layout.w, layout.h, nullptr);
        SetViewportOrgEx(memory, layout.x, layout.y, nullptr);
        Canvas canvas(memory, fonts, colors);
        draw(canvas);
        RestoreDC(memory, -1);
        BitBlt(target, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    }
    if (memory) DeleteDC(memory);
    if (bitmap) DeleteObject(bitmap);
    EndPaint(window, &paintInfo);
    dirty = false;
}

LRESULT CALLBACK App::procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->message(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::message(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        layoutLadderCountEdit();
        dirty = true;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE: updateLanguage(); updateAppearance(); dirty = true; return 0;
    case WM_THEMECHANGED: case WM_SYSCOLORCHANGE: updateAppearance(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(); return 0;
    case WM_TIMER: poll(); return 0;
    case WM_KEYDOWN: key(static_cast<int>(wParam), (lParam & (1LL << 30)) != 0); return 0;
    case WM_COMMAND:
        if (ladderCountEdit && reinterpret_cast<HWND>(lParam) == ladderCountEdit) {
            if (HIWORD(wParam) == EN_CHANGE) readLadderCount();
            if (HIWORD(wParam) == EN_SETFOCUS) focused = LadderTrialCount;
            dirty = true;
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
        if (ladderCountEdit && reinterpret_cast<HWND>(lParam) == ladderCountEdit) {
            const auto dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, colors.textColor);
            SetBkColor(dc, colors.raised);
            SetDCBrushColor(dc, colors.raised);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        break;
    case WM_GETMINMAXINFO: {
        auto& info = *reinterpret_cast<MINMAXINFO*>(lParam);
        info.ptMinTrackSize = {820, 530};
        return 0;
    }
    case WM_MOUSEMOVE:
        pointer = Layout::forWindow(window).point(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (pressed == Seek) dragCurrentX = pointer.x;
        if (!tracking) {
            TRACKMOUSEEVENT event {sizeof(event), TME_LEAVE, window, 0};
            tracking = TrackMouseEvent(&event) != 0;
        }
        dirty = true;
        return 0;
    case WM_MOUSELEAVE: pointer = {-1, -1}; tracking = false; dirty = true; return 0;
    case WM_LBUTTONDOWN:
        pointer = Layout::forWindow(window).point(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        for (const auto& hit : hits) {
            if (PtInRect(&hit.area, pointer)) {
                focused = pressed = hit.id;
                if (pressed == Seek) dragStartX = dragCurrentX = pointer.x;
                SetFocus(window);
                SetCapture(window);
                dirty = true;
                break;
            }
        }
        return 0;
    case WM_LBUTTONUP: {
        pointer = Layout::forWindow(window).point(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        const int pressedId = pressed;
        pressed = 0;
        if (GetCapture() == window) ReleaseCapture();
        if (pressedId == Seek && active && audio && panel == Panel::Main) {
            if (const auto range = draggedLoop(pointer.x)) {
                setLoop(range);
                return 0;
            }
        }
        const bool inside = std::any_of(hits.begin(), hits.end(), [&](const Hit& hit) {
            return hit.id == pressedId && PtInRect(&hit.area, pointer);
        });
        if (inside) click(pressedId);
        return 0;
    }
    case WM_CAPTURECHANGED: pressed = 0; dirty = true; return 0;
    case WM_DESTROY: KillTimer(window, 1); PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int App::run(HINSTANCE instance, int show) {
    WNDCLASSEXW windowClass {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_GOLDEN_EAR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_GOLDEN_EAR), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    windowClass.lpszClassName = L"GoldenEarWindow";
    if (!RegisterClassExW(&windowClass)) return 1;
    RECT desktop {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
    const int workW = desktop.right - desktop.left;
    const int workH = desktop.bottom - desktop.top;
    const double scale = std::min({1.0, (workW - 30) / static_cast<double>(width), (workH - 70) / static_cast<double>(height)});
    RECT outer = rect(0, 0, static_cast<int>(width * scale), static_cast<int>(height * scale));
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&outer, style, FALSE, 0);
    const int outerW = outer.right - outer.left;
    const int outerH = outer.bottom - outer.top;
    HWND created = CreateWindowExW(0, windowClass.lpszClassName, L"Golden Ear", style,
        desktop.left + (workW - outerW) / 2, desktop.top + (workH - outerH) / 2,
        outerW, outerH, nullptr, nullptr, instance, this);
    if (!created) return 1;
    ladderCountEdit = CreateWindowExW(0, L"EDIT", std::to_wstring(settings.trialsPerLevel).c_str(),
        WS_CHILD | WS_TABSTOP | ES_CENTER | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 0, 0, created,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(LadderTrialCount)), instance, nullptr);
    if (!ladderCountEdit) {
        DestroyWindow(created);
        return 1;
    }
    layoutLadderCountEdit();
    updateAppearance();
    SetTimer(created, 1, 33, nullptr);
    ShowWindow(created, show);
    UpdateWindow(created);
    MSG msg {};
    int result = 0;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (msg.hwnd == ladderCountEdit && msg.message == WM_KEYDOWN) {
            const bool repeat = (msg.lParam & (1LL << 30)) != 0;
            if (msg.wParam == VK_TAB) {
                key(VK_TAB, repeat);
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                if (!repeat && panel == Panel::Main && !active) click(Start);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return result < 0 ? 1 : static_cast<int>(msg.wParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    Language fallback = Language::English;
    try {
        fallback = resolveLanguage(LanguageSetting::Auto);
        App app;
        result = app.run(instance, show);
    } catch (const std::exception&) {
        MessageBoxW(nullptr, Localizer(fallback).get(Text::UnexpectedError).data(), L"Golden Ear", MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
