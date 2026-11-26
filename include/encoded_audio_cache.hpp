#pragma once

#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <vector>

enum class Codec;
struct Profile;

class EncodedAudioCache {
public:
    using Bytes = std::vector<char>;
    using Encoded = std::shared_ptr<const Bytes>;
    using Encoder = std::function<Bytes()>;

    static constexpr std::size_t defaultByteLimit = 128 * 1024 * 1024;
    explicit EncodedAudioCache(std::size_t byteLimit = defaultByteLimit) : byteLimit(byteLimit) {}

    // An empty result from the encoder is never cached. The callback runs outside the cache lock.
    Encoded getOrEncode(const std::filesystem::path& track, const Profile& profile, const Encoder& encoder,
                        std::stop_token stop = {});
    // Current encodes and their waiters may finish without putting their results back in the cache.
    void clear();

private:
    struct Key {
        std::filesystem::path track;
        std::filesystem::file_time_type modified;
        std::uintmax_t fileSize;
        Codec codec;
        int sampleRate;
        int bitrate;
        auto operator<=>(const Key&) const = default;
    };

    struct Entry {
        Key key;
        Encoded encoded;
    };

    static std::optional<Key> makeKey(const std::filesystem::path& track, const Profile& profile);
    void remember(const Key& key, const Encoded& encoded);

    static constexpr std::size_t entryLimit = 512;
    const std::size_t byteLimit;
    std::size_t cachedBytes = 0;
    std::uint64_t generation = 0;
    std::list<Entry> entries;
    std::set<Key> encoding;
    std::mutex mutex;
    std::condition_variable_any ready;
};
