#include "step_wire.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <type_traits>
#ifdef __ANDROID__
#include <android/sensor.h>
#endif

namespace sensor_probe {
static_assert(std::is_standard_layout_v<StepWireEvent> && std::is_trivially_copyable_v<StepWireEvent>);
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(StepWireEvent) == 104 && alignof(StepWireEvent) == 8);
static_assert(offsetof(StepWireEvent, type) == 8 && offsetof(StepWireEvent, timestamp) == 16);
static_assert(offsetof(StepWireEvent, payload) == 24 && offsetof(StepWireEvent, flags) == 88);
static_assert(sizeof(float) == 4);
#ifdef __ANDROID__
static_assert(sizeof(StepWireEvent) == sizeof(ASensorEvent));
static_assert(alignof(StepWireEvent) == alignof(ASensorEvent));
static_assert(offsetof(StepWireEvent, version) == offsetof(ASensorEvent, version));
static_assert(offsetof(StepWireEvent, sensor) == offsetof(ASensorEvent, sensor));
static_assert(offsetof(StepWireEvent, type) == offsetof(ASensorEvent, type));
static_assert(offsetof(StepWireEvent, timestamp) == offsetof(ASensorEvent, timestamp));
static_assert(offsetof(StepWireEvent, payload) == offsetof(ASensorEvent, u64));
static_assert(offsetof(StepWireEvent, flags) == offsetof(ASensorEvent, flags));
static_assert(offsetof(StepWireEvent, reserved1) == offsetof(ASensorEvent, reserved1));
static_assert(ASENSOR_TYPE_STEP_COUNTER == 19 && ASENSOR_TYPE_STEP_DETECTOR == 18);
#endif

bool encodeNonWakeStep(const PlannedStep& step, StepWireEvent& destination) noexcept {
    if (step.handle < 0 || step.timestampNanos <= 0) return false;
    StepWireEvent event{};
    event.version = sizeof(event);
    event.sensor = step.handle;
    event.timestamp = step.timestampNanos;
    switch (step.kind) {
        case StepKind::counter:
            event.type = 19;
            std::memcpy(event.payload.data(), &step.value, sizeof(step.value));
            break;
        case StepKind::detector: {
            if (step.value != 1) return false;
            event.type = 18;
            const float pulse = 1.0F;
            std::memcpy(event.payload.data(), &pulse, sizeof(pulse));
            break;
        }
        default: return false;
    }
    destination = event;
    return true;
}

bool readStepEvent(const void* bytes, std::size_t available, StepWireEvent& destination) noexcept {
    if (bytes == nullptr || available < sizeof(StepWireEvent)) return false;
    std::memcpy(&destination, bytes, sizeof(StepWireEvent));
    return true;
}

int describeStepEvent(const StepWireEvent& event, char* out, std::size_t capacity) noexcept {
    if (out == nullptr || capacity == 0) return 0;
    std::uint64_t payloadU64 = 0;
    float payloadF32 = 0.0F;
    std::memcpy(&payloadU64, event.payload.data(), sizeof(payloadU64));
    std::memcpy(&payloadF32, event.payload.data(), sizeof(payloadF32));
    const int written = std::snprintf(out, capacity,
        "size=%zu sensor=0x%08x type=%d version=%d ts=%lld payloadU64=%llu payloadF32=%.4f flags=0x%08x",
        sizeof(StepWireEvent), static_cast<unsigned>(event.sensor), event.type, event.version,
        static_cast<long long>(event.timestamp), static_cast<unsigned long long>(payloadU64),
        static_cast<double>(payloadF32), static_cast<unsigned>(event.flags));
    if (written < 0) { out[0] = '\0'; return 0; }
    return written < static_cast<int>(capacity) ? written : static_cast<int>(capacity) - 1;
}
}  // namespace sensor_probe
