#include "passthrough_hook.h"

#include <shadowhook.h>
#ifdef SENSOR_PROBE_EVENT_DUMP
#include "step_wire.h"
#include <android/log.h>
#include <atomic>
#include <cerrno>
#endif
#ifdef SENSOR_PROBE_STEPS
#include "step_hook.h"
#include <cerrno>
#endif

namespace sensor_probe {
namespace {
void* originalAddress = nullptr;
void* hookHandle = nullptr;
ForwardingStats stats;
const char* status = "passthrough-not-installed";

#ifdef SENSOR_PROBE_EVENT_DUMP
// 布局核对取样：只读、有界。真机上跑一次任意计步客户端，日志里就会留下前若干条真实
// step 事件（含 104 字节记录的字段分解），用来和 step_wire.h 的断言对照。
constexpr int maxDumpedStepEvents = 8;
std::atomic<int> dumpedStepEvents{0};
void dumpStepEvents(const void* events, std::size_t count) {
    if (events == nullptr || count == 0) return;
    const auto* base = static_cast<const std::byte*>(events);
    for (std::size_t i = 0; i < count; ++i) {
        if (dumpedStepEvents.load(std::memory_order_relaxed) >= maxDumpedStepEvents) return;
        StepWireEvent event{};
        if (!readStepEvent(base + i * sizeof(StepWireEvent), sizeof(StepWireEvent), event)) return;
        if (event.type != 18 && event.type != 19) continue;
        char line[256];
        describeStepEvent(event, line, sizeof(line));
        __android_log_print(ANDROID_LOG_INFO, "GlobalSensorProbe", "raw step event: %s", line);
        dumpedStepEvents.fetch_add(1, std::memory_order_relaxed);
    }
}
#endif

std::int32_t proxy(void* connection, const void* events, std::size_t count,
    void* scratch, const void* flushMap) {
#ifdef SENSOR_PROBE_EVENT_DUMP
    // 取样绝不能改变转发语义：日志入口可能改 errno，先存后还。
    const int dumpErrno = errno;
    dumpStepEvents(events, count);
    errno = dumpErrno;
#endif
#ifdef SENSOR_PROBE_STEPS
    const int saved = errno;
    std::int32_t result;
    if (trySubmitSteps(reinterpret_cast<SendEvents>(originalAddress), connection, events,
        count, scratch, flushMap, result)) {
        const int outgoing = errno;
        stats.batches.fetch_add(1, std::memory_order_relaxed);
        stats.events.fetch_add(count, std::memory_order_relaxed);
        if (result < 0) stats.failures.fetch_add(1, std::memory_order_relaxed);
        errno = outgoing;
        return result;
    }
    errno = saved;
#endif
    return forwardSendEvents(reinterpret_cast<SendEvents>(originalAddress), stats,
        connection, events, count, scratch, flushMap);
}
}  // namespace

bool installPassthrough(void* target) {
    if (hookHandle != nullptr) return true;
    if (target == nullptr) { status = "passthrough-no-target"; return false; }
    if (shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false) != 0) {
        status = "passthrough-engine-init-failed";
        return false;
    }
    // UNIQUE refuses competing hooks rather than chaining an unknown event rewriter.
    hookHandle = shadowhook_hook_func_addr_2(target, reinterpret_cast<void*>(proxy),
        &originalAddress, SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);
    if (hookHandle != nullptr && originalAddress == nullptr) {
        shadowhook_unhook(hookHandle);
        hookHandle = nullptr;
        status = "passthrough-missing-trampoline";
        return false;
    }
    status = hookHandle == nullptr ? "passthrough-engine-hook-failed" : "passthrough-installed-no-simulation";
#ifdef SENSOR_PROBE_EVENT_DUMP
    if (hookHandle != nullptr) {
        __android_log_print(ANDROID_LOG_INFO, "GlobalSensorProbe",
            "raw step-event dump enabled (max %d step events)", maxDumpedStepEvents);
    }
#endif
    return hookHandle != nullptr;
}

#ifdef SENSOR_PROBE_EVENT_DUMP
int dumpedStepEventCount() { return dumpedStepEvents.load(std::memory_order_relaxed); }
#else
int dumpedStepEventCount() { return 0; }
#endif

bool passthroughInstalled() { return hookHandle != nullptr; }
const char* passthroughStatus() { return status; }
const ForwardingStats& forwardingStats() { return stats; }

}  // namespace sensor_probe
