// Deferred contexts: what a deferred context records reaches the GPU when the
// immediate context executes it, and in the order the lists are executed.
//   - a clear recorded on a deferred context, read back after execution;
//   - a dynamic buffer written through the deferred context's Map
//     (WRITE_DISCARD), copied to a staging buffer in the same list;
//   - two lists clearing one target, executed in the other order than they
//     were recorded: the last executed wins;
//   - an event query ended after the lists completes.
// Prints "deferred: pass" when all hold, a FAIL line for the first that does
// not.
#include <array>
#include <cstring>

#include <d3d11.h>

#include <windows.h>

#include "test_utils.h"

using namespace dxvk;

static int fail(const char *what) {
  std::cout << "deferred: FAIL " << what << std::endl;
  return 1;
}

static bool readPixel(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, uint32_t *out) {
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Com<ID3D11Texture2D> staging;
  if (FAILED(dev->CreateTexture2D(&desc, nullptr, &staging)))
    return false;
  ctx->CopyResource(staging.ptr(), tex);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &m)))
    return false;
  *out = *reinterpret_cast<const uint32_t *>(m.pData);
  ctx->Unmap(staging.ptr(), 0);
  return true;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  Com<ID3D11Device> dev;
  Com<ID3D11DeviceContext> ctx;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    return fail("D3D11CreateDevice");

  D3D11_FEATURE_DATA_THREADING threading = { };
  dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading));
  std::cout << "deferred: threading concurrent creates " << threading.DriverConcurrentCreates
            << " command lists " << threading.DriverCommandLists << std::endl;

  Com<ID3D11DeviceContext> def;
  if (FAILED(dev->CreateDeferredContext(0, &def)))
    return fail("CreateDeferredContext");
  if (def->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED)
    return fail("GetType is not DEFERRED");

  D3D11_TEXTURE2D_DESC td = { };
  td.Width = 4;
  td.Height = 4;
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

  // 1. A deferred clear.
  const FLOAT red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
  def->ClearRenderTargetView(rtv.ptr(), red);
  Com<ID3D11CommandList> list;
  if (FAILED(def->FinishCommandList(FALSE, &list)))
    return fail("FinishCommandList (clear)");
  ctx->ExecuteCommandList(list.ptr(), FALSE);
  uint32_t px = 0;
  if (!readPixel(dev.ptr(), ctx.ptr(), tex.ptr(), &px))
    return fail("read back (clear)");
  if (px != 0xff0000ffu) {
    std::cout << "deferred: clear read " << std::hex << px << std::dec << std::endl;
    return fail("clear");
  }
  std::cout << "deferred: clear ok" << std::endl;

  // 2. A deferred Map(WRITE_DISCARD) of a dynamic buffer, copied in the list.
  D3D11_BUFFER_DESC bd = { };
  bd.ByteWidth = 256;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  Com<ID3D11Buffer> dyn;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &dyn)))
    return fail("dynamic buffer");
  bd.Usage = D3D11_USAGE_STAGING;
  bd.BindFlags = 0;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Com<ID3D11Buffer> rb;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &rb)))
    return fail("staging buffer");
  for (uint32_t round = 0; round < 2; round++) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(def->Map(dyn.ptr(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
      return fail("deferred Map");
    auto *w = reinterpret_cast<uint32_t *>(m.pData);
    for (uint32_t i = 0; i < 64; i++)
      w[i] = 0x1000u * (round + 1) + i;
    def->Unmap(dyn.ptr(), 0);
    def->CopyResource(rb.ptr(), dyn.ptr());
    list = nullptr;
    if (FAILED(def->FinishCommandList(FALSE, &list)))
      return fail("FinishCommandList (map)");
    ctx->ExecuteCommandList(list.ptr(), FALSE);
    if (FAILED(ctx->Map(rb.ptr(), 0, D3D11_MAP_READ, 0, &m)))
      return fail("Map staging");
    auto *r = reinterpret_cast<const uint32_t *>(m.pData);
    bool ok = true;
    for (uint32_t i = 0; i < 64; i++)
      ok &= r[i] == 0x1000u * (round + 1) + i;
    uint32_t first = r[0], last = r[63];
    ctx->Unmap(rb.ptr(), 0);
    if (!ok) {
      std::cout << "deferred: map round " << round << " read " << std::hex << first << " .. " << last << std::dec << std::endl;
      return fail("map");
    }
  }
  std::cout << "deferred: map ok" << std::endl;

  // 3. Two lists, executed in the other order.
  const FLOAT green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
  const FLOAT blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
  Com<ID3D11CommandList> toGreen, toBlue;
  def->ClearRenderTargetView(rtv.ptr(), green);
  if (FAILED(def->FinishCommandList(FALSE, &toGreen)))
    return fail("FinishCommandList (green)");
  def->ClearRenderTargetView(rtv.ptr(), blue);
  if (FAILED(def->FinishCommandList(FALSE, &toBlue)))
    return fail("FinishCommandList (blue)");
  ctx->ExecuteCommandList(toBlue.ptr(), FALSE);
  ctx->ExecuteCommandList(toGreen.ptr(), FALSE);
  if (!readPixel(dev.ptr(), ctx.ptr(), tex.ptr(), &px))
    return fail("read back (order)");
  if (px != 0xff00ff00u) {
    std::cout << "deferred: order read " << std::hex << px << std::dec << std::endl;
    return fail("order");
  }
  // A list executes again.
  ctx->ExecuteCommandList(toBlue.ptr(), FALSE);
  if (!readPixel(dev.ptr(), ctx.ptr(), tex.ptr(), &px) || px != 0xffff0000u)
    return fail("list executed twice");
  std::cout << "deferred: order ok" << std::endl;

  // 4. An event query after the lists.
  D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
  Com<ID3D11Query> q;
  if (FAILED(dev->CreateQuery(&qd, &q)))
    return fail("CreateQuery");
  ctx->ExecuteCommandList(toGreen.ptr(), FALSE);
  ctx->End(q.ptr());
  BOOL done = FALSE;
  HRESULT hr = S_FALSE;
  for (uint32_t i = 0; i < 100000 && hr == S_FALSE; i++)
    hr = ctx->GetData(q.ptr(), &done, sizeof(done), 0);
  if (hr != S_OK || !done)
    return fail("event query");
  std::cout << "deferred: query ok" << std::endl;

  std::cout << "deferred: pass" << std::endl;
  return 0;
}
