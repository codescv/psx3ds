#include "psx3ds/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PsxResult video_ppm_write_rgb24_raw(const char *path, const u8 *rgb24,
                                    u16 w, u16 h, u32 pitch_bytes) {
    if (!path || !rgb24 || w == 0u || h == 0u) {
        return PSX_ERR_BAD_ARG;
    }
    u32 row_bytes = (u32)w * 3u;
    if (pitch_bytes == 0u) {
        pitch_bytes = row_bytes;
    }
    if (pitch_bytes < row_bytes) {
        return PSX_ERR_BAD_ARG;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return PSX_ERR_IO;
    }
    if (fprintf(fp, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h) < 0) {
        fclose(fp);
        return PSX_ERR_IO;
    }
    for (u16 y = 0u; y < h; y++) {
        const u8 *src_row = rgb24 + (size_t)y * (size_t)pitch_bytes;
        if (fwrite(src_row, 1u, (size_t)row_bytes, fp) != (size_t)row_bytes) {
            fclose(fp);
            return PSX_ERR_IO;
        }
    }
    if (fclose(fp) != 0) {
        return PSX_ERR_IO;
    }
    return PSX_OK;
}

PsxResult video_ppm_write_rgb555(const char *path, const u16 *vram,
                                 u16 w, u16 h, u16 pitch_px, bool is24) {
    if (!path || !vram || w == 0u || h == 0u) {
        return PSX_ERR_BAD_ARG;
    }
    size_t row_rgb_bytes = (size_t)w * 3u;
    size_t stride_bytes = 0u;
    if (pitch_px == 0u) {
        stride_bytes = is24 ? ((row_rgb_bytes + 1u) & ~1u) : ((size_t)w * 2u);
    } else {
        stride_bytes = (size_t)pitch_px * 2u;
    }
    if (is24 && stride_bytes < row_rgb_bytes) {
        return PSX_ERR_BAD_ARG;
    }
    if (!is24 && stride_bytes < (size_t)w * 2u) {
        return PSX_ERR_BAD_ARG;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return PSX_ERR_IO;
    }
    if (fprintf(fp, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h) < 0) {
        fclose(fp);
        return PSX_ERR_IO;
    }

    u8 stack_row[1024u * 3u];
    u8 *heap_row = NULL;
    u8 *row_out = stack_row;
    if (!is24 && w > 1024u) {
        heap_row = (u8 *)malloc(row_rgb_bytes);
        if (!heap_row) {
            fclose(fp);
            return PSX_ERR_NOMEM;
        }
        row_out = heap_row;
    }

    const u8 *base = (const u8 *)vram;
    for (u16 y = 0u; y < h; y++) {
        const u8 *src = base + (size_t)y * stride_bytes;
        if (is24) {
            if (fwrite(src, 1u, row_rgb_bytes, fp) != row_rgb_bytes) {
                free(heap_row);
                fclose(fp);
                return PSX_ERR_IO;
            }
        } else {
            for (u16 x = 0u; x < w; x++) {
                u16 px = (u16)((u16)src[(size_t)x * 2u] |
                               ((u16)src[(size_t)x * 2u + 1u] << 8));
                u8 r5 = (u8)(px & 0x1Fu);
                u8 g5 = (u8)((px >> 5) & 0x1Fu);
                u8 b5 = (u8)((px >> 10) & 0x1Fu);
                row_out[(size_t)x * 3u + 0u] = (u8)((r5 << 3) | (r5 >> 2));
                row_out[(size_t)x * 3u + 1u] = (u8)((g5 << 3) | (g5 >> 2));
                row_out[(size_t)x * 3u + 2u] = (u8)((b5 << 3) | (b5 >> 2));
            }
            if (fwrite(row_out, 1u, row_rgb_bytes, fp) != row_rgb_bytes) {
                free(heap_row);
                fclose(fp);
                return PSX_ERR_IO;
            }
        }
    }

    free(heap_row);
    if (fclose(fp) != 0) {
        return PSX_ERR_IO;
    }
    return PSX_OK;
}
