/*
 * dirk's Windows shim: the calls d3d11.dll makes to its Unix side
 * (d3d11.so), which holds the D3D device (/dev/gex-d3d) and its ring.
 */
#ifndef DIRK_PE_UNIXLIB_H
#define DIRK_PE_UNIXLIB_H

#include "wine/unixlib.h"

/* Opens the device and maps its ring, once: the ring's address (the PE side
 * writes records at ring + DIRK_RING_COMMANDS). */
struct dirk_unix_init_params {
    UINT64 ring;
    UINT32 capacity;
    UINT32 reply_capacity;
};

/* Submits len bytes of records; wait: the last record waits, and its answer
 * is at ring + DIRK_RING_COMMANDS + capacity, reply_len bytes. */
struct dirk_unix_submit_params {
    UINT32 len;
    UINT32 wait;
    UINT32 reply_len;
    UINT32 status;
};

/* The surface showing a window (0: none, its frames are not shown) and the
 * window's client size. */
struct dirk_unix_surface_params {
    UINT64 hwnd;
    UINT32 id;
    UINT32 width;
    UINT32 height;
};

enum dirk_unix_funcs {
    dirk_unix_init,
    dirk_unix_submit,
    dirk_unix_surface,
    dirk_unix_funcs_count,
};

#define DIRK_RING_COMMANDS 32

#endif
