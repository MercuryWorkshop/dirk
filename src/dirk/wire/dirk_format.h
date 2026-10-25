/*
 * The sizes of DXGI formats, for the bytes a texture's data takes: the
 * block's width and height in pixels (1 for a plain format) and its bytes.
 */
#ifndef DIRK_FORMAT_H
#define DIRK_FORMAT_H

#include <stdint.h>

struct dirk_format_size {
    uint32_t block_bytes, block_w, block_h;
};

static inline struct dirk_format_size dirk_format_size(uint32_t f)
{
    struct dirk_format_size s = { 0, 1, 1 };
    if (f >= 1 && f <= 4) s.block_bytes = 16;         /* R32G32B32A32 */
    else if (f >= 5 && f <= 8) s.block_bytes = 12;    /* R32G32B32 */
    else if (f >= 9 && f <= 22) s.block_bytes = 8;    /* R16G16B16A16, R32G32, R32G8X24 */
    else if (f >= 23 && f <= 47) s.block_bytes = 4;   /* R10G10B10A2 .. R24G8, R8G8B8A8, R16G16, R32 */
    else if (f >= 48 && f <= 59) s.block_bytes = 2;   /* R8G8, R16 */
    else if (f >= 60 && f <= 65) s.block_bytes = 1;   /* R8, A8 */
    else if (f == 66) { s.block_bytes = 1; s.block_w = 8; }  /* R1 */
    else if (f >= 67 && f <= 69) s.block_bytes = 4;   /* R9G9B9E5, R8G8_B8G8, G8R8_G8B8 */
    else if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) {   /* BC1, BC4 */
        s.block_bytes = 8; s.block_w = s.block_h = 4;
    } else if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) { /* BC2, BC3, BC5, BC6H, BC7 */
        s.block_bytes = 16; s.block_w = s.block_h = 4;
    }
    else if (f >= 85 && f <= 86) s.block_bytes = 2;   /* B5G6R5, B5G5R5A1 */
    else if (f >= 87 && f <= 93) s.block_bytes = 4;   /* B8G8R8A8 .. B8G8R8X8_UNORM_SRGB */
    else if (f == 115) s.block_bytes = 2;             /* B4G4R4A4 */
    else s.block_bytes = 4;                           /* video formats: not carried */
    return s;
}

/* The bytes of a box of w x h x d pixels laid out with these pitches. */
static inline uint64_t dirk_data_size(uint32_t format, uint32_t w, uint32_t h, uint32_t d,
                                      uint32_t row_pitch, uint32_t depth_pitch)
{
    struct dirk_format_size s = dirk_format_size(format);
    uint64_t rows = (h + s.block_h - 1) / s.block_h;
    uint64_t row = (uint64_t)((w + s.block_w - 1) / s.block_w) * s.block_bytes;
    if (!w || !h || !d)
        return 0;
    return (uint64_t)(d - 1) * depth_pitch + (rows - 1) * row_pitch + row;
}

static inline uint32_t dirk_mip_count(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t m = w > h ? w : h, n = 1;
    if (d > m)
        m = d;
    while (m > 1) {
        m >>= 1;
        n++;
    }
    return n;
}

static inline uint32_t dirk_mip(uint32_t size, uint32_t level)
{
    size >>= level;
    return size ? size : 1;
}

#endif
