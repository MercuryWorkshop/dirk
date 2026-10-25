// dirk's host side: a client's handle table, the stream's reading, and the
// records the generator leaves to it (dirk_wire.h's special ops).
#include <cstdio>
#include <cstdlib>

#include "dirk_host.h"

extern "C" {
#include "dirk_format.h"
}

extern "C" HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory);

namespace dirk {

  // A record's scratch memory, at most: more is a guest's mistake.
  constexpr uint64_t MaxScratch = uint64_t(1) << 30;

  static bool debug() {
    static const bool on = std::getenv("DIRK_WIRE_DEBUG") != nullptr;
    return on;
  }

  const char* DirkReader::string() {
    uint32_t n = u32();
    if (n == ~0u)
      return nullptr;
    const uint8_t* p = take(n);
    char* s = static_cast<char*>(m_c.scratch(uint64_t(n) + 1));
    if (p && s)
      std::memcpy(s, p, n);
    return s;
  }

  const WCHAR* DirkReader::wstring() {
    uint32_t n = u32();
    if (n == ~0u)
      return nullptr;
    const uint8_t* p = take(uint64_t(n) * 2);
    WCHAR* s = static_cast<WCHAR*>(m_c.scratch((uint64_t(n) + 1) * sizeof(WCHAR)));
    for (uint32_t i = 0; p && s && i < n; i++) {
      uint16_t c;
      std::memcpy(&c, p + 2 * i, 2);
      s[i] = c;
    }
    return s;
  }

  void DirkWriter::new_riid(Client& c, const GUID&, void* p) {
    u32(c.handle_for(p));
  }

  Client::~Client() {
    for (auto& e : m_objects) {
      if (e.second)
        static_cast<IUnknown*>(e.second)->Release();
    }
  }

  void* Client::scratch(uint64_t n) {
    if (n == ~uint64_t(0) || n > MaxScratch)
      return nullptr;
    auto mem = std::make_unique<uint8_t[]>(n ? n : 1);
    std::memset(mem.get(), 0, n ? n : 1);
    void* p = mem.get();
    m_scratch.push_back(std::move(mem));
    return p;
  }

  void* Client::copy(const void* p, uint64_t n) {
    void* d = p ? scratch(n) : nullptr;
    if (d)
      std::memcpy(d, p, n);
    return d;
  }

  void Client::bind(uint32_t handle, void* object, int) {
    if (!handle || handle >= DIRK_HOST_HANDLE || m_objects.count(handle)) {
      // Not a handle the guest may pick: the object is not kept.
      m_unknown = true;
      if (object)
        static_cast<IUnknown*>(object)->Release();
      return;
    }
    // A failed create keeps its handle, naming nothing, until the guest
    // releases it.
    m_objects.emplace(handle, object);
    if (object)
      m_handles.emplace(object, handle);
  }

  uint32_t Client::handle_for(void* object) {
    if (!object)
      return 0;
    auto h = m_handles.find(object);
    if (h != m_handles.end()) {
      static_cast<IUnknown*>(object)->Release();
      return h->second;
    }
    uint32_t handle = m_next_host++;
    if (m_next_host == 0)
      m_next_host = DIRK_HOST_HANDLE;
    m_objects.emplace(handle, object);
    m_handles.emplace(object, handle);
    return handle;
  }

  void Client::release(uint32_t handle) {
    auto e = m_objects.find(handle);
    if (e == m_objects.end()) {
      m_unknown = true;
      return;
    }
    void* object = e->second;
    m_objects.erase(e);
    if (!object)
      return;
    auto h = m_handles.find(object);
    if (h != m_handles.end() && h->second == handle)
      m_handles.erase(h);
    static_cast<IUnknown*>(object)->Release();
  }

  void Client::missing(const Record&) {
    m_missing = true;
  }

  void Client::failed(const Record& rec, HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
      m_removed = true;
    if (debug())
      std::fprintf(stderr, "dirk: op %u on %u failed: 0x%08x\n", rec.op, rec.handle, unsigned(hr));
  }

  // ------------------------------------------------------------ resources

  struct SubDims {
    uint32_t w = 0, h = 0, d = 0, format = 0;
    bool buffer = false;
  };

  // A subresource's size and format, from the host's own desc.
  static bool subDims(ID3D11Resource* res, UINT sub, SubDims* o) {
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    switch (dim) {
      case D3D11_RESOURCE_DIMENSION_BUFFER: {
        D3D11_BUFFER_DESC d;
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        o->w = d.ByteWidth; o->h = o->d = 1; o->buffer = true;
        return sub == 0;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC d;
        static_cast<ID3D11Texture1D*>(res)->GetDesc(&d);
        o->w = dirk_mip(d.Width, sub % d.MipLevels); o->h = o->d = 1; o->format = d.Format;
        return sub < d.MipLevels * d.ArraySize;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC d;
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        o->w = dirk_mip(d.Width, sub % d.MipLevels); o->h = dirk_mip(d.Height, sub % d.MipLevels);
        o->d = 1; o->format = d.Format;
        return sub < d.MipLevels * d.ArraySize;
      }
      case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC d;
        static_cast<ID3D11Texture3D*>(res)->GetDesc(&d);
        o->w = dirk_mip(d.Width, sub); o->h = dirk_mip(d.Height, sub); o->d = dirk_mip(d.Depth, sub);
        o->format = d.Format;
        return sub < d.MipLevels;
      }
      default:
        return false;
    }
  }

  static uint64_t dataSize(const SubDims& d, uint32_t row, uint32_t depth) {
    return d.buffer ? d.w : dirk_data_size(d.format, d.w, d.h, d.d, row, depth);
  }

  // A create's descs, by kind.
  union ResourceDesc {
    D3D11_BUFFER_DESC buffer;
    D3D11_TEXTURE1D_DESC tex1d;
    D3D11_TEXTURE2D_DESC tex2d;
    D3D11_TEXTURE3D_DESC tex3d;
    D3D11_TEXTURE2D_DESC1 tex2d1;
    D3D11_TEXTURE3D_DESC1 tex3d1;
  };

  // The subresources a create's initial data has, and each one's size.
  static uint32_t initialCount(uint32_t kind, const ResourceDesc& d) {
    switch (kind) {
      case DIRK_RESOURCE_BUFFER: return 1;
      case DIRK_RESOURCE_TEXTURE1D:
        return (d.tex1d.MipLevels ? d.tex1d.MipLevels : dirk_mip_count(d.tex1d.Width, 1, 1)) * d.tex1d.ArraySize;
      case DIRK_RESOURCE_TEXTURE2D:
      case DIRK_RESOURCE_TEXTURE2D1:
        return (d.tex2d.MipLevels ? d.tex2d.MipLevels : dirk_mip_count(d.tex2d.Width, d.tex2d.Height, 1)) *
               d.tex2d.ArraySize;
      default:
        return d.tex3d.MipLevels ? d.tex3d.MipLevels
                                 : dirk_mip_count(d.tex3d.Width, d.tex3d.Height, d.tex3d.Depth);
    }
  }

  static uint64_t initialSize(uint32_t kind, const ResourceDesc& d, uint32_t sub, uint32_t row, uint32_t depth) {
    SubDims s;
    switch (kind) {
      case DIRK_RESOURCE_BUFFER:
        return d.buffer.ByteWidth;
      case DIRK_RESOURCE_TEXTURE1D: {
        uint32_t mips = d.tex1d.MipLevels ? d.tex1d.MipLevels : dirk_mip_count(d.tex1d.Width, 1, 1);
        s.w = dirk_mip(d.tex1d.Width, sub % mips); s.h = s.d = 1; s.format = d.tex1d.Format;
        break;
      }
      case DIRK_RESOURCE_TEXTURE2D:
      case DIRK_RESOURCE_TEXTURE2D1: {
        uint32_t mips = d.tex2d.MipLevels ? d.tex2d.MipLevels : dirk_mip_count(d.tex2d.Width, d.tex2d.Height, 1);
        s.w = dirk_mip(d.tex2d.Width, sub % mips); s.h = dirk_mip(d.tex2d.Height, sub % mips);
        s.d = 1; s.format = d.tex2d.Format;
        break;
      }
      default:
        s.w = dirk_mip(d.tex3d.Width, sub); s.h = dirk_mip(d.tex3d.Height, sub);
        s.d = dirk_mip(d.tex3d.Depth, sub); s.format = d.tex3d.Format;
        break;
    }
    return dataSize(s, row, depth);
  }

  // ------------------------------------------------------------ specials

  bool Client::decode_special(const Record& rec, DirkReader& r, DirkWriter& w) {
    switch (rec.op) {
      case DIRK_OP_RELEASE:
        release(rec.handle);
        return true;

      case DIRK_OP_QUERY_INTERFACE: {
        GUID iid;
        r.bytes(&iid, 16);
        auto* self = obj<IUnknown>(rec.handle);
        if (!self) {
          missing(rec);
          return true;
        }
        void* p = nullptr;
        HRESULT hr = self->QueryInterface(iid, &p);
        w.val(&hr, sizeof(hr), 8);
        w.u32(SUCCEEDED(hr) ? handle_for(p) : 0);
        return true;
      }

      case DIRK_OP_DUPLICATE: {
        uint32_t handle = r.u32();
        auto* self = obj<IUnknown>(rec.handle);
        if (self)
          self->AddRef();
        bind(handle, self, 0);
        return true;
      }

      case DIRK_OP_CREATE_DEVICE: {
        auto* adapter = obj<IDXGIAdapter>(r.u32());
        auto driver = D3D_DRIVER_TYPE(r.u32());
        UINT flags = r.u32();
        UINT sdk = r.u32();
        uint32_t dev_h = r.u32();
        uint32_t ctx_h = r.u32();
        uint32_t levels_n = r.u32();
        auto* levels = levels_n == ~0u ? nullptr : out_array<D3D_FEATURE_LEVEL>(levels_n);
        for (uint32_t i = 0; levels && i < levels_n; i++)
          levels[i] = D3D_FEATURE_LEVEL(r.u32());
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL(0);
        HRESULT hr = D3D11CreateDevice(adapter, driver, nullptr, flags, levels,
                                       levels ? levels_n : 0, sdk, &dev, &level, &ctx);
        ID3D11Device5* dev5 = nullptr;
        ID3D11DeviceContext4* ctx4 = nullptr;
        if (SUCCEEDED(hr)) {
          dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&dev5));
          ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&ctx4));
          dev->Release();
          ctx->Release();
          if (!dev5 || !ctx4) {
            if (dev5) dev5->Release();
            if (ctx4) ctx4->Release();
            dev5 = nullptr;
            ctx4 = nullptr;
            hr = E_NOINTERFACE;
          }
        }
        bool ok = SUCCEEDED(hr);
        if (ok) {
          if (dev_h) bind(dev_h, dev5, DIRK_IFACE_ID3D11Device5); else dev5->Release();
          if (ctx_h) bind(ctx_h, ctx4, DIRK_IFACE_ID3D11DeviceContext4); else ctx4->Release();
        }
        w.val(&hr, sizeof(hr), 8);
        w.val(&level, sizeof(level), 4);
        w.u32(ok);
        return true;
      }

      case DIRK_OP_CREATE_FACTORY: {
        GUID iid;
        r.bytes(&iid, 16);
        UINT flags = r.u32();
        uint32_t h = r.u32();
        void* p = nullptr;
        HRESULT hr = CreateDXGIFactory2(flags, iid, &p);
        bool ok = SUCCEEDED(hr) && p;
        if (ok) {
          if (h) bind(h, p, 0); else static_cast<IUnknown*>(p)->Release();
        }
        w.val(&hr, sizeof(hr), 8);
        w.u32(ok);
        return true;
      }

      case DIRK_OP_CREATE_RESOURCE: {
        auto* self = obj<ID3D11Device3>(rec.handle);
        uint32_t kind = r.u32();
        ResourceDesc desc = { };
        switch (kind) {
          case DIRK_RESOURCE_BUFFER: dirk_get_D3D11_BUFFER_DESC(r, &desc.buffer); break;
          case DIRK_RESOURCE_TEXTURE1D: dirk_get_D3D11_TEXTURE1D_DESC(r, &desc.tex1d); break;
          case DIRK_RESOURCE_TEXTURE2D: dirk_get_D3D11_TEXTURE2D_DESC(r, &desc.tex2d); break;
          case DIRK_RESOURCE_TEXTURE3D: dirk_get_D3D11_TEXTURE3D_DESC(r, &desc.tex3d); break;
          case DIRK_RESOURCE_TEXTURE2D1: dirk_get_D3D11_TEXTURE2D_DESC1(r, &desc.tex2d1); break;
          case DIRK_RESOURCE_TEXTURE3D1: dirk_get_D3D11_TEXTURE3D_DESC1(r, &desc.tex3d1); break;
          default: return false;
        }
        uint32_t h = r.u32();
        uint32_t count = r.u32();
        D3D11_SUBRESOURCE_DATA* data = nullptr;
        bool short_data = false;
        if (count != ~0u) {
          if (count != initialCount(kind, desc))
            return false;
          data = out_array<D3D11_SUBRESOURCE_DATA>(count);
          for (uint32_t i = 0; data && i < count; i++) {
            data[i].SysMemPitch = r.u32();
            data[i].SysMemSlicePitch = r.u32();
            uint64_t n;
            data[i].pSysMem = r.blob(&n);
            if (!data[i].pSysMem || n < initialSize(kind, desc, i, data[i].SysMemPitch, data[i].SysMemSlicePitch))
              short_data = true;
          }
        }
        if (!self) {
          missing(rec);
          return true;
        }
        HRESULT hr = E_INVALIDARG;
        void* p = nullptr;
        void** out = h ? &p : nullptr;
        if (!short_data) {
          switch (kind) {
            case DIRK_RESOURCE_BUFFER:
              hr = self->CreateBuffer(&desc.buffer, data, reinterpret_cast<ID3D11Buffer**>(out)); break;
            case DIRK_RESOURCE_TEXTURE1D:
              hr = self->CreateTexture1D(&desc.tex1d, data, reinterpret_cast<ID3D11Texture1D**>(out)); break;
            case DIRK_RESOURCE_TEXTURE2D:
              hr = self->CreateTexture2D(&desc.tex2d, data, reinterpret_cast<ID3D11Texture2D**>(out)); break;
            case DIRK_RESOURCE_TEXTURE3D:
              hr = self->CreateTexture3D(&desc.tex3d, data, reinterpret_cast<ID3D11Texture3D**>(out)); break;
            case DIRK_RESOURCE_TEXTURE2D1:
              hr = self->CreateTexture2D1(&desc.tex2d1, data, reinterpret_cast<ID3D11Texture2D1**>(out)); break;
            case DIRK_RESOURCE_TEXTURE3D1:
              hr = self->CreateTexture3D1(&desc.tex3d1, data, reinterpret_cast<ID3D11Texture3D1**>(out)); break;
          }
        }
        if (FAILED(hr))
          failed(rec, hr);
        if (rec.wait)
          w.val(&hr, sizeof(hr), 8);
        if (h)
          bind(h, p, 0);
        return true;
      }

      case DIRK_OP_MAP: {
        auto* self = obj<ID3D11DeviceContext>(rec.handle);
        auto* res = obj<ID3D11Resource>(r.u32());
        UINT sub = r.u32();
        auto type = D3D11_MAP(r.u32());
        UINT flags = r.u32();
        SubDims d;
        if (!self || !res || !subDims(res, sub, &d)) {
          missing(rec);
          return true;
        }
        D3D11_MAPPED_SUBRESOURCE m = { };
        HRESULT hr = self->Map(res, sub, type, flags, &m);
        uint64_t size = 0;
        bool data = false;
        if (SUCCEEDED(hr)) {
          size = d.buffer ? d.w : uint64_t(m.DepthPitch) * d.d;
          if (!d.buffer && d.d == 1 && !m.DepthPitch)
            size = dataSize(d, m.RowPitch, 0);
          data = type != D3D11_MAP_WRITE_DISCARD && m.pData;
        }
        w.val(&hr, sizeof(hr), 8);
        w.u32(m.RowPitch);
        w.u32(m.DepthPitch);
        w.u64(size);
        w.u32(data);
        if (data)
          w.bytes(m.pData, size);
        if (SUCCEEDED(hr))
          self->Unmap(res, sub);
        return true;
      }

      case DIRK_OP_UNMAP: {
        auto* self = obj<ID3D11DeviceContext>(rec.handle);
        auto* res = obj<ID3D11Resource>(r.u32());
        UINT sub = r.u32();
        auto type = D3D11_MAP(r.u32());
        uint32_t row = r.u32();
        uint32_t depth = r.u32();
        uint64_t len;
        const void* bytes = r.blob(&len);
        SubDims d;
        if (!self || !res || !bytes || !subDims(res, sub, &d)) {
          missing(rec);
          return true;
        }
        D3D11_MAPPED_SUBRESOURCE m = { };
        HRESULT hr = self->Map(res, sub, type, 0, &m);
        if (FAILED(hr)) {
          failed(rec, hr);
          return true;
        }
        if (d.buffer) {
          std::memcpy(m.pData, bytes, len < d.w ? len : d.w);
        } else {
          // Row by row, in case the pitches differ.
          struct dirk_format_size f = dirk_format_size(d.format);
          uint32_t rows = (d.h + f.block_h - 1) / f.block_h;
          uint64_t row_bytes = uint64_t((d.w + f.block_w - 1) / f.block_w) * f.block_bytes;
          for (uint32_t z = 0; z < d.d; z++) {
            for (uint32_t y = 0; y < rows; y++) {
              uint64_t from = uint64_t(z) * depth + uint64_t(y) * row;
              if (from + row_bytes > len)
                break;
              std::memcpy(static_cast<uint8_t*>(m.pData) + uint64_t(z) * m.DepthPitch + uint64_t(y) * m.RowPitch,
                          static_cast<const uint8_t*>(bytes) + from, row_bytes);
            }
          }
        }
        self->Unmap(res, sub);
        return true;
      }

      case DIRK_OP_UPDATE_SUBRESOURCE: {
        auto* self = obj<ID3D11DeviceContext1>(rec.handle);
        auto* res = obj<ID3D11Resource>(r.u32());
        UINT sub = r.u32();
        bool has_box = r.u32();
        D3D11_BOX box;
        box.left = r.u32(); box.top = r.u32(); box.front = r.u32();
        box.right = r.u32(); box.bottom = r.u32(); box.back = r.u32();
        uint32_t row = r.u32();
        uint32_t depth = r.u32();
        uint32_t flags = r.u32();
        uint64_t len;
        const void* bytes = r.blob(&len);
        SubDims d;
        if (!self || !res || !bytes || !subDims(res, sub, &d)) {
          missing(rec);
          return true;
        }
        if (has_box) {
          if (box.right > d.w || box.bottom > d.h || box.back > d.d ||
              box.right <= box.left || box.bottom <= box.top || box.back <= box.front)
            return true;
          d.w = box.right - box.left;
          d.h = box.bottom - box.top;
          d.d = box.back - box.front;
        }
        if (len < dataSize(d, row, depth))
          return true;
        self->UpdateSubresource1(res, sub, has_box ? &box : nullptr, bytes, row, depth, flags);
        return true;
      }

      case DIRK_OP_SURFACE: {
        uint64_t window = r.u64();
        uint32_t kind = r.u32();
        uint32_t id = r.u32();
        uint32_t width = r.u32();
        uint32_t height = r.u32();
        if (m_ops.surface)
          m_ops.surface(m_ops.user, window, kind, id, width, height);
        return true;
      }
    }
    return false;
  }

  bool Client::decode(const Record& rec, DirkReader& r, DirkWriter& w) {
    if (rec.op < DIRK_OP_GENERATED)
      return decode_special(rec, r, w);
    return decode_generated(*this, rec, r, w);
  }

  bool Client::submit(const uint8_t* records, size_t len, const uint8_t** reply, size_t* reply_len) {
    size_t at = 0;
    *reply = nullptr;
    *reply_len = 0;
    while (at < len) {
      Record rec;
      if (len - at < DIRK_REC_HEADER)
        return false;
      std::memcpy(&rec.size, records + at, 4);
      std::memcpy(&rec.op, records + at + 4, 2);
      std::memcpy(&rec.flags, records + at + 6, 2);
      std::memcpy(&rec.handle, records + at + 8, 4);
      if (rec.size < DIRK_REC_HEADER || rec.size > len - at)
        return false;
      rec.wait = rec.flags & DIRK_REC_WAIT;
      if (rec.wait && at + rec.size != len)
        return false;

      m_reply.clear();
      m_reply.resize(4);
      m_missing = false;
      m_unknown = false;
      DirkReader r(*this, records + at + DIRK_REC_HEADER, records + at + rec.size);
      DirkWriter w(m_reply);
      bool known = true;
      // A lost device takes calls no more; a release still frees.
      if (!m_removed || rec.op == DIRK_OP_RELEASE)
        known = decode(rec, r, w);
      m_scratch.clear();
      if (!known || r.bad()) {
        if (debug())
          std::fprintf(stderr, "dirk: a malformed record, op %u\n", rec.op);
        return false;
      }
      if (debug() && (m_unknown || m_missing))
        std::fprintf(stderr, "dirk: op %u names an object not there\n", rec.op);
      if (rec.wait) {
        uint32_t status = m_missing || m_removed ? DIRK_REPLY_REMOVED : DIRK_REPLY_OK;
        if (status != DIRK_REPLY_OK)
          m_reply.resize(4);
        std::memcpy(m_reply.data(), &status, 4);
        *reply = m_reply.data();
        *reply_len = m_reply.size();
      }
      at += rec.size;
    }
    return true;
  }

}

extern "C" {

struct dirk_wire_client* dirk_wire_client_create(const struct dirk_wire_host_ops* ops) {
  dirk_wire_host_ops none = { };
  return reinterpret_cast<dirk_wire_client*>(new dirk::Client(ops ? *ops : none));
}

bool dirk_wire_client_submit(struct dirk_wire_client* client, const uint8_t* records, size_t len,
                             const uint8_t** reply, size_t* reply_len) {
  // DXVK's exceptions end here: the caller is built without them.
  try {
    return reinterpret_cast<dirk::Client*>(client)->submit(records, len, reply, reply_len);
  } catch (...) {
    std::fprintf(stderr, "dirk: a D3D call threw; the stream is refused\n");
    *reply = nullptr;
    *reply_len = 0;
    return false;
  }
}

void dirk_wire_client_destroy(struct dirk_wire_client* client) {
  delete reinterpret_cast<dirk::Client*>(client);
}

}
