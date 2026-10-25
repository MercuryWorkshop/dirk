#pragma once

// dirk: the device is the gallium screen and the one gallium context made
// on it. Every DXVK object it makes is a gallium object, or a key the
// context turns into one when it binds it; nothing here is a Vulkan object.

#include <optional>

#include "dxvk_adapter.h"
#include "dxvk_buffer.h"
#include "dxvk_constant_state.h"
#include "dxvk_context.h"
#include "dxvk_extensions.h"
#include "dxvk_fence.h"
#include "dxvk_framebuffer.h"
#include "dxvk_gpu_event.h"
#include "dxvk_gpu_query.h"
#include "dxvk_image.h"
#include "dxvk_instance.h"
#include "dxvk_latency.h"
#include "dxvk_memory.h"
#include "dxvk_options.h"
#include "dxvk_sampler.h"
#include "dxvk_shader.h"
#include "dxvk_sparse.h"
#include "dxvk_stats.h"

struct pipe_context;
struct pipe_screen;
struct cso_context;

namespace dxvk {

  class DxvkInstance;
  class Presenter;

  struct DxvkDevicePerfHints {
    VkBool32 preferFbDepthStencilCopy   : 1;
    VkBool32 renderPassClearFormatBug   : 1;
    VkBool32 renderPassResolveFormatBug : 1;
    VkBool32 preferRenderPassOps        : 1;
    VkBool32 preferPrimaryCmdBufs       : 1;
  };

  struct DxvkDeviceQueue {
    VkQueue   queueHandle = VK_NULL_HANDLE;
    uint32_t  queueFamily = 0;
    uint32_t  queueIndex  = 0;
  };

  struct DxvkDeviceQueueSet {
    DxvkDeviceQueue graphics;
    DxvkDeviceQueue transfer;
    DxvkDeviceQueue sparse;
  };

  struct DxvkFramebufferSize {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
  };

  struct DxvkSamplerStats {
    uint32_t totalCount = 0u;
    uint32_t liveCount  = 0u;
  };

  /**
   * \brief Command list
   *
   * The batch a context records into. gallium keeps the batch itself, so
   * this only names the recording for the front end.
   */
  class DxvkCommandList : public RcObject { };

  class DxvkDevice : public RcObject {
    friend class DxvkContext;
  public:

    DxvkDevice(
      const Rc<DxvkInstance>&         instance,
      const Rc<DxvkAdapter>&          adapter,
      const DxvkDeviceFeatures&       features);

    ~DxvkDevice();

    Rc<vk::DeviceFn> vkd() const {
      return m_vkd;
    }

    Rc<vk::InstanceFn> vki() const;

    VkDevice handle() const {
      return VK_NULL_HANDLE;
    }

    DxvkDebugFlags debugFlags() const {
      return m_debugFlags;
    }

    const DxvkOptions& config() const {
      return m_options;
    }

    const DxvkDeviceQueueSet& queues() const {
      return m_queues;
    }

    bool hasDedicatedTransferQueue() const {
      return false;
    }

    DxvkSharingModeInfo getSharingMode() const {
      return DxvkSharingModeInfo();
    }

    Rc<DxvkInstance> instance() const {
      return m_instance;
    }

    Rc<DxvkAdapter> adapter() const {
      return m_adapter;
    }

    const DxvkDeviceFeatures& features() const {
      return m_features;
    }

    const DxvkDeviceInfo& properties() const {
      return m_properties;
    }

    DxvkFormatFeatures getFormatFeatures(VkFormat format) const {
      return m_adapter->getFormatFeatures(format);
    }

    std::optional<DxvkFormatLimits> getFormatLimits(
      const DxvkFormatQuery&          query) const {
      return m_adapter->getFormatLimits(query);
    }

    VkResult getDeviceStatus() const {
      return m_status;
    }

    VkSubresourceLayout queryImageSubresourceLayout(
      const DxvkImageCreateInfo&        createInfo,
      const VkImageSubresource&         subresource);

    bool isUnifiedMemoryArchitecture() const {
      return false;
    }

    bool canUseGraphicsPipelineLibrary() const {
      return false;
    }

    bool canUsePipelineCacheControl() const {
      return false;
    }

    bool mustTrackPipelineLifetime() const {
      return false;
    }

    DxvkFramebufferSize getDefaultFramebufferSize() const;

    VkPipelineStageFlags getShaderPipelineStages() const;

    DxvkDevicePerfHints perfHints() const {
      return m_perfHints;
    }

    Rc<DxvkCommandList> createCommandList() {
      return new DxvkCommandList();
    }

    Rc<DxvkContext> createContext();

    Rc<DxvkEvent> createGpuEvent();

    Rc<DxvkQuery> createGpuQuery(
            VkQueryType           type,
            VkQueryControlFlags   flags,
            uint32_t              index);

    Rc<DxvkFence> createFence(
      const DxvkFenceCreateInfo& fenceInfo);

    Rc<DxvkBuffer> createBuffer(
      const DxvkBufferCreateInfo& createInfo,
            VkMemoryPropertyFlags memoryType);

    Rc<DxvkImage> createImage(
      const DxvkImageCreateInfo&  createInfo,
            VkMemoryPropertyFlags memoryType);

    Rc<DxvkSampler> createSampler(
      const DxvkSamplerKey&         createInfo);

    DxvkLocalAllocationCache createAllocationCache(
            VkBufferUsageFlags    bufferUsage,
            VkMemoryPropertyFlags propertyFlags) {
      return DxvkLocalAllocationCache();
    }

    Rc<DxvkSparsePageAllocator> createSparsePageAllocator() {
      return new DxvkSparsePageAllocator();
    }

    Rc<DxvkBuffer> importBuffer(
      const DxvkBufferCreateInfo& createInfo,
      const DxvkBufferImportInfo& importInfo,
            VkMemoryPropertyFlags memoryType) {
      return nullptr;
    }

    Rc<DxvkImage> importImage(
      const DxvkImageCreateInfo&  createInfo,
            VkImage               image,
            VkMemoryPropertyFlags memoryType) {
      return nullptr;
    }

    DxvkStatCounters getStatCounters();

    DxvkMemoryStats getMemoryStats(uint32_t heap) {
      return DxvkMemoryStats();
    }

    DxvkSamplerStats getSamplerStats() {
      return DxvkSamplerStats();
    }

    uint32_t getCurrentFrameId() const {
      return m_frameId;
    }

    void notifyMemoryStats(
            uint32_t            heap,
            int64_t             allocated,
            int64_t             used) {
      m_adapter->notifyMemoryStats(heap, allocated, used);
    }

    void registerShader(
      const Rc<DxvkShader>&         shader) { }

    void requestCompileShader(
      const Rc<DxvkShader>&         shader) { }

    Rc<DxvkLatencyTracker> createLatencyTracker(
      const Rc<Presenter>&            presenter) {
      return nullptr;
    }

    void lockSubmission() { }

    void unlockSubmission() { }

    void addStatCtr(DxvkStatCounter counter, uint64_t value) {
      m_statCounters.addCtr(counter, value);
    }

    VkResult waitForSubmission(DxvkSubmitStatus* status) {
      return status ? status->result.load() : VK_SUCCESS;
    }

    void waitForFence(sync::Fence& fence, uint64_t value) {
      fence.wait(value);
    }

    /**
     * \brief Waits until the host may access a resource
     *
     * Reads the GPU's writes back into the host copy (\c isInUse).
     */
    void waitForResource(const DxvkPagedResource& resource, DxvkAccess access);

    void waitForIdle();

    /** \brief The gallium screen */
    pipe_screen* screen() const {
      return m_screen;
    }

    /** \brief The gallium context, for the device's thread only */
    pipe_context* pipe() const {
      return m_pipe;
    }

    cso_context* cso() const {
      return m_cso;
    }

    /** \brief Counts a presented frame */
    void endFrame() {
      m_frameId += 1u;
    }

  private:

    DxvkOptions                 m_options;
    Rc<DxvkInstance>            m_instance;
    Rc<DxvkAdapter>             m_adapter;
    Rc<vk::DeviceFn>            m_vkd;
    DxvkDebugFlags              m_debugFlags;
    DxvkDeviceQueueSet          m_queues;
    DxvkDeviceFeatures          m_features;
    DxvkDeviceInfo              m_properties;
    DxvkDevicePerfHints         m_perfHints = { };
    DxvkStatCounters            m_statCounters;
    VkResult                    m_status  = VK_SUCCESS;
    uint32_t                    m_frameId = 0u;

    pipe_screen*                m_screen  = nullptr;
    pipe_context*               m_pipe    = nullptr;
    cso_context*                m_cso     = nullptr;

    DxvkContext*                m_context = nullptr;

  };

}
