/*
 * dirk's guest test: a Linux program whose D3D11 calls go through the wire
 * to gex's D3D device (/dev/gex-d3d) and the page's Mesa. It draws a
 * triangle over a clear, coloured from a constant buffer written through
 * Map(WRITE_DISCARD), copies the back buffer to a staging texture, reads it
 * through Map(READ) and checks two pixels. Prints `dirk guest: pass`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* A Windows program (W64's test): the D3D11 it links is the shim's. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#define CreateDeviceAndSwapChain D3D11CreateDeviceAndSwapChain
#else
#include "dirk_guest.h"
#include "gex_transport.h"
#define CreateDeviceAndSwapChain dirk_D3D11CreateDeviceAndSwapChain
#endif

#include "tri_vs.h"
#include "tri_ps.h"

#define W 128
#define H 96

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static int close_to(unsigned a, unsigned b)
{
    for (int c = 0; c < 32; c += 8) {
        int d = (int)((a >> c) & 0xff) - (int)((b >> c) & 0xff);
        if (d > 1 || d < -1)
            return 0;
    }
    return 1;
}

int main(void)
{
#ifdef _WIN32
    /* A window of its own, shown: its frames go to the window's surface. */
    WNDCLASSA wc = { 0 };
    RECT r = { 0, 0, W, H };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dirk_tri";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowA("dirk_tri", "dirk tri", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                                r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!window) {
        printf("FAIL CreateWindow\n");
        return 1;
    }
#else
    struct dirk_transport transport;
    if (dirk_gex_transport(&transport) != 0)
        return 1;
    dirk_guest_init(&transport);
    /* A window with no surface: frames are drawn and read back, not shown. */
    dirk_guest_window_surface(1, 0, 0, W, H);
    HWND window = (HWND)1;
#endif

    DXGI_SWAP_CHAIN_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.BufferDesc.Width = W;
    sd.BufferDesc.Height = H;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = window;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *swapchain = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
                                          D3D11_SDK_VERSION, &sd, &swapchain, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        printf("FAIL D3D11CreateDeviceAndSwapChain: 0x%08x\n", (unsigned)hr);
        return 1;
    }
    printf("dirk guest: device and swap chain made\n");

    ID3D11Texture2D *back = NULL;
    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&back);
    CHECK(SUCCEEDED(hr) && back, "GetBuffer 0x%08x", (unsigned)hr);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);

    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Device_CreateVertexShader(dev, tri_vs, sizeof(tri_vs), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, tri_ps, sizeof(tri_ps), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC el = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                    D3D11_INPUT_PER_VERTEX_DATA, 0 };
    ID3D11InputLayout *layout = NULL;
    hr = ID3D11Device_CreateInputLayout(dev, &el, 1, tri_vs, sizeof(tri_vs), &layout);

    static const float verts[] = { -0.8f, -0.8f, 0.0f, 0.8f, 0.8f, -0.8f };
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
    ID3D11Buffer *vb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, &init, &vb);

    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);

    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(back, &td);
    CHECK(td.Width == W && td.Height == H, "back buffer %ux%u", td.Width, td.Height);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging);

    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    const float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
    const UINT stride = 8, offset = 0;
    /* FRAMES=N in the environment: more presents after the check (to watch). */
    int frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 3;
    if (frames < 3)
        frames = 3;
    for (int frame = 0; frame < frames; frame++) {
        D3D11_MAPPED_SUBRESOURCE m;
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        CHECK(SUCCEEDED(hr), "map the constant buffer 0x%08x", (unsigned)hr);
        if (SUCCEEDED(hr)) {
            const float yellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
            memcpy(m.pData, yellow, sizeof(yellow));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
        }
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        if (frame == 2) {
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)back);
            hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m);
            CHECK(SUCCEEDED(hr), "map the staging texture 0x%08x", (unsigned)hr);
            if (SUCCEEDED(hr)) {
                const unsigned char *px = m.pData;
                unsigned corner, centre;
                memcpy(&corner, px + 2 * m.RowPitch + 2 * 4, 4);
                memcpy(&centre, px + (H / 2) * m.RowPitch + (W / 2) * 4, 4);
                printf("dirk guest: corner %08x centre %08x\n", corner, centre);
                CHECK(close_to(corner, 0xff4c3319u), "corner is %08x, not the clear colour", corner);
                CHECK(close_to(centre, 0xff00ffffu), "centre is %08x, not the triangle", centre);
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
            }
        }
        hr = IDXGISwapChain_Present(swapchain, 0, 0);
        CHECK(SUCCEEDED(hr), "present 0x%08x", (unsigned)hr);
    }

    ID3D11DeviceContext_ClearState(ctx);
    ID3D11DeviceContext_Flush(ctx);
    ID3D11Texture2D_Release(staging);
    ID3D11Buffer_Release(cb);
    ID3D11Buffer_Release(vb);
    ID3D11InputLayout_Release(layout);
    ID3D11PixelShader_Release(ps);
    ID3D11VertexShader_Release(vs);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(back);
    IDXGISwapChain_Release(swapchain);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    printf(failures ? "dirk guest: %d failures\n" : "dirk guest: pass\n", failures);
    return failures != 0;
}
