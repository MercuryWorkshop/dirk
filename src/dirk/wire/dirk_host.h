// dirk's host side: what the generated decoder (dirk_wire_host.cpp) and the
// hand-written records (dirk_host.cpp) share.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <d3d11_4.h>
#include <dxgi1_6.h>

extern "C" {
#include "dirk_wire.h"
}
#include "dirk_wire_host.h"

namespace dirk {

  class Client;

  struct Record {
    uint32_t size;
    uint16_t op;
    uint16_t flags;
    uint32_t handle;
    bool wait;
  };

  // Reads a record's parameters. Past the record's end it reads zeros and
  // remembers that it did.
  class DirkReader {
  public:
    DirkReader(Client& c, const uint8_t* p, const uint8_t* end) : m_c(c), m_p(p), m_end(end) { }

    bool bad() const { return m_bad; }

    const uint8_t* take(size_t n) {
      if (size_t(m_end - m_p) < n) {
        m_bad = true;
        m_p = m_end;
        return nullptr;
      }
      const uint8_t* p = m_p;
      m_p += n;
      return p;
    }

    void bytes(void* dst, size_t n) {
      const uint8_t* p = take(n);
      if (p)
        std::memcpy(dst, p, n);
      else
        std::memset(dst, 0, n);
    }

    void val(void* dst, size_t size, size_t width) {
      const uint8_t* p = take(width);
      if (p)
        dirk_get_val(p, dst, size, width);
      else
        std::memset(dst, 0, size);
    }

    uint32_t u32() { uint32_t v; val(&v, 4, 4); return v; }
    uint64_t u64() { uint64_t v; val(&v, 8, 8); return v; }
    void skip(size_t n) { take(n); }

    // A blob in the record: NULL for a NULL pointer.
    const void* blob(uint64_t* len = nullptr) {
      uint64_t n = u64();
      if (len)
        *len = n == ~uint64_t(0) ? 0 : n;
      if (n == ~uint64_t(0))
        return nullptr;
      const uint8_t* p = take(n);
      if (!p && len)
        *len = 0;
      return p;
    }

    const char* string();
    const WCHAR* wstring();

    template<typename T>
    T* vals(Client& c, size_t width);

  private:
    Client& m_c;
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_bad = false;
  };

  class DirkWriter {
  public:
    explicit DirkWriter(std::vector<uint8_t>& out) : m_out(out) { }

    void bytes(const void* p, size_t n) {
      const uint8_t* b = static_cast<const uint8_t*>(p);
      m_out.insert(m_out.end(), b, b + n);
    }

    void val(const void* v, size_t size, size_t width) {
      size_t at = m_out.size();
      m_out.resize(at + width);
      dirk_put_val(m_out.data() + at, v, size, width);
    }

    void u32(uint32_t v) { bytes(&v, 4); }
    void u64(uint64_t v) { bytes(&v, 8); }

    void blob(const void* p, uint64_t n) {
      if (!p) {
        u64(~uint64_t(0));
        return;
      }
      u64(n);
      bytes(p, n);
    }

    void string(const char* s, int) {
      if (!s) {
        u32(~0u);
        return;
      }
      uint32_t n = uint32_t(std::strlen(s));
      u32(n);
      bytes(s, n);
    }

    void vals(const void* p, size_t size, size_t width, uint32_t n) {
      u32(p ? n : 0);
      for (uint32_t i = 0; p && i < n; i++)
        val(static_cast<const uint8_t*>(p) + i * size, size, width);
    }

    template<typename T>
    void out_objs(Client& c, T** objs, uint32_t n, int iface);

    void new_riid(Client& c, const GUID& riid, void* p);

  private:
    std::vector<uint8_t>& m_out;
  };

  class Client {
  public:
    explicit Client(const dirk_wire_host_ops& ops) : m_ops(ops) { }
    ~Client();

    bool submit(const uint8_t* records, size_t len, const uint8_t** reply, size_t* reply_len);

    // The object a handle names, as T. NULL for 0 or a handle not known.
    template<typename T>
    T* obj(uint32_t handle) {
      if (!handle)
        return nullptr;
      auto e = m_objects.find(handle);
      if (e == m_objects.end()) {
        m_unknown = true;
        return nullptr;
      }
      return static_cast<T*>(e->second);
    }

    template<typename T>
    T** objs(DirkReader& r) {
      uint32_t n = r.u32();
      if (n == ~0u)
        return nullptr;
      T** a = out_objs<T>(n);
      for (uint32_t i = 0; a && i < n; i++)
        a[i] = obj<T>(r.u32());
      return a;
    }

    template<typename T>
    T** out_objs(uint32_t n) {
      return n == ~0u ? nullptr : static_cast<T**>(scratch(uint64_t(n) * sizeof(T*)));
    }

    template<typename T>
    T* out_array(uint32_t n) {
      return n == ~0u ? nullptr : static_cast<T*>(scratch(uint64_t(n) * sizeof(T)));
    }

    // Zeroed memory for this record. NULL for ~0 or for too much.
    void* scratch(uint64_t n);
    void* copy(const void* p, uint64_t n);

    // The object a create made, under the handle the guest picked. Keeps the
    // create's reference.
    void bind(uint32_t handle, void* object, int iface);

    // The handle for an object the host hands out with a reference: the one
    // it has (the reference dropped), or a new one keeping it.
    uint32_t handle_for(void* object);

    void release(uint32_t handle);

    void missing(const Record& rec);
    void verify(const void* have, const void* want, uint64_t len, uint64_t offset);
    void failed(const Record& rec, HRESULT hr);

    const dirk_wire_host_ops& ops() const { return m_ops; }

  private:
    bool decode(const Record& rec, DirkReader& r, DirkWriter& w);
    bool decode_special(const Record& rec, DirkReader& r, DirkWriter& w);

    dirk_wire_host_ops m_ops;
    std::unordered_map<uint32_t, void*> m_objects;
    std::unordered_map<void*, uint32_t> m_handles;
    uint32_t m_next_host = DIRK_HOST_HANDLE;
    std::vector<std::unique_ptr<uint8_t[]>> m_scratch;
    std::vector<uint8_t> m_reply;
    bool m_unknown = false;
    bool m_missing = false;
    uint64_t m_verified = 0, m_verified_bytes = 0, m_mismatched = 0;
    bool m_removed = false;
  };

  template<typename T>
  T* DirkReader::vals(Client& c, size_t width) {
    uint32_t n = u32();
    if (n == ~0u)
      return nullptr;
    T* a = c.out_array<T>(n);
    for (uint32_t i = 0; i < n; i++) {
      if (a)
        val(&a[i], sizeof(T), width);
      else
        skip(width);
    }
    return a;
  }

  template<typename T>
  void DirkWriter::out_objs(Client& c, T** objs, uint32_t n, int) {
    if (!objs || n == ~0u) {
      u32(0);
      return;
    }
    u32(n);
    for (uint32_t i = 0; i < n; i++)
      u32(c.handle_for(objs[i]));
  }

  bool decode_generated(Client& c, const Record& rec, DirkReader& r, DirkWriter& w);

  void dirk_get_D3D11_BUFFER_DESC(DirkReader& r, D3D11_BUFFER_DESC* v);
  void dirk_get_D3D11_TEXTURE1D_DESC(DirkReader& r, D3D11_TEXTURE1D_DESC* v);
  void dirk_get_D3D11_TEXTURE2D_DESC(DirkReader& r, D3D11_TEXTURE2D_DESC* v);
  void dirk_get_D3D11_TEXTURE3D_DESC(DirkReader& r, D3D11_TEXTURE3D_DESC* v);
  void dirk_get_D3D11_TEXTURE2D_DESC1(DirkReader& r, D3D11_TEXTURE2D_DESC1* v);
  void dirk_get_D3D11_TEXTURE3D_DESC1(DirkReader& r, D3D11_TEXTURE3D_DESC1* v);

}
