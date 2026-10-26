// dirk: instance, adapter, device and resources on a gallium screen.

#include <cstring>

#include "dxvk_device.h"
#include "dxvk_instance.h"

#include "../dirk/dirk.h"
#include "../wsi/wsi_platform.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_simple_shaders.h"
#include "util/format/u_format.h"
#include "cso_cache/cso_context.h"

extern "C" enum pipe_format vk_format_to_pipe_format(VkFormat vkformat);

namespace dxvk {

  static pipe_screen*     g_screen = nullptr;
  static const dirk_host* g_host   = nullptr;

  // ---------------------------------------------------------------- loader

  // Every Vulkan name resolves to null, vkGetDeviceProcAddr included, which
  // resolves to this same function so that device tables are null too.
  static PFN_vkVoidFunction VKAPI_CALL dirkNullDeviceProc(VkDevice, const char*) {
    return nullptr;
  }

  static PFN_vkVoidFunction VKAPI_CALL dirkNullInstanceProc(VkInstance, const char* name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
      return reinterpret_cast<PFN_vkVoidFunction>(&dirkNullDeviceProc);
    return nullptr;
  }

  PFN_vkGetInstanceProcAddr DxvkInstance::nullLoader() {
    return &dirkNullInstanceProc;
  }

  // ---------------------------------------------------------------- instance

  DxvkInstance::DxvkInstance(DxvkInstanceFlags flags) {
    wsi::init();

    m_config = Config::getUserConfig();
    m_config.merge(Config::getAppConfig(env::getExePath()));
    m_options = DxvkOptions(m_config);

    m_vkl = new vk::LibraryFn(nullLoader());
    m_vki = new vk::InstanceFn(m_vkl, false, VK_NULL_HANDLE);
    m_extensions = DxvkInstanceExtensions();

    if (g_screen)
      m_adapters.push_back(new DxvkAdapter(m_vki, g_screen));
    else
      Logger::err("dirk: no gallium screen registered");
  }


  DxvkInstance::~DxvkInstance() {
    wsi::quit();
  }


  Rc<DxvkAdapter> DxvkInstance::findAdapterByLuid(const void* luid) const {
    for (const auto& adapter : m_adapters) {
      const auto& vk11 = adapter->devicePropertiesExt().vk11;
      if (vk11.deviceLUIDValid && !std::memcmp(luid, vk11.deviceLUID, VK_LUID_SIZE))
        return adapter;
    }
    return nullptr;
  }


  Rc<DxvkAdapter> DxvkInstance::findAdapterByDeviceId(uint16_t vendorId, uint16_t deviceId) const {
    for (const auto& adapter : m_adapters) {
      const auto& props = adapter->deviceProperties();
      if (props.vendorID == vendorId && props.deviceID == deviceId)
        return adapter;
    }
    return nullptr;
  }

  // ---------------------------------------------------------------- adapter

  DxvkAdapter::DxvkAdapter(const Rc<vk::InstanceFn>& vki, pipe_screen* screen)
  : m_vki(vki), m_screen(screen) {
    queryDeviceInfo();
    queryDeviceFeatures();
  }


  DxvkAdapter::~DxvkAdapter() { }


  void DxvkAdapter::queryDeviceInfo() {
    const pipe_caps& caps = m_screen->caps;
    const pipe_shader_caps& fs = m_screen->shader_caps[MESA_SHADER_FRAGMENT];

    m_deviceInfo = DxvkDeviceInfo();
    m_deviceInfo.core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    auto& p = m_deviceInfo.core.properties;
    p.apiVersion    = VK_MAKE_API_VERSION(0, 1, 3, 0);
    p.driverVersion = VK_MAKE_VERSION(1, 0, 0);
    p.vendorID      = uint32_t(DxvkGpuVendor::Amd);
    p.deviceID      = 0x73bf;
    p.deviceType    = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    std::snprintf(p.deviceName, sizeof(p.deviceName), "dirk (WebGPU)");

    auto& l = p.limits;
    const uint32_t tex2d = std::max(caps.max_texture_2d_size, 4096u);
    l.maxImageDimension1D             = tex2d;
    l.maxImageDimension2D             = tex2d;
    l.maxImageDimension3D             = 1u << std::max(caps.max_texture_3d_levels, 1u) >> 1;
    l.maxImageDimensionCube           = tex2d;
    l.maxImageArrayLayers             = std::max(caps.max_texture_array_layers, 1u);
    l.maxTexelBufferElements          = std::max(caps.max_texel_buffer_elements, 65536u);
    l.maxUniformBufferRange           = 65536;
    l.maxStorageBufferRange           = std::max(caps.max_shader_buffer_size, 1u << 27);
    l.maxPushConstantsSize            = MaxPushConstantSize;
    l.maxMemoryAllocationCount        = 1u << 20;
    l.maxSamplerAllocationCount       = 1u << 20;
    l.bufferImageGranularity          = 1;
    l.maxBoundDescriptorSets          = 4;
    l.maxPerStageDescriptorSamplers   = fs.max_texture_samplers;
    l.maxPerStageDescriptorUniformBuffers = fs.max_const_buffers;
    l.maxPerStageDescriptorStorageBuffers = fs.max_shader_buffers;
    l.maxPerStageDescriptorSampledImages  = fs.max_sampler_views;
    l.maxPerStageDescriptorStorageImages  = fs.max_shader_images;
    l.maxPerStageResources            = 512;
    l.maxDescriptorSetSamplers        = 4096;
    l.maxDescriptorSetUniformBuffers  = 4096;
    l.maxDescriptorSetUniformBuffersDynamic = 64;
    l.maxDescriptorSetStorageBuffers  = 4096;
    l.maxDescriptorSetStorageBuffersDynamic = 64;
    l.maxDescriptorSetSampledImages   = 4096;
    l.maxDescriptorSetStorageImages   = 4096;
    l.maxDescriptorSetInputAttachments = 8;
    l.maxVertexInputAttributes        = 32;
    l.maxVertexInputBindings          = 32;
    l.maxVertexInputAttributeOffset   = 2047;
    l.maxVertexInputBindingStride     = 2048;
    l.maxVertexOutputComponents       = 128;
    l.maxTessellationGenerationLevel  = 64;
    l.maxTessellationPatchSize        = 32;
    l.maxGeometryShaderInvocations    = 32;
    l.maxGeometryInputComponents      = 128;
    l.maxGeometryOutputComponents     = 128;
    l.maxGeometryOutputVertices       = 1024;
    l.maxGeometryTotalOutputComponents = 1024;
    l.maxFragmentInputComponents      = 128;
    l.maxFragmentOutputAttachments    = 8;
    l.maxFragmentDualSrcAttachments   = 1;
    l.maxFragmentCombinedOutputResources = 16;
    l.maxComputeSharedMemorySize      = 32768;
    l.maxComputeWorkGroupCount[0]     = 65535;
    l.maxComputeWorkGroupCount[1]     = 65535;
    l.maxComputeWorkGroupCount[2]     = 65535;
    l.maxComputeWorkGroupInvocations  = 1024;
    l.maxComputeWorkGroupSize[0]      = 1024;
    l.maxComputeWorkGroupSize[1]      = 1024;
    l.maxComputeWorkGroupSize[2]      = 64;
    l.subPixelPrecisionBits           = 8;
    l.subTexelPrecisionBits           = 8;
    l.mipmapPrecisionBits             = 8;
    l.maxDrawIndexedIndexValue        = ~0u;
    l.maxDrawIndirectCount            = ~0u;
    l.maxSamplerLodBias               = 15.0f;
    l.maxSamplerAnisotropy            = 16.0f;
    l.maxViewports                    = std::max(caps.max_viewports, 1u);
    l.maxViewportDimensions[0]        = tex2d;
    l.maxViewportDimensions[1]        = tex2d;
    l.viewportBoundsRange[0]          = -32768.0f;
    l.viewportBoundsRange[1]          = 32767.0f;
    l.viewportSubPixelBits            = 8;
    l.minMemoryMapAlignment           = 64;
    l.minTexelBufferOffsetAlignment   = std::max(caps.texture_buffer_offset_alignment, 16u);
    l.minUniformBufferOffsetAlignment = std::max(caps.constant_buffer_offset_alignment, 16u);
    l.minStorageBufferOffsetAlignment = std::max(caps.shader_buffer_offset_alignment, 16u);
    l.minTexelOffset                  = -8;
    l.maxTexelOffset                  = 7;
    l.minTexelGatherOffset            = -32;
    l.maxTexelGatherOffset            = 31;
    l.minInterpolationOffset          = -0.5f;
    l.maxInterpolationOffset          = 0.4375f;
    l.subPixelInterpolationOffsetBits = 4;
    l.maxFramebufferWidth             = tex2d;
    l.maxFramebufferHeight            = tex2d;
    l.maxFramebufferLayers            = l.maxImageArrayLayers;
    l.framebufferColorSampleCounts    = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l.framebufferDepthSampleCounts    = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l.framebufferStencilSampleCounts  = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l.framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l.maxColorAttachments             = std::max(caps.max_render_targets, 1u);
    l.sampledImageColorSampleCounts   = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l.sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l.sampledImageDepthSampleCounts   = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    l.sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    l.storageImageSampleCounts        = VK_SAMPLE_COUNT_1_BIT;
    l.maxSampleMaskWords              = 1;
    l.timestampComputeAndGraphics     = VK_TRUE;
    l.timestampPeriod                 = 1.0f;
    l.maxClipDistances                = 8;
    l.maxCullDistances                = 8;
    l.maxCombinedClipAndCullDistances = 8;
    l.discreteQueuePriorities         = 2;
    l.pointSizeRange[0]               = 1.0f;
    l.pointSizeRange[1]               = 1.0f;
    l.lineWidthRange[0]               = 1.0f;
    l.lineWidthRange[1]               = 1.0f;
    l.pointSizeGranularity            = 1.0f;
    l.lineWidthGranularity            = 1.0f;
    l.strictLines                     = VK_FALSE;
    l.standardSampleLocations         = VK_TRUE;
    l.optimalBufferCopyOffsetAlignment = 256;
    l.optimalBufferCopyRowPitchAlignment = 256;
    l.nonCoherentAtomSize             = 64;

    m_deviceInfo.vk11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    m_deviceInfo.vk11.deviceLUIDValid = VK_TRUE;
    m_deviceInfo.vk11.deviceLUID[0] = 0x64;
    m_deviceInfo.vk11.deviceLUID[1] = 0x69;
    m_deviceInfo.vk11.deviceLUID[2] = 0x72;
    m_deviceInfo.vk11.deviceLUID[3] = 0x6b;
    m_deviceInfo.vk11.maxMultiviewViewCount = 1;
    m_deviceInfo.vk11.maxPerSetDescriptors = 4096;
    m_deviceInfo.vk11.maxMemoryAllocationSize = VkDeviceSize(1u) << 31;

    m_deviceInfo.vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    m_deviceInfo.vk12.driverID = VK_DRIVER_ID_MESA_LLVMPIPE;
    std::snprintf(m_deviceInfo.vk12.driverName, sizeof(m_deviceInfo.vk12.driverName), "dirk");
    std::snprintf(m_deviceInfo.vk12.driverInfo, sizeof(m_deviceInfo.vk12.driverInfo), "gallium on WebGPU");
    m_deviceInfo.vk12.denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
    m_deviceInfo.vk12.roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
    m_deviceInfo.vk12.filterMinmaxSingleComponentFormats = VK_FALSE;

    m_deviceInfo.vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;

    m_deviceInfo.extTransformFeedback.maxTransformFeedbackStreams = 4;
    m_deviceInfo.extTransformFeedback.maxTransformFeedbackBuffers = 4;
    m_deviceInfo.extTransformFeedback.maxTransformFeedbackBufferSize = VkDeviceSize(1u) << 31;
    m_deviceInfo.extTransformFeedback.maxTransformFeedbackStreamDataSize = 512;
    m_deviceInfo.extTransformFeedback.maxTransformFeedbackBufferDataSize = 512;
    m_deviceInfo.extTransformFeedback.maxTransformFeedbackBufferDataStride = 2048;

    m_deviceInfo.extVertexAttributeDivisor.maxVertexAttribDivisor = ~0u;
    m_deviceInfo.extRobustness2.robustStorageBufferAccessSizeAlignment = 4;
    m_deviceInfo.extRobustness2.robustUniformBufferAccessSizeAlignment = 16;
    m_deviceInfo.driverVersion = Version(1, 0, 0);
  }


  void DxvkAdapter::queryDeviceFeatures() {
    m_deviceFeatures = DxvkDeviceFeatures();
    auto& f = m_deviceFeatures.core.features;

    // What D3D11 feature level 11_0 needs. Geometry, tessellation and stream
    // output are emulated by the driver on compute.
    f.depthBiasClamp              = VK_TRUE;
    f.depthClamp                  = VK_TRUE;
    f.dualSrcBlend                = VK_TRUE;
    f.fillModeNonSolid            = VK_TRUE;
    f.fullDrawIndexUint32         = VK_TRUE;
    f.geometryShader              = VK_TRUE;
    f.imageCubeArray              = VK_TRUE;
    f.independentBlend            = VK_TRUE;
    f.multiViewport               = VK_TRUE;
    f.occlusionQueryPrecise       = VK_TRUE;
    f.sampleRateShading           = VK_TRUE;
    f.samplerAnisotropy           = VK_TRUE;
    f.shaderClipDistance          = VK_TRUE;
    f.shaderCullDistance          = VK_TRUE;
    f.shaderImageGatherExtended   = VK_TRUE;
    f.textureCompressionBC        = VK_TRUE;
    f.drawIndirectFirstInstance   = VK_TRUE;
    f.fragmentStoresAndAtomics    = VK_TRUE;
    f.tessellationShader          = VK_TRUE;
    f.robustBufferAccess          = VK_TRUE;
    f.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    f.shaderStorageImageReadWithoutFormat  = VK_TRUE;
    f.shaderStorageImageExtendedFormats    = VK_TRUE;
    f.multiDrawIndirect           = VK_TRUE;
    f.variableMultisampleRate     = VK_TRUE;

    m_deviceFeatures.vk11.shaderDrawParameters      = VK_TRUE;
    m_deviceFeatures.vk12.samplerMirrorClampToEdge  = VK_TRUE;
    m_deviceFeatures.vk12.drawIndirectCount         = VK_FALSE;
    m_deviceFeatures.vk12.timelineSemaphore         = VK_TRUE;
    m_deviceFeatures.vk12.hostQueryReset            = VK_TRUE;
    m_deviceFeatures.vk12.shaderOutputLayer         = VK_TRUE;
    m_deviceFeatures.vk12.shaderOutputViewportIndex = VK_TRUE;
    m_deviceFeatures.vk12.uniformBufferStandardLayout = VK_TRUE;
    m_deviceFeatures.vk13.shaderDemoteToHelperInvocation = VK_TRUE;
    m_deviceFeatures.vk13.dynamicRendering          = VK_TRUE;
    m_deviceFeatures.vk13.synchronization2          = VK_TRUE;
    m_deviceFeatures.vk13.maintenance4              = VK_TRUE;

    m_deviceFeatures.extTransformFeedback.transformFeedback = VK_TRUE;
    m_deviceFeatures.extTransformFeedback.geometryStreams   = VK_TRUE;
    m_deviceFeatures.extDepthClipEnable.depthClipEnable     = VK_TRUE;
    m_deviceFeatures.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor = VK_TRUE;
    m_deviceFeatures.extRobustness2.robustBufferAccess2 = VK_TRUE;
    m_deviceFeatures.extRobustness2.nullDescriptor      = VK_TRUE;
  }


  DxvkAdapterMemoryInfo DxvkAdapter::getMemoryHeapInfo() const {
    DxvkAdapterMemoryInfo info = { };
    info.heapCount = 2;
    info.heaps[0].heapFlags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    info.heaps[0].heapSize = VkDeviceSize(2u) << 30;
    info.heaps[0].memoryBudget = info.heaps[0].heapSize;
    info.heaps[0].memoryAllocated = m_memoryStats[0].allocated.load();
    info.heaps[1].heapFlags = 0;
    info.heaps[1].heapSize = VkDeviceSize(1u) << 30;
    info.heaps[1].memoryBudget = info.heaps[1].heapSize;
    info.heaps[1].memoryAllocated = m_memoryStats[1].allocated.load();
    return info;
  }


  VkPhysicalDeviceMemoryProperties DxvkAdapter::memoryProperties() const {
    VkPhysicalDeviceMemoryProperties props = { };
    props.memoryHeapCount = 2;
    props.memoryHeaps[0].size = VkDeviceSize(2u) << 30;
    props.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    props.memoryHeaps[1].size = VkDeviceSize(1u) << 30;
    props.memoryTypeCount = 2;
    props.memoryTypes[0].heapIndex = 0;
    props.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    props.memoryTypes[1].heapIndex = 1;
    props.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    return props;
  }


  static bool dirkSupported(pipe_screen* screen, pipe_format format,
      pipe_texture_target target, unsigned bind) {
    return format != PIPE_FORMAT_NONE
        && screen->is_format_supported(screen, format, target, 0, 0, bind);
  }


  DxvkFormatFeatures DxvkAdapter::getFormatFeatures(VkFormat format) const {
    DxvkFormatFeatures result = { };
    const pipe_format pf = vk_format_to_pipe_format(format);
    if (pf == PIPE_FORMAT_NONE)
      return result;

    const bool depth = util_format_is_depth_or_stencil(pf);

    if (dirkSupported(m_screen, pf, PIPE_TEXTURE_2D, PIPE_BIND_SAMPLER_VIEW)) {
      result.optimal |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT
        | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT
        | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT
        | VK_FORMAT_FEATURE_2_BLIT_SRC_BIT;
      if (!util_format_is_pure_integer(pf))
        result.optimal |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      if (depth)
        result.optimal |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
    }

    if (!depth && dirkSupported(m_screen, pf, PIPE_TEXTURE_2D, PIPE_BIND_RENDER_TARGET)) {
      result.optimal |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT
        | VK_FORMAT_FEATURE_2_BLIT_DST_BIT;
      if (!util_format_is_pure_integer(pf))
        result.optimal |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT;
    }

    if (depth && dirkSupported(m_screen, pf, PIPE_TEXTURE_2D, PIPE_BIND_DEPTH_STENCIL))
      result.optimal |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;

    if (dirkSupported(m_screen, pf, PIPE_TEXTURE_2D, PIPE_BIND_SHADER_IMAGE)) {
      result.optimal |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT
        | VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT
        | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    }

    if (dirkSupported(m_screen, pf, PIPE_BUFFER, PIPE_BIND_VERTEX_BUFFER))
      result.buffer |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;

    if (dirkSupported(m_screen, pf, PIPE_BUFFER, PIPE_BIND_SAMPLER_VIEW))
      result.buffer |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;

    if (dirkSupported(m_screen, pf, PIPE_BUFFER, PIPE_BIND_SHADER_IMAGE)) {
      result.buffer |= VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT
        | VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT
        | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    }

    // No linear images: the front end maps images through buffers.
    result.linear = 0;
    return result;
  }


  std::optional<DxvkFormatLimits> DxvkAdapter::getFormatLimits(
    const DxvkFormatQuery&          query) const {
    if (query.tiling != VK_IMAGE_TILING_OPTIMAL)
      return std::nullopt;

    DxvkFormatFeatures features = getFormatFeatures(query.format);
    VkFormatFeatureFlags2 need = 0;
    if (query.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
      need |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    if (query.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      need |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
    if (query.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      need |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (query.usage & VK_IMAGE_USAGE_STORAGE_BIT)
      need |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
    if ((features.optimal & need) != need || !features.optimal)
      return std::nullopt;

    const auto& l = m_deviceInfo.core.properties.limits;
    DxvkFormatLimits result = { };
    switch (query.type) {
      case VK_IMAGE_TYPE_1D: result.maxExtent = { l.maxImageDimension1D, 1, 1 }; break;
      case VK_IMAGE_TYPE_2D: result.maxExtent = { l.maxImageDimension2D, l.maxImageDimension2D, 1 }; break;
      default:               result.maxExtent = { l.maxImageDimension3D, l.maxImageDimension3D, l.maxImageDimension3D }; break;
    }
    result.maxMipLevels   = 15;
    result.maxArrayLayers = query.type == VK_IMAGE_TYPE_3D ? 1 : l.maxImageArrayLayers;
    result.maxResourceSize = VkDeviceSize(1u) << 31;

    // WebGPU multisamples at 4 only, and not storage or integer images.
    const pipe_format pf = vk_format_to_pipe_format(query.format);
    result.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    if (query.type == VK_IMAGE_TYPE_2D && !(query.usage & VK_IMAGE_USAGE_STORAGE_BIT)
     && m_screen->is_format_supported(m_screen, pf, PIPE_TEXTURE_2D, 4, 4,
          util_format_is_depth_or_stencil(pf) ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET))
      result.sampleCounts |= VK_SAMPLE_COUNT_4_BIT;
    return result;
  }


  bool DxvkAdapter::checkFeatureSupport(const DxvkDeviceFeatures& required) const {
    return true;
  }


  Rc<DxvkDevice> DxvkAdapter::createDevice(
    const Rc<DxvkInstance>&   instance,
          DxvkDeviceFeatures  enabledFeatures) {
    return new DxvkDevice(instance, this, m_deviceFeatures);
  }


  void DxvkAdapter::notifyMemoryStats(uint32_t heap, int64_t allocated, int64_t used) {
    if (heap < m_memoryStats.size()) {
      m_memoryStats[heap].allocated += allocated;
      m_memoryStats[heap].used += used;
    }
  }


  void DxvkAdapter::logAdapterInfo() const {
    Logger::info(str::format(m_deviceInfo.core.properties.deviceName, ":"));
  }

  // ---------------------------------------------------------------- device

  DxvkDevice::DxvkDevice(
    const Rc<DxvkInstance>&         instance,
    const Rc<DxvkAdapter>&          adapter,
    const DxvkDeviceFeatures&       features)
  : m_options     (instance->options()),
    m_instance    (instance),
    m_adapter     (adapter),
    m_debugFlags  (instance->debugFlags()),
    m_features    (features),
    m_properties  (adapter->devicePropertiesExt()) {
    m_vkd = new vk::DeviceFn(new vk::InstanceLoader(
      new vk::LibraryLoader(DxvkInstance::nullLoader()), false, VK_NULL_HANDLE),
      false, VK_NULL_HANDLE);

    m_screen = adapter->screen();
    m_pipe = m_screen->context_create(m_screen, nullptr, 0);
    if (!m_pipe)
      throw DxvkError("dirk: failed to create a gallium context");
    m_cso = cso_create_context(m_pipe, CSO_NO_USER_VERTEX_BUFFERS);
  }


  DxvkDevice::~DxvkDevice() {
    if (m_cso)
      cso_destroy_context(m_cso);
    if (m_emptyFs)
      m_pipe->delete_fs_state(m_pipe, m_emptyFs);
    if (m_pipe)
      m_pipe->destroy(m_pipe);
  }


  void* DxvkDevice::emptyFragmentShader() {
    if (!m_emptyFs)
      m_emptyFs = util_make_empty_fragment_shader(m_pipe);
    return m_emptyFs;
  }


  Rc<vk::InstanceFn> DxvkDevice::vki() const {
    return m_instance->vki();
  }


  VkSubresourceLayout DxvkDevice::queryImageSubresourceLayout(
    const DxvkImageCreateInfo&        createInfo,
    const VkImageSubresource&         subresource) {
    // No linear images, so nothing asks for a layout that means anything.
    VkSubresourceLayout layout = { };
    const DxvkFormatInfo* info = lookupFormatInfo(createInfo.format);
    VkExtent3D extent = util::computeMipLevelExtent(createInfo.extent, subresource.mipLevel);
    VkExtent3D blocks = util::computeBlockCount(extent, info->blockSize);
    layout.rowPitch   = info->elementSize * blocks.width;
    layout.depthPitch = layout.rowPitch * blocks.height;
    layout.arrayPitch = layout.depthPitch * blocks.depth;
    layout.size       = layout.arrayPitch;
    return layout;
  }


  DxvkFramebufferSize DxvkDevice::getDefaultFramebufferSize() const {
    const auto& l = m_properties.core.properties.limits;
    return DxvkFramebufferSize { l.maxFramebufferWidth, l.maxFramebufferHeight, l.maxFramebufferLayers };
  }


  VkPipelineStageFlags DxvkDevice::getShaderPipelineStages() const {
    return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
         | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT
         | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT
         | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT
         | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
         | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  }


  Rc<DxvkContext> DxvkDevice::createContext() {
    return new DxvkContext(this);
  }


  Rc<DxvkEvent> DxvkDevice::createGpuEvent() {
    return new DxvkEvent(this);
  }


  Rc<DxvkQuery> DxvkDevice::createGpuQuery(
          VkQueryType           type,
          VkQueryControlFlags   flags,
          uint32_t              index) {
    return new DxvkQuery(this, type, flags, index);
  }


  Rc<DxvkFence> DxvkDevice::createFence(const DxvkFenceCreateInfo& fenceInfo) {
    return new DxvkFence(this, fenceInfo);
  }


  Rc<DxvkBuffer> DxvkDevice::createBuffer(
    const DxvkBufferCreateInfo& createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkBuffer(this, createInfo, memoryType);
  }


  Rc<DxvkImage> DxvkDevice::createImage(
    const DxvkImageCreateInfo&  createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkImage(this, createInfo, memoryType);
  }


  Rc<DxvkSampler> DxvkDevice::createSampler(const DxvkSamplerKey& key) {
    return new DxvkSampler(key);
  }


  DxvkStatCounters DxvkDevice::getStatCounters() {
    return m_statCounters;
  }


  void DxvkDevice::waitForResource(const DxvkPagedResource& resource, DxvkAccess access) {
    if (!resource.isInUse(access))
      return;

    // The GPU wrote the resource since the host copy was last made: read it
    // back. Only buffers have host copies.
    auto buffer = dynamic_cast<const DxvkBuffer*>(&resource);
    if (buffer)
      const_cast<DxvkBuffer*>(buffer)->readBack(m_pipe);
  }


  void DxvkDevice::waitForIdle() {
    pipe_fence_handle* fence = nullptr;
    m_pipe->flush(m_pipe, &fence, 0);
    if (fence) {
      m_screen->fence_finish(m_screen, m_pipe, fence, OS_TIMEOUT_INFINITE);
      m_screen->fence_reference(m_screen, &fence, nullptr);
    }
  }

  // ---------------------------------------------------------------- memory

  std::atomic<uint64_t> DxvkPagedResource::s_cookie = { 0u };

  DxvkPagedResource::~DxvkPagedResource() { }


  DxvkResourceAllocation::DxvkResourceAllocation(VkDeviceSize size, bool mapped)
  : m_size(size) {
    if (mapped) {
      // 64-byte aligned, and a little over, so a vec4 load at the end of a
      // constant buffer never reads past the block.
      m_mapPtr = std::aligned_alloc(64, align(size + 16, 64));
      // All of it: a whole upload sends the padding up to the GPU buffer's
      // size too (DxvkBuffer::uploadHostWrites).
      if (m_mapPtr)
        std::memset(m_mapPtr, 0, align(size + 16, 64));
    }
  }


  DxvkResourceAllocation::~DxvkResourceAllocation() {
    std::free(m_mapPtr);
  }

  // ---------------------------------------------------------------- buffer

  static unsigned dirkBufferBind(VkBufferUsageFlags usage) {
    unsigned bind = 0;
    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
      bind |= PIPE_BIND_VERTEX_BUFFER;
    if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
      bind |= PIPE_BIND_INDEX_BUFFER;
    if (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
      bind |= PIPE_BIND_CONSTANT_BUFFER;
    if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
      bind |= PIPE_BIND_SHADER_BUFFER;
    if (usage & VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)
      bind |= PIPE_BIND_SAMPLER_VIEW;
    // gex_wgpu has no storage texel buffers: a typed UAV on a buffer is
    // refused when it is bound, not when the buffer is made.
    if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
      bind |= PIPE_BIND_COMMAND_ARGS_BUFFER;
    if (usage & VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT)
      bind |= PIPE_BIND_STREAM_OUTPUT;
    return bind;
  }


  DxvkBuffer::DxvkBuffer(
          DxvkDevice*           device,
    const DxvkBufferCreateInfo& createInfo,
          VkMemoryPropertyFlags memFlags)
  : m_device(device),
    m_properties(memFlags),
    m_shaderStages(util::shaderStages(createInfo.stages)),
    m_info(createInfo) {
    m_info.debugName = nullptr;

    pipe_resource templ = { };
    templ.target     = PIPE_BUFFER;
    templ.format     = PIPE_FORMAT_R8_UNORM;
    templ.width0     = uint32_t(align(m_info.size, 16));
    templ.height0    = 1;
    templ.depth0     = 1;
    templ.array_size = 1;
    templ.bind       = dirkBufferBind(m_info.usage);
    templ.usage      = PIPE_USAGE_DEFAULT;
    m_resource = device->screen()->resource_create(device->screen(), &templ);
    if (!m_resource)
      throw DxvkError(str::format("dirk: failed to create a buffer of ", m_info.size, " bytes"));

    assignStorage(allocateStorage());
  }


  DxvkBuffer::~DxvkBuffer() {
    for (auto& v : m_views)
      v.second.releaseGallium();
    pipe_resource_reference(&m_resource, nullptr);
  }


  Rc<DxvkResourceAllocation> DxvkBuffer::allocateStorage(DxvkLocalAllocationCache* cache) {
    return new DxvkResourceAllocation(m_info.size,
      (m_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);
  }


  Rc<DxvkResourceAllocation> DxvkBuffer::assignStorage(Rc<DxvkResourceAllocation>&& slice) {
    Rc<DxvkResourceAllocation> result = std::move(m_storage);
    m_storage = std::move(slice);
    m_mapPtr = m_storage != nullptr ? m_storage->mapPtr() : nullptr;

    // A new host copy: all of it is what the GPU copy is to hold, unless it
    // replaced nothing (a buffer just made has nothing to upload).
    if (m_mapPtr && result != nullptr)
      markWritten(0, m_info.size);
    return result;
  }


  Rc<DxvkBufferView> DxvkBuffer::createView(const DxvkBufferViewKey& info) {
    std::unique_lock lock(m_viewMutex);
    auto entry = m_views.emplace(std::piecewise_construct,
      std::make_tuple(info), std::make_tuple(this, info));
    return &entry.first->second;
  }


  void DxvkBuffer::uploadHostWrites(pipe_context* pipe) {
    if (m_dirtyEnd > m_dirtyBegin && m_mapPtr) {
      // Out to whole words, where the buffer has them: an unaligned upload
      // cannot be recorded into the driver's batch, so it submits the draws
      // before it, and a draw range (16-bit indices from an odd start, say)
      // is often unaligned -- one submit per draw. The bytes around are the
      // host copy's, which is what the GPU copy holds there already unless
      // the GPU wrote the buffer since (then the range stays as it is).
      const bool widen = !isInUse(DxvkAccess::Write);
      VkDeviceSize begin = widen ? m_dirtyBegin & ~VkDeviceSize(3u) : m_dirtyBegin;
      VkDeviceSize end = std::min(m_dirtyEnd, m_info.size);
      const VkDeviceSize aligned = (end + 3u) & ~VkDeviceSize(3u);
      if (widen && aligned <= m_info.size)
        end = aligned;
      // All of the buffer: then all of the GPU buffer, whose size is rounded
      // up past the host copy's, so the driver sees a write of the whole of
      // it and can put it in a buffer of its own rather than stage it in
      // the pending batch. The host copy has the room (allocateStorage).
      if (begin == 0 && end >= m_info.size)
        end = m_resource->width0;
      pipe->buffer_subdata(pipe, m_resource, PIPE_MAP_WRITE,
        unsigned(begin), unsigned(end - begin),
        reinterpret_cast<const char*>(m_mapPtr) + begin);
    }
    clearWritten();
  }


  void DxvkBuffer::readBack(pipe_context* pipe) {
    if (m_mapPtr) {
      pipe_buffer_read(pipe, m_resource, 0, unsigned(m_info.size), m_mapPtr);
      clearWritten();
    }
    markHostStale(false);
  }

  // ---------------------------------------------------------------- image

  static pipe_texture_target dirkTarget(const DxvkImageCreateInfo& info) {
    switch (info.type) {
      case VK_IMAGE_TYPE_1D:
        return info.numLayers > 1 ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
      case VK_IMAGE_TYPE_3D:
        return PIPE_TEXTURE_3D;
      default:
        if (info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
          return info.numLayers > 6 ? PIPE_TEXTURE_CUBE_ARRAY : PIPE_TEXTURE_CUBE;
        return info.numLayers > 1 ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
    }
  }


  DxvkImage::DxvkImage(
          DxvkDevice*           device,
    const DxvkImageCreateInfo&  createInfo,
          VkMemoryPropertyFlags memFlags)
  : m_device(device),
    m_properties(memFlags),
    m_shaderStages(util::shaderStages(createInfo.stages)),
    m_info(createInfo) {
    m_info.debugName = nullptr;

    for (uint32_t i = 0; i < createInfo.viewFormatCount; i++)
      m_viewFormats.push_back(createInfo.viewFormats[i]);
    m_info.viewFormatCount = m_viewFormats.size();
    m_info.viewFormats = m_viewFormats.data();

    const pipe_format format = vk_format_to_pipe_format(m_info.format);
    if (format == PIPE_FORMAT_NONE)
      throw DxvkError(str::format("dirk: image format ", m_info.format, " has no gallium format"));

    pipe_resource templ = { };
    templ.target      = dirkTarget(m_info);
    templ.format      = format;
    templ.width0      = m_info.extent.width;
    templ.height0     = m_info.extent.height;
    templ.depth0      = m_info.extent.depth;
    templ.array_size  = m_info.type == VK_IMAGE_TYPE_3D ? 1 : m_info.numLayers;
    templ.last_level  = m_info.mipLevels - 1;
    templ.nr_samples  = m_info.sampleCount > VK_SAMPLE_COUNT_1_BIT ? m_info.sampleCount : 0;
    templ.nr_storage_samples = templ.nr_samples;
    templ.usage       = PIPE_USAGE_DEFAULT;

    unsigned bind = PIPE_BIND_SAMPLER_VIEW;
    const bool depth = util_format_is_depth_or_stencil(format);
    if (m_info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      bind |= PIPE_BIND_RENDER_TARGET;
    if (depth || (m_info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
      bind |= PIPE_BIND_DEPTH_STENCIL;
    if (m_info.usage & VK_IMAGE_USAGE_STORAGE_BIT)
      bind |= PIPE_BIND_SHADER_IMAGE;
    // Blits and clears of colour images render into them.
    if (!depth && device->screen()->is_format_supported(device->screen(),
          format, templ.target, templ.nr_samples, templ.nr_storage_samples, PIPE_BIND_RENDER_TARGET))
      bind |= PIPE_BIND_RENDER_TARGET;
    templ.bind = bind;

    m_resource = device->screen()->resource_create(device->screen(), &templ);
    if (!m_resource)
      throw DxvkError(str::format("dirk: failed to create a ", m_info.extent.width, "x",
        m_info.extent.height, " image of format ", m_info.format));

    m_storage = new DxvkResourceAllocation(0, false);
  }


  DxvkImage::~DxvkImage() {
    for (auto& v : m_views)
      v.second.releaseGallium();
    pipe_resource_reference(&m_resource, nullptr);
  }


  Rc<DxvkImageView> DxvkImage::createView(const DxvkImageViewKey& info) {
    std::unique_lock lock(m_viewMutex);
    auto entry = m_views.emplace(std::piecewise_construct,
      std::make_tuple(info), std::make_tuple(this, info));
    return &entry.first->second;
  }


  Rc<DxvkResourceAllocation> DxvkImage::allocateStorage() {
    return m_storage;
  }


  Rc<DxvkResourceAllocation> DxvkImage::allocateStorageWithUsage(
    const DxvkImageUsageInfo&         usage,
          DxvkAllocationModes         mode) {
    return m_storage;
  }


  Rc<DxvkResourceAllocation> DxvkImage::assignStorage(
          Rc<DxvkResourceAllocation>&& resource) {
    return m_storage;
  }


  Rc<DxvkResourceAllocation> DxvkImage::assignStorageWithUsage(
          Rc<DxvkResourceAllocation>&& resource,
    const DxvkImageUsageInfo&         usageInfo) {
    return m_storage;
  }


  void DxvkImageView::releaseGallium() {
    if (m_samplerView)
      pipe_sampler_view_reference(&m_samplerView, nullptr);
  }


  void DxvkBufferView::releaseGallium() {
    if (m_samplerView)
      pipe_sampler_view_reference(&m_samplerView, nullptr);
  }

  // ---------------------------------------------------------------- query

  static unsigned dirkQueryType(VkQueryType type, VkQueryControlFlags flags) {
    switch (type) {
      case VK_QUERY_TYPE_OCCLUSION:
        return (flags & VK_QUERY_CONTROL_PRECISE_BIT)
          ? PIPE_QUERY_OCCLUSION_COUNTER : PIPE_QUERY_OCCLUSION_PREDICATE;
      case VK_QUERY_TYPE_TIMESTAMP:
        return PIPE_QUERY_TIMESTAMP;
      default:
        return ~0u;
    }
  }


  DxvkQuery::DxvkQuery(
    const Rc<DxvkDevice>&             device,
          VkQueryType                 type,
          VkQueryControlFlags         flags,
          uint32_t                    index)
  : m_device(device), m_type(type), m_flags(flags), m_index(index) { }


  DxvkQuery::~DxvkQuery() {
    if (m_query)
      m_device->pipe()->destroy_query(m_device->pipe(), m_query);
  }


  void DxvkQuery::begin() {
    m_ended.store(false, std::memory_order_release);
    m_available = false;
    m_queryData = { };

    const unsigned type = dirkQueryType(m_type, m_flags);
    if (type == ~0u)
      return;
    pipe_context* pipe = m_device->pipe();
    if (!m_query)
      m_query = pipe->create_query(pipe, type, m_index);
    if (m_query && type != PIPE_QUERY_TIMESTAMP)
      pipe->begin_query(pipe, m_query);
  }


  void DxvkQuery::end() {
    const unsigned type = dirkQueryType(m_type, m_flags);
    pipe_context* pipe = m_device->pipe();
    if (type != ~0u && !m_query)
      m_query = pipe->create_query(pipe, type, m_index);
    if (m_query)
      pipe->end_query(pipe, m_query);
    m_ended.store(true, std::memory_order_release);
  }


  DxvkGpuQueryStatus DxvkQuery::getData(DxvkQueryData& queryData) {
    if (!m_ended.load(std::memory_order_acquire))
      return DxvkGpuQueryStatus::Pending;

    if (!m_available) {
      if (m_query) {
        pipe_query_result result = { };
        pipe_context* pipe = m_device->pipe();
        if (!pipe->get_query_result(pipe, m_query, false, &result))
          return DxvkGpuQueryStatus::Pending;

        switch (m_type) {
          case VK_QUERY_TYPE_OCCLUSION:
            m_queryData.occlusion.samplesPassed =
              dirkQueryType(m_type, m_flags) == PIPE_QUERY_OCCLUSION_COUNTER
                ? result.u64 : uint64_t(result.b);
            break;
          case VK_QUERY_TYPE_TIMESTAMP:
            m_queryData.timestamp.time = result.u64;
            break;
          default:
            break;
        }
      }
      m_available = true;
    }

    queryData = m_queryData;
    return DxvkGpuQueryStatus::Available;
  }

}

// ---------------------------------------------------------------- C API

extern "C" void dirk_register_screen(struct pipe_screen* screen) {
  dxvk::g_screen = screen;
}

extern "C" void dirk_init(struct pipe_screen* screen, const struct dirk_host* host) {
  dxvk::g_screen = screen;
  dxvk::g_host = host;
}

extern "C" const struct dirk_host* dirk_get_host(void) {
  return dxvk::g_host;
}

static std::string g_exePath = "dirk.exe";

extern "C" void dirk_set_exe_path(const char* path) {
  g_exePath = path ? path : "";
}

extern "C" const char* dirk_exe_path(void) {
  return g_exePath.c_str();
}
