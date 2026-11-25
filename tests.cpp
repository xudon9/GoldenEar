#include "audio.hpp"
#include "i18n.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void testLanguages() {
    using namespace std::literals;
    require(selectLanguage(std::array{L"de-DE"sv, L"ES-MX"sv, L"en-US"sv}) == Language::Spanish,
            "Detection did not use the first supported display language");
    require(selectLanguage(std::array{L"zh-Hans-CN"sv, L"en"sv}) == Language::SimplifiedChinese &&
            selectLanguage(std::array{L"zh-SG"sv}) == Language::SimplifiedChinese,
            "Automatic language did not recognize Simplified Chinese");
    require(selectLanguage(std::array{L"zh-TW"sv, L"zh-Hant"sv, L"es-ES"sv}) == Language::Spanish,
            "Traditional Chinese must use the next supported preference");
    require(selectLanguage(std::array{L"en-GB"sv, L"es"sv}) == Language::English &&
            selectLanguage(std::array{L"fr-CA"sv}) == Language::English && selectLanguage({}) == Language::English,
            "English priority or fallback is incorrect");

    constexpr std::array options {LanguageSetting::Auto, LanguageSetting::English, LanguageSetting::SimplifiedChinese, LanguageSetting::Spanish};
    for (const auto option : options) require(parseLanguageCode(languageCode(option)) == option, "Language code did not round-trip");
    require(parseLanguageCode("unknown") == LanguageSetting::Auto &&
            resolveLanguage(LanguageSetting::SimplifiedChinese) == Language::SimplifiedChinese &&
            resolveLanguage(LanguageSetting::Spanish) == Language::Spanish, "Manual override or unknown preference is incorrect");

    for (const auto language : {Language::English, Language::SimplifiedChinese, Language::Spanish}) {
        const Localizer locale(language);
        for (int i = 0; i < static_cast<int>(Text::Count); ++i) {
            const auto rendered = locale.format(static_cast<Text>(i), 1, 2, 3);
            require(!rendered.empty() && rendered.find_first_of(L"{}") == std::wstring::npos, "Translation is missing or has an invalid template");
        }
    }
    const Localizer spanish(Language::Spanish);
    require(spanish.sampleRate(22050) == L"22,05 kHz" && spanish.pValue(0.04) == L"p = 0,040", "Spanish decimal display is incorrect");
    require(Localizer().sampleRate(44100) == L"44.1 kHz", "English decimal display is incorrect");
    require(Localizer(Language::SimplifiedChinese).format(Text::AbxAnswer, L'A', L'B') == L"X 与 A 相同。B 是无损版本。",
            "Answer formatting lost the selected samples");
}

void testSessions() {
    std::mt19937_64 random(57);
    Session ladder(Mode::Abx, Difficulty::Ladder, {0, 2, 4}, 3, random);
    require(ladder.trial.profile == 0, "Ladder must start at the easiest enabled profile");
    auto track = ladder.trial.track;
    ladder.advance(Advance::ChangeSong);
    require(ladder.trial.track != track && ladder.trial.profile == 0 && ladder.replaced == 1,
            "Change song must keep the profile and replace the track");
    ladder.advance(Advance::Skip);
    require(ladder.trial.profile == 0 && ladder.answered == 0 && ladder.skipped == 1, "Skipping must not advance the ladder or score");
    for (std::size_t level : {0, 2, 4}) {
        for (unsigned i = 0; i < Session::ladderQuota; ++i) {
            require(ladder.trial.profile == level, "Ladder advanced at the wrong time or to an excluded profile");
            require(ladder.trial.source(2) == ladder.trial.source(ladder.trial.match), "X must play its matching reference");
            require(ladder.submit(ladder.trial.match), "A first answer should count");
            require(!ladder.submit(1 - ladder.trial.match), "A second answer must not count");
            ladder.advance(Advance::Next);
        }
    }
    require(ladder.trial.profile == 4 && ladder.answered == 3 * Session::ladderQuota, "Ladder must stay at the hardest level");
    require(ladder.scores[0].correct == Session::ladderQuota && ladder.scores[2].correct == Session::ladderQuota &&
            ladder.scores[4].correct == Session::ladderQuota,
            "Answers should be scored in the correct profile");
    require(ladder.scores[0].adjustedP(profiles.size()) < 0.05, "A perfect ladder level must be able to show above-chance evidence");

    Session shuffled(Mode::Identify, Difficulty::Random, {1, 3}, 1, random);
    unsigned counts[2] {};
    bool pairs[2][2] {};
    for (int i = 0; i < 200; ++i) {
        const auto trial = shuffled.trial;
        require(trial.profile == 1 || trial.profile == 3, "Random picked an excluded profile");
        require(trial.track == 0 && trial.source(trial.lossless) == 0, "Reference assignment is inconsistent");
        ++counts[trial.profile == 3];
        pairs[trial.lossless][trial.match] = true;
        shuffled.submit(1 - trial.lossless);
        shuffled.advance(Advance::Next);
    }
    require(counts[0] > 50 && counts[1] > 50 && pairs[0][0] && pairs[0][1] && pairs[1][0] && pairs[1][1],
            "Random profile and source assignments lack coverage");
    require(shuffled.scores[1].correct + shuffled.scores[3].correct == 0, "The MP3 side counted as lossless");
    Session preference(Mode::Preference, Difficulty::Random, {0}, 1, random);
    preference.submit(preference.trial.lossless);
    require(preference.scores[0].correct == 1, "Preference did not record a lossless pick");
}

void testEvidence() {
    Score score;
    require(score.adjustedP(1) == 1.0, "Zero answers must give no statistical evidence");
    score.record(true);
    require(std::abs(score.adjustedP(1) - 5.0 / (1.20 + 1.40 + 1.60 + 1.80 + 1.98)) < 1e-12,
            "Mixture likelihood ratio is incorrect");
    require(score.adjustedP(5) == 1.0, "Adjusted p must be capped at one");
    for (int i = 0; i < 16; ++i) score.record(true);
    const double peak = score.adjustedP(5);
    require(peak < 0.05 && score.adjustedP(1) * 5 == peak, "Strong evidence or profile correction is incorrect");
    for (int i = 0; i < 20; ++i) score.record(false);
    require(score.adjustedP(5) == peak, "Anytime p-value must use the running maximum evidence");
    Score guessing;
    for (int i = 0; i < 100; ++i) guessing.record(i % 2 == 0);
    require(guessing.adjustedP(1) > 0.05, "Alternating outcomes should not show above-chance evidence");
}

void testRender() {
    PreparedAudio audio;
    for (std::size_t i = 0; i < 230; ++i) {
        const auto value = static_cast<float>(i) / 1000;
        audio.samples[0].insert(audio.samples[0].end(), {value, -value});
        audio.samples[1].insert(audio.samples[1].end(), {value + 0.5f, -value - 0.5f});
    }
    std::array<float, 440> stream {};
    float blend = 0;
    renderAudio(audio, 0, stream.data(), 10, 0, blend, 48000);
    renderAudio(audio, 10, stream.data() + 20, 48, 1, blend, 48000);
    require(blend > 0.48f && blend < 0.52f, "Switch should crossfade briefly at the same frame");
    renderAudio(audio, 58, stream.data() + 116, 52, 1, blend, 48000);
    require(blend == 1.0f && stream[218] == audio.samples[1][218], "Switched audio restarted or drifted");
    renderAudio(audio, 110, stream.data() + 220, 110, 0, blend, 48000);
    require(blend == 0.0f && stream[438] == audio.samples[0][438], "Return switch restarted or drifted");
    for (std::size_t i = 2; i < stream.size(); i += 2) {
        require(std::abs(stream[i] - stream[i - 2]) < 0.01f && stream[i + 1] == -stream[i], "Crossfade introduced a discontinuity");
    }
}

void testLoopRender() {
    PreparedAudio audio;
    for (std::size_t frame = 0; frame < 100; ++frame) {
        const auto value = static_cast<float>(frame) / 100;
        audio.samples[0].insert(audio.samples[0].end(), {value, -value});
        audio.samples[1].insert(audio.samples[1].end(), {value / 2, -value / 2});
    }
    const LoopRange loop {50, 62};
    require(loop.frameAt(59, 2) == 61 && loop.frameAt(59, 3) == 50 && loop.frameAt(59, 28) == 51,
            "The playhead must wrap at the exclusive loop end");

    std::array<float, 56> output {};
    for (const int side : {0, 1}) {
        auto blend = static_cast<float>(side);
        renderAudio(audio, 59, output.data(), output.size() / 2, side, blend, 1000, &loop);
        for (std::size_t i = 0; i < output.size() / 2; ++i) {
            const auto offset = (9 + i) % 12;
            const auto frame = 50 + offset;
            const auto gain = std::min({offset, 11 - offset, std::size_t{2}}) / 2.0f;
            require(output[2 * i] == audio.samples[side][2 * frame] * gain && output[2 * i + 1] == -output[2 * i],
                    "The render must wrap within the buffer and apply identical edge fades to both sources");
        }
    }

    float blend = 0;
    renderAudio(audio, 60, output.data(), 4, 1, blend, 4000, &loop);
    require(blend == 0.5f && std::abs(output[6] - std::lerp(0.51f, 0.255f, 0.5f) / 5) < 1e-6f,
            "Switching source across the loop seam must preserve the crossfade and frame alignment");

    const LoopRange singleFrame {50, 51};
    renderAudio(audio, 50, output.data(), output.size() / 2, 0, blend, 1000, &singleFrame);
    require(output.front() == 0.5f && output.back() == -0.5f, "A one-frame selection must still wrap and render");
}

void writeNumber(std::ostream& file, std::uint32_t number, int bytes) {
    for (int i = 0; i < bytes; ++i) file.put(static_cast<char>((number >> (8 * i)) & 255));
}

void makeWav(const fs::path& filePath, int rate) {
    std::ofstream file(filePath, std::ios::binary);
    const int size = rate * 2 * 4;
    file.write("RIFF", 4);
    writeNumber(file, size + 36, 4);
    file.write("WAVEfmt ", 8);
    writeNumber(file, 16, 4);
    writeNumber(file, 1, 2);
    writeNumber(file, 2, 2);
    writeNumber(file, rate, 4);
    writeNumber(file, rate * 4, 4);
    writeNumber(file, 4, 2);
    writeNumber(file, 16, 2);
    file.write("data", 4);
    writeNumber(file, size, 4);
    for (int i = 0; i < 2 * rate; ++i) {
        const double t = static_cast<double>(i) / rate;
        double v = 0;
        for (double hz : {127.0, 317.0, 573.0, 1259.0, 2129.0}) {
            v += 0.13 * std::sin(2 * std::numbers::pi * t * hz * (1 + 0.032 * t));
        }
        const double transient = std::exp(-std::pow((t - 0.68) / 0.12, 2));
        const auto value = static_cast<int16_t>(32760 * (0.25 + 0.75 * transient) * v);
        writeNumber(file, static_cast<uint16_t>(value), 2);
        writeNumber(file, static_cast<uint16_t>(value * 0.7), 2);
    }
    require(file.good(), "Could not create WAV test input");
}

void testCodecs() {
    constexpr int rate = 96000;
    const auto path = fs::temp_directory_path() / (L"Golden Ear 测试 " + std::to_wstring(GetCurrentProcessId()) + L".wav");
    struct Cleanup {
        const fs::path& path;
        ~Cleanup() { std::error_code error; fs::remove(path, error); }
    } cleanup {path};
    makeWav(path, rate);
    for (const auto& profile : profiles) {
        const auto prepared = prepareAudio(path, profile, rate);
        if (!prepared.audio) {
            std::wcerr << L"Audio issue " << static_cast<int>(prepared.error.issue) << L" " << prepared.error.detail << L'\n';
        }
        require(prepared.audio != nullptr, "Could not prepare one of the MP3 profiles");
        const auto& pair = *prepared.audio;
        require(pair.frames() >= 2 * rate - 12 && pair.samples[0].size() == pair.samples[1].size(),
                "Gapless decode did not preserve the reference length");
        double energy[2] {};
        for (std::size_t side = 0; side < 2; ++side) {
            for (float sample : pair.samples[side]) {
                require(std::abs(sample) <= 0.981f, "Signal clipped or contained non-finite values");
                energy[side] += static_cast<double>(sample) * sample;
            }
        }
        require(energy[0] > 0 && std::abs(energy[1] / energy[0] - 1) < 1e-5, "Whole-track RMS is mismatched");

        // A unique transient and several frequencies make an untrimmed MP3 delay visible.
        double best = -1;
        int bestLag = 0;
        for (int lag = -12; lag <= 12; ++lag) {
            double dot = 0;
            double aEnergy = 0;
            double bEnergy = 0;
            for (int frame = rate / 4; frame < 7 * rate / 4; frame += 4) {
                const double a = pair.samples[0][2 * frame];
                const double b = pair.samples[1][2 * (frame + lag)];
                dot += a * b;
                aEnergy += a * a;
                bEnergy += b * b;
            }
            const double correlation = dot / std::sqrt(aEnergy * bEnergy);
            if (correlation > best) { best = correlation; bestLag = lag; }
        }
        require(best > 0.95 && std::abs(bestLag) <= 2, "Compensated MP3 is not aligned with the reference");
        std::wcout << Localizer().sampleRate(profile.sampleRate) << L" / " << profile.bitrate << L" kbps | lag "
                   << bestLag << L" frames | correlation " << best << L'\n';
    }
    std::stop_source stop;
    stop.request_stop();
    require(prepareAudio(path, profiles[0], rate, stop.get_token()).error.issue == AudioIssue::Cancelled,
            "Cancelled preparation returned the wrong error");
    fs::path missing = path;
    missing += L".missing";
    require(prepareAudio(missing, profiles[0], rate).error.issue == AudioIssue::TrackMissing, "Missing track returned the wrong error");
}

void testOutputDevice() {
    using namespace std::chrono_literals;
    Player player;
    require(player.open(), "Windows did not provide a compatible audio output device");
    const int rate = player.sampleRate();
    auto audio = std::make_shared<PreparedAudio>();
    for (auto& stream : audio->samples) stream.resize(6 * rate, 0.0f);
    player.load(audio, 0);
    player.toggle();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && player.state().position < static_cast<std::size_t>(rate / 8)) {
        std::this_thread::sleep_for(10ms);
    }
    const auto before = player.state();
    require(before.playing && !before.failed && before.position > 0, "Device playback clock did not start");
    for (const int source : {1, 0, 1}) {
        player.select(source);
        std::this_thread::sleep_for(25ms);
        require(player.state().position >= before.position, "A source switch reset the device playback clock");
    }
    require(player.state().playing && player.state().position > before.position, "A source switch interrupted device playback");
    player.toggle();
    const auto paused = player.state();
    std::this_thread::sleep_for(100ms);
    const auto still = player.state();
    require(!still.playing && still.position >= paused.position && still.position - paused.position < static_cast<std::size_t>(rate / 20),
            "The device did not pause");
    player.seek(rate);
    require(player.state().position == static_cast<std::size_t>(rate), "Seeking while paused did not set the playhead");
    player.toggle();
    std::this_thread::sleep_for(150ms);
    require(player.state().position > static_cast<std::size_t>(rate), "Playback did not resume after seeking");

    const LoopRange loop {static_cast<std::size_t>(rate / 4), static_cast<std::size_t>(rate / 2)};
    player.setLoop(loop);
    auto last = player.state().position;
    bool wrapped = false;
    int source = 0;
    const auto loopDeadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < loopDeadline && !wrapped) {
        std::this_thread::sleep_for(10ms);
        const auto state = player.state();
        require(state.playing && !state.failed && state.position >= loop.start && state.position < loop.end,
                "Loop playback must stay active and inside the selected range");
        wrapped = state.position < last;
        last = state.position;
        player.select(source);
        source = 1 - source;
    }
    require(wrapped, "The output device playhead did not wrap");

    player.toggle();
    player.seek(0);
    require(player.state().position == loop.start, "Seeking before the selected range must stop at its start");
    player.seek(rate);
    require(player.state().position == loop.end - 1, "Seeking beyond the selected range must stop at its end");
    player.setLoop({});
    require(!player.state().loop && player.state().position == loop.end - 1, "Clearing a paused loop changed the playhead");
    player.toggle();
    std::this_thread::sleep_for(150ms);
    require(player.state().position >= loop.end && player.state().playing, "Clearing a loop must restore full-track playback");
    std::cout << "Windows output clock verified at " << rate << " Hz\n";
}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--audio") {
            testCodecs();
        } else if (argc > 1 && std::string_view(argv[1]) == "--playback") {
            testOutputDevice();
        } else {
            testLanguages();
            testSessions();
            testEvidence();
            testRender();
            testLoopRender();
        }
        std::cout << "Passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
