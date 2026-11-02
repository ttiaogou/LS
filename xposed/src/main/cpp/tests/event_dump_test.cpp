// 诊断取样（SENSOR_PROBE_EVENT_DUMP）的行为契约：只读、只计 step 事件、有界、不动转发语义。
#include "passthrough_hook.h"
#include "step_wire.h"
#include <shadowhook.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int checks = 0;
int originalCalls = 0;
std::size_t lastCount = 0;
const void* lastEvents = nullptr;
int target = 0, handle = 0;
void* capturedProxy = nullptr;

void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
std::int32_t original(void* receiver, const void* events, std::size_t count, void* scratch,
    const void* flush) {
    require(receiver == &target && scratch == &target && flush == &target, "args forwarded intact");
    lastEvents = events;
    lastCount = count;
    ++originalCalls;
    errno = EAGAIN;
    return -EAGAIN;
}
sensor_probe::StepWireEvent stepEvent(std::int32_t type, std::int32_t sensor, std::uint64_t payload) {
    sensor_probe::StepWireEvent event{};
    event.version = sizeof(event);
    event.sensor = sensor;
    event.type = type;
    event.timestamp = 123456789;
    std::memcpy(event.payload.data(), &payload, sizeof(payload));
    return event;
}
}

int shadowhook_init(shadowhook_mode_t mode, bool debuggable) {
    require(mode == SHADOWHOOK_MODE_UNIQUE && !debuggable, "unique non-debug engine");
    return 0;
}
void* shadowhook_hook_func_addr_2(void* address, void* proxy, void** out, std::uint32_t flags, ...) {
    require(address == &target && flags == SHADOWHOOK_HOOK_WITH_UNIQUE_MODE, "target and hook mode");
    capturedProxy = proxy;
    *out = reinterpret_cast<void*>(original);
    return &handle;
}
int shadowhook_unhook(void*) { return 0; }

int main() {
    using namespace sensor_probe;
    require(dumpedStepEventCount() == 0, "no events counted before install");
    require(installPassthrough(&target), "passthrough installs");
    require(capturedProxy != nullptr, "proxy captured");
    const auto proxy = reinterpret_cast<SendEvents>(capturedProxy);
    require(dumpedStepEventCount() == 0, "install alone counts nothing");

    // 第一批：混入非 step 事件，只计 step 类型，且缓冲区不被改写
    std::array<StepWireEvent, 5> small{};
    small[0] = stepEvent(1, 0, 0);
    for (std::size_t i = 1; i < small.size(); ++i) small[i] = stepEvent(i % 2 == 0 ? 19 : 18, 10, 2000 + i);
    std::array<StepWireEvent, 5> before{};
    std::memcpy(before.data(), small.data(), sizeof(small));
    require(proxy(&target, small.data(), small.size(), &target, &target) == -EAGAIN && errno == EAGAIN,
        "forwarding contract unchanged by the dump");
    require(originalCalls == 1 && lastCount == small.size() && lastEvents == small.data(),
        "original received the very same batch");
    require(std::memcmp(before.data(), small.data(), sizeof(small)) == 0, "dump never writes to events");
    require(dumpedStepEventCount() == 4, "only step events counted");

    // 第二批：超过上限后停止采样（有界），转发不受影响
    std::array<StepWireEvent, 12> large{};
    large[0] = stepEvent(1, 0, 0);
    for (std::size_t i = 1; i < large.size(); ++i) large[i] = stepEvent(i % 2 == 0 ? 19 : 18, 11, 3000 + i);
    require(proxy(&target, large.data(), large.size(), &target, &target) == -EAGAIN && errno == EAGAIN,
        "second batch forwards");
    require(originalCalls == 2 && lastCount == large.size(), "second batch reached the original");
    require(dumpedStepEventCount() == 8, "dump is bounded to eight step events");
    require(proxy(&target, large.data(), large.size(), &target, &target) == -EAGAIN, "third batch forwards");
    require(originalCalls == 3 && dumpedStepEventCount() == 8, "bound holds across batches");
    std::printf("Event dump passed %d assertions\n", checks);
}
