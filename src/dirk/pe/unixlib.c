/*
 * dirk's Windows shim, the Unix side (d3d11.so): gex's D3D device
 * (/dev/gex-d3d), the GL device's ring with the D3D magic. The PE side
 * writes records into the ring; this submits them, and names the surface a
 * window is shown by.
 */
#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntuser.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

#define D3D_MAGIC 0x44334447u /* "GD3D" */
#define D3D_PROTOCOL_VERSION 1u
#define CMD_CAPACITY (32u * 1024 * 1024)
#define REPLY_CAPACITY (32u * 1024 * 1024)
#define GEX_GL_SUBMIT 0xc0086701u
#define GEX_GL_SUBMIT_ASYNC 0xc0086704u

struct header {
    uint32_t magic, capacity, tail, reply_capacity, reply_len, status, failed_at, protocol_version;
};

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static int fd = -1;
static struct header *ring;

/* winewayland.drv's: the wl_surface id of the client surface showing a
 * window, made on first use (0 if it has none). Weak: without the display
 * driver's entry, frames are drawn and not shown. */
extern UINT32 wayland_gex_client_surface(HWND hwnd) __attribute__((weak));

static NTSTATUS dirk_init(void *args)
{
    struct dirk_unix_init_params *params = args;
    NTSTATUS status = STATUS_SUCCESS;
    pthread_mutex_lock(&init_lock);
    if (!ring) {
        size_t total = DIRK_RING_COMMANDS + CMD_CAPACITY + REPLY_CAPACITY;
        void *p;
        fd = open("/dev/gex-d3d", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            ERR("dirk: open(/dev/gex-d3d): %s\n", strerror(errno));
            status = STATUS_DEVICE_NOT_CONNECTED;
            goto done;
        }
        p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED || (uintptr_t)p + total > 0xffffffffu) {
            ERR("dirk: no ring the PE side can reach\n");
            if (p != MAP_FAILED)
                munmap(p, total);
            close(fd);
            fd = -1;
            status = STATUS_NO_MEMORY;
            goto done;
        }
        ring = p;
        ring->magic = D3D_MAGIC;
        ring->capacity = CMD_CAPACITY;
        ring->reply_capacity = REPLY_CAPACITY;
        ring->protocol_version = D3D_PROTOCOL_VERSION;
    }
    params->ring = (UINT64)(uintptr_t)ring;
    params->capacity = CMD_CAPACITY;
    params->reply_capacity = REPLY_CAPACITY;
done:
    pthread_mutex_unlock(&init_lock);
    return status;
}

/* The PE side holds the stream's lock across a submit and its answer. */
static NTSTATUS dirk_submit(void *args)
{
    struct dirk_unix_submit_params *params = args;
    if (!ring || params->len > CMD_CAPACITY)
        return STATUS_INVALID_PARAMETER;
    ring->tail = params->len;
    ring->reply_len = 0;
    if (ioctl(fd, params->wait ? GEX_GL_SUBMIT : GEX_GL_SUBMIT_ASYNC, ring) < 0) {
        static int said;
        params->status = ring->status;
        if (!said) {
            said = 1;
            ERR("dirk: the D3D device refused a stream (status %u): %s\n", ring->status, strerror(errno));
        }
        return STATUS_DEVICE_NOT_READY;
    }
    params->reply_len = params->wait ? ring->reply_len : 0;
    return STATUS_SUCCESS;
}

static NTSTATUS dirk_surface(void *args)
{
    struct dirk_unix_surface_params *params = args;
    HWND hwnd = (HWND)(ULONG_PTR)params->hwnd;
    RECT rect = {0};
    if (!NtUserGetClientRect(hwnd, &rect, NtUserGetDpiForWindow(hwnd)))
        return STATUS_INVALID_HANDLE;
    params->width = rect.right - rect.left;
    params->height = rect.bottom - rect.top;
    params->id = wayland_gex_client_surface ? wayland_gex_client_surface(hwnd) : 0;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] = {
    dirk_init,
    dirk_submit,
    dirk_surface,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == dirk_unix_funcs_count);
