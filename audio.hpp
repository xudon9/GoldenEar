#pragma once

#include "session.hpp"

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>

#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

struct PreparedAudio {
    static constexpr std::size_t overviewSize = 128;
    std::array<std::vector<float>, 2> samples;
    std::array<float, overviewSize> overview {};
    std::size_t frames() const { return samples[0].size() / 2; }
};

struct PreparedResult {
    std::shared_ptr<PreparedAudio> audio;
    std::wstring error;
};

PreparedResult prepareAudio(const std::filesystem::path& track, const Profile& profile,
                            int outputRate, std::stop_token stop = {});

// Render an interleaved stereo range. The caller owns the single frame cursor.
void renderAudio(const PreparedAudio& audio, std::size_t frame, float* output, std::size_t count,
                 int source, float& blend, int sampleRate);

class Player {
public:
    Player() = default;
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool open();
    int sampleRate() const { return rate; }
    void load(std::shared_ptr<const PreparedAudio> next, int source);
    void select(int source);
    void toggle();
    void seek(std::size_t frame);

    struct State {
        std::size_t position;
        bool playing;
        bool failed;
    };
    State state();

private:
    struct Buffer {
        WAVEHDR header {};
        std::vector<float> data;
        bool queued = false;
    };

    std::mutex mutex;
    HANDLE wake = nullptr;
    HWAVEOUT device = nullptr;
    int rate = 96000;
    std::array<Buffer, 4> buffers;
    std::thread worker;
    std::shared_ptr<const PreparedAudio> audio;
    std::size_t cursor = 0;
    std::size_t origin = 0;
    int target = 0;
    float blend = 0;
    bool playing = false;
    bool drained = false;
    bool closing = false;
    bool failed = false;

    void run();
    void fill();
    void reset(std::size_t frame);
    std::size_t position();
    void close();
};
