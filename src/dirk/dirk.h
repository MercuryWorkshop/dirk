/*
 * dirk: DXVK's D3D11/DXGI front end on a gallium screen.
 *
 * The host (the process that owns the WebGPU device and its gallium screen)
 * calls dirk_init once, then the D3D11 and DXGI entry points as a Windows
 * program would. A window is whatever the host says it is: an opaque handle
 * the callbacks below are asked about.
 */
#ifndef DIRK_H
#define DIRK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_screen;
struct pipe_resource;

struct dirk_host {
   /* The size of a window's client area. False if it is no window. */
   bool (*window_size)(void *window, uint32_t *width, uint32_t *height);

   /* The size and refresh rate (millihertz) of the display. */
   void (*display_mode)(uint32_t *width, uint32_t *height, uint32_t *refresh_mhz);

   /* The gallium format a window's surface wants (enum pipe_format), or 0
    * (PIPE_FORMAT_NONE) for the back buffer's own. */
   uint32_t (*surface_format)(void *window);

   /* Shows a frame: the context has been flushed, and image (of the
    * surface's format) holds the frame until the next present to this
    * window. */
   void (*present)(void *window, struct pipe_resource *image,
                   uint32_t width, uint32_t height, uint32_t sync_interval);
};

/* Before any D3D call. The screen outlives every device made on it. */
void dirk_init(struct pipe_screen *screen, const struct dirk_host *host);

const struct dirk_host *dirk_get_host(void);

/* The Windows path of the program being served, for DXVK's per-application
 * settings. Copied; before the first device. */
void dirk_set_exe_path(const char *path);

const char *dirk_exe_path(void);

#ifdef __cplusplus
}
#endif

#endif
