#include "step_hook.h"
#include "passthrough_hook.h"

#include <shadowhook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <time.h>
#include <type_traits>

namespace sensor_probe {
namespace {
constexpr std::size_t maxUids = 4096;
constexpr std::size_t maxCatalogInts = 32;
struct Configuration {
    StepSnapshot snapshot;
    std::array<std::int32_t, maxUids> uids{};
    std::size_t uidCount = 0;
    std::array<std::int32_t, maxCatalogInts> catalog{};
    std::size_t catalogSize = 0;
};
Configuration configuration;
std::mutex configurationMutex;
struct Connection {
    std::atomic<void*> receiver{nullptr};
    std::atomic<bool> retired{false};
    std::optional<StepStream> stream;
    StepSubscription subscription;
};
std::array<Connection, 64> connections;
std::mutex streamsMutex;
std::atomic<std::uint64_t> lifecycleEpoch{1};
std::atomic<unsigned> lifecycleChanges{0};
std::uint64_t nextLifetime = 0;
std::atomic<bool> installed{false};
std::atomic<std::uint64_t> synthesized{0}, suppressed{0};
// 析构器 ABI 的运行期观察：本 ROM 的 D2 是 virtual-base 形态 (this, VTT)，代理完整转发后
// 这里应当看到非空 VTT；它给"析构器 VTT 校验"留下设备侧证据，进 diagnostics 槽位。
std::atomic<std::uint64_t> destructorCallsSeen{0}, destructorVttSeen{0};
std::atomic<std::uint64_t> counterEvents{0}, detectorPulses{0}, lastCounterValue{0};
int uidOffset = -1;
using HasSensor = bool (*)(const void*, std::int32_t);
using RemoveSensor = bool (*)(void*, std::int32_t);
// 辅助函数 ABI 与析构器一样按完整签名钉住，防止被收窄：hasSensor 是 const 成员
// （符号 _ZNK…hasSensorEi），removeSensor 是普通成员，两者都是 (this, handle) -> bool。
using AllArgumentHasSensor = bool (*)(const void*, std::int32_t);
using AllArgumentRemoveSensor = bool (*)(void*, std::int32_t);
static_assert(std::is_same_v<HasSensor, AllArgumentHasSensor>,
    "hasSensor must stay (const this, handle) -> bool");
static_assert(std::is_same_v<RemoveSensor, AllArgumentRemoveSensor>,
    "removeSensor must stay (this, handle) -> bool");
// The complete-object destructor is the virtual-base form on this ROM:
// ~SensorEventConnection(this, VTT). The deleting destructor at 0x98dc8 loads the
// table (ldr x1,[x1,#40]) and passes it in x1, and the D2 body at 0x98e08
// dereferences x1 at +0x18/+0x34/+0x3c to restore each virtual base vptr. Declaring
// the original as void(*)(void*) left x1 holding stale garbage, so that body stored
// a bogus vptr and faulted at D2+0x30 (SIGSEGV, fault addr 0xffffffffffffffec,
// system_server pid 2922 on 2026-10-15). Forward every integer argument register:
// harmless for a one-argument destructor, required for this virtual-base one.
using Destroy = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
// Host tests cannot reproduce the register clobbering that crashed system_server: with a
// narrowed proxy the second argument survives by luck on the host compiler. Pin the shape
// against a literal type instead, so neither the trampoline alias nor the proxy can be
// narrowed back to one argument without breaking the build.
using AllArgumentDestroy = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
static_assert(std::is_same_v<Destroy, AllArgumentDestroy>,
    "destructor trampoline must forward every integer argument register");
HasSensor hasSensor = nullptr;
void* originalRemove = nullptr;
void* originalDestroy = nullptr;
const char* status = "steps-not-installed";

void invalidate() {
    if (lifecycleEpoch.fetch_add(1, std::memory_order_acq_rel) ==
        std::numeric_limits<std::uint64_t>::max()) installed.store(false, std::memory_order_release);
}
bool removeProxy(void* receiver, std::int32_t handle) {
    lifecycleChanges.fetch_add(1, std::memory_order_acq_rel);
    invalidate();
    const bool result = reinterpret_cast<RemoveSensor>(originalRemove)(receiver, handle);
    const int saved = errno;
    invalidate();
    lifecycleChanges.fetch_sub(1, std::memory_order_release);
    errno = saved;
    return result;
}
void destroyProxy(void* receiver, void* vtt, void* second, void* third, void* fourth, void* fifth,
    void* sixth, void* seventh) {
    destructorCallsSeen.fetch_add(1, std::memory_order_relaxed);
    if (vtt != nullptr) destructorVttSeen.fetch_add(1, std::memory_order_relaxed);
    lifecycleChanges.fetch_add(1, std::memory_order_acq_rel);
    // Lifecycle hooks must not acquire streamsMutex: the original may re-enter
    // them while sendEvents owns it. Reclaim the matching slot on the next send.
    auto retire = [&] {
        for (auto& entry : connections)
            if (entry.receiver.load(std::memory_order_acquire) == receiver)
                entry.retired.store(true, std::memory_order_release);
    };
    retire();
    invalidate();
    reinterpret_cast<Destroy>(originalDestroy)(receiver, vtt, second, third, fourth, fifth, sixth, seventh);
    const int saved = errno;
    retire();
    invalidate();
    lifecycleChanges.fetch_sub(1, std::memory_order_release);
    errno = saved;
}
static_assert(std::is_same_v<decltype(&removeProxy), AllArgumentRemoveSensor>,
    "removeSensor proxy must keep (this, handle) -> bool");
static_assert(std::is_same_v<decltype(&destroyProxy), AllArgumentDestroy>,
    "destructor proxy must forward every integer argument register: the virtual-base destructor "
    "reads its VTT from x1, and a narrowed proxy lets the caller clobber it");

}  // namespace

bool installStepDelivery(const TargetReport& report) {
    if (installed.load(std::memory_order_acquire)) return true;
    if (!report.runtimeFragmentsVerified || report.address == nullptr ||
        report.receiver.offset < 0 || report.receiver.offset > 4092 ||
        report.receiver.offset != report.uid.offset || report.receiver.matches != 1 ||
        std::strcmp(report.receiver.status, "receiver-alias-proven-subset") != 0) {
        status = "steps-receiver-proof-refused";
        return false;
    }
    // Android linker namespaces can deny a handle even though the service is
    // already mapped. Apply the locator's ELF + loaded RX byte validation to
    // every helper rather than requiring dlopen from the module's namespace.
    hasSensor = reinterpret_cast<HasSensor>(resolveLoadedServiceFunction(
        "_ZNK7android13SensorService21SensorEventConnection9hasSensorEi"));
    void* remove = resolveLoadedServiceFunction(
        "_ZN7android13SensorService21SensorEventConnection12removeSensorEi");
    void* destroy = resolveLoadedServiceFunction(
        "_ZN7android13SensorService21SensorEventConnectionD2Ev");
    if (hasSensor == nullptr || remove == nullptr || destroy == nullptr) {
        status = "steps-lifecycle-symbols-missing-or-unverified";
        return false;
    }
    // Install the established forwarding proxy first. Until the release-store
    // below, all traffic remains exactly passthrough, including partial failures.
    if (!installPassthrough(report.address)) { status = "steps-forwarding-install-failed"; return false; }
    void* removeHook = shadowhook_hook_func_addr_2(remove, reinterpret_cast<void*>(removeProxy),
        &originalRemove, SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);
    if (removeHook == nullptr || originalRemove == nullptr) {
        if (removeHook != nullptr) shadowhook_unhook(removeHook);
        status = "steps-remove-hook-failed";
        return false;
    }
    void* destroyHook = shadowhook_hook_func_addr_2(destroy, reinterpret_cast<void*>(destroyProxy),
        &originalDestroy, SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);
    if (destroyHook == nullptr || originalDestroy == nullptr) {
        if (destroyHook != nullptr) shadowhook_unhook(destroyHook);
        shadowhook_unhook(removeHook);
        status = "steps-destroy-hook-failed";
        return false;
    }
    uidOffset = report.receiver.offset;
    status = "steps-installed-experimental";
    installed.store(true, std::memory_order_release);
    return true;
}

const char* stepDeliveryStatus() { return status; }
bool stepDeliveryInstalled() { return installed.load(std::memory_order_acquire); }
bool configureStepCatalog(std::span<const std::int32_t> pairs) {
    if (pairs.empty() || pairs.size() % 2 != 0 || pairs.size() > maxCatalogInts) return false;
    for (std::size_t i = 0; i < pairs.size(); i += 2) {
        if (pairs[i] < 0 || (pairs[i + 1] != 18 && pairs[i + 1] != 19)) return false;
        for (std::size_t j = 0; j < i; j += 2) if (pairs[i] == pairs[j]) return false;
    }
    std::lock_guard lock(configurationMutex);
    const bool changed = configuration.catalogSize != pairs.size() ||
        !std::equal(pairs.begin(), pairs.end(), configuration.catalog.begin());
    std::copy(pairs.begin(), pairs.end(), configuration.catalog.begin());
    configuration.catalogSize = pairs.size();
    if (changed) invalidate();
    return true;
}
bool configureStepSnapshot(const StepSnapshot& snapshot, std::span<const std::int32_t> uids) {
    if (uids.size() > maxUids || (snapshot.active &&
        (snapshot.session == 0 || !std::isfinite(snapshot.steps) || snapshot.steps < 0 ||
         snapshot.steps > StepStream::maxExactSteps || !std::isfinite(snapshot.cadenceSpm) ||
         snapshot.cadenceSpm < 0 || snapshot.cadenceSpm > StepStream::maxCadenceSpm ||
         snapshot.sampleNanos <= 0 || snapshot.expiryNanos <= snapshot.sampleNanos))) {
        disableStepDelivery();
        return false;
    }
    for (auto uid : uids) {
        if (uid < 0 || uid % 100000 < 10000 || uid % 100000 >= 20000) {
            disableStepDelivery();
            return false;
        }
    }
    std::lock_guard lock(configurationMutex);
    if (configuration.snapshot.active != snapshot.active ||
        configuration.snapshot.session != snapshot.session) invalidate();
    configuration.snapshot = snapshot;
    std::copy(uids.begin(), uids.end(), configuration.uids.begin());
    configuration.uidCount = uids.size();
    std::sort(configuration.uids.begin(), configuration.uids.begin() + configuration.uidCount);
    return true;
}
void disableStepDelivery() {
    std::lock_guard lock(configurationMutex);
    configuration.snapshot.active = false;
    invalidate();
}

bool trySubmitSteps(SendEvents original, void* receiver, const void* events, std::size_t count,
    void* scratch, const void* flushMap, std::int32_t& result) {
    const int incomingErrno = errno;
    if (!installed.load(std::memory_order_acquire) || receiver == nullptr || events == nullptr ||
        count == 0 || count > maxDeliveryInputEvents) return false;
    StepSnapshot snapshot;
    std::array<std::int32_t, maxCatalogInts> catalog;
    std::size_t catalogSize;
    {
        std::unique_lock lock(configurationMutex, std::try_to_lock);
        if (!lock.owns_lock() || !configuration.snapshot.active || configuration.catalogSize == 0) return false;
        std::int32_t uid;
        // Only reached after live runtime code/receiver proof; never in probe or passthrough.
        std::memcpy(&uid, static_cast<const std::byte*>(receiver) + uidOffset, sizeof(uid));
        if (!std::binary_search(configuration.uids.begin(),
            configuration.uids.begin() + configuration.uidCount, uid)) return false;
        snapshot = configuration.snapshot;
        catalog = configuration.catalog;
        catalogSize = configuration.catalogSize;
    }
    std::unique_lock lock(streamsMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (lifecycleChanges.load(std::memory_order_acquire) != 0) return false;
    const auto epoch = lifecycleEpoch.load(std::memory_order_acquire);
    for (auto& entry : connections) {
        if (entry.retired.exchange(false, std::memory_order_acq_rel)) {
            entry.receiver.store(nullptr, std::memory_order_release);
            entry.stream.reset();
            entry.subscription = {};
        }
    }
    StepSubscription subscription;
    if (hasSensor(receiver, -1)) return false;
    // A connection may hold the wake-up and the non-wake handle at the same time;
    // register every catalog handle it actually subscribed to.
    for (std::size_t i = 0; i < catalogSize; i += 2) {
        if (!hasSensor(receiver, catalog[i])) continue;
        const bool accepted = catalog[i + 1] == 19 ? subscription.addCounter(catalog[i])
                                                   : subscription.addDetector(catalog[i]);
        if (!accepted) return false;
    }
    if (subscription.empty()) return false;
    Connection* entry = nullptr;
    Connection* empty = nullptr;
    for (auto& candidate : connections) {
        const auto candidateReceiver = candidate.receiver.load(std::memory_order_acquire);
        if (candidateReceiver == receiver) { entry = &candidate; break; }
        if (candidateReceiver == nullptr) empty = &candidate;
    }
    if (entry == nullptr) {
        if (empty == nullptr || nextLifetime == std::numeric_limits<std::uint64_t>::max()) return false;
        entry = empty;
        entry->stream.emplace(++nextLifetime);
        entry->receiver.store(receiver, std::memory_order_release);
    }
    subscription.epoch = entry->subscription.epoch;
    if (!(subscription == entry->subscription)) {
        if (subscription.epoch == std::numeric_limits<std::uint64_t>::max()) return false;
        ++subscription.epoch;
    }
    if (lifecycleChanges.load(std::memory_order_acquire) != 0 ||
        lifecycleEpoch.load(std::memory_order_acquire) != epoch) return false;
    entry->subscription = subscription;
    timespec time{};
    if (clock_gettime(CLOCK_BOOTTIME, &time) != 0) return false;
    const auto now = static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000LL + time.tv_nsec;
    errno = incomingErrno;
    const auto delivery = submitSteps(original, receiver, events, count, scratch, flushMap,
        *entry->stream, snapshot, subscription, now, true);
    const int outgoingErrno = errno;
    if (delivery.submitted) {
        synthesized.fetch_add(delivery.synthesized, std::memory_order_relaxed);
        suppressed.fetch_add(delivery.suppressed, std::memory_order_relaxed);
        counterEvents.fetch_add(delivery.counterEvents, std::memory_order_relaxed);
        detectorPulses.fetch_add(delivery.detectorPulses, std::memory_order_relaxed);
        // 只有真的发出过计步事件才更新"最后一次计数"：否则一个没有计步的空批会把诊断写成 0，
        // 让设备侧看起来像"一直发 0 步"（实际投递是正常的）。
        if (delivery.counterEvents > 0) {
            lastCounterValue.store(delivery.lastCounterValue, std::memory_order_relaxed);
        }
    }
    result = delivery.result;
    lock.unlock();
    errno = outgoingErrno;
    return true;  // Adapter already called original, including its fallback paths.
}
std::uint64_t submittedStepEvents() { return synthesized.load(std::memory_order_relaxed); }
std::uint64_t suppressedStepEvents() { return suppressed.load(std::memory_order_relaxed); }
std::uint64_t destructorInvocations() { return destructorCallsSeen.load(std::memory_order_relaxed); }
std::uint64_t destructorVttObservations() { return destructorVttSeen.load(std::memory_order_relaxed); }
std::uint64_t stepCounterEvents() { return counterEvents.load(std::memory_order_relaxed); }
std::uint64_t stepDetectorPulses() { return detectorPulses.load(std::memory_order_relaxed); }
std::uint64_t stepLastCounterValue() { return lastCounterValue.load(std::memory_order_relaxed); }

std::int64_t stepCounterShift() {
    std::unique_lock lock(streamsMutex, std::try_to_lock);
    if (!lock.owns_lock()) return 0;
    for (auto& entry : connections)
        if (entry.stream && entry.stream->counterBaseAdopted()) return entry.stream->counterShiftValue();
    return 0;
}
std::int64_t stepPhysicalBase() {
    std::unique_lock lock(streamsMutex, std::try_to_lock);
    if (!lock.owns_lock()) return -1;
    for (auto& entry : connections)
        if (entry.stream && entry.stream->counterBaseAdopted()) return entry.stream->physicalBaseValue();
    return -1;
}
bool stepCounterBaseAdopted() {
    std::unique_lock lock(streamsMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    for (auto& entry : connections)
        if (entry.stream && entry.stream->counterBaseAdopted()) return true;
    return false;
}
}  // namespace sensor_probe
