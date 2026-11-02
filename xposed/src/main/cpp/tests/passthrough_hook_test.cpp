#include "passthrough_hook.h"
#include <shadowhook.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
const char* scenario;
int initCalls = 0, hookCalls = 0, unhookCalls = 0, originalCalls = 0;
void* capturedProxy = nullptr;
int target = 0, handle = 0;

void require(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "%s\n", reason); std::exit(1); }
}
std::int32_t original(void* receiver, const void* events, std::size_t count, void* scratch, const void* flush) {
    require(receiver == &target && events == &target && count == 5 && scratch == &target && flush == &target,
        "proxy preserves every ABI argument");
    ++originalCalls;
    errno = EAGAIN;
    return -7;
}
}

int shadowhook_init(shadowhook_mode_t mode, bool debuggable) {
    require(mode == SHADOWHOOK_MODE_UNIQUE && !debuggable, "unique non-debug engine");
    ++initCalls;
    return std::strcmp(scenario, "init-failure") == 0 ? -1 : 0;
}
void* shadowhook_hook_func_addr_2(void* address, void* proxy, void** out, std::uint32_t flags, ...) {
    require(address == &target && flags == SHADOWHOOK_HOOK_WITH_UNIQUE_MODE, "target and hook mode");
    ++hookCalls;
    if (std::strcmp(scenario, "hook-failure") == 0) return nullptr;
    capturedProxy = proxy;
    if (std::strcmp(scenario, "missing-trampoline") != 0) *out = reinterpret_cast<void*>(original);
    return &handle;
}
int shadowhook_unhook(void* value) {
    require(value == &handle, "unhook exact owned handle");
    ++unhookCalls;
    return 0;
}

int main(int argc, char** argv) {
    require(argc == 2, "scenario argument");
    scenario = argv[1];
    require(!sensor_probe::installPassthrough(nullptr) && initCalls == 0, "null target does not initialize engine");
    const bool success = std::strcmp(scenario, "success") == 0;
    require(sensor_probe::installPassthrough(&target) == success, "install result");
    require(sensor_probe::passthroughInstalled() == success && initCalls == 1, "readiness follows actual hook");
    if (std::strcmp(scenario, "init-failure") == 0) require(hookCalls == 0, "init failure never hooks");
    else require(hookCalls == 1, "one hook attempt");
    require(unhookCalls == (std::strcmp(scenario, "missing-trampoline") == 0 ? 1 : 0), "rollback contract");
    if (success) {
        const auto proxy = reinterpret_cast<sensor_probe::SendEvents>(capturedProxy);
        require(proxy(&target, &target, 5, &target, &target) == -7 && errno == EAGAIN && originalCalls == 1,
            "real proxy forwarding contract");
        const auto& stats = sensor_probe::forwardingStats();
        require(stats.batches == 1 && stats.events == 5 && stats.failures == 1, "proxy counters");
        require(sensor_probe::installPassthrough(&target) && initCalls == 1 && hookCalls == 1, "idempotent install");
    }
    std::printf("hook scenario passed: %s\n", scenario);
}
