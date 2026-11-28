#include "audio.hpp"
#include "encoded_audio_cache.hpp"

#include <ldacBT.h>
#include <ldacdec.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace fs = std::filesystem;

namespace {

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
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

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

AudioError runFfmpeg(const std::wstring& arguments, std::vector<float>* samples, std::stop_token stop) {
    if (stop.stop_requested()) return {AudioIssue::Cancelled, {}};
    SECURITY_ATTRIBUTES inherit {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &inherit, 0)) return {AudioIssue::PipeCreate, {}};
    Handle readEnd(readRaw);
    Handle writeEnd(writeRaw);
    if (!SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0)) return {AudioIssue::PipeConfigure, {}};

    TempFile log;
    Handle error(CreateFileW(log.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, &inherit, TRUNCATE_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!error || !input) return {AudioIssue::FileHandles, {}};

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
        return {AudioIssue::FfmpegUnavailable, {}};
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
        return {AudioIssue::FfmpegLaunchFailed, {}};
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
    if (stop.stop_requested()) return {AudioIssue::Cancelled, {}};
    DWORD exitCode = 1;
    GetExitCodeProcess(process.get(), &exitCode);
    if (exitCode != 0) {
        return {AudioIssue::FfmpegFailed, diagnostic(error.get())};
    }
    if (readError != ERROR_BROKEN_PIPE || offset % (2 * sizeof(float)) != 0) {
        return {AudioIssue::Incomplete, {}};
    }
    return {};
}

AudioError decode(const fs::path& path, int rate, std::vector<float>& samples, std::stop_token stop,
                  const std::wstring& inputOptions = {}, const std::wstring& filter = {}) {
    return runFfmpeg(inputOptions + L" -i " + quote(path.wstring()) + L" -map 0:a:0 -t 900.01 " + filter +
                     L" -ac 2 -ar " + std::to_wstring(rate) + L" -c:a pcm_f32le -f f32le pipe:1", &samples, stop);
}

// These raw streams carry no delay or end-padding metadata. Trim at the codec's sample rate.
constexpr int aptxHdDelay = 90;
constexpr int sbcDelay = 73;
constexpr int ldacFrameSamples = LDACBT_ENC_LSU; // At 48 kHz.

std::wstring pcmFormat(int rate) {
    return L" -f f32le -ar " + std::to_wstring(rate) + L" -ac 2 ";
}

AudioError encodeLdac(const fs::path& track, const Profile& profile, const fs::path& output, std::stop_token stop) {
    TempFile pcm;
    if (auto error = runFfmpeg(L"-y -i " + quote(track.wstring()) + L" -map 0:a:0 -t 900.01" +
        pcmFormat(profile.sampleRate) + L"-c:a pcm_f32le " + quote(pcm.path.wstring()), nullptr, stop); error) return error;

    // The Sony encoder writes shared transform table pointers on every initialization.
    static std::mutex encoderMutex;
    const std::scoped_lock lock(encoderMutex);
    if (stop.stop_requested()) return {AudioIssue::Cancelled, {}};
    const auto quality = profile.bitrate == 990 ? LDACBT_EQMID_HQ :
                         profile.bitrate == 660 ? LDACBT_EQMID_SQ : LDACBT_EQMID_MQ;
    const std::unique_ptr<std::remove_pointer_t<HANDLE_LDAC_BT>, decltype(&ldacBT_free_handle)>
        encoder(ldacBT_get_handle(), ldacBT_free_handle);
    constexpr int bluetoothMtu = 679;
    if (!encoder || ldacBT_init_handle_encode(encoder.get(), bluetoothMtu, quality, LDACBT_CHANNEL_MODE_STEREO,
        LDACBT_SMPL_FMT_F32, profile.sampleRate) != 0 || ldacBT_get_bitrate(encoder.get()) != profile.bitrate) {
        return {AudioIssue::LdacFailed, {}};
    }

    std::ifstream input(pcm.path, std::ios::binary);
    std::ofstream encoded(output, std::ios::binary | std::ios::trunc);
    if (!input || !encoded) return {AudioIssue::Io, {}};
    std::array<float, 2 * ldacFrameSamples> samples {};
    std::array<unsigned char, LDACBT_MAX_NBYTES> stream {};
    constexpr auto blockSize = static_cast<std::streamsize>(sizeof samples);
    for (;;) {
        if (stop.stop_requested()) return {AudioIssue::Cancelled, {}};
        input.read(reinterpret_cast<char*>(samples.data()), blockSize);
        const auto read = input.gcount();
        if (input.bad() || read % (2 * sizeof(float)) != 0) return {AudioIssue::Io, {}};
        std::fill(samples.begin() + read / sizeof(float), samples.end(), 0.0f);
        int consumed = 0;
        int bytes = 0;
        int frames = 0;
        if (ldacBT_encode(encoder.get(), read ? samples.data() : nullptr, &consumed, stream.data(), &bytes, &frames) != 0 ||
            (read && consumed != blockSize)) return {AudioIssue::LdacFailed, {}};
        if (!encoded.write(reinterpret_cast<const char*>(stream.data()), bytes)) return {AudioIssue::Io, {}};
        if (!read && !bytes) break;
    }
    encoded.close();
    return encoded ? AudioError {} : AudioError {AudioIssue::Io, {}};
}

AudioError decodeLdac(const fs::path& path, int codecRate, int rate, std::vector<float>& samples, std::stop_token stop) {
    static std::once_flag tables;
    std::call_once(tables, ldacdecInitTables);
    ldacdec_t decoder {};
    ldacdecInit(&decoder);
    TempFile pcm;
    std::ifstream encoded(path, std::ios::binary);
    std::ofstream decoded(pcm.path, std::ios::binary | std::ios::trunc);
    if (!encoded || !decoded) return {AudioIssue::Io, {}};
    // Only process frames generated by our encoder, using the length from the transport header.
    constexpr int headerSize = 3;
    constexpr int maxFrameSize = headerSize + 512;
    std::array<std::uint8_t, maxFrameSize + 4> frame {};
    std::array<float, 2 * ldacFrameSamples> block {};
    bool first = true;
    for (;;) {
        if (stop.stop_requested()) return {AudioIssue::Cancelled, {}};
        if (!encoded.read(reinterpret_cast<char*>(frame.data()), headerSize)) break;
        const int payloadSize = (((frame[1] & 7) << 6) | (frame[2] >> 2)) + 1;
        if (frame[0] != 0xaa || (frame[1] >> 5) != 1 || ((frame[1] >> 3) & 3) != 2 ||
            !encoded.read(reinterpret_cast<char*>(frame.data() + headerSize), payloadSize)) return {AudioIssue::Incomplete, {}};
        int used = 0;
        if (ldacDecode(&decoder, frame.data(), block.data(), &used) != 0 || used != payloadSize + headerSize) {
            return {AudioIssue::LdacFailed, {}};
        }
        // The overlap-add filter delays the decoded signal by one frame.
        if (!first && !decoded.write(reinterpret_cast<const char*>(block.data()), sizeof block)) return {AudioIssue::Io, {}};
        first = false;
    }
    if (encoded.bad() || encoded.gcount() != 0) return {AudioIssue::Incomplete, {}};
    decoded.close();
    if (!decoded) return {AudioIssue::Io, {}};
    return decode(pcm.path, rate, samples, stop, pcmFormat(codecRate));
}

AudioError encodeAudio(const fs::path& track, const Profile& profile, const fs::path& output, std::stop_token stop) {
    if (profile.codec == Codec::Ldac) return encodeLdac(track, profile, output, stop);
    const auto sampleRate = std::to_wstring(profile.sampleRate);
    const auto bitrate = L" -b:a " + std::to_wstring(profile.bitrate) + L"k";
    std::wstring options;
    switch (profile.codec) {
    case Codec::Mp3: options = L" -c:a libmp3lame" + bitrate + L" -write_xing 1 -f mp3"; break;
    case Codec::Aac: options = L" -c:a aac -profile:a aac_low" + bitrate + L" -movie_timescale " + sampleRate + L" -f mp4"; break;
    case Codec::AptxHd:
        options = L" -af aresample=" + sampleRate + L",apad=pad_len=128 -c:a aptx_hd -f aptx_hd";
        break;
    case Codec::Sbc:
        // FFmpeg selects joint stereo below 180 kbps, then -q:a sets the A2DP bitpool.
        options = L" -af aresample=" + sampleRate + L",apad=pad_len=256 -c:a sbc -b:a 160k -q:a " +
            std::to_wstring(profile.bitrate == 237 ? 33 : 51) + L" -f sbc";
        break;
    case Codec::Ldac: break;
    }
    return runFfmpeg(L"-y -i " + quote(track.wstring()) + L" -map 0:a:0 -map_metadata -1 -t 900.01 -ac 2 -ar " +
        sampleRate + options + L" " + quote(output.wstring()), nullptr, stop);
}

AudioError decodeEncoded(const fs::path& path, const Profile& profile, int rate, std::vector<float>& samples,
                         std::stop_token stop) {
    switch (profile.codec) {
    case Codec::Ldac: return decodeLdac(path, profile.sampleRate, rate, samples, stop);
    case Codec::AptxHd:
        return decode(path, rate, samples, stop, L"-f aptx_hd -sample_rate " + std::to_wstring(profile.sampleRate),
                      L"-af atrim=start_sample=" + std::to_wstring(aptxHdDelay));
    case Codec::Sbc:
        return decode(path, rate, samples, stop, L"-f sbc", L"-af atrim=start_sample=" + std::to_wstring(sbcDelay));
    default: return decode(path, rate, samples, stop);
    }
}

EncodedAudioCache::Encoded requestEncodedAudio(EncodedAudioCache& cache, const fs::path& track, const Profile& profile, std::stop_token stop,
                                               std::optional<TempFile>& output, AudioError& error) {
    return cache.getOrEncode(track, profile, [&] {
        output.emplace();
        error = encodeAudio(track, profile, output->path, stop);
        if (error) return EncodedAudioCache::Bytes {};
        std::ifstream file(output->path, std::ios::binary | std::ios::ate);
        const auto length = static_cast<std::streamoff>(file.tellg());
        if (length <= 0) throw std::runtime_error("Could not read encoded audio");
        EncodedAudioCache::Bytes bytes(static_cast<std::size_t>(length));
        file.seekg(0);
        if (!file.read(bytes.data(), length)) throw std::runtime_error("Could not read encoded audio");
        return bytes;
    }, stop);
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
    constexpr auto blockSize = PreparedAudio::peakBlockFrames;
    for (std::size_t side = 0; side < audio.samples.size(); ++side) {
        auto& samples = audio.samples[side];
        auto& blocks = audio.peakBlocks[side];
        const auto scale = gain[side] * headroom;
        blocks.resize((audio.frames() + blockSize - 1) / blockSize);
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const auto end = std::min((i + 1) * blockSize, audio.frames());
            auto& block = blocks[i];
            for (auto frame = i * blockSize; frame < end; ++frame) {
                auto& left = samples[2 * frame];
                auto& right = samples[2 * frame + 1];
                left *= scale;
                right *= scale;
                block.include(std::min(left, right), std::max(left, right));
            }
        }
    }

    const auto& reference = audio.samples[0];
    for (std::size_t i = 0; i < reference.size(); i += 2) {
        const auto bin = static_cast<std::uint64_t>(i) * PreparedAudio::overviewSize / reference.size();
        audio.overview[bin] = std::max({audio.overview[bin], std::abs(reference[i]), std::abs(reference[i + 1])});
    }
}

} // namespace

WaveformPeak PreparedAudio::waveformPeak(int source, std::size_t begin, std::size_t end) const {
    WaveformPeak peak;
    const auto& data = samples[source];
    const auto& blocks = peakBlocks[source];
    const auto scan = [&](std::size_t until) {
        for (; begin < until; ++begin) {
            const auto left = data[2 * begin];
            const auto right = data[2 * begin + 1];
            peak.include(std::min(left, right), std::max(left, right));
        }
    };
    // Use complete blocks for the interior. The edges need exact samples to stay within the view.
    if (!blocks.empty()) {
        const auto firstBlock = (begin + peakBlockFrames - 1) / peakBlockFrames;
        const auto endBlock = end / peakBlockFrames;
        scan(std::min(end, firstBlock * peakBlockFrames));
        for (auto block = firstBlock; block < endBlock; ++block) {
            peak.include(blocks[block].low, blocks[block].high);
            begin += peakBlockFrames;
        }
    }
    scan(end);
    return peak;
}

void prefetchAudio(EncodedAudioCache& cache, const fs::path& track, const Profile& profile, std::stop_token stop) {
    try {
        if (stop.stop_requested() || !fs::is_regular_file(track)) return;
        std::optional<TempFile> output;
        AudioError error;
        requestEncodedAudio(cache, track, profile, stop, output, error);
    } catch (const std::exception&) {
        // Foreground preparation can retry and report errors.
    }
}

PreparedResult prepareAudio(EncodedAudioCache& cache, const fs::path& track, const Profile& profile, int rate, std::stop_token stop) {
    try {
        if (!fs::is_regular_file(track)) return {{}, {AudioIssue::TrackMissing, {}}};
        auto audio = std::make_shared<PreparedAudio>();
        std::error_code timestampError;
        const auto modified = fs::last_write_time(track, timestampError);
        if (!timestampError) audio->sourceWriteTime = modified;
        if (auto error = decode(track, rate, audio->samples[0], stop); error) return {{}, std::move(error)};
        if (audio->frames() > static_cast<std::size_t>(rate) * 900) return {{}, {AudioIssue::TrackTooLong, {}}};
        if (audio->frames() == 0) return {{}, {AudioIssue::Empty, {}}};
        std::optional<TempFile> compressed;
        {
            AudioError error;
            const auto encoded = requestEncodedAudio(cache, track, profile, stop, compressed, error);
            if (!encoded) {
                if (error) return {{}, std::move(error)};
                return {{}, {stop.stop_requested() ? AudioIssue::Cancelled : AudioIssue::Io, {}}};
            }
            if (!compressed) {
                // MP3 and AAC use container metadata for gapless playback from a seekable input.
                compressed.emplace();
                std::ofstream file(compressed->path, std::ios::binary | std::ios::trunc);
                file.write(encoded->data(), static_cast<std::streamsize>(encoded->size()));
                file.close();
                if (!file) return {{}, {AudioIssue::Io, {}}};
            }
        }
        if (auto error = decodeEncoded(compressed->path, profile, rate, audio->samples[1], stop); error) {
            return {{}, std::move(error)};
        }
        if (audio->sourceWriteTime && (fs::last_write_time(track, timestampError) != *audio->sourceWriteTime || timestampError)) {
            return {{}, {AudioIssue::Io, {}}};
        }
        const auto commonSize = std::min(audio->samples[0].size(), audio->samples[1].size());
        if (!commonSize) return {{}, {AudioIssue::Empty, {}}};
        for (auto& samples : audio->samples) samples.resize(commonSize);
        matchLevels(*audio);
        return {std::move(audio), {}};
    } catch (const std::bad_alloc&) {
        return {{}, {AudioIssue::Memory, {}}};
    } catch (const std::exception&) {
        return {{}, {AudioIssue::Io, {}}};
    }
}

namespace {

void renderFrames(const PreparedAudio& audio, std::size_t frame, float* output, std::size_t count,
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

std::optional<LoopRange> clampLoop(std::optional<LoopRange> range, std::size_t frames) {
    if (!range) return {};
    range->end = std::min(range->end, frames);
    return range->start < range->end ? range : std::nullopt;
}

}

void renderAudio(const PreparedAudio& audio, std::size_t frame, float* output, std::size_t count,
                 int source, float& blend, int sampleRate, const LoopRange* loop) {
    if (!loop) {
        renderFrames(audio, frame, output, count, source, blend, sampleRate);
        return;
    }

    // Use the same edge fades for both versions to suppress clicks at the loop seam.
    const auto fade = std::min((loop->end - loop->start - 1) / 2, static_cast<std::size_t>(sampleRate / 500));
    while (count) {
        const auto chunk = std::min(count, loop->end - frame);
        renderFrames(audio, frame, output, chunk, source, blend, sampleRate);
        for (std::size_t i = 0; i < chunk; ++i) {
            const auto distance = std::min(frame + i - loop->start, loop->end - 1 - frame - i);
            if (distance < fade) {
                const auto gain = static_cast<float>(distance) / fade;
                output[2 * i] *= gain;
                output[2 * i + 1] *= gain;
            }
        }
        output += 2 * chunk;
        count -= chunk;
        frame = loop->start;
    }
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
    loop.reset();
    playing = false;
}

void Player::load(std::shared_ptr<const PreparedAudio> next, int source, std::optional<LoopRange> range) {
    std::scoped_lock lock(mutex);
    audio = std::move(next);
    playing = false;
    target = source;
    loop = clampLoop(range, audio ? audio->frames() : 0);
    reset(loop ? loop->start : 0);
}

void Player::reset(std::size_t frame) {
    if (device) waveOutReset(device);
    for (auto& buffer : buffers) buffer.queued = false;
    origin = frame;
    submitted = 0;
    drained = false;
    blend = static_cast<float>(target);
    if (device && playing) {
        if (waveOutRestart(device) == MMSYSERR_NOERROR) {
            SetEvent(wake);
        } else {
            playing = false;
            failed = true;
        }
    }
}

void Player::select(int source) {
    std::scoped_lock lock(mutex);
    target = source;
}

void Player::toggle() {
    std::scoped_lock lock(mutex);
    if (!device || !audio || failed) return;
    if (!playing && (drained || position() == audio->frames())) reset(loop ? loop->start : 0);
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
    reset(loop ? std::clamp(frame, loop->start, loop->end - 1) : std::min(frame, audio->frames()));
}

void Player::setLoop(std::optional<LoopRange> range) {
    std::scoped_lock lock(mutex);
    if (!device || !audio || failed || (!loop && !range)) return;
    const auto frame = position();
    loop = clampLoop(range, audio->frames());
    reset(loop ? loop->start : frame);
}

std::size_t Player::frameAt(std::uint64_t elapsed) const {
    return loop ? loop->frameAt(origin, elapsed) : static_cast<std::size_t>(origin + elapsed);
}

std::size_t Player::position() {
    if (!device || !audio) return 0;
    if (drained) return frameAt(submitted);
    MMTIME time {};
    time.wType = TIME_SAMPLES;
    if (waveOutGetPosition(device, &time, sizeof(time)) != MMSYSERR_NOERROR) return origin;
    std::uint64_t period = std::uint64_t{1} << 32;
    std::uint64_t elapsed = 0;
    switch (time.wType) {
    case TIME_SAMPLES: elapsed = time.u.sample; break;
    case TIME_BYTES:
        elapsed = time.u.cb / (2 * sizeof(float));
        period /= 2 * sizeof(float);
        break;
    case TIME_MS:
        elapsed = static_cast<std::uint64_t>(time.u.ms) * rate / 1000;
        period = period * rate / 1000;
        break;
    default: return origin;
    }
    // The device's 32-bit counter can wrap during a long-running loop.
    if (submitted >= elapsed) elapsed += (submitted - elapsed) / period * period;
    return frameAt(std::min(submitted, elapsed));
}

Player::State Player::state() {
    std::scoped_lock lock(mutex);
    return {position(), playing, failed || !device, loop};
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
        const auto frame = frameAt(submitted);
        if (!buffer.queued && frame < audio->frames()) {
            const auto capacity = buffer.data.size() / 2;
            const auto count = loop ? capacity : std::min(capacity, audio->frames() - frame);
            renderAudio(*audio, frame, buffer.data.data(), count, target, blend, rate, loop ? &*loop : nullptr);
            buffer.header.dwBufferLength = static_cast<DWORD>(2 * count * sizeof(float));
            if (waveOutWrite(device, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                playing = false;
                failed = true;
                waveOutReset(device);
                return;
            }
            buffer.queued = true;
            submitted += count;
        }
        queued |= buffer.queued;
    }
    if (!queued) {
        playing = false;
        drained = true;
    }
}
