#pragma once

#include "passthrough.h"

namespace sensor_probe {

bool installPassthrough(void* target);
bool passthroughInstalled();
const char* passthroughStatus();
const ForwardingStats& forwardingStats();
// 已转储的真实 step 事件条数；SENSOR_PROBE_EVENT_DUMP 未开启时恒为 0。
int dumpedStepEventCount();

}  // namespace sensor_probe
