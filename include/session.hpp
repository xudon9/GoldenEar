#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <random>
#include <vector>

enum class Codec { Mp3, Aac, Ldac, AptxHd, Sbc };
inline constexpr std::array codecs {Codec::Mp3, Codec::Aac, Codec::Ldac, Codec::AptxHd, Codec::Sbc};

enum class Scenario { None, AacEveryday, AacHigherRate, LdacMetro, LdacCafe, LdacStudy, AptxHome, SbcBasic, SbcHigh };

struct Profile {
    int sampleRate;
    int bitrate;
    Codec codec = Codec::Mp3;
    Scenario scenario = Scenario::None;
};

inline constexpr std::array<Profile, 13> profiles {{
    {16000, 32},
    {22050, 56},
    {32000, 96},
    {44100, 160},
    {48000, 320},
    {48000, 256, Codec::Aac, Scenario::AacEveryday},
    {48000, 320, Codec::Aac, Scenario::AacHigherRate},
    {48000, 330, Codec::Ldac, Scenario::LdacMetro},
    {48000, 660, Codec::Ldac, Scenario::LdacCafe},
    {48000, 990, Codec::Ldac, Scenario::LdacStudy},
    {48000, 576, Codec::AptxHd, Scenario::AptxHome},
    {48000, 237, Codec::Sbc, Scenario::SbcBasic},
    {48000, 345, Codec::Sbc, Scenario::SbcHigh}
}};

constexpr unsigned profileMask(Codec codec) {
    unsigned mask = 0;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].codec == codec) mask |= 1U << i;
    }
    return mask;
}

enum class Mode { Abx, Identify, Preference };
enum class Difficulty { Random, Ladder };
enum class Advance { Next, Skip, ChangeSong };

struct Score {
    unsigned total = 0;
    unsigned correct = 0;
    double peakLogEvidence = 0;

    void record(bool success) {
        ++total;
        correct += success;
        constexpr std::array alternatives {0.60, 0.70, 0.80, 0.90, 0.99};
        std::array<double, alternatives.size()> logRatios {};
        for (std::size_t i = 0; i < alternatives.size(); ++i) {
            const double p = alternatives[i];
            logRatios[i] = correct * std::log(2 * p) + (total - correct) * std::log(2 * (1 - p));
        }
        const double largest = *std::max_element(logRatios.begin(), logRatios.end());
        double sum = 0;
        for (double logRatio : logRatios) sum += std::exp(logRatio - largest);
        peakLogEvidence = std::max(peakLogEvidence, largest + std::log(sum / alternatives.size()));
    }

    double adjustedP(std::size_t familySize) const {
        return std::min(1.0, static_cast<double>(familySize) * std::exp(-peakLogEvidence));
    }
};

struct Trial {
    std::size_t track = 0;
    std::size_t profile = 0;
    int lossless = 0;
    int match = 0;
    int response = -1;

    int source(int side) const { return (side == 2 ? match : side) == lossless ? 0 : 1; }
    int answer(Mode mode) const { return mode == Mode::Abx ? match : lossless; }
};

class Session {
public:
    static constexpr unsigned defaultTrialsPerLevel = 12;
    static constexpr unsigned maxTrialsPerLevel = 999;

    Session(Mode mode, Difficulty difficulty, std::vector<std::size_t> enabled,
            std::size_t trackCount, std::mt19937_64& random, bool practice = false,
            bool shuffleTracks = true, std::optional<std::size_t> previousTrack = {},
            unsigned trialsPerLevel = defaultTrialsPerLevel)
        : mode(mode), difficulty(difficulty), enabled(std::move(enabled)), trackCount(trackCount), practice(practice),
          shuffleTracks(shuffleTracks), trialsPerLevel(std::clamp(trialsPerLevel, 1U, maxTrialsPerLevel)), random(random) {
        trial = draw(Advance::Next, ladderIndex, random, previousTrack);
    }

    std::optional<Trial> previewNext() const {
        const bool promote = ladderQuotaReached(trial.response < 0);
        if (promote && ladderIndex + 1 == enabled.size()) return {};
        auto previewRandom = random;
        return draw(Advance::Next, ladderIndex + promote, previewRandom, trial.track);
    }

    bool complete() const {
        return ladderIndex + 1 == enabled.size() && ladderQuotaReached(false);
    }

    std::size_t randomOtherTrack() {
        return trackCount > 1 ? drawTrack(random, trial.track) : trial.track;
    }

    void advance(Advance action, std::optional<std::size_t> track = {}) {
        if (complete()) return;
        if (action == Advance::Skip) ++skipped;
        if (action == Advance::ChangeSong) ++replaced;
        if (action == Advance::Next && ladderQuotaReached(false)) ++ladderIndex;
        trial = draw(action, ladderIndex, random, trial.track, track);
    }

    bool submit(int side) {
        if (trial.response != -1 || (side != 0 && side != 1)) return false;
        trial.response = side;
        auto& score = scores[trial.profile];
        if (practice) ++score.total;
        else score.record(side == trial.answer(mode));
        ++answered;
        return true;
    }

    const Mode mode;
    const Difficulty difficulty;
    const std::vector<std::size_t> enabled;
    const std::size_t trackCount;
    const bool practice;
    bool shuffleTracks;
    Trial trial;
    std::array<Score, profiles.size()> scores {};
    unsigned answered = 0;
    unsigned skipped = 0;
    unsigned replaced = 0;

private:
    std::size_t ladderIndex = 0;
    const unsigned trialsPerLevel;
    std::mt19937_64& random;

    bool ladderQuotaReached(bool pendingAnswer) const {
        return difficulty == Difficulty::Ladder &&
            scores[enabled[ladderIndex]].total >= trialsPerLevel - pendingAnswer;
    }

    std::size_t drawTrack(std::mt19937_64& generator, std::optional<std::size_t> previousTrack) const {
        auto track = std::uniform_int_distribution<std::size_t>(0, trackCount - 1 - previousTrack.has_value())(generator);
        if (previousTrack && track >= *previousTrack) ++track;
        return track;
    }

    Trial draw(Advance action, std::size_t levelIndex, std::mt19937_64& generator,
               std::optional<std::size_t> previousTrack, std::optional<std::size_t> chosenTrack = {}) const {
        const auto choose = [&](std::size_t count) {
            return std::uniform_int_distribution<std::size_t>(0, count - 1)(generator);
        };
        std::size_t track = chosenTrack.value_or(previousTrack.value_or(0));
        if (!chosenTrack && trackCount > 1 && (shuffleTracks || action == Advance::ChangeSong)) {
            track = drawTrack(generator, previousTrack);
        }
        std::size_t level = enabled[levelIndex];
        if (action == Advance::ChangeSong) {
            level = trial.profile;
        } else if (difficulty == Difficulty::Random) {
            level = enabled[choose(enabled.size())];
        }
        return {track, level, static_cast<int>(choose(2)), static_cast<int>(choose(2)), -1};
    }
};
