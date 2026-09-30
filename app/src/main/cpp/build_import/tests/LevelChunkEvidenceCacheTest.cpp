#include "LevelChunkEvidenceCache.h"

#include <cassert>
#include <chrono>
#include <iostream>

namespace {

using build_import::ChunkCoord;
using build_import::LevelChunkEvidenceCache;

void testEvictsOldestCurrentCoordinate() {
    LevelChunkEvidenceCache cache(2, std::chrono::hours(1));
    const auto start = LevelChunkEvidenceCache::TimePoint{};
    cache.note({1, 1}, start + std::chrono::seconds(1));
    cache.note({2, 2}, start + std::chrono::seconds(2));
    cache.note({3, 3}, start + std::chrono::seconds(3));

    assert(cache.size() == 2);
    assert(!cache.wasReceivedSince({1, 1}, start));
    assert(cache.wasReceivedSince({2, 2}, start));
    assert(cache.wasReceivedSince({3, 3}, start));
}

void testRepeatedCoordinateDoesNotEvictNewEvidence() {
    LevelChunkEvidenceCache cache(2, std::chrono::hours(1));
    const auto same_time = LevelChunkEvidenceCache::TimePoint{} + std::chrono::seconds(1);
    cache.note({1, 1}, same_time);
    cache.note({2, 2}, same_time);
    cache.note({1, 1}, same_time);
    cache.note({3, 3}, same_time);

    assert(cache.size() == 2);
    assert(cache.wasReceivedSince({1, 1}, same_time));
    assert(!cache.wasReceivedSince({2, 2}, same_time));
    assert(cache.wasReceivedSince({3, 3}, same_time));
}

void testAgeEvictionSkipsStaleQueueEntries() {
    LevelChunkEvidenceCache cache(4, std::chrono::seconds(10));
    const auto start = LevelChunkEvidenceCache::TimePoint{};
    cache.note({1, 1}, start);
    cache.note({1, 1}, start + std::chrono::seconds(5));
    cache.note({2, 2}, start + std::chrono::seconds(11));

    assert(cache.wasReceivedSince({1, 1}, start + std::chrono::seconds(5)));
    cache.note({3, 3}, start + std::chrono::seconds(16));
    assert(!cache.wasReceivedSince({1, 1}, start));
    assert(cache.wasReceivedSince({2, 2}, start));
    assert(cache.wasReceivedSince({3, 3}, start));
}

void testClearRemovesMapAndQueueState() {
    LevelChunkEvidenceCache cache(1, std::chrono::hours(1));
    const auto now = LevelChunkEvidenceCache::TimePoint{};
    cache.note({1, 1}, now);
    cache.clear();
    cache.note({2, 2}, now);

    assert(cache.size() == 1);
    assert(!cache.wasReceivedSince({1, 1}, now));
    assert(cache.wasReceivedSince({2, 2}, now));
}

}  // namespace

int main() {
    testEvictsOldestCurrentCoordinate();
    testRepeatedCoordinateDoesNotEvictNewEvidence();
    testAgeEvictionSkipsStaleQueueEntries();
    testClearRemovesMapAndQueueState();
    std::cout << "LevelChunkEvidenceCache tests passed\n";
    return 0;
}
