#include "audio.hpp"
#include "i18n.hpp"
#include "encoded_audio_cache.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
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
        for (unsigned i = 0; i < Session::defaultTrialsPerLevel; ++i) {
            require(ladder.trial.profile == level, "Ladder advanced at the wrong time or to an excluded profile");
            require(ladder.trial.source(2) == ladder.trial.source(ladder.trial.match), "X must play its matching reference");
            const auto beforePreview = random;
            const auto next = ladder.previewNext();
            require(random == beforePreview, "Previewing must not consume a random draw");
            require(ladder.submit(ladder.trial.match), "A first answer should count");
            require(!ladder.submit(1 - ladder.trial.match), "A second answer must not count");
            const bool last = level == 4 && i + 1 == Session::defaultTrialsPerLevel;
            require(ladder.complete() == last && next.has_value() != last,
                    "Only the final answer should complete the default Ladder");
            ladder.advance(Advance::Next);
            if (next) {
                require(ladder.trial.track == next->track && ladder.trial.profile == next->profile &&
                        ladder.trial.lossless == next->lossless && ladder.trial.match == next->match,
                        "The preview must match the next trial, including ladder promotion");
            }
        }
    }
    require(ladder.trial.profile == 4 && ladder.answered == 3 * Session::defaultTrialsPerLevel && ladder.trial.response >= 0,
            "Completed Ladder should keep the final answer available");
    require(ladder.scores[0].correct == Session::defaultTrialsPerLevel && ladder.scores[2].correct == Session::defaultTrialsPerLevel &&
            ladder.scores[4].correct == Session::defaultTrialsPerLevel,
            "Answers should be scored in the correct profile");
    require(ladder.scores[0].adjustedP(ladder.enabled.size()) < 0.05, "A perfect ladder level must be able to show above-chance evidence");

    Session shuffled(Mode::Identify, Difficulty::Random, {1, 3}, 1, random, false, true, {}, 1);
    unsigned counts[2] {};
    bool pairs[2][2] {};
    for (int i = 0; i < 200; ++i) {
        const auto trial = shuffled.trial;
        require(trial.profile == 1 || trial.profile == 3, "Random picked an excluded profile");
        require(trial.track == 0 && trial.source(trial.lossless) == 0, "Reference assignment is inconsistent");
        ++counts[trial.profile == 3];
        pairs[trial.lossless][trial.match] = true;
        const auto next = shuffled.previewNext();
        shuffled.submit(1 - trial.lossless);
        shuffled.advance(Advance::Next);
        require(!shuffled.complete() && next && shuffled.trial.profile == next->profile &&
                shuffled.trial.lossless == next->lossless && shuffled.trial.match == next->match,
                "Random mode should keep drawing trials and match the preview");
    }
    require(counts[0] > 50 && counts[1] > 50 && pairs[0][0] && pairs[0][1] && pairs[1][0] && pairs[1][1],
            "Random profile and source assignments lack coverage");
    require(shuffled.scores[1].correct + shuffled.scores[3].correct == 0, "The MP3 side counted as lossless");
    Session identify(Mode::Identify, Difficulty::Random, {0}, 1, random);
    identify.submit(identify.trial.lossless);
    require(identify.scores[0].correct == 1, "Identify Hi-Fi did not score the lossless answer as correct");

    constexpr unsigned practiceCount = 2;
    Session practice(Mode::Abx, Difficulty::Ladder, {0, 2}, 1, random, true, true, {}, practiceCount);
    for (unsigned i = 0; i < practiceCount; ++i) {
        const auto next = practice.previewNext();
        require(practice.submit(practice.trial.match), "Practice should accept an answer");
        practice.advance(Advance::Next);
        require(next && practice.trial.profile == next->profile, "Practice should prefetch the next Ladder profile");
    }
    const auto& practiceScore = practice.scores[0];
    require(practice.trial.profile == 2 && practiceScore.total == practiceCount,
            "Practice answers should count toward Ladder advancement");
    require(practiceScore.correct == 0 && practiceScore.peakLogEvidence == 0,
            "Practice answers should not accumulate blind-test evidence");

    Session fixedTrack(Mode::Abx, Difficulty::Ladder, {0, 2}, 3, random, false, false, 2);
    require(fixedTrack.trial.track == 2 && fixedTrack.previewNext().value().track == 2,
            "Disabling shuffle should keep the previous track in a new session and its preview");
    fixedTrack.advance(Advance::Skip);
    require(fixedTrack.trial.track == 2 && fixedTrack.skipped == 1, "Skip should keep the track when shuffle is off");
    fixedTrack.advance(Advance::ChangeSong, 1);
    require(fixedTrack.trial.track == 1 && fixedTrack.trial.profile == 0 && fixedTrack.replaced == 1,
            "Choosing another track for an unanswered trial should keep the profile");
    for (unsigned i = 1; i < Session::defaultTrialsPerLevel; ++i) {
        fixedTrack.submit(fixedTrack.trial.match);
        fixedTrack.advance(Advance::Next);
        require(fixedTrack.trial.track == 1 && fixedTrack.trial.profile == 0, "The chosen track should persist without shuffle");
    }
    fixedTrack.submit(fixedTrack.trial.match);
    fixedTrack.advance(Advance::Next, 0);
    require(fixedTrack.trial.track == 0 && fixedTrack.trial.profile == 2 && fixedTrack.replaced == 1,
            "Picking a track for the next trial should respect Ladder promotion and preserve the answered trial");
    const auto alternative = fixedTrack.randomOtherTrack();
    require(alternative > 0 && alternative < fixedTrack.trackCount, "An explicit random pick must choose a different song with shuffle off");
    fixedTrack.advance(Advance::ChangeSong, alternative);
    require(fixedTrack.trial.track == alternative && fixedTrack.trial.profile == 2 && !fixedTrack.shuffleTracks,
            "The random replacement should keep the current quality and shuffle setting");
    fixedTrack.shuffleTracks = true;
    require(fixedTrack.previewNext().value().track != fixedTrack.trial.track, "Turning on shuffle should preview a different track");
    Session shuffledRestart(Mode::Abx, Difficulty::Random, {0}, 3, random, false, true, 1);
    require(shuffledRestart.trial.track != 1, "Starting a shuffled session should avoid the previous track");
    require(shuffled.randomOtherTrack() == shuffled.trial.track, "A single-song repertoire has no alternative");

    Session preset(Mode::Abx, Difficulty::Ladder, {0, 2}, 2, random, false, false, {}, 3);
    for (int i = 0; i < 2; ++i) {
        preset.submit(1 - preset.trial.match);
        preset.advance(Advance::Next);
        require(preset.trial.profile == 0, "Ladder should wait for the preset answer count before promoting");
    }
    preset.advance(Advance::Skip);
    preset.advance(Advance::ChangeSong, 1);
    require(preset.trial.profile == 0 && preset.scores[0].total == 2 && preset.previewNext().value().profile == 2,
            "Skips and song changes should leave the preset threshold unchanged");
    preset.submit(preset.trial.match);
    preset.advance(Advance::Next, 0);
    require(preset.trial.profile == 2 && preset.trial.track == 0 && preset.scores[0].total == 3,
            "The preset should advance to the next enabled profile, including when choosing a song");

    Session single(Mode::Abx, Difficulty::Ladder, {0}, 2, random, false, false, {}, 1);
    require(!single.complete() && !single.previewNext(), "The only trial should not prefetch a successor");
    single.submit(single.trial.match);
    single.advance(Advance::Next);
    single.advance(Advance::Skip);
    single.advance(Advance::ChangeSong, 1);
    require(single.complete() && single.trial.track == 0 && single.trial.response >= 0 && single.answered == 1 &&
            single.skipped == 0 && single.replaced == 0, "A completed Ladder should keep its final trial and counts");
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

void testEncodedAudioCache() {
    const auto root = fs::current_path() / (L"mp3-cache-test-" + std::to_wstring(GetCurrentProcessId()));
    auto other = root;
    other += L"-other";
    struct Cleanup {
        const fs::path& first;
        const fs::path& second;
        ~Cleanup() {
            std::error_code error;
            fs::remove(first, error);
            fs::remove(second, error);
        }
    } cleanup {root, other};
    std::ofstream(root).put('a');
    std::ofstream(other).put('b');

    EncodedAudioCache cache(8);
    unsigned encodes = 0;
    const auto encode = [&] {
        ++encodes;
        return EncodedAudioCache::Bytes(4, 'm');
    };
    const auto first = cache.getOrEncode(root, profiles[0], encode);
    const auto second = cache.getOrEncode(other, profiles[0], encode);
    require(first && second && first != second && encodes == 2, "The source file must be part of the cache key");
    const auto alias = root.parent_path() / L"." / root.filename();
    require(cache.getOrEncode(alias, profiles[0], encode) == first && encodes == 2, "A hit must reuse the canonical file entry");
    const auto better = cache.getOrEncode(root, profiles[1], encode);
    require(better && better != first && encodes == 3, "The quality must be part of the cache key");
    require(cache.getOrEncode(root, profiles[0], encode) == first && encodes == 3, "A hit must refresh the LRU order");
    require(cache.getOrEncode(other, profiles[0], encode) != second && encodes == 4, "The cache must evict the least recently used MP3");

    std::ofstream(root, std::ios::app).put('c');
    require(cache.getOrEncode(root, profiles[0], encode) != first && encodes == 5, "A changed source must be encoded again");
    auto oversized = cache.getOrEncode(other, profiles[1], [] { return EncodedAudioCache::Bytes(9, 'x'); });
    const auto empty = [] { return EncodedAudioCache::Bytes {}; };
    require(oversized && oversized->size() == 9 && !cache.getOrEncode(other, profiles[1], empty),
            "An oversized MP3 may be used but must not be retained");
    require(!cache.getOrEncode(other, profiles[2], empty) && cache.getOrEncode(other, profiles[2], encode),
            "A failed request must allow another attempt");

    std::promise<void> started;
    std::promise<void> release;
    auto encoding = started.get_future();
    auto released = release.get_future();
    EncodedAudioCache::Encoded background;
    EncodedAudioCache::Encoded foreground;
    std::jthread prefetch([&] {
        background = cache.getOrEncode(root, profiles[4], [&] {
            started.set_value();
            released.wait();
            return EncodedAudioCache::Bytes(4, 'p');
        });
    });
    encoding.wait();
    std::jthread playback([&] { foreground = cache.getOrEncode(root, profiles[4], empty); });
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto stopped = cache.getOrEncode(root, profiles[4], empty, cancelled.get_token());
    release.set_value();
    prefetch.join();
    playback.join();
    require(!stopped && background && foreground == background, "Concurrent requests should share a successful encode");

    const std::weak_ptr<const EncodedAudioCache::Bytes> retained = background;
    background.reset();
    foreground.reset();
    require(!retained.expired(), "The cache should own its retained MP3");
    cache.clear();
    require(retained.expired() && !cache.getOrEncode(root, profiles[4], empty) &&
            !cache.getOrEncode(other, profiles[2], empty), "Clearing should release all retained MP3s");

    std::promise<void> encoderStarted;
    std::promise<void> finishEncode;
    auto running = encoderStarted.get_future();
    auto finish = finishEncode.get_future();
    EncodedAudioCache::Encoded completed;
    std::jthread inFlight([&] {
        completed = cache.getOrEncode(root, profiles[4], [&] {
            encoderStarted.set_value();
            finish.wait();
            return EncodedAudioCache::Bytes(4, 'm');
        });
    });
    running.wait();
    cache.clear();
    finishEncode.set_value();
    inFlight.join();
    require(completed && !cache.getOrEncode(root, profiles[4], empty), "An in-flight encode should finish without refilling the cleared cache");
    const auto fresh = cache.getOrEncode(root, profiles[4], encode);
    require(fresh && cache.getOrEncode(root, profiles[4], empty) == fresh, "A new request should cache its result after clearing");

    auto sameRateAac = profiles[4];
    sameRateAac.codec = Codec::Aac;
    const auto aac = cache.getOrEncode(root, sameRateAac, [] { return EncodedAudioCache::Bytes(4, 'a'); });
    require(aac && aac != fresh && cache.getOrEncode(root, profiles[4], empty) == fresh,
            "AAC and MP3 must use distinct cache entries even at the same sample rate and bitrate");

    EncodedAudioCache resized(12);
    const auto mostRecent = resized.getOrEncode(root, profiles[0], encode);
    resized.getOrEncode(root, profiles[1], encode);
    resized.getOrEncode(root, profiles[2], encode);
    resized.getOrEncode(root, profiles[0], empty);
    resized.setByteLimit(4);
    require(resized.getOrEncode(root, profiles[0], empty) == mostRecent &&
            !resized.getOrEncode(root, profiles[1], empty) && !resized.getOrEncode(root, profiles[2], empty),
            "Lowering the budget must immediately evict the least recently used entries");

    resized.setByteLimit(8);
    const auto extraEntry = resized.getOrEncode(root, profiles[1], encode);
    require(extraEntry && resized.getOrEncode(root, profiles[1], empty) == extraEntry &&
            resized.getOrEncode(root, profiles[0], empty) == mostRecent,
            "Raising the budget must allow more cached audio without replacing existing entries");

    const auto uncached = resized.getOrEncode(other, profiles[0], [&] {
        resized.setByteLimit(0);
        return encode();
    });
    require(uncached && !resized.getOrEncode(other, profiles[0], empty) &&
            !resized.getOrEncode(root, profiles[0], empty) && !resized.getOrEncode(root, profiles[1], empty),
            "Disabling the cache must clear retained entries and let an in-flight encode finish without caching");
}

void testCodecs() {
    constexpr int rate = 96000;
    const auto path = fs::temp_directory_path() / (L"Golden Ear 测试 " + std::to_wstring(GetCurrentProcessId()) + L".wav");
    struct Cleanup {
        const fs::path& path;
        ~Cleanup() { std::error_code error; fs::remove(path, error); }
    } cleanup {path};
    makeWav(path, rate);
    EncodedAudioCache cache;
    const auto noEncode = [] { return EncodedAudioCache::Bytes {}; };
    for (const auto& profile : profiles) {
        const auto prepared = prepareAudio(cache, path, profile, rate);
        if (!prepared.audio) {
            std::wcerr << L"Audio issue " << static_cast<int>(prepared.error.issue) << L" " << prepared.error.detail << L'\n';
        }
        require(prepared.audio != nullptr, "Could not prepare a codec preset");
        const auto& pair = *prepared.audio;
        const auto encoded = cache.getOrEncode(path, profile, noEncode);
        require(encoded && !encoded->empty(), "Each codec preset should cache its encoded output");
        if (profile.codec == Codec::Ldac) {
            const auto frameBytes = static_cast<std::size_t>(profile.bitrate / 3); // 128 samples at 48 kHz.
            require(encoded->size() % frameBytes == 0 && static_cast<unsigned char>(encoded->front()) == 0xaa,
                    "LDAC frame size must match the selected bitrate");
        } else if (profile.codec == Codec::Sbc) {
            require(encoded->size() >= 4 && static_cast<unsigned char>((*encoded)[0]) == 0x9c &&
                    static_cast<unsigned char>((*encoded)[1]) == 0xfd && (*encoded)[2] == (profile.bitrate == 237 ? 33 : 51),
                    "SBC must use the configured 48 kHz A2DP joint-stereo preset");
        }
        require(pair.sourceWriteTime == fs::last_write_time(path), "Prepared audio must identify the source file version");
        require(pair.frames() >= 2 * rate - 12 && pair.samples[0].size() == pair.samples[1].size(),
                "Gapless decode did not preserve the reference length");
        double energy[2] {};
        for (std::size_t side = 0; side < 2; ++side) {
            constexpr auto block = PreparedAudio::peakBlockFrames;
            const auto& samples = pair.samples[side];
            const std::array ranges {LoopRange {block - 3, 3 * block + 5}, LoopRange {block, block + 1},
                                     LoopRange {pair.frames() - block - 1, pair.frames()}};
            for (const auto range : ranges) {
                const auto [low, high] = std::minmax_element(samples.begin() + 2 * range.start, samples.begin() + 2 * range.end);
                const auto peak = pair.waveformPeak(static_cast<int>(side), range.start, range.end);
                require(peak.low == *low && peak.high == *high, "Waveform peaks must match the exact displayed stereo range");
            }
            for (float sample : samples) {
                require(std::abs(sample) <= 0.981f, "Signal clipped or contained non-finite values");
                energy[side] += static_cast<double>(sample) * sample;
            }
        }
        require(energy[0] > 0 && std::abs(energy[1] / energy[0] - 1) < 1e-5, "Whole-track RMS is mismatched");

        // A unique transient and several frequencies make an untrimmed codec delay visible.
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
        require(best > 0.95 && std::abs(bestLag) <= 2, "Compensated codec output is not aligned with the reference");
        std::wcout << Localizer().sampleRate(profile.sampleRate) << L" / " << profile.bitrate << L" kbps | lag "
                   << bestLag << L" frames | correlation " << best << L'\n';
    }
    std::stop_source stop;
    stop.request_stop();
    EncodedAudioCache prefetched;
    prefetchAudio(prefetched, path, profiles[0], stop.get_token());
    require(!prefetched.getOrEncode(path, profiles[0], noEncode), "Cancelled prefetch must leave the cache empty");
    require(prepareAudio(cache, path, profiles[0], rate, stop.get_token()).error.issue == AudioIssue::Cancelled,
            "Cancelled preparation returned the wrong error");
    prefetchAudio(prefetched, path, profiles[0]);
    require(prefetched.getOrEncode(path, profiles[0], noEncode) != nullptr, "Prefetch should populate the audio cache");
    for (const auto& profile : {profiles.front(), profiles.back()}) {
        const auto foreground = prepareAudio(cache, path, profile, rate);
        const auto background = prepareAudio(prefetched, path, profile, rate);
        require(foreground.audio && background.audio && foreground.audio->samples == background.audio->samples,
                "Cached and newly encoded audio should produce the same aligned samples");
    }
    fs::path missing = path;
    missing += L".missing";
    require(prepareAudio(cache, missing, profiles[0], rate).error.issue == AudioIssue::TrackMissing, "Missing track returned the wrong error");
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
            testEncodedAudioCache();
        }
        std::cout << "Passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
