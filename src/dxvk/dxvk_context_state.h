#pragma once

#include "dxvk_bind_mask.h"
#include "dxvk_buffer.h"
#include "dxvk_constant_state.h"
#include "dxvk_framebuffer.h"
#include "dxvk_image.h"
#include "dxvk_limits.h"
#include "dxvk_sampler.h"
#include "dxvk_shader.h"

namespace dxvk {

  enum class DxvkBarrierControl : uint32_t {
    ComputeAllowWriteOnlyOverlap  = 0,
    ComputeAllowReadWriteOverlap  = 1,
    GraphicsAllowReadWriteOverlap = 2,
  };

  using DxvkBarrierControlFlags  = Flags<DxvkBarrierControl>;

  struct DxvkViewport {
    VkViewport viewport = { };
    VkRect2D   scissor  = { };
  };

  struct DxvkShaderResourceSlot {
    Rc<DxvkSampler>    sampler;
    Rc<DxvkImageView>  imageView;
    Rc<DxvkBufferView> bufferView;
    DxvkBufferSlice    bufferSlice;
  };

  struct DxvkSubmitStatus {
    std::atomic<VkResult> result = { VK_SUCCESS };
  };

  /** \brief What changed since the context last set gallium state */
  enum class DirkDirty : uint32_t {
    Shaders,
    VertexInput,
    VertexBuffers,
    IndexBuffer,
    Rasterizer,
    DepthStencil,
    Blend,
    Viewports,
    BlendConstants,
    StencilRef,
    Framebuffer,
    Resources,
    PushConstants,
    SampleMask,
    ComputeShader,
    ComputeResources,
  };

  using DirkDirtyFlags = Flags<DirkDirty>;

}
