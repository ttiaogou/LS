#pragma once

#include "step_stream.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace sensor_probe {
// Portable representation of the ARM64 ASensorEvent wire layout. Not a delivery API.
struct alignas(8) StepWireEvent {
    std::int32_t version;
    std::int32_t sensor;
    std::int32_t type;
    std::int32_t reserved0;
    std::int64_t timestamp;
    std::array<std::byte, 64> payload;
    std::uint32_t flags;
    std::array<std::int32_t, 3> reserved1;
};

// Encodes planned step events for both the wake-up and the non-wake step handles: the
// handle alone decides which variant a consumer receives, and the original sendEvents
// marks wake-up events itself (ACK bit 0x80000000 in the flags word, wake-lock
// accounting, battery attribution), so this encoder never fabricates wake-up state.
// No flush metadata, wake-lock/ACK flags or object reads.
// Invalid input leaves the destination unchanged.
bool encodeNonWakeStep(const PlannedStep&, StepWireEvent&) noexcept;

// 诊断用只读取样：把一条原始记录按同一布局取出，不改写、不解释唤醒状态。
// 用于在真机上核对本 ROM 的真实事件布局与上面的断言是否一致。
bool readStepEvent(const void* bytes, std::size_t available, StepWireEvent& destination) noexcept;
// 把取样结果写成一行动态描述（超长截断，始终以 '\0' 结尾）；返回写入字符数（不含结尾）。
int describeStepEvent(const StepWireEvent&, char* out, std::size_t capacity) noexcept;
}  // namespace sensor_probe
