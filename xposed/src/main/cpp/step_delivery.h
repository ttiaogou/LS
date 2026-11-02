#pragma once

#include "passthrough.h"
#include "step_wire.h"

namespace sensor_probe {

struct StepDeliveryResult {
    std::int32_t result = 0;
    bool submitted = false;
    bool progressRetired = false;
    std::size_t synthesized = 0;
    std::size_t suppressed = 0;
    // 事件构成拆分：counter 事件数、detector 脉冲数，以及最后一次发出的计数。
    // 物理计步每秒约 2.6 条、detector 每个真实步伐 1 条；若这里每秒上百条，
    // 说明脉冲是按 sendEvents 调用频率发的，而不是按步发的。
    std::size_t counterEvents = 0;
    std::size_t detectorPulses = 0;
    std::uint64_t lastCounterValue = 0;
    const char* status = "delivery-passthrough";
};

// Adapter only: caller owns a live, authorized connection, a serialized stream,
// and the wake-up and/or non-wake handles that connection currently holds. Caller
// MUST verify handle -1 is absent from this connection. Never infers subscriptions
// from a catalog. Wake-up bookkeeping (ACK bit, wake lock, battery attribution) is
// performed by the original sendEvents from the handle, not by this adapter.
// Original retains subscription, first-flush, AppOps, privacy and cache handling.
// Original indices (including metadata/flush mappings) are retained. Oversized
// buffers and metadata without a filtering map are forwarded unchanged.
// A submission retires progress even on error: original may already have cached
// some/all events. This is at-most-once submission, NOT confirmed app delivery.
StepDeliveryResult submitSteps(SendEvents original, void* connection,
    const void* events, std::size_t count, void* scratch, const void* flushMap,
    StepStream&, const StepSnapshot&, const StepSubscription&, std::int64_t nowNanos,
    bool authorized) noexcept;

inline constexpr std::size_t maxDeliveryInputEvents = 256;
}  // namespace sensor_probe
