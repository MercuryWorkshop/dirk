#pragma once

// dirk: the context records DXVK's commands into the device's gallium
// context. State is kept as DXVK hands it over and turned into gallium
// state at the next draw or dispatch (dirk_context.cpp); host writes to
// mapped memory are uploaded before the first command that may read them.

#include <vector>

#include "dxvk_context_state.h"
#include "dxvk_fence.h"
#include "dxvk_gpu_event.h"
#include "dxvk_gpu_query.h"
#include "dxvk_latency.h"
#include "dxvk_sparse.h"
#include "dxvk_stats.h"
#include "dxvk_util.h"

#include "../util/sync/sync_signal.h"

struct pipe_context;
struct pipe_resource;
struct pipe_sampler_view;
struct pipe_surface;
struct cso_context;

namespace dxvk {

  class DxvkCommandList;
  class DxvkDevice;
  struct DirkPipeShader;

  class DxvkContext : public RcObject {

  public:

    DxvkContext(const Rc<DxvkDevice>& device);
    ~DxvkContext();

    void beginRecording(
      const Rc<DxvkCommandList>& cmdList);

    Rc<DxvkCommandList> endRecording(
      const VkDebugUtilsLabelEXT*       reason);

    void endFrame();

    void beginLatencyTracking(
      const Rc<DxvkLatencyTracker>&     tracker,
            uint64_t                    frameId) { }

    void endLatencyTracking(
      const Rc<DxvkLatencyTracker>&     tracker) { }

    void flushCommandList(
      const VkDebugUtilsLabelEXT*       reason,
            DxvkSubmitStatus*           status);

    void beginQuery(
      const Rc<DxvkQuery>&      query);

    void endQuery(
      const Rc<DxvkQuery>&      query);

    void bindRenderTargets(
            DxvkRenderTargets&&   targets,
            VkImageAspectFlags    feedbackLoop) {
      m_rt = std::move(targets);
      m_dirty.set(DirkDirty::Framebuffer);
    }

    void bindDrawBuffers(
            DxvkBufferSlice&&     argBuffer,
            DxvkBufferSlice&&     cntBuffer) {
      m_argBuffer = std::move(argBuffer);
      m_cntBuffer = std::move(cntBuffer);
    }

    void bindIndexBuffer(
            DxvkBufferSlice&&     buffer,
            VkIndexType           indexType) {
      m_indexBuffer = std::move(buffer);
      m_indexType   = indexType;
      m_dirty.set(DirkDirty::IndexBuffer);
    }

    void bindIndexBufferRange(
            VkDeviceSize          offset,
            VkDeviceSize          length,
            VkIndexType           indexType) {
      m_indexBuffer.setRange(offset, length);
      m_indexType = indexType;
      m_dirty.set(DirkDirty::IndexBuffer);
    }

    void bindUniformBuffer(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            DxvkBufferSlice&&     buffer) {
      m_rc[slot].bufferSlice = std::move(buffer);
      dirtyResources(stages);
    }

    void bindUniformBufferRange(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            VkDeviceSize          offset,
            VkDeviceSize          length) {
      m_rc[slot].bufferSlice.setRange(offset, length);
      dirtyResources(stages);
    }

    void bindResourceImageView(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            Rc<DxvkImageView>&&   view) {
      if (m_rc[slot].bufferView != nullptr) {
        m_rc[slot].bufferSlice = DxvkBufferSlice();
        m_rc[slot].bufferView  = nullptr;
      }
      m_rc[slot].imageView = std::move(view);
      dirtyResources(stages);
    }

    void bindResourceBufferView(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            Rc<DxvkBufferView>&&  view) {
      if (m_rc[slot].imageView != nullptr)
        m_rc[slot].imageView = nullptr;
      if (view != nullptr) {
        m_rc[slot].bufferSlice = DxvkBufferSlice(view);
        m_rc[slot].bufferView = std::move(view);
      } else {
        m_rc[slot].bufferSlice = DxvkBufferSlice();
        m_rc[slot].bufferView = nullptr;
      }
      dirtyResources(stages);
    }

    void bindResourceSampler(
            VkShaderStageFlags    stages,
            uint32_t              slot,
            Rc<DxvkSampler>&&     sampler) {
      m_rc[slot].sampler = std::move(sampler);
      dirtyResources(stages);
    }

    template<VkShaderStageFlagBits Stage>
    void bindShader(
            Rc<DxvkShader>&&      shader) {
      switch (Stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:                  m_vs  = std::move(shader); break;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    m_tcs = std::move(shader); break;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: m_tes = std::move(shader); break;
        case VK_SHADER_STAGE_GEOMETRY_BIT:                m_gs  = std::move(shader); break;
        case VK_SHADER_STAGE_FRAGMENT_BIT:                m_fs  = std::move(shader); break;
        case VK_SHADER_STAGE_COMPUTE_BIT:                 m_cs  = std::move(shader); break;
        default: return;
      }

      if (Stage == VK_SHADER_STAGE_COMPUTE_BIT) {
        m_dirty.set(DirkDirty::ComputeShader, DirkDirty::ComputeResources);
      } else {
        m_dirty.set(DirkDirty::Shaders, DirkDirty::Resources,
          DirkDirty::VertexInput, DirkDirty::Blend);
      }
    }

    void bindVertexBuffer(
            uint32_t              binding,
            DxvkBufferSlice&&     buffer,
            uint32_t              stride) {
      m_vertexBuffers[binding] = std::move(buffer);
      m_vertexStrides[binding] = stride;
      m_dirty.set(DirkDirty::VertexBuffers);
    }

    void bindVertexBufferRange(
            uint32_t              binding,
            VkDeviceSize          offset,
            VkDeviceSize          length,
            uint32_t              stride) {
      m_vertexBuffers[binding].setRange(offset, length);
      m_vertexStrides[binding] = stride;
      m_dirty.set(DirkDirty::VertexBuffers);
    }

    void bindXfbBuffer(
            uint32_t              binding,
            DxvkBufferSlice&&     buffer,
            DxvkBufferSlice&&     counter) {
      m_xfbBuffers [binding] = std::move(buffer);
      m_xfbCounters[binding] = std::move(counter);
    }

    void blitImageView(
      const Rc<DxvkImageView>&    dstView,
      const VkOffset3D*           dstOffsets,
      const Rc<DxvkImageView>&    srcView,
      const VkOffset3D*           srcOffsets,
            VkFilter              filter);
    void changeImageLayout(
      const Rc<DxvkImage>&        image,
            VkImageLayout         layout);
    void clearBuffer(
      const Rc<DxvkBuffer>&       buffer,
            VkDeviceSize          offset,
            VkDeviceSize          length,
            uint32_t              value);
    void clearBufferView(
      const Rc<DxvkBufferView>&   bufferView,
            VkDeviceSize          offset,
            VkDeviceSize          length,
            VkClearColorValue     value);
    void clearRenderTarget(
      const Rc<DxvkImageView>&    imageView,
            VkImageAspectFlags    clearAspects,
            VkClearValue          clearValue,
            VkImageAspectFlags    discardAspects);
    void clearImageView(
      const Rc<DxvkImageView>&    imageView,
            VkOffset3D            offset,
            VkExtent3D            extent,
            VkImageAspectFlags    aspect,
            VkClearValue          value);
    void copyBuffer(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstOffset,
      const Rc<DxvkBuffer>&       srcBuffer,
            VkDeviceSize          srcOffset,
            VkDeviceSize          numBytes);
    void copyBufferRegion(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstOffset,
            VkDeviceSize          srcOffset,
            VkDeviceSize          numBytes);
    void copyBufferToImage(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
            VkExtent3D            dstExtent,
      const Rc<DxvkBuffer>&       srcBuffer,
            VkDeviceSize          srcOffset,
            VkDeviceSize          rowAlignment,
            VkDeviceSize          sliceAlignment,
            VkFormat              srcFormat);
    void copyImage(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
      const Rc<DxvkImage>&        srcImage,
            VkImageSubresourceLayers srcSubresource,
            VkOffset3D            srcOffset,
            VkExtent3D            extent);
    void copyImageRegion(
      const Rc<DxvkImage>&        dstImage,
            VkImageSubresourceLayers dstSubresource,
            VkOffset3D            dstOffset,
            VkOffset3D            srcOffset,
            VkExtent3D            extent);
    void copyImageToBuffer(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstOffset,
            VkDeviceSize          rowAlignment,
            VkDeviceSize          sliceAlignment,
            VkFormat              dstFormat,
      const Rc<DxvkImage>&        srcImage,
            VkImageSubresourceLayers srcSubresource,
            VkOffset3D            srcOffset,
            VkExtent3D            srcExtent);
    void copyPackedBufferImage(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstBufferOffset,
            VkOffset3D            dstOffset,
            VkExtent3D            dstSize,
      const Rc<DxvkBuffer>&       srcBuffer,
            VkDeviceSize          srcBufferOffset,
            VkOffset3D            srcOffset,
            VkExtent3D            srcSize,
            VkExtent3D            extent,
            VkDeviceSize          elementSize);
    void copySparsePagesToBuffer(
      const Rc<DxvkBuffer>&       dstBuffer,
            VkDeviceSize          dstOffset,
      const Rc<DxvkPagedResource>& srcResource,
            uint32_t              pageCount,
      const uint32_t*             pages);
    void copySparsePagesFromBuffer(
      const Rc<DxvkPagedResource>& dstResource,
            uint32_t              pageCount,
      const uint32_t*             pages,
      const Rc<DxvkBuffer>&       srcBuffer,
            VkDeviceSize          srcOffset);
    void discardImage(
      const Rc<DxvkImage>&          image);
    void dispatch(
            uint32_t          x,
            uint32_t          y,
            uint32_t          z);
    void dispatchIndirect(
            VkDeviceSize      offset);
    void draw(
            uint32_t          count,
      const VkDrawIndirectCommand* draws);
    void drawIndirect(
            VkDeviceSize      offset,
            uint32_t          count,
            uint32_t          stride,
            bool              unroll);
    void drawIndirectCount(
            VkDeviceSize      offset,
            VkDeviceSize      countOffset,
            uint32_t          maxCount,
            uint32_t          stride);
    void drawIndexed(
            uint32_t          count,
      const VkDrawIndexedIndirectCommand* draws);
    void drawIndexedIndirect(
            VkDeviceSize      offset,
            uint32_t          count,
            uint32_t          stride,
            bool              unroll);
    void drawIndexedIndirectCount(
            VkDeviceSize      offset,
            VkDeviceSize      countOffset,
            uint32_t          maxCount,
            uint32_t          stride);
    void drawIndirectXfb(
            VkDeviceSize      counterOffset,
            uint32_t          counterDivisor,
            uint32_t          counterBias);
    void emitGraphicsBarrier(
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);
    void emitBufferBarrier(
      const Rc<DxvkBuffer>&           resource,
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);
    void emitImageBarrier(
      const Rc<DxvkImage>&            resource,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);
    void generateMipmaps(
      const Rc<DxvkImageView>&        imageView,
            VkFilter                  filter);
    void initBuffer(
      const Rc<DxvkBuffer>&           buffer);
    void initImage(
      const Rc<DxvkImage>&            image,
            VkImageLayout             initialLayout);
    void initSparseImage(
      const Rc<DxvkImage>&            image);
    void invalidateBuffer(
      const Rc<DxvkBuffer>&           buffer,
            Rc<DxvkResourceAllocation>&& slice);
    void ensureBufferAddress(
      const Rc<DxvkBuffer>&           buffer);
    void invalidateImage(
      const Rc<DxvkImage>&            image,
            Rc<DxvkResourceAllocation>&& slice);
    void invalidateImageWithUsage(
      const Rc<DxvkImage>&            image,
            Rc<DxvkResourceAllocation>&& slice,
      const DxvkImageUsageInfo&       usageInfo);
    bool ensureImageCompatibility(
      const Rc<DxvkImage>&            image,
      const DxvkImageUsageInfo&       usageInfo);
    void pushConstants(
            uint32_t                  offset,
            uint32_t                  size,
      const void*                     data) {
      std::memcpy(&m_pushConstants[offset], data, size);
      m_dirty.set(DirkDirty::PushConstants);
    }

    void resolveImage(
      const Rc<DxvkImage>&            dstImage,
      const Rc<DxvkImage>&            srcImage,
      const VkImageResolve&           region,
            VkFormat                  format,
            VkResolveModeFlagBits     mode,
            VkResolveModeFlagBits     stencilMode);
    void transformImage(
      const Rc<DxvkImage>&            dstImage,
      const VkImageSubresourceRange&  dstSubresources,
            VkImageLayout             srcLayout,
            VkImageLayout             dstLayout);
    void updateBuffer(
      const Rc<DxvkBuffer>&           buffer,
            VkDeviceSize              offset,
            VkDeviceSize              size,
      const void*                     data);
    void uploadBuffer(
      const Rc<DxvkBuffer>&           buffer,
      const Rc<DxvkBuffer>&           source,
            VkDeviceSize              sourceOffset);
    void uploadImage(
      const Rc<DxvkImage>&            image,
      const Rc<DxvkBuffer>&           source,
            VkDeviceSize              sourceOffset,
            VkDeviceSize              subresourceAlignment,
            VkFormat                  format);
    void setViewports(
            uint32_t            viewportCount,
      const DxvkViewport*       viewports);
    void setBlendConstants(
            DxvkBlendConstants  blendConstants);
    void setDepthBias(
            DxvkDepthBias       depthBias);
    void setDepthBiasRepresentation(
            DxvkDepthBiasRepresentation  depthBiasRepresentation);
    void setDepthBounds(
            DxvkDepthBounds     depthBounds);
    void setStencilReference(
            uint32_t            reference);
    void setInputAssemblyState(
      const DxvkInputAssemblyState& ia);
    void setInputLayout(
            uint32_t             attributeCount,
      const DxvkVertexInput*     attributes,
            uint32_t             bindingCount,
      const DxvkVertexInput*     bindings);
    void setRasterizerState(
      const DxvkRasterizerState& rs);
    void setMultisampleState(
      const DxvkMultisampleState& ms);
    void setDepthStencilState(
      const DxvkDepthStencilState& ds);
    void setLogicOpState(
      const DxvkLogicOpState&   lo);
    void setBlendMode(
            uint32_t            attachment,
      const DxvkBlendMode&      blendMode);
    void setSpecConstant(
            VkPipelineBindPoint pipeline,
            uint32_t            index,
            uint32_t            value) {
      if (pipeline == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        m_gpSpec[index] = value;
        m_dirty.set(DirkDirty::Shaders);
      } else {
        m_cpSpec[index] = value;
        m_dirty.set(DirkDirty::ComputeShader);
      }
    }

    void setBarrierControl(
            DxvkBarrierControlFlags control);
    void updatePageTable(
      const DxvkSparseBindInfo&   bindInfo,
            DxvkSparseBindFlags   flags);
    void launchCuKernelNVX(
      const VkCuLaunchInfoNVX& nvxLaunchInfo,
      const std::vector<std::pair<Rc<DxvkBuffer>, DxvkAccessFlags>>& buffers,
      const std::vector<std::pair<Rc<DxvkImage>, DxvkAccessFlags>>& images);
    void signalGpuEvent(
      const Rc<DxvkEvent>&      event);
    void writeTimestamp(
      const Rc<DxvkQuery>&      query);
    void signal(
      const Rc<sync::Signal>&   signal,
            uint64_t            value);
    void waitFence(const Rc<DxvkFence>& fence, uint64_t value);
    void signalFence(const Rc<DxvkFence>& fence, uint64_t value);
    void beginDebugLabel(const VkDebugUtilsLabelEXT& label);
    void endDebugLabel();
    void insertDebugLabel(const VkDebugUtilsLabelEXT& label);
    void addStatCtr(DxvkStatCounter counter, uint64_t value) {
      m_stats.addCtr(counter, value);
    }

    void setDebugName(const Rc<DxvkPagedResource>& resource, const char* name) {
      if (resource != nullptr)
        resource->setDebugName(name);
    }

    /** \brief Uploads a buffer's host writes before the next reading command */
    void queueHostWrites(DxvkBuffer* buffer);

    /** \brief Presents an image: copies it to the surface the host named */
    void presentImage(
      const Rc<DxvkImageView>&  view,
            void*               surface,
            uint32_t            syncInterval);

    /**
     * \brief Takes a range of a buffer's host copy as written by the host
     *
     * It is uploaded before the next command that reads the buffer.
     */
    void markHostWritten(const Rc<DxvkBuffer>& buffer, VkDeviceSize offset, VkDeviceSize length);

  private:

    Rc<DxvkDevice>            m_device;
    pipe_context*             m_pipe = nullptr;
    cso_context*              m_cso  = nullptr;

    DirkDirtyFlags            m_dirty;
    VkShaderStageFlags        m_dirtyStages = 0u;

    Rc<DxvkShader>            m_vs, m_tcs, m_tes, m_gs, m_fs, m_cs;
    std::array<uint32_t, MaxNumSpecConstants> m_gpSpec = { };
    std::array<uint32_t, MaxNumSpecConstants> m_cpSpec = { };

    DxvkInputAssemblyState    m_ia = { };
    uint32_t                  m_attrCount = 0u;
    uint32_t                  m_bindCount = 0u;
    std::array<DxvkVertexAttribute, MaxNumVertexAttributes> m_attrs = { };
    std::array<DxvkVertexBinding,   MaxNumVertexBindings>   m_binds = { };

    DxvkRasterizerState       m_rs = { };
    DxvkMultisampleState      m_ms = { };
    DxvkDepthStencilState     m_ds = { };
    DxvkLogicOpState          m_lo = { };
    std::array<DxvkBlendMode, MaxNumRenderTargets> m_blend = { };

    uint32_t                  m_viewportCount = 0u;
    std::array<DxvkViewport, MaxNumViewports> m_viewports = { };
    DxvkBlendConstants        m_blendConstants = { };
    DxvkDepthBias             m_depthBias = { };
    DxvkDepthBiasRepresentation m_depthBiasRepr = { };
    DxvkDepthBounds           m_depthBounds = { };
    uint32_t                  m_stencilRef = 0u;

    DxvkRenderTargets         m_rt;

    DxvkBufferSlice           m_indexBuffer;
    VkIndexType               m_indexType = VK_INDEX_TYPE_UINT32;
    std::array<DxvkBufferSlice, MaxNumVertexBindings> m_vertexBuffers = { };
    std::array<uint32_t,        MaxNumVertexBindings> m_vertexStrides = { };

    std::array<DxvkBufferSlice, MaxNumXfbBuffers> m_xfbBuffers;
    std::array<DxvkBufferSlice, MaxNumXfbBuffers> m_xfbCounters;

    DxvkBufferSlice           m_argBuffer;
    DxvkBufferSlice           m_cntBuffer;

    alignas(16) std::array<char, MaxPushConstantSize> m_pushConstants = { };

    std::vector<DxvkShaderResourceSlot> m_rc;

    std::vector<Rc<DxvkBuffer>> m_hostWrites;
    uint64_t                  m_hostWriteMark = 1u;

    std::vector<std::pair<Rc<DxvkFence>, uint64_t>> m_pendingFences;
    std::vector<std::pair<Rc<sync::Signal>, uint64_t>> m_pendingSignals;
    std::vector<Rc<DxvkEvent>> m_pendingEvents;

    DxvkStatCounters          m_stats;

    /// The gallium shaders bound, by gallium stage
    std::array<const DirkPipeShader*, 5> m_gfxShaders = { };
    const DirkPipeShader*     m_csShader = nullptr;
    bool                      m_computeBound = false;
    VkShaderStageFlags        m_pushStages = 0u;

    /// How many of each kind each gallium stage has bound, to unbind the rest
    struct DirkBoundCounts {
      uint32_t ubos = 0u, units = 0u, images = 0u, ssbos = 0u;
    };
    std::array<DirkBoundCounts, 6> m_bound = { };

    struct DirkPresentImage {
      pipe_resource* resource = nullptr;
    };
    std::unordered_map<void*, DirkPresentImage> m_presentImages;

    void dirtyResources(VkShaderStageFlags stages) {
      m_dirtyStages |= stages;
      if (stages & VK_SHADER_STAGE_COMPUTE_BIT)
        m_dirty.set(DirkDirty::ComputeResources);
      if (stages & ~VK_SHADER_STAGE_COMPUTE_BIT)
        m_dirty.set(DirkDirty::Resources);
    }

    void flushHostWrites();

    bool commitGraphicsState();

    bool commitComputeState();

    void commitFramebuffer();

    void commitShaders();

    void commitVertexInput();

    void commitVertexBuffers();

    void commitRasterizer();

    void commitDepthStencil();

    void commitBlend();

    void commitViewports();

    void commitResources(VkShaderStageFlagBits stage, const DirkPipeShader* pipeShader);


    void* getPipeShader(const Rc<DxvkShader>& shader, const DirkPipeShader** info);

    pipe_sampler_view* getSamplerView(const Rc<DxvkImageView>& view);

    pipe_sampler_view* getSamplerView(const Rc<DxvkBufferView>& view);

    void signalPending();

  };

}
