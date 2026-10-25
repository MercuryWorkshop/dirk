// A pixel shader's discard: a full-screen red triangle over a blue clear,
// discarding the left half of a 16x16 target. The left half reads blue, the
// right red. Prints "discard: pass", or a FAIL line with what it read.
#include <array>
#include <cstring>

#include <d3dcompiler.h>
#include <d3d11.h>

#include <windows.h>

#include "test_utils.h"

using namespace dxvk;

static const char g_vs[] =
  "float4 main(uint id : SV_VertexID) : SV_POSITION {\n"
  "  float2 p = float2((id << 1) & 2, id & 2);\n"
  "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
  "}\n";

// ddx keeps a derivative live across the discard, as a game's alpha-tested
// material does.
static const char g_ps[] =
  "float4 main(float4 pos : SV_POSITION) : SV_TARGET {\n"
  "  if (pos.x < 8.0) discard;\n"
  "  return float4(1, ddx(pos.x) - 1.0, 0, 1);\n"
  "}\n";

static int fail(const char *what) {
  std::cout << "discard: FAIL " << what << std::endl;
  return 1;
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
  if (FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)))
    return fail("CreateVertexShader");
  if (FAILED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)))
    return fail("CreatePixelShader");

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

  const FLOAT blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
  D3D11_VIEWPORT vp = { 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f };
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->ClearRenderTargetView(rtv.ptr(), blue);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.ptr(), nullptr, 0);
  ctx->PSSetShader(ps.ptr(), nullptr, 0);
  ctx->Draw(3, 0);
  ctx->CopyResource(staging.ptr(), tex.ptr());

  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &m)))
    return fail("Map");
  uint32_t bad = 0, first = 0, at = 0;
  for (uint32_t y = 0; y < 16; y++) {
    auto row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m.pData) + y * m.RowPitch);
    for (uint32_t x = 0; x < 16; x++) {
      const uint32_t want = x < 8 ? 0xffff0000u : 0xff0000ffu;
      if (row[x] != want && !bad++) {
        first = row[x];
        at = y * 16 + x;
      }
    }
  }
  ctx->Unmap(staging.ptr(), 0);
  if (bad) {
    std::cout << "discard: " << bad << " pixels wrong, first at " << at << ": " << std::hex << first << std::dec << std::endl;
    return fail("pixels");
  }
  std::cout << "discard: pass" << std::endl;
  return 0;
}
