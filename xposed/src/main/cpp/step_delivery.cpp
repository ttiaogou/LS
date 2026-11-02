#include "step_delivery.h"

#include <array>
#include <cerrno>
#include <cstring>

namespace sensor_probe {
StepDeliveryResult submitSteps(SendEvents original, void* connection,
    const void* events, std::size_t count, void* scratch, const void* flushMap,
    StepStream& stream, const StepSnapshot& snapshot, const StepSubscription& subscription,
    std::int64_t now, bool authorized) noexcept {
    StepDeliveryResult result;
    const int incomingErrno = errno;
    auto forward = [&] {
        errno = incomingErrno;
        result.result = original(connection, events, count, scratch, flushMap);
        return result;
    };
    if (original == nullptr) {
        result.result = -ENOSYS;
        result.status = "delivery-missing-original";
        return result;
    }
    if (!authorized || connection == nullptr || events == nullptr || count == 0 ||
        count > maxDeliveryInputEvents || !snapshot.active ||
        now >= snapshot.expiryNanos || now < snapshot.sampleNanos ||
        now - snapshot.sampleNanos > StepStream::maxSampleAgeNanos ||
        subscription.empty()) return forward();

    // Flush maps are indexed by ORIGINAL input positions. Keep every original
    // slot, never compact metadata or synthesize weak_ptr objects.
    std::array<StepWireEvent, maxDeliveryInputEvents + StepStream::maxBatchEvents> input;
    std::memcpy(input.data(), events, count * sizeof(StepWireEvent));
    for (std::size_t i = 0; i < count; ++i) {
        if ((input[i].type == 0 && flushMap == nullptr) ||
            (input[i].type != 0 && input[i].version != sizeof(StepWireEvent))) {
            result.status = "delivery-unsupported-batch";
            return forward();
        }
    }
    // 被抑制的物理计步值就是 App 本会看到的数；on-change 传感器可能很久才报一条，
    // 所以每次看到都记住，接管时即使本批没有也能用最近一次的值。
    std::int64_t physicalCounter = -1;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& event = input[i];
        if (event.type != 19 || !subscription.hasCounter(event.sensor)) continue;
        std::uint64_t value = 0;
        std::memcpy(&value, event.payload.data(), sizeof(value));
        if (value <= StepStream::maxExactSteps) physicalCounter = static_cast<std::int64_t>(value);
        break;
    }
    if (physicalCounter >= 0) stream.notePhysicalCounter(physicalCounter);
    std::array<PlannedStep, StepStream::maxBatchEvents> planned;
    const auto batch = stream.plan(snapshot, subscription, now, planned);
    if (!batch.valid) {
        result.status = batch.status;
        return forward();
    }
    // 只在"该连接还没发出过计步"时对齐一次：此时 App 尚未以我们的值为基线，
    // 对齐不会造成可见回跳；之后固定，不随会话切换重新采纳。
    if (!stream.counterDelivered() && batch.counterValue >= 0 &&
        static_cast<std::uint64_t>(batch.counterValue) <= StepStream::maxExactSteps) {
        std::int64_t base =
            physicalCounter >= 0 ? physicalCounter : stream.lastPhysicalCounterValue();
        if (base < 0) {
            // 物理基准还没到：这期间整批原样透传（App 会先用物理值建立基线），
            // 我们不发合成计步，从而不会抢先发出一个"另一套基准"的值。
            stream.markBaseWait(now);
            if (now - stream.baseWaitStartValue() < 2'000'000'000LL) return forward();
            base = 0;  // 有界等待超时：没有必要一直不投递
        }
        stream.adoptCounterBase(base, batch.counterValue);
    }
    std::array<StepWireEvent, maxDeliveryInputEvents + StepStream::maxBatchEvents> filtered;
    std::size_t used = count;
    for (std::size_t i = 0; i < count; ++i) {
        auto& event = input[i];
        const bool replacing = (event.type == 19 && subscription.hasCounter(event.sensor)) ||
            (event.type == 18 && subscription.hasDetector(event.sensor));
        if (replacing) {
            // The caller has verified this handle is NOT subscribed. Original's
            // scratch/filter path drops it while leaving every map index intact.
            event.sensor = -1;
            ++result.suppressed;
        }
    }
    for (std::size_t i = 0; i < batch.count; ++i) {
        PlannedStep step = planned[i];
        if (step.kind == StepKind::counter) {
            const std::int64_t shifted =
                static_cast<std::int64_t>(step.value) + stream.counterShiftValue();
            if (shifted < 0 || static_cast<std::uint64_t>(shifted) > StepStream::maxExactSteps) {
                result.suppressed = 0;
                result.status = "delivery-counter-base-invalid";
                return forward();
            }
            step.value = static_cast<std::uint64_t>(shifted);
            ++result.counterEvents;
            result.lastCounterValue = step.value;
        } else {
            ++result.detectorPulses;
        }
        if (!encodeNonWakeStep(step, input[used])) {
            result.suppressed = 0;
            result.status = "delivery-encoding-refused";
            return forward();
        }
        ++used;
    }
    // scratch is sized for the enlarged batch, not the caller's original count.
    // Appended records are never metadata, so they never index the original map.
    errno = incomingErrno;
    result.result = original(connection, input.data(), used, filtered.data(), flushMap);
    const int outgoingErrno = errno;
    result.submitted = true;
    result.synthesized = batch.count;
    result.progressRetired = stream.abandon(batch);
    result.status = "delivery-submitted-unconfirmed";
    errno = outgoingErrno;
    return result;
}
}  // namespace sensor_probe
