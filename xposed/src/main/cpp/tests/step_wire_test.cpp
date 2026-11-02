#include "step_wire.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
}
int main() {
    using namespace sensor_probe;
    StepWireEvent event;
    std::memset(&event, 0xa5, sizeof(event));
    require(encodeNonWakeStep({StepKind::counter, 10, 123456789, 2350}, event), "counter encoded");
    require(event.version == 104 && event.sensor == 10 && event.type == 19 && event.timestamp == 123456789,
        "counter header");
    std::uint64_t counter;
    std::memcpy(&counter, event.payload.data(), sizeof(counter));
    require(counter == 2350, "integer counter payload (not float)");
    require(event.flags == 0 && event.reserved0 == 0, "no ACK or reserved flags");
    for (const auto reserved : event.reserved1) require(reserved == 0, "reserved tail zeroed");
    for (std::size_t i = 8; i < event.payload.size(); ++i)
        require(event.payload[i] == std::byte{0}, "unused counter payload zeroed");
    require(encodeNonWakeStep({StepKind::counter, 0, 1, std::numeric_limits<std::uint64_t>::max()}, event),
        "wire format preserves full uint64");
    std::memcpy(&counter, event.payload.data(), sizeof(counter));
    require(counter == std::numeric_limits<std::uint64_t>::max(), "no float precision loss");
    require(encodeNonWakeStep({StepKind::detector, 11, 987654321, 1}, event), "detector encoded");
    float pulse;
    std::memcpy(&pulse, event.payload.data(), sizeof(pulse));
    require(event.type == 18 && event.sensor == 11 && pulse == 1.0F, "detector float pulse");
    for (std::size_t i = 4; i < event.payload.size(); ++i)
        require(event.payload[i] == std::byte{0}, "unused detector payload zeroed");
    const auto previous = event;
    for (const PlannedStep invalid : {
        PlannedStep{StepKind::counter, -1, 1, 1},
        PlannedStep{StepKind::counter, 0, 0, 1},
        PlannedStep{StepKind::detector, 0, -1, 1},
        PlannedStep{StepKind::detector, 0, 1, 0},
        PlannedStep{StepKind::detector, 0, 1, 2},
        PlannedStep{static_cast<StepKind>(99), 0, 1, 1}}) {
        require(!encodeNonWakeStep(invalid, event), "invalid wire event refused");
        require(std::memcmp(&event, &previous, sizeof(event)) == 0, "refusal leaves output untouched");
    }
    // Integration stays in local buffers: no sensor connection or transport exists.
    StepStream stream(1);
    const StepSnapshot snapshot{true, 1, 100.25, 120, 1'000'000'000LL, 10'000'000'000LL};
    const StepSubscription subscription = subscriptionOf({10}, {11}, 1);
    std::array<PlannedStep, 16> plan;
    auto batch = stream.plan(snapshot, subscription, snapshot.sampleNanos, plan);
    require(batch.valid && stream.commit(batch, true), "seed planning integration");
    batch = stream.plan(snapshot, subscription, snapshot.sampleNanos + 500'000'000LL, plan);
    require(batch.valid && batch.count == 2, "two-event plan");
    for (std::size_t i = 0; i < batch.count; ++i) {
        require(encodeNonWakeStep(plan[i], event), "planned event encodes");
        require(event.type == (plan[i].kind == StepKind::counter ? 19 : 18), "wire type follows plan");
    }
    require(stream.revision() == 1, "encoding alone does not acknowledge delivery");

    // 取样：只读、按 104 字节记录，短缓冲必须拒绝
    std::array<std::byte, sizeof(StepWireEvent)> raw{};
    StepWireEvent sampled{};
    require(!readStepEvent(nullptr, sizeof(raw), sampled), "null source refused");
    require(!readStepEvent(raw.data(), sizeof(raw) - 1, sampled), "short buffer refused");
    StepWireEvent counterEvent{};
    require(encodeNonWakeStep({StepKind::counter, 10, 42, 777}, counterEvent), "sample source encodes");
    std::memcpy(raw.data(), &counterEvent, sizeof(raw));
    require(readStepEvent(raw.data(), sizeof(raw), sampled) && sampled.sensor == 10 &&
            sampled.type == 19 && sampled.timestamp == 42, "sampled record matches the wire layout");
    char line[256];
    const int written = describeStepEvent(sampled, line, sizeof(line));
    require(written > 0 && std::strstr(line, "type=19") != nullptr &&
            std::strstr(line, "payloadU64=777") != nullptr, "description carries type and payload");
    char tight[24];
    require(describeStepEvent(sampled, tight, sizeof(tight)) > 0 && tight[sizeof(tight) - 1] == '\0',
        "description truncates and stays terminated");
    require(describeStepEvent(sampled, nullptr, sizeof(line)) == 0, "null destination refused");
    std::printf("Step wire passed %d assertions\n", checks);
}
