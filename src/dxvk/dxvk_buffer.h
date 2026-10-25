#pragma once

#include <unordered_map>
#include <vector>

#include "dxvk_format.h"
#include "dxvk_hash.h"
#include "dxvk_memory.h"
#include "dxvk_sparse.h"

struct pipe_context;
struct pipe_resource;
struct pipe_sampler_view;

namespace dxvk {

  class DxvkBuffer;
  class DxvkDevice;

  struct DxvkBufferCreateInfo {
    VkDeviceSize size = 0u;
    VkBufferUsageFlags usage = 0u;
    VkPipelineStageFlags stages = 0u;
    VkAccessFlags access = 0u;
    VkBufferCreateFlags flags = 0;
    const char* debugName = nullptr;
  };

  /**
   * \brief Buffer slice handle
   *
   * \c handle names the gallium resource; DXVK compares handles to see
   * whether two slices are of one buffer, which is all it is used for.
   */
  struct DxvkBufferSliceHandle {
    VkBuffer      handle = VK_NULL_HANDLE;
    VkDeviceSize  offset = 0u;
    VkDeviceSize  length = 0u;
    void*         mapPtr = nullptr;

    bool eq(const DxvkBufferSliceHandle& other) const {
      return handle == other.handle
          && offset == other.offset
          && length == other.length;
    }

    size_t hash() const {
      DxvkHashState result;
      result.add(std::hash<VkBuffer>()(handle));
      result.add(std::hash<VkDeviceSize>()(offset));
      result.add(std::hash<VkDeviceSize>()(length));
      return result;
    }
  };


  /**
   * \brief Buffer view
   *
   * A range of a buffer and a format: typed (a texel buffer) or, with
   * VK_FORMAT_UNDEFINED, raw words. The gallium objects for it are made
   * by the context when it binds the view.
   */
  class DxvkBufferView {
  public:

    DxvkBufferView(
            DxvkBuffer*                 buffer,
      const DxvkBufferViewKey&          key)
    : m_buffer(buffer), m_key(key) { }

    void incRef();
    void decRef();

    DxvkBufferSliceHandle getSliceHandle() const;

    VkDeviceSize elementCount() const {
      auto format = lookupFormatInfo(m_key.format);
      return m_key.size / format->elementSize;
    }

    DxvkBufferViewKey info() const {
      return m_key;
    }

    DxvkBuffer* buffer() const {
      return m_buffer;
    }

    const DxvkFormatInfo* formatInfo() const {
      return lookupFormatInfo(m_key.format);
    }

    /** \brief The gallium sampler view of a texel buffer, made on first use */
    pipe_sampler_view*& samplerView() {
      return m_samplerView;
    }

    void releaseGallium();

  private:

    DxvkBuffer*       m_buffer  = nullptr;
    DxvkBufferViewKey m_key     = { };
    pipe_sampler_view* m_samplerView = nullptr;

  };


  /**
   * \brief Buffer
   *
   * One gallium buffer for its whole life, plus a host copy (\c storage)
   * when its memory is mapped. A discard swaps the host copy; what was
   * written to it reaches the gallium buffer as an upload the next time a
   * command may read it (\c DxvkContext::flushHostWrites).
   */
  class DxvkBuffer : public DxvkPagedResource {
    friend DxvkBufferView;
  public:

    DxvkBuffer(
            DxvkDevice*           device,
      const DxvkBufferCreateInfo& createInfo,
            VkMemoryPropertyFlags memFlags);

    ~DxvkBuffer();

    const DxvkBufferCreateInfo& info() const {
      return m_info;
    }

    VkMemoryPropertyFlags memFlags() const {
      return m_properties;
    }

    void* mapPtr(VkDeviceSize offset) const {
      return m_mapPtr
        ? reinterpret_cast<char*>(m_mapPtr) + offset
        : nullptr;
    }

    VkDeviceAddress gpuAddress() const {
      return 0u;
    }

    VkShaderStageFlags getShaderStages() const {
      return m_shaderStages;
    }

    DxvkBufferSliceHandle getSliceHandle() const {
      return getSliceHandle(0u, m_info.size);
    }

    DxvkBufferSliceHandle getSliceHandle(VkDeviceSize offset, VkDeviceSize length) const {
      DxvkBufferSliceHandle result = { };
      result.handle = VkBuffer(reinterpret_cast<uintptr_t>(m_resource));
      result.offset = offset;
      result.length = length;
      result.mapPtr = mapPtr(offset);
      return result;
    }

    uint32_t getXfbVertexStride() const {
      return m_xfbStride;
    }

    void setXfbVertexStride(uint32_t stride) {
      m_xfbStride = stride;
    }

    Rc<DxvkResourceAllocation> allocateStorage() {
      return allocateStorage(nullptr);
    }

    Rc<DxvkResourceAllocation> allocateStorage(DxvkLocalAllocationCache* cache);

    /**
     * \brief Replaces the host copy
     *
     * Everything of the new copy is taken as written, unless the writer
     * narrowed it (\c markWritten after \c clearWritten).
     */
    Rc<DxvkResourceAllocation> assignStorage(Rc<DxvkResourceAllocation>&& slice);

    Rc<DxvkResourceAllocation> storage() const {
      return m_storage;
    }

    bit::uint48_t getResourceId() const {
      return bit::uint48_t(cookie());
    }

    bool canRelocate() const {
      return false;
    }

    void enableStableAddress() { }

    Rc<DxvkBufferView> createView(
      const DxvkBufferViewKey& info);

    DxvkSparsePageTable* getSparsePageTable() {
      return nullptr;
    }

    Rc<DxvkResourceAllocation> relocateStorage(
            DxvkAllocationModes         mode) {
      return nullptr;
    }

    void setDebugName(const char* name) {
      m_debugName = name ? name : "";
    }

    const char* getDebugName() const {
      return m_debugName.c_str();
    }

    /** \brief The gallium buffer */
    pipe_resource* resource() const {
      return m_resource;
    }

    /**
     * \brief Host writes not yet uploaded
     *
     * The range of the host copy written since the last upload, in bytes;
     * empty when \c end is not above \c begin.
     */
    VkDeviceSize dirtyBegin() const { return m_dirtyBegin; }
    VkDeviceSize dirtyEnd() const { return m_dirtyEnd; }

    /** \brief Adds a range the host wrote */
    void markWritten(VkDeviceSize offset, VkDeviceSize length) {
      m_dirtyBegin = std::min(m_dirtyBegin, offset);
      m_dirtyEnd = std::max(m_dirtyEnd, offset + length);
    }

    /** \brief Forgets what the host wrote, for a writer that says it itself */
    void clearWritten() {
      m_dirtyBegin = ~VkDeviceSize(0u);
      m_dirtyEnd = 0u;
    }

    /** \brief Uploads what the host wrote, on the device's thread */
    void uploadHostWrites(pipe_context* pipe);

    /** \brief Copies the GPU copy back to the host copy */
    void readBack(pipe_context* pipe);

    /** \brief Whether the context's list of buffers to upload has it */
    bool queuedForUpload(uint64_t mark) {
      return std::exchange(m_uploadMark, mark) == mark;
    }

  private:

    DxvkDevice*                 m_device        = nullptr;
    VkMemoryPropertyFlags       m_properties    = 0u;
    VkShaderStageFlags          m_shaderStages  = 0u;
    DxvkBufferCreateInfo        m_info          = { };
    uint32_t                    m_xfbStride     = 0u;

    pipe_resource*              m_resource      = nullptr;
    Rc<DxvkResourceAllocation>  m_storage;
    void*                       m_mapPtr        = nullptr;

    VkDeviceSize                m_dirtyBegin    = ~VkDeviceSize(0u);
    VkDeviceSize                m_dirtyEnd      = 0u;
    uint64_t                    m_uploadMark    = 0u;

    dxvk::mutex                 m_viewMutex;
    std::unordered_map<DxvkBufferViewKey,
      DxvkBufferView, DxvkHash, DxvkEq> m_views;

    std::string                 m_debugName;

  };


  /**
   * \brief Buffer slice
   */
  class DxvkBufferSlice {

  public:

    DxvkBufferSlice() { }

    DxvkBufferSlice(
            Rc<DxvkBuffer>  buffer,
            VkDeviceSize    rangeOffset,
            VkDeviceSize    rangeLength)
    : m_buffer(std::move(buffer)),
      m_offset(rangeOffset),
      m_length(rangeLength) { }

    explicit DxvkBufferSlice(Rc<DxvkBuffer> buffer)
    : m_buffer(std::move(buffer)),
      m_offset(0),
      m_length(m_buffer->info().size) { }

    explicit DxvkBufferSlice(const Rc<DxvkBufferView>& view)
    : DxvkBufferSlice(view->buffer(), view->info().offset, view->info().size) { }

    DxvkBufferSlice(const DxvkBufferSlice& ) = default;
    DxvkBufferSlice(      DxvkBufferSlice&&) = default;

    DxvkBufferSlice& operator = (const DxvkBufferSlice& other) {
      if (m_buffer != other.m_buffer)
        m_buffer = other.m_buffer;
      m_offset = other.m_offset;
      m_length = other.m_length;
      return *this;
    }

    DxvkBufferSlice& operator = (DxvkBufferSlice&&) = default;

    size_t offset() const { return m_offset; }
    size_t length() const { return m_length; }

    const Rc<DxvkBuffer>& buffer() const {
      return m_buffer;
    }

    const DxvkBufferCreateInfo& bufferInfo() const {
      return m_buffer->info();
    }

    DxvkBufferSlice subSlice(VkDeviceSize offset, VkDeviceSize length) const {
      return DxvkBufferSlice(m_buffer, m_offset + offset, length);
    }

    bool defined() const {
      return m_buffer != nullptr;
    }

    DxvkBufferSliceHandle getSliceHandle() const {
      return m_buffer != nullptr
        ? m_buffer->getSliceHandle(m_offset, m_length)
        : DxvkBufferSliceHandle();
    }

    DxvkBufferSliceHandle getSliceHandle(VkDeviceSize offset, VkDeviceSize length) const {
      return m_buffer != nullptr
        ? m_buffer->getSliceHandle(m_offset + offset, length)
        : DxvkBufferSliceHandle();
    }

    void* mapPtr(VkDeviceSize offset) const  {
      return m_buffer != nullptr
        ? m_buffer->mapPtr(m_offset + offset)
        : nullptr;
    }

    bool matches(const DxvkBufferSlice& other) const {
      return this->m_buffer == other.m_buffer
          && this->m_offset == other.m_offset
          && this->m_length == other.m_length;
    }

    bool matchesBuffer(const DxvkBufferSlice& other) const {
      return this->m_buffer == other.m_buffer;
    }

    bool matchesRange(const DxvkBufferSlice& other) const {
      return this->m_offset == other.m_offset
          && this->m_length == other.m_length;
    }

    void setRange(VkDeviceSize offset, VkDeviceSize length) {
      m_offset = offset;
      m_length = length;
    }

  private:

    Rc<DxvkBuffer> m_buffer = nullptr;
    VkDeviceSize   m_offset = 0;
    VkDeviceSize   m_length = 0;

  };


  inline DxvkBufferSliceHandle DxvkBufferView::getSliceHandle() const {
    return m_buffer->getSliceHandle(m_key.offset, m_key.size);
  }

  inline void DxvkBufferView::incRef() {
    m_buffer->incRef();
  }

  inline void DxvkBufferView::decRef() {
    m_buffer->decRef();
  }

}
