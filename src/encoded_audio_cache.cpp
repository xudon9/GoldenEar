#include "encoded_audio_cache.hpp"

#include "session.hpp"

#include <algorithm>
#include <utility>

namespace fs = std::filesystem;

std::optional<EncodedAudioCache::Key> EncodedAudioCache::makeKey(const fs::path& track, const Profile& profile) {
    std::error_code error;
    auto canonical = fs::canonical(track, error);
    if (error) return {};
    const auto modified = fs::last_write_time(canonical, error);
    if (error) return {};
    const auto size = fs::file_size(canonical, error);
    if (error) return {};
    return Key {std::move(canonical), modified, size, profile.codec, profile.sampleRate, profile.bitrate};
}

EncodedAudioCache::Encoded EncodedAudioCache::getOrEncode(const fs::path& track, const Profile& profile, const Encoder& encoder,
                                                         std::stop_token stop) {
    if (stop.stop_requested()) return {};
    const auto key = makeKey(track, profile);
    const auto encode = [&]() -> Encoded {
        if (stop.stop_requested()) return {};
        auto bytes = encoder();
        if (bytes.empty()) return {};
        return std::make_shared<const Bytes>(std::move(bytes));
    };
    if (!key) {
        auto encoded = encode();
        return stop.stop_requested() ? nullptr : encoded;
    }

    std::unique_lock lock(mutex);
    const auto requestGeneration = generation;
    if (!ready.wait(lock, stop, [&] { return !encoding.contains(*key); }) || stop.stop_requested()) return {};
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.key == *key; });
    if (found != entries.end()) {
        entries.splice(entries.begin(), entries, found);
        return found->encoded;
    }
    encoding.insert(*key);
    lock.unlock();

    Encoded encoded;
    try {
        encoded = encode();
        if (encoded && makeKey(track, profile) != key) encoded.reset();
    } catch (...) {
        lock.lock();
        encoding.erase(*key);
        ready.notify_all();
        throw;
    }

    lock.lock();
    encoding.erase(*key);
    ready.notify_all();
    if (encoded && requestGeneration == generation) remember(*key, encoded);
    return stop.stop_requested() ? nullptr : encoded;
}

void EncodedAudioCache::setByteLimit(std::size_t limit) {
    std::scoped_lock lock(mutex);
    byteLimit = limit;
    trim(byteLimit, entryLimit);
}

void EncodedAudioCache::clear() {
    std::scoped_lock lock(mutex);
    ++generation;
    entries.clear();
    cachedBytes = 0;
}

void EncodedAudioCache::remember(const Key& key, const Encoded& encoded) {
    const auto bytes = encoded->capacity();
    if (bytes > byteLimit) return;
    trim(byteLimit - bytes, entryLimit - 1);
    entries.push_front({key, encoded});
    cachedBytes += bytes;
}

void EncodedAudioCache::trim(std::size_t maxBytes, std::size_t maxEntries) {
    while (cachedBytes > maxBytes || entries.size() > maxEntries) {
        cachedBytes -= entries.back().encoded->capacity();
        entries.pop_back();
    }
}
