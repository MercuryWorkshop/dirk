/*
 * dirk's guest side: the stream, the handle table, and the calls the
 * generator leaves to it (dirk_wire_guest.c has the rest): IUnknown, the
 * creates and copies whose data sizes follow from a resource's desc, maps,
 * and the entry points.
 */
#include <stdio.h>
#include <stdlib.h>

#include "dirk_format.h"
#include "dirk_guest.h"

#ifdef _WIN32
static CRITICAL_SECTION stream_lock;
static INIT_ONCE stream_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK lock_init(INIT_ONCE *once, void *param, void **ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&stream_lock);
    return TRUE;
}
static void lock(void) { InitOnceExecuteOnce(&stream_once, lock_init, NULL, NULL); EnterCriticalSection(&stream_lock); }
static void unlock(void) { LeaveCriticalSection(&stream_lock); }
#else
#include <pthread.h>
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&stream_lock); }
static void unlock(void) { pthread_mutex_unlock(&stream_lock); }
#endif

/* The stream: records not yet sent. */
static struct {
    struct dirk_transport transport;
    uint8_t *buf;
    size_t len, cap;
    uint8_t *rec;           /* the record begun */
    int wait;
    uint32_t next_handle;
    int gone;
    int debug;
} s = { .next_handle = 1 };

/* Sent without waiting once the stream holds this much. */
#define FLUSH_SIZE (256 * 1024)

#define TABLE_SIZE 4096
static struct dirk_proxy *table[TABLE_SIZE];

static struct dirk_proxy *find(uint32_t handle)
{
    struct dirk_proxy *p = table[handle % TABLE_SIZE];
    while (p && p->handle != handle)
        p = p->next;
    return p;
}

static void insert(struct dirk_proxy *p)
{
    p->next = table[p->handle % TABLE_SIZE];
    table[p->handle % TABLE_SIZE] = p;
}

static void remove_proxy(struct dirk_proxy *p)
{
    struct dirk_proxy **at = &table[p->handle % TABLE_SIZE];
    while (*at && *at != p)
        at = &(*at)->next;
    if (*at)
        *at = p->next;
}

void dirk_guest_init(const struct dirk_transport *transport)
{
    s.transport = *transport;
    s.debug = getenv("DIRK_WIRE_DEBUG") != NULL;
}

static int send_held(int wait, const uint8_t **reply)
{
    const uint8_t *r = NULL;
    if (s.gone)
        return 0;
    if (s.len || wait)
        r = s.transport.send(s.transport.ctx, s.buf, s.len, wait);
    else
        r = s.buf;
    s.len = 0;
    if (!r) {
        s.gone = 1;
        return 0;
    }
    if (reply)
        *reply = r;
    return 1;
}

/* Room for a record of size bytes, under the lock. */
static uint8_t *reserve(size_t size)
{
    if (s.len + size > s.cap) {
        if (s.len && !send_held(0, NULL))
            return NULL;
        if (size > s.cap) {
            size_t cap = s.cap ? s.cap : 64 * 1024;
            while (cap < size)
                cap *= 2;
            uint8_t *buf = realloc(s.buf, cap);
            if (!buf)
                return NULL;
            s.buf = buf;
            s.cap = cap;
        }
    }
    return s.buf + s.len;
}

static uint8_t *begin_held(uint32_t handle, uint32_t op, size_t size, int wait)
{
    uint8_t *rec = s.gone ? NULL : reserve(size);
    if (!rec)
        return NULL;
    uint16_t op16 = (uint16_t)op, flags = wait ? DIRK_REC_WAIT : 0;
    memcpy(rec + 4, &op16, 2);
    memcpy(rec + 6, &flags, 2);
    memcpy(rec + 8, &handle, 4);
    s.rec = rec;
    s.wait = wait;
    return rec + DIRK_REC_HEADER;
}

static const uint8_t *end_held(uint8_t *w)
{
    uint32_t size = (uint32_t)(w - s.rec);
    const uint8_t *reply;
    uint32_t status;
    memcpy(s.rec, &size, 4);
    s.len += size;
    if (!s.wait) {
        if (s.len >= FLUSH_SIZE)
            send_held(0, NULL);
        return w;
    }
    if (!send_held(1, &reply))
        return NULL;
    memcpy(&status, reply, 4);
    if (status != DIRK_REPLY_OK)
        return NULL;
    return reply + 4;
}

uint8_t *dirk_begin(struct dirk_proxy *self, uint32_t op, size_t size, int wait)
{
    uint8_t *w;
    lock();
    w = begin_held(self ? self->handle : 0, op, size, wait);
    if (!w)
        unlock();
    return w;
}

const uint8_t *dirk_end(struct dirk_proxy *self, uint8_t *w)
{
    int wait = s.wait;
    const uint8_t *r = end_held(w);
    (void)self;
    if (!wait || !r)
        unlock();
    return r;
}

void dirk_reply_done(struct dirk_proxy *self)
{
    (void)self;
    unlock();
}

void dirk_flush(struct dirk_proxy *self)
{
    (void)self;
    lock();
    send_held(0, NULL);
    unlock();
}

/* ------------------------------------------------------------- proxies */

static struct dirk_proxy *proxy_make(uint32_t handle, int iface)
{
    struct dirk_proxy *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->vtbl = dirk_ifaces[iface].vtbl;
    p->refs = 1;
    p->handle = handle;
    p->iface = iface;
    insert(p);
    return p;
}

struct dirk_proxy *dirk_proxy_new(struct dirk_proxy *self, int iface)
{
    (void)self;
    uint32_t handle = s.next_handle++;
    if (s.next_handle >= DIRK_HOST_HANDLE)
        s.next_handle = 1;
    return proxy_make(handle, iface);
}

static void proxy_free(struct dirk_proxy *p)
{
    if (p->resource) {
        for (uint32_t i = 0; i < p->resource->sub_count; i++)
            free(p->resource->maps[i].data);
        free(p->resource->maps);
        free(p->resource);
    }
    free(p);
}

void dirk_proxy_drop(struct dirk_proxy *p)
{
    remove_proxy(p);
    proxy_free(p);
}

/* Whether interface a is b or derives from it. */
static int derives(int a, int b)
{
    for (; a >= 0; a = dirk_ifaces[a].base)
        if (a == b)
            return 1;
    return 0;
}

static int iface_of(REFIID riid)
{
    for (int i = 0; i < DIRK_IFACE_COUNT; i++)
        if (!memcmp(&dirk_ifaces[i].iid, riid, sizeof(GUID)))
            return i;
    return -1;
}

static void release_held(uint32_t handle)
{
    uint8_t *w = begin_held(handle, DIRK_OP_RELEASE, DIRK_REC_HEADER, 0);
    if (w)
        end_held(w);
}

/* The proxy for a handle the host answered with, as interface iface, with
 * a reference for the caller. Under the lock. */
static struct dirk_proxy *proxy_for(uint32_t handle, int iface)
{
    struct dirk_proxy *p;
    if (!handle)
        return NULL;
    p = find(handle);
    if (!p)
        return proxy_make(handle, iface);
    InterlockedIncrement(&p->refs);
    if (derives(p->iface, iface))
        return p;
    if (derives(iface, p->iface)) {
        /* A derived interface's vtable starts with its base's. */
        p->vtbl = dirk_ifaces[iface].vtbl;
        p->iface = iface;
        return p;
    }
    /* The same object as an unrelated interface: a second handle. */
    InterlockedDecrement(&p->refs);
    struct dirk_proxy *q = dirk_proxy_new(NULL, iface);
    uint8_t *w = q ? begin_held(handle, DIRK_OP_DUPLICATE, DIRK_REC_HEADER + 4, 0) : NULL;
    if (!w) {
        if (q)
            dirk_proxy_drop(q);
        return NULL;
    }
    w = dirk_put_u32(w, q->handle);
    end_held(w);
    return q;
}

const uint8_t *dirk_get_handles(const uint8_t *r, struct dirk_proxy *self, void **out, int iface)
{
    uint32_t count, handle;
    (void)self;
    r = dirk_get_val(r, &count, 4, 4);
    for (uint32_t i = 0; i < count; i++) {
        r = dirk_get_val(r, &handle, 4, 4);
        struct dirk_proxy *p = proxy_for(handle, iface);
        if (out)
            out[i] = p;
        else if (p)
            InterlockedDecrement(&p->refs);
    }
    return r;
}

const uint8_t *dirk_get_riid_handle(const uint8_t *r, struct dirk_proxy *self, REFIID riid, void **out)
{
    uint32_t handle;
    int iface = iface_of(riid);
    (void)self;
    r = dirk_get_val(r, &handle, 4, 4);
    if (!out)
        return r;
    *out = NULL;
    if (handle && iface < 0) {
        /* An interface the wire does not carry. */
        if (s.debug)
            fprintf(stderr, "dirk: an object as an interface not carried\n");
        struct dirk_proxy *p = find(handle);
        if (!p)
            release_held(handle);
        return r;
    }
    *out = proxy_for(handle, iface);
    return r;
}

/* ------------------------------------------------------------- IUnknown */

HRESULT STDMETHODCALLTYPE dirk_g_IUnknown_QueryInterface(IUnknown *This, REFIID riid, void **out)
{
    struct dirk_proxy *self = dirk_proxy_of(This);
    int iface = iface_of(riid);
    HRESULT hr;
    uint32_t handle;
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!memcmp(riid, &IID_IUnknown, sizeof(GUID)) || (iface >= 0 && derives(self->iface, iface))) {
        InterlockedIncrement(&self->refs);
        *out = This;
        return S_OK;
    }
    if (iface < 0)
        return E_NOINTERFACE;
    uint8_t *w = dirk_begin(self, DIRK_OP_QUERY_INTERFACE, DIRK_REC_HEADER + 16, 1);
    if (!w)
        return DXGI_ERROR_DEVICE_REMOVED;
    memcpy(w, riid, 16);
    const uint8_t *r = dirk_end(self, w + 16);
    if (!r)
        return DXGI_ERROR_DEVICE_REMOVED;
    r = dirk_get_val(r, &hr, sizeof(hr), 8);
    r = dirk_get_val(r, &handle, 4, 4);
    if (SUCCEEDED(hr) && handle)
        *out = proxy_for(handle, iface);
    dirk_reply_done(self);
    if (SUCCEEDED(hr) && !*out)
        hr = E_NOINTERFACE;
    return hr;
}

ULONG STDMETHODCALLTYPE dirk_g_IUnknown_AddRef(IUnknown *This)
{
    return InterlockedIncrement(&dirk_proxy_of(This)->refs);
}

ULONG STDMETHODCALLTYPE dirk_g_IUnknown_Release(IUnknown *This)
{
    struct dirk_proxy *self = dirk_proxy_of(This);
    LONG refs = self->refs;
    /* Above one, no lock: the proxy stays. To zero, under the lock, so an
     * answer naming the handle cannot find the proxy as it goes. */
    while (refs > 1) {
        LONG seen = InterlockedCompareExchange(&self->refs, refs - 1, refs);
        if (seen == refs)
            return refs - 1;
        refs = seen;
    }
    lock();
    refs = InterlockedDecrement(&self->refs);
    if (!refs) {
        remove_proxy(self);
        release_held(self->handle);
    }
    unlock();
    if (!refs)
        proxy_free(self);
    return refs;
}

/* -------------------------------------------------------------- strings */

static size_t string_len(const void *str, int char_size)
{
    size_t n = 0;
    if (char_size == 1)
        return strlen(str);
    while (((const uint16_t *)str)[n])
        n++;
    return n;
}

size_t dirk_wire_string_size(const void *str, int char_size)
{
    return 4 + (str ? string_len(str, char_size) * char_size : 0);
}

uint8_t *dirk_put_string(uint8_t *w, const void *str, int char_size)
{
    if (!str)
        return dirk_put_u32(w, ~0u);
    size_t n = string_len(str, char_size);
    w = dirk_put_u32(w, (uint32_t)n);
    memcpy(w, str, n * char_size);
    return w + n * char_size;
}

const uint8_t *dirk_skip_string(const uint8_t *r)
{
    uint32_t n;
    memcpy(&n, r, 4);
    return r + 4 + (n == ~0u ? 0 : n);
}

uint8_t *dirk_put_blob(uint8_t *w, const void *data, size_t size)
{
    if (!data)
        return dirk_put_u64(w, ~(uint64_t)0);
    w = dirk_put_u64(w, size);
    memcpy(w, data, size);
    return w + size;
}

const uint8_t *dirk_get_blob(const uint8_t *r, void *data, size_t size)
{
    uint64_t n;
    memcpy(&n, r, 8);
    r += 8;
    if (n == ~(uint64_t)0)
        return r;
    if (data)
        memcpy(data, r, n < size ? n : size);
    return r + n;
}

uint8_t *dirk_put_vals(uint8_t *w, const void *v, size_t size, size_t width, size_t count)
{
    if (!v)
        return dirk_put_u32(w, ~0u);
    w = dirk_put_u32(w, (uint32_t)count);
    for (size_t i = 0; i < count; i++)
        w = dirk_put_val(w, (const uint8_t *)v + i * size, size, width);
    return w;
}

const uint8_t *dirk_get_vals(const uint8_t *r, void *v, size_t size, size_t width, size_t count)
{
    uint32_t n;
    r = dirk_get_val(r, &n, 4, 4);
    for (uint32_t i = 0; i < n; i++) {
        if (v && i < count)
            dirk_get_val(r, (uint8_t *)v + i * size, size, width);
        r += width;
    }
    return r;
}

uint8_t *dirk_put_handles(uint8_t *w, void *const *objects, size_t count)
{
    if (!objects)
        return dirk_put_u32(w, ~0u);
    w = dirk_put_u32(w, (uint32_t)count);
    for (size_t i = 0; i < count; i++)
        w = dirk_put_u32(w, dirk_handle_of(objects[i]));
    return w;
}

void dirk_unsupported(void *self, const char *method)
{
    (void)self;
    if (s.debug)
        fprintf(stderr, "dirk: %s is not supported\n", method);
}

/* ------------------------------------------------------------ resources */

size_t dirk_size_D3D11_BUFFER_DESC(const D3D11_BUFFER_DESC *v);
uint8_t *dirk_put_D3D11_BUFFER_DESC(uint8_t *w, const D3D11_BUFFER_DESC *v);
size_t dirk_size_D3D11_TEXTURE1D_DESC(const D3D11_TEXTURE1D_DESC *v);
uint8_t *dirk_put_D3D11_TEXTURE1D_DESC(uint8_t *w, const D3D11_TEXTURE1D_DESC *v);
size_t dirk_size_D3D11_TEXTURE2D_DESC(const D3D11_TEXTURE2D_DESC *v);
uint8_t *dirk_put_D3D11_TEXTURE2D_DESC(uint8_t *w, const D3D11_TEXTURE2D_DESC *v);
size_t dirk_size_D3D11_TEXTURE3D_DESC(const D3D11_TEXTURE3D_DESC *v);
uint8_t *dirk_put_D3D11_TEXTURE3D_DESC(uint8_t *w, const D3D11_TEXTURE3D_DESC *v);
size_t dirk_size_D3D11_TEXTURE2D_DESC1(const D3D11_TEXTURE2D_DESC1 *v);
uint8_t *dirk_put_D3D11_TEXTURE2D_DESC1(uint8_t *w, const D3D11_TEXTURE2D_DESC1 *v);
size_t dirk_size_D3D11_TEXTURE3D_DESC1(const D3D11_TEXTURE3D_DESC1 *v);
uint8_t *dirk_put_D3D11_TEXTURE3D_DESC1(uint8_t *w, const D3D11_TEXTURE3D_DESC1 *v);

/* A subresource's size in pixels, its format, the subresource count. */
struct sub_dims {
    uint32_t w, h, d, format, count;
    int buffer;
};

static int sub_dims(const struct dirk_resource *res, UINT sub, struct sub_dims *o)
{
    uint32_t mips, level;
    memset(o, 0, sizeof(*o));
    switch (res->kind) {
    case DIRK_RESOURCE_BUFFER:
        o->w = res->desc.buffer.ByteWidth;
        o->h = o->d = 1;
        o->count = 1;
        o->buffer = 1;
        return sub == 0;
    case DIRK_RESOURCE_TEXTURE1D:
        mips = res->desc.tex1d.MipLevels;
        level = sub % mips;
        o->w = dirk_mip(res->desc.tex1d.Width, level);
        o->h = o->d = 1;
        o->format = res->desc.tex1d.Format;
        o->count = mips * res->desc.tex1d.ArraySize;
        return sub < o->count;
    case DIRK_RESOURCE_TEXTURE2D:
    case DIRK_RESOURCE_TEXTURE2D1:
        mips = res->desc.tex2d.MipLevels;
        level = sub % mips;
        o->w = dirk_mip(res->desc.tex2d.Width, level);
        o->h = dirk_mip(res->desc.tex2d.Height, level);
        o->d = 1;
        o->format = res->desc.tex2d.Format;
        o->count = mips * res->desc.tex2d.ArraySize;
        return sub < o->count;
    case DIRK_RESOURCE_TEXTURE3D:
    case DIRK_RESOURCE_TEXTURE3D1:
        mips = res->desc.tex3d.MipLevels;
        o->w = dirk_mip(res->desc.tex3d.Width, sub);
        o->h = dirk_mip(res->desc.tex3d.Height, sub);
        o->d = dirk_mip(res->desc.tex3d.Depth, sub);
        o->format = res->desc.tex3d.Format;
        o->count = mips;
        return sub < o->count;
    }
    return 0;
}

/* The guest's record of a resource: its desc, asked of the host if the
 * host made it. */
static struct dirk_resource *resource_of(ID3D11Resource *resource)
{
    struct dirk_proxy *p = dirk_proxy_of(resource);
    D3D11_RESOURCE_DIMENSION dim;
    if (p->resource)
        return p->resource;
    struct dirk_resource *res = calloc(1, sizeof(*res));
    if (!res)
        return NULL;
    ID3D11Resource_GetType(resource, &dim);
    switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER:
        res->kind = DIRK_RESOURCE_BUFFER;
        ID3D11Buffer_GetDesc((ID3D11Buffer *)resource, &res->desc.buffer);
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        res->kind = DIRK_RESOURCE_TEXTURE1D;
        ID3D11Texture1D_GetDesc((ID3D11Texture1D *)resource, &res->desc.tex1d);
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        res->kind = DIRK_RESOURCE_TEXTURE2D;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)resource, &res->desc.tex2d);
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        res->kind = DIRK_RESOURCE_TEXTURE3D;
        ID3D11Texture3D_GetDesc((ID3D11Texture3D *)resource, &res->desc.tex3d);
        break;
    default:
        free(res);
        return NULL;
    }
    p->resource = res;
    return res;
}

static HRESULT create_resource(struct dirk_proxy *self, int kind, const void *desc, size_t desc_size,
                               const D3D11_SUBRESOURCE_DATA *data, int iface, void **out)
{
    struct dirk_resource res;
    struct sub_dims d;
    size_t size = DIRK_REC_HEADER + 4 + desc_size + 4 + 4;
    uint32_t count = 0;

    memset(&res, 0, sizeof(res));
    res.kind = kind;
    memcpy(&res.desc, desc, kind == DIRK_RESOURCE_BUFFER ? sizeof(D3D11_BUFFER_DESC) :
           kind == DIRK_RESOURCE_TEXTURE1D ? sizeof(D3D11_TEXTURE1D_DESC) :
           kind == DIRK_RESOURCE_TEXTURE2D || kind == DIRK_RESOURCE_TEXTURE2D1 ?
           sizeof(D3D11_TEXTURE2D_DESC) : sizeof(D3D11_TEXTURE3D_DESC));
    /* MipLevels 0: the whole chain. */
    if (kind == DIRK_RESOURCE_TEXTURE1D && !res.desc.tex1d.MipLevels)
        res.desc.tex1d.MipLevels = dirk_mip_count(res.desc.tex1d.Width, 1, 1);
    if ((kind == DIRK_RESOURCE_TEXTURE2D || kind == DIRK_RESOURCE_TEXTURE2D1) && !res.desc.tex2d.MipLevels)
        res.desc.tex2d.MipLevels = dirk_mip_count(res.desc.tex2d.Width, res.desc.tex2d.Height, 1);
    if ((kind == DIRK_RESOURCE_TEXTURE3D || kind == DIRK_RESOURCE_TEXTURE3D1) && !res.desc.tex3d.MipLevels)
        res.desc.tex3d.MipLevels = dirk_mip_count(res.desc.tex3d.Width, res.desc.tex3d.Height,
                                                  res.desc.tex3d.Depth);
    sub_dims(&res, 0, &d);
    if (data) {
        count = d.count;
        for (uint32_t i = 0; i < count; i++) {
            sub_dims(&res, i, &d);
            size += 8 + 8 + (d.buffer ? d.w : dirk_data_size(d.format, d.w, d.h, d.d,
                             data[i].SysMemPitch, data[i].SysMemSlicePitch));
        }
    }

    int wait = out == NULL;
    uint8_t *w = dirk_begin(self, DIRK_OP_CREATE_RESOURCE, size, wait);
    if (!w)
        return DXGI_ERROR_DEVICE_REMOVED;
    w = dirk_put_u32(w, kind);
    switch (kind) {
    case DIRK_RESOURCE_BUFFER: w = dirk_put_D3D11_BUFFER_DESC(w, desc); break;
    case DIRK_RESOURCE_TEXTURE1D: w = dirk_put_D3D11_TEXTURE1D_DESC(w, desc); break;
    case DIRK_RESOURCE_TEXTURE2D: w = dirk_put_D3D11_TEXTURE2D_DESC(w, desc); break;
    case DIRK_RESOURCE_TEXTURE3D: w = dirk_put_D3D11_TEXTURE3D_DESC(w, desc); break;
    case DIRK_RESOURCE_TEXTURE2D1: w = dirk_put_D3D11_TEXTURE2D_DESC1(w, desc); break;
    case DIRK_RESOURCE_TEXTURE3D1: w = dirk_put_D3D11_TEXTURE3D_DESC1(w, desc); break;
    }
    struct dirk_proxy *p = out ? dirk_proxy_new(self, iface) : NULL;
    struct dirk_resource *kept = p ? malloc(sizeof(*kept)) : NULL;
    if (p && kept) {
        *kept = res;
        p->resource = kept;
    }
    w = dirk_put_u32(w, p ? p->handle : 0);
    w = dirk_put_u32(w, data ? count : ~0u);
    for (uint32_t i = 0; data && i < count; i++) {
        sub_dims(&res, i, &d);
        w = dirk_put_u32(w, data[i].SysMemPitch);
        w = dirk_put_u32(w, data[i].SysMemSlicePitch);
        w = dirk_put_blob(w, data[i].pSysMem, d.buffer ? d.w : dirk_data_size(d.format, d.w, d.h, d.d,
                          data[i].SysMemPitch, data[i].SysMemSlicePitch));
    }
    const uint8_t *r = dirk_end(self, w);
    if (!wait) {
        *out = p;
        return S_OK;
    }
    if (!r)
        return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr;
    dirk_get_val(r, &hr, sizeof(hr), 8);
    dirk_reply_done(self);
    return hr;
}

#define DESC_SIZE(type, v) dirk_size_##type(v)

HRESULT STDMETHODCALLTYPE dirk_g_ID3D11Device_CreateBuffer(ID3D11Device *This, const D3D11_BUFFER_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, ID3D11Buffer **out)
{
    if (!desc)
        return E_INVALIDARG;
    return create_resource(dirk_proxy_of(This), DIRK_RESOURCE_BUFFER, desc,
                           DESC_SIZE(D3D11_BUFFER_DESC, desc), data, DIRK_IFACE_ID3D11Buffer, (void **)out);
}

HRESULT STDMETHODCALLTYPE dirk_g_ID3D11Device_CreateTexture1D(ID3D11Device *This, const D3D11_TEXTURE1D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, ID3D11Texture1D **out)
{
    if (!desc)
        return E_INVALIDARG;
    return create_resource(dirk_proxy_of(This), DIRK_RESOURCE_TEXTURE1D, desc,
                           DESC_SIZE(D3D11_TEXTURE1D_DESC, desc), data, DIRK_IFACE_ID3D11Texture1D, (void **)out);
}

HRESULT STDMETHODCALLTYPE dirk_g_ID3D11Device_CreateTexture2D(ID3D11Device *This, const D3D11_TEXTURE2D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, ID3D11Texture2D **out)
{
    if (!desc)
        return E_INVALIDARG;
    return create_resource(dirk_proxy_of(This), DIRK_RESOURCE_TEXTURE2D, desc,
                           DESC_SIZE(D3D11_TEXTURE2D_DESC, desc), data, DIRK_IFACE_ID3D11Texture2D1, (void **)out);
}

HRESULT STDMETHODCALLTYPE dirk_g_ID3D11Device_CreateTexture3D(ID3D11Device *This, const D3D11_TEXTURE3D_DESC *desc,
        const D3D11_SUBRESOURCE_DATA *data, ID3D11Texture3D **out)
{
    if (!desc)
        return E_INVALIDARG;
    return create_resource(dirk_proxy_of(This), DIRK_RESOURCE_TEXTURE3D, desc,
                           DESC_SIZE(D3D11_TEXTURE3D_DESC, desc), data, DIRK_IFACE_ID3D11Texture3D1, (void **)out);
}

HRESULT STDMETHODCALLTYPE dirk_g_ID3D11Device3_CreateTexture2D1(ID3D11Device3 *This, const D3D11_TEXTURE2D_DESC1 *desc,
        const D3D11_SUBRESOURCE_DATA *data, ID3D11Texture2D1 **out)
{
    if (!desc)
        return E_INVALIDARG;
    return create_resource(dirk_proxy_of(This), DIRK_RESOURCE_TEXTURE2D1, desc,
                           DESC_SIZE(D3D11_TEXTURE2D_DESC1, desc), data, DIRK_IFACE_ID3D11Texture2D1, (void **)out);
}

HRESULT STDMETHODCALLTYPE dirk_g_ID3D11Device3_CreateTexture3D1(ID3D11Device3 *This, const D3D11_TEXTURE3D_DESC1 *desc,
        const D3D11_SUBRESOURCE_DATA *data, ID3D11Texture3D1 **out)
{
    if (!desc)
        return E_INVALIDARG;
    return create_resource(dirk_proxy_of(This), DIRK_RESOURCE_TEXTURE3D1, desc,
                           DESC_SIZE(D3D11_TEXTURE3D_DESC1, desc), data, DIRK_IFACE_ID3D11Texture3D1, (void **)out);
}

/* ----------------------------------------------------------------- maps */

static struct dirk_mapping *mapping_of(struct dirk_resource *res, UINT sub)
{
    struct sub_dims d;
    if (!sub_dims(res, sub, &d))
        return NULL;
    if (!res->maps) {
        res->maps = calloc(d.count, sizeof(*res->maps));
        if (!res->maps)
            return NULL;
        res->sub_count = d.count;
    }
    return &res->maps[sub];
}

static int map_size(struct dirk_mapping *m, size_t size)
{
    if (m->size >= size)
        return 1;
    uint8_t *data = realloc(m->data, size);
    if (!data)
        return 0;
    if (size > m->size)
        memset(data + m->size, 0, size - m->size);
    m->data = data;
    m->size = size;
    return 1;
}

HRESULT STDMETHODCALLTYPE dirk_g_ID3D11DeviceContext_Map(ID3D11DeviceContext *This, ID3D11Resource *resource,
        UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
    struct dirk_proxy *self = dirk_proxy_of(This);
    struct dirk_resource *res = resource ? resource_of(resource) : NULL;
    struct dirk_mapping *m = res ? mapping_of(res, sub) : NULL;
    HRESULT hr;
    uint32_t row, depth, has_data;
    uint64_t size;

    if (mapped)
        memset(mapped, 0, sizeof(*mapped));
    if (!m)
        return E_INVALIDARG;
    if (res->kind == DIRK_RESOURCE_BUFFER &&
        (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE)) {
        /* The bytes go at Unmap; the memory keeps what was written last. */
        if (!map_size(m, res->desc.buffer.ByteWidth))
            return E_OUTOFMEMORY;
        m->row_pitch = m->depth_pitch = res->desc.buffer.ByteWidth;
        m->type = type;
    } else {
        uint8_t *w = dirk_begin(self, DIRK_OP_MAP, DIRK_REC_HEADER + 16, 1);
        if (!w)
            return DXGI_ERROR_DEVICE_REMOVED;
        w = dirk_put_u32(w, dirk_handle_of(resource));
        w = dirk_put_u32(w, sub);
        w = dirk_put_u32(w, type);
        w = dirk_put_u32(w, flags);
        const uint8_t *r = dirk_end(self, w);
        if (!r)
            return DXGI_ERROR_DEVICE_REMOVED;
        r = dirk_get_val(r, &hr, sizeof(hr), 8);
        r = dirk_get_val(r, &row, 4, 4);
        r = dirk_get_val(r, &depth, 4, 4);
        r = dirk_get_val(r, &size, 8, 8);
        r = dirk_get_val(r, &has_data, 4, 4);
        if (SUCCEEDED(hr) && map_size(m, size)) {
            if (has_data)
                memcpy(m->data, r, size);
            m->row_pitch = row;
            m->depth_pitch = depth;
            m->type = type;
        } else if (SUCCEEDED(hr)) {
            hr = E_OUTOFMEMORY;
        }
        dirk_reply_done(self);
        if (FAILED(hr) || hr == S_FALSE)
            return hr;
    }
    if (mapped) {
        mapped->pData = m->data;
        mapped->RowPitch = m->row_pitch;
        mapped->DepthPitch = m->depth_pitch;
    }
    return S_OK;
}

void STDMETHODCALLTYPE dirk_g_ID3D11DeviceContext_Unmap(ID3D11DeviceContext *This, ID3D11Resource *resource, UINT sub)
{
    struct dirk_proxy *self = dirk_proxy_of(This);
    struct dirk_resource *res = resource ? dirk_proxy_of(resource)->resource : NULL;
    struct dirk_mapping *m = res ? mapping_of(res, sub) : NULL;
    D3D11_MAP type;
    if (!m || !m->type)
        return;
    type = m->type;
    m->type = 0;
    if (type == D3D11_MAP_READ)
        return;
    uint8_t *w = dirk_begin(self, DIRK_OP_UNMAP, DIRK_REC_HEADER + 20 + 8 + m->size, 0);
    if (!w)
        return;
    w = dirk_put_u32(w, dirk_handle_of(resource));
    w = dirk_put_u32(w, sub);
    w = dirk_put_u32(w, type == D3D11_MAP_READ_WRITE ? D3D11_MAP_WRITE : type);
    w = dirk_put_u32(w, m->row_pitch);
    w = dirk_put_u32(w, m->depth_pitch);
    w = dirk_put_blob(w, m->data, m->size);
    dirk_end(self, w);
}

static void update_subresource(struct dirk_proxy *self, ID3D11Resource *resource, UINT sub,
        const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch, UINT copy_flags)
{
    struct dirk_resource *res = resource ? resource_of(resource) : NULL;
    struct sub_dims d;
    uint64_t size;
    if (!res || !data || !sub_dims(res, sub, &d))
        return;
    if (box) {
        if (box->right <= box->left || box->bottom <= box->top || box->back <= box->front)
            return;
        d.w = box->right - box->left;
        d.h = box->bottom - box->top;
        d.d = box->back - box->front;
    }
    size = d.buffer ? d.w : dirk_data_size(d.format, d.w, d.h, d.d, row_pitch, depth_pitch);
    uint8_t *w = dirk_begin(self, DIRK_OP_UPDATE_SUBRESOURCE, DIRK_REC_HEADER + 48 + 8 + size, 0);
    if (!w)
        return;
    w = dirk_put_u32(w, dirk_handle_of(resource));
    w = dirk_put_u32(w, sub);
    w = dirk_put_u32(w, box != NULL);
    w = dirk_put_u32(w, box ? box->left : 0);
    w = dirk_put_u32(w, box ? box->top : 0);
    w = dirk_put_u32(w, box ? box->front : 0);
    w = dirk_put_u32(w, box ? box->right : 0);
    w = dirk_put_u32(w, box ? box->bottom : 0);
    w = dirk_put_u32(w, box ? box->back : 0);
    w = dirk_put_u32(w, row_pitch);
    w = dirk_put_u32(w, depth_pitch);
    w = dirk_put_u32(w, copy_flags);
    w = dirk_put_blob(w, data, size);
    dirk_end(self, w);
}

void STDMETHODCALLTYPE dirk_g_ID3D11DeviceContext_UpdateSubresource(ID3D11DeviceContext *This,
        ID3D11Resource *resource, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch,
        UINT depth_pitch)
{
    update_subresource(dirk_proxy_of(This), resource, sub, box, data, row_pitch, depth_pitch, 0);
}

void STDMETHODCALLTYPE dirk_g_ID3D11DeviceContext1_UpdateSubresource1(ID3D11DeviceContext1 *This,
        ID3D11Resource *resource, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch,
        UINT depth_pitch, UINT copy_flags)
{
    update_subresource(dirk_proxy_of(This), resource, sub, box, data, row_pitch, depth_pitch, copy_flags);
}

/* --------------------------------------------------------- entry points */

HRESULT WINAPI dirk_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type,
        HMODULE swrast, UINT flags, const D3D_FEATURE_LEVEL *feature_levels, UINT levels,
        UINT sdk_version, ID3D11Device **device, D3D_FEATURE_LEVEL *feature_level,
        ID3D11DeviceContext **context)
{
    static const D3D_FEATURE_LEVEL defaults[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1,
    };
    HRESULT hr;
    D3D_FEATURE_LEVEL level;
    uint32_t ok;
    (void)swrast;
    if (device)
        *device = NULL;
    if (context)
        *context = NULL;
    if (!feature_levels || !levels) {
        feature_levels = defaults;
        levels = sizeof(defaults) / sizeof(defaults[0]);
    }
    uint8_t *w = dirk_begin(NULL, DIRK_OP_CREATE_DEVICE, DIRK_REC_HEADER + 24 + 4 * levels, 1);
    if (!w)
        return DXGI_ERROR_DEVICE_REMOVED;
    struct dirk_proxy *dev = device ? dirk_proxy_new(NULL, DIRK_IFACE_ID3D11Device5) : NULL;
    struct dirk_proxy *ctx = context ? dirk_proxy_new(NULL, DIRK_IFACE_ID3D11DeviceContext4) : NULL;
    w = dirk_put_u32(w, dirk_handle_of(adapter));
    w = dirk_put_u32(w, driver_type);
    w = dirk_put_u32(w, flags);
    w = dirk_put_u32(w, sdk_version);
    w = dirk_put_u32(w, dev ? dev->handle : 0);
    w = dirk_put_u32(w, ctx ? ctx->handle : 0);
    w = dirk_put_vals(w, feature_levels, sizeof(*feature_levels), 4, levels);
    const uint8_t *r = dirk_end(NULL, w);
    if (!r) {
        /* The lock is not held: the proxies are not in a table anyone can
         * reach through the host, so they can go without it. */
        lock();
        if (dev) dirk_proxy_drop(dev);
        if (ctx) dirk_proxy_drop(ctx);
        unlock();
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    r = dirk_get_val(r, &hr, sizeof(hr), 8);
    r = dirk_get_val(r, &level, sizeof(level), 4);
    r = dirk_get_val(r, &ok, 4, 4);
    if (!ok) {
        if (dev) dirk_proxy_drop(dev);
        if (ctx) dirk_proxy_drop(ctx);
        dev = ctx = NULL;
    }
    dirk_reply_done(NULL);
    if (feature_level)
        *feature_level = level;
    if (device)
        *device = (ID3D11Device *)dev;
    if (context)
        *context = (ID3D11DeviceContext *)ctx;
    return hr;
}

HRESULT WINAPI dirk_CreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
    int iface = iface_of(riid);
    HRESULT hr;
    uint32_t ok;
    if (!factory)
        return E_INVALIDARG;
    *factory = NULL;
    if (iface < 0)
        return E_NOINTERFACE;
    uint8_t *w = dirk_begin(NULL, DIRK_OP_CREATE_FACTORY, DIRK_REC_HEADER + 24, 1);
    if (!w)
        return DXGI_ERROR_DEVICE_REMOVED;
    struct dirk_proxy *p = dirk_proxy_new(NULL, iface);
    memcpy(w, riid, 16);
    w = dirk_put_u32(w + 16, flags);
    w = dirk_put_u32(w, p ? p->handle : 0);
    const uint8_t *r = dirk_end(NULL, w);
    if (!r) {
        lock();
        if (p) dirk_proxy_drop(p);
        unlock();
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    r = dirk_get_val(r, &hr, sizeof(hr), 8);
    r = dirk_get_val(r, &ok, 4, 4);
    if (!ok && p) {
        dirk_proxy_drop(p);
        p = NULL;
    }
    dirk_reply_done(NULL);
    *factory = p;
    return hr;
}

HRESULT WINAPI dirk_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE driver_type, HMODULE swrast, UINT flags,
        const D3D_FEATURE_LEVEL *feature_levels, UINT levels, UINT sdk_version,
        const DXGI_SWAP_CHAIN_DESC *swapchain_desc, IDXGISwapChain **swapchain,
        ID3D11Device **device_out, D3D_FEATURE_LEVEL *feature_level, ID3D11DeviceContext **context)
{
    ID3D11Device *device = NULL;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *dxgi_adapter = NULL;
    IDXGIFactory *factory = NULL;
    HRESULT hr;

    if (swapchain)
        *swapchain = NULL;
    if (swapchain && !swapchain_desc)
        return E_INVALIDARG;
    hr = dirk_D3D11CreateDevice(adapter, driver_type, swrast, flags, feature_levels, levels,
                                sdk_version, &device, feature_level, context);
    if (FAILED(hr))
        return hr;
    if (swapchain) {
        hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
        if (SUCCEEDED(hr))
            hr = IDXGIDevice_GetAdapter(dxgi_device, &dxgi_adapter);
        if (SUCCEEDED(hr))
            hr = IDXGIAdapter_GetParent(dxgi_adapter, &IID_IDXGIFactory, (void **)&factory);
        if (SUCCEEDED(hr)) {
            DXGI_SWAP_CHAIN_DESC desc = *swapchain_desc;
            hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &desc, swapchain);
        }
        if (factory) IDXGIFactory_Release(factory);
        if (dxgi_adapter) IDXGIAdapter_Release(dxgi_adapter);
        if (dxgi_device) IDXGIDevice_Release(dxgi_device);
        if (FAILED(hr)) {
            ID3D11Device_Release(device);
            if (context && *context) {
                ID3D11DeviceContext_Release(*context);
                *context = NULL;
            }
            return hr;
        }
    }
    if (device_out)
        *device_out = device;
    else
        ID3D11Device_Release(device);
    return hr;
}

void dirk_guest_window_surface(uint64_t window, uint32_t surface)
{
    uint8_t *w = dirk_begin(NULL, DIRK_OP_SURFACE, DIRK_REC_HEADER + 12, 0);
    if (!w)
        return;
    w = dirk_put_u64(w, window);
    w = dirk_put_u32(w, surface);
    dirk_end(NULL, w);
}
