#include "step_delivery.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using namespace sensor_probe;
int checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
std::array<StepWireEvent, 272> captured;
std::size_t capturedCount;
const void* capturedInput;
void* capturedScratch;
const void* capturedMap;
int calls;
int incomingErrno;
int returnCode;
std::int32_t original(void*, const void* input, std::size_t count, void* scratch, const void* map) {
    ++calls;
    incomingErrno = errno;
    capturedInput = input;
    capturedScratch = scratch;
    capturedMap = map;
    capturedCount = count;
    if (input != nullptr && count <= captured.size()) {
        std::memcpy(captured.data(), input, count * sizeof(StepWireEvent));
        // Exercise all enlarged scratch slots under ASan, as the original may do.
        if (scratch != nullptr && scratch != input)
            std::memcpy(scratch, input, count * sizeof(StepWireEvent));
    }
    errno = EAGAIN;
    return returnCode;
}
}
int main() {
    using namespace sensor_probe;
    int receiver;
    std::array<StepWireEvent, 3> input{};
    for (auto& event : input) { event.version = sizeof(event); event.timestamp = 1'000'000'000LL; }
    input[0].sensor = 2; input[0].type = 1;
    input[0].payload.fill(std::byte{0xab}); input[0].flags = 1;
    input[1].sensor = 10; input[1].type = 19;
    // 物理计步器当前值：App 建立基线时看到的就是它，第一条合成值必须与它连续。
    const std::uint64_t physicalSteps = 1200;
    std::memcpy(input[1].payload.data(), &physicalSteps, sizeof(physicalSteps));
    input[2].sensor = 11; input[2].type = 18;
    const auto unchanged = input;
    std::array<StepWireEvent, 3> scratch;
    std::array<std::uint64_t, 6> map{};
    StepStream stream(1);
    StepSnapshot snapshot{true, 1, 100, 120, 1'000'000'000LL, 10'000'000'000LL};
    StepSubscription subscription = subscriptionOf({10}, {11}, 1);
    auto submit = [&](bool authorized, std::int64_t now) {
        errno = EDOM;
        return submitSteps(original, &receiver, input.data(), input.size(), scratch.data(),
            &map, stream, snapshot, subscription, now, authorized);
    };
    auto result = submit(false, snapshot.sampleNanos);
    require(!result.submitted && stream.revision() == 0, "unauthorized does not alter progress");
    require(capturedInput == input.data() && capturedScratch == scratch.data() && capturedMap == &map,
        "unauthorized forwards original arguments");
    require(incomingErrno == EDOM && errno == EAGAIN, "passthrough errno contract");
    result = submit(true, snapshot.sampleNanos);
    require(result.submitted && result.progressRetired && result.synthesized == 1 && result.suppressed == 2,
        "initial counter replaces physical steps");

    require(capturedCount == 4 && capturedMap == &map && capturedScratch != scratch.data(),
        "replacement scratch and original flush map");
    require(std::memcmp(&captured[0], &input[0], sizeof(StepWireEvent)) == 0,
        "unrelated event including wake flag unchanged");
    require(captured[1].sensor == -1 && captured[2].sensor == -1, "physical steps marked unsubscribed in original slots");
    require(captured[3].type == 19 && captured[3].sensor == 10, "synthesized counter appended after original slots");
    std::uint64_t count;
    std::memcpy(&count, captured[3].payload.data(), sizeof(count));
    // 接管对齐：物理值 1200、合成值 100 ⇒ 第一条合成值必须是 1200（App 的基线就建立在这上面）
    require(count == 1200, "first synthesized counter continues from the physical value");
    require(stream.counterBaseAdopted() && stream.physicalBaseValue() == 1200 &&
            stream.counterShiftValue() == 1100, "takeover base adopted once and recorded");
    require(result.counterEvents == 1 && result.detectorPulses == 0 && result.lastCounterValue == 1200,
        "first batch emits one counter, no pulse");
    require(std::memcmp(input.data(), unchanged.data(), sizeof(input)) == 0, "caller input untouched");
    require(incomingErrno == EDOM && errno == EAGAIN, "adapted errno contract");
    returnCode = -EAGAIN;
    result = submit(true, snapshot.sampleNanos + 500'000'000LL);
    require(result.result == -EAGAIN && result.synthesized == 2 && result.progressRetired,
        "cache-capable error retires uncertain progress");
    require(capturedCount == 5 && captured[3].type == 18 && captured[4].type == 19,
        "detector and counter submitted together");
    result = submit(true, snapshot.sampleNanos + 500'000'000LL);
    require(result.synthesized == 0 && capturedCount == 3, "uncertain submission never retried");
    returnCode = 0;
    auto revision = stream.revision();
    input[0].type = 0;
    result = submit(true, snapshot.sampleNanos + 1'000'000'000LL);
    require(result.submitted && capturedMap == &map && captured[0].type == 0 &&
        std::memcmp(&captured[0], &input[0], sizeof(input[0])) == 0,
        "metadata retains original map index and bytes");
    revision = stream.revision();
    result = submitSteps(original, &receiver, input.data(), input.size(), scratch.data(), nullptr,
        stream, snapshot, subscription, snapshot.sampleNanos + 1'000'000'000LL, true);
    require(!result.submitted && capturedInput == input.data() && stream.revision() == revision,
        "metadata without original map transparent");
    input = unchanged;
    snapshot.active = false;
    result = submit(true, snapshot.sampleNanos + 1'000'000'000LL);
    require(!result.submitted && capturedInput == input.data(), "inactive physical sensors restored");
    snapshot.active = true;
    result = submit(true, snapshot.sampleNanos + 6'000'000'000LL);
    require(!result.submitted && stream.revision() == revision, "stale sample not suppressed");
    snapshot.steps = -1;
    result = submit(true, snapshot.sampleNanos + 1'000'000'000LL);
    require(!result.submitted && stream.revision() == revision, "invalid controller data not suppressed");
    snapshot.steps = 102;
    subscription.epoch = 0;
    result = submit(true, snapshot.sampleNanos + 1'000'000'000LL);
    require(!result.submitted && stream.revision() == revision, "unverified subscription refused");
    subscription.epoch = 1;
    input[0].version = 1;
    result = submit(true, snapshot.sampleNanos + 1'000'000'000LL);
    require(!result.submitted, "unknown wire ABI refused");
    input = unchanged;
    std::array<StepWireEvent, maxDeliveryInputEvents + 1> large{};
    result = submitSteps(original, &receiver, large.data(), large.size(), nullptr, &map,
        stream, snapshot, subscription, snapshot.sampleNanos, true);
    require(!result.submitted && capturedInput == large.data(), "oversized batch transparent");
    const auto previousCalls = calls;
    result = submitSteps(nullptr, &receiver, input.data(), input.size(), scratch.data(), &map,
        stream, snapshot, subscription, snapshot.sampleNanos, true);
    require(result.result == -ENOSYS && calls == previousCalls, "missing trampoline fails safely");
    // Fill the complete expanded buffer; a tiny original scratch must not be used.
    std::array<StepWireEvent, maxDeliveryInputEvents> full{};
    for (auto& event : full) { event.version = sizeof(event); event.type = 1; event.sensor = 2; }
    result = submitSteps(original, &receiver, full.data(), full.size(), scratch.data(), &map,
        stream, snapshot, subscription, snapshot.sampleNanos + 1'500'000'000LL, true);
    require(result.submitted && capturedCount > full.size(), "expanded batch exceeds caller scratch capacity");
    // Abandon has the same ownership/revision checks as ACK, without claiming delivery.
    std::array<PlannedStep, 16> planned;
    auto batch = stream.plan(snapshot, subscription, snapshot.sampleNanos + 2'000'000'000LL, planned);
    StepStream foreign(2);
    require(!foreign.abandon(batch), "foreign uncertain batch refused");
    require(stream.abandon(batch) && !stream.abandon(batch), "uncertain batch retired exactly once");
    StepStream maximumStream(3);
    StepSnapshot maximumSnapshot{true, 2, 100, 600, 1'000'000'000LL, 5'000'000'000LL};
    auto seed = maximumStream.plan(maximumSnapshot, subscription, maximumSnapshot.sampleNanos, planned);
    require(maximumStream.commit(seed, true), "seed maximum expansion");
    result = submitSteps(original, &receiver, full.data(), full.size(), scratch.data(), &map,
        maximumStream, maximumSnapshot, subscription, 2'500'000'000LL, true);
    require(result.submitted && result.synthesized == 16 && capturedCount == captured.size(),
        "all 272 input and scratch slots exercised");
    // 已经发出过计步（App 已以我们的值为基线）后，即使物理值变化也不得重新采纳，
    // 否则会中途回跳。
    StepStream adoptedOnce(4);
    StepSnapshot s1{true, 11, 300, 120, 20'000'000'000LL, 90'000'000'000LL};
    const auto firstAdopt = submitSteps(original, &receiver, input.data(), input.size(),
        scratch.data(), &map, adoptedOnce, s1, subscription, s1.sampleNanos, true);
    require(firstAdopt.submitted && adoptedOnce.counterBaseAdopted() &&
            adoptedOnce.counterShiftValue() == 1200 - 300, "first emission adopts the base");
    std::uint64_t bump = 9000;
    std::memcpy(input[1].payload.data(), &bump, sizeof(bump));
    // （第二批里物理值变成 9000，但基线已在第一批固定）
    StepSnapshot s2{true, 11, 320, 120, 21'000'000'000LL, 90'000'000'000LL};
    const auto later = submitSteps(original, &receiver, input.data(), input.size(),
        scratch.data(), &map, adoptedOnce, s2, subscription, s2.sampleNanos, true);
    require(later.submitted && adoptedOnce.counterShiftValue() == 1200 - 300,
        "base stays fixed after the first delivery even when the physical value moves");
    std::memcpy(input[1].payload.data(), &physicalSteps, sizeof(physicalSteps));

    // 物理基准未到：等待期内必须整批透传（不抢先发合成计步），超时后按 0 基准发出。
    StepStream waiting(5);
    std::array<StepWireEvent, 2> accelOnly{};
    for (auto& event : accelOnly) { event.version = sizeof(event); event.timestamp = 40'000'000'000LL; }
    accelOnly[0].sensor = 2; accelOnly[0].type = 1;
    accelOnly[1].sensor = 11; accelOnly[1].type = 18;
    StepSnapshot waitSnap{true, 13, 500, 120, 40'000'000'000LL, 90'000'000'000LL};
    const auto waiting1 = submitSteps(original, &receiver, accelOnly.data(), accelOnly.size(),
        scratch.data(), &map, waiting, waitSnap, subscription, waitSnap.sampleNanos, true);
    require(!waiting1.submitted && !waiting.counterBaseAdopted() && capturedInput == accelOnly.data(),
        "without a physical base the batch is forwarded untouched");
    const auto waiting2 = submitSteps(original, &receiver, accelOnly.data(), accelOnly.size(),
        scratch.data(), &map, waiting, waitSnap, subscription,
        waitSnap.sampleNanos + 2'500'000'000LL, true);
    require(waiting2.submitted && waiting.counterBaseAdopted() &&
            waiting.physicalBaseValue() == 0, "bounded wait expires and falls back to a zero base");
    std::uint64_t emitted = 0;
    for (std::size_t i = 0; i < capturedCount; ++i) {
        if (captured[i].type == 19 && captured[i].sensor == 10) {
            std::memcpy(&emitted, captured[i].payload.data(), sizeof(emitted));
        }
    }
    require(emitted == 0, "after the fallback the counter starts at the zero base");

    std::printf("Step delivery passed %d assertions\n", checks);
}
