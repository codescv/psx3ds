#include "psx3ds/gpu_hw.h"

#include <stdlib.h>
#include <string.h>

#if PSX3DS_HAVE_CITRO3D
#include <3ds.h>
#include <citro3d.h>
#endif

static inline u64 range_mask64(u32 c0, u32 c1) {
    u32 count = c1 - c0 + 1u;
    if (count >= 64u) {
        return ~0ULL;
    }
    return ((~0ULL) >> (64u - count)) << c0;
}

static inline u32 range_mask32(u32 r0, u32 r1) {
    u32 count = r1 - r0 + 1u;
    if (count >= 32u) {
        return ~0u;
    }
    return ((~0u) >> (32u - count)) << r0;
}

void gpu_vram_rect_cell_masks(u16 x, u16 y, u16 w, u16 h, u64 *out_cols, u32 *out_rows) {
    if (w == 0u || h == 0u) {
        *out_cols = 0ULL;
        *out_rows = 0u;
        return;
    }

    u32 x0 = (u32)x & 0x3FFu;
    u32 y0 = (u32)y & 0x1FFu;

    u64 cols;
    if (w >= GPU_HW_VRAM_W) {
        cols = ~0ULL;
    } else {
        u32 x_end = x0 + (u32)w - 1u;
        if (x_end < GPU_HW_VRAM_W) {
            cols = range_mask64(x0 >> GPU_HW_CELL_SHIFT, x_end >> GPU_HW_CELL_SHIFT);
        } else {
            cols = range_mask64(x0 >> GPU_HW_CELL_SHIFT, 63u) |
                   range_mask64(0u, (x_end & 0x3FFu) >> GPU_HW_CELL_SHIFT);
        }
    }

    u32 rows;
    if (h >= GPU_HW_VRAM_H) {
        rows = ~0u;
    } else {
        u32 y_end = y0 + (u32)h - 1u;
        if (y_end < GPU_HW_VRAM_H) {
            rows = range_mask32(y0 >> GPU_HW_CELL_SHIFT, y_end >> GPU_HW_CELL_SHIFT);
        } else {
            rows = range_mask32(y0 >> GPU_HW_CELL_SHIFT, 31u) |
                   range_mask32(0u, (y_end & 0x1FFu) >> GPU_HW_CELL_SHIFT);
        }
    }

    *out_cols = cols;
    *out_rows = rows;
}

u32 gpu_clut_hash_fnv1a(const u16 *vram, u16 clut_attr, u8 tex_depth) {
    if (tex_depth >= 2u) {
        return 0u;
    }
    u32 count = (tex_depth == 0u) ? 16u : 256u;
    u32 clut_x = ((u32)clut_attr & 0x3Fu) << 4;
    u32 clut_y = ((u32)clut_attr >> 6) & 0x1FFu;
    const u16 *row = vram + (clut_y << 10);

    u32 hash = 0x811C9DC5u;
    if (clut_x + count <= GPU_HW_VRAM_W) {
        const u16 *p = row + clut_x;
        for (u32 i = 0; i < count; ++i) {
            u16 w = p[i];
            hash ^= (u32)(w & 0xFFu);
            hash *= 0x01000193u;
            hash ^= (u32)(w >> 8);
            hash *= 0x01000193u;
        }
    } else {
        for (u32 i = 0; i < count; ++i) {
            u16 w = row[(clut_x + i) & 0x3FFu];
            hash ^= (u32)(w & 0xFFu);
            hash *= 0x01000193u;
            hash ^= (u32)(w >> 8);
            hash *= 0x01000193u;
        }
    }
    return hash;
}

static u32 scan_clut_class_union(const u16 *vram, u16 clut_x, u16 clut_y, u32 count) {
    const u16 *row = vram + (((u32)clut_y & 0x1FFu) << 10);
    u32 mask = 0u;
    for (u32 i = 0; i < count; ++i) {
        u16 c = row[(clut_x + i) & 0x3FFu];
        if (c != 0x0000u) {
            mask |= (c & 0x8000u) ? 2u : 1u;
        }
    }
    return mask;
}

static bool ensure_slot_storage(GpuTexSlot *slot, bool want_rgba8) {
    u32 want_bytes = GPU_HW_TEXPAGE_PIXELS * (want_rgba8 ? 4u : 2u);

#if PSX3DS_HAVE_CITRO3D
    C3D_Tex *tex = (C3D_Tex *)slot->hw_tex;
    if (tex != NULL && slot->pixels != NULL && slot->is_rgba8 == want_rgba8 &&
        slot->pixel_bytes == want_bytes) {
        return true;
    }
    if (tex == NULL) {
        tex = (C3D_Tex *)calloc(1, sizeof(C3D_Tex));
        if (tex == NULL) {
            return false;
        }
        slot->hw_tex = tex;
    } else if (tex->data != NULL) {
        C3D_TexDelete(tex);
        memset(tex, 0, sizeof(*tex));
    }

    GPU_TEXCOLOR fmt = want_rgba8 ? GPU_RGBA8 : GPU_RGBA5551;
    if (!C3D_TexInit(tex, GPU_HW_TEXPAGE_DIM, GPU_HW_TEXPAGE_DIM, fmt)) {
        return false;
    }
    C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);

    slot->pixels = tex->data;
    slot->pixel_bytes = want_bytes;
    slot->is_rgba8 = want_rgba8;
    return true;
#else
    if (slot->pixels != NULL && slot->is_rgba8 == want_rgba8 && slot->pixel_bytes == want_bytes) {
        return true;
    }
    free(slot->pixels);
    slot->pixels = malloc(want_bytes);
    if (slot->pixels == NULL) {
        slot->pixel_bytes = 0u;
        return false;
    }
    slot->pixel_bytes = want_bytes;
    slot->is_rgba8 = want_rgba8;
    return true;
#endif
}

static void free_slot_storage(GpuTexSlot *slot) {
#if PSX3DS_HAVE_CITRO3D
    if (slot->hw_tex != NULL) {
        C3D_Tex *tex = (C3D_Tex *)slot->hw_tex;
        if (tex->data != NULL) {
            C3D_TexDelete(tex);
        }
        free(tex);
        slot->hw_tex = NULL;
    }
    slot->pixels = NULL;
    slot->pixel_bytes = 0u;
#else
    free(slot->pixels);
    slot->pixels = NULL;
    slot->pixel_bytes = 0u;
#endif
}

bool gpu_texcache_init(GpuTexCache *tc, bool allow_rgba8, u32 max_slots) {
    if (tc == NULL) {
        return false;
    }
    memset(tc, 0, sizeof(*tc));
    tc->allow_rgba8 = allow_rgba8;
    if (max_slots == 0u || max_slots > GPU_HW_TEXCACHE_SLOTS) {
        max_slots = GPU_HW_TEXCACHE_SLOTS;
    }
    tc->max_slots = max_slots;
    return true;
}

static inline bool slot_in_flight(const GpuTexCache *tc, const GpuTexSlot *s) {
    return tc->cur_frame != 0u && s->last_frame == tc->cur_frame;
}

static inline const GpuTexSlot *slot_touch(GpuTexCache *tc, GpuTexSlot *s) {
    s->last_used = ++tc->lru_clock;
    s->last_frame = tc->cur_frame;
    tc->mru = (u32)(s - tc->slots);
    return s;
}

void gpu_texcache_reset(GpuTexCache *tc) {
    if (tc == NULL) {
        return;
    }
    for (u32 i = 0; i < GPU_HW_TEXCACHE_SLOTS; ++i) {
        GpuTexSlot *s = &tc->slots[i];
        s->valid = false;
        s->texel_dirty = false;
        s->clut_dirty = false;
        s->last_used = 0u;
        s->last_frame = 0u;
        s->decode_gen = 0u;
    }
    tc->lru_clock = 0u;
    tc->hits = 0u;
    tc->uploads = 0u;
    tc->mru = 0u;
    tc->stalled = false;
}

void gpu_texcache_shutdown(GpuTexCache *tc) {
    if (tc == NULL) {
        return;
    }
    for (u32 i = 0; i < GPU_HW_TEXCACHE_SLOTS; ++i) {
        free_slot_storage(&tc->slots[i]);
    }
    memset(tc, 0, sizeof(*tc));
}

void gpu_texcache_invalidate_vram(GpuTexCache *tc, u16 x, u16 y, u16 w, u16 h) {
    if (tc == NULL || w == 0u || h == 0u) {
        return;
    }
    u64 cols = 0ULL;
    u32 rows = 0u;
    gpu_vram_rect_cell_masks(x, y, w, h, &cols, &rows);

    for (u32 i = 0; i < tc->max_slots; ++i) {
        GpuTexSlot *s = &tc->slots[i];
        if (!s->valid) {
            continue;
        }
        if ((s->tp_row_mask & rows) != 0u && (s->tp_col_mask & cols) != 0ULL) {
            s->texel_dirty = true;
        }
        if (s->has_clut && (s->clut_row_bit & rows) != 0u && (s->clut_col_mask & cols) != 0ULL) {
            s->clut_dirty = true;
        }
    }
}

static inline bool tw_keys_equal(const GpuTexWindowKey *a, const GpuTexWindowKey *b) {
    return a->mask_x == b->mask_x && a->mask_y == b->mask_y &&
           a->off_x == b->off_x && a->off_y == b->off_y;
}

static void update_slot_clut_footprint(GpuTexSlot *s, u16 clut_norm, u8 eff_depth) {
    if (eff_depth >= 2u) {
        s->has_clut = false;
        s->clut_attr = 0u;
        s->clut_col_mask = 0ULL;
        s->clut_row_bit = 0u;
        s->clut_dirty = false;
        return;
    }
    u16 clut_x = (u16)(((u32)clut_norm & 0x3Fu) << 4);
    u16 clut_y = (u16)(((u32)clut_norm >> 6) & 0x1FFu);
    u16 clut_w = (eff_depth == 0u) ? 16u : 256u;
    u64 ccols = 0ULL;
    u32 crows = 0u;
    gpu_vram_rect_cell_masks(clut_x, clut_y, clut_w, 1u, &ccols, &crows);
    s->has_clut = true;
    s->clut_attr = clut_norm;
    s->clut_col_mask = ccols;
    s->clut_row_bit = crows;
    s->clut_dirty = false;
}

const GpuTexSlot *gpu_texcache_lookup(GpuTexCache *tc, const u16 *vram,
                                      u16 texpage, u8 tex_depth, u16 clut,
                                      const GpuTexWindowKey *tw,
                                      PsxStats *stats, bool *out_hit) {
    if (tc == NULL || vram == NULL) {
        if (out_hit) {
            *out_hit = false;
        }
        return NULL;
    }

    u8  eff_depth = (tex_depth >= 2u) ? 2u : tex_depth;
    u16 tp_key = (u16)((texpage & 0x001Fu) | ((u16)eff_depth << 7));
    bool has_clut = (eff_depth < 2u);
    u16 clut_norm = has_clut ? (u16)(clut & 0x7FFFu) : 0u;

    GpuTexWindowKey tw_norm = { 0u, 0u, 0u, 0u };
    if (tw != NULL && ((tw->mask_x & 0x1Fu) != 0u || (tw->mask_y & 0x1Fu) != 0u)) {
        tw_norm.mask_x = (u8)(tw->mask_x & 0x1Fu);
        tw_norm.mask_y = (u8)(tw->mask_y & 0x1Fu);
        tw_norm.off_x  = (u8)((tw->off_x & tw->mask_x) & 0x1Fu);
        tw_norm.off_y  = (u8)((tw->off_y & tw->mask_y) & 0x1Fu);
    }

    tc->stalled = false;
    u32 nslots = tc->max_slots;

    /* 1. Ultra-fast zero-hash check: MRU slot first, then exact (texpage, clut_attr, tw) scan */
    {
        GpuTexSlot *s = &tc->slots[tc->mru < nslots ? tc->mru : 0u];
        if (s->valid && !s->texel_dirty && !s->clut_dirty &&
            s->texpage_key == tp_key && s->clut_attr == clut_norm &&
            tw_keys_equal(&s->tw, &tw_norm)) {
            tc->hits++;
            if (stats) {
                stats->gpu_tex_hits++;
            }
            if (out_hit) {
                *out_hit = true;
            }
            return slot_touch(tc, s);
        }
    }
    for (u32 i = 0; i < nslots; ++i) {
        GpuTexSlot *s = &tc->slots[i];
        if (s->valid && !s->texel_dirty && !s->clut_dirty &&
            s->texpage_key == tp_key && s->clut_attr == clut_norm &&
            tw_keys_equal(&s->tw, &tw_norm)) {
            tc->hits++;
            if (stats) {
                stats->gpu_tex_hits++;
            }
            if (out_hit) {
                *out_hit = true;
            }
            return slot_touch(tc, s);
        }
    }

    /* 2. Compute FNV-1a CLUT content hash and search for any clean-texel content match */
    u32 cur_hash = has_clut ? gpu_clut_hash_fnv1a(vram, clut_norm, eff_depth) : 0u;
    for (u32 i = 0; i < nslots; ++i) {
        GpuTexSlot *s = &tc->slots[i];
        if (s->valid && !s->texel_dirty &&
            s->texpage_key == tp_key && tw_keys_equal(&s->tw, &tw_norm) &&
            (!has_clut || s->clut_hash == cur_hash)) {
            update_slot_clut_footprint(s, clut_norm, eff_depth);
            tc->hits++;
            if (stats) {
                stats->gpu_tex_hits++;
            }
            if (out_hit) {
                *out_hit = true;
            }
            return slot_touch(tc, s);
        }
    }

    /*
     * 3. Cache miss: retire any texel-dirty matching slots, then pick a victim that the
     * PICA200 is not still going to sample this frame: a free slot first (preferring one
     * other than the retired exact match), else the oldest LRU slot that is not in flight.
     */
    GpuTexSlot *dirty_match = NULL;
    for (u32 i = 0; i < nslots; ++i) {
        GpuTexSlot *s = &tc->slots[i];
        if (s->valid && s->texel_dirty && s->texpage_key == tp_key &&
            tw_keys_equal(&s->tw, &tw_norm)) {
            s->valid = false;
            if (dirty_match == NULL && s->clut_attr == clut_norm && !slot_in_flight(tc, s)) {
                dirty_match = s;
            }
        }
    }

    GpuTexSlot *victim = NULL;
    for (u32 i = 0; i < nslots; ++i) {
        GpuTexSlot *s = &tc->slots[i];
        if (!s->valid && s != dirty_match && !slot_in_flight(tc, s)) {
            victim = s;
            break;
        }
    }
    if (victim == NULL && dirty_match != NULL) {
        victim = dirty_match;
    }
    if (victim == NULL) {
        for (u32 i = 0u; i < nslots; ++i) {
            GpuTexSlot *s = &tc->slots[i];
            if (slot_in_flight(tc, s)) {
                continue;
            }
            if (victim == NULL || s->last_used < victim->last_used) {
                victim = s;
            }
        }
    }
    if (victim == NULL) {
        tc->stalled = true;
        if (out_hit) {
            *out_hit = false;
        }
        return NULL;
    }

    u16 tp_x = (u16)((texpage & 0x0Fu) << 6);
    u16 tp_y = (u16)(((texpage >> 4) & 0x01u) << 8);
    u16 clut_x = (u16)(((u32)clut_norm & 0x3Fu) << 4);
    u16 clut_y = (u16)(((u32)clut_norm >> 6) & 0x1FFu);

    GpuTexAlphaClass acls = GPU_TEX_ALPHA_ALL_OPAQUE;
    if (eff_depth == 0u) {
        u32 c_union = scan_clut_class_union(vram, clut_x, clut_y, 16u);
        bool want8 = tc->allow_rgba8 && ((c_union & 3u) == 3u);
        if (!ensure_slot_storage(victim, want8)) {
            if (!want8 || !ensure_slot_storage(victim, false)) {
                if (out_hit) {
                    *out_hit = false;
                }
                return NULL;
            }
            want8 = false;
        }
        if (want8) {
            acls = gpu_morton_decode_4bpp_8888((u32 *)victim->pixels, vram,
                                               tp_x, tp_y, clut_x, clut_y, &tw_norm,
                                               &victim->tiles);
        } else {
            acls = gpu_morton_decode_4bpp_5551((u16 *)victim->pixels, vram,
                                               tp_x, tp_y, clut_x, clut_y, &tw_norm,
                                               &victim->tiles);
            if (acls == GPU_TEX_ALPHA_MIXED) {
                acls = GPU_TEX_ALPHA_ALL_STP;
            }
        }
    } else if (eff_depth == 1u) {
        u32 c_union = scan_clut_class_union(vram, clut_x, clut_y, 256u);
        bool want8 = tc->allow_rgba8 && ((c_union & 3u) == 3u);
        if (!ensure_slot_storage(victim, want8)) {
            if (!want8 || !ensure_slot_storage(victim, false)) {
                if (out_hit) {
                    *out_hit = false;
                }
                return NULL;
            }
            want8 = false;
        }
        if (want8) {
            acls = gpu_morton_decode_8bpp_8888((u32 *)victim->pixels, vram,
                                               tp_x, tp_y, clut_x, clut_y, &tw_norm,
                                               &victim->tiles);
        } else {
            acls = gpu_morton_decode_8bpp_5551((u16 *)victim->pixels, vram,
                                               tp_x, tp_y, clut_x, clut_y, &tw_norm,
                                               &victim->tiles);
            if (acls == GPU_TEX_ALPHA_MIXED) {
                acls = GPU_TEX_ALPHA_ALL_STP;
            }
        }
    } else {
        if (!ensure_slot_storage(victim, false)) {
            if (out_hit) {
                *out_hit = false;
            }
            return NULL;
        }
        acls = gpu_morton_decode_15bpp_5551((u16 *)victim->pixels, vram, tp_x, tp_y, &tw_norm,
                                            &victim->tiles);
        if (acls == GPU_TEX_ALPHA_MIXED) {
            if (tc->allow_rgba8 && ensure_slot_storage(victim, true)) {
                acls = gpu_morton_decode_15bpp_8888((u32 *)victim->pixels, vram, tp_x, tp_y,
                                                    &tw_norm, &victim->tiles);
            } else {
                acls = GPU_TEX_ALPHA_ALL_STP;
            }
        }
    }

#if PSX3DS_HAVE_CITRO3D
    /* Only flush D-cache on newly decoded/uploaded texture buffers! */
    GSPGPU_FlushDataCache(victim->pixels, victim->pixel_bytes);
#endif

    u16 tp_w_words = (eff_depth == 0u) ? 64u : ((eff_depth == 1u) ? 128u : 256u);
    gpu_vram_rect_cell_masks(tp_x, tp_y, tp_w_words, GPU_HW_TEXPAGE_DIM,
                             &victim->tp_col_mask, &victim->tp_row_mask);
    update_slot_clut_footprint(victim, clut_norm, eff_depth);

    victim->valid       = true;
    victim->texel_dirty = false;
    victim->tex_depth   = eff_depth;
    victim->alpha_class = (u8)acls;
    victim->texpage_key = tp_key;
    victim->tw          = tw_norm;
    victim->clut_hash   = cur_hash;
    victim->decode_gen++;
    (void)slot_touch(tc, victim);

    tc->uploads++;
    if (stats) {
        stats->gpu_tex_uploads++;
    }
    if (out_hit) {
        *out_hit = false;
    }
    return victim;
}
