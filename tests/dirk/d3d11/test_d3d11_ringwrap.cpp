// A streaming ring in a default vertex buffer too big to rename, written by
// UpdateSubresource a few quads ahead of the draws, so most writes land beside
// bytes the batch still reads and some wrap onto them. Each of the 64x8 pixels
// of the target is one quad with its own colour, drawn from the ring slot its
// vertices were written to; odd cells are indexed (base vertex at the slot),
// and in the last two rows every fourth cell's vertices arrive by a GPU copy
// from a second buffer instead. Two cells in three are written wrong first
// and right two cells before they are drawn, so the later write has to win.
// Flushes every 128 cells start fresh batches.
// Prints "ringwrap: pass", or a FAIL line with the first pixel that is wrong.
#include <cstring>
#include <vector>

#include <d3dcompiler.h>
#include <d3d11.h>

#include <windows.h>

#include "test_utils.h"

using namespace dxvk;

static const char g_vs[] =
  "struct I { float2 pos : POSITION; float4 col : COLOR; };\n"
  "struct O { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
  "O main(I i) { O o; o.pos = float4(i.pos, 0, 1); o.col = i.col; return o; }\n";

static const char g_ps[] =
  "float4 main(float4 pos : SV_POSITION, float4 col : COLOR) : SV_TARGET { return col; }\n";

struct Vertex { float x, y, r, g, b, a; };

static const UINT W = 64, H = 8, CELLS = W * H;
static const UINT SLOTS = 61, LEAD = 5, STRIDE = 7;
// Just past 1 MiB, on a vertex boundary.
static const UINT VB_SIZE = 2u << 20, RING_AT = 43776 * 24;
static const UINT QUAD = 4 * sizeof(Vertex);

static int fail(const char *what) {
  std::cout << "ringwrap: FAIL " << what << std::endl;
  return 1;
}

static uint32_t expected(uint32_t cell) {
  uint32_t x = cell % W, y = cell / W;
  uint32_t r = 4 * x, g = 32 * y, b = 255 - (cell * 37) % 200;
  return 0xff000000u | (b << 16) | (g << 8) | r;
}

static float unorm(uint32_t v) { return float(v) / 255.0f; }

static void quad(uint32_t cell, Vertex v[4]) {
  uint32_t c = expected(cell), x = cell % W, y = cell / W;
  float r = unorm(c & 0xff), g = unorm((c >> 8) & 0xff), b = unorm((c >> 16) & 0xff);
  float x0 = 2.0f * x / W - 1.0f, x1 = 2.0f * (x + 1) / W - 1.0f;
  float y0 = 1.0f - 2.0f * y / H, y1 = 1.0f - 2.0f * (y + 1) / H;
  v[0] = { x0, y0, r, g, b, 1 };
  v[1] = { x1, y0, r, g, b, 1 };
  v[2] = { x0, y1, r, g, b, 1 };
  v[3] = { x1, y1, r, g, b, 1 };
}

static UINT slotOf(uint32_t cell) { return (cell * STRIDE) % SLOTS; }
static bool byCopy(uint32_t cell) { return cell / W >= 6 && cell % 4 == 0; }

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
  };
  Com<ID3D11InputLayout> layout;
  if (FAILED(dev->CreateInputLayout(elements, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)))
    return fail("CreateInputLayout");

  // The ring, filled with junk so a draw that reads the wrong bytes shows.
  D3D11_BUFFER_DESC bd = { };
  bd.ByteWidth = VB_SIZE;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  std::vector<uint8_t> junk(VB_SIZE, 0x7f);
  D3D11_SUBRESOURCE_DATA init = { junk.data(), 0, 0 };
  Com<ID3D11Buffer> vb;
  if (FAILED(dev->CreateBuffer(&bd, &init, &vb)))
    return fail("vertex buffer");
  // The copy source: each copied cell's quad at cell * QUAD.
  std::vector<Vertex> src(CELLS * 4);
  for (uint32_t cell = 0; cell < CELLS; cell++)
    quad(cell, &src[cell * 4]);
  bd.ByteWidth = CELLS * QUAD;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA srcInit = { src.data(), 0, 0 };
  Com<ID3D11Buffer> copySrc;
  if (FAILED(dev->CreateBuffer(&bd, &srcInit, &copySrc)))
    return fail("copy source");
  // Indices for one quad as a list, through a dynamic buffer.
  bd.ByteWidth = 64;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  Com<ID3D11Buffer> ib;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &ib)))
    return fail("index buffer");
  {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(ib.ptr(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
      return fail("index map");
    const uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
    std::memcpy(m.pData, idx, sizeof(idx));
    ctx->Unmap(ib.ptr(), 0);
  }

  D3D11_TEXTURE2D_DESC td = { };
  td.Width = W;
  td.Height = H;
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
  D3D11_VIEWPORT vp = { 0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f };
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->ClearRenderTargetView(rtv.ptr(), black);
  ctx->IASetInputLayout(layout.ptr());
  ctx->VSSetShader(vs.ptr(), nullptr, 0);
  ctx->PSSetShader(ps.ptr(), nullptr, 0);
  ID3D11Buffer *vbs[1] = { vb.ptr() };
  UINT strides[1] = { sizeof(Vertex) }, offsets[1] = { 0 };
  ctx->IASetVertexBuffers(0, 1, vbs, strides, offsets);
  ctx->IASetIndexBuffer(ib.ptr(), DXGI_FORMAT_R16_UINT, 0);

  // Two cells in three are first written wrong (with the next cell's quad)
  // LEAD cells ahead, and right two cells ahead: two writes of the same bytes
  // with draws between that do not read them, so the later has to win.
  auto twice = [](uint32_t cell) { return cell % 3 != 0 && !byCopy(cell); };
  auto put = [&](uint32_t cell, bool wrong = false) {
    UINT at = RING_AT + slotOf(cell) * QUAD;
    if (byCopy(cell)) {
      D3D11_BOX box = { cell * QUAD, 0, 0, cell * QUAD + QUAD, 1, 1 };
      ctx->CopySubresourceRegion(vb.ptr(), 0, at, 0, 0, copySrc.ptr(), 0, &box);
      return;
    }
    Vertex v[4];
    quad(wrong ? (cell + 1) % CELLS : cell, v);
    if (!wrong && cell % 3 == 2) {
      // The right bytes in two halves, the second first.
      D3D11_BOX first = { at, 0, 0, at + QUAD / 2, 1, 1 };
      D3D11_BOX second = { at + QUAD / 2, 0, 0, at + QUAD, 1, 1 };
      ctx->UpdateSubresource(vb.ptr(), 0, &second, &v[2], 0, 0);
      ctx->UpdateSubresource(vb.ptr(), 0, &first, &v[0], 0, 0);
      return;
    }
    D3D11_BOX box = { at, 0, 0, at + QUAD, 1, 1 };
    ctx->UpdateSubresource(vb.ptr(), 0, &box, v, 0, 0);
  };

  for (uint32_t cell = 0; cell < LEAD; cell++)
    put(cell, twice(cell) && cell >= 2);
  for (uint32_t cell = 0; cell < CELLS; cell++) {
    // Ahead of the draws; past the first wrap the slot is one a draw of this
    // batch may still read.
    if (cell + LEAD < CELLS)
      put(cell + LEAD, twice(cell + LEAD));
    if (cell + 2 < CELLS && twice(cell + 2))
      put(cell + 2);
    UINT first = (RING_AT + slotOf(cell) * QUAD) / sizeof(Vertex);
    if (cell & 1) {
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->DrawIndexed(6, 0, INT(first));
    } else {
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      ctx->Draw(4, first);
    }
    // A slot is only rewritten once its cell is drawn, which LEAD < SLOTS
    // and the stride keep true: cell + LEAD shares a slot with cell + LEAD -
    // SLOTS at the nearest.
    if (cell % 128 == 127)
      ctx->Flush();
  }
  ctx->CopyResource(staging.ptr(), tex.ptr());

  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &m)))
    return fail("Map");
  uint32_t bad = 0, first = 0, at = 0;
  for (uint32_t y = 0; y < H; y++) {
    auto row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(m.pData) + y * m.RowPitch);
    for (uint32_t x = 0; x < W; x++) {
      if (row[x] != expected(y * W + x) && !bad++) {
        first = row[x];
        at = y * W + x;
      }
    }
  }
  ctx->Unmap(staging.ptr(), 0);
  if (bad) {
    std::cout << "ringwrap: " << bad << " pixels wrong, first at " << at << ": " << std::hex << first
              << " want " << expected(at) << std::dec << std::endl;
    return fail("pixels");
  }
  std::cout << "ringwrap: pass" << std::endl;
  return 0;
}
