// Two devices drawing at once, each through buffers rewritten between its
// draws: a constant buffer whole by UpdateSubresource, and a default vertex
// buffer in part (a box of one column's quad, so the rest is carried over),
// with the two devices' calls interleaved so both have draws recorded and
// unsubmitted while the other writes. Each draws a 16x16
// target column by column, three times over (the last pass is the one that
// shows), in colours of its own. Prints "twodev: pass", or a FAIL line with
// the device and the first pixel that is wrong.
#include <cstring>

#include <d3dcompiler.h>
#include <d3d11.h>

#include <windows.h>

#include "test_utils.h"

using namespace dxvk;

static const char g_vs[] =
  "cbuffer C : register(b0) { float4 col; };\n"
  "struct O { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
  "O main(float2 pos : POSITION) { O o; o.pos = float4(pos, 0, 1); o.col = col; return o; }\n";

static const char g_ps[] =
  "float4 main(float4 pos : SV_POSITION, float4 col : COLOR) : SV_TARGET { return col; }\n";

static int fail(const char *what) {
  std::cout << "twodev: FAIL " << what << std::endl;
  return 1;
}

// Column x's colour on device d in pass p, as the target stores it.
static uint32_t colour(uint32_t d, uint32_t p, uint32_t x) {
  uint32_t r = 8 * x + 4, g = d ? 200 : 40, b = 60 * p + 10;
  return 0xff000000u | (b << 16) | (g << 8) | r;
}

static float unorm(uint32_t v) { return float(v) / 255.0f; }

struct Device {
  Com<ID3D11Device> dev;
  Com<ID3D11DeviceContext> ctx;
  Com<ID3D11VertexShader> vs;
  Com<ID3D11PixelShader> ps;
  Com<ID3D11InputLayout> layout;
  Com<ID3D11Buffer> cb, vb;
  Com<ID3D11Texture2D> tex, staging;
  Com<ID3D11RenderTargetView> rtv;
};

static const char *setup(Device &d, ID3DBlob *vsBlob, ID3DBlob *psBlob) {
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &d.dev, nullptr, &d.ctx)))
    return "D3D11CreateDevice";
  if (FAILED(d.dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &d.vs)) ||
      FAILED(d.dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &d.ps)))
    return "shaders";
  const D3D11_INPUT_ELEMENT_DESC element = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
  if (FAILED(d.dev->CreateInputLayout(&element, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &d.layout)))
    return "CreateInputLayout";
  D3D11_BUFFER_DESC bd = { };
  bd.ByteWidth = 16;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(d.dev->CreateBuffer(&bd, nullptr, &d.cb)))
    return "constant buffer";
  bd.ByteWidth = 16 * 4 * 2 * sizeof(float);
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  if (FAILED(d.dev->CreateBuffer(&bd, nullptr, &d.vb)))
    return "vertex buffer";
  D3D11_TEXTURE2D_DESC td = { };
  td.Width = 16;
  td.Height = 16;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc = { 1, 0 };
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &d.tex)) ||
      FAILED(d.dev->CreateRenderTargetView(d.tex.ptr(), nullptr, &d.rtv)))
    return "render target";
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &d.staging)))
    return "staging";

  const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  D3D11_VIEWPORT vp = { 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f };
  ID3D11DeviceContext *ctx = d.ctx.ptr();
  ctx->OMSetRenderTargets(1, &d.rtv, nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->ClearRenderTargetView(d.rtv.ptr(), black);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  ctx->IASetInputLayout(d.layout.ptr());
  ctx->VSSetShader(d.vs.ptr(), nullptr, 0);
  ctx->VSSetConstantBuffers(0, 1, &d.cb);
  ctx->PSSetShader(d.ps.ptr(), nullptr, 0);
  UINT stride = 2 * sizeof(float), offset = 0;
  ctx->IASetVertexBuffers(0, 1, &d.vb, &stride, &offset);
  return nullptr;
}

static bool column(Device &d, uint32_t n, uint32_t p, uint32_t x) {
  ID3D11DeviceContext *ctx = d.ctx.ptr();
  uint32_t c = colour(n, p, x);
  const float col[4] = { unorm(c & 0xff), unorm((c >> 8) & 0xff), unorm((c >> 16) & 0xff), 1.0f };
  ctx->UpdateSubresource(d.cb.ptr(), 0, nullptr, col, 0, 0);
  float x0 = x / 8.0f - 1.0f, x1 = (x + 1) / 8.0f - 1.0f;
  const float quad[8] = { x0, 1, x1, 1, x0, -1, x1, -1 };
  const D3D11_BOX box = { UINT(x * sizeof(quad)), 0, 0, UINT((x + 1) * sizeof(quad)), 1, 1 };
  ctx->UpdateSubresource(d.vb.ptr(), 0, &box, quad, 0, 0);
  ctx->Draw(4, 4 * x);
  return true;
}

static uint32_t check(Device &d, uint32_t n) {
  ID3D11DeviceContext *ctx = d.ctx.ptr();
  ctx->CopyResource(d.staging.ptr(), d.tex.ptr());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(d.staging.ptr(), 0, D3D11_MAP_READ, 0, &m))) {
    std::cout << "twodev: device " << n << ": Map failed" << std::endl;
    return 256;
  }
  uint32_t bad = 0, first = 0, at = 0;
  for (uint32_t y = 0; y < 16; y++) {
    auto row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m.pData) + y * m.RowPitch);
    for (uint32_t x = 0; x < 16; x++)
      if (row[x] != colour(n, 2, x) && !bad++) {
        first = row[x];
        at = y * 16 + x;
      }
  }
  ctx->Unmap(d.staging.ptr(), 0);
  if (bad)
    std::cout << "twodev: device " << n << ": " << bad << " pixels wrong, first at " << at << ": "
              << std::hex << first << " want " << colour(n, 2, at % 16) << std::dec << std::endl;
  return bad;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  Com<ID3DBlob> vsBlob, psBlob, err;
  if (FAILED(D3DCompile(g_vs, sizeof(g_vs) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &err)))
    return fail("compile vs");
  if (FAILED(D3DCompile(g_ps, sizeof(g_ps) - 1, "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &err)))
    return fail("compile ps");
  Device dev[2];
  for (auto &d : dev)
    if (const char *why = setup(d, vsBlob.ptr(), psBlob.ptr()))
      return fail(why);
  for (uint32_t p = 0; p < 3; p++)
    for (uint32_t x = 0; x < 16; x++)
      for (uint32_t n = 0; n < 2; n++)
        if (!column(dev[n], n, p, x))
          return fail("draw");
  if (check(dev[0], 0) + check(dev[1], 1))
    return fail("pixels");
  std::cout << "twodev: pass" << std::endl;
  return 0;
}
