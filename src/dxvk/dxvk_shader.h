#pragma once

#include <unordered_map>
#include <vector>

#include "dxvk_hash.h"
#include "dxvk_include.h"
#include "dxvk_limits.h"
#include "dxvk_pipelayout.h"
#include "dxvk_shader_key.h"

#include "../spirv/spirv_code_buffer.h"
#include "../spirv/spirv_compression.h"
#include "../spirv/spirv_module.h"

namespace dxvk {

  class DxvkShader;

  /** \brief Frees a gallium shader the context made (dirk_context.cpp) */
  void dirkDestroyPipeShader(void* shader);

  enum DxvkShaderFlag : uint64_t {
    HasSampleRateShading,
    HasTransformFeedback,
    ExportsPosition,
    ExportsStencilRef,
    ExportsViewportIndexLayerFromVertexStage,
    ExportsSampleMask,
    UsesFragmentCoverage,
    UsesSparseResidency,
    TessellationPoints,
  };

  using DxvkShaderFlags = Flags<DxvkShaderFlag>;

  struct DxvkShaderCreateInfo {
    VkShaderStageFlagBits stage;
    uint32_t bindingCount = 0;
    const DxvkBindingInfo* bindings = nullptr;
    uint32_t inputMask = 0;
    uint32_t outputMask = 0;
    uint32_t flatShadingInputs = 0;
    VkShaderStageFlags pushConstStages = 0;
    uint32_t pushConstSize = 0;
    int32_t xfbRasterizedStream = 0;
    uint32_t patchVertexCount = 0;
    uint32_t xfbStrides[MaxNumXfbBuffers] = { };
    VkPrimitiveTopology inputTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    VkPrimitiveTopology outputTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  };

  /**
   * \brief What a shader is compiled for, besides its code
   *
   * The state of a draw that changes the module: DXVK's patches of its
   * SPIR-V for dual-source blending, flat shading, fragment inputs the
   * stage before does not write, and render target swizzles.
   */
  struct DxvkShaderModuleCreateInfo {
    bool      fsDualSrcBlend  = false;
    bool      fsFlatShading   = false;
    uint32_t  undefinedInputs = 0;
    VkPrimitiveTopology inputTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    std::array<VkComponentMapping, MaxNumRenderTargets> rtSwizzles = { };
    /// Specialization constants the shader reads, by DXVK's index
    uint32_t  specMask = 0u;
    std::array<uint32_t, MaxNumSpecConstants> spec = { };

    bool eq(const DxvkShaderModuleCreateInfo& other) const;

    size_t hash() const;
  };


  /**
   * \brief Shader
   *
   * DXVK's SPIR-V of a D3D shader, its bindings (DXVK's resource slots) and
   * what it reads and writes. The gallium shaders made of it, one for each
   * module state it is drawn with, are kept by it (\c variant), made and
   * deleted on the device's thread.
   */
  class DxvkShader : public RcObject {

  public:

    DxvkShader(
      const DxvkShaderCreateInfo&   info,
            SpirvCodeBuffer&&       spirv);

    ~DxvkShader();

    const DxvkShaderCreateInfo& info() const {
      return m_info;
    }

    DxvkShaderFlags flags() const {
      return m_flags;
    }

    const DxvkBindingLayout& getBindings() const {
      return m_bindings;
    }

    uint32_t getSpecConstantMask() const {
      return m_specConstantMask;
    }

    bool needsLibraryCompile() const {
      return false;
    }

    void notifyLibraryCompile() { }

    SpirvCodeBuffer getRawCode() const {
      return m_code.decompress();
    }

    /**
     * \brief The SPIR-V for one module state
     *
     * Bindings stay DXVK's resource slots; the gallium side maps them.
     */
    SpirvCodeBuffer getCode(
      const DxvkShaderModuleCreateInfo& state) const;

    void dump(std::ostream& outputStream) const;

    void setShaderKey(const DxvkShaderKey& key) {
      m_key = key;
      m_hash = key.hash();
    }

    DxvkShaderKey getShaderKey() const {
      return m_key;
    }

    size_t getHash() const {
      return m_hash;
    }

    std::string debugName() const {
      return m_key.toString();
    }

    static size_t getHash(const Rc<DxvkShader>& shader) {
      return shader != nullptr ? shader->getHash() : 0;
    }

    /**
     * \brief The gallium shader for one module state
     *
     * Only on the device's thread. Null until \c setVariant.
     */
    void* variant(const DxvkShaderModuleCreateInfo& state) const {
      auto entry = m_variants.find(state);
      return entry != m_variants.end() ? entry->second : nullptr;
    }

    void setVariant(const DxvkShaderModuleCreateInfo& state, void* cso) {
      m_variants.insert_or_assign(state, cso);
    }

    /** \brief Takes every gallium shader, for the device to delete */
    std::vector<void*> takeVariants() {
      std::vector<void*> result;
      for (auto& v : m_variants)
        result.push_back(v.second);
      m_variants.clear();
      return result;
    }

  private:

    DxvkShaderCreateInfo          m_info;
    SpirvCompressedBuffer         m_code;

    DxvkShaderFlags               m_flags;
    DxvkShaderKey                 m_key;
    size_t                        m_hash = 0;

    size_t                        m_o1IdxOffset = 0;
    size_t                        m_o1LocOffset = 0;

    uint32_t                      m_specConstantMask = 0;

    DxvkBindingLayout             m_bindings;

    std::unordered_map<DxvkShaderModuleCreateInfo,
      void*, DxvkHash, DxvkEq>    m_variants;

    static void eliminateInput(
            SpirvCodeBuffer&          code,
            uint32_t                  location);

    static void emitOutputSwizzles(
            SpirvCodeBuffer&          code,
            uint32_t                  outputMask,
            const VkComponentMapping* swizzles);

    static void emitFlatShadingDeclarations(
            SpirvCodeBuffer&          code,
            uint32_t                  inputMask);

    static void patchInputTopology(
            SpirvCodeBuffer&          code,
            VkPrimitiveTopology       topology);

  };

}
