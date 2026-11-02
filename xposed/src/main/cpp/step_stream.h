#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace sensor_probe {

struct StepSnapshot {
    bool active = false;
    std::uint64_t session = 0;
    double steps = 0;
    double cadenceSpm = 0;
    std::int64_t sampleNanos = 0;
    std::int64_t expiryNanos = 0;
};

// A connection may hold more than one handle per step type: this ROM exposes a
// wake-up and a non-wake step counter/detector, and an app receives whichever
// variant it registered. Both variants therefore appear in the catalog and both
// must be planned, suppressed and delivered for the connection that holds them.
// Wake-up semantics (the ACK bit, wake-lock accounting, battery attribution) are
// NOT the adapter's job: the original sendEvents looks the sensor up by handle and
// marks the events itself. See docs/native-step-delivery.md.
struct StepSubscription {
    static constexpr std::size_t maxHandles = 4;
    std::array<std::int32_t, maxHandles> counterHandles{-1, -1, -1, -1};
    std::size_t counterCount = 0;
    std::array<std::int32_t, maxHandles> detectorHandles{-1, -1, -1, -1};
    std::size_t detectorCount = 0;
    std::uint64_t epoch = 0;  // Increasing token after every subscription change, even same handles.

    bool empty() const noexcept { return counterCount == 0 && detectorCount == 0; }
    bool hasCounter(std::int32_t handle) const noexcept {
        for (std::size_t i = 0; i < counterCount; ++i) if (counterHandles[i] == handle) return true;
        return false;
    }
    bool hasDetector(std::int32_t handle) const noexcept {
        for (std::size_t i = 0; i < detectorCount; ++i) if (detectorHandles[i] == handle) return true;
        return false;
    }
    bool addCounter(std::int32_t handle) noexcept {
        if (handle < 0 || counterCount == maxHandles || hasCounter(handle) || hasDetector(handle)) return false;
        counterHandles[counterCount++] = handle;
        return true;
    }
    bool addDetector(std::int32_t handle) noexcept {
        if (handle < 0 || detectorCount == maxHandles || hasDetector(handle) || hasCounter(handle)) return false;
        detectorHandles[detectorCount++] = handle;
        return true;
    }
    bool operator==(const StepSubscription&) const = default;
};

// Explicit construction only: brace init on StepSubscription would let brace elision
// silently spill handle values into the count fields. Returns an empty subscription if
// a handle is negative, duplicated across either set, or the set is oversized.
inline StepSubscription subscriptionOf(std::initializer_list<std::int32_t> counters,
    std::initializer_list<std::int32_t> detectors, std::uint64_t epoch) noexcept {
    StepSubscription subscription;
    for (auto handle : counters) if (!subscription.addCounter(handle)) return {};
    for (auto handle : detectors) if (!subscription.addDetector(handle)) return {};
    subscription.epoch = epoch;
    return subscription;
}

enum class StepKind { counter, detector };
struct PlannedStep {
    StepKind kind;
    std::int32_t handle;
    std::int64_t timestampNanos;
    std::uint64_t value;  // Counter integer, or 1 for a detector pulse. Not a wire event.
};

class StepStream {
    struct Progress {
        std::uint64_t revision = 0;
        std::uint64_t session = 0;
        StepSubscription subscription;
        bool anchored = false;
        bool reportedCounter = false;
        std::uint64_t counter = 0;
        std::int64_t counterTime = 0;
        std::uint64_t detectorStep = 0;
        std::int64_t detectorTime = 0;
        double anchorSteps = 0;
        std::int64_t anchorTime = 0;
        std::int64_t sampleTime = 0;
    };
public:
    struct Batch {
        bool valid = false;
        bool rebased = false;
        std::size_t count = 0;
        // Integer counter this batch planned (-1 when no counter event was planned).
        std::int64_t counterValue = -1;
        const char* status = "step-invalid";
    private:
        friend class StepStream;
        const StepStream* owner = nullptr;
        std::uint64_t lifetime = 0;
        std::uint64_t expectedRevision = 0;
        Progress next;
    };

    // Caller assigns a fresh nonzero identity for every connection lifetime.
    explicit StepStream(std::uint64_t lifetime) noexcept : lifetime_(lifetime) {}
    StepStream(const StepStream&) = delete;
    StepStream& operator=(const StepStream&) = delete;

    // Caller serializes plan -> transport -> commit per connection. No allocation.
    // Every subscribed handle receives its own copy of the planned events, so the
    // output span must cover the fan-out; a span too small yields partial batches.
    Batch plan(const StepSnapshot&, const StepSubscription&, std::int64_t nowNanos,
        std::span<PlannedStep> output) const noexcept;
    // accepted means the WHOLE planned batch was accepted by a future transport;
    // false must mean ZERO events accepted. Unknown/partial outcomes need a
    // different transport protocol, not blind retry of this batch.
    // A zero-event initialization/pause batch needs no I/O and may be committed.
    bool commit(const Batch&, bool accepted) noexcept;
    // Advance after a transport attempt with unknown delivery (e.g. cached after
    // a socket error). This deliberately drops retry eligibility, NOT an ACK.
    bool abandon(const Batch&) noexcept;
    std::uint64_t revision() const noexcept { return progress_.revision; }
    // True once this connection has emitted a counter value (the app has therefore
    // baselined on our value, not on the physical one).
    bool counterDelivered() const noexcept { return emittedCounter; }

    // 接管对齐：系统级替换时 App 建立基线用的是物理计步值；若我们的合成值来自另一套基准
    // （跨会话累计），App 会看到"开局多出几千步"。仅在该连接尚未发出过计步时采纳一次，
    // 用被抑制的物理值反推偏移；此后固定，绝不因会话切换而回跳。
    bool adoptCounterBase(std::int64_t physicalValue, std::int64_t synthesizedValue) noexcept;
    std::int64_t counterShiftValue() const noexcept { return counterShift; }
    std::int64_t physicalBaseValue() const noexcept { return physicalBase; }
    bool counterBaseAdopted() const noexcept { return baseAdopted; }
    void notePhysicalCounter(std::int64_t value) noexcept {
        if (value >= 0 && static_cast<std::uint64_t>(value) <= maxExactSteps) lastPhysicalCounter = value;
    }
    std::int64_t lastPhysicalCounterValue() const noexcept { return lastPhysicalCounter; }
    // 还没有物理基准时不发合成计步（期间透传），避免"先发合成值、物理值后到"导致
    // 永远错过对齐。等待有界：超时后退回 0 基准。
    void markBaseWait(std::int64_t nowNanos) noexcept {
        if (baseWaitStart == 0) baseWaitStart = nowNanos;
    }
    std::int64_t baseWaitStartValue() const noexcept { return baseWaitStart; }

    static constexpr std::size_t maxBatchEvents = 16;
    static constexpr std::int64_t maxSampleAgeNanos = 5'000'000'000LL;
    static constexpr std::uint64_t maxExactSteps = (std::uint64_t{1} << 53) - 1;
    static constexpr double maxCadenceSpm = 600;
private:
    const std::uint64_t lifetime_;
    Progress progress_;
    std::int64_t counterShift = 0;       // emitted = planned + shift
    std::int64_t physicalBase = -1;      // physical value the app would have seen
    std::int64_t lastPhysicalCounter = -1;
    std::int64_t baseWaitStart = 0;
    bool baseAdopted = false;
    bool emittedCounter = false;
};
}  // namespace sensor_probe
