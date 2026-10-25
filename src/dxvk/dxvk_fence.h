#pragma once

// dirk: a timeline fence is a host counter. The context sets it when the
// batch that signals it is flushed (the WebGPU queue keeps order from then
// on); a wait for a value the GPU has not reached runs its callback then.

#include <atomic>
#include <functional>
#include <utility>
#include <vector>

#include "dxvk_include.h"

#include "../util/thread.h"

namespace dxvk {

  class DxvkDevice;

  struct DxvkFenceCreateInfo {
    uint64_t        initialValue;
    VkExternalSemaphoreHandleTypeFlagBits sharedType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
    union {
      HANDLE          sharedHandle = INVALID_HANDLE_VALUE;
    };
  };

  class DxvkFence : public RcObject {

  public:

    DxvkFence(
            DxvkDevice*           device,
      const DxvkFenceCreateInfo&  info)
    : m_info(info), m_value(info.initialValue) { }

    uint64_t getValue() {
      return m_value.load(std::memory_order_acquire);
    }

    /** \brief Runs \c event once the fence reaches \c value */
    void enqueueWait(uint64_t value, std::function<void ()>&& event) {
      std::unique_lock lock(m_mutex);
      if (value <= m_value.load(std::memory_order_acquire)) {
        lock.unlock();
        event();
        return;
      }
      m_waits.emplace_back(value, std::move(event));
    }

    void wait(uint64_t value) {
      std::unique_lock lock(m_mutex);
      m_cond.wait(lock, [this, value] {
        return m_value.load(std::memory_order_acquire) >= value;
      });
    }

    /** \brief Sets the value, on the device's thread after a flush */
    void signal(uint64_t value) {
      std::vector<std::function<void ()>> ready;
      { std::unique_lock lock(m_mutex);
        m_value.store(value, std::memory_order_release);
        for (size_t i = 0; i < m_waits.size(); ) {
          if (m_waits[i].first <= value) {
            ready.push_back(std::move(m_waits[i].second));
            m_waits[i] = std::move(m_waits.back());
            m_waits.pop_back();
          } else {
            i++;
          }
        }
      }
      m_cond.notify_all();
      for (auto& e : ready)
        e();
    }

    HANDLE sharedHandle() const {
      return INVALID_HANDLE_VALUE;
    }

  private:

    DxvkFenceCreateInfo     m_info;
    std::atomic<uint64_t>   m_value;

    dxvk::mutex             m_mutex;
    dxvk::condition_variable m_cond;
    std::vector<std::pair<uint64_t, std::function<void ()>>> m_waits;

  };

}
