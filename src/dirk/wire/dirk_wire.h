/*
 * dirk's wire: how a D3D program's calls reach the host's D3D device.
 *
 * The program (the guest) calls proxies of the D3D11 and DXGI interfaces.
 * Each call becomes a record in a stream the guest fills; the host reads the
 * stream in order and makes the same call on the real object. A call whose
 * caller needs an answer (an out parameter, a result it can act on) waits:
 * the guest sends what it holds, and the host answers the last record.
 *
 * Objects are named by handles. The guest picks the handle of an object it
 * creates, so a create need not wait; handles the guest picks count up from
 * 1. An object the host hands out that the guest has no handle for (a swap
 * chain's buffer, a view's resource) gets a handle the host picks, from
 * DIRK_HOST_HANDLE up. 0 is NULL.
 *
 * All values are little-endian and unaligned. A record:
 *
 *     u32 size     the record's bytes, header included
 *     u16 op       enum dirk_op: below DIRK_OP_GENERATED the hand-written
 *                  ones, from it the generated ones (dirk_wire_ops.h)
 *     u16 flags    DIRK_REC_WAIT: the guest waits for the answer
 *     u32 handle   the object called
 *
 * then the parameters in order: plain values in their width on the wire
 * (pointer-sized ones in 8 bytes), handles in 4, arrays as a u32 count
 * (~0 for a NULL pointer) and the elements, blobs as a u64 size (~0 for
 * NULL) and the bytes, strings as a u32 length in characters (~0 for NULL)
 * and the characters, structs field by field. An out parameter sends only
 * its count, or ~0 if it is NULL.
 *
 * An answer: u32 status (DIRK_REPLY_*), then the method's result in 8
 * bytes, then each out parameter in order.
 */
#ifndef DIRK_WIRE_H
#define DIRK_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dirk_wire_ops.h"

#define DIRK_REC_HEADER 12
#define DIRK_REC_WAIT 1
#define DIRK_HOST_HANDLE 0x80000000u

enum dirk_reply_status {
    DIRK_REPLY_OK = 0,
    /* The device is lost, or the call named an object that is not there. */
    DIRK_REPLY_REMOVED = 1,
};

enum dirk_special_op {
    /* handle: the object to forget. The host drops its reference. */
    DIRK_OP_RELEASE = 1,
    /* riid; answers the result and the handle of the interface. */
    DIRK_OP_QUERY_INTERFACE,
    /* u32 new handle: a second handle for the object called. */
    DIRK_OP_DUPLICATE,
    /* D3D11CreateDevice's parameters, the device's and the immediate
     * context's new handles; answers the result and the feature level. */
    DIRK_OP_CREATE_DEVICE,
    /* riid, u32 flags, u32 new handle; answers the result. */
    DIRK_OP_CREATE_FACTORY,
    /* u32 kind (enum dirk_resource_kind), the desc, u32 new handle, then
     * the initial data: u32 count (~0 for none), and for each subresource
     * u32 row pitch, u32 slice pitch, the bytes as a blob. */
    DIRK_OP_CREATE_RESOURCE,
    /* handle: the context. u32 resource, u32 subresource, u32 map type,
     * u32 map flags; answers the result, u32 row pitch, u32 depth pitch and
     * the subresource's bytes as a blob (none for a discard). */
    DIRK_OP_MAP,
    /* handle: the context. u32 resource, u32 subresource, u32 map type,
     * u32 row pitch, u32 depth pitch, the bytes as a blob: what a map the
     * guest made wrote. */
    DIRK_OP_UNMAP,
    /* handle: the context. u32 resource, u32 subresource, u32 has box, the
     * box (6 u32), u32 row pitch, u32 depth pitch, u32 copy flags, the
     * bytes as a blob. */
    DIRK_OP_UPDATE_SUBRESOURCE,
    /* u64 window, u32 kind, u32 id, u32 width, u32 height: the window the
     * guest makes swap chains for is shown by that surface (kind 0 a
     * Wayland surface, as the GL device's SURFACE says), and is that big. */
    DIRK_OP_SURFACE,
    /* handle: the immediate context. u32 buffer, u64 offset, the bytes as a
     * blob: bytes a no-overwrite map wrote, sent when a draw reads them
     * rather than at its Unmap. */
    DIRK_OP_WRITE_RANGE,
    /* handle: the immediate context. u32 buffer, u64 offset, the bytes as a
     * blob: what the guest says that range holds (DIRK_VERIFY). */
    DIRK_OP_VERIFY,
};

enum dirk_resource_kind {
    DIRK_RESOURCE_BUFFER,
    DIRK_RESOURCE_TEXTURE1D,
    DIRK_RESOURCE_TEXTURE2D,
    DIRK_RESOURCE_TEXTURE3D,
    DIRK_RESOURCE_TEXTURE2D1,
    DIRK_RESOURCE_TEXTURE3D1,
};

static inline uint8_t *dirk_put_u32(uint8_t *w, uint32_t v) { memcpy(w, &v, 4); return w + 4; }
static inline uint8_t *dirk_put_u64(uint8_t *w, uint64_t v) { memcpy(w, &v, 8); return w + 8; }

/* A value of `size` bytes in `width` wire bytes: cut or zero-extended. */
static inline uint8_t *dirk_put_val(uint8_t *w, const void *v, size_t size, size_t width)
{
    if (size >= width) {
        memcpy(w, v, width);
    } else {
        memcpy(w, v, size);
        memset(w + size, 0, width - size);
    }
    return w + width;
}

static inline const uint8_t *dirk_get_val(const uint8_t *r, void *v, size_t size, size_t width)
{
    if (size <= width) {
        memcpy(v, r, size);
    } else {
        memcpy(v, r, width);
        memset((uint8_t *)v + width, 0, size - width);
    }
    return r + width;
}

#endif
