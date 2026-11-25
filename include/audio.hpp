#pragma once

#include "session.hpp"

#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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

enum class AudioIssue {
    None, Cancelled, PipeCreate, PipeConfigure, FileHandles, FfmpegUnavailable, FfmpegLaunchFailed,
    FfmpegFailed, Incomplete, TrackMissing, TrackTooLong, Empty, Memory, Io, OutputUnavailable, OutputStopped
};

struct AudioError {
    AudioIssue issue = AudioIssue::None;
    std::wstring detail;
    explicit operator bool() const { return issue != AudioIssue::None; }
};

struct PreparedResult {
    std::shared_ptr<PreparedAudio> audio;
    AudioError error;
};

struct EncodedMp3;
std::shared_ptr<const EncodedMp3> preencodeMp3(const std::filesystem::path& track, const Profile& profile,
                                              std::stop_token stop = {});
PreparedResult prepareAudio(const std::filesystem::path& track, const Profile& profile,
                            int outputRate, std::stop_token stop = {},
                            std::shared_ptr<const EncodedMp3> prefetched = {});

struct LoopRange {
    std::size_t start;
    std::size_t end; // Exclusive

    std::size_t frameAt(std::size_t origin, std::uint64_t elapsed) const {
        return start + (origin - start + elapsed) % (end - start);
    }
};

// Render an interleaved stereo range, wrapping within a valid loop when provided.
void renderAudio(const PreparedAudio& audio, std::size_t frame, float* output, std::size_t count,
                 int source, float& blend, int sampleRate, const LoopRange* loop = nullptr);

class Player {
public:
    Player() = default;
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool open();
    int sampleRate() const { return rate; }
    void load(std::shared_ptr<const PreparedAudio> next, int source, std::optional<LoopRange> range = {});
    void select(int source);
    void toggle();
    void seek(std::size_t frame);
    void setLoop(std::optional<LoopRange> range);

    struct State {
        std::size_t position;
        bool playing;
        bool failed;
        std::optional<LoopRange> loop;
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
    std::optional<LoopRange> loop;
    std::uint64_t submitted = 0;
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
    std::size_t frameAt(std::uint64_t elapsed) const;
    std::size_t position();
    void close();
};
