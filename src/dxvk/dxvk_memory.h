#pragma once

// dirk: resources are gallium resources (pipe_resource) owned by the buffer
// or image that made them. What DXVK calls a resource's storage -- the
// allocation a discard swaps in -- is here only the host copy the front end
// maps and writes: a block of memory. The GPU copy stays the same resource,
// and what was written reaches it as a range upload at the point of the
// command stream where it was written (dirk_upload.h), so a discard never
// needs a new GPU resource.

#include <atomic>
#include <array>
#include <cstdlib>
#include <unordered_map>

#include "dxvk_access.h"
#include "dxvk_hash.h"
#include "dxvk_include.h"

struct pipe_resource;

namespace dxvk {

  class DxvkDevice;
  class DxvkMemoryAllocator;
  class DxvkSparsePageTable;

  enum class DxvkSharedHandleMode {
    None,
    Import,
    Export,
  };

  struct DxvkSharedHandleInfo {
    DxvkSharedHandleMode mode = DxvkSharedHandleMode::None;
    VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
    union {
      HANDLE handle = INVALID_HANDLE_VALUE;
    };
  };

  struct DxvkMemoryStats {
    VkDeviceSize memoryAllocated = 0;
    VkDeviceSize memoryUsed      = 0;
    VkDeviceSize memoryBudget    = 0;
  };

  struct DxvkSharingModeInfo {
    std::array<uint32_t, 2u> queueFamilies = { };
  };

  struct DxvkBufferViewKey {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkBufferUsageFlags usage = 0u;
    VkDeviceSize offset = 0u;
    VkDeviceSize size = 0u;

    size_t hash() const {
      DxvkHashState hash;
      hash.add(uint32_t(format));
      hash.add(uint32_t(usage));
      hash.add(offset);
      hash.add(size);
      return hash;
    }

    bool eq(const DxvkBufferViewKey& other) const {
      return format == other.format
          && usage  == other.usage
          && offset == other.offset
          && size   == other.size;
    }
  };

  struct DxvkImageViewKey {
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    VkImageUsageFlags usage = 0u;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspects = 0u;
    uint8_t mipIndex = 0u;
    uint8_t mipCount = 0u;
    uint16_t layerIndex = 0u;
    uint16_t layerCount = 0u;
    uint16_t packedSwizzle = 0u;

    size_t hash() const {
      DxvkHashState hash;
      hash.add(uint32_t(viewType));
      hash.add(uint32_t(usage));
      hash.add(uint32_t(format));
      hash.add(uint32_t(aspects));
      hash.add(uint32_t(mipIndex) | (uint32_t(mipCount) << 16));
      hash.add(uint32_t(layerIndex) | (uint32_t(layerCount) << 16));
      hash.add(uint32_t(packedSwizzle));
      return hash;
    }

    bool eq(const DxvkImageViewKey& other) const {
      return viewType == other.viewType
          && usage == other.usage
          && format == other.format
          && aspects == other.aspects
          && mipIndex == other.mipIndex
          && mipCount == other.mipCount
          && layerIndex == other.layerIndex
          && layerCount == other.layerCount
          && packedSwizzle == other.packedSwizzle;
    }

    VkComponentMapping unpackSwizzle() const {
      return VkComponentMapping {
        VkComponentSwizzle((packedSwizzle >>  0) & 0xf),
        VkComponentSwizzle((packedSwizzle >>  4) & 0xf),
        VkComponentSwizzle((packedSwizzle >>  8) & 0xf),
        VkComponentSwizzle((packedSwizzle >> 12) & 0xf) };
    }

    static uint16_t packSwizzle(VkComponentMapping mapping) {
      return (uint16_t(mapping.r) <<  0)
           | (uint16_t(mapping.g) <<  4)
           | (uint16_t(mapping.b) <<  8)
           | (uint16_t(mapping.a) << 12);
    }
  };

  struct DxvkResourceMemoryInfo {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0u;
    VkDeviceSize size = 0u;
  };

  struct DxvkBufferImportInfo {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* mapPtr = nullptr;
  };

  enum class DxvkAllocationMode : uint32_t {
    NoFallback      = 0,
    NoAllocation    = 1,
    NoDedicated     = 2,
    eFlagEnum
  };

  using DxvkAllocationModes = Flags<DxvkAllocationMode>;

  /** \brief DXVK's allocation page size, which the front end sizes uploads by */
  struct DxvkPageAllocator {
    constexpr static uint32_t PageBits = 16;
    constexpr static uint64_t PageSize = 1u << PageBits;
  };


  /**
   * \brief Host copy of a resource
   *
   * A block of host memory of the resource's size, which the front end maps.
   * Mapped memory has one; device-local memory has none (mapPtr null).
   * Reference counted, so a block still named by a pending command outlives
   * a discard that replaced it.
   */
  class DxvkResourceAllocation {
  public:

    DxvkResourceAllocation(VkDeviceSize size, bool mapped);
    ~DxvkResourceAllocation();

    void incRef() {
      m_useCount.fetch_add(1u, std::memory_order_acquire);
    }

    void decRef() {
      if (unlikely(m_useCount.fetch_sub(1u, std::memory_order_release) == 1u))
        delete this;
    }

    void* mapPtr() const {
      return m_mapPtr;
    }

    VkDeviceSize size() const {
      return m_size;
    }

    DxvkResourceMemoryInfo getMemoryInfo() const {
      DxvkResourceMemoryInfo result = { };
      result.size = m_size;
      return result;
    }

    VkMemoryPropertyFlags getMemoryProperties() const {
      return m_mapPtr
        ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }

    DxvkSparsePageTable* getSparsePageTable() const {
      return nullptr;
    }

  private:

    std::atomic<uint32_t> m_useCount = { 0u };
    VkDeviceSize          m_size     = 0u;
    void*                 m_mapPtr   = nullptr;

  };


  /**
   * \brief Allocation cache
   *
   * DXVK keeps small mapped allocations of a context's own here. Host
   * blocks need no cache of their own: malloc is that.
   */
  class DxvkLocalAllocationCache {

  public:

    DxvkLocalAllocationCache() = default;

  };


  /**
   * \brief Paged resource
   *
   * Base class of buffers and images: a cookie that names the resource,
   * and a reference count.
   */
  class DxvkPagedResource {

  public:

    DxvkPagedResource()
    : m_cookie(++s_cookie) { }

    virtual ~DxvkPagedResource();

    uint64_t cookie() const {
      return m_cookie;
    }

    force_inline void incRef() {
      m_useCount.fetch_add(1u, std::memory_order_acquire);
    }

    force_inline void decRef() {
      if (unlikely(m_useCount.fetch_sub(1u, std::memory_order_release) == 1u))
        delete this;
    }

    force_inline void acquire(DxvkAccess) { incRef(); }
    force_inline void release(DxvkAccess) { decRef(); }
    force_inline void convertRef(DxvkAccess, DxvkAccess) { }

    Rc<DxvkPagedResource> tryAcquire() {
      uint64_t refCount = m_useCount.load(std::memory_order_acquire);
      do {
        if (!refCount)
          return nullptr;
      } while (!m_useCount.compare_exchange_strong(refCount,
        refCount + 1u, std::memory_order_relaxed));
      return Rc<DxvkPagedResource>::unsafeCreate(this);
    }

    /**
     * \brief Whether the GPU still uses the resource
     *
     * Uploads copy what they read when they are recorded, so nothing the
     * host writes can race the GPU: reads are never in use. The GPU's
     * writes reach the host copy only when it is read back, so a resource
     * the GPU wrote is in use for writing until then (\c hostStale).
     */
    force_inline bool isInUse(DxvkAccess access) const {
      return access == DxvkAccess::Write
        && m_hostStale.load(std::memory_order_acquire);
    }

    void markHostStale(bool stale) {
      m_hostStale.store(stale, std::memory_order_release);
    }

    uint64_t getTrackId() const { return m_trackId >> 1u; }

    bool trackId(uint64_t trackingId, DxvkAccess access) {
      uint64_t trackId = (trackingId << 1u) + uint64_t(access == DxvkAccess::Write);
      if (trackId <= m_trackId)
        return false;
      m_trackId = trackId;
      return true;
    }

    bool isTracked(uint64_t trackingId, DxvkAccess access) const {
      return m_trackId >= (trackingId << 1u) + uint64_t(access != DxvkAccess::Write);
    }

    void resetTracking() { m_trackId = 0u; }

    bool hasGfxStores() const { return m_hasGfxStores; }
    bool trackGfxStores() { return std::exchange(m_hasGfxStores, true); }

    virtual DxvkSparsePageTable* getSparsePageTable() = 0;

    virtual Rc<DxvkResourceAllocation> relocateStorage(
            DxvkAllocationModes         mode) = 0;

    virtual void setDebugName(const char* name) = 0;

    virtual const char* getDebugName() const = 0;

  private:

    std::atomic<uint64_t> m_useCount  = { 0u };
    std::atomic<bool>     m_hostStale = { false };
    uint64_t              m_trackId   = 0u;
    uint64_t              m_cookie    = 0u;
    bool                  m_hasGfxStores = false;

    static std::atomic<uint64_t> s_cookie;

  };

}
