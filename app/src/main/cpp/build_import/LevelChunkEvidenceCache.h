#ifndef INFINITE_TEXTURE_LEVEL_CHUNK_EVIDENCE_CACHE_H
#define INFINITE_TEXTURE_LEVEL_CHUNK_EVIDENCE_CACHE_H

#include "BuildImportTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace build_import {

class LevelChunkEvidenceCache {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit LevelChunkEvidenceCache(
            size_t capacity = 4096,
            Clock::duration retention = std::chrono::minutes(10))
        : capacity_(capacity), retention_(retention) {}

    void note(const ChunkCoord& coord, TimePoint received_at) {
        const uint64_t sequence = ++next_sequence_;
        latest_[coord] = {received_at, sequence};
        fifo_.push_back({received_at, coord, sequence});

        evictBefore(received_at - retention_);
        while (latest_.size() > capacity_ && !fifo_.empty()) {
            evictFront();
        }
    }

    bool wasReceivedSince(const ChunkCoord& coord, TimePoint cutoff) const {
        const auto found = latest_.find(coord);
        return found != latest_.end() && found->second.received_at >= cutoff;
    }

    size_t size() const noexcept { return latest_.size(); }

    void clear() noexcept {
        latest_.clear();
        fifo_.clear();
        next_sequence_ = 0;
    }

private:
    struct LatestEntry {
        TimePoint received_at;
        uint64_t sequence = 0;
    };

    struct QueueEntry {
        TimePoint received_at;
        ChunkCoord coord;
        uint64_t sequence = 0;
    };

    void evictBefore(TimePoint cutoff) {
        while (!fifo_.empty() && fifo_.front().received_at < cutoff) {
            evictFront();
        }
    }

    void evictFront() {
        const QueueEntry expired = fifo_.front();
        fifo_.pop_front();
        const auto found = latest_.find(expired.coord);
        if (found != latest_.end() && found->second.sequence == expired.sequence) {
            latest_.erase(found);
        }
    }

    size_t capacity_ = 0;
    Clock::duration retention_{};
    uint64_t next_sequence_ = 0;
    std::map<ChunkCoord, LatestEntry> latest_;
    std::deque<QueueEntry> fifo_;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_LEVEL_CHUNK_EVIDENCE_CACHE_H
