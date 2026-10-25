#include "dxvk_cs.h"

namespace dxvk {
  
  DxvkCsChunk::DxvkCsChunk() {
    
  }
  
  
  DxvkCsChunk::~DxvkCsChunk() {
    this->reset();
  }
  
  
  void DxvkCsChunk::init(DxvkCsChunkFlags flags) {
    m_flags = flags;
  }


  void DxvkCsChunk::executeAll(DxvkContext* ctx) {
    auto cmd = m_head;
    
    if (m_flags.test(DxvkCsChunkFlag::SingleUse)) {
      m_commandOffset = 0;
      
      while (cmd != nullptr) {
        auto next = cmd->next();
        cmd->exec(ctx);
        cmd->~DxvkCsCmd();
        cmd = next;
      }

      m_head = nullptr;
      m_next = &m_head;
    } else {
      while (cmd != nullptr) {
        cmd->exec(ctx);
        cmd = cmd->next();
      }
    }
  }
  
  
  void DxvkCsChunk::reset() {
    auto cmd = m_head;

    while (cmd != nullptr) {
      auto next = cmd->next();
      cmd->~DxvkCsCmd();
      cmd = next;
    }
    
    m_head = nullptr;
    m_next = &m_head;

    m_commandOffset = 0;
  }
  
  
  DxvkCsChunkPool::DxvkCsChunkPool() {
    
  }
  
  
  DxvkCsChunkPool::~DxvkCsChunkPool() {
    for (DxvkCsChunk* chunk : m_chunks)
      delete chunk;
  }
  
  
  DxvkCsChunk* DxvkCsChunkPool::allocChunk(DxvkCsChunkFlags flags) {
    DxvkCsChunk* chunk = nullptr;

    { std::lock_guard<dxvk::mutex> lock(m_mutex);
      
      if (m_chunks.size() != 0) {
        chunk = m_chunks.back();
        m_chunks.pop_back();
      }
    }
    
    if (!chunk)
      chunk = new DxvkCsChunk();
    
    chunk->init(flags);
    return chunk;
  }
  
  
  void DxvkCsChunkPool::freeChunk(DxvkCsChunk* chunk) {
    chunk->reset();
    
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_chunks.push_back(chunk);
  }
  
  
  DxvkCsThread::DxvkCsThread(
    const Rc<DxvkDevice>&   device,
    const Rc<DxvkContext>&  context)
  : m_device(device), m_context(context) { }


  DxvkCsThread::~DxvkCsThread() { }


  uint64_t DxvkCsThread::dispatchChunk(DxvkCsChunkRef&& chunk) {
    chunk->executeAll(m_context.ptr());
    chunk = DxvkCsChunkRef();
    return ++m_seq;
  }


  void DxvkCsThread::injectChunk(DxvkCsQueue queue, DxvkCsChunkRef&& chunk, bool synchronize) {
    chunk->executeAll(m_context.ptr());
    chunk = DxvkCsChunkRef();
  }

}
