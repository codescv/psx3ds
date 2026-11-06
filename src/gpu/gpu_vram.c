#include "gpu_internal.h"
#include "psx3ds/platform.h"

#include <string.h>

void gpu_vram_clear(u16 *vram) {
    if (!vram) {
        return;
    }
    memset(vram, 0, PSX_VRAM_BYTES);
}

void gpu_vram_fill_rect(u16 *vram, u16 x, u16 y, u16 w, u16 h, u32 bgr24) {
    if (!vram) {
        return;
    }
    /*
     * PSX GP0(02h) FillRect hardware alignment & wrapping rules:
     * - X rounded down to 16-pixel boundary: x &= 0x3F0
     * - Y masked to 0..511: y &= 0x1FF
     * - W masked to 0..1023 then rounded UP to multiple of 16: ((w & 0x3FF) + 0xF) & ~0xF
     * - H masked to 0..511: h &= 0x1FF
     * - Ignores DrawOffset, DrawArea clip, and MaskBit (bit 15 written is always 0).
     */
    u32 fx = (u32)x & 0x3F0u;
    u32 fy = (u32)y & 0x1FFu;
    u32 fw = (((u32)w & 0x3FFu) + 0xFu) & ~0xFu;
    u32 fh = (u32)h & 0x1FFu;
    if (fw == 0u || fh == 0u) {
        return;
    }

    u16 col15 = (u16)(((bgr24 >> 3) & 0x1Fu) |
                      (((bgr24 >> 11) & 0x1Fu) << 5) |
                      (((bgr24 >> 19) & 0x1Fu) << 10));

    for (u32 ry = 0u; ry < fh; ry++) {
        u16 *row = vram + (((fy + ry) & 0x1FFu) * PSX_VRAM_W);
        if (fx + fw <= PSX_VRAM_W) {
            gpu_soft_span_fill16(row + fx, col15, fw);
        } else {
            u32 first = PSX_VRAM_W - fx;
            u32 second = fw - first;
            gpu_soft_span_fill16(row + fx, col15, first);
            gpu_soft_span_fill16(row, col15, second);
        }
    }
}

static void copy_row_masked(u16 *vram, u32 src_y, u32 dst_y,
                            u32 sx, u32 dx, u32 w,
                            u8 check_mask, u8 set_mask,
                            u16 row_tmp[PSX_VRAM_W]) {
    const u16 *srow = vram + ((src_y & 0x1FFu) * PSX_VRAM_W);
    u16       *drow = vram + ((dst_y & 0x1FFu) * PSX_VRAM_W);
    u16        mask_or = set_mask ? 0x8000u : 0u;

    /* Gather entire source row span first so horizontal overlap/wrap is 100% safe */
    if (sx + w <= PSX_VRAM_W) {
        memcpy(row_tmp, srow + sx, w * sizeof(u16));
    } else {
        u32 n1 = PSX_VRAM_W - sx;
        u32 n2 = w - n1;
        memcpy(row_tmp, srow + sx, n1 * sizeof(u16));
        memcpy(row_tmp + n1, srow, n2 * sizeof(u16));
    }

    if (!check_mask && !set_mask) {
        if (dx + w <= PSX_VRAM_W) {
            memcpy(drow + dx, row_tmp, w * sizeof(u16));
        } else {
            u32 n1 = PSX_VRAM_W - dx;
            u32 n2 = w - n1;
            memcpy(drow + dx, row_tmp, n1 * sizeof(u16));
            memcpy(drow, row_tmp + n1, n2 * sizeof(u16));
        }
        return;
    }

    if (!check_mask) {
        for (u32 i = 0u; i < w; i++) {
            drow[(dx + i) & 0x3FFu] = (u16)(row_tmp[i] | mask_or);
        }
    } else {
        for (u32 i = 0u; i < w; i++) {
            u16 *dp = &drow[(dx + i) & 0x3FFu];
            if ((*dp & 0x8000u) == 0u) {
                *dp = (u16)(row_tmp[i] | mask_or);
            }
        }
    }
}

void gpu_vram_copy_rect(u16 *vram, u16 sx, u16 sy, u16 dx, u16 dy,
                        u16 w, u16 h, u8 check_mask, u8 set_mask) {
    if (!vram || w == 0u || h == 0u) {
        return;
    }
    u32 csx = (u32)sx & 0x3FFu;
    u32 csy = (u32)sy & 0x1FFu;
    u32 cdx = (u32)dx & 0x3FFu;
    u32 cdy = (u32)dy & 0x1FFu;
    u32 cw  = (w > PSX_VRAM_W) ? PSX_VRAM_W : (u32)w;
    u32 ch  = (h > PSX_VRAM_H) ? PSX_VRAM_H : (u32)h;

    if (csx == cdx && csy == cdy && !set_mask) {
        return;
    }

    u16 row_tmp[PSX_VRAM_W];
    u32 delta_y = (cdy - csy) & 0x1FFu;

    /*
     * Direction selection on the 512-row VRAM ring:
     * - delta_y == 0 or delta_y >= ch: forward iteration (ry = 0..ch-1) never
     *   overwrites an unread source row.
     * - 1 <= delta_y < ch and delta_y + ch <= 512: backward iteration
     *   (ry = ch-1..0) never overwrites an unread source row.
     */
    if (delta_y > 0u && delta_y < ch && (delta_y + ch) <= PSX_VRAM_H) {
        for (u32 i = ch; i > 0u; i--) {
            u32 ry = i - 1u;
            copy_row_masked(vram, csy + ry, cdy + ry, csx, cdx, cw,
                            check_mask, set_mask, row_tmp);
        }
    } else {
        for (u32 ry = 0u; ry < ch; ry++) {
            copy_row_masked(vram, csy + ry, cdy + ry, csx, cdx, cw,
                            check_mask, set_mask, row_tmp);
        }
    }
}

void gpu_vram_extract_rgb555(const u16 *vram, u16 vx, u16 vy, u16 w, u16 h,
                             bool blank, u16 *out_rgb555, u16 out_pitch_px) {
    if (!out_rgb555 || w == 0u || h == 0u) {
        return;
    }
    if (blank || !vram) {
        for (u32 ry = 0u; ry < (u32)h; ry++) {
            gpu_soft_span_fill16(out_rgb555 + ry * (u32)out_pitch_px, 0u, (u32)w);
        }
        return;
    }

    u32 sx = (u32)vx & 0x3FFu;
    u32 sy = (u32)vy & 0x1FFu;
    u32 cw = (w > PSX_VRAM_W) ? PSX_VRAM_W : (u32)w;

    for (u32 ry = 0u; ry < (u32)h; ry++) {
        const u16 *srow = vram + (((sy + ry) & 0x1FFu) * PSX_VRAM_W);
        u16       *drow = out_rgb555 + ry * (u32)out_pitch_px;
        if (sx + cw <= PSX_VRAM_W) {
            memcpy(drow, srow + sx, cw * sizeof(u16));
        } else {
            u32 n1 = PSX_VRAM_W - sx;
            u32 n2 = cw - n1;
            memcpy(drow, srow + sx, n1 * sizeof(u16));
            memcpy(drow + n1, srow, n2 * sizeof(u16));
        }
    }
}

void gpu_vram_extract_rgb888(const u16 *vram, u16 vx, u16 vy, u16 w, u16 h,
                             bool blank, u8 *out_rgb888, u32 out_row_bytes) {
    if (!out_rgb888 || w == 0u || h == 0u) {
        return;
    }
    u32 row_bytes = (u32)w * 3u;
    if (blank || !vram) {
        for (u32 ry = 0u; ry < (u32)h; ry++) {
            memset(out_rgb888 + ry * out_row_bytes, 0, row_bytes);
        }
        return;
    }

    u32 byte_x = ((u32)vx & 0x3FFu) * 2u;
    u32 sy     = (u32)vy & 0x1FFu;
    const u32 vram_row_bytes = PSX_VRAM_W * 2u; /* 2048 bytes per VRAM scanline */

    for (u32 ry = 0u; ry < (u32)h; ry++) {
        const u8 *srow = (const u8 *)(vram + (((sy + ry) & 0x1FFu) * PSX_VRAM_W));
        u8       *drow = out_rgb888 + ry * out_row_bytes;
        if (byte_x + row_bytes <= vram_row_bytes) {
            memcpy(drow, srow + byte_x, row_bytes);
        } else {
            u32 n1 = vram_row_bytes - byte_x;
            u32 n2 = row_bytes - n1;
            if (n2 > vram_row_bytes) {
                n2 = vram_row_bytes;
            }
            memcpy(drow, srow + byte_x, n1);
            memcpy(drow + n1, srow, n2);
        }
    }
}

void gpu_vram_present(const u16 *vram, u16 *staging_buf, const GpuDisplay *disp) {
    if (!vram || !disp) {
        return;
    }
    u16 w = disp->hres ? disp->hres : 320u;
    u16 h = disp->vres ? disp->vres : 240u;
    if (w > 640u) w = 640u;
    if (h > 480u) h = 480u;

    u32 vx = (u32)disp->vram_x & 0x3FFu;
    u32 vy = (u32)disp->vram_y & 0x1FFu;
    bool is24 = (disp->color24 != 0u);

    /*
     * Fast zero-copy path when display is unblanked and the active display
     * window does not wrap across the right (2048B) or bottom (512-line) edge.
     */
    if (!disp->blank && (vy + (u32)h <= PSX_VRAM_H)) {
        u32 needed_bytes = is24 ? ((u32)w * 3u) : ((u32)w * 2u);
        if ((vx * 2u) + needed_bytes <= (PSX_VRAM_W * 2u)) {
            const u16 *ptr = vram + (vy * PSX_VRAM_W) + vx;
            plat_video_present(ptr, w, h, (u16)PSX_VRAM_W, is24);
            return;
        }
    }

    if (!staging_buf) {
        return;
    }

    if (!is24) {
        gpu_vram_extract_rgb555(vram, (u16)vx, (u16)vy, w, h,
                                disp->blank != 0u, staging_buf, w);
        plat_video_present(staging_buf, w, h, w, false);
    } else {
        u16 pitch_u16 = (u16)(((u32)w * 3u + 1u) >> 1);
        gpu_vram_extract_rgb888(vram, (u16)vx, (u16)vy, w, h,
                                disp->blank != 0u, (u8 *)staging_buf,
                                (u32)pitch_u16 * 2u);
        plat_video_present(staging_buf, w, h, pitch_u16, true);
    }
}
