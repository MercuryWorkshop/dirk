/*
 * dirk's Windows shim, d3d11.dll: the program's D3D11 and DXGI calls become
 * the wire's records (src/dirk/wire) in the D3D device's ring, which the
 * Unix side (unixlib.c) submits. The device draws them with DXVK on the
 * host. dxgi.dll's entry points are this DLL's (dxgi.spec forwards them),
 * so a process has one stream and one set of handles.
 */
#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "dirk_guest.h"
#include "unixlib.h"

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static uint8_t *ring;
static UINT32 capacity;
static BOOL ready;

static const uint8_t *send_ring(void *ctx, const uint8_t *records, size_t len, int wait)
{
    struct dirk_unix_submit_params p;
    uint8_t *cmds = ring + DIRK_RING_COMMANDS;
    (void)ctx;
    if (len > capacity)
        return NULL;
    memcpy(cmds, records, len);
    p.len = (UINT32)len;
    p.wait = wait != 0;
    p.reply_len = 0;
    p.status = 0;
    if (WINE_UNIX_CALL(dirk_unix_submit, &p))
        return NULL;
    if (!wait)
        return cmds;
    if (p.reply_len < 4)
        return NULL;
    return cmds + capacity;
}

/* A window's surface and size, sent when new or changed. */
#define WINDOWS_MAX 16
static struct { UINT64 hwnd; UINT32 id, width, height; } windows[WINDOWS_MAX];
static unsigned window_count;
static CRITICAL_SECTION window_lock;

static void window_changed(void *ctx, uint64_t hwnd)
{
    struct dirk_unix_surface_params p = { hwnd };
    unsigned i;
    (void)ctx;
    if (WINE_UNIX_CALL(dirk_unix_surface, &p)) {
        if (dirk_trace_on)
            dirk_trace("window %llx: no surface (the Unix side refused it)", (unsigned long long)hwnd);
        return;
    }
    EnterCriticalSection(&window_lock);
    for (i = 0; i < window_count && windows[i].hwnd != hwnd; i++)
        ;
    if (i == window_count) {
        if (window_count == WINDOWS_MAX) {
            LeaveCriticalSection(&window_lock);
            return;
        }
        window_count++;
        windows[i].hwnd = hwnd;
        windows[i].id = ~0u;
    }
    if (windows[i].id == p.id && windows[i].width == p.width && windows[i].height == p.height) {
        LeaveCriticalSection(&window_lock);
        return;
    }
    windows[i].id = p.id;
    windows[i].width = p.width;
    windows[i].height = p.height;
    LeaveCriticalSection(&window_lock);
    if (dirk_trace_on)
        dirk_trace("window %llx: surface %u, %ux%u", (unsigned long long)hwnd, p.id, p.width, p.height);
    dirk_guest_window_surface(hwnd, 0, p.id, p.width, p.height);
}

static BOOL CALLBACK init(INIT_ONCE *once, void *param, void **ctx)
{
    struct dirk_unix_init_params p;
    struct dirk_transport t;
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&window_lock);
    if (__wine_init_unix_call() || WINE_UNIX_CALL(dirk_unix_init, &p))
        return TRUE;
    ring = (uint8_t *)(ULONG_PTR)p.ring;
    capacity = p.capacity;
    t.send = send_ring;
    t.ctx = NULL;
    t.window = window_changed;
    dirk_guest_init(&t);
    ready = TRUE;
    return TRUE;
}

static BOOL start(void)
{
    InitOnceExecuteOnce(&init_once, init, NULL, NULL);
    return ready;
}

HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE swrast,
        UINT flags, const D3D_FEATURE_LEVEL *feature_levels, UINT levels, UINT sdk_version,
        ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context)
{
    if (!start())
        return E_FAIL;
    return dirk_D3D11CreateDevice(adapter, driver_type, swrast, flags, feature_levels, levels,
                                  sdk_version, device, feature_level, context);
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type,
        HMODULE swrast, UINT flags, const D3D_FEATURE_LEVEL *feature_levels, UINT levels,
        UINT sdk_version, const DXGI_SWAP_CHAIN_DESC *swapchain_desc, IDXGISwapChain **swapchain,
        ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context)
{
    if (!start())
        return E_FAIL;
    return dirk_D3D11CreateDeviceAndSwapChain(adapter, driver_type, swrast, flags, feature_levels,
                                              levels, sdk_version, swapchain_desc, swapchain,
                                              device, feature_level, context);
}

/* dxgi.dll's, forwarded here (dxgi.spec). */
HRESULT WINAPI dirk_dxgi_CreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
    if (!start())
        return E_FAIL;
    return dirk_CreateDXGIFactory2(flags, riid, factory);
}

HRESULT WINAPI dirk_dxgi_CreateDXGIFactory1(REFIID riid, void **factory)
{
    return dirk_dxgi_CreateDXGIFactory2(0, riid, factory);
}

HRESULT WINAPI dirk_dxgi_CreateDXGIFactory(REFIID riid, void **factory)
{
    return dirk_dxgi_CreateDXGIFactory2(0, riid, factory);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
