// dirk's host cube: D3D11 through dirk on wink's WebGPU gallium driver, in
// one wasm module, with no guest. It draws a textured cube for a few frames,
// reads the back buffer back through a staging texture and checks it.
//
// Built with DIRK_WIRE_TEST, the cube's calls go through the wire instead:
// the guest's proxies write records, which the host's decoder reads and
// calls DXVK with, all in this module.
//
// Entered from the page through WebAssembly.promising (readbacks suspend).

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <emscripten/emscripten.h>
#include <webgpu/webgpu.h>

#include "wink.h"
#include "gex_wgpu_public.h"
#include "../../src/dirk/dirk.h"

#include "cube_vs.h"
#include "cube_ps.h"

extern "C" void* wink_js_create_surface(const char* selector);
extern "C" void dirk_test_publish(void);

#ifdef DIRK_WIRE_TEST
#include "../../src/dirk/wire/dirk_wire_host.h"
extern "C" {
  struct dirk_transport {
    const uint8_t* (*send)(void* ctx, const uint8_t* records, size_t len, int wait);
    void* ctx;
    void (*window)(void* ctx, uint64_t window);
  };
  void dirk_guest_init(const dirk_transport* transport);
  HRESULT dirk_D3D11CreateDeviceAndSwapChain(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
}

namespace {
  dirk_wire_client* g_client;
  uint64_t g_sent_bytes, g_sends, g_waits;

  const uint8_t* wireSend(void*, const uint8_t* records, size_t len, int wait) {
    static const uint8_t none[4] = { };
    const uint8_t* reply;
    size_t reply_len;
    g_sent_bytes += len;
    g_sends++;
    g_waits += wait != 0;
    if (!dirk_wire_client_submit(g_client, records, len, &reply, &reply_len)) {
      std::printf("FAIL the host refused the stream\n");
      return nullptr;
    }
    return wait ? reply : none;
  }
}
#define D3D11CreateDeviceAndSwapChain dirk_D3D11CreateDeviceAndSwapChain
#endif

namespace {

  constexpr uint32_t W = 256, H = 192;

  WGPUDevice  g_device;
  WGPUSurface g_canvas;
  bool        g_configured;
  int         g_failures;

  #define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; std::printf("FAIL " __VA_ARGS__); std::printf("\n"); } } while (0)

  bool hostWindowSize(void* window, uint32_t* w, uint32_t* h) {
    if (window != reinterpret_cast<void*>(1))
      return false;
    *w = W;
    *h = H;
    return true;
  }

  void hostDisplayMode(uint32_t* w, uint32_t* h, uint32_t* mhz) {
    *w = 1920;
    *h = 1080;
    *mhz = 60000;
  }

  // The back buffer is RGBA8, as the canvas is configured: no conversion.
  uint32_t hostSurfaceFormat(void*) {
    return 0;
  }

  void hostPresent(void* window, pipe_resource* image, uint32_t w, uint32_t h, uint32_t) {
    // DIRK_TEST_NO_PRESENT=1: draw and check without the canvas, for a
    // headless Chrome on SwiftShader, which loses the device at the first
    // getCurrentTexture.
    WGPUTexture from = gex_wgpu_resource_texture(image);
    if (!from || !g_canvas || std::getenv("DIRK_TEST_NO_PRESENT"))
      return;

    if (!g_configured) {
      WGPUSurfaceConfiguration cfg = { };
      cfg.device = g_device;
      cfg.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
      cfg.format = WGPUTextureFormat_RGBA8Unorm;
      cfg.width = W;
      cfg.height = H;
      cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
      cfg.presentMode = WGPUPresentMode_Fifo;
      wgpuSurfaceConfigure(g_canvas, &cfg);
      g_configured = true;
    }

    WGPUSurfaceTexture current = { };
    wgpuSurfaceGetCurrentTexture(g_canvas, &current);
    if (!current.texture)
      return;

    WGPUExtent3D extent = { w, h, 1 };
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = from;
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = current.texture;
    wgpuCommandEncoderCopyTextureToTexture(encoder, &src, &dst, &extent);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    WGPUQueue queue = wgpuDeviceGetQueue(g_device);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuQueueRelease(queue);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureRelease(current.texture);
  }

  const dirk_host g_host = {
    hostWindowSize, hostDisplayMode, hostSurfaceFormat, hostPresent,
  };

  struct Vertex {
    float pos[3];
    float color[3];
    float uv[2];
  };

  struct Frame {
    float mvp[16];
    float tint[4];
  };

  void multiply(float* r, const float* a, const float* b) {
    float t[16];
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        t[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j]
                     + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    std::memcpy(r, t, sizeof(t));
  }

  // Row-major, column vectors (mul(mvp, v) with the cbuffer's column-major
  // default packing reads it transposed, so this is stored transposed).
  void makeMvp(float* out, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float ry[16] = { c, 0, s, 0,  0, 1, 0, 0,  -s, 0, c, 0,  0, 0, 0, 1 };
    const float c2 = std::cos(0.5f), s2 = std::sin(0.5f);
    const float rx[16] = { 1, 0, 0, 0,  0, c2, -s2, 0,  0, s2, c2, 0,  0, 0, 0, 1 };
    const float tr[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 4.0f,  0, 0, 0, 1 };
    const float f = 1.0f / std::tan(0.5f), aspect = float(W) / float(H), n = 0.1f, fa = 100.0f;
    const float pr[16] = { f / aspect, 0, 0, 0,  0, f, 0, 0,
      0, 0, fa / (fa - n), -n * fa / (fa - n),  0, 0, 1, 0 };
    float m[16];
    multiply(m, rx, ry);
    multiply(m, tr, m);
    multiply(m, pr, m);
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        out[j * 4 + i] = m[i * 4 + j];
  }

  int run() {
    wink_init_attribs ia;
    wink_init_attribs_default(&ia);
    int rc = wink_init(&ia);
    if (rc != WINK_OK) {
      std::printf("FAIL wink_init: %s\n", wink_status_string(rc));
      return 1;
    }
    g_device = static_cast<WGPUDevice>(wink_webgpu_device());
    pipe_screen* screen = gex_wgpu_screen_create(g_device);
    if (!screen) {
      std::printf("FAIL no gallium screen\n");
      return 1;
    }
    g_canvas = static_cast<WGPUSurface>(wink_js_create_surface("#canvas"));
    dirk_init(screen, &g_host);
#ifdef DIRK_WIRE_TEST
    g_client = dirk_wire_client_create(nullptr);
    const dirk_transport transport = { wireSend, nullptr };
    dirk_guest_init(&transport);
    std::printf("dirk: through the wire\n");
#endif

    DXGI_SWAP_CHAIN_DESC sd = { };
    sd.BufferDesc.Width = W;
    sd.BufferDesc.Height = H;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = reinterpret_cast<HWND>(1);
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swapchain = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
      &fl, 1, D3D11_SDK_VERSION, &sd, &swapchain, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
      std::printf("FAIL D3D11CreateDeviceAndSwapChain: 0x%08x\n", unsigned(hr));
      return 1;
    }
    std::printf("dirk: device and swap chain made\n");

    ID3D11Texture2D* back = nullptr;
    swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(back, nullptr, &rtv);

    D3D11_TEXTURE2D_DESC dd = { };
    dd.Width = W;
    dd.Height = H;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth = nullptr;
    dev->CreateTexture2D(&dd, nullptr, &depth);
    ID3D11DepthStencilView* dsv = nullptr;
    if (depth)
      dev->CreateDepthStencilView(depth, nullptr, &dsv);
    CHECK(dsv, "no depth view");

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    CHECK(SUCCEEDED(dev->CreateVertexShader(cube_vs, sizeof(cube_vs), nullptr, &vs)), "vertex shader");
    CHECK(SUCCEEDED(dev->CreatePixelShader(cube_ps, sizeof(cube_ps), nullptr, &ps)), "pixel shader");

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
      { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout* il = nullptr;
    CHECK(SUCCEEDED(dev->CreateInputLayout(layout, 3, cube_vs, sizeof(cube_vs), &il)), "input layout");

    // Each face its own colour, uv over the face.
    static const float faces[6][3] = { {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1} };
    Vertex verts[24];
    uint16_t idx[36];
    for (int f = 0; f < 6; f++) {
      const int axis = f / 2;
      const float sign = (f & 1) ? -1.0f : 1.0f;
      for (int v = 0; v < 4; v++) {
        float u = (v & 1) ? 1.0f : -1.0f, w = (v & 2) ? 1.0f : -1.0f;
        float p[3];
        p[axis] = sign;
        p[(axis + 1) % 3] = u * sign;
        p[(axis + 2) % 3] = w;
        Vertex& vx = verts[f * 4 + v];
        std::memcpy(vx.pos, p, sizeof(p));
        std::memcpy(vx.color, faces[f], sizeof(vx.color));
        vx.uv[0] = (u + 1.0f) * 0.5f;
        vx.uv[1] = (w + 1.0f) * 0.5f;
      }
      const uint16_t b = uint16_t(f * 4);
      const uint16_t q[6] = { b, uint16_t(b + 1), uint16_t(b + 2), uint16_t(b + 2), uint16_t(b + 1), uint16_t(b + 3) };
      std::memcpy(&idx[f * 6], q, sizeof(q));
    }

    D3D11_BUFFER_DESC bd = { };
    D3D11_SUBRESOURCE_DATA init = { };
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    init.pSysMem = verts;
    ID3D11Buffer* vb = nullptr;
    dev->CreateBuffer(&bd, &init, &vb);

    bd.ByteWidth = sizeof(idx);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    init.pSysMem = idx;
    ID3D11Buffer* ib = nullptr;
    dev->CreateBuffer(&bd, &init, &ib);

    bd.ByteWidth = sizeof(Frame);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr;
    dev->CreateBuffer(&bd, nullptr, &cb);
    CHECK(vb && ib && cb, "buffers");

    // A 4x4 checker of white and grey.
    uint32_t texels[16];
    for (int i = 0; i < 16; i++)
      texels[i] = ((i ^ (i >> 2)) & 1) ? 0xffffffffu : 0xff808080u;
    D3D11_TEXTURE2D_DESC td = { };
    td.Width = 4;
    td.Height = 4;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA tinit = { texels, 16, 0 };
    ID3D11Texture2D* tex = nullptr;
    dev->CreateTexture2D(&td, &tinit, &tex);
    ID3D11ShaderResourceView* srv = nullptr;
    if (tex)
      dev->CreateShaderResourceView(tex, nullptr, &srv);

    D3D11_SAMPLER_DESC smp = { };
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* sampler = nullptr;
    dev->CreateSamplerState(&smp, &sampler);
    CHECK(srv && sampler, "texture");

    D3D11_RASTERIZER_DESC rd = { };
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rs = nullptr;
    dev->CreateRasterizerState(&rd, &rs);

    D3D11_TEXTURE2D_DESC sd2 = { };
    back->GetDesc(&sd2);
    sd2.Usage = D3D11_USAGE_STAGING;
    sd2.BindFlags = 0;
    sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd2.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&sd2, nullptr, &staging);
    CHECK(staging, "staging texture");

    const float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
    const UINT stride = sizeof(Vertex), offset = 0;
    D3D11_VIEWPORT vp = { 0, 0, float(W), float(H), 0, 1 };

    const int frames = 60;
    double t0 = emscripten_get_now();
    for (int frame = 0; frame < frames; frame++) {
      D3D11_MAPPED_SUBRESOURCE m = { };
      if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        Frame* fr = static_cast<Frame*>(m.pData);
        makeMvp(fr->mvp, 0.6f + 0.05f * frame);
        fr->tint[0] = fr->tint[1] = fr->tint[2] = fr->tint[3] = 1.0f;
        ctx->Unmap(cb, 0);
      }

      ctx->OMSetRenderTargets(1, &rtv, dsv);
      ctx->ClearRenderTargetView(rtv, clear);
      if (dsv)
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
      ctx->RSSetViewports(1, &vp);
      ctx->RSSetState(rs);
      ctx->IASetInputLayout(il);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
      ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
      ctx->VSSetShader(vs, nullptr, 0);
      ctx->VSSetConstantBuffers(0, 1, &cb);
      ctx->PSSetShader(ps, nullptr, 0);
      ctx->PSSetConstantBuffers(0, 1, &cb);
      ctx->PSSetShaderResources(0, 1, &srv);
      ctx->PSSetSamplers(0, 1, &sampler);
      ctx->DrawIndexed(36, 0, 0);

      if (frame == frames - 1 && staging) {
        ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE r = { };
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &r))) {
          auto px = [&] (uint32_t x, uint32_t y) {
            return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(r.pData) + y * r.RowPitch + x * 4);
          };
          const uint32_t corner = px(2, 2), center = px(W / 2, H / 2);
          std::printf("dirk: corner %08x center %08x\n", corner, center);
          auto near = [] (uint32_t a, uint32_t b) {
            for (int c = 0; c < 32; c += 8)
              if (std::abs(int((a >> c) & 0xff) - int((b >> c) & 0xff)) > 1)
                return false;
            return true;
          };
          CHECK(near(corner, 0xff4d331au), "corner is %08x, not the clear colour", corner);
          CHECK(center != corner && (center >> 24) == 0xffu, "center is %08x: nothing drawn", center);
          ctx->Unmap(staging, 0);
        } else {
          CHECK(false, "staging map");
        }
      }

      hr = swapchain->Present(1, 0);
      CHECK(SUCCEEDED(hr), "present 0x%08x", unsigned(hr));
      wink_yield_frames(1);
    }
    double t1 = emscripten_get_now();
    std::printf("dirk: %d frames in %.1f ms\n", frames, t1 - t0);

    ctx->ClearState();
    ctx->Flush();
    swapchain->Release();
    ctx->Release();
    std::printf("dirk: device refs left %lu\n", dev->Release());
#ifdef DIRK_WIRE_TEST
    std::printf("dirk wire: %llu bytes in %llu sends, %llu of them waiting\n",
      (unsigned long long)g_sent_bytes, (unsigned long long)g_sends, (unsigned long long)g_waits);
    dirk_wire_client_destroy(g_client);
#endif
    return g_failures;
  }

}

extern "C" EMSCRIPTEN_KEEPALIVE int dirk_cube_main(void) {
  dirk_test_publish();
  int failures = run();
  std::printf(failures ? "dirk cube: %d failures\n" : "dirk cube: pass\n", failures);
  return failures;
}
