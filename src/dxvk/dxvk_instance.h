#pragma once

// dirk: the instance has the one adapter of the gallium screen the host
// registered (dirk_register_screen). It loads no Vulkan library: its
// function tables resolve every name to null, for the few front-end paths
// that look for an optional Vulkan extension.

#include "../util/config/config.h"

#include "dxvk_adapter.h"
#include "dxvk_options.h"

struct pipe_screen;

namespace dxvk {

  struct DxvkInstanceImportInfo {
    PFN_vkGetInstanceProcAddr loaderProc;
    VkInstance instance;
    uint32_t extensionCount;
    const char** extensionNames;
  };

  enum class DxvkDebugFlag : uint32_t {
    Validation        = 0,
    Capture           = 1,
    Markers           = 2,
  };

  using DxvkDebugFlags = Flags<DxvkDebugFlag>;

  enum class DxvkInstanceFlag : uint32_t {
    ClientApiIsD3D9,
  };

  using DxvkInstanceFlags = Flags<DxvkInstanceFlag>;

  class DxvkInstance : public RcObject {

  public:

    explicit DxvkInstance(DxvkInstanceFlags flags);

    /** \brief Importing a Vulkan instance: there is none, so a plain one */
    explicit DxvkInstance(
      const DxvkInstanceImportInfo& args,
            DxvkInstanceFlags       flags)
    : DxvkInstance(flags) { }

    ~DxvkInstance();

    Rc<vk::InstanceFn> vki() const {
      return m_vki;
    }

    VkInstance handle() {
      return VK_NULL_HANDLE;
    }

    size_t adapterCount() {
      return m_adapters.size();
    }

    Rc<DxvkAdapter> enumAdapters(
            uint32_t      index) const {
      return index < m_adapters.size() ? m_adapters[index] : nullptr;
    }

    Rc<DxvkAdapter> findAdapterByLuid(
      const void*         luid) const;

    Rc<DxvkAdapter> findAdapterByDeviceId(
            uint16_t      vendorId,
            uint16_t      deviceId) const;

    const Config& config() const {
      return m_config;
    }

    const DxvkOptions& options() const {
      return m_options;
    }

    const DxvkInstanceExtensions& extensions() const {
      return m_extensions;
    }

    DxvkDebugFlags debugFlags() const {
      return m_debugFlags;
    }

    /** \brief A function table that resolves every name to null */
    static PFN_vkGetInstanceProcAddr nullLoader();

  private:

    Config                  m_config;
    DxvkOptions             m_options;

    Rc<vk::LibraryFn>       m_vkl;
    Rc<vk::InstanceFn>      m_vki;
    DxvkInstanceExtensions  m_extensions;
    DxvkDebugFlags          m_debugFlags = 0u;

    std::vector<Rc<DxvkAdapter>> m_adapters;

  };

}

/** \brief Sets the screen the next instance's adapter is made of */
extern "C" void dirk_register_screen(struct pipe_screen* screen);
