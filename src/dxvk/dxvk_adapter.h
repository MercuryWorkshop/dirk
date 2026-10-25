#pragma once

// dirk: the one adapter is the gallium screen of the page's WebGPU device.
// What DXVK reads off a Vulkan physical device -- properties, features,
// format support, memory heaps -- is filled in from the screen's caps
// (dirk_adapter.cpp), so the front end picks its D3D feature level and
// format support as it would on a Vulkan driver of that capability.

#include <functional>
#include <optional>

#include "dxvk_device_info.h"
#include "dxvk_extensions.h"
#include "dxvk_include.h"
#include "dxvk_format.h"

struct pipe_screen;

namespace dxvk {

  class DxvkDevice;
  class DxvkInstance;

  using DxvkQueueCallback = std::function<void (bool)>;

  enum class DxvkGpuVendor : uint16_t {
    Amd    = 0x1002,
    Nvidia = 0x10de,
    Intel  = 0x8086,
  };

  struct DxvkAdapterMemoryHeapInfo {
    VkMemoryHeapFlags heapFlags;
    VkDeviceSize heapSize;
    VkDeviceSize memoryBudget;
    VkDeviceSize memoryAllocated;
  };

  struct DxvkAdapterMemoryInfo {
    uint32_t                  heapCount;
    DxvkAdapterMemoryHeapInfo heaps[VK_MAX_MEMORY_HEAPS];
  };

  struct DxvkAdapterMemoryStats {
    std::atomic<uint64_t> allocated = { 0u };
    std::atomic<uint64_t> used = { 0u };
  };

  struct DxvkDeviceImportInfo {
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamily;
    uint32_t extensionCount;
    const char** extensionNames;
    const VkPhysicalDeviceFeatures2* features;
    DxvkQueueCallback queueCallback;
  };

  class DxvkAdapter : public RcObject {

  public:

    DxvkAdapter(
      const Rc<vk::InstanceFn>& vki,
            pipe_screen*        screen);

    ~DxvkAdapter();

    Rc<vk::InstanceFn> vki() const {
      return m_vki;
    }

    VkPhysicalDevice handle() const {
      return VkPhysicalDevice(m_screen);
    }

    pipe_screen* screen() const {
      return m_screen;
    }

    const VkPhysicalDeviceProperties& deviceProperties() const {
      return m_deviceInfo.core.properties;
    }

    const DxvkDeviceInfo& devicePropertiesExt() const {
      return m_deviceInfo;
    }

    const DxvkDeviceFeatures& features() const {
      return m_deviceFeatures;
    }

    DxvkAdapterMemoryInfo getMemoryHeapInfo() const;

    VkPhysicalDeviceMemoryProperties memoryProperties() const;

    DxvkFormatFeatures getFormatFeatures(
            VkFormat                  format) const;

    std::optional<DxvkFormatLimits> getFormatLimits(
      const DxvkFormatQuery&          query) const;

    bool checkFeatureSupport(
      const DxvkDeviceFeatures& required) const;

    void enableExtensions(
      const DxvkNameSet&        extensions) { }

    Rc<DxvkDevice> createDevice(
      const Rc<DxvkInstance>&   instance,
            DxvkDeviceFeatures  enabledFeatures);

    Rc<DxvkDevice> importDevice(
      const Rc<DxvkInstance>&   instance,
      const DxvkDeviceImportInfo& args) {
      return nullptr;
    }

    void notifyMemoryStats(
            uint32_t            heap,
            int64_t             allocated,
            int64_t             used);

    bool matchesDriver(
            VkDriverIdKHR       driver,
            Version             minVer,
            Version             maxVer) const {
      return false;
    }

    bool matchesDriver(
            VkDriverIdKHR       driver) const {
      return false;
    }

    void logAdapterInfo() const;

    bool isUnifiedMemoryArchitecture() const {
      return false;
    }

    Rc<DxvkAdapter> linkedIGPUAdapter() const {
      return nullptr;
    }

    bool isLinkedToDGPU() const {
      return false;
    }

  private:

    Rc<vk::InstanceFn>  m_vki;
    pipe_screen*        m_screen = nullptr;

    DxvkDeviceInfo      m_deviceInfo;
    DxvkDeviceFeatures  m_deviceFeatures;

    std::array<DxvkAdapterMemoryStats, VK_MAX_MEMORY_HEAPS> m_memoryStats = { };

    void queryDeviceInfo();
    void queryDeviceFeatures();

  };

}
