#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <random>
#include <vector>

struct Profile {
    int sampleRate;
    int bitrate;
};

inline constexpr std::array<Profile, 5> profiles {{
    {16000, 32},
    {22050, 56},
    {32000, 96},
    {44100, 160},
    {48000, 320}
}};

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
    static constexpr unsigned ladderQuota = 12;

    Session(Mode mode, Difficulty difficulty, std::vector<std::size_t> enabled,
            std::size_t trackCount, std::mt19937_64& random, bool practice = false,
            bool shuffleTracks = true, std::optional<std::size_t> previousTrack = {})
        : mode(mode), difficulty(difficulty), enabled(std::move(enabled)), trackCount(trackCount), practice(practice),
          shuffleTracks(shuffleTracks), random(random) {
        trial = draw(Advance::Next, ladderIndex, random, previousTrack);
    }

    Trial previewNext() const {
        auto previewRandom = random;
        const bool pendingAnswer = trial.response < 0 && trial.profile == enabled[ladderIndex];
        return draw(Advance::Next, nextLadderIndex(pendingAnswer), previewRandom, trial.track);
    }

    void advance(Advance action, std::optional<std::size_t> track = {}) {
        if (action == Advance::Skip) ++skipped;
        if (action == Advance::ChangeSong) ++replaced;
        if (action == Advance::Next) ladderIndex = nextLadderIndex(0);
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
    std::mt19937_64& random;

    std::size_t nextLadderIndex(unsigned pendingAnswers) const {
        if (difficulty == Difficulty::Ladder && ladderIndex + 1 < enabled.size() &&
            scores[enabled[ladderIndex]].total + pendingAnswers >= ladderQuota) return ladderIndex + 1;
        return ladderIndex;
    }

    Trial draw(Advance action, std::size_t levelIndex, std::mt19937_64& generator,
               std::optional<std::size_t> previousTrack, std::optional<std::size_t> chosenTrack = {}) const {
        const auto choose = [&](std::size_t count) {
            return std::uniform_int_distribution<std::size_t>(0, count - 1)(generator);
        };
        std::size_t track = chosenTrack.value_or(previousTrack.value_or(0));
        if (!chosenTrack && trackCount > 1 && (shuffleTracks || action == Advance::ChangeSong)) {
            track = choose(trackCount - previousTrack.has_value());
            if (previousTrack && track >= *previousTrack) ++track;
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
