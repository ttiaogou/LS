#include "step_stream.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

namespace {
using namespace sensor_probe;
constexpr std::int64_t second = 1'000'000'000LL;
constexpr std::int64_t start = second;
const StepSubscription both = subscriptionOf({10}, {11}, 1);
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
StepSnapshot sample() { return {true, 1, 100.25, 120, start, start + 86400 * second}; }
void seed(StepStream& stream, const StepSubscription& subscription = both) {
    std::array<PlannedStep, 16> out;
    const auto batch = stream.plan(sample(), subscription, start, out);
    require(batch.valid && stream.commit(batch, true), "seed connection");
}
bool same(const PlannedStep& a, const PlannedStep& b) {
    return a.kind == b.kind && a.handle == b.handle && a.timestampNanos == b.timestampNanos && a.value == b.value;
}
}

int main() {
    using namespace sensor_probe;
    std::array<PlannedStep, 32> out;
    StepStream stream(1);
    auto snapshot = sample();
    auto batch = stream.plan(snapshot, both, start, out);
    require(batch.valid && batch.rebased && batch.count == 1, "initial counter without historical detector burst");
    require(out[0].kind == StepKind::counter && out[0].value == 100 && out[0].handle == 10, "counter baseline");
    require(stream.revision() == 0, "planning must not advance state");
    require(!stream.commit(batch, false) && stream.revision() == 0, "failed transport does not advance state");
    require(stream.commit(batch, true) && stream.revision() == 1, "accepted batch advances state");
    require(!stream.commit(batch, true), "duplicate acknowledgment rejected");
    batch = stream.plan(snapshot, both, start + second / 4, out);
    require(batch.valid && batch.count == 0 && stream.commit(batch, true), "fractional-only change is silent");
    batch = stream.plan(snapshot, both, start + second / 2, out);
    require(batch.valid && batch.count == 2, "one pulse and changed counter");
    require(out[0].kind == StepKind::detector && out[0].handle == 11 && out[0].value == 1 &&
        out[0].timestampNanos == start + 375'000'000, "interpolated integer crossing");
    require(out[1].kind == StepKind::counter && out[1].value == 101 &&
        out[1].timestampNanos == start + second / 2, "counter follows pulse in timestamp order");
    const auto pending = batch;
    const auto first = out[0];
    require(!stream.commit(batch, false), "pulse transport failure");
    batch = stream.plan(snapshot, both, start + second / 2, out);
    require(batch.count == 2 && same(out[0], first), "retry reproduces pending pulse");
    require(stream.commit(batch, true), "retry accepted");
    require(!stream.commit(pending, true), "stale competing plan rejected");
    batch = stream.plan(snapshot, both, start + second / 2, out);
    require(batch.valid && batch.count == 0, "no duplicate pulses at same time");
    StepStream another(1);
    require(!another.commit(batch, true), "batch owner checked even when lifetime IDs collide");
    seed(another);
    const auto independent = another.plan(snapshot, both, start + second / 2, out);
    require(independent.count == 2, "independent connection progress");

    StepStream emptyId(0);
    require(!emptyId.plan(snapshot, both, start, out).valid, "zero lifetime rejected");
    alignas(StepStream) std::byte memory[sizeof(StepStream)];
    auto* recycled = std::construct_at(reinterpret_cast<StepStream*>(memory), 10);
    const auto oldLifetime = recycled->plan(snapshot, both, start, out);
    std::destroy_at(recycled);
    recycled = std::construct_at(reinterpret_cast<StepStream*>(memory), 11);
    require(!recycled->commit(oldLifetime, true), "reused address rejects old lifetime plan");
    std::destroy_at(recycled);

    for (const auto value : {-1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN(), 9007199254740992.0}) {
        auto invalid = sample(); invalid.steps = value;
        require(!stream.plan(invalid, both, start + second, out).valid, "invalid step number rejected");
    }
    for (const auto value : {-1.0, 601.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = sample(); invalid.cadenceSpm = value;
        require(!stream.plan(invalid, both, start + second, out).valid, "invalid cadence rejected");
    }
    auto invalid = sample(); invalid.session = 0;
    require(!stream.plan(invalid, both, start + second, out).valid, "session must be explicit");
    invalid = sample(); invalid.sampleNanos = -1;
    require(!stream.plan(invalid, both, start + second, out).valid, "negative sample time");
    invalid = sample(); invalid.expiryNanos = invalid.sampleNanos;
    require(!stream.plan(invalid, both, start + second, out).valid, "invalid expiry interval");
    require(!stream.plan(sample(), both, start - 1, out).valid, "future sample/clock rollback rejected");
    StepSubscription builder;
    require(!builder.addCounter(-1) && !builder.addDetector(-1), "negative handles rejected");
    require(builder.addCounter(5) && !builder.addCounter(5) && !builder.addDetector(5),
        "duplicate handles rejected across both sets");
    require(!stream.plan(sample(), subscriptionOf({10}, {11}, 0), start + second, out).valid,
        "subscription epoch required");

    StepStream expiry(20);
    seed(expiry);
    batch = expiry.plan(sample(), both, start + 5 * second + 1, out);
    require(batch.valid && batch.count == 0 && expiry.commit(batch, true), "five-second freshness independent of long expiry");
    auto fresh = sample(); fresh.sampleNanos = start + 6 * second; fresh.steps = 112.25;
    batch = expiry.plan(fresh, both, fresh.sampleNanos, out);
    require(batch.rebased && batch.count == 1 && out[0].kind == StepKind::counter, "resume has no stale pulse burst");
    require(expiry.commit(batch, true), "resume accepted");
    require(!expiry.plan(sample(), both, start + 6 * second, out).valid, "older sample rejected after refresh");
    auto deadline = sample(); deadline.expiryNanos = start + second;
    StepStream deadlineStream(21);
    batch = deadlineStream.plan(deadline, both, deadline.expiryNanos, out);
    require(batch.valid && batch.count == 0, "expiry boundary is exclusive");
    StepStream paused(22); seed(paused);
    auto stopped = sample(); stopped.active = false;
    batch = paused.plan(stopped, both, start + second / 4, out);
    require(batch.valid && batch.count == 0 && paused.commit(batch, true), "pause commits no-event state");
    batch = paused.plan(sample(), both, start + second / 2, out);
    require(batch.rebased && batch.count == 1 && out[0].kind == StepKind::counter, "no pause/resume detector backfill");

    StepStream sessions(30); seed(sessions);
    auto changed = sample(); changed.session = 2; changed.steps = 105.25; changed.sampleNanos = start + second;
    batch = sessions.plan(changed, both, changed.sampleNanos, out);
    require(batch.rebased && batch.count == 1 && out[0].value == 105 && sessions.commit(batch, true), "session change rebases detector");
    changed.steps = 1; changed.sampleNanos += second;
    batch = sessions.plan(changed, both, changed.sampleNanos, out);
    require(batch.valid && batch.count == 0 && sessions.commit(batch, true), "counter cannot regress");
    changed.steps = 107; changed.sampleNanos += second;
    batch = sessions.plan(changed, subscriptionOf({12}, {13}, 2), changed.sampleNanos, out);
    require(batch.rebased && batch.count == 1 && out[0].handle == 12, "subscription change starts fresh detector progress");
    batch = sessions.plan(changed, subscriptionOf({10}, {11}, 3), changed.sampleNanos, out);
    require(batch.rebased && batch.count == 1 && out[0].kind == StepKind::counter,
        "same handles with new epoch do not reuse old subscription progress");
    StepStream subscriptionEpoch(32); seed(subscriptionEpoch);
    const auto obsolete = subscriptionEpoch.plan(sample(), both, start + second, out);
    batch = subscriptionEpoch.plan(sample(), subscriptionOf({12}, {13}, 2), start + second, out);
    require(batch.valid && subscriptionEpoch.commit(batch, true), "new subscription epoch accepted");
    require(!subscriptionEpoch.commit(obsolete, true), "old acknowledgment after resubscribe rejected");
    require(!subscriptionEpoch.plan(sample(), both, start + second, out).valid, "old epoch cannot return");
    require(!subscriptionEpoch.plan(sample(), subscriptionOf({12}, {}, 2), start + second, out).valid,
        "changing subscription without new epoch rejected");
    batch = subscriptionEpoch.plan(sample(), subscriptionOf({}, {}, 3), start + second, out);
    require(batch.valid && batch.count == 0 && subscriptionEpoch.commit(batch, true), "explicit unsubscribe epoch");
    batch = subscriptionEpoch.plan(sample(), subscriptionOf({10}, {11}, 4), start + 2 * second, out);
    require(batch.rebased && batch.count == 1 && out[0].kind == StepKind::counter, "resubscribe has no historical pulse burst");
    StepStream jump(31); seed(jump);
    changed = sample(); changed.steps = 10000; changed.sampleNanos += second;
    batch = jump.plan(changed, both, changed.sampleNanos, out);
    require(batch.rebased && batch.count == 1 && out[0].value == 10000, "implausible jump does not generate pulse storm");
    changed.steps = 120; changed.sampleNanos = start + 10 * second;
    batch = jump.plan(changed, both, changed.sampleNanos, out);
    require(batch.rebased && batch.count == 1, "long callback gap rebases detector");

    StepStream partial(40);
    auto fast = sample(); fast.cadenceSpm = 600;
    batch = partial.plan(fast, both, start, out); require(partial.commit(batch, true), "seed fast stream");
    auto one = std::span(out.data(), 1);
    auto zero = std::span<PlannedStep>{};
    batch = partial.plan(fast, both, start + 2 * second, zero);
    require(!batch.valid && partial.revision() == 1, "no capacity retains backlog");
    unsigned pulses = 0, counters = 0;
    std::int64_t previousPulse = start;
    for (unsigned i = 0; i < 21; ++i) {
        batch = partial.plan(fast, both, start + 2 * second, one);
        require(batch.valid && batch.count == 1, "single-slot bounded drain");
        if (out[0].kind == StepKind::detector) {
            ++pulses;
            require(out[0].timestampNanos > previousPulse && out[0].timestampNanos <= start + 2 * second,
                "partial timestamps remain ordered");
            previousPulse = out[0].timestampNanos;
        } else { ++counters; require(out[0].value == 120, "counter waits behind detector backlog"); }
        require(partial.commit(batch, true), "partial accepted");
    }
    require(pulses == 20 && counters == 1, "backpressure neither loses nor duplicates steps");
    batch = partial.plan(fast, both, start + 2 * second, out);
    require(batch.valid && batch.count == 0, "drain completion is silent");
    StepStream bounded(41);
    batch = bounded.plan(fast, both, start, out); require(bounded.commit(batch, true), "seed bounded stream");
    batch = bounded.plan(fast, both, start + 2 * second, out);
    require(batch.valid && batch.count == StepStream::maxBatchEvents && bounded.commit(batch, true), "batch event budget");
    batch = bounded.plan(fast, both, start + 2 * second, out);
    require(batch.count == 5 && out[4].kind == StepKind::counter, "remaining pulses plus counter");

    StepStream detectorOnly(50);
    const StepSubscription detector = subscriptionOf({}, {0}, 1);
    batch = detectorOnly.plan(sample(), detector, start, zero);
    require(batch.valid && batch.count == 0 && detectorOnly.commit(batch, true), "detector-only seed needs no I/O");
    batch = detectorOnly.plan(sample(), detector, start + second / 2, out);
    require(batch.count == 1 && out[0].handle == 0, "handle zero is valid");
    StepStream counterOnly(51);
    const StepSubscription counter = subscriptionOf({0}, {}, 1);
    seed(counterOnly, counter);
    batch = counterOnly.plan(sample(), counter, start + second, out);
    require(batch.count == 1 && out[0].kind == StepKind::counter && out[0].value == 102, "counter-only changed integer");
    const auto detectorAfterCounter = subscriptionOf({}, {11}, 2);
    batch = counterOnly.plan(sample(), detectorAfterCounter, start + second, out);
    require(batch.valid && counterOnly.commit(batch, true), "counter subscription can change to detector-only");
    require(counterOnly.counterDelivered(), "subscription changes never erase counter delivery history");
    StepStream noSensors(52);
    batch = noSensors.plan(sample(), {}, start + second, zero);
    require(batch.valid && batch.count == 0, "no subscription means no events");
    auto stationary = sample(); stationary.cadenceSpm = 0;
    StepStream still(53);
    batch = still.plan(stationary, both, start, out); require(still.commit(batch, true), "stationary seed");
    batch = still.plan(stationary, both, start + second, out);
    require(batch.valid && batch.count == 0, "zero cadence does not extrapolate");
    auto extreme = sample(); extreme.steps = static_cast<double>(StepStream::maxExactSteps); extreme.cadenceSpm = 0;
    StepStream exact(60);
    batch = exact.plan(extreme, counter, start, out);
    require(batch.valid && out[0].value == StepStream::maxExactSteps, "largest exact integer counter");
    extreme.cadenceSpm = 600;
    require(!exact.plan(extreme, counter, start + second, out).valid, "projected step overflow rejected");
    StepStream timeResolution(62); seed(timeResolution);
    auto instantaneous = sample(); instantaneous.steps = 101; instantaneous.cadenceSpm = 0;
    batch = timeResolution.plan(instantaneous, both, start, out);
    require(!batch.valid && timeResolution.revision() == 1, "counter update requires time to advance");
    batch = timeResolution.plan(instantaneous, both, start + 1, out);
    require(batch.valid && batch.count == 2 && out[0].timestampNanos == start + 1,
        "one-nanosecond crossing remains bounded");
    StepStream zeroCounter(63);
    instantaneous.steps = 0;
    batch = zeroCounter.plan(instantaneous, counter, start, out);
    require(batch.valid && batch.count == 1 && out[0].value == 0, "zero counter baseline is reported");
    StepStream freshnessBoundary(64); seed(freshnessBoundary);
    batch = freshnessBoundary.plan(sample(), both, start + 5 * second, out);
    require(batch.valid && !batch.rebased && batch.count == 11, "freshness age boundary is inclusive");
    extreme = sample(); extreme.sampleNanos = std::numeric_limits<std::int64_t>::max() - second;
    extreme.expiryNanos = std::numeric_limits<std::int64_t>::max();
    StepStream late(61);
    batch = late.plan(extreme, both, extreme.sampleNanos, out);
    require(batch.valid && late.commit(batch, true), "large monotonic timestamp seed");
    batch = late.plan(extreme, both, extreme.sampleNanos + second / 2, out);
    require(batch.valid && batch.count == 2 && out[0].timestampNanos < extreme.expiryNanos, "large timestamps do not overflow");

    StepStream property(70);
    snapshot = sample();
    std::uint32_t rng = 0x17bc9241;
    std::int64_t lastPulse = 0;
    std::uint64_t lastCounter = 0;
    for (unsigned tick = 0; tick < 2000; ++tick) {
        const auto now = start + tick * 10'000'000LL;
        if (tick % 100 == 0) {
            snapshot.steps = 100.25 + tick * 0.02;
            snapshot.sampleNanos = now;
        }
        rng = rng * 1664525U + 1013904223U;
        const auto capacity = rng % 20;
        batch = property.plan(snapshot, both, now, std::span(out.data(), capacity));
        require(batch.count <= capacity && batch.count <= StepStream::maxBatchEvents, "randomized capacity bound");
        const bool accepted = (rng & 7U) != 0;
        if (!batch.valid || !property.commit(batch, accepted)) continue;
        for (std::size_t i = 0; i < batch.count; ++i) {
            require(out[i].timestampNanos <= now, "no future events");
            if (out[i].kind == StepKind::detector) {
                require(out[i].timestampNanos > lastPulse && out[i].value == 1, "detector property");
                lastPulse = out[i].timestampNanos;
            } else {
                require(out[i].value >= lastCounter, "counter property");
                lastCounter = out[i].value;
            }
        }
    }
    std::printf("Step stream passed %d assertions including 2000 transport/capacity ticks\n", checks);
}
