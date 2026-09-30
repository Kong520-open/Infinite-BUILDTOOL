#include "../BuildImportThroughput.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <limits>

using namespace build_import;

namespace {

bool near(double left, double right) {
    return std::fabs(left - right) < 1e-9;
}

}  // namespace

int main() {
    const BuildImportThroughputPolicy low = buildImportThroughputPolicy(20);
    assert(near(low.block_rate_per_second, 20.0));
    assert(near(low.block_burst, 2.0));
    // One second of bounded catch-up so a chunk-load or barrier pause does not
    // permanently forfeit the block budget accrued while it ran.
    assert(near(low.catch_up_block_burst, 20.0));
    assert(low.catch_up_block_burst > low.block_burst);
    assert(near(low.command_rate_per_second, 160.0));
    assert(near(low.command_burst, 8.0));
    assert(low.max_batch_commands == 8);
    assert(near(low.catch_up_command_burst, 16.0));
    assert(low.max_catch_up_batch_commands == 16);
    assert(low.dispatch_interval == std::chrono::milliseconds(50));

    const BuildImportThroughputPolicy medium = buildImportThroughputPolicy(5000);
    assert(near(medium.block_burst, 500.0));
    assert(near(medium.command_rate_per_second, 5000.0));
    assert(medium.max_batch_commands == 250);
    assert(medium.max_catch_up_batch_commands == 500);

    const BuildImportThroughputPolicy high =
        buildImportThroughputPolicy(kMaximumBlocksPerSecond);
    assert(near(high.block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond)));
    assert(near(high.block_burst,
                static_cast<double>(kMaximumBlocksPerSecond) * 0.1));
    assert(near(high.catch_up_block_burst,
                static_cast<double>(kMaximumBlocksPerSecond)));
    assert(near(high.command_rate_per_second, 10240.0));
    assert(near(high.command_burst, 512.0));
    assert(high.max_batch_commands == 512);
    assert(high.max_catch_up_batch_commands == 1024);

    const BuildImportThroughputPolicy invalid = buildImportThroughputPolicy(0);
    assert(near(invalid.block_rate_per_second, 1.0));
    assert(near(invalid.block_burst, 1.0));
    assert(invalid.max_batch_commands == 8);

    const BuildImportThroughputPolicy excessive =
        buildImportThroughputPolicy(kMaximumBlocksPerSecond + 1);
    assert(near(excessive.block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond)));
    assert(near(excessive.block_burst,
                static_cast<double>(kMaximumBlocksPerSecond) * 0.1));

    const BuildImportThroughputPolicy throttled =
        buildImportThroughputPolicy(kMaximumBlocksPerSecond, 0.5);
    assert(near(throttled.block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond) * 0.5));
    assert(near(throttled.command_rate_per_second, 5120.0));
    assert(throttled.max_batch_commands == 256);
    assert(throttled.max_catch_up_batch_commands == 512);

    const BuildImportThroughputPolicy minimum_throttle =
        buildImportThroughputPolicy(kMaximumBlocksPerSecond, 0.0);
    assert(near(minimum_throttle.block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond) * 0.125));
    assert(near(minimum_throttle.command_rate_per_second, 1280.0));
    assert(minimum_throttle.max_batch_commands == 64);
    assert(minimum_throttle.max_catch_up_batch_commands == 128);

    const BuildImportThroughputPolicy minimum_input =
        buildImportThroughputPolicy(std::numeric_limits<int32_t>::min());
    assert(near(minimum_input.block_rate_per_second, 1.0));
    assert(minimum_input.max_batch_commands == 8);
    const BuildImportThroughputPolicy maximum_input =
        buildImportThroughputPolicy(std::numeric_limits<int32_t>::max());
    assert(near(maximum_input.block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond)));
    assert(maximum_input.max_batch_commands == 512);

    const BuildImportThroughputPolicy nan_scale = buildImportThroughputPolicy(
        kMaximumBlocksPerSecond, std::numeric_limits<double>::quiet_NaN());
    assert(near(nan_scale.block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond) * 0.125));
    assert(nan_scale.max_batch_commands == 64);
    const BuildImportThroughputPolicy positive_infinity = buildImportThroughputPolicy(
        kMaximumBlocksPerSecond, std::numeric_limits<double>::infinity());
    assert(near(positive_infinity.block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond)));
    const BuildImportThroughputPolicy negative_infinity = buildImportThroughputPolicy(
        kMaximumBlocksPerSecond, -std::numeric_limits<double>::infinity());
    assert(near(negative_infinity.block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond) * 0.125));

    assert(kMaximumScheduledFillBlockCount == kMaximumFillBlockCount);
    assert(fillBlockLimitForRate(std::numeric_limits<int32_t>::min()) == 64);
    assert(fillBlockLimitForRate(20) == 64);
    assert(fillBlockLimitForRate(16383) == 32766);
    assert(fillBlockLimitForRate(16384) == kMaximumFillBlockCount);
    assert(fillBlockLimitForRate(std::numeric_limits<int32_t>::max()) ==
           kMaximumFillBlockCount);
    assert(dynamicFillBlockLimitForRate(std::numeric_limits<int32_t>::min()) == 32);
    assert(dynamicFillBlockLimitForRate(20) == 32);
    assert(dynamicFillBlockLimitForRate(8192) == kMaximumDynamicFillBlockCount);
    assert(dynamicFillBlockLimitForRate(std::numeric_limits<int32_t>::max()) ==
           kMaximumDynamicFillBlockCount);

    using Clock = BuildImportThroughputGovernor::Clock;
    const Clock::time_point start{};
    BuildImportThroughputGovernor governor(kMaximumBlocksPerSecond);
    assert(governor.dispatchReady(start));
    assert(governor.dispatchBatchLimit(start) == 512);
    governor.noteDispatch(start);
    assert(!governor.dispatchReady(start + std::chrono::milliseconds(49)));
    assert(governor.dispatchReady(start + std::chrono::milliseconds(50)));
    assert(governor.dispatchBatchLimit(start + std::chrono::milliseconds(50)) == 512);
    assert(governor.dispatchBatchLimit(start + std::chrono::milliseconds(75)) == 768);
    assert(governor.dispatchBatchLimit(start + std::chrono::milliseconds(100)) == 1024);
    assert(governor.dispatchBatchLimit(start + std::chrono::milliseconds(500)) == 1024);
    assert(governor.dispatchBatchLimit(start - std::chrono::milliseconds(1)) == 512);

    // A partially filled transport batch is charged by the actual command
    // count. This keeps the packet-rate limit exact without wasting the rest
    // of a full 50 ms dispatch slot when spool decoding misses its deadline.
    governor.resetDispatchCadence();
    governor.noteDispatch(start, 256);
    assert(!governor.dispatchReady(start + std::chrono::milliseconds(24)));
    assert(governor.dispatchReady(start + std::chrono::milliseconds(25)));
    governor.resetDispatchCadence();
    governor.noteDispatch(start, 1);
    assert(!governor.dispatchReady(start + std::chrono::microseconds(96)));
    assert(governor.dispatchReady(start + std::chrono::microseconds(98)));

    governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
    assert(governor.hasAckLatencySample());
    assert(near(governor.ackLatencyEwmaMilliseconds(), 200.0));
    assert(near(governor.rateScale(), 1.0));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(800));
    assert(near(governor.rateScale(), 0.75));
    assert(near(governor.policy().block_rate_per_second,
                static_cast<double>(kMaximumBlocksPerSecond) * 0.75));
    assert(near(governor.policy().command_rate_per_second, 7680.0));
    assert(governor.policy().max_batch_commands == 384);

    governor.noteBarrierAcknowledged(std::chrono::milliseconds(1600));
    assert(near(governor.rateScale(), 0.375));
    governor.noteBarrierTimeout();
    assert(near(governor.rateScale(), 0.1875));
    governor.noteBarrierTimeout();
    assert(near(governor.rateScale(), 0.125));
    governor.noteBarrierTimeout();
    assert(near(governor.rateScale(), 0.125));

    // Two consecutive healthy barriers recover additively; an ambiguous
    // middle-latency sample breaks the streak.
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(300));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
    assert(near(governor.rateScale(), 0.125));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
    assert(near(governor.rateScale(), 0.225));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
    assert(near(governor.rateScale(), 0.225));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
    assert(near(governor.rateScale(), 0.325));

    for (int recovery = 0; recovery < 20; ++recovery) {
        governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
        governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
    }
    assert(near(governor.rateScale(), 1.0));

    // A near-idle queue recovers with the doubled additive step.
    governor.noteBarrierTimeout();
    assert(near(governor.rateScale(), 0.5));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(50));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(50));
    assert(near(governor.rateScale(), 0.7));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(50));
    governor.noteBarrierAcknowledged(std::chrono::milliseconds(200));
    assert(near(governor.rateScale(), 0.8));

    governor.resetFeedback();
    assert(near(governor.rateScale(), 1.0));
    assert(!governor.hasAckLatencySample());
    governor.resetDispatchCadence();
    assert(governor.dispatchReady(start));
    return 0;
}
