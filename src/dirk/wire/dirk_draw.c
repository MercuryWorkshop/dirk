/*
 * dirk's guest: the bytes a draw reads that no-overwrite maps wrote.
 *
 * A map hands out guest memory, and its Unmap used to send the whole of it.
 * A game that streams its vertices into a ring buffer maps a buffer of a few
 * MB hundreds of times a frame with D3D11_MAP_WRITE_NO_OVERWRITE and writes a
 * few hundred bytes each time, so nearly everything sent was bytes the host
 * already had. Such a buffer's (a dynamic vertex or index buffer: lazy) Unmap
 * now sends nothing. A draw on the immediate context works out the bytes it
 * reads of each lazy buffer bound -- from the input layout, the vertex and
 * index buffer bindings and its own parameters, and for an indexed draw the
 * smallest and largest index -- and sends only those of them that differ
 * from what the host holds (host, a copy of what was sent). A draw this
 * cannot bound sends the whole buffer, as does anything else that reads one
 * (a copy, a command list).
 *
 * Deferred contexts keep the old way: their maps send everything at Unmap,
 * and a command list's execution first brings every lazy buffer up to date.
 *
 * DIRK_WIRE_STATS=1: the bytes maps sent per frame, every 64 frames.
 * DIRK_VERIFY=1: each range a draw reads is checked by the host against
 * what the guest holds, and a mismatch said (dirk_host.cpp).
 * DIRK_EAGER_MAPS=1: every Unmap sends everything, as before.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dirk_format.h"
#include "dirk_guest.h"

#define SLOTS D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT

static int stats, verify, eager;

int dirk_stats_on(void)
{
    return stats;
}

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}

void dirk_draw_init(void)
{
    stats = env_on("DIRK_WIRE_STATS");
    verify = env_on("DIRK_VERIFY");
    eager = env_on("DIRK_EAGER_MAPS");
}

/* ------------------------------------------------------------- stats */

static struct {
    uint64_t frames, unmaps, lazy_unmaps, unmap_bytes, ranges, range_bytes, whole, whole_bytes;
} st, st_was;

double dirk_now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    return 0;
#endif
}

/* Sends, by whether they waited, and the waited ones by the op that did. */
#define OPS 4096
static struct {
    uint64_t n[2], bytes;
    double ms[2], max_ms[2];
    uint32_t by_op[OPS];
    double ms_op[OPS];
} tx, tx_was;

void dirk_stats_send(int wait, unsigned op, size_t len, double ms)
{
    wait = wait != 0;
    tx.n[wait]++;
    tx.ms[wait] += ms;
    if (ms > tx.max_ms[wait])
        tx.max_ms[wait] = ms;
    tx.bytes += len;
    if (wait) {
        tx.by_op[op % OPS]++;
        tx.ms_op[op % OPS] += ms;
    }
}

static const char *op_name(unsigned op)
{
    static const char *const special[] = { "?", "Release", "QueryInterface", "Duplicate", "CreateDevice",
        "CreateFactory", "CreateResource", "Map", "Unmap", "UpdateSubresource", "Surface", "WriteRange", "Verify" };
    if (op < sizeof(special) / sizeof(*special))
        return special[op];
    const char *n = dirk_op_name(op);
    return n ? n : "?";
}

static void stats_frame(void)
{
    if (!stats || ++st.frames % 64)
        return;
    uint64_t n = st.frames - st_was.frames;
    uint64_t sends[2] = { tx.n[0] - tx_was.n[0], tx.n[1] - tx_was.n[1] };
    fprintf(stderr, "dirk: stats, frame %llu: per frame %.1f sends unwaited (%.3f ms each, max %.2f), "
            "%.1f waited (%.3f ms each, max %.2f), %.1f KiB sent\n", (unsigned long long)st.frames,
            (double)sends[0] / n, sends[0] ? (tx.ms[0] - tx_was.ms[0]) / sends[0] : 0, tx.max_ms[0],
            (double)sends[1] / n, sends[1] ? (tx.ms[1] - tx_was.ms[1]) / sends[1] : 0, tx.max_ms[1],
            (double)(tx.bytes - tx_was.bytes) / n / 1024);
    /* The waited ones' ops, most first. */
    for (int k = 0; k < 8; k++) {
        unsigned best = 0;
        uint32_t most = 0;
        for (unsigned op = 0; op < OPS; op++) {
            uint32_t c = tx.by_op[op] - tx_was.by_op[op];
            if (c > most) {
                most = c;
                best = op;
            }
        }
        if (!most)
            break;
        fprintf(stderr, "dirk: stats, waited for %s: %.2f per frame, %.3f ms each\n", op_name(best),
                (double)most / n, (tx.ms_op[best] - tx_was.ms_op[best]) / most);
        tx_was.by_op[best] = tx.by_op[best];
        tx_was.ms_op[best] = tx.ms_op[best];
    }
    memcpy(tx_was.by_op, tx.by_op, sizeof(tx.by_op));
    memcpy(tx_was.ms_op, tx.ms_op, sizeof(tx.ms_op));
    tx_was.n[0] = tx.n[0];
    tx_was.n[1] = tx.n[1];
    tx_was.ms[0] = tx.ms[0];
    tx_was.ms[1] = tx.ms[1];
    tx_was.bytes = tx.bytes;
    tx.max_ms[0] = tx.max_ms[1] = 0;
    fprintf(stderr, "dirk: stats, frame %llu: per frame %.1f Unmaps (%.1f sending nothing) sending %.1f KiB, "
            "%.1f draw ranges sending %.1f KiB, %.1f whole buffers sending %.1f KiB\n",
            (unsigned long long)st.frames, (double)(st.unmaps - st_was.unmaps) / n,
            (double)(st.lazy_unmaps - st_was.lazy_unmaps) / n, (double)(st.unmap_bytes - st_was.unmap_bytes) / n / 1024,
            (double)(st.ranges - st_was.ranges) / n, (double)(st.range_bytes - st_was.range_bytes) / n / 1024,
            (double)(st.whole - st_was.whole) / n, (double)(st.whole_bytes - st_was.whole_bytes) / n / 1024);
    st_was = st;
}

/* ------------------------------------------------- input layouts, state */

struct layout {
    uint32_t used, instanced;
    uint32_t end[SLOTS];        /* the bytes past an element's start an element of the slot reaches */
    uint32_t step[SLOTS];
};

struct context {
    int type;                   /* 0 not known yet, else D3D11_DEVICE_CONTEXT_TYPE + 1 */
    struct dirk_proxy *vb[SLOTS];
    uint32_t stride[SLOTS], offset[SLOTS];
    struct dirk_proxy *ib, *layout;
    DXGI_FORMAT ib_format;
    uint32_t ib_offset;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    /* Index bounds already found, while the index buffer is unwritten. */
    struct {
        struct dirk_proxy *ib;
        uint32_t writes, gen, at, count, format, min, max;
        int restart;
    } bounds[4];
    unsigned bounds_next;
};

static void hold(struct dirk_proxy *p)
{
    if (p)
        InterlockedIncrement(&p->refs);
}

static void drop(struct dirk_proxy *p)
{
    if (p)
        dirk_g_IUnknown_Release((IUnknown *)p);
}

static struct context *context_of(void *com)
{
    struct dirk_proxy *p = dirk_proxy_of(com);
    if (!p->draw)
        p->draw = calloc(1, sizeof(struct context));
    return p->draw;
}

static int deferred(void *com, struct context *c)
{
    if (!c->type) {
        ID3D11DeviceContext *ctx = com;
        c->type = 1 + (int)ctx->lpVtbl->GetType(ctx);
    }
    return c->type == 1 + D3D11_DEVICE_CONTEXT_DEFERRED;
}

static void clear(struct context *c)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        drop(c->vb[i]);
        c->vb[i] = NULL;
    }
    drop(c->ib);
    drop(c->layout);
    c->ib = c->layout = NULL;
    c->topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    memset(c->bounds, 0, sizeof(c->bounds));
}

void dirk_draw_free(struct dirk_proxy *p)
{
    if (p->draw && (p->iface < 0 || strncmp(dirk_ifaces[p->iface].name, "ID3D11DeviceContext", 19) == 0))
        clear(p->draw);
    free(p->draw);
    p->draw = NULL;
    if (p->resource) {
        struct dirk_resource *res = p->resource;
        if (res->pending_at)
            dirk_pending_remove(p);
        free(res->host);
        free(res->index);
    }
}

void dirk_p_ID3D11Device_CreateInputLayout(struct dirk_proxy *made, ID3D11Device *This,
        const D3D11_INPUT_ELEMENT_DESC *descs, UINT count, const void *code, SIZE_T code_size,
        ID3D11InputLayout **out)
{
    struct layout *l = calloc(1, sizeof(*l));
    uint32_t next[SLOTS] = { 0 };
    (void)This; (void)code; (void)code_size; (void)out;
    if (!l)
        return;
    for (UINT i = 0; descs && i < count; i++) {
        const D3D11_INPUT_ELEMENT_DESC *e = &descs[i];
        if (e->InputSlot >= SLOTS)
            continue;
        uint32_t at = e->AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? next[e->InputSlot] : e->AlignedByteOffset;
        uint32_t end = at + dirk_format_size(e->Format).block_bytes;
        next[e->InputSlot] = end;
        l->used |= 1u << e->InputSlot;
        if (end > l->end[e->InputSlot])
            l->end[e->InputSlot] = end;
        if (e->InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA) {
            l->instanced |= 1u << e->InputSlot;
            l->step[e->InputSlot] = e->InstanceDataStepRate;
        }
    }
    made->draw = l;
}

void dirk_h_ID3D11DeviceContext_IASetVertexBuffers(ID3D11DeviceContext *This, UINT start, UINT count,
        ID3D11Buffer *const *buffers, const UINT *strides, const UINT *offsets)
{
    struct context *c = context_of(This);
    if (!c)
        return;
    for (UINT i = 0; i < count && start + i < SLOTS; i++) {
        struct dirk_proxy *b = buffers ? dirk_proxy_of(buffers[i]) : NULL;
        hold(b);
        drop(c->vb[start + i]);
        c->vb[start + i] = b;
        c->stride[start + i] = strides ? strides[i] : 0;
        c->offset[start + i] = offsets ? offsets[i] : 0;
    }
}

void dirk_h_ID3D11DeviceContext_IASetIndexBuffer(ID3D11DeviceContext *This, ID3D11Buffer *buffer,
        DXGI_FORMAT format, UINT offset)
{
    struct context *c = context_of(This);
    if (!c)
        return;
    struct dirk_proxy *b = buffer ? dirk_proxy_of(buffer) : NULL;
    hold(b);
    drop(c->ib);
    c->ib = b;
    c->ib_format = format;
    c->ib_offset = offset;
}

void dirk_h_ID3D11DeviceContext_IASetInputLayout(ID3D11DeviceContext *This, ID3D11InputLayout *layout)
{
    struct context *c = context_of(This);
    if (!c)
        return;
    struct dirk_proxy *l = layout ? dirk_proxy_of(layout) : NULL;
    hold(l);
    drop(c->layout);
    c->layout = l;
}

void dirk_h_ID3D11DeviceContext_IASetPrimitiveTopology(ID3D11DeviceContext *This, D3D11_PRIMITIVE_TOPOLOGY topology)
{
    struct context *c = context_of(This);
    if (c)
        c->topology = topology;
}

void dirk_h_ID3D11DeviceContext_ClearState(ID3D11DeviceContext *This)
{
    struct context *c = context_of(This);
    if (c)
        clear(c);
}

/* ------------------------------------------------------ lazy buffers */

static int lazy_buffer(const D3D11_BUFFER_DESC *d)
{
    return !eager && d->Usage == D3D11_USAGE_DYNAMIC && (d->CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) &&
           d->BindFlags && !(d->BindFlags & ~(D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER));
}

void dirk_draw_created(struct dirk_proxy *p, const D3D11_SUBRESOURCE_DATA *data)
{
    struct dirk_resource *res = p->resource;
    if (res->kind != DIRK_RESOURCE_BUFFER)
        return;
    const D3D11_BUFFER_DESC *d = &res->desc.buffer;
    res->lazy = lazy_buffer(d);
    if (!data || !data->pSysMem)
        return;
    /* What the host was made with: a map of a dynamic buffer starts from it
     * (a no-overwrite map keeps the rest), and an index buffer's draws are
     * bounded by it. */
    if (d->Usage == D3D11_USAGE_DYNAMIC) {
        res->maps = calloc(1, sizeof(*res->maps));
        if (res->maps && (res->maps[0].data = malloc(d->ByteWidth))) {
            res->sub_count = 1;
            res->maps[0].size = d->ByteWidth;
            memcpy(res->maps[0].data, data->pSysMem, d->ByteWidth);
        }
        if (res->lazy && (res->host = malloc(d->ByteWidth))) {
            memcpy(res->host, data->pSysMem, d->ByteWidth);
            res->host_valid = 1;
        }
    } else if ((d->BindFlags & D3D11_BIND_INDEX_BUFFER) && (res->index = malloc(d->ByteWidth))) {
        memcpy(res->index, data->pSysMem, d->ByteWidth);
        res->index_valid = 1;
    }
}

void dirk_draw_written(struct dirk_proxy *p, const D3D11_BOX *box, const void *data)
{
    struct dirk_resource *res = p ? p->resource : NULL;
    if (!res || res->kind != DIRK_RESOURCE_BUFFER)
        return;
    res->writes++;
    if (res->lazy)
        res->host_valid = 0;
    if (!res->index)
        return;
    uint32_t size = res->desc.buffer.ByteWidth;
    uint32_t lo = box ? box->left : 0, hi = box ? box->right : size;
    if (data && lo < hi && hi <= size)
        memcpy(res->index + lo, data, hi - lo);
    else
        res->index_valid = 0;
}

/* The lazy buffers some Unmap left unsent, for what reads them all. */
static struct dirk_proxy **pending;
static unsigned pending_count, pending_cap;

static void pending_add(struct dirk_proxy *p)
{
    if (p->resource->pending_at)
        return;
    dirk_lock();
    if (pending_count == pending_cap) {
        unsigned cap = pending_cap ? pending_cap * 2 : 64;
        struct dirk_proxy **n = realloc(pending, cap * sizeof(*n));
        if (!n) {
            dirk_unlock();
            return;
        }
        pending = n;
        pending_cap = cap;
    }
    pending[pending_count++] = p;
    p->resource->pending_at = pending_count;
    dirk_unlock();
}

void dirk_pending_remove(struct dirk_proxy *p)
{
    dirk_lock();
    unsigned at = p->resource->pending_at;
    if (at && at <= pending_count && pending[at - 1] == p) {
        pending[at - 1] = pending[--pending_count];
        if (at - 1 < pending_count)
            pending[at - 1]->resource->pending_at = at;
    }
    p->resource->pending_at = 0;
    dirk_unlock();
}

static void put_range(void *ctx, uint32_t op, struct dirk_proxy *buf, uint64_t at, const void *bytes, uint64_t n)
{
    struct dirk_proxy *self = dirk_proxy_of(ctx);
    uint8_t *w = dirk_begin(self, op, DIRK_REC_HEADER + 4 + 8 + 8 + n, 0);
    if (!w)
        return;
    w = dirk_put_u32(w, buf->handle);
    w = dirk_put_u64(w, at);
    w = dirk_put_blob(w, bytes, n);
    dirk_end(self, w);
}

/* The first byte at or after lo, and one past the last before hi, where a
 * and b differ; lo == hi when they do not. */
static void differ(const uint8_t *a, const uint8_t *b, uint64_t *plo, uint64_t *phi)
{
    uint64_t lo = *plo, hi = *phi;
    while (hi - lo >= 64 && !memcmp(a + lo, b + lo, 64))
        lo += 64;
    while (lo < hi && a[lo] == b[lo])
        lo++;
    while (hi - lo >= 64 && !memcmp(a + hi - 64, b + hi - 64, 64))
        hi -= 64;
    while (hi > lo && a[hi - 1] == b[hi - 1])
        hi--;
    *plo = lo;
    *phi = hi;
}

/* Brings bytes [lo, hi) of a lazy buffer up to date on the host. */
static void sync_range(void *ctx, struct dirk_proxy *buf, uint64_t lo, uint64_t hi)
{
    struct dirk_resource *res = buf ? buf->resource : NULL;
    if (!res || !res->lazy || res->gen == res->synced_gen)
        return;
    uint64_t size = res->desc.buffer.ByteWidth;
    struct dirk_mapping *m = res->maps ? &res->maps[0] : NULL;
    if (!m || !m->data || m->size < size)
        return;
    if (hi > size)
        hi = size;
    if (lo >= hi)
        return;
    for (unsigned i = 0; i < 4; i++)
        if (res->seen[i].gen == res->gen && res->seen[i].lo <= lo && hi <= res->seen[i].hi)
            return;
    if (!res->host && !(res->host = malloc(size)))
        return;
    if (!res->host_valid) {
        lo = 0;
        hi = size;
    }
    uint64_t a = lo, b = hi;
    if (res->host_valid)
        differ(m->data, res->host, &a, &b);
    if (a < b) {
        static int said;
        if (dirk_trace_on && !said) {
            said = 1;
            dirk_trace("first draw range: %llu bytes at %llu of %llu", (unsigned long long)(b - a),
                       (unsigned long long)a, (unsigned long long)size);
        }
        put_range(ctx, DIRK_OP_WRITE_RANGE, buf, a, m->data + a, b - a);
        memcpy(res->host + a, m->data + a, b - a);
    }
    if (!res->host_valid) {
        res->host_valid = 1;
        st.whole++;
        st.whole_bytes += b - a;
    } else {
        st.ranges++;
        st.range_bytes += b - a;
    }
    if (verify)
        put_range(ctx, DIRK_OP_VERIFY, buf, lo, m->data + lo, hi - lo);
    if (lo == 0 && hi == size) {
        res->synced_gen = res->gen;
        if (res->pending_at)
            dirk_pending_remove(buf);
        return;
    }
    res->seen[res->seen_next].lo = lo;
    res->seen[res->seen_next].hi = hi;
    res->seen[res->seen_next].gen = res->gen;
    res->seen_next = (res->seen_next + 1) % 4;
}

static void sync_whole(void *ctx, struct dirk_proxy *buf)
{
    sync_range(ctx, buf, 0, ~(uint64_t)0);
}

/* Every lazy buffer some Unmap left unsent. */
static void sync_pending(void *ctx)
{
    struct dirk_proxy **list;
    unsigned n;
    dirk_lock();
    n = pending_count;
    list = n ? malloc(n * sizeof(*list)) : NULL;
    for (unsigned i = 0; list && i < n; i++) {
        list[i] = pending[i];
        hold(list[i]);
    }
    dirk_unlock();
    for (unsigned i = 0; list && i < n; i++) {
        sync_whole(ctx, list[i]);
        drop(list[i]);
    }
    free(list);
}

int dirk_draw_unmap(void *ctx, struct dirk_proxy *buf, D3D11_MAP type)
{
    struct dirk_resource *res = buf->resource;
    struct context *c = context_of(ctx);
    int lazy = res->lazy && c && !deferred(ctx, c);
    if (res->lazy || res->index)
        res->writes++;
    if (res->lazy) {
        res->gen++;
        if (lazy && type == D3D11_MAP_WRITE_NO_OVERWRITE && res->host_valid) {
            static int said;
            if (dirk_trace_on && !said) {
                said = 1;
                dirk_trace("first no-overwrite Unmap left for the draws");
            }
            st.unmaps++;
            st.lazy_unmaps++;
            pending_add(buf);
            return 1;
        }
        /* Sent whole: the host holds what the guest does, once a deferred
         * context's list has run, which nothing here can know. */
        struct dirk_mapping *m = res->maps ? &res->maps[0] : NULL;
        if (lazy && m && m->data && (res->host || (res->host = malloc(m->size)))) {
            memcpy(res->host, m->data, m->size);
            res->host_valid = 1;
            res->synced_gen = res->gen;
            if (res->pending_at)
                dirk_pending_remove(buf);
        } else {
            res->host_valid = 0;
        }
    }
    struct dirk_mapping *m = res->maps ? &res->maps[0] : NULL;
    st.unmaps++;
    st.unmap_bytes += m ? m->size : 0;
    return 0;
}

/* ----------------------------------------------------------- draws */

static const uint8_t *index_bytes(struct dirk_resource *res)
{
    if (res->desc.buffer.Usage == D3D11_USAGE_DYNAMIC)
        return res->maps && res->maps[0].data && res->maps[0].size >= res->desc.buffer.ByteWidth
               ? res->maps[0].data : NULL;
    return res->index_valid ? res->index : NULL;
}

static int strip(D3D11_PRIMITIVE_TOPOLOGY t)
{
    return t == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP || t == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
           t == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ || t == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
}

/* The smallest and largest index of count from start, or 0 if not known. */
static int index_bounds(struct context *c, uint32_t start, uint32_t count, uint32_t *pmin, uint32_t *pmax)
{
    struct dirk_resource *res = c->ib ? c->ib->resource : NULL;
    if (!res || res->kind != DIRK_RESOURCE_BUFFER)
        return 0;
    uint32_t isz = c->ib_format == DXGI_FORMAT_R16_UINT ? 2 : c->ib_format == DXGI_FORMAT_R32_UINT ? 4 : 0;
    if (!isz)
        return 0;
    uint64_t at = c->ib_offset + (uint64_t)start * isz, size = res->desc.buffer.ByteWidth;
    if (at >= size)
        return 0;
    if (at + (uint64_t)count * isz > size)
        count = (uint32_t)((size - at) / isz);
    if (!count)
        return 0;
    int restart = strip(c->topology);
    for (unsigned i = 0; i < 4; i++) {
        if (c->bounds[i].ib == c->ib && c->bounds[i].writes == res->writes && c->bounds[i].gen == res->gen &&
            c->bounds[i].at == at && c->bounds[i].count == count && c->bounds[i].format == isz &&
            c->bounds[i].restart == restart) {
            *pmin = c->bounds[i].min;
            *pmax = c->bounds[i].max;
            return 1;
        }
    }
    const uint8_t *bytes = index_bytes(res);
    if (!bytes)
        return 0;
    uint32_t lo = ~0u, hi = 0;
    if (isz == 2) {
        const uint8_t *p = bytes + at;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t v;
            memcpy(&v, p + 2 * i, 2);
            if (restart && v == 0xffff)
                continue;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    } else {
        const uint8_t *p = bytes + at;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v;
            memcpy(&v, p + 4 * i, 4);
            if (restart && v == 0xffffffffu)
                continue;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    if (lo > hi)
        return 0;
    unsigned k = c->bounds_next;
    c->bounds_next = (k + 1) % 4;
    c->bounds[k].ib = c->ib;
    c->bounds[k].writes = res->writes;
    c->bounds[k].gen = res->gen;
    c->bounds[k].at = (uint32_t)at;
    c->bounds[k].count = count;
    c->bounds[k].format = isz;
    c->bounds[k].restart = restart;
    c->bounds[k].min = lo;
    c->bounds[k].max = hi;
    *pmin = lo;
    *pmax = hi;
    return 1;
}

/* Elements first..last of a slot, as bytes. */
static void sync_slot(void *ctx, struct context *c, const struct layout *l, unsigned slot, int64_t first, int64_t last)
{
    if (last < first)
        return;
    if (first < 0)
        first = 0;
    uint64_t stride = c->stride[slot], end = l && l->end[slot] ? l->end[slot] : stride;
    uint64_t lo = c->offset[slot] + stride * (uint64_t)first;
    uint64_t hi = c->offset[slot] + stride * (uint64_t)last + end;
    sync_range(ctx, c->vb[slot], lo, hi);
}

/* What a draw reads: vertices first..last (unknown: whole buffers), and
 * instances from start_instance. */
static void sync_draw(void *ctx, int known, int64_t first, int64_t last, uint32_t instances, uint32_t start_instance)
{
    struct context *c = context_of(ctx);
    if (!c || deferred(ctx, c))
        return;
    const struct layout *l = c->layout ? c->layout->draw : NULL;
    for (unsigned slot = 0; slot < SLOTS; slot++) {
        struct dirk_proxy *b = c->vb[slot];
        if (!b || !b->resource || !b->resource->lazy || b->resource->gen == b->resource->synced_gen)
            continue;
        if (l && !(l->used & (1u << slot)))
            continue;
        if (!known || !l) {
            sync_whole(ctx, b);
        } else if (l->instanced & (1u << slot)) {
            if (!instances)
                continue;
            uint32_t step = l->step[slot];
            uint64_t n = step ? (instances - 1) / step : 0;
            sync_slot(ctx, c, l, slot, start_instance, (int64_t)start_instance + (int64_t)n);
        } else {
            sync_slot(ctx, c, l, slot, first, last);
        }
    }
}

static void draw_indexed(void *ctx, uint32_t count, uint32_t start, int32_t base, uint32_t instances, uint32_t start_instance)
{
    struct context *c = context_of(ctx);
    if (!c || deferred(ctx, c) || !count)
        return;
    uint32_t isz = c->ib_format == DXGI_FORMAT_R16_UINT ? 2 : 4, lo, hi;
    if (c->ib)
        sync_range(ctx, c->ib, c->ib_offset + (uint64_t)start * isz, c->ib_offset + (uint64_t)(start + (uint64_t)count) * isz);
    int known = index_bounds(c, start, count, &lo, &hi);
    sync_draw(ctx, known, known ? (int64_t)base + lo : 0, known ? (int64_t)base + hi : 0, instances, start_instance);
}

void dirk_h_ID3D11DeviceContext_Draw(ID3D11DeviceContext *This, UINT count, UINT start)
{
    if (count)
        sync_draw(This, 1, start, (int64_t)start + count - 1, 1, 0);
}

void dirk_h_ID3D11DeviceContext_DrawInstanced(ID3D11DeviceContext *This, UINT count, UINT instances,
        UINT start, UINT start_instance)
{
    if (count && instances)
        sync_draw(This, 1, start, (int64_t)start + count - 1, instances, start_instance);
}

void dirk_h_ID3D11DeviceContext_DrawIndexed(ID3D11DeviceContext *This, UINT count, UINT start, INT base)
{
    draw_indexed(This, count, start, base, 1, 0);
}

void dirk_h_ID3D11DeviceContext_DrawIndexedInstanced(ID3D11DeviceContext *This, UINT count, UINT instances,
        UINT start, INT base, UINT start_instance)
{
    if (instances)
        draw_indexed(This, count, start, base, instances, start_instance);
}

static void draw_unbounded(void *ctx)
{
    struct context *c = context_of(ctx);
    if (!c || deferred(ctx, c))
        return;
    sync_whole(ctx, c->ib);
    sync_draw(ctx, 0, 0, 0, 0, 0);
}

void dirk_h_ID3D11DeviceContext_DrawAuto(ID3D11DeviceContext *This)
{
    draw_unbounded(This);
}

void dirk_h_ID3D11DeviceContext_DrawInstancedIndirect(ID3D11DeviceContext *This, ID3D11Buffer *args, UINT at)
{
    (void)args; (void)at;
    draw_unbounded(This);
}

void dirk_h_ID3D11DeviceContext_DrawIndexedInstancedIndirect(ID3D11DeviceContext *This, ID3D11Buffer *args, UINT at)
{
    (void)args; (void)at;
    draw_unbounded(This);
}

/* ------------------------------------------------- copies, command lists */

static void copy(void *ctx, ID3D11Resource *dst, ID3D11Resource *src)
{
    struct context *c = context_of(ctx);
    struct dirk_proxy *d = dst ? dirk_proxy_of(dst) : NULL;
    if (c && src && !deferred(ctx, c))
        sync_whole(ctx, dirk_proxy_of(src));
    /* The GPU writes it: what the guest holds of it is no longer so. */
    if (d && d->resource && d->resource->kind == DIRK_RESOURCE_BUFFER) {
        d->resource->writes++;
        d->resource->host_valid = 0;
        d->resource->index_valid = 0;
    }
}

void dirk_h_ID3D11DeviceContext_CopyResource(ID3D11DeviceContext *This, ID3D11Resource *dst, ID3D11Resource *src)
{
    copy(This, dst, src);
}

void dirk_h_ID3D11DeviceContext_CopySubresourceRegion(ID3D11DeviceContext *This, ID3D11Resource *dst, UINT dst_sub,
        UINT x, UINT y, UINT z, ID3D11Resource *src, UINT src_sub, const D3D11_BOX *box)
{
    (void)dst_sub; (void)x; (void)y; (void)z; (void)src_sub; (void)box;
    copy(This, dst, src);
}

void dirk_h_ID3D11DeviceContext1_CopySubresourceRegion1(ID3D11DeviceContext1 *This, ID3D11Resource *dst, UINT dst_sub,
        UINT x, UINT y, UINT z, ID3D11Resource *src, UINT src_sub, const D3D11_BOX *box, UINT flags)
{
    (void)dst_sub; (void)x; (void)y; (void)z; (void)src_sub; (void)box; (void)flags;
    copy(This, dst, src);
}

void dirk_h_ID3D11DeviceContext_CopyStructureCount(ID3D11DeviceContext *This, ID3D11Buffer *dst, UINT at,
        ID3D11UnorderedAccessView *view)
{
    (void)at; (void)view;
    copy(This, (ID3D11Resource *)dst, NULL);
}

void dirk_h_ID3D11DeviceContext_FinishCommandList(ID3D11DeviceContext *This, BOOL restore, ID3D11CommandList **out)
{
    struct context *c = context_of(This);
    (void)out;
    if (c && !restore)
        clear(c);
}

void dirk_h_ID3D11DeviceContext_ExecuteCommandList(ID3D11DeviceContext *This, ID3D11CommandList *list, BOOL restore)
{
    struct context *c = context_of(This);
    (void)list;
    if (!c || deferred(This, c))
        return;
    /* A list's draws were bounded by nothing here. */
    sync_pending(This);
    if (!restore)
        clear(c);
}

void dirk_h_IDXGISwapChain_Present(IDXGISwapChain *This, UINT interval, UINT flags)
{
    (void)This; (void)interval; (void)flags;
    stats_frame();
}

void dirk_h_IDXGISwapChain1_Present1(IDXGISwapChain1 *This, UINT interval, UINT flags,
        const DXGI_PRESENT_PARAMETERS *params)
{
    (void)This; (void)interval; (void)flags; (void)params;
    stats_frame();
}
