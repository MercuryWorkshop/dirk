// dirk: DXVK's context commands on a gallium context.

#include <cstring>

#include "dxvk_device.h"

#include "../dirk/dirk.h"
#include "../dirk/dirk_nir.h"
#include "../util/util_bit.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "util/u_sampler.h"
#include "util/box.h"
#include "util/format/u_format.h"
#include "cso_cache/cso_context.h"

extern "C" enum pipe_format vk_format_to_pipe_format(VkFormat vkformat);

namespace dxvk {

  /** \brief A gallium shader made of a DXVK shader, and its binding map */
  struct DirkPipeShader {
    pipe_context*             pipe  = nullptr;
    enum dirk_stage           stage = DIRK_STAGE_VERTEX;
    void*                     cso   = nullptr;
    dirk_shader_layout        layout = { };
  };


  void dirkDestroyPipeShader(void* shader) {
    auto s = reinterpret_cast<DirkPipeShader*>(shader);
    if (!s)
      return;
    dirk_nir_delete(s->pipe, s->stage, s->cso);
    delete s;
  }

  // ---------------------------------------------------------------- enums

  static mesa_shader_stage dirkStage(VkShaderStageFlagBits stage) {
    switch (stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:                  return MESA_SHADER_VERTEX;
      case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return MESA_SHADER_TESS_CTRL;
      case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return MESA_SHADER_TESS_EVAL;
      case VK_SHADER_STAGE_GEOMETRY_BIT:                return MESA_SHADER_GEOMETRY;
      case VK_SHADER_STAGE_FRAGMENT_BIT:                return MESA_SHADER_FRAGMENT;
      default:                                          return MESA_SHADER_COMPUTE;
    }
  }


  static enum dirk_stage dirkNirStage(VkShaderStageFlagBits stage) {
    return static_cast<enum dirk_stage>(dirkStage(stage));
  }


  static mesa_prim dirkPrim(VkPrimitiveTopology t) {
    switch (t) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:                    return MESA_PRIM_POINTS;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:                     return MESA_PRIM_LINES;
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:                    return MESA_PRIM_LINE_STRIP;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:                 return MESA_PRIM_TRIANGLES;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:                return MESA_PRIM_TRIANGLE_STRIP;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:                  return MESA_PRIM_TRIANGLE_FAN;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:      return MESA_PRIM_LINES_ADJACENCY;
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:     return MESA_PRIM_LINE_STRIP_ADJACENCY;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:  return MESA_PRIM_TRIANGLES_ADJACENCY;
      case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: return MESA_PRIM_TRIANGLE_STRIP_ADJACENCY;
      case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:                    return MESA_PRIM_PATCHES;
      default:                                                  return MESA_PRIM_TRIANGLES;
    }
  }


  static unsigned dirkBlendFactor(VkBlendFactor f) {
    switch (f) {
      case VK_BLEND_FACTOR_ZERO:                     return PIPE_BLENDFACTOR_ZERO;
      case VK_BLEND_FACTOR_ONE:                      return PIPE_BLENDFACTOR_ONE;
      case VK_BLEND_FACTOR_SRC_COLOR:                return PIPE_BLENDFACTOR_SRC_COLOR;
      case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:      return PIPE_BLENDFACTOR_INV_SRC_COLOR;
      case VK_BLEND_FACTOR_DST_COLOR:                return PIPE_BLENDFACTOR_DST_COLOR;
      case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:      return PIPE_BLENDFACTOR_INV_DST_COLOR;
      case VK_BLEND_FACTOR_SRC_ALPHA:                return PIPE_BLENDFACTOR_SRC_ALPHA;
      case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:      return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
      case VK_BLEND_FACTOR_DST_ALPHA:                return PIPE_BLENDFACTOR_DST_ALPHA;
      case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:      return PIPE_BLENDFACTOR_INV_DST_ALPHA;
      case VK_BLEND_FACTOR_CONSTANT_COLOR:           return PIPE_BLENDFACTOR_CONST_COLOR;
      case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return PIPE_BLENDFACTOR_INV_CONST_COLOR;
      case VK_BLEND_FACTOR_CONSTANT_ALPHA:           return PIPE_BLENDFACTOR_CONST_ALPHA;
      case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
      case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:       return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
      case VK_BLEND_FACTOR_SRC1_COLOR:               return PIPE_BLENDFACTOR_SRC1_COLOR;
      case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:     return PIPE_BLENDFACTOR_INV_SRC1_COLOR;
      case VK_BLEND_FACTOR_SRC1_ALPHA:               return PIPE_BLENDFACTOR_SRC1_ALPHA;
      case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:     return PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
      default:                                       return PIPE_BLENDFACTOR_ONE;
    }
  }


  static bool dirkIsDualSource(VkBlendFactor f) {
    return f == VK_BLEND_FACTOR_SRC1_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR
        || f == VK_BLEND_FACTOR_SRC1_ALPHA || f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
  }


  static unsigned dirkBlendOp(VkBlendOp op) {
    switch (op) {
      case VK_BLEND_OP_ADD:              return PIPE_BLEND_ADD;
      case VK_BLEND_OP_SUBTRACT:         return PIPE_BLEND_SUBTRACT;
      case VK_BLEND_OP_REVERSE_SUBTRACT: return PIPE_BLEND_REVERSE_SUBTRACT;
      case VK_BLEND_OP_MIN:              return PIPE_BLEND_MIN;
      case VK_BLEND_OP_MAX:              return PIPE_BLEND_MAX;
      default:                           return PIPE_BLEND_ADD;
    }
  }


  static unsigned dirkStencilOp(VkStencilOp op) {
    switch (op) {
      case VK_STENCIL_OP_KEEP:                return PIPE_STENCIL_OP_KEEP;
      case VK_STENCIL_OP_ZERO:                return PIPE_STENCIL_OP_ZERO;
      case VK_STENCIL_OP_REPLACE:             return PIPE_STENCIL_OP_REPLACE;
      case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return PIPE_STENCIL_OP_INCR;
      case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return PIPE_STENCIL_OP_DECR;
      case VK_STENCIL_OP_INVERT:              return PIPE_STENCIL_OP_INVERT;
      case VK_STENCIL_OP_INCREMENT_AND_WRAP:  return PIPE_STENCIL_OP_INCR_WRAP;
      case VK_STENCIL_OP_DECREMENT_AND_WRAP:  return PIPE_STENCIL_OP_DECR_WRAP;
      default:                                return PIPE_STENCIL_OP_KEEP;
    }
  }


  static unsigned dirkWrap(uint32_t mode) {
    switch (VkSamplerAddressMode(mode)) {
      case VK_SAMPLER_ADDRESS_MODE_REPEAT:               return PIPE_TEX_WRAP_REPEAT;
      case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:      return PIPE_TEX_WRAP_MIRROR_REPEAT;
      case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:        return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:      return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
      case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
      default:                                           return PIPE_TEX_WRAP_REPEAT;
    }
  }


  static unsigned dirkSwizzle(VkComponentSwizzle s, unsigned identity) {
    switch (s) {
      case VK_COMPONENT_SWIZZLE_IDENTITY: return identity;
      case VK_COMPONENT_SWIZZLE_ZERO:     return PIPE_SWIZZLE_0;
      case VK_COMPONENT_SWIZZLE_ONE:      return PIPE_SWIZZLE_1;
      case VK_COMPONENT_SWIZZLE_R:        return PIPE_SWIZZLE_X;
      case VK_COMPONENT_SWIZZLE_G:        return PIPE_SWIZZLE_Y;
      case VK_COMPONENT_SWIZZLE_B:        return PIPE_SWIZZLE_Z;
      case VK_COMPONENT_SWIZZLE_A:        return PIPE_SWIZZLE_W;
      default:                            return identity;
    }
  }


  static pipe_texture_target dirkViewTarget(VkImageViewType type) {
    switch (type) {
      case VK_IMAGE_VIEW_TYPE_1D:         return PIPE_TEXTURE_1D;
      case VK_IMAGE_VIEW_TYPE_1D_ARRAY:   return PIPE_TEXTURE_1D_ARRAY;
      case VK_IMAGE_VIEW_TYPE_2D:         return PIPE_TEXTURE_2D;
      case VK_IMAGE_VIEW_TYPE_2D_ARRAY:   return PIPE_TEXTURE_2D_ARRAY;
      case VK_IMAGE_VIEW_TYPE_CUBE:       return PIPE_TEXTURE_CUBE;
      case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return PIPE_TEXTURE_CUBE_ARRAY;
      case VK_IMAGE_VIEW_TYPE_3D:         return PIPE_TEXTURE_3D;
      default:                            return PIPE_TEXTURE_2D;
    }
  }


  /** \brief The gallium format of a view, picking the aspect it reads */
  static pipe_format dirkViewFormat(VkFormat format, VkImageAspectFlags aspects) {
    pipe_format pf = vk_format_to_pipe_format(format);
    if (util_format_is_depth_and_stencil(pf)) {
      if (aspects == VK_IMAGE_ASPECT_STENCIL_BIT)
        pf = util_format_stencil_only(pf);
      else if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT)
        pf = util_format_get_depth_only(pf);
    }
    return pf;
  }


  static pipe_surface dirkSurface(const Rc<DxvkImageView>& view) {
    pipe_surface s = { };
    const auto key = view->info();
    s.format      = vk_format_to_pipe_format(key.format);
    s.texture     = view->image()->resource();
    s.level       = key.mipIndex;
    if (view->image()->info().type == VK_IMAGE_TYPE_3D) {
      s.first_layer = 0;
      s.last_layer  = view->mipLevelExtent(0).depth - 1;
    } else {
      s.first_layer = key.layerIndex;
      s.last_layer  = key.layerIndex + key.layerCount - 1;
    }
    return s;
  }

  // ---------------------------------------------------------------- context

  DxvkContext::DxvkContext(const Rc<DxvkDevice>& device)
  : m_device(device),
    m_pipe(device->pipe()),
    m_cso(device->cso()),
    m_rc(MaxNumResourceSlots) {
    m_dirty.set(DirkDirty::Shaders, DirkDirty::VertexInput,
      DirkDirty::VertexBuffers, DirkDirty::Rasterizer, DirkDirty::DepthStencil,
      DirkDirty::Blend, DirkDirty::Viewports, DirkDirty::BlendConstants,
      DirkDirty::StencilRef, DirkDirty::Framebuffer, DirkDirty::Resources,
      DirkDirty::SampleMask);
    m_dirtyStages = VK_SHADER_STAGE_ALL;

    for (auto& b : m_blend)
    {
      b.setBlendEnable(false);
      b.setColorOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
      b.setAlphaOp(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
      b.setWriteMask(0xf);
    }
    m_ms.setSampleMask(0xffff);
  }


  DxvkContext::~DxvkContext() {
    for (auto& e : m_presentImages)
      pipe_resource_reference(&e.second.resource, nullptr);
  }


  void DxvkContext::beginRecording(const Rc<DxvkCommandList>& cmdList) { }


  Rc<DxvkCommandList> DxvkContext::endRecording(const VkDebugUtilsLabelEXT* reason) {
    flushHostWrites();
    return new DxvkCommandList();
  }


  void DxvkContext::endFrame() { }


  void DxvkContext::flushCommandList(const VkDebugUtilsLabelEXT* reason, DxvkSubmitStatus* status) {
    flushHostWrites();
    m_pipe->flush(m_pipe, nullptr, PIPE_FLUSH_ASYNC);
    signalPending();
    if (status)
      status->result = VK_SUCCESS;
  }


  void DxvkContext::signalPending() {
    for (auto& e : m_pendingEvents)
      e->setStatus(DxvkGpuEventStatus::Signaled);
    m_pendingEvents.clear();

    for (auto& f : m_pendingFences)
      f.first->signal(f.second);
    m_pendingFences.clear();

    for (auto& s : m_pendingSignals)
      s.first->signal(s.second);
    m_pendingSignals.clear();
  }


  void DxvkContext::beginQuery(const Rc<DxvkQuery>& query) {
    query->begin();
  }


  void DxvkContext::endQuery(const Rc<DxvkQuery>& query) {
    query->end();
  }


  void DxvkContext::writeTimestamp(const Rc<DxvkQuery>& query) {
    query->end();
  }


  void DxvkContext::signalGpuEvent(const Rc<DxvkEvent>& event) {
    event->setStatus(DxvkGpuEventStatus::Pending);
    m_pendingEvents.push_back(event);
  }


  void DxvkContext::signal(const Rc<sync::Signal>& signal, uint64_t value) {
    m_pendingSignals.emplace_back(signal, value);
  }


  void DxvkContext::waitFence(const Rc<DxvkFence>& fence, uint64_t value) {
    // One queue: everything signalled before this point is already ahead.
  }


  void DxvkContext::signalFence(const Rc<DxvkFence>& fence, uint64_t value) {
    m_pendingFences.emplace_back(fence, value);
  }


  void DxvkContext::beginDebugLabel(const VkDebugUtilsLabelEXT& label) { }
  void DxvkContext::endDebugLabel() { }
  void DxvkContext::insertDebugLabel(const VkDebugUtilsLabelEXT& label) { }
  void DxvkContext::setBarrierControl(DxvkBarrierControlFlags control) { }
  void DxvkContext::updatePageTable(const DxvkSparseBindInfo& bindInfo, DxvkSparseBindFlags flags) { }

  void DxvkContext::launchCuKernelNVX(
    const VkCuLaunchInfoNVX& nvxLaunchInfo,
    const std::vector<std::pair<Rc<DxvkBuffer>, DxvkAccessFlags>>& buffers,
    const std::vector<std::pair<Rc<DxvkImage>, DxvkAccessFlags>>& images) { }

  // ---------------------------------------------------------------- host writes

  void DxvkContext::queueHostWrites(DxvkBuffer* buffer) {
    if (buffer->dirtyEnd() > buffer->dirtyBegin() && !buffer->queuedForUpload(m_hostWriteMark))
      m_hostWrites.push_back(buffer);
  }


  void DxvkContext::flushHostWrites() {
    if (m_hostWrites.empty())
      return;
    for (auto& b : m_hostWrites)
      b->uploadHostWrites(m_pipe);
    m_hostWrites.clear();
    m_hostWriteMark += 1u;
  }


  void DxvkContext::markHostWritten(const Rc<DxvkBuffer>& buffer, VkDeviceSize offset, VkDeviceSize length) {
    buffer->markWritten(offset, length);
    queueHostWrites(buffer.ptr());
  }


  void DxvkContext::invalidateBuffer(
    const Rc<DxvkBuffer>&           buffer,
          Rc<DxvkResourceAllocation>&& slice) {
    buffer->assignStorage(std::move(slice));
    queueHostWrites(buffer.ptr());
  }


  void DxvkContext::ensureBufferAddress(const Rc<DxvkBuffer>& buffer) { }


  void DxvkContext::invalidateImage(
    const Rc<DxvkImage>&            image,
          Rc<DxvkResourceAllocation>&& slice) { }


  void DxvkContext::invalidateImageWithUsage(
    const Rc<DxvkImage>&            image,
          Rc<DxvkResourceAllocation>&& slice,
    const DxvkImageUsageInfo&       usageInfo) { }


  bool DxvkContext::ensureImageCompatibility(
    const Rc<DxvkImage>&            image,
    const DxvkImageUsageInfo&       usageInfo) {
    return true;
  }

  // ---------------------------------------------------------------- state

  void DxvkContext::setViewports(uint32_t viewportCount, const DxvkViewport* viewports) {
    m_viewportCount = std::min<uint32_t>(viewportCount, MaxNumViewports);
    for (uint32_t i = 0; i < m_viewportCount; i++)
      m_viewports[i] = viewports[i];
    m_dirty.set(DirkDirty::Viewports);
  }


  void DxvkContext::setBlendConstants(DxvkBlendConstants blendConstants) {
    m_blendConstants = blendConstants;
    m_dirty.set(DirkDirty::BlendConstants);
  }


  void DxvkContext::setDepthBias(DxvkDepthBias depthBias) {
    m_depthBias = depthBias;
    m_dirty.set(DirkDirty::Rasterizer);
  }


  void DxvkContext::setDepthBiasRepresentation(DxvkDepthBiasRepresentation depthBiasRepresentation) {
    m_depthBiasRepr = depthBiasRepresentation;
  }


  void DxvkContext::setDepthBounds(DxvkDepthBounds depthBounds) {
    m_depthBounds = depthBounds;
    m_dirty.set(DirkDirty::DepthStencil);
  }


  void DxvkContext::setStencilReference(uint32_t reference) {
    m_stencilRef = reference;
    m_dirty.set(DirkDirty::StencilRef);
  }


  void DxvkContext::setInputAssemblyState(const DxvkInputAssemblyState& ia) {
    if (ia.primitiveTopology() != m_ia.primitiveTopology())
      m_dirty.set(DirkDirty::Shaders);
    m_ia = ia;
  }


  void DxvkContext::setInputLayout(
          uint32_t             attributeCount,
    const DxvkVertexInput*     attributes,
          uint32_t             bindingCount,
    const DxvkVertexInput*     bindings) {
    m_attrCount = std::min<uint32_t>(attributeCount, MaxNumVertexAttributes);
    m_bindCount = std::min<uint32_t>(bindingCount, MaxNumVertexBindings);
    for (uint32_t i = 0; i < m_attrCount; i++)
      m_attrs[i] = attributes[i].attribute();
    for (uint32_t i = 0; i < m_bindCount; i++)
      m_binds[i] = bindings[i].binding();
    m_dirty.set(DirkDirty::VertexInput);
  }


  void DxvkContext::setRasterizerState(const DxvkRasterizerState& rs) {
    m_rs = rs;
    m_dirty.set(DirkDirty::Rasterizer);
  }


  void DxvkContext::setMultisampleState(const DxvkMultisampleState& ms) {
    m_ms = ms;
    m_dirty.set(DirkDirty::SampleMask, DirkDirty::Blend);
  }


  void DxvkContext::setDepthStencilState(const DxvkDepthStencilState& ds) {
    m_ds = ds;
    m_dirty.set(DirkDirty::DepthStencil);
  }


  void DxvkContext::setLogicOpState(const DxvkLogicOpState& lo) {
    m_lo = lo;
    m_dirty.set(DirkDirty::Blend);
  }


  void DxvkContext::setBlendMode(uint32_t attachment, const DxvkBlendMode& blendMode) {
    m_blend[attachment] = blendMode;
    m_dirty.set(DirkDirty::Blend, DirkDirty::Shaders);
  }

  // ---------------------------------------------------------------- commit

  void DxvkContext::commitFramebuffer() {
    pipe_framebuffer_state fb = { };
    uint32_t w = ~0u, h = ~0u, layers = ~0u;

    auto extend = [&] (const Rc<DxvkImageView>& view) {
      VkExtent3D extent = view->mipLevelExtent(0);
      w = std::min(w, extent.width);
      h = std::min(h, extent.height);
      layers = std::min<uint32_t>(layers, view->info().layerCount);
      fb.samples = std::max<uint8_t>(fb.samples, uint8_t(view->image()->info().sampleCount));
    };

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      const auto& view = m_rt.color[i].view;
      if (view == nullptr)
        continue;
      fb.cbufs[i] = dirkSurface(view);
      fb.nr_cbufs = i + 1;
      extend(view);
    }

    if (m_rt.depth.view != nullptr) {
      fb.zsbuf = dirkSurface(m_rt.depth.view);
      extend(m_rt.depth.view);
    }

    if (w == ~0u) {
      w = 1;
      h = 1;
      layers = 1;
    }

    fb.width  = w;
    fb.height = h;
    fb.layers = layers;
    cso_set_framebuffer(m_cso, &fb);
  }


  void* DxvkContext::getPipeShader(const Rc<DxvkShader>& shader, const DirkPipeShader** info) {
    VkShaderStageFlagBits stage = shader->info().stage;
    DxvkShaderModuleCreateInfo state;

    if (stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
      for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
        const auto& b = m_blend[i];
        if (b.blendEnable() && (dirkIsDualSource(b.colorSrcFactor()) || dirkIsDualSource(b.colorDstFactor())
         || dirkIsDualSource(b.alphaSrcFactor()) || dirkIsDualSource(b.alphaDstFactor())))
          state.fsDualSrcBlend = true;
      }
      state.fsFlatShading = m_rs.flatShading() && shader->info().flatShadingInputs;

      // WebGPU wants every fragment input written by the stage before.
      Rc<DxvkShader> prev = m_gs != nullptr ? m_gs : (m_tes != nullptr ? m_tes : m_vs);
      if (prev != nullptr)
        state.undefinedInputs = shader->info().inputMask & ~prev->info().outputMask;
    } else if (stage == VK_SHADER_STAGE_GEOMETRY_BIT) {
      state.inputTopology = m_ia.primitiveTopology();
    }

    const uint32_t specMask = shader->getSpecConstantMask();
    const bool compute = stage == VK_SHADER_STAGE_COMPUTE_BIT;
    state.specMask = specMask & ((1u << MaxNumSpecConstants) - 1u);
    for (uint32_t i = 0; i < MaxNumSpecConstants; i++) {
      if (state.specMask & (1u << i))
        state.spec[i] = compute ? m_cpSpec[i] : m_gpSpec[i];
    }

    auto variant = reinterpret_cast<DirkPipeShader*>(shader->variant(state));
    if (!variant) {
      variant = new DirkPipeShader();
      variant->pipe  = m_pipe;
      variant->stage = dirkNirStage(stage);

      std::array<dirk_spec_constant, MaxNumSpecConstants + 1> spec;
      unsigned specCount = 0;
      for (uint32_t i = 0; i < MaxNumSpecConstants; i++) {
        if (state.specMask & (1u << i))
          spec[specCount++] = { i, state.spec[i] };
      }
      if (specMask & (1u << MaxNumSpecConstants))
        spec[specCount++] = { uint32_t(MaxNumSpecConstants), 1u };

      SpirvCodeBuffer code = shader->getCode(state);
      char why[256] = "";
      variant->cso = dirk_nir_compile(m_pipe, variant->stage,
        code.data(), code.dwords(), spec.data(), specCount,
        &variant->layout, why, sizeof(why));
      if (!variant->cso)
        Logger::err(str::format("dirk: ", shader->debugName(), ": ", why));

      shader->setVariant(state, variant);
    }

    *info = variant;
    return variant->cso;
  }


  void DxvkContext::commitShaders() {
    const DirkPipeShader* info = nullptr;
    m_gfxShaders = { };

    void* vs = m_vs != nullptr ? getPipeShader(m_vs, &info) : nullptr;
    m_gfxShaders[MESA_SHADER_VERTEX] = vs ? info : nullptr;
    cso_set_vertex_shader_handle(m_cso, vs);

    void* tcs = m_tcs != nullptr ? getPipeShader(m_tcs, &info) : nullptr;
    m_gfxShaders[MESA_SHADER_TESS_CTRL] = tcs ? info : nullptr;
    cso_set_tessctrl_shader_handle(m_cso, tcs);

    void* tes = m_tes != nullptr ? getPipeShader(m_tes, &info) : nullptr;
    m_gfxShaders[MESA_SHADER_TESS_EVAL] = tes ? info : nullptr;
    cso_set_tesseval_shader_handle(m_cso, tes);

    void* gs = m_gs != nullptr ? getPipeShader(m_gs, &info) : nullptr;
    m_gfxShaders[MESA_SHADER_GEOMETRY] = gs ? info : nullptr;
    cso_set_geometry_shader_handle(m_cso, gs);

    void* fs = m_fs != nullptr ? getPipeShader(m_fs, &info) : nullptr;
    m_gfxShaders[MESA_SHADER_FRAGMENT] = fs ? info : nullptr;
    // No pixel shader: a depth-only pass. The empty shader has no
    // resources, so the stage keeps no binding map.
    cso_set_fragment_shader_handle(m_cso,
      m_fs != nullptr ? fs : m_device->emptyFragmentShader());

    // The binding map of a stage is its shader's.
    m_dirtyStages |= VK_SHADER_STAGE_ALL_GRAPHICS;
    m_dirty.set(DirkDirty::Resources, DirkDirty::VertexInput);
  }


  void DxvkContext::commitVertexInput() {
    cso_velems_state velems = { };
    uint32_t maxLocation = 0;

    for (uint32_t i = 0; i < m_attrCount; i++)
      maxLocation = std::max(maxLocation, m_attrs[i].location + 1);

    // Element i feeds shader input location i; a location the layout does
    // not supply points at a buffer that is never bound.
    for (uint32_t i = 0; i < maxLocation; i++) {
      auto& e = velems.velems[i];
      e.vertex_buffer_index = PIPE_MAX_ATTRIBS - 1;
      e.src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
    }

    for (uint32_t i = 0; i < m_attrCount; i++) {
      const auto& a = m_attrs[i];
      auto& e = velems.velems[a.location];
      e.src_offset = a.offset;
      e.vertex_buffer_index = a.binding;
      e.src_format = vk_format_to_pipe_format(a.format);
      e.src_stride = m_vertexStrides[a.binding];
      e.instance_divisor = 0;
      for (uint32_t j = 0; j < m_bindCount; j++) {
        if (m_binds[j].binding == a.binding && m_binds[j].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
          e.instance_divisor = m_binds[j].divisor ? m_binds[j].divisor : ~0u;
      }
    }

    velems.count = maxLocation;
    cso_set_vertex_elements(m_cso, &velems);
  }


  void DxvkContext::commitVertexBuffers() {
    std::array<pipe_vertex_buffer, MaxNumVertexBindings> vbs = { };
    uint32_t count = 0;

    for (uint32_t i = 0; i < MaxNumVertexBindings; i++) {
      const auto& slice = m_vertexBuffers[i];
      if (!slice.defined())
        continue;
      vbs[i].buffer.resource = slice.buffer()->resource();
      vbs[i].buffer_offset = slice.offset();
      count = i + 1;
    }

    cso_set_vertex_buffers(m_cso, count, vbs.data());
  }


  void DxvkContext::commitRasterizer() {
    pipe_rasterizer_state rs = { };
    rs.fill_front = rs.fill_back = m_rs.polygonMode() == VK_POLYGON_MODE_LINE
      ? PIPE_POLYGON_MODE_LINE : m_rs.polygonMode() == VK_POLYGON_MODE_POINT
      ? PIPE_POLYGON_MODE_POINT : PIPE_POLYGON_MODE_FILL;
    switch (m_rs.cullMode()) {
      case VK_CULL_MODE_FRONT_BIT:      rs.cull_face = PIPE_FACE_FRONT; break;
      case VK_CULL_MODE_BACK_BIT:       rs.cull_face = PIPE_FACE_BACK; break;
      case VK_CULL_MODE_FRONT_AND_BACK: rs.cull_face = PIPE_FACE_FRONT_AND_BACK; break;
      default:                          rs.cull_face = PIPE_FACE_NONE; break;
    }
    rs.front_ccw = m_rs.frontFace() == VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depth_clip_near = m_rs.depthClip();
    rs.depth_clip_far = m_rs.depthClip();
    rs.depth_clamp = !m_rs.depthClip();
    rs.offset_tri = rs.offset_line = rs.offset_point = m_rs.depthBias();
    rs.offset_units = m_depthBias.depthBiasConstant;
    rs.offset_scale = m_depthBias.depthBiasSlope;
    rs.offset_clamp = m_depthBias.depthBiasClamp;
    rs.offset_units_unscaled = false;
    rs.scissor = 1;
    rs.half_pixel_center = 1;
    rs.bottom_edge_rule = 0;
    rs.clip_halfz = 1;
    rs.multisample = m_rs.sampleCount() > VK_SAMPLE_COUNT_1_BIT;
    rs.flatshade = m_rs.flatShading();
    rs.flatshade_first = 1;
    rs.line_width = 1.0f;
    rs.point_size = 1.0f;
    rs.point_quad_rasterization = 0;
    rs.line_smooth = m_rs.lineMode() == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
    rs.line_rectangular = m_rs.lineMode() != VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT;
    rs.depth_clip_near = rs.depth_clip_far = m_rs.depthClip();
    cso_set_rasterizer(m_cso, &rs);
  }


  void DxvkContext::commitDepthStencil() {
    pipe_depth_stencil_alpha_state dsa = { };
    dsa.depth_enabled = m_ds.depthTest();
    dsa.depth_writemask = m_ds.depthTest() && m_ds.depthWrite();
    dsa.depth_func = m_ds.depthCompareOp();

    auto stencil = [&] (pipe_stencil_state& s, DxvkStencilOp op) {
      s.enabled = m_ds.stencilTest();
      s.func = op.compareOp();
      s.fail_op = dirkStencilOp(op.failOp());
      s.zpass_op = dirkStencilOp(op.passOp());
      s.zfail_op = dirkStencilOp(op.depthFailOp());
      s.valuemask = op.compareMask();
      s.writemask = op.writeMask();
    };

    stencil(dsa.stencil[0], m_ds.stencilOpFront());
    stencil(dsa.stencil[1], m_ds.stencilOpBack());
    cso_set_depth_stencil_alpha(m_cso, &dsa);
  }


  void DxvkContext::commitBlend() {
    pipe_blend_state blend = { };
    blend.independent_blend_enable = 1;
    blend.alpha_to_coverage = m_ms.alphaToCoverage();
    blend.logicop_enable = m_lo.logicOpEnable();
    blend.logicop_func = m_lo.logicOp();
    blend.max_rt = MaxNumRenderTargets - 1;

    for (uint32_t i = 0; i < MaxNumRenderTargets; i++) {
      const auto& b = m_blend[i];
      auto& rt = blend.rt[i];
      rt.blend_enable = b.blendEnable();
      rt.rgb_func = dirkBlendOp(b.colorBlendOp());
      rt.rgb_src_factor = dirkBlendFactor(b.colorSrcFactor());
      rt.rgb_dst_factor = dirkBlendFactor(b.colorDstFactor());
      rt.alpha_func = dirkBlendOp(b.alphaBlendOp());
      rt.alpha_src_factor = dirkBlendFactor(b.alphaSrcFactor());
      rt.alpha_dst_factor = dirkBlendFactor(b.alphaDstFactor());
      rt.colormask = b.writeMask();
    }

    cso_set_blend(m_cso, &blend);
  }


  void DxvkContext::commitViewports() {
    std::array<pipe_viewport_state, MaxNumViewports> vps = { };
    std::array<pipe_scissor_state, MaxNumViewports> scs = { };
    const uint32_t count = std::max(m_viewportCount, 1u);

    for (uint32_t i = 0; i < count; i++) {
      const VkViewport& v = m_viewports[i].viewport;
      vps[i].scale[0] = v.width * 0.5f;
      vps[i].scale[1] = v.height * 0.5f;
      vps[i].scale[2] = v.maxDepth - v.minDepth;
      vps[i].translate[0] = v.x + v.width * 0.5f;
      vps[i].translate[1] = v.y + v.height * 0.5f;
      vps[i].translate[2] = v.minDepth;
      vps[i].swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
      vps[i].swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
      vps[i].swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
      vps[i].swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;

      const VkRect2D& r = m_viewports[i].scissor;
      scs[i].minx = std::max(r.offset.x, 0);
      scs[i].miny = std::max(r.offset.y, 0);
      scs[i].maxx = std::max<int32_t>(r.offset.x + int32_t(r.extent.width), 0);
      scs[i].maxy = std::max<int32_t>(r.offset.y + int32_t(r.extent.height), 0);
    }

    if (count == 1)
      cso_set_viewport(m_cso, &vps[0]);
    else
      m_pipe->set_viewport_states(m_pipe, 0, count, vps.data());
    m_pipe->set_scissor_states(m_pipe, 0, count, scs.data());
  }


  pipe_sampler_view* DxvkContext::getSamplerView(const Rc<DxvkImageView>& view) {
    pipe_sampler_view*& sv = view->samplerView();
    if (sv)
      return sv;

    const auto key = view->info();
    pipe_sampler_view templ = { };
    u_sampler_view_default_template(&templ, view->image()->resource(),
      dirkViewFormat(key.format, key.aspects));
    templ.target = dirkViewTarget(key.viewType);
    templ.u.tex.first_level = key.mipIndex;
    templ.u.tex.last_level = key.mipIndex + key.mipCount - 1;
    if (view->image()->info().type == VK_IMAGE_TYPE_3D) {
      templ.u.tex.first_layer = 0;
      templ.u.tex.last_layer = 0;
    } else {
      templ.u.tex.first_layer = key.layerIndex;
      templ.u.tex.last_layer = key.layerIndex + key.layerCount - 1;
    }

    const VkComponentMapping swizzle = key.unpackSwizzle();
    templ.swizzle_r = dirkSwizzle(swizzle.r, PIPE_SWIZZLE_X);
    templ.swizzle_g = dirkSwizzle(swizzle.g, PIPE_SWIZZLE_Y);
    templ.swizzle_b = dirkSwizzle(swizzle.b, PIPE_SWIZZLE_Z);
    templ.swizzle_a = dirkSwizzle(swizzle.a, PIPE_SWIZZLE_W);

    sv = m_pipe->create_sampler_view(m_pipe, view->image()->resource(), &templ);
    return sv;
  }


  pipe_sampler_view* DxvkContext::getSamplerView(const Rc<DxvkBufferView>& view) {
    pipe_sampler_view*& sv = view->samplerView();
    if (sv)
      return sv;

    const auto key = view->info();
    pipe_sampler_view templ = { };
    pipe_resource* res = view->buffer()->resource();
    templ.format = key.format != VK_FORMAT_UNDEFINED
      ? vk_format_to_pipe_format(key.format) : PIPE_FORMAT_R32_UINT;
    templ.target = PIPE_BUFFER;
    templ.u.buf.offset = key.offset;
    templ.u.buf.size = key.size;
    templ.swizzle_r = PIPE_SWIZZLE_X;
    templ.swizzle_g = PIPE_SWIZZLE_Y;
    templ.swizzle_b = PIPE_SWIZZLE_Z;
    templ.swizzle_a = PIPE_SWIZZLE_W;
    sv = m_pipe->create_sampler_view(m_pipe, res, &templ);
    return sv;
  }


  static void dirkSamplerState(const DxvkSamplerKey& key, pipe_sampler_state& s) {
    s = { };
    s.min_img_filter = key.u.p.minFilter ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
    s.mag_img_filter = key.u.p.magFilter ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
    s.min_mip_filter = key.u.p.mipMode ? PIPE_TEX_MIPFILTER_LINEAR : PIPE_TEX_MIPFILTER_NEAREST;
    s.max_anisotropy = key.u.p.anisotropy;
    s.wrap_s = dirkWrap(key.u.p.addressU);
    s.wrap_t = dirkWrap(key.u.p.addressV);
    s.wrap_r = dirkWrap(key.u.p.addressW);
    s.lod_bias = bit::decodeFixed<int32_t, 6, 8>(key.u.p.lodBias);
    s.min_lod = bit::decodeFixed<uint32_t, 4, 8>(key.u.p.minLod);
    s.max_lod = bit::decodeFixed<uint32_t, 4, 8>(key.u.p.maxLod);
    s.compare_mode = key.u.p.compareEnable ? PIPE_TEX_COMPARE_R_TO_TEXTURE : PIPE_TEX_COMPARE_NONE;
    s.compare_func = key.u.p.compareOp;
    s.unnormalized_coords = key.u.p.pixelCoord;
    s.seamless_cube_map = !key.u.p.legacyCube;
    if (key.u.p.hasBorder)
      std::memcpy(s.border_color.ui, key.borderColor.uint32, sizeof(s.border_color.ui));
  }


  void DxvkContext::commitResources(
          VkShaderStageFlagBits stage,
    const DirkPipeShader*       shader) {
    const mesa_shader_stage pstage = dirkStage(stage);
    const dirk_shader_layout empty = { };
    const dirk_shader_layout& l = shader ? shader->layout : empty;
    auto& bound = m_bound[pstage];

    // Constant buffers
    for (uint32_t i = 0; i < l.ubo_count; i++) {
      pipe_constant_buffer cb = { };
      if (i == l.push_ubo) {
        cb.buffer_size = MaxPushConstantSize;
        u_upload_data(m_pipe->const_uploader, 0, MaxPushConstantSize, 256,
          m_pushConstants.data(), &cb.buffer_offset, &cb.buffer, nullptr);
        m_pipe->set_constant_buffer(m_pipe, pstage, i, &cb);
        pipe_resource_reference(&cb.buffer, nullptr);
        continue;
      }

      const auto& slice = m_rc[l.ubos[i]].bufferSlice;
      if (slice.defined()) {
        cb.buffer = slice.buffer()->resource();
        cb.buffer_offset = slice.offset();
        cb.buffer_size = std::min<uint32_t>(slice.length(), MaxUniformBufferSize);
        m_pipe->set_constant_buffer(m_pipe, pstage, i, &cb);
      } else {
        m_pipe->set_constant_buffer(m_pipe, pstage, i, nullptr);
      }
    }
    for (uint32_t i = l.ubo_count; i < bound.ubos; i++)
      m_pipe->set_constant_buffer(m_pipe, pstage, i, nullptr);
    bound.ubos = l.ubo_count;

    // Texture units
    std::array<pipe_sampler_view*, DIRK_MAX_UNITS> views = { };
    for (uint32_t i = 0; i < l.unit_count; i++) {
      const auto& t = m_rc[l.units[i].texture];
      if (t.imageView != nullptr)
        views[i] = getSamplerView(t.imageView);
      else if (t.bufferView != nullptr)
        views[i] = getSamplerView(t.bufferView);

      pipe_sampler_state ss;
      if (l.units[i].sampler != DIRK_NO_BINDING && m_rc[l.units[i].sampler].sampler != nullptr) {
        dirkSamplerState(m_rc[l.units[i].sampler].sampler->key(), ss);
      } else {
        ss = { };
        ss.wrap_s = ss.wrap_t = ss.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
        ss.min_img_filter = ss.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
        ss.min_mip_filter = PIPE_TEX_MIPFILTER_NEAREST;
        ss.max_lod = 16.0f;
      }
      cso_single_sampler(m_cso, pstage, i, &ss);
    }
    cso_single_sampler_done(m_cso, pstage);
    m_pipe->set_sampler_views(m_pipe, pstage, 0, l.unit_count,
      bound.units > l.unit_count ? bound.units - l.unit_count : 0, views.data());
    bound.units = l.unit_count;

    // Images
    std::array<pipe_image_view, DIRK_MAX_IMAGES> images = { };
    for (uint32_t i = 0; i < l.image_count; i++) {
      const auto& slot = m_rc[l.images[i]];
      auto& iv = images[i];
      if (slot.imageView != nullptr) {
        const auto key = slot.imageView->info();
        iv.resource = slot.imageView->image()->resource();
        iv.format = vk_format_to_pipe_format(key.format);
        iv.access = PIPE_IMAGE_ACCESS_READ_WRITE;
        iv.shader_access = PIPE_IMAGE_ACCESS_READ_WRITE;
        iv.u.tex.level = key.mipIndex;
        iv.u.tex.first_layer = key.layerIndex;
        iv.u.tex.last_layer = key.layerIndex + key.layerCount - 1;
        iv.u.tex.single_layer_view = key.layerCount == 1
          && slot.imageView->image()->info().numLayers > 1;
      } else if (slot.bufferView != nullptr) {
        const auto key = slot.bufferView->info();
        iv.resource = slot.bufferView->buffer()->resource();
        iv.format = key.format != VK_FORMAT_UNDEFINED
          ? vk_format_to_pipe_format(key.format) : PIPE_FORMAT_R32_UINT;
        iv.access = PIPE_IMAGE_ACCESS_READ_WRITE;
        iv.shader_access = PIPE_IMAGE_ACCESS_READ_WRITE;
        iv.u.buf.offset = key.offset;
        iv.u.buf.size = key.size;
        if (slot.bufferView->buffer()->mapPtr(0))
          slot.bufferView->buffer()->markHostStale(true);
      }
    }
    m_pipe->set_shader_images(m_pipe, pstage, 0, l.image_count,
      bound.images > l.image_count ? bound.images - l.image_count : 0, images.data());
    bound.images = l.image_count;

    // Storage buffers
    std::array<pipe_shader_buffer, DIRK_MAX_SSBOS> ssbos = { };
    for (uint32_t i = 0; i < l.ssbo_count; i++) {
      const auto& slice = m_rc[l.ssbos[i]].bufferSlice;
      if (!slice.defined())
        continue;
      ssbos[i].buffer = slice.buffer()->resource();
      ssbos[i].buffer_offset = slice.offset();
      ssbos[i].buffer_size = slice.length();
      if (slice.buffer()->mapPtr(0))
        slice.buffer()->markHostStale(true);
    }
    const uint32_t ssboCount = std::max(l.ssbo_count, bound.ssbos);
    m_pipe->set_shader_buffers(m_pipe, pstage, 0, ssboCount, ssbos.data(),
      (1u << l.ssbo_count) - 1u);
    bound.ssbos = l.ssbo_count;
  }


  bool DxvkContext::commitGraphicsState() {
    flushHostWrites();

    if (m_dirty.test(DirkDirty::Framebuffer))
      commitFramebuffer();
    if (m_dirty.test(DirkDirty::Shaders))
      commitShaders();
    if (m_dirty.test(DirkDirty::VertexInput) || m_dirty.test(DirkDirty::VertexBuffers))
      commitVertexInput();
    if (m_dirty.test(DirkDirty::VertexBuffers) || m_dirty.test(DirkDirty::VertexInput))
      commitVertexBuffers();
    if (m_dirty.test(DirkDirty::Rasterizer))
      commitRasterizer();
    if (m_dirty.test(DirkDirty::DepthStencil))
      commitDepthStencil();
    if (m_dirty.test(DirkDirty::Blend))
      commitBlend();
    if (m_dirty.test(DirkDirty::Viewports))
      commitViewports();
    if (m_dirty.test(DirkDirty::BlendConstants)) {
      pipe_blend_color c = { };
      std::memcpy(c.color, &m_blendConstants, sizeof(c.color));
      m_pipe->set_blend_color(m_pipe, &c);
    }
    if (m_dirty.test(DirkDirty::StencilRef)) {
      pipe_stencil_ref ref = { };
      ref.ref_value[0] = ref.ref_value[1] = uint8_t(m_stencilRef);
      cso_set_stencil_ref(m_cso, ref);
    }
    if (m_dirty.test(DirkDirty::SampleMask))
      cso_set_sample_mask(m_cso, m_ms.sampleMask());

    if (m_dirty.test(DirkDirty::Resources) || m_dirty.test(DirkDirty::PushConstants)) {
      static const VkShaderStageFlagBits stages[] = {
        VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT };
      for (auto stage : stages) {
        const DirkPipeShader* shader = m_gfxShaders[dirkStage(stage)];
        const bool push = m_dirty.test(DirkDirty::PushConstants)
          && shader && shader->layout.push_ubo != DIRK_NO_BINDING;
        if ((m_dirtyStages & stage) || push)
          commitResources(stage, shader);
      }
      m_dirtyStages &= ~VK_SHADER_STAGE_ALL_GRAPHICS;
    }

    m_dirty.clr(DirkDirty::Framebuffer, DirkDirty::Shaders, DirkDirty::VertexInput,
      DirkDirty::VertexBuffers, DirkDirty::Rasterizer, DirkDirty::DepthStencil,
      DirkDirty::Blend, DirkDirty::Viewports, DirkDirty::BlendConstants,
      DirkDirty::StencilRef, DirkDirty::SampleMask, DirkDirty::Resources);
    m_dirty.clr(DirkDirty::PushConstants);
    m_computeBound = false;

    return m_gfxShaders[MESA_SHADER_VERTEX] && m_gfxShaders[MESA_SHADER_VERTEX]->cso
        && (!m_fs || (m_gfxShaders[MESA_SHADER_FRAGMENT] && m_gfxShaders[MESA_SHADER_FRAGMENT]->cso));
  }


  bool DxvkContext::commitComputeState() {
    flushHostWrites();

    if (m_dirty.test(DirkDirty::ComputeShader)) {
      const DirkPipeShader* info = nullptr;
      void* cs = m_cs != nullptr ? getPipeShader(m_cs, &info) : nullptr;
      m_csShader = cs ? info : nullptr;
      cso_set_compute_shader_handle(m_cso, cs);
      m_dirty.set(DirkDirty::ComputeResources);
      m_dirty.clr(DirkDirty::ComputeShader);
    }

    if (m_dirty.test(DirkDirty::ComputeResources) || m_dirty.test(DirkDirty::PushConstants)) {
      commitResources(VK_SHADER_STAGE_COMPUTE_BIT, m_csShader);
      m_dirtyStages &= ~VK_SHADER_STAGE_COMPUTE_BIT;
      m_dirty.clr(DirkDirty::ComputeResources);
      // Graphics push constants are uploaded again at the next draw.
      m_dirty.set(DirkDirty::Resources);
      m_dirtyStages |= VK_SHADER_STAGE_ALL_GRAPHICS & m_pushStages;
    }

    return m_csShader && m_csShader->cso;
  }

  // ---------------------------------------------------------------- draws

  void DxvkContext::draw(uint32_t count, const VkDrawIndirectCommand* draws) {
    if (!commitGraphicsState())
      return;

    pipe_draw_info info = { };
    info.mode = dirkPrim(m_ia.primitiveTopology());
    info.primitive_restart = false;
    if (info.mode == MESA_PRIM_PATCHES)
      m_pipe->set_patch_vertices(m_pipe, m_ia.patchVertexCount());

    for (uint32_t i = 0; i < count; i++) {
      info.start_instance = draws[i].firstInstance;
      info.instance_count = draws[i].instanceCount;
      pipe_draw_start_count_bias sc = { };
      sc.start = draws[i].firstVertex;
      sc.count = draws[i].vertexCount;
      cso_draw_vbo(m_cso, &info, 0, nullptr, &sc, 1);
    }
  }


  void DxvkContext::drawIndexed(uint32_t count, const VkDrawIndexedIndirectCommand* draws) {
    if (!commitGraphicsState() || !m_indexBuffer.defined())
      return;

    const uint32_t indexSize = m_indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    pipe_draw_info info = { };
    info.mode = dirkPrim(m_ia.primitiveTopology());
    info.index_size = indexSize;
    info.index.resource = m_indexBuffer.buffer()->resource();
    info.primitive_restart = m_ia.primitiveRestart();
    info.restart_index = indexSize == 2 ? 0xffffu : 0xffffffffu;
    if (info.mode == MESA_PRIM_PATCHES)
      m_pipe->set_patch_vertices(m_pipe, m_ia.patchVertexCount());

    const uint32_t base = uint32_t(m_indexBuffer.offset() / indexSize);
    for (uint32_t i = 0; i < count; i++) {
      info.start_instance = draws[i].firstInstance;
      info.instance_count = draws[i].instanceCount;
      pipe_draw_start_count_bias sc = { };
      sc.start = base + draws[i].firstIndex;
      sc.count = draws[i].indexCount;
      sc.index_bias = draws[i].vertexOffset;
      cso_draw_vbo(m_cso, &info, 0, nullptr, &sc, 1);
    }
  }


  void DxvkContext::drawIndirect(VkDeviceSize offset, uint32_t count, uint32_t stride, bool unroll) {
    if (!commitGraphicsState() || !m_argBuffer.defined())
      return;

    pipe_draw_info info = { };
    info.mode = dirkPrim(m_ia.primitiveTopology());
    pipe_draw_indirect_info indirect = { };
    indirect.buffer = m_argBuffer.buffer()->resource();
    indirect.offset = uint32_t(m_argBuffer.offset() + offset);
    indirect.stride = stride;
    indirect.draw_count = count;
    pipe_draw_start_count_bias sc = { };
    cso_draw_vbo(m_cso, &info, 0, &indirect, &sc, 1);
  }


  void DxvkContext::drawIndirectCount(VkDeviceSize offset, VkDeviceSize countOffset,
      uint32_t maxCount, uint32_t stride) {
    if (!commitGraphicsState() || !m_argBuffer.defined() || !m_cntBuffer.defined())
      return;

    pipe_draw_info info = { };
    info.mode = dirkPrim(m_ia.primitiveTopology());
    pipe_draw_indirect_info indirect = { };
    indirect.buffer = m_argBuffer.buffer()->resource();
    indirect.offset = uint32_t(m_argBuffer.offset() + offset);
    indirect.stride = stride;
    indirect.draw_count = maxCount;
    indirect.indirect_draw_count = m_cntBuffer.buffer()->resource();
    indirect.indirect_draw_count_offset = uint32_t(m_cntBuffer.offset() + countOffset);
    pipe_draw_start_count_bias sc = { };
    cso_draw_vbo(m_cso, &info, 0, &indirect, &sc, 1);
  }


  void DxvkContext::drawIndexedIndirect(VkDeviceSize offset, uint32_t count, uint32_t stride, bool unroll) {
    if (!commitGraphicsState() || !m_argBuffer.defined() || !m_indexBuffer.defined())
      return;

    const uint32_t indexSize = m_indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    pipe_draw_info info = { };
    info.mode = dirkPrim(m_ia.primitiveTopology());
    info.index_size = indexSize;
    info.index.resource = m_indexBuffer.buffer()->resource();
    info.primitive_restart = m_ia.primitiveRestart();
    info.restart_index = indexSize == 2 ? 0xffffu : 0xffffffffu;
    pipe_draw_indirect_info indirect = { };
    indirect.buffer = m_argBuffer.buffer()->resource();
    indirect.offset = uint32_t(m_argBuffer.offset() + offset);
    indirect.stride = stride;
    indirect.draw_count = count;
    pipe_draw_start_count_bias sc = { };
    sc.start = uint32_t(m_indexBuffer.offset() / indexSize);
    cso_draw_vbo(m_cso, &info, 0, &indirect, &sc, 1);
  }


  void DxvkContext::drawIndexedIndirectCount(VkDeviceSize offset, VkDeviceSize countOffset,
      uint32_t maxCount, uint32_t stride) {
    if (!commitGraphicsState() || !m_argBuffer.defined() || !m_cntBuffer.defined() || !m_indexBuffer.defined())
      return;

    const uint32_t indexSize = m_indexType == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    pipe_draw_info info = { };
    info.mode = dirkPrim(m_ia.primitiveTopology());
    info.index_size = indexSize;
    info.index.resource = m_indexBuffer.buffer()->resource();
    info.primitive_restart = m_ia.primitiveRestart();
    info.restart_index = indexSize == 2 ? 0xffffu : 0xffffffffu;
    pipe_draw_indirect_info indirect = { };
    indirect.buffer = m_argBuffer.buffer()->resource();
    indirect.offset = uint32_t(m_argBuffer.offset() + offset);
    indirect.stride = stride;
    indirect.draw_count = maxCount;
    indirect.indirect_draw_count = m_cntBuffer.buffer()->resource();
    indirect.indirect_draw_count_offset = uint32_t(m_cntBuffer.offset() + countOffset);
    pipe_draw_start_count_bias sc = { };
    sc.start = uint32_t(m_indexBuffer.offset() / indexSize);
    cso_draw_vbo(m_cso, &info, 0, &indirect, &sc, 1);
  }


  void DxvkContext::drawIndirectXfb(VkDeviceSize counterOffset, uint32_t counterDivisor, uint32_t counterBias) {
    static bool said;
    if (!std::exchange(said, true))
      Logger::warn("dirk: DrawAuto is not implemented");
  }


  void DxvkContext::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    if (!commitComputeState())
      return;

    pipe_grid_info grid = { };
    grid.block[0] = m_csShader->layout.workgroup[0];
    grid.block[1] = m_csShader->layout.workgroup[1];
    grid.block[2] = m_csShader->layout.workgroup[2];
    grid.grid[0] = x;
    grid.grid[1] = y;
    grid.grid[2] = z;
    m_pipe->launch_grid(m_pipe, &grid);
  }


  void DxvkContext::dispatchIndirect(VkDeviceSize offset) {
    if (!commitComputeState() || !m_argBuffer.defined())
      return;

    pipe_grid_info grid = { };
    grid.block[0] = m_csShader->layout.workgroup[0];
    grid.block[1] = m_csShader->layout.workgroup[1];
    grid.block[2] = m_csShader->layout.workgroup[2];
    grid.indirect = m_argBuffer.buffer()->resource();
    grid.indirect_offset = uint32_t(m_argBuffer.offset() + offset);
    m_pipe->launch_grid(m_pipe, &grid);
  }

  // ---------------------------------------------------------------- clears

  static void dirkClearColor(VkFormat format, const VkClearValue& value, pipe_color_union& color) {
    std::memcpy(color.ui, value.color.uint32, sizeof(color.ui));
  }


  void DxvkContext::clearRenderTarget(
    const Rc<DxvkImageView>&    imageView,
          VkImageAspectFlags    clearAspects,
          VkClearValue          clearValue,
          VkImageAspectFlags    discardAspects) {
    if (!clearAspects)
      return;
    VkExtent3D extent = imageView->mipLevelExtent(0);
    clearImageView(imageView, VkOffset3D { 0, 0, 0 }, extent, clearAspects, clearValue);
  }


  void DxvkContext::clearImageView(
    const Rc<DxvkImageView>&    imageView,
          VkOffset3D            offset,
          VkExtent3D            extent,
          VkImageAspectFlags    aspect,
          VkClearValue          value) {
    flushHostWrites();
    pipe_surface surf = dirkSurface(imageView);

    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      pipe_color_union color;
      dirkClearColor(imageView->info().format, value, color);
      m_pipe->clear_render_target(m_pipe, &surf, &color,
        offset.x, offset.y, extent.width, extent.height, false);
    } else {
      unsigned flags = 0;
      if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        flags |= PIPE_CLEAR_DEPTH;
      if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
        flags |= PIPE_CLEAR_STENCIL;
      m_pipe->clear_depth_stencil(m_pipe, &surf, flags,
        value.depthStencil.depth, value.depthStencil.stencil,
        offset.x, offset.y, extent.width, extent.height, false);
    }
  }


  void DxvkContext::clearBuffer(
    const Rc<DxvkBuffer>&       buffer,
          VkDeviceSize          offset,
          VkDeviceSize          length,
          uint32_t              value) {
    if (buffer->mapPtr(0) && !buffer->isInUse(DxvkAccess::Write)) {
      auto dst = reinterpret_cast<uint32_t*>(buffer->mapPtr(offset));
      for (VkDeviceSize i = 0; i < length / 4; i++)
        dst[i] = value;
      markHostWritten(buffer, offset, length);
      return;
    }
    flushHostWrites();
    m_pipe->clear_buffer(m_pipe, buffer->resource(), unsigned(offset), unsigned(length), &value, 4);
    if (buffer->mapPtr(0))
      buffer->markHostStale(true);
  }


  void DxvkContext::clearBufferView(
    const Rc<DxvkBufferView>&   bufferView,
          VkDeviceSize          offset,
          VkDeviceSize          length,
          VkClearColorValue     value) {
    const auto key = bufferView->info();
    const pipe_format format = vk_format_to_pipe_format(key.format);
    const unsigned size = util_format_get_blocksize(format);
    if (!size)
      return;

    union { uint32_t u[4]; uint8_t b[16]; } packed = { };
    util_format_pack_rgba(format, packed.b, value.uint32, 1);

    flushHostWrites();
    m_pipe->clear_buffer(m_pipe, bufferView->buffer()->resource(),
      unsigned(key.offset + offset * size), unsigned(length * size), packed.b, int(size));
    if (bufferView->buffer()->mapPtr(0))
      bufferView->buffer()->markHostStale(true);
  }

  // ---------------------------------------------------------------- copies

  static pipe_box dirkBox(VkOffset3D offset, VkExtent3D extent, uint32_t layer, const DxvkImage* image) {
    pipe_box box;
    if (image->info().type == VK_IMAGE_TYPE_3D)
      u_box_3d(offset.x, offset.y, offset.z, extent.width, extent.height, extent.depth, &box);
    else
      u_box_3d(offset.x, offset.y, layer, extent.width, extent.height, 1, &box);
    return box;
  }


  void DxvkContext::copyBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkDeviceSize          numBytes) {
    if (!numBytes)
      return;

    // A source the host holds the latest copy of is copied from the host.
    const bool srcHost = srcBuffer->mapPtr(0) && !srcBuffer->isInUse(DxvkAccess::Write);
    if (srcHost && dstBuffer->mapPtr(0) && !dstBuffer->isInUse(DxvkAccess::Write)) {
      std::memmove(dstBuffer->mapPtr(dstOffset), srcBuffer->mapPtr(srcOffset), numBytes);
      markHostWritten(dstBuffer, dstOffset, numBytes);
      return;
    }

    flushHostWrites();
    if (srcHost) {
      m_pipe->buffer_subdata(m_pipe, dstBuffer->resource(), PIPE_MAP_WRITE,
        unsigned(dstOffset), unsigned(numBytes), srcBuffer->mapPtr(srcOffset));
    } else {
      pipe_box box;
      u_box_1d(int(srcOffset), int(numBytes), &box);
      m_pipe->resource_copy_region(m_pipe, dstBuffer->resource(), 0,
        unsigned(dstOffset), 0, 0, srcBuffer->resource(), 0, &box);
    }
    if (dstBuffer->mapPtr(0))
      dstBuffer->markHostStale(true);
  }


  void DxvkContext::copyBufferRegion(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
          VkDeviceSize          srcOffset,
          VkDeviceSize          numBytes) {
    copyBuffer(dstBuffer, dstOffset, dstBuffer, srcOffset, numBytes);
  }


  void DxvkContext::copyBufferToImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
          VkExtent3D            dstExtent,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset,
          VkDeviceSize          rowAlignment,
          VkDeviceSize          sliceAlignment,
          VkFormat              srcFormat) {
    if (srcBuffer->isInUse(DxvkAccess::Write))
      m_device->waitForResource(*srcBuffer, DxvkAccess::Write);
    if (!srcBuffer->mapPtr(0)) {
      static bool said;
      if (!std::exchange(said, true))
        Logger::warn("dirk: a copy from a device-local buffer to an image is not implemented");
      return;
    }

    flushHostWrites();

    const DxvkFormatInfo* formatInfo = dstImage->formatInfo();
    const VkDeviceSize elementSize = formatInfo->elementSize;
    VkExtent3D blockCount = util::computeBlockCount(dstExtent, formatInfo->blockSize);
    VkDeviceSize rowPitch = blockCount.width * elementSize;
    if (rowAlignment > elementSize)
      rowPitch = rowAlignment >= rowPitch ? rowAlignment : align(rowPitch, rowAlignment);
    VkDeviceSize slicePitch = blockCount.height * rowPitch;
    if (dstImage->info().type == VK_IMAGE_TYPE_3D && sliceAlignment > elementSize)
      slicePitch = sliceAlignment >= slicePitch ? sliceAlignment : align(slicePitch, sliceAlignment);

    VkDeviceSize offset = srcOffset;
    for (uint32_t i = 0; i < dstSubresource.layerCount; i++) {
      pipe_box box = dirkBox(dstOffset, dstExtent, dstSubresource.baseArrayLayer + i, dstImage.ptr());
      m_pipe->texture_subdata(m_pipe, dstImage->resource(), dstSubresource.mipLevel,
        PIPE_MAP_WRITE, &box, srcBuffer->mapPtr(offset), unsigned(rowPitch), unsigned(slicePitch));
      offset += blockCount.depth * slicePitch;
    }
  }


  void DxvkContext::copyImageToBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
          VkDeviceSize          rowAlignment,
          VkDeviceSize          sliceAlignment,
          VkFormat              dstFormat,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            srcExtent) {
    if (!dstBuffer->mapPtr(0)) {
      static bool said;
      if (!std::exchange(said, true))
        Logger::warn("dirk: a copy from an image to a device-local buffer is not implemented");
      return;
    }

    // Read back now: the host copy is what the buffer is mapped through.
    flushHostWrites();

    const DxvkFormatInfo* formatInfo = srcImage->formatInfo();
    const VkDeviceSize elementSize = formatInfo->elementSize;
    VkExtent3D blockCount = util::computeBlockCount(srcExtent, formatInfo->blockSize);
    VkDeviceSize rowPitch = blockCount.width * elementSize;
    if (rowAlignment > elementSize)
      rowPitch = rowAlignment >= rowPitch ? rowAlignment : align(rowPitch, rowAlignment);
    VkDeviceSize slicePitch = blockCount.height * rowPitch;
    if (srcImage->info().type == VK_IMAGE_TYPE_3D && sliceAlignment > elementSize)
      slicePitch = sliceAlignment >= slicePitch ? sliceAlignment : align(slicePitch, sliceAlignment);

    VkDeviceSize offset = dstOffset;
    for (uint32_t i = 0; i < srcSubresource.layerCount; i++) {
      pipe_box box = dirkBox(srcOffset, srcExtent, srcSubresource.baseArrayLayer + i, srcImage.ptr());
      pipe_transfer* transfer = nullptr;
      auto src = reinterpret_cast<const uint8_t*>(m_pipe->texture_map(m_pipe, srcImage->resource(),
        srcSubresource.mipLevel, PIPE_MAP_READ, &box, &transfer));
      if (src) {
        auto dst = reinterpret_cast<uint8_t*>(dstBuffer->mapPtr(offset));
        const VkDeviceSize rowBytes = blockCount.width * elementSize;
        for (uint32_t z = 0; z < blockCount.depth; z++) {
          for (uint32_t y = 0; y < blockCount.height; y++) {
            std::memcpy(dst + z * slicePitch + y * rowPitch,
              src + z * transfer->layer_stride + y * transfer->stride, rowBytes);
          }
        }
        m_pipe->texture_unmap(m_pipe, transfer);
      }
      offset += blockCount.depth * slicePitch;
    }

    // The host copy is now ahead of the GPU copy of the buffer.
    dstBuffer->markWritten(dstOffset, offset - dstOffset);
    queueHostWrites(dstBuffer.ptr());
  }


  void DxvkContext::copyImage(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
    const Rc<DxvkImage>&        srcImage,
          VkImageSubresourceLayers srcSubresource,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    flushHostWrites();
    for (uint32_t i = 0; i < dstSubresource.layerCount; i++) {
      pipe_box box = dirkBox(srcOffset, extent, srcSubresource.baseArrayLayer + i, srcImage.ptr());
      const uint32_t dz = dstImage->info().type == VK_IMAGE_TYPE_3D
        ? uint32_t(dstOffset.z) : dstSubresource.baseArrayLayer + i;
      m_pipe->resource_copy_region(m_pipe, dstImage->resource(), dstSubresource.mipLevel,
        dstOffset.x, dstOffset.y, dz, srcImage->resource(), srcSubresource.mipLevel, &box);
    }
  }


  void DxvkContext::copyImageRegion(
    const Rc<DxvkImage>&        dstImage,
          VkImageSubresourceLayers dstSubresource,
          VkOffset3D            dstOffset,
          VkOffset3D            srcOffset,
          VkExtent3D            extent) {
    copyImage(dstImage, dstSubresource, dstOffset, dstImage, dstSubresource, srcOffset, extent);
  }


  void DxvkContext::copyPackedBufferImage(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstBufferOffset,
          VkOffset3D            dstOffset,
          VkExtent3D            dstSize,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcBufferOffset,
          VkOffset3D            srcOffset,
          VkExtent3D            srcSize,
          VkExtent3D            extent,
          VkDeviceSize          elementSize) {
    // Buffer-backed images: a 3D copy between two buffers, row by row.
    for (uint32_t z = 0; z < extent.depth; z++) {
      for (uint32_t y = 0; y < extent.height; y++) {
        VkDeviceSize s = srcBufferOffset + elementSize * (srcOffset.x
          + srcSize.width * (srcOffset.y + y + srcSize.height * (srcOffset.z + z)));
        VkDeviceSize d = dstBufferOffset + elementSize * (dstOffset.x
          + dstSize.width * (dstOffset.y + y + dstSize.height * (dstOffset.z + z)));
        copyBuffer(dstBuffer, d, srcBuffer, s, elementSize * extent.width);
      }
    }
  }


  void DxvkContext::copySparsePagesToBuffer(
    const Rc<DxvkBuffer>&       dstBuffer,
          VkDeviceSize          dstOffset,
    const Rc<DxvkPagedResource>& srcResource,
          uint32_t              pageCount,
    const uint32_t*             pages) { }


  void DxvkContext::copySparsePagesFromBuffer(
    const Rc<DxvkPagedResource>& dstResource,
          uint32_t              pageCount,
    const uint32_t*             pages,
    const Rc<DxvkBuffer>&       srcBuffer,
          VkDeviceSize          srcOffset) { }


  void DxvkContext::discardImage(const Rc<DxvkImage>& image) { }


  void DxvkContext::blitImageView(
    const Rc<DxvkImageView>&    dstView,
    const VkOffset3D*           dstOffsets,
    const Rc<DxvkImageView>&    srcView,
    const VkOffset3D*           srcOffsets,
          VkFilter              filter) {
    flushHostWrites();
    pipe_blit_info blit = { };
    const auto dk = dstView->info();
    const auto sk = srcView->info();
    blit.dst.resource = dstView->image()->resource();
    blit.dst.level = dk.mipIndex;
    blit.dst.format = vk_format_to_pipe_format(dk.format);
    blit.src.resource = srcView->image()->resource();
    blit.src.level = sk.mipIndex;
    blit.src.format = vk_format_to_pipe_format(sk.format);
    u_box_3d(dstOffsets[0].x, dstOffsets[0].y, dk.layerIndex,
      dstOffsets[1].x - dstOffsets[0].x, dstOffsets[1].y - dstOffsets[0].y, dk.layerCount, &blit.dst.box);
    u_box_3d(srcOffsets[0].x, srcOffsets[0].y, sk.layerIndex,
      srcOffsets[1].x - srcOffsets[0].x, srcOffsets[1].y - srcOffsets[0].y, sk.layerCount, &blit.src.box);
    blit.mask = PIPE_MASK_RGBA;
    blit.filter = filter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
    m_pipe->blit(m_pipe, &blit);
  }


  void DxvkContext::resolveImage(
    const Rc<DxvkImage>&            dstImage,
    const Rc<DxvkImage>&            srcImage,
    const VkImageResolve&           region,
          VkFormat                  format,
          VkResolveModeFlagBits     mode,
          VkResolveModeFlagBits     stencilMode) {
    flushHostWrites();
    pipe_blit_info blit = { };
    const pipe_format pf = vk_format_to_pipe_format(format ? format : dstImage->info().format);
    blit.dst.resource = dstImage->resource();
    blit.dst.level = region.dstSubresource.mipLevel;
    blit.dst.format = pf;
    blit.src.resource = srcImage->resource();
    blit.src.level = region.srcSubresource.mipLevel;
    blit.src.format = pf;
    u_box_3d(region.dstOffset.x, region.dstOffset.y, region.dstSubresource.baseArrayLayer,
      region.extent.width, region.extent.height, region.dstSubresource.layerCount, &blit.dst.box);
    u_box_3d(region.srcOffset.x, region.srcOffset.y, region.srcSubresource.baseArrayLayer,
      region.extent.width, region.extent.height, region.srcSubresource.layerCount, &blit.src.box);
    blit.mask = util_format_is_depth_or_stencil(pf)
      ? (util_format_has_depth(util_format_description(pf)) ? PIPE_MASK_Z : 0)
      | (util_format_has_stencil(util_format_description(pf)) ? PIPE_MASK_S : 0)
      : PIPE_MASK_RGBA;
    blit.filter = PIPE_TEX_FILTER_NEAREST;
    m_pipe->blit(m_pipe, &blit);
  }


  void DxvkContext::generateMipmaps(const Rc<DxvkImageView>& imageView, VkFilter filter) {
    flushHostWrites();
    const auto key = imageView->info();
    pipe_resource* res = imageView->image()->resource();
    const pipe_format pf = vk_format_to_pipe_format(key.format);

    for (uint32_t m = key.mipIndex + 1; m < uint32_t(key.mipIndex + key.mipCount); m++) {
      VkExtent3D src = imageView->image()->mipLevelExtent(m - 1);
      VkExtent3D dst = imageView->image()->mipLevelExtent(m);
      pipe_blit_info blit = { };
      blit.dst.resource = res;
      blit.dst.level = m;
      blit.dst.format = pf;
      blit.src.resource = res;
      blit.src.level = m - 1;
      blit.src.format = pf;
      const bool is3d = imageView->image()->info().type == VK_IMAGE_TYPE_3D;
      u_box_3d(0, 0, is3d ? 0 : key.layerIndex, dst.width, dst.height,
        is3d ? dst.depth : key.layerCount, &blit.dst.box);
      u_box_3d(0, 0, is3d ? 0 : key.layerIndex, src.width, src.height,
        is3d ? src.depth : key.layerCount, &blit.src.box);
      blit.mask = PIPE_MASK_RGBA;
      blit.filter = filter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
      m_pipe->blit(m_pipe, &blit);
    }
  }


  void DxvkContext::initBuffer(const Rc<DxvkBuffer>& buffer) {
    // WebGPU zeroes new buffers, and the host copy starts zeroed.
  }


  void DxvkContext::initImage(const Rc<DxvkImage>& image, VkImageLayout initialLayout) {
    // WebGPU zeroes new textures.
  }


  void DxvkContext::initSparseImage(const Rc<DxvkImage>& image) { }


  void DxvkContext::emitGraphicsBarrier(VkPipelineStageFlags, VkAccessFlags,
    VkPipelineStageFlags, VkAccessFlags) { }

  void DxvkContext::emitBufferBarrier(const Rc<DxvkBuffer>&, VkPipelineStageFlags,
    VkAccessFlags, VkPipelineStageFlags, VkAccessFlags) { }

  void DxvkContext::emitImageBarrier(const Rc<DxvkImage>&, VkImageLayout,
    VkPipelineStageFlags, VkAccessFlags, VkImageLayout, VkPipelineStageFlags, VkAccessFlags) { }

  void DxvkContext::changeImageLayout(const Rc<DxvkImage>& image, VkImageLayout layout) { }

  void DxvkContext::transformImage(const Rc<DxvkImage>&, const VkImageSubresourceRange&,
    VkImageLayout, VkImageLayout) { }


  void DxvkContext::updateBuffer(
    const Rc<DxvkBuffer>&           buffer,
          VkDeviceSize              offset,
          VkDeviceSize              size,
    const void*                     data) {
    if (buffer->mapPtr(0) && !buffer->isInUse(DxvkAccess::Write)) {
      std::memcpy(buffer->mapPtr(offset), data, size);
      markHostWritten(buffer, offset, size);
      return;
    }
    flushHostWrites();
    m_pipe->buffer_subdata(m_pipe, buffer->resource(), PIPE_MAP_WRITE,
      unsigned(offset), unsigned(size), data);
    if (buffer->mapPtr(0))
      buffer->markHostStale(true);
  }


  void DxvkContext::uploadBuffer(
    const Rc<DxvkBuffer>&           buffer,
    const Rc<DxvkBuffer>&           source,
          VkDeviceSize              sourceOffset) {
    copyBuffer(buffer, 0, source, sourceOffset, buffer->info().size);
  }


  void DxvkContext::uploadImage(
    const Rc<DxvkImage>&            image,
    const Rc<DxvkBuffer>&           source,
          VkDeviceSize              sourceOffset,
          VkDeviceSize              subresourceAlignment,
          VkFormat                  format) {
    VkDeviceSize offset = sourceOffset;
    for (uint32_t m = 0; m < image->info().mipLevels; m++) {
      VkExtent3D mipExtent = image->mipLevelExtent(m);
      VkDeviceSize mipSize = util::computeImageDataSize(
        image->info().format, mipExtent, image->formatInfo()->aspectMask);

      for (uint32_t l = 0; l < image->info().numLayers; l++) {
        VkImageSubresourceLayers layers = { };
        layers.aspectMask = image->formatInfo()->aspectMask;
        layers.mipLevel = m;
        layers.baseArrayLayer = l;
        layers.layerCount = 1;
        copyBufferToImage(image, layers, VkOffset3D { 0, 0, 0 }, mipExtent,
          source, offset, 0, 0, format);
        offset += align(mipSize, subresourceAlignment);
      }
    }
  }

  // ---------------------------------------------------------------- present

  void DxvkContext::presentImage(
    const Rc<DxvkImageView>&  view,
          void*               surface,
          uint32_t            syncInterval) {
    const dirk_host* host = dirk_get_host();
    if (!host || !host->present)
      return;

    flushHostWrites();

    pipe_resource* image = view->image()->resource();
    const VkExtent3D extent = view->mipLevelExtent(0);
    pipe_format want = host->surface_format
      ? pipe_format(host->surface_format(surface)) : PIPE_FORMAT_NONE;
    if (want == PIPE_FORMAT_NONE)
      want = image->format;

    // The host's surface may want another format, and a multisampled back
    // buffer is resolved: a blit to a texture of the surface's own.
    pipe_resource* out = image;
    if (want != image->format || image->nr_samples > 1) {
      auto& entry = m_presentImages[surface];
      if (!entry.resource || entry.resource->width0 != extent.width
       || entry.resource->height0 != extent.height || entry.resource->format != want) {
        pipe_resource_reference(&entry.resource, nullptr);
        pipe_resource templ = { };
        templ.target = PIPE_TEXTURE_2D;
        templ.format = want;
        templ.width0 = extent.width;
        templ.height0 = extent.height;
        templ.depth0 = 1;
        templ.array_size = 1;
        templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_DISPLAY_TARGET;
        templ.usage = PIPE_USAGE_DEFAULT;
        entry.resource = m_device->screen()->resource_create(m_device->screen(), &templ);
      }

      if (entry.resource) {
        pipe_blit_info blit = { };
        blit.dst.resource = entry.resource;
        blit.dst.format = want;
        blit.src.resource = image;
        blit.src.format = image->format;
        u_box_2d(0, 0, extent.width, extent.height, &blit.dst.box);
        u_box_2d(0, 0, extent.width, extent.height, &blit.src.box);
        blit.mask = PIPE_MASK_RGBA;
        blit.filter = PIPE_TEX_FILTER_NEAREST;
        m_pipe->blit(m_pipe, &blit);
        out = entry.resource;
      }
    }

    m_pipe->flush(m_pipe, nullptr, PIPE_FLUSH_END_OF_FRAME);
    signalPending();
    host->present(surface, out, extent.width, extent.height, syncInterval);
  }

}
