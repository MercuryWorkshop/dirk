#pragma once

// dirk: WebGPU has no sparse resources, and the adapter reports none, so the
// front end makes no tiled resources. These are the types its tiled-resource
// paths name; nothing here holds pages.

#include <vector>

#include "dxvk_memory.h"

namespace dxvk {

  constexpr static VkDeviceSize SparseMemoryPageSize = 1ull << 16;

  enum class DxvkSparsePageType : uint32_t {
    None            = 0,
    Buffer          = 1,
    Image           = 2,
    ImageMipTail    = 3,
  };

  struct DxvkSparsePageBufferInfo {
    VkDeviceSize              offset;
    VkDeviceSize              length;
  };

  struct DxvkSparsePageImageInfo {
    VkImageSubresource        subresource;
    VkOffset3D                offset;
    VkExtent3D                extent;
  };

  struct DxvkSparsePageMipTailInfo {
    VkDeviceSize              resourceOffset;
    VkDeviceSize              resourceLength;
  };

  struct DxvkSparsePageInfo {
    DxvkSparsePageType type;
    union {
      DxvkSparsePageBufferInfo  buffer;
      DxvkSparsePageImageInfo   image;
      DxvkSparsePageMipTailInfo mipTail;
    };
  };

  struct DxvkSparseImageProperties {
    VkSparseImageFormatFlags  flags;
    VkExtent3D                pageRegionExtent;
    uint32_t                  pagedMipCount;
    uint32_t                  metadataPageCount;
    uint32_t                  mipTailPageIndex;
    VkDeviceSize              mipTailOffset;
    VkDeviceSize              mipTailSize;
    VkDeviceSize              mipTailStride;
  };

  struct DxvkSparseImageSubresourceProperties {
    VkBool32                  isMipTail;
    VkExtent3D                pageCount;
    uint32_t                  pageIndex;
  };

  enum class DxvkSparseBindFlag : uint32_t {
    SkipSynchronization,
  };

  using DxvkSparseBindFlags = Flags<DxvkSparseBindFlag>;

  enum class DxvkSparseBindMode : uint32_t {
    Null,
    Bind,
    Copy,
  };

  struct DxvkSparseBind {
    DxvkSparseBindMode        mode;
    uint32_t                  dstPage;
    uint32_t                  srcPage;
  };

  class DxvkSparsePageAllocator : public RcObject {
  public:
    void setCapacity(uint32_t pageCount) { m_pageCount = pageCount; }
  private:
    uint32_t m_pageCount = 0u;
  };

  struct DxvkSparseBindInfo {
    Rc<DxvkPagedResource>       dstResource;
    Rc<DxvkPagedResource>       srcResource;
    Rc<DxvkSparsePageAllocator> srcAllocator;
    std::vector<DxvkSparseBind> binds;
  };

  class DxvkSparsePageTable {
  public:
    explicit operator bool () const { return false; }
    uint32_t getPageCount() const { return 0u; }
    uint32_t getSubresourceCount() const { return 0u; }
    DxvkSparseImageProperties getProperties() const { return DxvkSparseImageProperties(); }
    DxvkSparseImageSubresourceProperties getSubresourceProperties(uint32_t) const {
      return DxvkSparseImageSubresourceProperties();
    }
    DxvkSparsePageInfo getPageInfo(uint32_t) const { return DxvkSparsePageInfo(); }
    uint32_t computePageIndex(uint32_t, VkOffset3D, VkExtent3D, VkBool32, uint32_t) const {
      return 0u;
    }
  };

}
