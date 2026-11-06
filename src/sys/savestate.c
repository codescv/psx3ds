#include "psx3ds/system.h"

#include <stdlib.h>
#include <string.h>

/*
 * Chunked binary save-state serializer & deserializer with IEEE 802.3 CRC32
 * integrity verification (ARCHITECTURE.md §9).
 *
 * File header (20 bytes):
 *   [0..7]   "PSX3DSS\0" (8 bytes)
 *   [8..11]  u32 version (PSX_STATE_VERSION = 1)
 *   [12..15] u32 payload_bytes
 *   [16..19] u32 crc32 (computed over payload_bytes starting at byte 20)
 */

#define PSX_STATE_HDR_BYTES 20u

u32 psx_crc32(const void *data, size_t len) {
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (u32)p[i];
        for (int b = 0; b < 8; b++) {
            u32 mask = (u32)-(s32)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static bool state_check_space(StateIO *s, u32 len) {
    if (!s || s->error) {
        return false;
    }
    if ((u64)s->pos + (u64)len > (u64)s->cap) {
        s->error = true;
        return false;
    }
    /* Allow dry-run sizing pass when writing with buf == NULL */
    if (!s->buf && !s->writing) {
        s->error = true;
        return false;
    }
    return true;
}

void state_u8(StateIO *s, u8 *v) {
    if (!v || !state_check_space(s, 1u)) {
        return;
    }
    if (s->writing) {
        if (s->buf) {
            s->buf[s->pos] = *v;
        }
    } else {
        *v = s->buf[s->pos];
    }
    s->pos += 1u;
}

void state_u16(StateIO *s, u16 *v) {
    if (!v || !state_check_space(s, 2u)) {
        return;
    }
    if (s->writing) {
        if (s->buf) {
            memcpy(s->buf + s->pos, v, 2u);
        }
    } else {
        memcpy(v, s->buf + s->pos, 2u);
    }
    s->pos += 2u;
}

void state_u32(StateIO *s, u32 *v) {
    if (!v || !state_check_space(s, 4u)) {
        return;
    }
    if (s->writing) {
        if (s->buf) {
            memcpy(s->buf + s->pos, v, 4u);
        }
    } else {
        memcpy(v, s->buf + s->pos, 4u);
    }
    s->pos += 4u;
}

void state_u64(StateIO *s, u64 *v) {
    if (!v || !state_check_space(s, 8u)) {
        return;
    }
    if (s->writing) {
        if (s->buf) {
            memcpy(s->buf + s->pos, v, 8u);
        }
    } else {
        memcpy(v, s->buf + s->pos, 8u);
    }
    s->pos += 8u;
}

void state_bool(StateIO *s, bool *v) {
    if (!v) {
        return;
    }
    u8 b = *v ? 1u : 0u;
    state_u8(s, &b);
    if (!s->writing && !s->error) {
        *v = (b != 0u);
    }
}

void state_bytes(StateIO *s, void *data, u32 len) {
    if (len == 0u) {
        return;
    }
    if (!data || !state_check_space(s, len)) {
        return;
    }
    if (s->writing) {
        if (s->buf) {
            memcpy(s->buf + s->pos, data, len);
        }
    } else {
        memcpy(data, s->buf + s->pos, len);
    }
    s->pos += len;
}

void state_skip(StateIO *s, u32 len) {
    if (len == 0u) {
        return;
    }
    if (!state_check_space(s, len)) {
        return;
    }
    if (s->writing && s->buf) {
        memset(s->buf + s->pos, 0, len);
    }
    s->pos += len;
}

/* Fallback serializers used only if an owner slice's xxx_state() is still a 0-byte stub */
static void fallback_cpu_state(PsxSystem *sys, StateIO *st) {
    PsxCpu *c = &sys->cpu;
    state_bytes(st, c->gpr, (u32)sizeof(c->gpr));
    state_u32(st, &c->pc);
    state_u32(st, &c->next_pc);
    state_u32(st, &c->hi);
    state_u32(st, &c->lo);
    state_u32(st, (u32 *)&c->cycles_left);
    state_u32(st, (u32 *)&c->cycle_deficit);
    state_u32(st, &c->irq_pending);
    state_u32(st, &c->in_delay_slot);
    state_u32(st, &c->load_delay_reg);
    state_u32(st, &c->load_delay_val);
    state_u32(st, &c->branch_target);
    state_u32(st, &c->branch_taken);
    state_u32(st, (u32 *)&c->slice_len);
    state_bytes(st, c->cop0, (u32)sizeof(c->cop0));
    state_bytes(st, c->gte_d, (u32)sizeof(c->gte_d));
    state_bytes(st, c->gte_c, (u32)sizeof(c->gte_c));
    state_u64(st, &c->total_cycles);
    state_u32(st, &c->cache_ctrl);
    state_u32(st, &c->exc_pending);
}

static void fallback_bus_state(PsxSystem *sys, StateIO *st) {
    if (sys->bus.ram) {
        state_bytes(st, sys->bus.ram, PSX_RAM_SIZE);
    }
    if (sys->bus.scratch_page) {
        state_bytes(st, sys->bus.scratch_page, PSX_SCRATCH_SIZE);
    }
    state_bool(st, &sys->bus.isolated);
    state_u32(st, &sys->bus.ram_size_reg);
    state_bytes(st, sys->bus.mem_ctrl, (u32)sizeof(sys->bus.mem_ctrl));
    state_u32(st, &sys->bus.exp2_post);
}

static void fallback_sched_state(PsxSystem *sys, StateIO *st) {
    Scheduler *sc = &sys->sched;
    state_u32(st, &sc->now);
    state_u32(st, &sc->slice_start);
    state_u8(st, &sc->count);
    state_u16(st, &sc->active_mask);
    state_u64(st, &sc->total);
    for (u32 i = 0; i < EV_COUNT; i++) {
        state_u32(st, &sc->q[i].when);
        state_u8(st, &sc->q[i].id);
        state_u32(st, &sc->when_by_id[i]);
    }
}

static void fallback_irq_state(PsxSystem *sys, StateIO *st) {
    state_u32(st, &sys->irq.stat);
    state_u32(st, &sys->irq.mask);
}

static void fallback_dma_state(PsxSystem *sys, StateIO *st) {
    for (u32 i = 0; i < 7u; i++) {
        state_u32(st, &sys->dma.ch[i].madr);
        state_u32(st, &sys->dma.ch[i].bcr);
        state_u32(st, &sys->dma.ch[i].chcr);
    }
    state_u32(st, &sys->dma.dpcr);
    state_u32(st, &sys->dma.dicr);
    state_u8(st, &sys->dma.running_mask);
}

static void fallback_timers_state(PsxSystem *sys, StateIO *st) {
    for (u32 i = 0; i < 3u; i++) {
        state_u32(st, &sys->timers.t[i].mode);
        state_u32(st, &sys->timers.t[i].target);
        state_u32(st, &sys->timers.t[i].base_value);
        state_u32(st, &sys->timers.t[i].base_tick);
        state_u32(st, &sys->timers.t[i].rate_num);
        state_u32(st, &sys->timers.t[i].rate_den);
        state_u8(st, &sys->timers.t[i].gated);
        state_u8(st, &sys->timers.t[i].reached_target);
        state_u8(st, &sys->timers.t[i].reached_ffff);
    }
    state_u32(st, &sys->timers.scanline);
    state_u32(st, &sys->timers.line_frac_q16);
    state_bool(st, &sys->timers.in_vblank);
}

static void fallback_gpu_state(PsxSystem *sys, StateIO *st) {
    if (sys->gpu.vram) {
        state_bytes(st, sys->gpu.vram, PSX_VRAM_BYTES);
    }
    state_u32(st, &sys->gpu.gpustat);
    state_bytes(st, &sys->gpu.env, (u32)sizeof(sys->gpu.env));
    state_bytes(st, &sys->gpu.disp, (u32)sizeof(sys->gpu.disp));
    u32 gp0_st = (u32)sys->gpu.st;
    state_u32(st, &gp0_st);
    if (!st->writing) {
        sys->gpu.st = gp0_st;
    }
    state_bytes(st, sys->gpu.fifo, (u32)sizeof(sys->gpu.fifo));
    state_u8(st, &sys->gpu.fifo_n);
    state_u8(st, &sys->gpu.fifo_need);
    state_u8(st, &sys->gpu.cmd);
    state_u16(st, &sys->gpu.xfer_x);
    state_u16(st, &sys->gpu.xfer_y);
    state_u16(st, &sys->gpu.xfer_w);
    state_u16(st, &sys->gpu.xfer_h);
    state_u16(st, &sys->gpu.xfer_cx);
    state_u16(st, &sys->gpu.xfer_cy);
    state_u32(st, &sys->gpu.xfer_words_left);
    state_u32(st, &sys->gpu.gpuread_latch);
    state_bool(st, &sys->gpu.gpuread_valid);
    state_u32(st, &sys->gpu.irq1);
    state_u8(st, &sys->gpu.dma_dir);
    state_u8(st, &sys->gpu.tex_disable);
}

static void fallback_spu_state(PsxSystem *sys, StateIO *st) {
    if (sys->spu.ram) {
        state_bytes(st, sys->spu.ram, PSX_SPU_RAM_BYTES);
    }
    state_bytes(st, sys->spu.regs, (u32)sizeof(sys->spu.regs));
    state_bytes(st, sys->spu.v, (u32)sizeof(sys->spu.v));
    state_u32(st, &sys->spu.spucnt);
    state_u32(st, &sys->spu.spustat);
    state_u32(st, &sys->spu.irq_addr);
    state_u32(st, &sys->spu.xfer_addr);
    state_u32(st, &sys->spu.xfer_ctrl);
    state_u32(st, &sys->spu.noise_lfsr);
    state_u32(st, &sys->spu.kon);
    state_u32(st, &sys->spu.koff);
    state_u32(st, &sys->spu.endx);
    state_bytes(st, sys->spu.reverb_regs, (u32)sizeof(sys->spu.reverb_regs));
    state_u32(st, &sys->spu.reverb_pos);
}

static void fallback_cdrom_state(PsxSystem *sys, StateIO *st) {
    PsxCdrom *cd = &sys->cdrom;
    state_u8(st, &cd->index);
    state_u8(st, &cd->status);
    state_u8(st, &cd->mode);
    state_u8(st, &cd->irq_enable);
    state_u8(st, &cd->irq_flag);
    state_bytes(st, cd->param_fifo, (u32)sizeof(cd->param_fifo));
    state_bytes(st, cd->resp_fifo, (u32)sizeof(cd->resp_fifo));
    state_u8(st, &cd->param_n);
    state_u8(st, &cd->resp_r);
    state_u8(st, &cd->resp_w);
    state_bytes(st, cd->sector, (u32)sizeof(cd->sector));
    state_bytes(st, cd->data_fifo, (u32)sizeof(cd->data_fifo));
    state_u32(st, &cd->data_r);
    state_u32(st, &cd->data_n);
    state_bool(st, &cd->data_ready);
    state_bytes(st, cd->pending_int, (u32)sizeof(cd->pending_int));
    state_bytes(st, cd->pending_resp, (u32)sizeof(cd->pending_resp));
    state_bytes(st, cd->pending_n, (u32)sizeof(cd->pending_n));
    state_u8(st, &cd->pending_count);
    state_u32(st, &cd->seek_lba);
    state_u32(st, &cd->cur_lba);
    state_u32(st, &cd->setloc_lba);
    state_bool(st, &cd->setloc_pending);
    state_bool(st, &cd->reading);
    state_bool(st, &cd->playing);
    state_bool(st, &cd->paused);
    state_bool(st, &cd->muted);
    state_bool(st, &cd->double_speed);
    state_bool(st, &cd->xa_filter_on);
    state_u8(st, &cd->xa_file);
    state_u8(st, &cd->xa_channel);
    state_u8(st, &cd->cmd);
    state_u8(st, &cd->state);
    state_u32(st, &cd->state_tick);
}

static void fallback_mdec_state(PsxSystem *sys, StateIO *st) {
    PsxMdec *m = &sys->mdec;
    state_u32(st, &m->status);
    state_u32(st, &m->control);
    state_bytes(st, m->iq_y, (u32)sizeof(m->iq_y));
    state_bytes(st, m->iq_uv, (u32)sizeof(m->iq_uv));
    state_bytes(st, m->idct, (u32)sizeof(m->idct));
    state_bytes(st, m->in_fifo, (u32)sizeof(m->in_fifo));
    state_u32(st, &m->in_r);
    state_u32(st, &m->in_w);
    state_bytes(st, m->out_fifo, (u32)sizeof(m->out_fifo));
    state_u32(st, &m->out_r);
    state_u32(st, &m->out_w);
    state_u32(st, &m->cur_block);
    state_u32(st, &m->cur_coeff);
    state_u32(st, &m->words_left);
    state_u8(st, &m->out_depth);
    state_u8(st, &m->out_signed);
    state_u8(st, &m->out_bit15);
    state_bool(st, &m->decoding);
}

static void fallback_sio_state(PsxSystem *sys, StateIO *st) {
    PsxSio *s = &sys->sio;
    state_u16(st, &s->mode);
    state_u16(st, &s->ctrl);
    state_u16(st, &s->baud);
    state_u32(st, &s->stat);
    state_u8(st, &s->tx_data);
    state_bytes(st, s->rx_fifo, (u32)sizeof(s->rx_fifo));
    state_u8(st, &s->rx_r);
    state_u8(st, &s->rx_w);
    state_bool(st, &s->tx_pending);
    state_bool(st, &s->selected);
    state_u8(st, &s->device);
    state_u8(st, &s->port);
    state_u8(st, &s->seq);
    state_bytes(st, s->pad_mode, (u32)sizeof(s->pad_mode));
    state_bytes(st, s->pad_config, (u32)sizeof(s->pad_config));
}

static void sys_meta_state(PsxSystem *sys, StateIO *st) {
    state_bool(st, &sys->frame_done);
    for (int i = 0; i < 2; i++) {
        state_u16(st, &sys->pads[i].buttons);
        state_u8(st, &sys->pads[i].lx);
        state_u8(st, &sys->pads[i].ly);
        state_u8(st, &sys->pads[i].rx);
        state_u8(st, &sys->pads[i].ry);
        state_u8(st, &sys->pads[i].analog_mode);
        state_u8(st, &sys->pads[i].connected);
    }
}

typedef struct StateSection {
    u32 tag;
    u32 fb_tag;
    void (*primary)(PsxSystem *sys, StateIO *st);
    void (*fallback)(PsxSystem *sys, StateIO *st);
} StateSection;

static const StateSection k_sections[] = {
    { PSX_FOURCC('C', 'P', 'U', ' '), PSX_FOURCC('c', 'p', 'u', '_'), cpu_state,    fallback_cpu_state    },
    { PSX_FOURCC('B', 'U', 'S', ' '), PSX_FOURCC('b', 'u', 's', '_'), bus_state,    fallback_bus_state    },
    { PSX_FOURCC('S', 'C', 'H', 'D'), PSX_FOURCC('s', 'c', 'h', '_'), sched_state,  fallback_sched_state  },
    { PSX_FOURCC('I', 'R', 'Q', ' '), PSX_FOURCC('i', 'r', 'q', '_'), irq_state,    fallback_irq_state    },
    { PSX_FOURCC('D', 'M', 'A', ' '), PSX_FOURCC('d', 'm', 'a', '_'), dma_state,    fallback_dma_state    },
    { PSX_FOURCC('T', 'M', 'R', ' '), PSX_FOURCC('t', 'm', 'r', '_'), timers_state, fallback_timers_state },
    { PSX_FOURCC('G', 'P', 'U', ' '), PSX_FOURCC('g', 'p', 'u', '_'), gpu_state,    fallback_gpu_state    },
    { PSX_FOURCC('S', 'P', 'U', ' '), PSX_FOURCC('s', 'p', 'u', '_'), spu_state,    fallback_spu_state    },
    { PSX_FOURCC('C', 'D', 'R', 'M'), PSX_FOURCC('c', 'd', 'r', '_'), cdrom_state,  fallback_cdrom_state  },
    { PSX_FOURCC('M', 'D', 'E', 'C'), PSX_FOURCC('m', 'd', 'c', '_'), mdec_state,   fallback_mdec_state   },
    { PSX_FOURCC('S', 'I', 'O', '0'), PSX_FOURCC('s', 'i', 'o', '_'), sio_state,    fallback_sio_state    },
    { PSX_FOURCC('B', 'I', 'O', 'S'), PSX_FOURCC('b', 'i', 'o', '_'), bios_state,   NULL                  },
    { PSX_FOURCC('S', 'Y', 'S', ' '), PSX_FOURCC('s', 'y', 's', '_'), sys_meta_state, NULL                },
};

PsxResult psx_state_save(PsxSystem *sys, StateIO *st) {
    if (!sys || !st || !st->writing) {
        return PSX_ERR_BAD_ARG;
    }

    /* Ensure hardware GPU renders are flushed back to shadow VRAM before saving */
    if (sys->gpu.be && sys->gpu.be->sync_to_shadow) {
        sys->gpu.be->sync_to_shadow(sys->gpu.be_self, 0u, 0u, PSX_VRAM_W, PSX_VRAM_H);
    }

    u32 hdr_start = st->pos;
    char magic[8] = { 'P', 'S', 'X', '3', 'D', 'S', 'S', '\0' };
    u32 version = PSX_STATE_VERSION;
    u32 payload_len = 0u;
    u32 crc = 0u;

    state_bytes(st, magic, 8u);
    state_u32(st, &version);
    state_u32(st, &payload_len);
    state_u32(st, &crc);
    if (st->error) {
        return PSX_ERR_NOMEM;
    }

    u32 payload_start = st->pos;

    for (size_t i = 0; i < sizeof(k_sections) / sizeof(k_sections[0]); i++) {
        u32 chunk_hdr = st->pos;
        u32 tag = k_sections[i].tag;
        u32 chunk_len = 0u;
        state_u32(st, &tag);
        state_u32(st, &chunk_len);
        if (st->error) {
            return PSX_ERR_NOMEM;
        }

        u32 data_start = st->pos;
        k_sections[i].primary(sys, st);
        if (st->error) {
            return PSX_ERR_NOMEM;
        }

        chunk_len = st->pos - data_start;
        if (chunk_len == 0u && k_sections[i].fallback != NULL) {
            tag = k_sections[i].fb_tag;
            k_sections[i].fallback(sys, st);
            if (st->error) {
                return PSX_ERR_NOMEM;
            }
            chunk_len = st->pos - data_start;
        }

        if (st->buf) {
            memcpy(st->buf + chunk_hdr + 0u, &tag, 4u);
            memcpy(st->buf + chunk_hdr + 4u, &chunk_len, 4u);
        }
    }

    payload_len = st->pos - payload_start;
    if (st->buf) {
        crc = psx_crc32(st->buf + payload_start, payload_len);
        memcpy(st->buf + hdr_start + 12u, &payload_len, 4u);
        memcpy(st->buf + hdr_start + 16u, &crc, 4u);
    }

    return PSX_OK;
}

PsxResult psx_state_load(PsxSystem *sys, StateIO *st) {
    if (!sys || !st || st->writing || !st->buf) {
        return PSX_ERR_BAD_ARG;
    }
    if (st->cap < st->pos || (st->cap - st->pos) < PSX_STATE_HDR_BYTES) {
        st->error = true;
        return PSX_ERR_FORMAT;
    }

    char magic[8] = { 0 };
    u32 version = 0u;
    u32 payload_len = 0u;
    u32 expected_crc = 0u;

    state_bytes(st, magic, 8u);
    state_u32(st, &version);
    state_u32(st, &payload_len);
    state_u32(st, &expected_crc);
    if (st->error) {
        return PSX_ERR_FORMAT;
    }

    const char expected_magic[8] = { 'P', 'S', 'X', '3', 'D', 'S', 'S', '\0' };
    if (memcmp(magic, expected_magic, 8u) != 0 || version != PSX_STATE_VERSION) {
        st->error = true;
        return PSX_ERR_FORMAT;
    }

    if ((st->cap - st->pos) < payload_len) {
        st->error = true;
        return PSX_ERR_FORMAT;
    }

    /* Validate CRC32 across full payload BEFORE mutating live PsxSystem */
    u32 actual_crc = psx_crc32(st->buf + st->pos, payload_len);
    if (actual_crc != expected_crc) {
        st->error = true;
        return PSX_ERR_FORMAT;
    }

    u32 payload_end = st->pos + payload_len;
    while (st->pos < payload_end) {
        if ((payload_end - st->pos) < 8u) {
            st->error = true;
            return PSX_ERR_FORMAT;
        }
        u32 tag = 0u;
        u32 chunk_len = 0u;
        state_u32(st, &tag);
        state_u32(st, &chunk_len);
        if ((payload_end - st->pos) < chunk_len) {
            st->error = true;
            return PSX_ERR_FORMAT;
        }

        StateIO sub = {
            .writing = false,
            .error   = false,
            .buf     = st->buf,
            .pos     = st->pos,
            .cap     = st->pos + chunk_len
        };

        for (size_t i = 0; i < sizeof(k_sections) / sizeof(k_sections[0]); i++) {
            if (tag == k_sections[i].tag) {
                k_sections[i].primary(sys, &sub);
                break;
            }
            if (tag == k_sections[i].fb_tag && k_sections[i].fallback != NULL) {
                k_sections[i].fallback(sys, &sub);
                break;
            }
        }
        if (sub.error) {
            st->error = true;
            return PSX_ERR_FORMAT;
        }
        st->pos += chunk_len;
    }

    /* Rebuild fastmem tables (respecting SR.IsC), flush JIT & GPU caches */
    sys->cpu.sys.p = sys;
    if (sys->bus.ram) {
        sys->cpu.ram_base.p = sys->bus.ram;
    }
    if (sys->bus.scratch_page) {
        sys->cpu.scratch_base.p = sys->bus.scratch_page;
    }
    if (sys->bus.fm_r && sys->bus.fm_w) {
        fastmem_build(&sys->bus);
        bool isc = (sys->cpu.cop0[COP0_SR] & PSX_SR_ISC) != 0u;
        fastmem_set_isolated(&sys->bus, isc);
        sys->cpu.fastmem_r.p = sys->bus.fm_r;
        sys->cpu.fastmem_w.p = (sys->cfg.isc_swap_table && isc && sys->bus.fm_w_isc_null)
                                   ? sys->bus.fm_w_isc_null
                                   : sys->bus.fm_w;
    }
    if (sys->jit.ibtc) {
        sys->cpu.ibtc.p = sys->jit.ibtc;
    }
    if (sys->jit.tab_ram) {
        sys->cpu.blocktab_ram.p = sys->jit.tab_ram;
    }
    if (sys->jit.tab_bios) {
        sys->cpu.blocktab_bios.p = sys->jit.tab_bios;
    }

    jit_flush_all(sys);
    sys->cpu.icache_dirty = 1u;

    if (sys->gpu.be) {
        if (sys->gpu.be->set_draw_env) {
            sys->gpu.be->set_draw_env(sys->gpu.be_self, &sys->gpu.env);
        }
        if (sys->gpu.be->vram_written) {
            sys->gpu.be->vram_written(sys->gpu.be_self, 0u, 0u, PSX_VRAM_W, PSX_VRAM_H);
        }
    }

    sio_set_pad(sys, 0, &sys->pads[0]);
    sio_set_pad(sys, 1, &sys->pads[1]);
    cpu_check_irq(sys);
    cpu_request_exit(sys);
    return PSX_OK;
}

PsxResult psx_state_save_file(PsxSystem *sys, const char *path) {
    if (!sys || !path || path[0] == '\0') {
        return PSX_ERR_BAD_ARG;
    }

    /* Pass 1: exact size calculation */
    StateIO dry = { .writing = true, .error = false, .buf = NULL, .pos = 0u, .cap = UINT32_MAX };
    PsxResult rc = psx_state_save(sys, &dry);
    if (rc != PSX_OK || dry.pos == 0u) {
        return rc != PSX_OK ? rc : PSX_ERR_NOMEM;
    }

    u8 *buf = (u8 *)malloc(dry.pos);
    if (!buf) {
        return PSX_ERR_NOMEM;
    }

    StateIO st = { .writing = true, .error = false, .buf = buf, .pos = 0u, .cap = dry.pos };
    rc = psx_state_save(sys, &st);
    if (rc != PSX_OK) {
        free(buf);
        return rc;
    }

    PlatFile *f = plat_fopen(path, "wb");
    if (!f) {
        free(buf);
        return PSX_ERR_IO;
    }
    u32 written = plat_fwrite(f, buf, st.pos);
    plat_fclose(f);
    free(buf);
    return (written == st.pos) ? PSX_OK : PSX_ERR_IO;
}

PsxResult psx_state_load_file(PsxSystem *sys, const char *path) {
    if (!sys || !path || path[0] == '\0') {
        return PSX_ERR_BAD_ARG;
    }
    PlatFile *f = plat_fopen(path, "rb");
    if (!f) {
        return PSX_ERR_IO;
    }
    u64 fsz = plat_fsize(f);
    if (fsz < PSX_STATE_HDR_BYTES || fsz > (u64)PSX_STATE_MAX_SIZE) {
        plat_fclose(f);
        return PSX_ERR_FORMAT;
    }
    u8 *buf = (u8 *)malloc((size_t)fsz);
    if (!buf) {
        plat_fclose(f);
        return PSX_ERR_NOMEM;
    }
    u32 got = plat_fread(f, buf, (u32)fsz);
    plat_fclose(f);
    if (got != (u32)fsz) {
        free(buf);
        return PSX_ERR_IO;
    }
    StateIO st = { .writing = false, .error = false, .buf = buf, .pos = 0u, .cap = got };
    PsxResult rc = psx_state_load(sys, &st);
    free(buf);
    return rc;
}
