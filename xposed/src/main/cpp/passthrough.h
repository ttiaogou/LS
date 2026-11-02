#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace sensor_probe {

// AOSP sendEvents ABI: implicit receiver followed by four explicit arguments.
// Opaque pointers are intentional: M3 never reads event or connection memory.
using SendEvents = std::int32_t (*)(void*, const void*, std::size_t, void*, const void*);

struct ForwardingStats {
    std::atomic<std::uint64_t> batches{0};
    std::atomic<std::uint64_t> events{0};
    std::atomic<std::uint64_t> failures{0};
};

std::int32_t forwardSendEvents(SendEvents original, ForwardingStats& stats, void* connection,
    const void* events, std::size_t count, void* scratch, const void* flushMap) noexcept;

}  // namespace sensor_probe
