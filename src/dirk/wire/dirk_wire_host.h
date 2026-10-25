/*
 * dirk's host side of the wire (dirk_wire.h): the stream of one guest
 * process, read in order and called on DXVK's D3D11 and DXGI objects.
 *
 * A client is one guest process. It holds a reference to every object the
 * guest has a handle for; destroying it drops them all, as the process's
 * exit would. One thread at a time calls a client.
 */
#ifndef DIRK_WIRE_HOST_H
#define DIRK_WIRE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dirk_wire_client;

struct dirk_wire_host_ops {
    /* The guest says which surface shows a window, and its size
     * (DIRK_OP_SURFACE). */
    void (*surface)(void *user, uint64_t window, uint32_t kind, uint32_t id,
                    uint32_t width, uint32_t height);
    void *user;
};

struct dirk_wire_client *dirk_wire_client_create(const struct dirk_wire_host_ops *ops);

/* Reads len bytes of records. If the last one waits, *reply and *reply_len
 * are its answer, valid until the next call; else *reply_len is 0. False if
 * the records are malformed: the guest is not to be trusted again. */
bool dirk_wire_client_submit(struct dirk_wire_client *client, const uint8_t *records, size_t len,
                             const uint8_t **reply, size_t *reply_len);

void dirk_wire_client_destroy(struct dirk_wire_client *client);

#ifdef __cplusplus
}
#endif

#endif
