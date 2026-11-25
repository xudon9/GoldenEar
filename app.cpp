#include "audio.hpp"

#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <cwctype>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace {

constexpr int width = 1180;
constexpr int height = 740;
constexpr COLORREF background = RGB(17, 19, 23);
constexpr COLORREF surface = RGB(25, 28, 34);
constexpr COLORREF raised = RGB(32, 36, 43);
constexpr COLORREF hovered = RGB(42, 47, 56);
constexpr COLORREF line = RGB(48, 54, 64);
constexpr COLORREF textColor = RGB(241, 242, 239);
constexpr COLORREF muted = RGB(166, 176, 187);
constexpr COLORREF accent = RGB(222, 243, 145);
constexpr COLORREF accentBg = RGB(48, 56, 35);
constexpr COLORREF errorColor = RGB(255, 177, 158);

constexpr RECT rect(int x, int y, int w, int h) { return {x, y, x + w, y + h}; }

enum Shortcut { HearA, HearB, HearX, ToggleAB, Play, Back, Forward, AnswerA, AnswerB, KeyCount };
constexpr std::array<int, KeyCount> defaultKeys {'J', 'K', 'L', VK_OEM_1, VK_SPACE, VK_OEM_COMMA, VK_OEM_PERIOD, '1', '2'};
constexpr std::array<const wchar_t*, KeyCount> shortcutNames {
    L"Listen to A", L"Listen to B", L"Listen to X", L"Toggle A/B", L"Play / pause",
    L"Back five seconds", L"Forward five seconds", L"Answer A", L"Answer B"
};

enum Control {
    Library = 1, Shortcuts, Import, Start, Finish, Skip, Change, Next, Retry, Seek,
    PlayPause, Rewind, FastForward, SelectA, SelectB, SelectX, Toggle, SubmitA, SubmitB,
    PreviousPage, NextPage, ClosePanel, ResetKeys,
    ModeBase = 40, DifficultyBase = 50, ProfileBase = 60, KeyBase = 70, RemoveBase = 100
};

enum class Panel { Main, Library, Shortcuts };
enum class Font { Small, Body, Bold, Title, Hero, Sample, Mono, Count };
enum class Style { Normal, Primary, Selected, Quiet };

struct Hit {
    int id;
    RECT area;
};

std::wstring keyName(int key) {
    if (key == VK_SPACE) return L"Space";
    if (key == VK_OEM_1) return L";";
    if (key == VK_OEM_COMMA) return L",";
    if (key == VK_OEM_PERIOD) return L".";
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::wstring(1, static_cast<wchar_t>(key));
    wchar_t name[40] {};
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const LONG packed = static_cast<LONG>((scan & 0xff) << 16) | ((scan & 0xff00) ? (1 << 24) : 0);
    if (GetKeyNameTextW(packed, name, static_cast<int>(std::size(name)))) return name;
    return L"Key " + std::to_wstring(key);
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

const wchar_t* modeName(Mode mode) {
    constexpr std::array names {L"ABX", L"Identify Hi-Fi", L"Preference"};
    return names[static_cast<std::size_t>(mode)];
}

wchar_t sideName(int side) { return L"ABX"[side]; }

std::wstring clockText(std::size_t frames, int rate) {
    const auto seconds = frames / rate;
    return std::to_wstring(seconds / 60) + L":" + (seconds % 60 < 10 ? L"0" : L"") + std::to_wstring(seconds % 60);
}

std::wstring percent(unsigned hits, unsigned total) {
    return std::to_wstring(static_cast<unsigned>(std::lround(100.0 * hits / total))) + L"%";
}

std::wstring pText(double value) {
    if (value < 0.001) return L"p < .001";
    if (value > 0.05 && value < 0.0505) return L"p > .050";
    std::wostringstream out;
    out << L"p = " << std::fixed << std::setprecision(3) << value;
    return out.str();
}

struct Settings {
    std::vector<fs::path> tracks;
    std::array<int, KeyCount> keys = defaultKeys;
    Mode mode = Mode::Abx;
    Difficulty difficulty = Difficulty::Random;
    unsigned enabled = (1U << profiles.size()) - 1;
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
        if (!(in >> version) || version != "GoldenEar1") return;
        int savedMode = 0;
        int savedDifficulty = 0;
        unsigned savedEnabled = 0;
        if (!(in >> savedMode >> savedDifficulty >> savedEnabled)) return;
        if (savedMode >= 0 && savedMode <= 2) mode = static_cast<Mode>(savedMode);
        if (savedDifficulty >= 0 && savedDifficulty <= 1) difficulty = static_cast<Difficulty>(savedDifficulty);
        if (savedEnabled & enabled) enabled &= savedEnabled;
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
        out << "GoldenEar1\n" << static_cast<int>(mode) << ' ' << static_cast<int>(difficulty) << ' ' << enabled << '\n';
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
    Canvas(HDC dc, const std::array<HFONT, static_cast<std::size_t>(Font::Count)>& fonts) : dc(dc), fonts(fonts) {
        SetBkMode(dc, TRANSPARENT);
    }

    void box(RECT area, COLORREF fill, COLORREF border, int radius = 12) const {
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, border);
        RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    }

    void stroke(int x1, int y1, int x2, int y2, COLORREF color) const {
        SetDCPenColor(dc, color);
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
    }

    void label(std::wstring_view value, RECT area, Font font, COLORREF color = textColor, UINT align = DT_LEFT) const {
        SelectObject(dc, fonts[static_cast<std::size_t>(font)]);
        SetTextColor(dc, color);
        DrawTextW(dc, value.data(), static_cast<int>(value.size()), &area,
                  align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    void paragraph(std::wstring_view value, RECT area, Font font, COLORREF color = muted) const {
        SelectObject(dc, fonts[static_cast<std::size_t>(font)]);
        SetTextColor(dc, color);
        DrawTextW(dc, value.data(), static_cast<int>(value.size()), &area, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

private:
    HDC dc;
    const std::array<HFONT, static_cast<std::size_t>(Font::Count)>& fonts;
};

class App {
public:
    App();
    ~App();
    int run(HINSTANCE instance, int show);

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT message(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    void draw(Canvas& canvas);
    void setup(Canvas& canvas);
    void listening(Canvas& canvas);
    void results(Canvas& canvas);
    void library(Canvas& canvas);
    void shortcuts(Canvas& canvas);
    void button(Canvas& canvas, int id, RECT area, std::wstring_view label, Style style = Style::Normal, bool enabled = true);
    void sample(Canvas& canvas, int side, RECT area);
    void transport(Canvas& canvas, Player::State state);

    void click(int id);
    void key(int key, bool repeat);
    void navigate(bool backwards);
    void save();
    void importTracks();
    void start();
    void prepare();
    void poll();
    void finish();
    void choose(int side);
    void movePlayhead(long long frame);
    void seekPoint(int logicalX);

    HWND window = nullptr;
    std::array<HFONT, static_cast<std::size_t>(Font::Count)> fonts {};
    Settings settings;
    Player player;
    std::mt19937_64 random {std::random_device{}()};
    std::optional<Session> session;
    Panel panel = Panel::Main;
    std::shared_ptr<PreparedAudio> audio;
    std::mutex resultMutex;
    std::optional<PreparedResult> prepared;
    std::jthread preparation;
    std::vector<Hit> hits;
    std::wstring error;
    std::wstring status;
    POINT pointer {-1, -1};
    int selected = 0;
    int lastAB = 0;
    int focused = 0;
    int pressed = 0;
    int capturing = -1;
    std::size_t page = 0;
    bool active = false;
    bool tracking = false;
    bool dirty = true;
    bool wasPlaying = false;

    static constexpr std::size_t pageSize = 8;
    static constexpr RECT seekArea = rect(66, 393, 652, 55);
};

App::App() {
    settings.load();
    const auto font = [&](Font type, int size, int weight, const wchar_t* family = L"Segoe UI") {
        fonts[static_cast<std::size_t>(type)] = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
    };
    font(Font::Small, 13, FW_NORMAL);
    font(Font::Body, 16, FW_NORMAL);
    font(Font::Bold, 16, FW_SEMIBOLD);
    font(Font::Title, 27, FW_SEMIBOLD);
    font(Font::Hero, 35, FW_SEMIBOLD);
    font(Font::Sample, 48, FW_SEMIBOLD);
    font(Font::Mono, 14, FW_NORMAL, L"Consolas");
}

App::~App() {
    if (preparation.joinable()) {
        preparation.request_stop();
        preparation.join();
    }
    for (HFONT font : fonts) DeleteObject(font);
}

void App::button(Canvas& canvas, int id, RECT area, std::wstring_view label, Style style, bool enabled) {
    const bool hot = enabled && PtInRect(&area, pointer);
    COLORREF fill = hot ? hovered : raised;
    COLORREF border = line;
    COLORREF ink = enabled ? textColor : RGB(109, 116, 127);
    if (style == Style::Quiet) fill = hot ? raised : surface;
    if (style == Style::Selected && enabled) {
        fill = hot ? RGB(61, 70, 47) : accentBg;
        border = RGB(108, 122, 67);
        ink = accent;
    }
    if (style == Style::Primary && enabled) {
        fill = hot ? RGB(234, 252, 174) : accent;
        border = fill;
        ink = background;
    }
    if (enabled && focused == id) border = textColor;
    if (enabled) hits.push_back({id, area});
    canvas.box(area, fill, border, 10);
    area.left += 5;
    area.right -= 5;
    canvas.label(label, area, Font::Bold, ink, DT_CENTER);
}

void App::draw(Canvas& canvas) {
    hits.clear();
    canvas.box(rect(39, 27, 35, 35), accent, accent, 11);
    for (int i = 0; i < 5; ++i) {
        constexpr int amplitudes[] = {4, 8, 11, 7, 4};
        canvas.stroke(47 + 5 * i, 44 - amplitudes[i], 47 + 5 * i, 45 + amplitudes[i], background);
        canvas.stroke(48 + 5 * i, 44 - amplitudes[i], 48 + 5 * i, 45 + amplitudes[i], background);
    }
    canvas.label(L"Golden Ear", rect(86, 19, 268, 38), Font::Title);
    canvas.label(L"Blind listening lab", rect(88, 57, 268, 18), Font::Small, muted);
    button(canvas, Library, rect(858, 26, 138, 40), L"Repertoire", panel == Panel::Library ? Style::Selected : Style::Quiet);
    button(canvas, Shortcuts, rect(1006, 26, 135, 40), L"Shortcuts", panel == Panel::Shortcuts ? Style::Selected : Style::Quiet);
    canvas.stroke(39, 91, 1141, 91, line);
    canvas.box(rect(39, 110, 705, 574), surface, line, 17);
    canvas.box(rect(762, 110, 379, 574), surface, line, 17);

    if (panel == Panel::Library) library(canvas);
    else if (panel == Panel::Shortcuts) shortcuts(canvas);
    else if (active) listening(canvas);
    else setup(canvas);

    results(canvas);
    if (!status.empty()) canvas.label(status, rect(40, 697, 1101, 25), Font::Small, muted);
}

void App::setup(Canvas& canvas) {
    canvas.label(session ? L"Start another session" : L"Set up a listening test", rect(64, 127, 654, 46), Font::Hero);
    canvas.label(L"Test type", rect(65, 190, 650, 25), Font::Bold);
    for (int i = 0; i < 3; ++i) {
        button(canvas, ModeBase + i, rect(65 + i * 221, 222, 211, 41), modeName(static_cast<Mode>(i)),
               static_cast<int>(settings.mode) == i ? Style::Selected : Style::Normal);
    }
    constexpr std::array descriptions {L"Match the hidden sample X to A or B.", L"Identify which sample is the lossless source.", L"Choose the version you prefer to hear."};
    canvas.label(descriptions[static_cast<std::size_t>(settings.mode)], rect(65, 268, 650, 28), Font::Small, muted);
    canvas.label(L"Difficulty", rect(65, 314, 650, 25), Font::Bold);
    button(canvas, DifficultyBase, rect(65, 345, 321, 41), L"Random", settings.difficulty == Difficulty::Random ? Style::Selected : Style::Normal);
    button(canvas, DifficultyBase + 1, rect(396, 345, 321, 41), L"Ladder", settings.difficulty == Difficulty::Ladder ? Style::Selected : Style::Normal);
    canvas.label(settings.difficulty == Difficulty::Random ? L"Draw any enabled MP3 quality each trial." :
                 L"Start low and move up after " + std::to_wstring(Session::ladderQuota) + L" answers per level.",
                 rect(65, 391, 650, 27), Font::Small, muted);
    canvas.label(L"MP3 profiles", rect(65, 440, 652, 25), Font::Bold);
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const int x = 65 + static_cast<int>(i) * 132;
        const bool selected = (settings.enabled & (1U << i)) != 0;
        button(canvas, ProfileBase + static_cast<int>(i), rect(x, 473, 124, 41), std::to_wstring(profiles[i].bitrate) + L" kbps",
               selected ? Style::Selected : Style::Normal);
        std::wstring rate(profiles[i].label);
        rate.resize(rate.find(L" /"));
        canvas.label(rate, rect(x, 521, 124, 18), Font::Small, muted, DT_CENTER);
    }
    canvas.stroke(65, 565, 717, 565, line);
    const auto tracks = settings.tracks.size();
    canvas.label(tracks == 0 ? L"Add a FLAC or WAV to begin" : std::to_wstring(tracks) + (tracks == 1 ? L" track ready" : L" tracks ready"),
                 rect(65, 589, 282, 47), Font::Body, muted);
    button(canvas, Import, rect(365, 589, 151, 47), L"Import audio");
    button(canvas, Start, rect(526, 589, 191, 47), L"Start session", Style::Primary, tracks != 0 && !preparation.joinable());
}

void App::results(Canvas& canvas) {
    canvas.label(L"Session results", rect(785, 132, 334, 37), Font::Title);
    if (session) {
        const auto& s = *session;
        const std::wstring meta = std::wstring(modeName(s.mode)) + L"  /  " + (s.difficulty == Difficulty::Random ? L"Random" : L"Ladder") +
            (active ? L"  /  In progress" : L"  /  Finished");
        canvas.label(meta, rect(787, 174, 330, 26), Font::Small, muted);
    } else {
        canvas.label(L"Scores appear after your first answer.", rect(787, 174, 330, 26), Font::Small, muted);
    }
    const bool preference = session ? session->mode == Mode::Preference : settings.mode == Mode::Preference;
    canvas.stroke(787, 215, 1116, 215, line);
    canvas.label(L"MP3 QUALITY", rect(787, 227, 150, 23), Font::Small, muted);
    canvas.label(preference ? L"HI-FI PICKS" : L"CORRECT", rect(933, 227, 81, 23), Font::Small, muted, DT_CENTER);
    canvas.label(preference ? L"SHARE" : L"ADJ. P", rect(1016, 227, 100, 23), Font::Small, muted, DT_RIGHT);
    int y = 254;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (session && std::find(session->enabled.begin(), session->enabled.end(), i) == session->enabled.end()) continue;
        if (!session && (settings.enabled & (1U << i)) == 0) continue;
        canvas.stroke(787, y, 1116, y, line);
        canvas.label(profiles[i].label, rect(787, y + 4, 150, 30), Font::Body);
        if (session && session->scores[i].total) {
            const auto& score = session->scores[i];
            const double p = score.adjustedP(session->enabled.size());
            canvas.label(std::to_wstring(score.correct) + L" / " + std::to_wstring(score.total), rect(933, y + 2, 81, 27), Font::Mono, textColor, DT_CENTER);
            if (!preference) canvas.label(percent(score.correct, score.total), rect(933, y + 26, 81, 19), Font::Small, muted, DT_CENTER);
            canvas.label(preference ? percent(score.correct, score.total) : pText(p), rect(1016, y + 2, 100, 27), Font::Mono,
                         !preference && p <= 0.05 ? accent : textColor, DT_RIGHT);
            if (!preference) canvas.label(p <= 0.05 ? L"Above chance" : L"Inconclusive", rect(1016, y + 26, 100, 19), Font::Small,
                                         p <= 0.05 ? accent : muted, DT_RIGHT);
        } else {
            canvas.label(L"—", rect(933, y + 4, 81, 30), Font::Mono, muted, DT_CENTER);
            canvas.label(L"—", rect(1016, y + 4, 100, 30), Font::Mono, muted, DT_RIGHT);
        }
        y += 49;
    }
    canvas.stroke(787, y, 1116, y, line);
    if (session) {
        const auto& s = *session;
        const std::wstring counts = std::to_wstring(s.answered) + L" answered   ·   " + std::to_wstring(s.skipped) +
            L" skipped   ·   " + std::to_wstring(s.replaced) + L" replaced";
        canvas.label(counts, rect(787, 517, 330, 24), Font::Small, muted);
    }
    canvas.paragraph(preference ? L"Share is the fraction of trials in which you preferred the lossless source." :
        L"Adjusted p-values test performance above 50% guessing. You can check them and stop at any time.",
        rect(787, 551, 330, 46), Font::Small);
    if (active) button(canvas, Finish, rect(787, 613, 330, 43), L"Finish session", Style::Quiet);
}

void App::sample(Canvas& canvas, int side, RECT area) {
    const int id = SelectA + side;
    const bool enabled = audio != nullptr;
    const bool on = enabled && side == selected;
    const bool hot = enabled && PtInRect(&area, pointer);
    const COLORREF border = focused == id ? textColor : on ? RGB(108, 122, 67) : line;
    canvas.box(area, on ? accentBg : hot ? hovered : raised, border, 15);
    if (enabled) hits.push_back({id, area});
    canvas.label(std::wstring(1, sideName(side)), rect(area.left + 18, area.top + 10, 86, 60), Font::Sample,
                 on ? accent : enabled ? textColor : muted);
    const auto key = keyName(settings.keys[HearA + side]);
    const int keyWidth = key.size() > 3 ? 69 : 30;
    const int keyX = area.right - keyWidth - 14;
    canvas.box(rect(keyX, area.top + 16, keyWidth, 24), on ? accentBg : surface, border, 6);
    canvas.label(key, rect(keyX + 3, area.top + 16, keyWidth - 6, 23), Font::Small, on ? accent : muted, DT_CENTER);
    canvas.label(on ? L"Selected" : side == 2 ? L"Unknown sample" : L"Reference sample", rect(area.left + 19, area.top + 73, area.right - area.left - 37, 25),
                 Font::Small, on ? accent : muted);
}

void App::transport(Canvas& canvas, Player::State state) {
    const bool ready = audio != nullptr;
    const auto frameCount = ready ? audio->frames() : 0;
    const int timelineX = seekArea.left;
    const int timelineW = seekArea.right - seekArea.left;
    const double progress = frameCount ? static_cast<double>(state.position) / frameCount : 0;
    const int playhead = timelineX + static_cast<int>(timelineW * progress);
    for (std::size_t i = 0; i < PreparedAudio::overviewSize; ++i) {
        const int x = timelineX + 2 + static_cast<int>((timelineW - 4) * i / PreparedAudio::overviewSize);
        const int amplitude = ready ? 2 + static_cast<int>(17 * std::sqrt(audio->overview[i])) : 2;
        const COLORREF waveform = ready && x <= playhead ? accent : RGB(72, 82, 96);
        canvas.stroke(x, 420 - amplitude, x, 421 + amplitude, waveform);
        canvas.stroke(x + 1, 420 - amplitude, x + 1, 421 + amplitude, waveform);
    }
    if (ready) {
        hits.push_back({Seek, seekArea});
        canvas.stroke(playhead, 396, playhead, 445, accent);
        if (focused == Seek) canvas.stroke(timelineX, 447, timelineX + timelineW, 447, textColor);
    }
    canvas.label(clockText(state.position, player.sampleRate()), rect(66, 449, 75, 20), Font::Mono, muted);
    canvas.label(clockText(frameCount, player.sampleRate()), rect(643, 449, 75, 20), Font::Mono, muted, DT_RIGHT);

    button(canvas, PlayPause, rect(66, 479, 156, 40), state.playing ? L"Pause" : L"Play", Style::Primary, ready);
    button(canvas, Rewind, rect(232, 479, 104, 40), L"−5 sec", Style::Normal, ready);
    button(canvas, FastForward, rect(346, 479, 104, 40), L"+5 sec", Style::Normal, ready);
    button(canvas, Toggle, rect(575, 479, 143, 40), L"Toggle A/B", Style::Quiet, ready);
}

void App::listening(Canvas& canvas) {
    const auto& s = *session;
    const auto& trial = s.trial;
    const std::wstring meta = L"TRIAL " + std::to_wstring(s.answered + s.skipped + s.replaced + (trial.response < 0 ? 1 : 0)) +
        L"  /  " + modeName(s.mode);
    canvas.label(meta, rect(66, 131, 420, 24), Font::Mono, accent);
    canvas.label(settings.tracks[trial.track].stem().wstring(), rect(64, 156, 654, 52), Font::Hero);

    if (audio) {
        const auto rate = player.sampleRate();
        const std::wstring label = L"Stereo comparison  ·  " + std::to_wstring(rate / 1000) + (rate == 44100 ? L".1 kHz playback" : L" kHz playback");
        canvas.label(label, rect(66, 212, 650, 27), Font::Small, muted);
    } else if (!error.empty()) {
        canvas.paragraph(error, rect(66, 207, 650, 49), Font::Small, errorColor);
    } else {
        canvas.label(L"Preparing both versions…", rect(66, 212, 650, 27), Font::Body, muted);
    }

    const int sides = s.mode == Mode::Abx ? 3 : 2;
    const int sampleWidth = sides == 3 ? 210 : 320;
    for (int side = 0; side < sides; ++side) sample(canvas, side, rect(66 + side * (sampleWidth + 11), 267, sampleWidth, 109));
    transport(canvas, player.state());
    canvas.stroke(66, 541, 718, 541, line);

    if (trial.response >= 0) {
        std::wstring headline;
        if (s.mode == Mode::Preference) {
            headline = std::wstring(L"You preferred ") + sideName(trial.response) + L". " + sideName(trial.lossless) + L" was lossless.";
        } else {
            headline = trial.response == trial.answer(s.mode) ? L"Correct. " : L"Incorrect. ";
            if (s.mode == Mode::Abx) headline += std::wstring(L"X matched ") + sideName(trial.match) + L". ";
            headline += std::wstring(1, sideName(trial.lossless)) + L" was lossless.";
        }
        canvas.label(headline, rect(66, 552, 648, 31), Font::Bold, s.mode == Mode::Preference || trial.response == trial.answer(s.mode) ? accent : textColor);
        canvas.label(std::wstring(L"MP3  ·  ") + profiles[trial.profile].label, rect(66, 592, 440, 41), Font::Body, muted);
        button(canvas, Next, rect(537, 593, 181, 43), L"Next trial", Style::Primary, !preparation.joinable());
        return;
    }

    constexpr std::array questions {L"Which sample matches X?", L"Which sample is lossless?", L"Which sample do you prefer?"};
    canvas.label(questions[static_cast<std::size_t>(s.mode)], rect(66, 552, 398, 28), Font::Bold);
    const bool idle = !preparation.joinable();
    if (!error.empty()) {
        button(canvas, Retry, rect(66, 590, 285, 45), L"Retry preparation", Style::Primary, idle);
    } else {
        button(canvas, SubmitA, rect(66, 590, 138, 45), L"A  /  " + keyName(settings.keys[AnswerA]), Style::Selected, audio != nullptr);
        button(canvas, SubmitB, rect(213, 590, 138, 45), L"B  /  " + keyName(settings.keys[AnswerB]), Style::Selected, audio != nullptr);
    }
    button(canvas, Skip, rect(456, 590, 100, 45), L"Skip", Style::Quiet, idle);
    button(canvas, Change, rect(566, 590, 152, 45), L"Change song", Style::Quiet, idle && s.trackCount > 1);
}

void App::library(Canvas& canvas) {
    canvas.label(L"Repertoire", rect(65, 128, 640, 47), Font::Hero);
    canvas.label(L"FLAC and WAV references", rect(66, 182, 648, 24), Font::Body, muted);
    canvas.stroke(66, 224, 718, 224, line);
    if (settings.tracks.empty()) {
        canvas.label(L"Import the source tracks you want to compare.", rect(66, 253, 636, 35), Font::Body, muted);
    }
    const auto first = page * pageSize;
    const auto end = std::min(settings.tracks.size(), first + pageSize);
    for (std::size_t i = first; i < end; ++i) {
        const int y = 230 + static_cast<int>(i - first) * 42;
        const auto& path = settings.tracks[i];
        canvas.label(path.filename().wstring(), rect(66, y + 2, 489, 22), Font::Bold);
        canvas.label(path.parent_path().wstring(), rect(66, y + 22, 489, 17), Font::Small, muted);
        if (!active) button(canvas, RemoveBase + static_cast<int>(i - first), rect(603, y + 4, 115, 31), L"Remove", Style::Quiet);
        canvas.stroke(66, y + 41, 718, y + 41, line);
    }
    if (settings.tracks.size() > pageSize) {
        button(canvas, PreviousPage, rect(66, 575, 92, 33), L"Previous", Style::Quiet, page > 0);
        button(canvas, NextPage, rect(167, 575, 74, 33), L"Next", Style::Quiet, end < settings.tracks.size());
        canvas.label(std::to_wstring(first + 1) + L"–" + std::to_wstring(end) + L" of " + std::to_wstring(settings.tracks.size()),
                     rect(253, 575, 158, 33), Font::Small, muted);
    }
    button(canvas, ClosePanel, rect(465, 611, 100, 42), L"Done", Style::Quiet);
    button(canvas, Import, rect(576, 611, 142, 42), L"Import audio", Style::Primary, !active);
}

void App::shortcuts(Canvas& canvas) {
    canvas.label(L"Keyboard shortcuts", rect(65, 128, 640, 47), Font::Hero);
    canvas.label(L"Select a key to change it. Press Esc to cancel.", rect(66, 182, 648, 24), Font::Body, muted);
    for (int i = 0; i < KeyCount; ++i) {
        const int column = i / 5;
        const int y = 235 + 64 * (i % 5);
        const int x = 66 + 335 * column;
        canvas.stroke(x, y, x + 316, y, line);
        canvas.label(shortcutNames[i], rect(x, y + 7, 210, 44), Font::Body);
        button(canvas, KeyBase + i, rect(x + 212, y + 11, 105, 36), capturing == i ? L"Press a key" : keyName(settings.keys[i]),
               capturing == i ? Style::Selected : Style::Normal);
    }
    button(canvas, ResetKeys, rect(66, 611, 159, 42), L"Reset defaults", Style::Quiet);
    button(canvas, ClosePanel, rect(602, 611, 116, 42), L"Done", Style::Primary);
}

void App::save() {
    if (!settings.save()) status = L"Settings could not be saved. Check the Golden Ear settings directory.";
}

void App::importTracks() {
    if (active) return;
    std::vector<wchar_t> filenames(65536);
    OPENFILENAMEW dialog {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Lossless audio (*.flac;*.wav)\0*.flac;*.wav\0\0";
    dialog.lpstrFile = filenames.data();
    dialog.nMaxFile = static_cast<DWORD>(filenames.size());
    dialog.lpstrTitle = L"Import into your repertoire";
    dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError()) status = L"The file picker could not return these files. Try selecting fewer files at once.";
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
        if (std::none_of(settings.tracks.begin(), settings.tracks.end(), [&](const auto& existing) {
            return CompareStringOrdinal(path.c_str(), -1, existing.c_str(), -1, TRUE) == CSTR_EQUAL;
        })) settings.tracks.push_back(path);
    };
    if (!*part) add(first);
    for (; *part; part += wcslen(part) + 1) add(first / part);
    const auto imported = settings.tracks.size() - oldCount;
    status = imported ? L"Imported " + std::to_wstring(imported) + (imported == 1 ? L" track." : L" tracks.") : L"No new FLAC or WAV tracks were added.";
    if (imported) save();
}

void App::start() {
    if (active || settings.tracks.empty() || preparation.joinable()) return;
    std::vector<std::size_t> enabled;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (settings.enabled & (1U << i)) enabled.push_back(i);
    }
    if (enabled.empty()) return;
    session.emplace(settings.mode, settings.difficulty, std::move(enabled), settings.tracks.size(), random);
    active = true;
    status.clear();
    prepare();
}

void App::prepare() {
    if (!active || preparation.joinable()) return;
    audio.reset();
    error.clear();
    player.load(nullptr, 0);
    selected = lastAB = 0;
    focused = 0;
    if (!player.open()) {
        error = L"Could not open a Windows output device. Connect or enable an audio device and retry.";
        return;
    }
    const auto track = settings.tracks[session->trial.track];
    const auto profile = profiles[session->trial.profile];
    const int rate = player.sampleRate();
    preparation = std::jthread([this, track, profile, rate](std::stop_token stop) {
        auto result = prepareAudio(track, profile, rate, stop);
        std::scoped_lock lock(resultMutex);
        prepared = std::move(result);
    });
}

void App::poll() {
    std::optional<PreparedResult> result;
    {
        std::scoped_lock lock(resultMutex);
        result.swap(prepared);
    }
    if (result) {
        preparation.join();
        if (active) {
            audio = std::move(result->audio);
            error = std::move(result->error);
            player.load(audio, session->trial.source(selected));
        }
        dirty = true;
    }
    if (active && audio) {
        const auto state = player.state();
        if (state.failed) {
            player.load(nullptr, 0);
            audio.reset();
            error = L"The audio output stopped. Check the Windows output device and retry.";
        }
        dirty |= state.playing || wasPlaying || state.failed;
        wasPlaying = state.playing;
    }
    if (dirty) InvalidateRect(window, nullptr, FALSE);
}

void App::finish() {
    if (!active) return;
    if (preparation.joinable()) preparation.request_stop();
    player.load(nullptr, 0);
    audio.reset();
    error.clear();
    active = false;
    capturing = -1;
    panel = Panel::Main;
    focused = 0;
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

void App::seekPoint(int logicalX) {
    if (!active || !audio) return;
    const double progress = std::clamp(static_cast<double>(logicalX - seekArea.left) / (seekArea.right - seekArea.left), 0.0, 1.0);
    movePlayhead(static_cast<long long>(progress * audio->frames()));
}

void App::click(int id) {
    status.clear();
    switch (id) {
    case Library:
        panel = panel == Panel::Library ? Panel::Main : Panel::Library;
        capturing = -1;
        page = 0;
        break;
    case Shortcuts:
        panel = panel == Panel::Shortcuts ? Panel::Main : Panel::Shortcuts;
        capturing = -1;
        break;
    case ClosePanel: panel = Panel::Main; capturing = -1; break;
    case Import: importTracks(); break;
    case Start: start(); break;
    case Finish: finish(); break;
    case Retry: prepare(); break;
    case Next: case Skip: case Change:
        if (!active || preparation.joinable()) break;
        if ((id == Next) != (session->trial.response >= 0)) break;
        if (id == Change && session->trackCount < 2) break;
        session->advance(id == Next ? Advance::Next : id == Skip ? Advance::Skip : Advance::ChangeSong);
        prepare();
        break;
    case Seek: seekPoint(pointer.x); break;
    case PlayPause: if (active && audio) player.toggle(); break;
    case Rewind: case FastForward:
        movePlayhead(static_cast<long long>(player.state().position) + (id == Rewind ? -5LL : 5LL) * player.sampleRate());
        break;
    case SelectA: case SelectB: case SelectX: choose(id - SelectA); break;
    case Toggle: choose(1 - lastAB); break;
    case SubmitA: case SubmitB:
        if (active && audio && session->submit(id - SubmitA)) focused = Next;
        break;
    case PreviousPage: if (page > 0) --page; break;
    case NextPage: if ((page + 1) * pageSize < settings.tracks.size()) ++page; break;
    case ResetKeys: settings.keys = defaultKeys; capturing = -1; save(); break;
    default:
        if (id >= ModeBase && id < ModeBase + 3 && !active) {
            settings.mode = static_cast<Mode>(id - ModeBase);
            save();
        } else if (id >= DifficultyBase && id < DifficultyBase + 2 && !active) {
            settings.difficulty = static_cast<Difficulty>(id - DifficultyBase);
            save();
        } else if (id >= ProfileBase && id < ProfileBase + static_cast<int>(profiles.size()) && !active) {
            const auto enabled = settings.enabled ^ (1U << (id - ProfileBase));
            if (enabled) { settings.enabled = enabled; save(); }
            else status = L"Keep at least one MP3 profile enabled.";
        } else if (id >= KeyBase && id < KeyBase + static_cast<int>(KeyCount)) {
            capturing = id - KeyBase;
        } else if (id >= RemoveBase && id < RemoveBase + static_cast<int>(pageSize) && !active) {
            const auto index = page * pageSize + (id - RemoveBase);
            if (index < settings.tracks.size()) {
                settings.tracks.erase(settings.tracks.begin() + static_cast<std::ptrdiff_t>(index));
                if (page > 0 && page * pageSize >= settings.tracks.size()) --page;
                status = L"Track removed from the repertoire.";
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
    std::size_t index = found == hits.end() ? (backwards ? count - 1 : 0) :
        (static_cast<std::size_t>(found - hits.begin()) + (backwards ? count - 1 : 1)) % count;
    focused = hits[index].id;
    dirty = true;
}

void App::key(int code, bool repeat) {
    if (capturing >= 0) {
        if (repeat) return;
        if (code == VK_ESCAPE) {
            capturing = -1;
            status.clear();
        } else if (!bindable(code) || ((GetKeyState(VK_CONTROL) | GetKeyState(VK_SHIFT)) & 0x8000)) {
            status = L"Choose a single key without modifiers. Tab, Enter and Esc are reserved.";
        } else {
            const auto other = std::find(settings.keys.begin(), settings.keys.end(), code);
            if (other != settings.keys.end() && other - settings.keys.begin() != capturing) {
                status = std::wstring(L"That key already controls ") + shortcutNames[other - settings.keys.begin()] + L".";
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
    if (code == VK_RETURN && !repeat) {
        if (focused != Seek && std::any_of(hits.begin(), hits.end(), [&](const Hit& hit) { return hit.id == focused; })) click(focused);
        return;
    }
    if (!active || panel != Panel::Main || ((GetKeyState(VK_CONTROL) | GetKeyState(VK_SHIFT)) & 0x8000)) return;
    constexpr std::array controls {SelectA, SelectB, SelectX, Toggle, PlayPause, Rewind, FastForward, SubmitA, SubmitB};
    for (std::size_t i = 0; i < settings.keys.size(); ++i) {
        if (settings.keys[i] == code && (!repeat || i == Back || i == Forward)) click(controls[i]);
    }
}

void App::paint() {
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
        SetDCBrushColor(memory, background);
        FillRect(memory, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const auto layout = Layout::forWindow(window);
        SaveDC(memory);
        SetMapMode(memory, MM_ANISOTROPIC);
        SetWindowExtEx(memory, width, height, nullptr);
        SetViewportExtEx(memory, layout.w, layout.h, nullptr);
        SetViewportOrgEx(memory, layout.x, layout.y, nullptr);
        Canvas canvas(memory, fonts);
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
    case WM_SIZE: dirty = true; InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(); return 0;
    case WM_TIMER: poll(); return 0;
    case WM_KEYDOWN: key(static_cast<int>(wParam), (lParam & (1LL << 30)) != 0); return 0;
    case WM_GETMINMAXINFO: {
        auto& info = *reinterpret_cast<MINMAXINFO*>(lParam);
        info.ptMinTrackSize = {820, 530};
        return 0;
    }
    case WM_MOUSEMOVE:
        pointer = Layout::forWindow(window).point(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
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
        const bool inside = std::any_of(hits.begin(), hits.end(), [&](const Hit& hit) {
            return hit.id == pressedId && PtInRect(&hit.area, pointer);
        });
        if (inside) click(pressedId);
        return 0;
    }
    case WM_CAPTURECHANGED: pressed = 0; return 0;
    case WM_DESTROY: KillTimer(window, 1); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}

int App::run(HINSTANCE instance, int show) {
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = L"GoldenEarWindow";
    if (!RegisterClassW(&windowClass)) return 1;
    RECT desktop {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
    const int workW = desktop.right - desktop.left;
    const int workH = desktop.bottom - desktop.top;
    const double scale = std::min({1.0, (workW - 30) / static_cast<double>(width), (workH - 70) / static_cast<double>(height)});
    RECT outer = rect(0, 0, static_cast<int>(width * scale), static_cast<int>(height * scale));
    constexpr DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&outer, style, FALSE, 0);
    const int outerW = outer.right - outer.left;
    const int outerH = outer.bottom - outer.top;
    HWND created = CreateWindowExW(0, windowClass.lpszClassName, L"Golden Ear", style,
        desktop.left + (workW - outerW) / 2, desktop.top + (workH - outerH) / 2,
        outerW, outerH, nullptr, nullptr, instance, this);
    if (!created) return 1;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(created, 20, &dark, sizeof(dark));
    SetTimer(created, 1, 33, nullptr);
    ShowWindow(created, show);
    UpdateWindow(created);
    MSG msg {};
    int result = 0;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
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
    try {
        App app;
        result = app.run(instance, show);
    } catch (const std::exception&) {
        MessageBoxW(nullptr, L"Golden Ear encountered an unexpected Windows or memory error.", L"Golden Ear", MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
