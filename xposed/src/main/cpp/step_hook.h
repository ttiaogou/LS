#pragma once

#include "step_delivery.h"
#include "target_locator.h"
#include <cstdint>
#include <span>

namespace sensor_probe {
bool installStepDelivery(const TargetReport&);
const char* stepDeliveryStatus();
bool stepDeliveryInstalled();
bool configureStepCatalog(std::span<const std::int32_t> pairs);
bool configureStepSnapshot(const StepSnapshot&, std::span<const std::int32_t> uids);
void disableStepDelivery();
bool trySubmitSteps(SendEvents, void*, const void*, std::size_t, void*, const void*, std::int32_t&);
std::uint64_t submittedStepEvents();
std::uint64_t suppressedStepEvents();
// 析构器 ABI 的运行期证据：D2 调用次数，以及其中 VTT 参数非空的次数。
std::uint64_t destructorInvocations();
std::uint64_t destructorVttObservations();
// 事件构成：counter 事件数、detector 脉冲数、最后一次发出的计数。
std::uint64_t stepCounterEvents();
std::uint64_t stepDetectorPulses();
std::uint64_t stepLastCounterValue();
// 接管对齐证据：偏移、采纳时的物理值、是否已采纳。
std::int64_t stepCounterShift();
std::int64_t stepPhysicalBase();
bool stepCounterBaseAdopted();
}  // namespace sensor_probe
