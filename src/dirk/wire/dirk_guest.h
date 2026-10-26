/*
 * dirk's guest side: proxies of the D3D11 and DXGI interfaces, whose calls
 * are records in a stream the host reads (dirk_wire.h).
 *
 * Threads: one stream per process, behind one lock. Every call on every
 * proxy, the immediate context's and deferred contexts' alike, is written
 * under it, so the stream's order is the order the calls were made in, and
 * a call one thread makes after another thread's (as the program orders
 * them, as D3D asks it to for a deferred context's command list) is after
 * it in the stream too. A call that waits holds the lock until its answer
 * is read. Handles come from one counter, under the same lock.
 */
#ifndef DIRK_GUEST_H
#define DIRK_GUEST_H

#define COBJMACROS
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include "dirk_wire.h"

#ifndef _WIN32
/* Outside Windows (a host-only test), the atomics Windows has. */
static inline LONG InterlockedIncrement(LONG volatile *v) { return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(LONG volatile *v) { return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(LONG volatile *v, LONG xchg, LONG cmp)
{
    __atomic_compare_exchange_n(v, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
#endif

/* How the stream reaches the host. */
struct dirk_transport {
    /* Sends len bytes of records. If wait, the last record waits: returns
     * its answer, valid until the next send. Else returns non-NULL. NULL if
     * the host is gone. */
    const uint8_t *(*send)(void *ctx, const uint8_t *records, size_t len, int wait);
    void *ctx;
    /* Optional: a window the program makes a swap chain for, called again
     * before each present and resize, outside the stream's lock. It names
     * the window's surface and size (dirk_guest_window_surface) when they
     * are new or changed. */
    void (*window)(void *ctx, uint64_t window);
};

struct dirk_iface {
    const char *name;
    GUID iid;
    void *const *vtbl;
    int base;           /* the interface it derives from, or -1 for IUnknown */
};

extern const struct dirk_iface dirk_ifaces[DIRK_IFACE_COUNT];

/* What the guest keeps of a resource, for the calls whose sizes depend on
 * it: Map, UpdateSubresource, initial data. */
struct dirk_resource {
    int kind;                   /* enum dirk_resource_kind, or -1 not known yet */
    union {
        D3D11_BUFFER_DESC buffer;
        D3D11_TEXTURE1D_DESC tex1d;
        D3D11_TEXTURE2D_DESC tex2d;
        D3D11_TEXTURE3D_DESC tex3d;
    } desc;
    /* The memory a map hands out, per subresource; kept for the next map
     * (a no-overwrite map writes into what the last one left). */
    uint32_t sub_count;
    struct dirk_mapping *maps;
    /* dirk_draw.c's: a dynamic vertex or index buffer whose no-overwrite
     * maps send nothing at Unmap, their bytes going when a draw reads them
     * (lazy); what the host holds of it (host, when host_valid); and an
     * index buffer's bytes where no map holds them (index). */
    int lazy, host_valid, index_valid;
    uint32_t gen, synced_gen, writes;
    uint8_t *host, *index;
    struct { uint64_t lo, hi; uint32_t gen; } seen[4];
    unsigned seen_next;
    int pending_at;             /* 1 + its place among the pending, or 0 */
};

struct dirk_mapping {
    uint8_t *data;
    size_t size;
    uint32_t row_pitch, depth_pitch;
    D3D11_MAP type;             /* 0: not mapped */
};

struct dirk_proxy {
    void *const *vtbl;          /* first: the COM pointer is the proxy */
    LONG refs;
    uint32_t handle;
    int iface;                  /* enum dirk_iface_id */
    struct dirk_proxy *next;    /* the handle table's chain */
    struct dirk_resource *resource;
    void *draw;                 /* dirk_draw.c's: a context's input state, a layout's slots */
};

HRESULT STDMETHODCALLTYPE dirk_g_IUnknown_QueryInterface(IUnknown *This, REFIID riid, void **out);
ULONG STDMETHODCALLTYPE dirk_g_IUnknown_AddRef(IUnknown *This);
ULONG STDMETHODCALLTYPE dirk_g_IUnknown_Release(IUnknown *This);

static inline struct dirk_proxy *dirk_proxy_of(const void *com)
{
    return (struct dirk_proxy *)com;
}

/* Sets the transport, before any call. */
void dirk_guest_init(const struct dirk_transport *transport);

/* Starts a record of `size` bytes at most for self's object, taking the
 * stream's lock. NULL if the host is gone (the lock is not taken). */
uint8_t *dirk_begin(struct dirk_proxy *self, uint32_t op, size_t size, int wait);

/* Ends the record begun. Not waiting: releases the lock, returns non-NULL.
 * Waiting: sends the stream and returns the answer after its status, still
 * holding the lock (dirk_reply_done releases it); or NULL, the lock
 * released, if the host could not answer. */
const uint8_t *dirk_end(struct dirk_proxy *self, uint8_t *w);
void dirk_reply_done(struct dirk_proxy *self);

/* Sends what the stream holds, without waiting. */
void dirk_flush(struct dirk_proxy *self);

/* A new proxy with a new handle, one reference. Under the stream's lock. */
struct dirk_proxy *dirk_proxy_new(struct dirk_proxy *self, int iface);
/* Forgets a proxy the host never made an object for. Under the lock. */
void dirk_proxy_drop(struct dirk_proxy *proxy);

static inline uint32_t dirk_handle_of(void *com)
{
    return com ? dirk_proxy_of(com)->handle : 0;
}

void dirk_unsupported(void *self, const char *method);

/* DIRK_WIRE_TRACE=1 (off by default): each method's first call, and the
 * capability checks with their answers, to stderr. */
extern int dirk_trace_on;
void dirk_trace_first(const char *method);
void dirk_trace(const char *fmt, ...);

/* The stream's lock, for what shares it. */
void dirk_lock(void);
void dirk_unlock(void);

/* dirk_draw.c: the bytes a draw reads that no-overwrite maps wrote. */
int dirk_stats_on(void);
double dirk_now_ms(void);
/* One send of len bytes that took ms; a waited one's last record is op's. */
void dirk_stats_send(int wait, unsigned op, size_t len, double ms);
void dirk_draw_created(struct dirk_proxy *p, const D3D11_SUBRESOURCE_DATA *data);
/* At an Unmap: nonzero when nothing is to be sent now. Else what is sent
 * is the mapping's whole size, counted. */
int dirk_draw_unmap(void *context, struct dirk_proxy *resource, D3D11_MAP type);
/* A resource written other than through a map. */
void dirk_draw_written(struct dirk_proxy *resource, const D3D11_BOX *box, const void *data);
void dirk_draw_free(struct dirk_proxy *p);
void dirk_pending_remove(struct dirk_proxy *p);
void dirk_draw_init(void);

uint8_t *dirk_put_string(uint8_t *w, const void *s, int char_size);
size_t dirk_wire_string_size(const void *s, int char_size);
const uint8_t *dirk_skip_string(const uint8_t *r);
uint8_t *dirk_put_blob(uint8_t *w, const void *data, size_t size);
const uint8_t *dirk_get_blob(const uint8_t *r, void *data, size_t size);
uint8_t *dirk_put_vals(uint8_t *w, const void *v, size_t size, size_t width, size_t count);
const uint8_t *dirk_get_vals(const uint8_t *r, void *v, size_t size, size_t width, size_t count);
uint8_t *dirk_put_handles(uint8_t *w, void *const *objects, size_t count);
/* Proxies for the handles of an answer, each with a reference. Under the
 * lock. */
const uint8_t *dirk_get_handles(const uint8_t *r, struct dirk_proxy *self, void **out, int iface);
const uint8_t *dirk_get_riid_handle(const uint8_t *r, struct dirk_proxy *self, REFIID riid, void **out);

static inline const uint8_t *dirk_skip(const uint8_t *r, size_t n) { return r + n; }

/* The entry points, as the shim exports them. */
HRESULT WINAPI dirk_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type,
        HMODULE swrast, UINT flags, const D3D_FEATURE_LEVEL *feature_levels, UINT levels,
        UINT sdk_version, ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level,
        ID3D11DeviceContext **context);
HRESULT WINAPI dirk_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE driver_type, HMODULE swrast, UINT flags,
        const D3D_FEATURE_LEVEL *feature_levels, UINT levels, UINT sdk_version,
        const DXGI_SWAP_CHAIN_DESC *swapchain_desc, IDXGISwapChain **swapchain,
        ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context);
HRESULT WINAPI dirk_CreateDXGIFactory2(UINT flags, REFIID riid, void **factory);

/* A window a call names (generated code, before the record): told to the
 * transport. dirk_windows_check: each window seen, again (a present, a
 * resize: the window may have changed size). */
void dirk_window_seen(uint64_t window);
void dirk_windows_check(void);

/* Tells the host which surface shows a window, and its size. */
void dirk_guest_window_surface(uint64_t window, uint32_t kind, uint32_t id,
                               uint32_t width, uint32_t height);

#endif
