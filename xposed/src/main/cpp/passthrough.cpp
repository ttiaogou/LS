#include "passthrough.h"

#include <cerrno>

namespace sensor_probe {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

std::int32_t forwardSendEvents(SendEvents original, ForwardingStats& stats, void* connection,
    const void* events, std::size_t count, void* scratch, const void* flushMap) noexcept {
    if (original == nullptr) return -ENOSYS;
    const auto result = original(connection, events, count, scratch, flushMap);
    const int savedErrno = errno;
    stats.batches.fetch_add(1, std::memory_order_relaxed);
    stats.events.fetch_add(count, std::memory_order_relaxed);
    if (result < 0) stats.failures.fetch_add(1, std::memory_order_relaxed);
    errno = savedErrno;
    return result;
}

}  // namespace sensor_probe
