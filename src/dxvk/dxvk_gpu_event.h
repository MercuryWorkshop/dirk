#pragma once

// dirk: an event is signaled when the batch that signaled it is flushed to
// the WebGPU queue. The queue keeps order, so the host has nothing to wait
// for after that: what D3D asks of an event (a query that says whether the
// GPU got this far) is answered by the flush.

#include <atomic>

#include "dxvk_include.h"

namespace dxvk {

  class DxvkDevice;

  enum class DxvkGpuEventStatus : uint32_t {
    Invalid   = 0,
    Pending   = 1,
    Signaled  = 2,
  };

  class DxvkEvent {
    friend class DxvkContext;
  public:

    DxvkEvent(const Rc<DxvkDevice>& device)
    : m_device(device) { }

    force_inline void incRef() {
      m_refCount.fetch_add(1u, std::memory_order_acquire);
    }

    force_inline void decRef() {
      if (m_refCount.fetch_sub(1u, std::memory_order_release) == 1u)
        delete this;
    }

    DxvkGpuEventStatus test() {
      return DxvkGpuEventStatus(m_status.load(std::memory_order_acquire));
    }

  private:

    std::atomic<uint32_t> m_refCount = { 0u };
    std::atomic<uint32_t> m_status   = { uint32_t(DxvkGpuEventStatus::Invalid) };
    Rc<DxvkDevice>        m_device;

    void setStatus(DxvkGpuEventStatus status) {
      m_status.store(uint32_t(status), std::memory_order_release);
    }

  };

}
