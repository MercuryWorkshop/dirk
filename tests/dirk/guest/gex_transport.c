/*
 * dirk's wire over gex's GL device: /dev/gex-d3d, the GL device's ring with
 * the D3D magic. A record stream is copied into the ring's command area and
 * submitted; a waiting one's answer is the ring's reply area.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gex_transport.h"

#define D3D_MAGIC 0x44334447u /* "GD3D" */
#define D3D_PROTOCOL_VERSION 1u
#define HEADER_BYTES 32u
#define CMD_CAPACITY (32u * 1024 * 1024)
#define REPLY_CAPACITY (32u * 1024 * 1024)
#define GEX_GL_SUBMIT 0xc0086701u
#define GEX_GL_SUBMIT_ASYNC 0xc0086704u

struct header {
    uint32_t magic, capacity, tail, reply_capacity, reply_len, status, failed_at, protocol_version;
};

static int fd = -1;
static struct header *ring;

static const uint8_t *send_ring(void *ctx, const uint8_t *records, size_t len, int wait)
{
    uint8_t *cmds;
    (void)ctx;
    if (!ring || len > CMD_CAPACITY)
        return NULL;
    cmds = (uint8_t *)ring + HEADER_BYTES;
    memcpy(cmds, records, len);
    ring->tail = (uint32_t)len;
    ring->reply_len = 0;
    if (ioctl(fd, wait ? GEX_GL_SUBMIT : GEX_GL_SUBMIT_ASYNC, ring) < 0) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "dirk: the D3D device refused a stream (status %u): %s\n",
                    ring->status, strerror(errno));
        }
        return NULL;
    }
    if (!wait)
        return cmds;
    if (ring->reply_len < 4)
        return NULL;
    return cmds + CMD_CAPACITY;
}

int dirk_gex_transport(struct dirk_transport *out)
{
    size_t total = HEADER_BYTES + CMD_CAPACITY + REPLY_CAPACITY;
    void *p = MAP_FAILED;
    fd = open("/dev/gex-d3d", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "dirk: open(/dev/gex-d3d): %s\n", strerror(errno));
        return -1;
    }
#if defined(__x86_64__) && defined(MAP_32BIT)
    p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#endif
    if (p == MAP_FAILED)
        p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        close(fd);
        fd = -1;
        return -1;
    }
    ring = p;
    ring->magic = D3D_MAGIC;
    ring->capacity = CMD_CAPACITY;
    ring->reply_capacity = REPLY_CAPACITY;
    ring->protocol_version = D3D_PROTOCOL_VERSION;
    out->send = send_ring;
    out->ctx = NULL;
    out->window = NULL;
    return 0;
}
