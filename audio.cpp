#include "audio.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace {

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value; }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
    void reset() {
        if (*this) CloseHandle(value);
        value = nullptr;
    }
private:
    HANDLE value;
};

struct TempFile {
    fs::path path;
    TempFile() {
        const auto directory = fs::temp_directory_path().wstring();
        wchar_t name[MAX_PATH] {};
        if (!GetTempFileNameW(directory.c_str(), L"GEA", 0, name)) {
            throw std::runtime_error("Could not create an audio work file in the Windows temporary directory.");
        }
        path = name;
    }
    ~TempFile() { DeleteFileW(path.c_str()); }
};

// Quote an argument using Windows' backslash and double-quote rules.
std::wstring quote(std::wstring_view value) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(ch == L'"' ? 2 * backslashes + 1 : backslashes, L'\\');
        result += ch;
        backslashes = 0;
    }
    result.append(2 * backslashes, L'\\');
    result += L'"';
    return result;
}

std::wstring ffmpegPath() {
    const DWORD length = GetEnvironmentVariableW(L"GOLDEN_EAR_FFMPEG", nullptr, 0);
    if (length) {
        std::wstring path(length, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"GOLDEN_EAR_FFMPEG", path.data(), length);
        if (copied && copied < length) {
            path.resize(copied);
            return path;
        }
    }
    return L"ffmpeg.exe";
}

std::wstring diagnostic(HANDLE file) {
    SetFilePointer(file, 0, nullptr, FILE_BEGIN);
    char bytes[512] {};
    DWORD count = 0;
    if (!ReadFile(file, bytes, sizeof bytes, &count, nullptr) || count == 0) return {};
    UINT encoding = CP_UTF8;
    int size = MultiByteToWideChar(encoding, MB_ERR_INVALID_CHARS, bytes, count, nullptr, 0);
    if (!size) {
        encoding = CP_ACP;
        size = MultiByteToWideChar(encoding, 0, bytes, count, nullptr, 0);
    }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(encoding, 0, bytes, count, result.data(), size);
    for (wchar_t& ch : result) if (ch < L' ') ch = L' ';
    return result;
}

std::wstring runFfmpeg(const std::wstring& arguments, std::vector<float>* samples, std::stop_token stop) {
    if (stop.stop_requested()) return L"Audio preparation cancelled.";
    SECURITY_ATTRIBUTES inherit {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &inherit, 0)) return L"Could not create an FFmpeg output pipe.";
    Handle readEnd(readRaw);
    Handle writeEnd(writeRaw);
    if (!SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0)) return L"Could not configure the audio pipe.";

    TempFile log;
    Handle error(CreateFileW(log.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, &inherit, TRUNCATE_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!error || !input) return L"Could not open the FFmpeg log or input handle.";

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = writeEnd.get();
    startup.hStdError = error.get();
    PROCESS_INFORMATION info {};
    std::wstring command = quote(ffmpegPath()) + L" -nostdin -hide_banner -loglevel error " + arguments;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &info)) {
        return L"Could not start FFmpeg. Put an FFmpeg build with libmp3lame on PATH, or set GOLDEN_EAR_FFMPEG to its full path, then retry.";
    }
    Handle process(info.hProcess);
    Handle mainThread(info.hThread);
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit {};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const bool managed = job && SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit)) &&
        AssignProcessToJobObject(job.get(), process.get());
    const auto terminate = [&](UINT code) {
        if (!managed || !TerminateJobObject(job.get(), code)) TerminateProcess(process.get(), code);
    };
    const std::stop_callback cancel(stop, [&] { terminate(ERROR_CANCELLED); });
    if (ResumeThread(mainThread.get()) == static_cast<DWORD>(-1) && !stop.stop_requested()) {
        terminate(ERROR_INVALID_FUNCTION);
        WaitForSingleObject(process.get(), INFINITE);
        return L"Windows could not start the FFmpeg process.";
    }
    writeEnd.reset();

    std::array<char, 65536> bytes {};
    std::size_t offset = 0;
    DWORD count = 0;
    try {
        while (ReadFile(readEnd.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count) {
            if (!samples) continue;
            samples->resize((offset + count + sizeof(float) - 1) / sizeof(float));
            std::memcpy(reinterpret_cast<char*>(samples->data()) + offset, bytes.data(), count);
            offset += count;
        }
    } catch (...) {
        terminate(ERROR_NOT_ENOUGH_MEMORY);
        WaitForSingleObject(process.get(), INFINITE);
        throw;
    }
    const DWORD readError = GetLastError();
    readEnd.reset();
    WaitForSingleObject(process.get(), INFINITE);
    if (stop.stop_requested()) return L"Audio preparation cancelled.";
    DWORD exitCode = 1;
    GetExitCodeProcess(process.get(), &exitCode);
    if (exitCode != 0) {
        return L"FFmpeg could not prepare this track. Check that the file plays and FFmpeg has libmp3lame. " + diagnostic(error.get());
    }
    if (readError != ERROR_BROKEN_PIPE || offset % (2 * sizeof(float)) != 0) {
        return L"FFmpeg returned an incomplete audio stream.";
    }
    return {};
}

std::wstring decode(const fs::path& path, int rate, std::vector<float>& samples, std::stop_token stop) {
    return runFfmpeg(L"-i " + quote(path.wstring()) + L" -map 0:a:0 -t 900.01 -ac 2 -ar " +
                     std::to_wstring(rate) + L" -c:a pcm_f32le -f f32le pipe:1", &samples, stop);
}

void matchLevels(PreparedAudio& audio) {
    std::array<double, 2> energy {};
    std::array<float, 2> peak {};
    for (std::size_t side = 0; side < audio.samples.size(); ++side) {
        for (float& sample : audio.samples[side]) {
            if (!std::isfinite(sample)) sample = 0;
            energy[side] += static_cast<double>(sample) * sample;
            peak[side] = std::max(peak[side], std::abs(sample));
        }
    }
    std::array<float, 2> gain {1, 1};
    if (energy[0] > 0 && energy[1] > 0) {
        const auto quieter = std::min(energy[0], energy[1]);
        for (std::size_t i = 0; i < gain.size(); ++i) gain[i] = static_cast<float>(std::sqrt(quieter / energy[i]));
    }
    const float maxPeak = std::max(peak[0] * gain[0], peak[1] * gain[1]);
    const float headroom = maxPeak > 0.98f ? 0.98f / maxPeak : 1;
    for (std::size_t side = 0; side < audio.samples.size(); ++side) {
        for (float& sample : audio.samples[side]) sample *= gain[side] * headroom;
    }

    const auto& reference = audio.samples[0];
    for (std::size_t i = 0; i < reference.size(); i += 2) {
        const auto bin = i * PreparedAudio::overviewSize / reference.size();
        audio.overview[bin] = std::max({audio.overview[bin], std::abs(reference[i]), std::abs(reference[i + 1])});
    }
}

} // namespace

PreparedResult prepareAudio(const fs::path& track, const Profile& profile, int rate, std::stop_token stop) {
    try {
        if (!fs::is_regular_file(track)) return {{}, L"This track has moved or is unavailable. Choose another song or re-import it."};
        auto audio = std::make_shared<PreparedAudio>();
        if (auto error = decode(track, rate, audio->samples[0], stop); !error.empty()) return {{}, std::move(error)};
        if (audio->frames() > static_cast<std::size_t>(rate) * 900) return {{}, L"This track exceeds the 15-minute limit. Choose another song."};
        TempFile mp3;
        const std::wstring encode = L"-y -i " + quote(track.wstring()) + L" -map 0:a:0 -map_metadata -1 -ac 2 -ar " +
            std::to_wstring(profile.sampleRate) + L" -c:a libmp3lame -b:a " + std::to_wstring(profile.bitrate) +
            L"k -write_xing 1 -f mp3 " + quote(mp3.path.wstring());
        if (auto error = runFfmpeg(encode, nullptr, stop); !error.empty()) return {{}, std::move(error)};
        if (auto error = decode(mp3.path, rate, audio->samples[1], stop); !error.empty()) return {{}, std::move(error)};
        const auto commonSize = std::min(audio->samples[0].size(), audio->samples[1].size());
        if (!commonSize) return {{}, L"This file contains no playable audio."};
        for (auto& samples : audio->samples) samples.resize(commonSize);
        matchLevels(*audio);
        return {std::move(audio), {}};
    } catch (const std::bad_alloc&) {
        return {{}, L"There is too little memory to prepare both versions. Try a shorter track."};
    } catch (const std::exception&) {
        return {{}, L"Could not read the track or write temporary audio. Check the file and the Windows temporary directory."};
    }
}

void renderAudio(const PreparedAudio& audio, std::size_t frame, float* output, std::size_t count,
                 int source, float& blend, int sampleRate) {
    const auto offset = 2 * frame;
    const auto* first = audio.samples[0].data() + offset;
    const auto* second = audio.samples[1].data() + offset;
    const float destination = static_cast<float>(source);
    const float step = 1.0f / std::max(1, sampleRate / 500); // 2 ms
    std::size_t i = 0;
    while (i < 2 * count && blend != destination) {
        blend += std::clamp(destination - blend, -step, step);
        output[i] = std::lerp(first[i], second[i], blend);
        output[i + 1] = std::lerp(first[i + 1], second[i + 1], blend);
        i += 2;
    }
    const auto* active = source == 0 ? first : second;
    std::copy(active + i, active + 2 * count, output + i);
}

bool Player::open() {
    if (device && !state().failed) return true;
    close();
    closing = false;
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) return false;
    for (const int candidate : {96000, 48000, 44100}) {
        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = candidate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * sizeof(float);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (waveOutOpen(&device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(wake), 0,
                        CALLBACK_EVENT) == MMSYSERR_NOERROR) {
            rate = candidate;
            break;
        }
        device = nullptr;
    }
    if (!device) {
        close();
        return false;
    }
    for (auto& buffer : buffers) {
        buffer.data.resize(2 * (rate / 100));
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<char*>(buffer.data.data());
        buffer.header.dwBufferLength = static_cast<DWORD>(buffer.data.size() * sizeof(float));
        if (waveOutPrepareHeader(device, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            close();
            return false;
        }
    }
    failed = false;
    worker = std::thread(&Player::run, this);
    return true;
}

Player::~Player() { close(); }

void Player::close() {
    if (worker.joinable()) {
        {
            std::scoped_lock lock(mutex);
            closing = true;
            SetEvent(wake);
        }
        worker.join();
    }
    if (device) {
        waveOutReset(device);
        for (auto& buffer : buffers) {
            if (buffer.header.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(device, &buffer.header, sizeof(WAVEHDR));
            buffer.queued = false;
        }
        waveOutClose(device);
        device = nullptr;
    }
    if (wake) CloseHandle(wake);
    wake = nullptr;
    audio.reset();
    playing = false;
}

void Player::load(std::shared_ptr<const PreparedAudio> next, int source) {
    std::scoped_lock lock(mutex);
    audio = std::move(next);
    playing = false;
    target = source;
    reset(0);
}

void Player::reset(std::size_t frame) {
    if (device) waveOutReset(device);
    for (auto& buffer : buffers) buffer.queued = false;
    cursor = origin = frame;
    drained = false;
    blend = static_cast<float>(target);
    if (device && playing) SetEvent(wake);
}

void Player::select(int source) {
    std::scoped_lock lock(mutex);
    target = source;
}

void Player::toggle() {
    std::scoped_lock lock(mutex);
    if (!device || !audio || failed) return;
    if (!playing && (drained || position() == audio->frames())) reset(0);
    playing = !playing;
    const auto result = playing ? waveOutRestart(device) : waveOutPause(device);
    if (result != MMSYSERR_NOERROR) {
        playing = false;
        failed = true;
    }
    SetEvent(wake);
}

void Player::seek(std::size_t frame) {
    std::scoped_lock lock(mutex);
    if (!device || !audio) return;
    reset(std::min(frame, audio->frames()));
    if (playing) waveOutRestart(device);
}

std::size_t Player::position() {
    if (!device || !audio) return 0;
    if (drained) return cursor;
    MMTIME time {};
    time.wType = TIME_SAMPLES;
    if (waveOutGetPosition(device, &time, sizeof(time)) != MMSYSERR_NOERROR) return origin;
    if (time.wType == TIME_SAMPLES) return std::min(cursor, origin + time.u.sample);
    if (time.wType == TIME_BYTES) return std::min(cursor, origin + time.u.cb / (2 * sizeof(float)));
    if (time.wType == TIME_MS) return std::min(cursor, origin + static_cast<std::size_t>(time.u.ms) * rate / 1000);
    return origin;
}

Player::State Player::state() {
    std::scoped_lock lock(mutex);
    return {position(), playing, failed || !device};
}

void Player::run() {
    while (true) {
        WaitForSingleObject(wake, INFINITE);
        std::scoped_lock lock(mutex);
        if (closing) return;
        if (playing && audio) fill();
    }
}

void Player::fill() {
    bool queued = false;
    for (auto& buffer : buffers) {
        if (buffer.queued && (buffer.header.dwFlags & WHDR_DONE)) buffer.queued = false;
        if (!buffer.queued && cursor < audio->frames()) {
            const auto count = std::min(buffer.data.size() / 2, audio->frames() - cursor);
            renderAudio(*audio, cursor, buffer.data.data(), count, target, blend, rate);
            buffer.header.dwBufferLength = static_cast<DWORD>(2 * count * sizeof(float));
            if (waveOutWrite(device, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                playing = false;
                failed = true;
                waveOutReset(device);
                return;
            }
            buffer.queued = true;
            cursor += count;
        }
        queued |= buffer.queued;
    }
    if (!queued) {
        playing = false;
        drained = true;
    }
}
