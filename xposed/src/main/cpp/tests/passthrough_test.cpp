#include "passthrough.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
struct Call { void* receiver; const void* events; std::size_t count; void* scratch; const void* flush; };
Call observed{};
int calls = 0;
std::int32_t nextResult = 0;
std::int32_t original(void* receiver, const void* events, std::size_t count, void* scratch, const void* flush) {
    observed = {receiver, events, count, scratch, flush};
    ++calls;
    errno = EAGAIN;
    return nextResult;
}
void require(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "%s\n", reason); std::exit(1); }
}
}

int main() {
    sensor_probe::ForwardingStats stats;
    int connection = 1, events = 2, scratch = 3, flush = 4;
    auto result = sensor_probe::forwardSendEvents(original, stats, &connection, &events, 7, &scratch, &flush);
    require(result == 0 && calls == 1, "original called exactly once");
    require(observed.receiver == &connection && observed.events == &events && observed.count == 7 &&
        observed.scratch == &scratch && observed.flush == &flush, "all five ABI arguments unchanged");
    require(connection == 1 && events == 2 && scratch == 3 && flush == 4, "caller storage unchanged");
    require(errno == EAGAIN && stats.batches == 1 && stats.events == 7 && stats.failures == 0, "errno and counters");
    nextResult = -11;
    result = sensor_probe::forwardSendEvents(original, stats, nullptr, nullptr, 0, nullptr, nullptr);
    require(result == -11 && calls == 2 && observed.count == 0 && stats.failures == 1, "negative return and empty batch");
    nextResult = 123;
    result = sensor_probe::forwardSendEvents(original, stats, &connection, &events,
        std::numeric_limits<std::size_t>::max(), &scratch, &flush);
    require(result == 123 && calls == 3 && observed.count == std::numeric_limits<std::size_t>::max(),
        "count is forwarded without buffer traversal");
    require(sensor_probe::forwardSendEvents(nullptr, stats, nullptr, nullptr, 0, nullptr, nullptr) == -ENOSYS && calls == 3,
        "missing trampoline guard");
    std::puts("7 passthrough contract checks passed");
}
