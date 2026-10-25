#pragma once

#include <unordered_map>

#include "dxvk_format.h"
#include "dxvk_memory.h"
#include "dxvk_sparse.h"
#include "dxvk_util.h"

struct pipe_resource;
struct pipe_sampler_view;

namespace dxvk {

  class DxvkDevice;
  class DxvkImage;

  struct DxvkImageCreateInfo {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageCreateFlags flags = 0u;
    VkSampleCountFlagBits sampleCount = VkSampleCountFlagBits(0u);
    VkExtent3D extent = { };
    uint32_t numLayers = 0u;
    uint32_t mipLevels = 0u;
    VkImageUsageFlags usage = 0u;
    VkPipelineStageFlags stages = 0u;
    VkAccessFlags access = 0u;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_MAX_ENUM_KHR;
    VkBool32 shared = VK_FALSE;
    VkBool32 transient = VK_FALSE;
    uint32_t viewFormatCount = 0;
    const VkFormat* viewFormats = nullptr;
    DxvkSharedHandleInfo sharing = { };
    const char* debugName = nullptr;
  };

  struct DxvkImageUsageInfo {
    VkImageCreateFlags flags = 0u;
    VkImageUsageFlags usage = 0u;
    VkPipelineStageFlags stages = 0u;
    VkAccessFlags access = 0u;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_MAX_ENUM_KHR;
    uint32_t viewFormatCount = 0u;
    const VkFormat* viewFormats = nullptr;
    VkBool32 stableGpuAddress = VK_FALSE;
  };


  /**
   * \brief Image view
   *
   * A key into an image: type, format, aspects, mips and layers, swizzle.
   * The gallium views for it (sampler view, surface, image view) are made
   * by the context when it binds it.
   */
  class DxvkImageView {

  public:

    DxvkImageView(
            DxvkImage*                image,
      const DxvkImageViewKey&         key)
    : m_image(image), m_key(key) { }

    void incRef();
    void decRef();

    /** \brief No Vulkan view: only extension paths the adapter lacks ask */
    VkImageView handle() const {
      return VK_NULL_HANDLE;
    }

    VkImageViewType type() const {
      return m_key.viewType;
    }

    DxvkImageViewKey info() const {
      return m_key;
    }

    DxvkImage* image() const {
      return m_image;
    }

    const DxvkFormatInfo* formatInfo() const {
      return lookupFormatInfo(m_key.format);
    }

    VkExtent3D mipLevelExtent(uint32_t level) const;

    VkImageSubresourceRange subresources() const {
      VkImageSubresourceRange result;
      result.aspectMask     = m_key.aspects;
      result.baseMipLevel   = m_key.mipIndex;
      result.levelCount     = m_key.mipCount;
      result.baseArrayLayer = m_key.layerIndex;
      result.layerCount     = m_key.layerCount;
      return result;
    }

    VkImageSubresourceRange imageSubresources() const;

    VkImageLayout pickLayout(VkImageLayout layout) const {
      return layout;
    }

    bool matchesView(const Rc<DxvkImageView>& view) const {
      if (this == view.ptr())
        return true;

      return this->image()         == view->image()
          && this->subresources()  == view->subresources()
          && this->info().viewType == view->info().viewType
          && this->info().format   == view->info().format;
    }

    bool checkSubresourceOverlap(const Rc<DxvkImageView>& view) const {
      if (likely(m_image != view->m_image))
        return false;

      return vk::checkSubresourceRangeOverlap(
        this->imageSubresources(),
        view->imageSubresources());
    }

    VkImageLayout defaultLayout() const {
      return VK_IMAGE_LAYOUT_GENERAL;
    }

    bool isMultisampled() const;

    bool hasGfxStores() const {
      return false;
    }

    /** \brief The gallium sampler view, made on first use */
    pipe_sampler_view*& samplerView() {
      return m_samplerView;
    }

    void releaseGallium();

  private:

    DxvkImage*              m_image     = nullptr;
    DxvkImageViewKey        m_key       = { };
    pipe_sampler_view*      m_samplerView = nullptr;

  };


  /**
   * \brief Image
   *
   * One gallium texture. Images are never host-visible here: the front end
   * maps them through buffers (the device reports no linear images).
   */
  class DxvkImage : public DxvkPagedResource {
    friend DxvkImageView;
  public:

    DxvkImage(
            DxvkDevice*           device,
      const DxvkImageCreateInfo&  createInfo,
            VkMemoryPropertyFlags memFlags);

    ~DxvkImage();

    VkImage handle() const {
      return VkImage(reinterpret_cast<uintptr_t>(m_resource));
    }

    const DxvkImageCreateInfo& info() const {
      return m_info;
    }

    VkMemoryPropertyFlags memFlags() const {
      return m_properties;
    }

    VkShaderStageFlags getShaderStages() const {
      return m_shaderStages;
    }

    void* mapPtr(VkDeviceSize offset) const {
      return nullptr;
    }

    const DxvkFormatInfo* formatInfo() const {
      return lookupFormatInfo(m_info.format);
    }

    VkExtent3D mipLevelExtent(uint32_t level) const {
      return util::computeMipLevelExtent(m_info.extent, level);
    }

    VkExtent3D mipLevelExtent(uint32_t level, VkImageAspectFlags aspect) const {
      return util::computeMipLevelExtent(m_info.extent, level, m_info.format, aspect);
    }

    VkImageLayout pickLayout(VkImageLayout layout) const {
      return layout;
    }

    void setLayout(VkImageLayout layout) {
      m_info.layout = layout;
    }

    bool isFullSubresource(
      const VkImageSubresourceLayers& subresource,
            VkExtent3D                extent) const {
      return subresource.aspectMask == this->formatInfo()->aspectMask
          && extent == this->mipLevelExtent(subresource.mipLevel);
    }

    bool isViewCompatible(VkFormat format) const {
      bool result = m_info.format == format;
      for (uint32_t i = 0; i < m_viewFormats.size() && !result; i++)
        result |= m_viewFormats[i] == format;
      return result;
    }

    DxvkResourceMemoryInfo getMemoryInfo() const {
      return DxvkResourceMemoryInfo();
    }

    VkImageSubresourceRange getAvailableSubresources() const {
      VkImageSubresourceRange result;
      result.aspectMask     = formatInfo()->aspectMask;
      result.baseMipLevel   = 0;
      result.levelCount     = info().mipLevels;
      result.baseArrayLayer = 0;
      result.layerCount     = info().numLayers;
      return result;
    }

    bool canRelocate() const {
      return false;
    }

    HANDLE sharedHandle() const {
      return INVALID_HANDLE_VALUE;
    }

    DxvkSparsePageTable* getSparsePageTable() {
      return nullptr;
    }

    Rc<DxvkResourceAllocation> relocateStorage(
            DxvkAllocationModes         mode) {
      return nullptr;
    }

    Rc<DxvkResourceAllocation> allocateStorage();

    Rc<DxvkResourceAllocation> allocateStorageWithUsage(
      const DxvkImageUsageInfo&         usage,
            DxvkAllocationModes         mode);

    Rc<DxvkResourceAllocation> assignStorage(
            Rc<DxvkResourceAllocation>&& resource);

    Rc<DxvkResourceAllocation> assignStorageWithUsage(
            Rc<DxvkResourceAllocation>&& resource,
      const DxvkImageUsageInfo&         usageInfo);

    Rc<DxvkResourceAllocation> storage() const {
      return m_storage;
    }

    bit::uint48_t getResourceId() const {
      return bit::uint48_t(cookie());
    }

    uint64_t getTrackingAddress(uint32_t mip, uint32_t layer) const {
      return uint64_t((m_info.numLayers * mip) + layer) << 48u;
    }

    Rc<DxvkImageView> createView(
      const DxvkImageViewKey& info);

    void trackInitialization(
      const VkImageSubresourceRange& subresources) { }

    bool isInitialized(
      const VkImageSubresourceRange& subresources) const {
      return true;
    }

    void setDebugName(const char* name) {
      m_debugName = name ? name : "";
    }

    const char* getDebugName() const {
      return m_debugName.c_str();
    }

    /** \brief The gallium texture */
    pipe_resource* resource() const {
      return m_resource;
    }

  private:

    DxvkDevice*                 m_device      = nullptr;
    VkMemoryPropertyFlags       m_properties  = 0u;
    VkShaderStageFlags          m_shaderStages = 0u;
    DxvkImageCreateInfo         m_info        = { };

    pipe_resource*              m_resource    = nullptr;
    Rc<DxvkResourceAllocation>  m_storage     = nullptr;

    small_vector<VkFormat, 4>   m_viewFormats;

    dxvk::mutex                 m_viewMutex;
    std::unordered_map<DxvkImageViewKey,
      DxvkImageView, DxvkHash, DxvkEq> m_views;

    std::string                 m_debugName;

  };


  inline void DxvkImageView::incRef() {
    m_image->incRef();
  }

  inline void DxvkImageView::decRef() {
    m_image->decRef();
  }

  inline bool DxvkImageView::isMultisampled() const {
    return m_image->info().sampleCount > VK_SAMPLE_COUNT_1_BIT;
  }

  inline VkImageSubresourceRange DxvkImageView::imageSubresources() const {
    VkImageSubresourceRange result = { };
    result.aspectMask     = m_key.aspects;
    result.baseMipLevel   = m_key.mipIndex;
    result.levelCount     = m_key.mipCount;

    if (likely(m_image->info().type != VK_IMAGE_TYPE_3D)) {
      result.baseArrayLayer = m_key.layerIndex;
      result.layerCount     = m_key.layerCount;
    } else {
      result.baseArrayLayer = 0;
      result.layerCount     = 1;
    }

    return result;
  }

  inline VkExtent3D DxvkImageView::mipLevelExtent(uint32_t level) const {
    return m_image->mipLevelExtent(level + m_key.mipIndex, m_key.aspects);
  }

}
