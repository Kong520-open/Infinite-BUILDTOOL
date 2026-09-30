#ifndef INFINITE_TEXTURE_VERIFICATION_REPAIR_TRACKER_H
#define INFINITE_TEXTURE_VERIFICATION_REPAIR_TRACKER_H

#include <cstddef>
#include <cstdint>
#include <optional>

namespace build_import {

// Final verification may inspect several samples from one region. Repair
// attempts belong to the exact failing sample, not to the surrounding region:
// otherwise a later sample inherits an earlier sample's retries and can pause
// the import before receiving its own repair attempts.
class VerificationRepairTracker {
public:
    void reset() {
        sample_index_.reset();
        attempts_ = 0;
    }

    uint32_t attemptsFor(size_t sample_index) const {
        return sample_index_ && *sample_index_ == sample_index ? attempts_ : 0U;
    }

    bool canAttempt(size_t sample_index, uint32_t maximum_attempts) const {
        return attemptsFor(sample_index) < maximum_attempts;
    }

    uint32_t beginAttempt(size_t sample_index) {
        if (!sample_index_ || *sample_index_ != sample_index) {
            sample_index_ = sample_index;
            attempts_ = 0;
        }
        return ++attempts_;
    }

    void markSamplePassed(size_t sample_index) {
        if (sample_index_ && *sample_index_ == sample_index) reset();
    }

private:
    std::optional<size_t> sample_index_;
    uint32_t attempts_ = 0;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_VERIFICATION_REPAIR_TRACKER_H
