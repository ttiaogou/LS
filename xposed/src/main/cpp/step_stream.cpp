#include "step_stream.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sensor_probe {
StepStream::Batch StepStream::plan(const StepSnapshot& snapshot, const StepSubscription& subscription,
    std::int64_t now, std::span<PlannedStep> output) const noexcept {
    Batch batch;
    if (lifetime_ == 0 || progress_.revision == std::numeric_limits<std::uint64_t>::max()) return batch;
    batch.owner = this;
    batch.lifetime = lifetime_;
    batch.expectedRevision = progress_.revision;
    batch.next = progress_;
    ++batch.next.revision;
    auto& next = batch.next;
    if (!snapshot.active) {
        next.anchored = false;
        batch.valid = true;
        batch.status = "step-inactive";
        return batch;
    }
    if (snapshot.session == 0 || !std::isfinite(snapshot.steps) || snapshot.steps < 0 ||
        snapshot.steps > static_cast<double>(maxExactSteps) || !std::isfinite(snapshot.cadenceSpm) ||
        snapshot.cadenceSpm < 0 || snapshot.cadenceSpm > maxCadenceSpm ||
        now <= 0 || snapshot.sampleNanos < 0 || snapshot.expiryNanos <= snapshot.sampleNanos ||
        now < snapshot.sampleNanos || now < progress_.anchorTime || snapshot.sampleNanos < progress_.sampleTime ||
        (!subscription.empty() && subscription.epoch == 0)) {
        return batch;
    }
    if (now >= snapshot.expiryNanos || now - snapshot.sampleNanos > maxSampleAgeNanos) {
        next.anchored = false;
        batch.valid = true;
        batch.status = "step-expired";
        return batch;
    }
    const bool changedSubscription = !(subscription == progress_.subscription);
    if (changedSubscription && progress_.subscription.epoch != 0 &&
        subscription.epoch <= progress_.subscription.epoch) return batch;
    const auto projected = snapshot.steps + (now - snapshot.sampleNanos) / 1.0e9 * snapshot.cadenceSpm / 60.0;
    if (!std::isfinite(projected) || projected > static_cast<double>(maxExactSteps)) return batch;
    const auto target = std::max({projected, progress_.anchorSteps, static_cast<double>(progress_.counter)});
    const auto whole = static_cast<std::uint64_t>(std::floor(target));
    const auto elapsed = now - progress_.anchorTime;
    const auto stepDelta = target - progress_.anchorSteps;
    const bool rebase = !progress_.anchored || snapshot.session != progress_.session || changedSubscription ||
        elapsed > maxSampleAgeNanos || stepDelta > elapsed / 1.0e9 * maxCadenceSpm / 60.0 + 2.0 ||
        whole - progress_.detectorStep > 32 ||
        (subscription.detectorCount != 0 && whole - progress_.detectorStep > static_cast<std::uint64_t>(elapsed));
    next.session = snapshot.session;
    next.subscription = subscription;
    next.sampleTime = snapshot.sampleNanos;
    if (changedSubscription) next.reportedCounter = false;
    next.anchored = true;
    if (rebase) {
        next.detectorStep = whole;
        next.anchorSteps = target;
        next.anchorTime = now;
        batch.rebased = true;
    }
    const auto pending = subscription.detectorCount != 0 && !rebase ? whole - next.detectorStep : 0;
    const bool counterNeeded = subscription.counterCount != 0 && (!next.reportedCounter || whole > next.counter);
    const auto capacity = std::min(output.size(), maxBatchEvents);
    if ((pending != 0 || counterNeeded) && capacity == 0) {
        batch.status = "step-buffer-full";
        return batch;
    }
    if (counterNeeded && progress_.reportedCounter && now <= progress_.counterTime) {
        batch.status = "step-counter-time-not-advanced";
        return batch;
    }
    // Every subscribed handle receives its own copy of the planned events. Detector
    // pulses take the batch budget first (backpressure must drain them one by one);
    // counter events are included only when their fan-out still fits.
    const std::size_t detectorFan = std::max<std::size_t>(1, subscription.detectorCount);
    const std::size_t counterFan = subscription.counterCount;
    const std::size_t pulseCapacity = capacity / detectorFan;
    const auto pulseCount = static_cast<std::size_t>(std::min<std::uint64_t>(pending, pulseCapacity));
    const bool allPulsesFit = pulseCount == pending;
    const bool includeCounter = counterNeeded && allPulsesFit &&
        pulseCount * detectorFan + counterFan <= capacity;
    // Keep an unfinished detector trajectory at the last accepted pulse, rather
    // than moving its anchor to now and silently discarding backpressure.
    for (std::size_t i = 0; i < pulseCount; ++i) {
        const auto ordinal = next.detectorStep + 1;
        const long double fraction = (static_cast<long double>(ordinal) - progress_.anchorSteps) / stepDelta;
        const auto delta = static_cast<std::int64_t>(std::clamp(
            std::ceil(fraction * elapsed), 0.0L, static_cast<long double>(elapsed)));
        const auto timestamp = std::max(progress_.anchorTime + delta, next.detectorTime + 1);
        if (timestamp > now) { batch.count = 0; batch.status = "step-timestamp-resolution"; return batch; }
        for (std::size_t d = 0; d < subscription.detectorCount; ++d) {
            output[batch.count++] = {StepKind::detector, subscription.detectorHandles[d], timestamp, 1};
        }
        next.detectorStep = ordinal;
        next.detectorTime = timestamp;
        next.anchorSteps = static_cast<double>(ordinal);
        next.anchorTime = timestamp;
    }
    if (includeCounter) {
        for (std::size_t c = 0; c < counterFan; ++c) {
            output[batch.count++] = {StepKind::counter, subscription.counterHandles[c], now, whole};
        }
        next.counter = whole;
        next.counterTime = now;
        next.reportedCounter = true;
    }
    if (allPulsesFit) {
        next.anchorSteps = target;
        next.anchorTime = now;
        if (subscription.detectorCount == 0) next.detectorStep = whole;
    }
    batch.valid = true;
    batch.counterValue = static_cast<std::int64_t>(whole);
    batch.status = (pending > pulseCount || (counterNeeded && !includeCounter)) ? "step-partial" : "step-ready";
    return batch;
}

bool StepStream::adoptCounterBase(std::int64_t physicalValue, std::int64_t synthesizedValue) noexcept {
    if (physicalValue < 0 || synthesizedValue < 0) return false;
    if (static_cast<std::uint64_t>(physicalValue) > maxExactSteps ||
        static_cast<std::uint64_t>(synthesizedValue) > maxExactSteps) return false;
    counterShift = physicalValue - synthesizedValue;
    physicalBase = physicalValue;
    baseAdopted = true;
    return true;
}

bool StepStream::commit(const Batch& batch, bool accepted) noexcept {
    return accepted && abandon(batch);
}

bool StepStream::abandon(const Batch& batch) noexcept {
    if (!batch.valid || batch.owner != this || batch.lifetime != lifetime_ ||
        batch.expectedRevision != progress_.revision) return false;
    progress_ = batch.next;
    emittedCounter = emittedCounter || progress_.reportedCounter;
    return true;
}
}  // namespace sensor_probe
