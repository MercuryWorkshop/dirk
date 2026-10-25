#pragma once

// dirk: no Reflex. The device asks a tracker whether it is this class; none is.

#include "dxvk_latency.h"

namespace dxvk {

  struct DxvkReflexFrameReport {
    VkLatencyTimingsFrameReportNV report;
    uint64_t gpuActiveTimeUs;
  };

  class DxvkReflexLatencyTrackerNv : public DxvkLatencyTracker {
  public:
    void setLatencySleepMode(bool, bool, uint64_t) { }
    void setLatencyMarker(uint64_t, VkLatencyMarkerNV) { }
    void latencySleep() { }
    uint32_t getFrameReports(uint32_t, DxvkReflexFrameReport*) { return 0u; }
    uint64_t frameIdFromAppFrameId(uint64_t) { return 0u; }
  };

}
