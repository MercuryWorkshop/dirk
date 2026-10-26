// Streaming through no-overwrite maps, as a game's sprite batcher does: a
// dynamic vertex buffer, index buffer and instance buffer, each a ring that
// one discard fills with junk and no-overwrite maps then append to. Columns
// 0-7 of a 16x16 target are one indexed quad each (base vertex, start index
// and start instance all moving through the rings); columns 8-15 are one
// instanced draw whose per-instance offsets and colours come from the
// instance ring. Prints "ring: pass", or a FAIL line with the first pixel
// that is wrong.
#include <array>
#include <cstring>

#include <d3dcompiler.h>
#include <d3d11.h>

#include <windows.h>

#include "test_utils.h"

using namespace dxvk;

static const char g_vs[] =
  "struct I { float2 pos : POSITION; float4 col : COLOR; float2 off : OFFSET; float4 icol : ICOLOR; };\n"
  "struct O { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
  "O main(I i) { O o; o.pos = float4(i.pos + i.off, 0, 1); o.col = i.col + i.icol; return o; }\n";

static const char g_ps[] =
  "float4 main(float4 pos : SV_POSITION, float4 col : COLOR) : SV_TARGET { return col; }\n";

struct Vertex { float x, y, r, g, b, a; };
struct Instance { float x, y, r, g, b, a; };

static int fail(const char *what) {
  std::cout << "ring: FAIL " << what << std::endl;
  return 1;
}

static uint32_t expected(uint32_t x) {
  // Column x's colour as the target stores it (R8G8B8A8, little-endian).
  uint32_t r = x < 8 ? 16 * (x + 1) : 0, g = x < 8 ? 0 : 16 * (x - 7), b = 64 + 8 * x;
  return 0xff000000u | (b << 16) | (g << 8) | r;
}

static float unorm(uint32_t v) { return float(v) / 255.0f; }

template<typename T>
static bool append(ID3D11DeviceContext *ctx, ID3D11Buffer *buf, UINT &at, const T *items, UINT count) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(buf, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m)))
    return false;
  std::memcpy(static_cast<uint8_t *>(m.pData) + at * sizeof(T), items, count * sizeof(T));
  ctx->Unmap(buf, 0);
  at += count;
  return true;
}

static bool junk(ID3D11DeviceContext *ctx, ID3D11Buffer *buf, UINT size) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    return false;
  std::memset(m.pData, 0x7f, size);
  ctx->Unmap(buf, 0);
  return true;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  Com<ID3D11Device> dev;
  Com<ID3D11DeviceContext> ctx;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    return fail("D3D11CreateDevice");

  Com<ID3DBlob> vsBlob, psBlob, err;
  if (FAILED(D3DCompile(g_vs, sizeof(g_vs) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &err)))
    return fail("compile vs");
  if (FAILED(D3DCompile(g_ps, sizeof(g_ps) - 1, "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &err)))
    return fail("compile ps");
  Com<ID3D11VertexShader> vs;
  Com<ID3D11PixelShader> ps;
  if (FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)) ||
      FAILED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)))
    return fail("shaders");

  const D3D11_INPUT_ELEMENT_DESC elements[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    { "ICOLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
  };
  Com<ID3D11InputLayout> layout;
  if (FAILED(dev->CreateInputLayout(elements, 4, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)))
    return fail("CreateInputLayout");

  // Rings bigger than what is drawn, so most of each is junk.
  const UINT vbSize = 64 * 1024, ibSize = 16 * 1024, instSize = 16 * 1024;
  D3D11_BUFFER_DESC bd = { };
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  Com<ID3D11Buffer> vb, ib, inst;
  bd.ByteWidth = vbSize;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &vb)))
    return fail("vertex buffer");
  bd.ByteWidth = instSize;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &inst)))
    return fail("instance buffer");
  bd.ByteWidth = ibSize;
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &ib)))
    return fail("index buffer");
  if (!junk(ctx.ptr(), vb.ptr(), vbSize) || !junk(ctx.ptr(), ib.ptr(), ibSize) || !junk(ctx.ptr(), inst.ptr(), instSize))
    return fail("discard maps");

  D3D11_TEXTURE2D_DESC td = { };
  td.Width = 16;
  td.Height = 16;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc = { 1, 0 };
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> tex;
  Com<ID3D11RenderTargetView> rtv;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(dev->CreateRenderTargetView(tex.ptr(), nullptr, &rtv)))
    return fail("render target");
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Com<ID3D11Texture2D> staging;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging)))
    return fail("staging");

  const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  D3D11_VIEWPORT vp = { 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f };
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->ClearRenderTargetView(rtv.ptr(), black);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(layout.ptr());
  ctx->VSSetShader(vs.ptr(), nullptr, 0);
  ctx->PSSetShader(ps.ptr(), nullptr, 0);
  ID3D11Buffer *vbs[2] = { vb.ptr(), inst.ptr() };
  UINT strides[2] = { sizeof(Vertex), sizeof(Instance) }, offsets[2] = { 0, 0 };
  ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
  ctx->IASetIndexBuffer(ib.ptr(), DXGI_FORMAT_R16_UINT, 0);

  // Ring positions, in elements; a gap of junk before each append.
  UINT vAt = 3, iAt = 5, nAt = 2;
  const uint16_t quad[6] = { 0, 1, 2, 2, 1, 3 };
  for (uint32_t x = 0; x < 8; x++) {
    uint32_t c = expected(x);
    float r = unorm(c & 0xff), g = unorm((c >> 8) & 0xff), b = unorm((c >> 16) & 0xff);
    float x0 = x / 8.0f - 1.0f, x1 = (x + 1) / 8.0f - 1.0f;
    Vertex v[4] = { { x0, 1, r, g, b, 1 }, { x1, 1, r, g, b, 1 }, { x0, -1, r, g, b, 1 }, { x1, -1, r, g, b, 1 } };
    Instance none = { 0, 0, 0, 0, 0, 0 };
    vAt += 2; iAt += 3; nAt += 1;
    UINT base = vAt, start = iAt, startInstance = nAt;
    if (!append(ctx.ptr(), vb.ptr(), vAt, v, 4) || !append(ctx.ptr(), ib.ptr(), iAt, quad, 6) ||
        !append(ctx.ptr(), inst.ptr(), nAt, &none, 1))
      return fail("no-overwrite maps");
    ctx->DrawIndexedInstanced(6, 1, start, INT(base), startInstance);
  }
  // Columns 8-15: one quad at column 0, moved and coloured per instance.
  {
    Vertex v[4] = { { -1, 1, 0, 0, 0, 1 }, { -0.875f, 1, 0, 0, 0, 1 }, { -1, -1, 0, 0, 0, 1 }, { -0.875f, -1, 0, 0, 0, 1 } };
    Instance in[8];
    for (uint32_t i = 0; i < 8; i++) {
      uint32_t c = expected(8 + i);
      in[i] = { (8 + i) / 8.0f, 0, unorm(c & 0xff), unorm((c >> 8) & 0xff), unorm((c >> 16) & 0xff), 0 };
    }
    vAt += 7; iAt += 1; nAt += 4;
    UINT base = vAt, start = iAt, startInstance = nAt;
    if (!append(ctx.ptr(), vb.ptr(), vAt, v, 4) || !append(ctx.ptr(), ib.ptr(), iAt, quad, 6) ||
        !append(ctx.ptr(), inst.ptr(), nAt, in, 8))
      return fail("no-overwrite maps (instanced)");
    ctx->DrawIndexedInstanced(6, 8, start, INT(base), startInstance);
  }
  ctx->CopyResource(staging.ptr(), tex.ptr());

  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &m)))
    return fail("Map");
  uint32_t bad = 0, first = 0, at = 0;
  for (uint32_t y = 0; y < 16; y++) {
    auto row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m.pData) + y * m.RowPitch);
    for (uint32_t x = 0; x < 16; x++) {
      if (row[x] != expected(x) && !bad++) {
        first = row[x];
        at = y * 16 + x;
      }
    }
  }
  ctx->Unmap(staging.ptr(), 0);
  if (bad) {
    std::cout << "ring: " << bad << " pixels wrong, first at " << at << ": " << std::hex << first
              << " want " << expected(at % 16) << std::dec << std::endl;
    return fail("pixels");
  }
  std::cout << "ring: pass" << std::endl;
  return 0;
}
