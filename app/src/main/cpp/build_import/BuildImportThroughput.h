#ifndef INFINITE_TEXTURE_BUILD_IMPORT_THROUGHPUT_H
#define INFINITE_TEXTURE_BUILD_IMPORT_THROUGHPUT_H

#include "BuildImportTypes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace build_import {

struct BuildImportThroughputPolicy {
    double block_rate_per_second = 1.0;
    double block_burst = 1.0;
    // Placement is repeatedly suspended for chunk loading, queue barriers and
    // phase settling. Capping the bucket at the steady-state burst discarded
    // the block budget accrued during every such pause, so a job could never
    // reach its configured rate. One second of bounded catch-up lets the
    // scheduler recover a normal pause without turning a long stall into an
    // unbounded burst.
    double catch_up_block_burst = 1.0;
    double command_rate_per_second = 1.0;
    double command_burst = 1.0;
    size_t max_batch_commands = 1;
    double catch_up_command_burst = 1.0;
    size_t max_catch_up_batch_commands = 1;
    std::chrono::milliseconds dispatch_interval{50};
};

// Block throughput may be very high when one /fill represents thousands of
// blocks. Packet throughput is deliberately bounded independently so a
// fragmented blueprint cannot turn a high block target into an unsafe packet
// flood on the game thread or server command queue.
inline BuildImportThroughputPolicy buildImportThroughputPolicy(
        int32_t requested_blocks_per_second, double rate_scale = 1.0) {
    constexpr double kMinimumCommandRate = 160.0;
    constexpr double kMaximumCommandRate = 10240.0;
    constexpr double kMinimumAdaptiveScale = 0.125;
    constexpr double kMinimumAdaptiveCommandRate = 20.0;
    constexpr size_t kMinimumBatch = 1;
    constexpr size_t kMaximumBatch = 512;
    constexpr size_t kMaximumCatchUpBatch = 1024;
    constexpr double kBurstSeconds = 0.1;
    constexpr double kCatchUpBurstSeconds = 1.0;
    constexpr double kDispatchSeconds = 0.05;

    // Widen before clamping so this remains well-defined if the Java/JNI edge
    // ever supplies either int32 extreme. Non-finite feedback fails closed:
    // NaN and negative infinity select the protective minimum, while positive
    // infinity is simply equivalent to an unthrottled scale.
    const int64_t bounded_requested_rate = std::max<int64_t>(
        1, std::min<int64_t>(kMaximumBlocksPerSecond,
                             static_cast<int64_t>(requested_blocks_per_second)));
    const double requested_block_rate = static_cast<double>(bounded_requested_rate);
    const double scale = std::isfinite(rate_scale)
        ? std::max(kMinimumAdaptiveScale, std::min(1.0, rate_scale))
        : (rate_scale > 0.0 ? 1.0 : kMinimumAdaptiveScale);
    const double block_rate = std::max(1.0, requested_block_rate * scale);
    const double requested_command_rate = std::max(
        kMinimumCommandRate, std::min(kMaximumCommandRate, requested_block_rate));
    const double command_rate = std::max(
        kMinimumAdaptiveCommandRate, requested_command_rate * scale);
    const size_t batch = std::max(
        kMinimumBatch,
        std::min(kMaximumBatch,
                 static_cast<size_t>(std::ceil(command_rate * kDispatchSeconds))));
    const size_t catch_up_batch = std::max(
        batch,
        std::min(kMaximumCatchUpBatch, batch * 2));
    return {
        block_rate,
        std::max(1.0, block_rate * kBurstSeconds),
        std::max(1.0, block_rate * kCatchUpBurstSeconds),
        command_rate,
        static_cast<double>(batch),
        batch,
        static_cast<double>(catch_up_batch),
        catch_up_batch,
        std::chrono::milliseconds(50),
    };
}

// Keeps expensive C++ -> Python crossings close to 20 per second while still
// allowing one bounded catch-up batch after a delayed game tick. Congestion is
// learned only from queue-barrier acknowledgements: those ACKs are ordered
// after all preceding fill commands and therefore measure actual server queue
// pressure without requiring an ACK for every idempotent data command.
class BuildImportThroughputGovernor {
public:
    using Clock = std::chrono::steady_clock;

    explicit BuildImportThroughputGovernor(int32_t requested_blocks_per_second = 20)
        : requested_blocks_per_second_(requested_blocks_per_second) {}

    void setRequestedBlocksPerSecond(int32_t requested_blocks_per_second) {
        requested_blocks_per_second_ = requested_blocks_per_second;
    }

    BuildImportThroughputPolicy policy() const {
        return buildImportThroughputPolicy(requested_blocks_per_second_, rate_scale_);
    }

    bool dispatchReady(Clock::time_point now) const {
        if (!has_last_dispatch_) return true;
        return now >= next_dispatch_at_;
    }

    size_t dispatchBatchLimit(Clock::time_point now) const {
        const BuildImportThroughputPolicy current = policy();
        if (!has_last_dispatch_ || now <= last_dispatch_at_) {
            return current.max_batch_commands;
        }
        const auto elapsed = std::min(
            now - last_dispatch_at_,
            std::chrono::duration_cast<Clock::duration>(current.dispatch_interval * 2));
        const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        const size_t elapsed_budget = static_cast<size_t>(
            std::ceil(current.command_rate_per_second * elapsed_seconds));
        return std::max(
            current.max_batch_commands,
            std::min(current.max_catch_up_batch_commands, elapsed_budget));
    }

    void noteDispatch(Clock::time_point now) {
        noteDispatch(now, policy().max_batch_commands);
    }

    void noteDispatch(Clock::time_point now, size_t command_count) {
        const BuildImportThroughputPolicy current = policy();
        const double charged_seconds = std::min(
            std::chrono::duration<double>(current.dispatch_interval).count(),
            static_cast<double>(command_count) / current.command_rate_per_second);
        last_dispatch_at_ = now;
        next_dispatch_at_ = now + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(charged_seconds));
        has_last_dispatch_ = true;
    }

    void resetDispatchCadence() {
        last_dispatch_at_ = {};
        next_dispatch_at_ = {};
        has_last_dispatch_ = false;
    }

    void noteBarrierAcknowledged(std::chrono::milliseconds latency) {
        const double sample_ms = static_cast<double>(std::max<int64_t>(0, latency.count()));
        if (!has_ack_latency_sample_) {
            ack_latency_ewma_ms_ = sample_ms;
            has_ack_latency_sample_ = true;
        } else {
            constexpr double kEwmaAlpha = 0.2;
            ack_latency_ewma_ms_ += kEwmaAlpha * (sample_ms - ack_latency_ewma_ms_);
        }

        if (sample_ms >= 1500.0) {
            decreaseRate(0.5);
            return;
        }
        if (sample_ms >= 750.0) {
            decreaseRate(0.75);
            return;
        }
        if (sample_ms > 250.0) {
            healthy_ack_streak_ = 0;
            return;
        }

        if (++healthy_ack_streak_ >= 2) {
            // Additive recovery avoids immediately recreating the queue spike
            // that triggered a multiplicative decrease. A near-idle queue
            // (very low ACK latency) may climb back twice as fast: barriers
            // arrive only every few thousand commands, so each recovery step
            // is expensive wall-clock time.
            rate_scale_ = std::min(1.0, rate_scale_ + (sample_ms <= 100.0 ? 0.2 : 0.1));
            healthy_ack_streak_ = 0;
        }
    }

    void noteBarrierTimeout() {
        decreaseRate(0.5);
    }

    void resetFeedback() {
        rate_scale_ = 1.0;
        ack_latency_ewma_ms_ = 0.0;
        has_ack_latency_sample_ = false;
        healthy_ack_streak_ = 0;
    }

    double rateScale() const { return rate_scale_; }
    double ackLatencyEwmaMilliseconds() const { return ack_latency_ewma_ms_; }
    bool hasAckLatencySample() const { return has_ack_latency_sample_; }

private:
    void decreaseRate(double factor) {
        constexpr double kMinimumAdaptiveScale = 0.125;
        rate_scale_ = std::max(kMinimumAdaptiveScale, rate_scale_ * factor);
        healthy_ack_streak_ = 0;
    }

    int32_t requested_blocks_per_second_ = 20;
    double rate_scale_ = 1.0;
    double ack_latency_ewma_ms_ = 0.0;
    bool has_ack_latency_sample_ = false;
    uint32_t healthy_ack_streak_ = 0;
    Clock::time_point last_dispatch_at_{};
    Clock::time_point next_dispatch_at_{};
    bool has_last_dispatch_ = false;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_IMPORT_THROUGHPUT_H
