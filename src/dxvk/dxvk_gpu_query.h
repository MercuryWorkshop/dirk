#pragma once

// dirk: a query is one gallium query (pipe_query), made by the context the
// first time the query begins and read back with get_query_result without
// waiting. Statistics and stream-output queries have no WebGPU counterpart
// and report zeros once ended.

#include <atomic>

#include "dxvk_include.h"

struct pipe_query;

namespace dxvk {

  class DxvkDevice;

  enum class DxvkGpuQueryStatus : uint32_t {
    Invalid   = 0,
    Pending   = 1,
    Available = 2,
    Failed    = 3,
  };

  struct DxvkQueryOcclusionData {
    uint64_t samplesPassed;
  };

  struct DxvkQueryTimestampData {
    uint64_t time;
  };

  struct DxvkQueryStatisticData {
    uint64_t iaVertices;
    uint64_t iaPrimitives;
    uint64_t vsInvocations;
    uint64_t gsInvocations;
    uint64_t gsPrimitives;
    uint64_t clipInvocations;
    uint64_t clipPrimitives;
    uint64_t fsInvocations;
    uint64_t tcsPatches;
    uint64_t tesInvocations;
    uint64_t csInvocations;
  };

  struct DxvkQueryXfbStreamData {
    uint64_t primitivesWritten;
    uint64_t primitivesNeeded;
  };

  union DxvkQueryData {
    DxvkQueryOcclusionData occlusion;
    DxvkQueryTimestampData timestamp;
    DxvkQueryStatisticData statistic;
    DxvkQueryXfbStreamData xfbStream;
  };


  /**
   * \brief Query
   *
   * Begun and ended by the context, on the device's thread; \c getData
   * may be called on any thread and takes the device's lock.
   */
  class DxvkQuery {
    friend class DxvkContext;
  public:

    DxvkQuery(
      const Rc<DxvkDevice>&             device,
            VkQueryType                 type,
            VkQueryControlFlags         flags,
            uint32_t                    index);

    ~DxvkQuery();

    force_inline void incRef() {
      m_refCount.fetch_add(1u, std::memory_order_acquire);
    }

    force_inline void decRef() {
      if (m_refCount.fetch_sub(1u, std::memory_order_release) == 1u)
        delete this;
    }

    VkQueryType type() const {
      return m_type;
    }

    VkQueryControlFlags flags() const {
      return m_flags;
    }

    uint32_t index() const {
      return m_index;
    }

    DxvkGpuQueryStatus getData(
            DxvkQueryData&      queryData);

    void begin();

    void end();

    /** \brief The gallium query, or null for a type gallium lacks */
    pipe_query* gallium() const {
      return m_query;
    }

  private:

    std::atomic<uint32_t> m_refCount = { 0u };

    Rc<DxvkDevice>      m_device;

    VkQueryType         m_type  = VK_QUERY_TYPE_MAX_ENUM;
    VkQueryControlFlags m_flags = 0u;
    uint32_t            m_index = 0u;

    pipe_query*         m_query = nullptr;
    std::atomic<bool>   m_ended = { false };
    bool                m_available = false;
    DxvkQueryData       m_queryData = { };

  };

}
