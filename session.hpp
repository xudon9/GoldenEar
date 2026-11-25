#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
            std::size_t trackCount, std::mt19937_64& random)
        : mode(mode), difficulty(difficulty), enabled(std::move(enabled)), trackCount(trackCount), random(random) {
        trial = draw(Advance::Next, false);
    }

    void advance(Advance action) {
        if (action == Advance::Skip) ++skipped;
        if (action == Advance::ChangeSong) ++replaced;
        trial = draw(action, true);
    }

    bool submit(int side) {
        if (trial.response != -1 || (side != 0 && side != 1)) return false;
        trial.response = side;
        scores[trial.profile].record(side == trial.answer(mode));
        ++answered;
        return true;
    }

    const Mode mode;
    const Difficulty difficulty;
    const std::vector<std::size_t> enabled;
    const std::size_t trackCount;
    Trial trial;
    std::array<Score, profiles.size()> scores {};
    unsigned answered = 0;
    unsigned skipped = 0;
    unsigned replaced = 0;

private:
    std::size_t ladderIndex = 0;
    std::mt19937_64& random;

    std::size_t choose(std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(random);
    }

    Trial draw(Advance action, bool hasPrevious) {
        std::size_t track = 0;
        if (hasPrevious && trackCount > 1) {
            track = choose(trackCount - 1);
            track += track >= trial.track;
        } else if (trackCount > 1) {
            track = choose(trackCount);
        }
        std::size_t level;
        if (action == Advance::ChangeSong) {
            level = trial.profile;
        } else if (difficulty == Difficulty::Random) {
            level = enabled[choose(enabled.size())];
        } else {
            if (action == Advance::Next && ladderIndex + 1 < enabled.size() &&
                scores[enabled[ladderIndex]].total >= ladderQuota) ++ladderIndex;
            level = enabled[ladderIndex];
        }
        return {track, level, static_cast<int>(choose(2)), static_cast<int>(choose(2)), -1};
    }
};
