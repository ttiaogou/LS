#include "step_hook.h"
#include <shadowhook.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>

namespace {
int checks = 0, forwardingInstalls = 0, hooks = 0, rollbacks = 0, calls = 0;
int removeTarget, destroyTarget, hookHandles[2];
void* removeProxy;
void* destroyProxy;
const char* scenario;
bool subscribed = true;
bool reservedHandleSubscribed = false;
bool removeDuringForward = false;
std::size_t capturedCount;
std::array<sensor_probe::StepWireEvent, 272> captured;
struct Receiver { std::array<std::byte, 56> prefix{}; std::int32_t uid = 10123; };
static_assert(offsetof(Receiver, uid) == 56);
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
const void* hasSensorReceiver;
std::int32_t hasSensorHandle;
int hasSensorCalls;
bool hasSensorSawReservedProbe;
bool hasSensor(const void* receiver, std::int32_t handle) {
    ++hasSensorCalls;
    hasSensorReceiver = receiver;
    hasSensorHandle = handle;
    if (handle == -1) { hasSensorSawReservedProbe = true; return reservedHandleSubscribed; }
    return subscribed && (handle == 10 || handle == 11 || handle == 12 || handle == 13);
}
void* removedReceiver;
std::int32_t removedHandle;
bool removeOriginal(void* receiver, std::int32_t handle) {
    removedReceiver = receiver; removedHandle = handle;
    subscribed = false; errno = EDOM; return true;
}
void* destroyedVtt;
void destroyOriginal(void*, void* vtt, void*, void*, void*, void*, void*, void*) {
    destroyedVtt = vtt;
    errno = ERANGE;
}
std::int32_t original(void*, const void* input, std::size_t count, void*, const void*) {
    ++calls;
    capturedCount = count;
    require(count <= captured.size(), "bounded wire batch");
    std::memcpy(captured.data(), input, count * sizeof(captured[0]));
    if (removeDuringForward) {
        removeDuringForward = false;
        int other;
        reinterpret_cast<bool (*)(void*, std::int32_t)>(removeProxy)(&other, 2);
        subscribed = true;
    }
    errno = EAGAIN;
    return -EAGAIN;
}
std::int64_t now() {
    timespec time{};
    require(clock_gettime(CLOCK_BOOTTIME, &time) == 0, "test clock");
    return time.tv_sec * 1'000'000'000LL + time.tv_nsec;
}
}

namespace sensor_probe {
// Host-only stand-in for the runtime-validated symbol resolver.
void* resolveLoadedServiceFunction(const char* name) {
    if (std::strcmp(scenario, "missing-symbol") == 0) return nullptr;
    if (std::strstr(name, "9hasSensorEi")) return reinterpret_cast<void*>(hasSensor);
    if (std::strstr(name, "12removeSensorEi")) return &removeTarget;
    if (std::strstr(name, "D2Ev")) return &destroyTarget;
    return nullptr;
}
}
void* shadowhook_hook_func_addr_2(void* target, void* proxy, void** out, std::uint32_t flags, ...) {
    require(flags == SHADOWHOOK_HOOK_WITH_UNIQUE_MODE, "unique lifecycle hooks");
    const int index = hooks++;
    require(index < 2, "bounded hook count");
    const bool removing = target == &removeTarget;
    require(removing || target == &destroyTarget, "exact lifecycle target");
    if ((removing && std::strcmp(scenario, "remove-failure") == 0) ||
        (!removing && std::strcmp(scenario, "destroy-failure") == 0)) return nullptr;
    if (removing) { removeProxy = proxy; *out = reinterpret_cast<void*>(removeOriginal); }
    else {
        destroyProxy = proxy;
        if (std::strcmp(scenario, "missing-trampoline") != 0) *out = reinterpret_cast<void*>(destroyOriginal);
    }
    return &hookHandles[index];
}
int shadowhook_unhook(void*) { ++rollbacks; return 0; }
namespace sensor_probe {
bool installPassthrough(void*) { ++forwardingInstalls; return true; }
}

int main(int argc, char** argv) {
    using namespace sensor_probe;
    require(argc == 2, "scenario argument");
    scenario = argv[1];
    int target;
    TargetReport report;
    report.address = &target;
    report.receiver = {56, 1, "receiver-alias-proven-subset", 0, 0};
    report.uid.offset = 56;
    require(!installStepDelivery(report) && forwardingInstalls == 0, "runtime byte verification mandatory");
    report.runtimeFragmentsVerified = true;
    report.uid.offset = 60;
    require(!installStepDelivery(report) && forwardingInstalls == 0, "receiver and UID proof agree");
    report.uid.offset = 56;
    const bool success = std::strcmp(scenario, "success") == 0 || std::strcmp(scenario, "both-variants") == 0;
    require(installStepDelivery(report) == success && stepDeliveryInstalled() == success, "installation gate");
    if (!success) {
        const int expected = std::strcmp(scenario, "destroy-failure") == 0 ? 1 :
            std::strcmp(scenario, "missing-trampoline") == 0 ? 2 : 0;
        require(rollbacks == expected, "partial lifecycle installation rollback");
        std::printf("Step hook %s passed %d assertions\n", scenario, checks);
        return 0;
    }
    require(installStepDelivery(report) && hooks == 2 && forwardingInstalls == 1, "idempotent install");
    if (std::strcmp(scenario, "both-variants") == 0) {
        // This ROM exposes a wake-up and a non-wake step counter/detector. A connection
        // holding both must receive one event per subscribed handle, for both types.
        const std::array<std::int32_t, 8> bothCatalog{10, 19, 11, 18, 12, 19, 13, 18};
        require(configureStepCatalog(bothCatalog), "both-variant catalog accepted");
        Receiver both;
        const std::array<std::int32_t, 1> only{both.uid};
        const auto first = now();
        StepSnapshot snap{true, 1, 100, 120, first, first + 5'000'000'000LL};
        require(configureStepSnapshot(snap, only), "both-variant controller accepted");
        StepWireEvent probe{};
        probe.version = sizeof(probe); probe.sensor = 2; probe.type = 1; probe.timestamp = first;
        std::int32_t res;
        int counters = 0, detectors = 0;
        bool counter10 = false, counter12 = false, detector11 = false, detector13 = false;
        auto classify = [&] {
            for (std::size_t i = 0; i < capturedCount; ++i) {
                if (captured[i].sensor == -1) continue;  // 被抑制的物理槽位不是合成事件
                if (captured[i].type == 19) {
                    ++counters;
                    if (captured[i].sensor == 10) counter10 = true;
                    if (captured[i].sensor == 12) counter12 = true;
                } else if (captured[i].type == 18) {
                    ++detectors;
                    if (captured[i].sensor == 11) detector11 = true;
                    if (captured[i].sensor == 13) detector13 = true;
                }
            }
        };
        StepWireEvent physical{};
        physical.version = sizeof(physical); physical.sensor = 10; physical.type = 19; physical.timestamp = first;
        const std::uint64_t physicalValue = 700;
        std::memcpy(physical.payload.data(), &physicalValue, sizeof(physicalValue));
        std::array<StepWireEvent, 2> registrationBatch{probe, physical};
        require(trySubmitSteps(original, &both, registrationBatch.data(), registrationBatch.size(),
                nullptr, nullptr, res), "both-variant submit");
        classify();
        require(counter10 && counter12 && counters == 2, "one counter event per subscribed counter handle");
        // The first batch anchors the trajectory; advance the snapshot so the next batch
        // carries detector pulses for both detector handles.
        const auto second = now();
        StepSnapshot advanced{true, 1, 101, 120, second, second + 5'000'000'000LL};
        require(configureStepSnapshot(advanced, only), "both-variant advance accepted");
        counters = detectors = 0;
        require(trySubmitSteps(original, &both, &probe, 1, nullptr, nullptr, res), "both-variant second submit");
        classify();
        require(detector11 && detector13 && detectors == 2, "one detector pulse per subscribed detector handle");
        require(counter12 && counters == 2, "counter fan-out preserved on later batches");
        std::printf("Step hook %s passed %d assertions\n", scenario, checks);
        return 0;
    }
    const std::array<std::int32_t, 4> catalog{10, 19, 11, 18};
    require(configureStepCatalog(catalog), "catalog accepted");
    const std::array<std::int32_t, 4> badCatalog{10, 19, 10, 18};
    require(!configureStepCatalog(badCatalog), "duplicate catalog rejected");
    Receiver receiver;
    const std::array<std::int32_t, 1> uids{receiver.uid};
    const auto time = now();
    StepSnapshot snapshot{true, 1, 100, 120, time, time + 5'000'000'000LL};
    require(configureStepSnapshot(snapshot, uids), "controller accepted");
    StepWireEvent carrier{};
    carrier.version = sizeof(carrier); carrier.sensor = 2; carrier.type = 1; carrier.timestamp = time;
    std::int32_t result;
    receiver.uid = 10124;
    require(!trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result) && calls == 0,
        "UID not selected never submitted");
    receiver.uid = uids[0];
    reservedHandleSubscribed = true;
    require(!trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result) && calls == 0,
        "reserved handle conflict refuses synthesis");
    reservedHandleSubscribed = false;
    // 真实传感器在注册时会立刻发一次当前计步值：App 的基线就建立在这条上。
    StepWireEvent physical{};
    physical.version = sizeof(physical); physical.sensor = 10; physical.type = 19; physical.timestamp = time;
    const std::uint64_t physicalValue = 700;
    std::memcpy(physical.payload.data(), &physicalValue, sizeof(physicalValue));
    std::array<StepWireEvent, 2> registrationBatch{carrier, physical};
    require(trySubmitSteps(original, &receiver, registrationBatch.data(), registrationBatch.size(),
            nullptr, nullptr, result), "registration batch submitted");
    require(result == -EAGAIN && errno == EAGAIN && capturedCount == 3 && captured[2].type == 19,
        "actual adapter retains uncertain return and emits counter");
    require(captured[1].sensor == -1, "physical counter suppressed in its original slot");
    std::uint64_t firstEmitted = 0;
    std::memcpy(&firstEmitted, captured[2].payload.data(), sizeof(firstEmitted));
    require(firstEmitted == physicalValue, "first synthesized counter continues from the physical value");
    require(submittedStepEvents() == 1 && suppressedStepEvents() == 1, "submission diagnostics");
    require(trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result) && capturedCount == 1,
        "uncertain socket failure not duplicated");
    // 空批（没有计步事件）不能把"最后一次计数"清零
    require(stepLastCounterValue() == 700, "a batch without a counter keeps the last counter");
    auto advance = [&](double steps) {
        const auto timestamp = now();
        snapshot.steps = steps;
        snapshot.cadenceSpm = 0;
        snapshot.sampleNanos = timestamp;
        snapshot.expiryNanos = timestamp + 5'000'000'000LL;
        require(configureStepSnapshot(snapshot, uids), "advanced controller accepted");
    };
    advance(110);
    require(trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result),
        "counter advances before unrelated lifecycle changes");
    require(stepLastCounterValue() == 710, "aligned counter includes synthetic delta");
    Receiver unrelated;
    require(reinterpret_cast<bool (*)(void*, std::int32_t)>(removeProxy)(&unrelated, 2),
        "unrelated sensor removed");
    subscribed = true;
    reinterpret_cast<void (*)(void*, void*, void*, void*, void*, void*, void*, void*)>(destroyProxy)(
        &unrelated, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    advance(111);
    require(trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result),
        "target continues after unrelated remove and destruction");
    require(stepLastCounterValue() == 711, "unrelated lifecycle never resets target alignment");
    advance(112);
    removeDuringForward = true;
    require(trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result),
        "lifecycle re-entry from the original cannot deadlock");
    advance(113);
    require(trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result) &&
        stepLastCounterValue() == 713, "reentrant removal preserves target counter continuity");
    // 辅助函数 ABI：hasSensor 收 (const this, handle)，removeSensor 收 (this, handle) 并回传 bool
    require(hasSensorCalls >= 3 && hasSensorReceiver == &receiver && hasSensorSawReservedProbe,
        "hasSensor probed the live receiver, including the reserved handle");
    require(hasSensorHandle == 11, "hasSensor forwarded each catalog handle");
    require(reinterpret_cast<bool (*)(void*, std::int32_t)>(removeProxy)(&receiver, 10) && errno == EDOM,
        "remove hook retains result and errno");
    require(removedReceiver == &receiver && removedHandle == 10,
        "removeSensor forwarded (this, handle)");
    require(!trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result), "unsubscribed excluded");
    subscribed = true;
    // 同一个 receiver 重新订阅复用同一连接：基准已固定、计数没变，因此不会重发计步（无回跳）。
    require(trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result) && capturedCount == 1,
        "re-subscribe neither resets the adopted base nor re-emits the counter");
    void* vtt = &destroyTarget;
    reinterpret_cast<void (*)(void*, void*, void*, void*, void*, void*, void*, void*)>(destroyProxy)(
        &receiver, vtt, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    require(destroyedVtt == vtt, "destructor forwards the virtual-table argument");
    require(errno == ERANGE, "destructor preserves errno");
    // 析构器 VTT 校验：运行期留下"D2 被调用 + VTT 非空"的证据，空 VTT 不计入
    require(destructorInvocations() == 2 && destructorVttObservations() == 1,
        "destructor invocation with a virtual-table argument is observed");
    reinterpret_cast<void (*)(void*, void*, void*, void*, void*, void*, void*, void*)>(destroyProxy)(
        &receiver, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    require(destructorInvocations() == 3 && destructorVttObservations() == 1,
        "null virtual-table argument is not counted as an observation");
    // Pointer reuse after actual destruction is a new connection, not the old stream.
    const std::uint64_t recycledPhysical = 900;
    std::memcpy(registrationBatch[1].payload.data(), &recycledPhysical, sizeof(recycledPhysical));
    require(trySubmitSteps(original, &receiver, registrationBatch.data(), registrationBatch.size(),
            nullptr, nullptr, result), "recycled receiver starts a new stream");
    require(stepLastCounterValue() == 900, "recycled pointer adopts its own physical base");
    disableStepDelivery();
    require(!trySubmitSteps(original, &receiver, &carrier, 1, nullptr, nullptr, result), "disable restores forwarding");
    const std::array<std::int32_t, 1> systemUid{1000};
    require(!configureStepSnapshot(snapshot, systemUid), "system UID rejected");
    std::printf("Step hook %s passed %d assertions\n", scenario, checks);
}
