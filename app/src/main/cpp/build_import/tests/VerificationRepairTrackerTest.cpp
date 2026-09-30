#include "../VerificationRepairTracker.h"

#include <cassert>

using namespace build_import;

int main() {
    VerificationRepairTracker tracker;
    constexpr size_t sample_a = 17;
    constexpr size_t sample_b = 18;

    assert(tracker.canAttempt(sample_a, 2));
    assert(tracker.beginAttempt(sample_a) == 1);
    assert(tracker.canAttempt(sample_a, 2));
    assert(tracker.beginAttempt(sample_a) == 2);
    assert(!tracker.canAttempt(sample_a, 2));

    // A different sample in the same region starts with its own retry budget.
    assert(tracker.canAttempt(sample_b, 2));
    assert(tracker.beginAttempt(sample_b) == 1);
    assert(tracker.attemptsFor(sample_a) == 0);
    assert(tracker.attemptsFor(sample_b) == 1);

    tracker.markSamplePassed(sample_b);
    assert(tracker.attemptsFor(sample_b) == 0);
    assert(tracker.canAttempt(sample_b, 2));

    tracker.beginAttempt(sample_a);
    tracker.markSamplePassed(sample_b);
    assert(tracker.attemptsFor(sample_a) == 1);
    tracker.reset();
    assert(tracker.attemptsFor(sample_a) == 0);
    return 0;
}
