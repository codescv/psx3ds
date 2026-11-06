#include "psx3ds/dma.h"
#include "psx3ds/system.h"

#include <string.h>

#define DMA_CHCR_RW_MASK      0x71770703u
#define DMA_OTC_CHCR_RW_MASK  0x51000000u
#define DMA_OTC_CHCR_FIXED_1  0x00000002u
#define DMA_CHCR_BUSY         (1u << 24)
#define DMA_CHCR_TRIGGER      (1u << 28)
#define DMA_CHCR_CHOPPING     (1u << 8)

#define DICR_LOW_RW_MASK      0x00FF803Fu
#define DICR_FORCE_IRQ        (1u << 15)
#define DICR_MASTER_ENABLE    (1u << 23)
#define DICR_FLAGS_MASK       0x7F000000u
#define DICR_MASTER_FLAG      0x80000000u

PSX_INLINE u32 dma_width_mask(u32 width) {
    if (width == 1u) {
        return 0xFFu;
    }
    if (width == 2u) {
        return 0xFFFFu;
    }
    return 0xFFFFFFFFu;
}

static u32 dma_compute_dicr(u32 raw) {
    u32 low = raw & DICR_LOW_RW_MASK;
    u32 flags = raw & DICR_FLAGS_MASK;
    bool force = (low & DICR_FORCE_IRQ) != 0u;
    bool master_en = (low & DICR_MASTER_ENABLE) != 0u;
    u32 active = (flags >> 24) & (low >> 16) & 0x7Fu;
    u32 master_flag = (force || (master_en && active != 0u)) ? DICR_MASTER_FLAG : 0u;
    return low | flags | master_flag;
}

static void dma_update_dicr_and_irq(PsxSystem *sys, u32 old_dicr, u32 new_raw) {
    u32 next = dma_compute_dicr(new_raw);
    sys->dma.dicr = next;
    if (((old_dicr & DICR_MASTER_FLAG) == 0u) && ((next & DICR_MASTER_FLAG) != 0u)) {
        irq_raise(sys, PSX_IRQ_DMA);
    }
}

static void dma_complete_channel(PsxSystem *sys, u32 ch) {
    PsxDma *d = &sys->dma;
    if (ch == PSX_DMA_CH_OTC) {
        d->ch[ch].chcr = (d->ch[ch].chcr & ~0x11000000u) | DMA_OTC_CHCR_FIXED_1;
    } else {
        d->ch[ch].chcr &= ~0x11000000u;
    }
    d->running_mask &= (u8)~(1u << ch);

    u32 old_dicr = d->dicr;
    u32 new_raw = old_dicr;
    if ((old_dicr & (1u << (16u + ch))) != 0u) {
        new_raw |= (1u << (24u + ch));
    }
    dma_update_dicr_and_irq(sys, old_dicr, new_raw);
}

static u32 dma_word_cost(const PsxSystem *sys, u32 ch, u32 words) {
    if (words == 0u) {
        return 1u;
    }
    if (ch == PSX_DMA_CH_CDROM) {
        /* ~40 cycles per 16 words (2.5 cycles/word) per ARCHITECTURE.md §8.2 */
        u32 cyc = (words * 5u + 1u) >> 1;
        return (cyc > 0u) ? cyc : 1u;
    }
    u32 mult = (sys->cfg.dma_cycles_per_word != 0u) ? sys->cfg.dma_cycles_per_word : 1u;
    u32 cyc = words * mult;
    return (cyc > 0u) ? cyc : 1u;
}

static void dma_xfer_chunk(PsxSystem *sys, u32 ch, u32 madr, u32 words,
                           bool from_ram, bool backward) {
    u32 buf[256];
    u32 rem = words;
    u32 cur = madr & 0x1FFFFCu;

    while (rem > 0u) {
        u32 n = (rem > 256u) ? 256u : rem;
        if (from_ram) {
            if (!backward) {
                bus_dma_read_words(sys, cur, buf, n);
                cur = (cur + n * 4u) & 0x1FFFFCu;
            } else {
                for (u32 i = 0u; i < n; i++) {
                    bus_dma_read_words(sys, cur, &buf[i], 1u);
                    cur = (cur - 4u) & 0x1FFFFCu;
                }
            }
            switch (ch) {
                case PSX_DMA_CH_MDEC_IN:
                    (void)mdec_dma_in_words(sys, buf, n);
                    break;
                case PSX_DMA_CH_GPU:
                    gpu_dma_write_words(sys, buf, n);
                    break;
                case PSX_DMA_CH_SPU:
                    spu_dma_write_words(sys, buf, n);
                    break;
                default:
                    /* PIO / read-only device channels ignore RAM->dev writes */
                    break;
            }
        } else {
            memset(buf, 0, (size_t)n * sizeof(u32));
            switch (ch) {
                case PSX_DMA_CH_MDEC_OUT:
                    (void)mdec_dma_out_words(sys, buf, n);
                    break;
                case PSX_DMA_CH_GPU:
                    (void)gpu_dma_read_words(sys, buf, n);
                    break;
                case PSX_DMA_CH_CDROM:
                    (void)cdrom_dma_read_words(sys, buf, n);
                    break;
                case PSX_DMA_CH_SPU:
                    (void)spu_dma_read_words(sys, buf, n);
                    break;
                case PSX_DMA_CH_PIO:
                    for (u32 i = 0u; i < n; i++) {
                        buf[i] = 0xFFFFFFFFu;
                    }
                    break;
                default:
                    break;
            }
            if (!backward) {
                bus_dma_write_words(sys, cur, buf, n);
                cur = (cur + n * 4u) & 0x1FFFFCu;
            } else {
                for (u32 i = 0u; i < n; i++) {
                    bus_dma_write_words(sys, cur, &buf[i], 1u);
                    cur = (cur - 4u) & 0x1FFFFCu;
                }
            }
        }
        rem -= n;
    }
}

static bool dma_channel_ready(const PsxDma *d, u32 ch) {
    if (((d->dpcr >> (ch * 4u + 3u)) & 1u) == 0u) {
        return false;
    }
    u32 chcr = d->ch[ch].chcr;
    if ((chcr & DMA_CHCR_BUSY) == 0u) {
        return false;
    }
    if ((d->running_mask & (1u << ch)) != 0u) {
        return true;
    }
    u32 sync_mode = (ch == PSX_DMA_CH_OTC) ? 0u : ((chcr >> 9) & 3u);
    if (sync_mode == 0u && (chcr & DMA_CHCR_TRIGGER) == 0u) {
        return false;
    }
    if (sync_mode == 3u) {
        return false;
    }
    return true;
}

/*
 * Executes one step/slice of channel `ch`.
 * Returns 0 if the channel completed immediately, or >0 cycles until the next EV_DMA tick.
 */
static u32 dma_step_channel(PsxSystem *sys, u32 ch) {
    PsxDma *d = &sys->dma;
    DmaChannel *c = &d->ch[ch];
    u32 chcr = c->chcr;

    if (ch == PSX_DMA_CH_OTC) {
        u32 n = c->bcr & 0xFFFFu;
        if (n == 0u) {
            n = 0x10000u;
        }
        bus_dma_fill_desc(sys, c->madr, n);
        dma_complete_channel(sys, ch);
        return 0u;
    }

    u32 sync_mode = (chcr >> 9) & 3u;
    bool from_ram = (chcr & 1u) != 0u;
    bool backward = (chcr & 2u) != 0u;
    s32 step_bytes = backward ? -4 : 4;

    if (sync_mode == 0u) {
        u32 rem = c->bcr & 0xFFFFu;
        if (rem == 0u) {
            rem = 0x10000u;
        }
        if ((chcr & DMA_CHCR_CHOPPING) != 0u) {
            c->chcr &= ~DMA_CHCR_TRIGGER;
            u32 dma_win = 1u << ((chcr >> 16) & 7u);
            u32 cpu_win = 1u << ((chcr >> 20) & 7u);
            u32 chunk = (rem < dma_win) ? rem : dma_win;
            dma_xfer_chunk(sys, ch, c->madr, chunk, from_ram, backward);
            c->madr = (c->madr + (u32)((s32)chunk * step_bytes)) & 0x00FFFFFFu;
            u32 left = rem - chunk;
            c->bcr = (c->bcr & 0xFFFF0000u) | (left & 0xFFFFu);
            if (left == 0u) {
                dma_complete_channel(sys, ch);
                return 0u;
            }
            d->running_mask |= (u8)(1u << ch);
            return dma_word_cost(sys, ch, chunk) + cpu_win;
        }

        dma_xfer_chunk(sys, ch, c->madr, rem, from_ram, backward);
        dma_complete_channel(sys, ch);
        return 0u;
    }

    if (sync_mode == 1u) {
        c->chcr &= ~DMA_CHCR_TRIGGER;
        u32 bs = c->bcr & 0xFFFFu;
        if (bs == 0u) {
            bs = 0x10000u;
        }
        u32 ba = c->bcr >> 16;
        if (ba == 0u) {
            if ((d->running_mask & (1u << ch)) != 0u) {
                dma_complete_channel(sys, ch);
                return 0u;
            }
            ba = 0x10000u;
        }

        u32 max_blocks = 256u / bs;
        if (max_blocks == 0u) {
            max_blocks = 1u;
        }
        u32 blk_chunk = (ba < max_blocks) ? ba : max_blocks;
        u32 words = blk_chunk * bs;

        dma_xfer_chunk(sys, ch, c->madr, words, from_ram, backward);
        c->madr = (c->madr + (u32)((s32)words * step_bytes)) & 0x00FFFFFFu;
        u32 ba_left = ba - blk_chunk;
        c->bcr = ((ba_left & 0xFFFFu) << 16) | (c->bcr & 0xFFFFu);

        if (ba_left == 0u) {
            dma_complete_channel(sys, ch);
            return 0u;
        }
        d->running_mask |= (u8)(1u << ch);
        return dma_word_cost(sys, ch, words);
    }

    if (sync_mode == 2u) {
        if (ch != PSX_DMA_CH_GPU) {
            dma_complete_channel(sys, ch);
            return 0u;
        }
        c->chcr &= ~DMA_CHCR_TRIGGER;

        u32 max_nodes = (sys->cfg.dma_ll_max_nodes_per_tick != 0u)
                            ? sys->cfg.dma_ll_max_nodes_per_tick
                            : 64u;
        u32 nodes = 0u;
        u32 words_sent = 0u;
        u32 buf[256];

        while (nodes < max_nodes && words_sent < 256u) {
            u32 addr = c->madr & 0x1FFFFCu;
            u32 hdr = 0u;
            bus_dma_read_words(sys, addr, &hdr, 1u);
            u32 count = hdr >> 24;
            if (count > 0u) {
                bus_dma_read_words(sys, (addr + 4u) & 0x1FFFFCu, buf, count);
                gpu_dma_write_words(sys, buf, count);
                words_sent += count;
            }
            u32 next = hdr & 0x00FFFFFFu;
            c->madr = next;
            nodes++;

            if ((next & 0x00800000u) != 0u) {
                dma_complete_channel(sys, ch);
                return 0u;
            }
        }

        d->running_mask |= (u8)(1u << ch);
        u32 cost = dma_word_cost(sys, ch, words_sent + nodes);
        if (cost < 16u) {
            cost = 16u;
        }
        return cost;
    }

    dma_complete_channel(sys, ch);
    return 0u;
}

static int dma_pick_highest_ready(const PsxDma *d) {
    int best_ch = -1;
    u32 best_prio = 8u;
    for (u32 ch = 0u; ch < 7u; ch++) {
        if (!dma_channel_ready(d, ch)) {
            continue;
        }
        u32 prio = (d->dpcr >> (ch * 4u)) & 7u;
        if (prio < best_prio || (prio == best_prio && (int)ch > best_ch)) {
            best_prio = prio;
            best_ch = (int)ch;
        }
    }
    return best_ch;
}

static void dma_schedule_or_run(PsxSystem *sys) {
    PsxDma *d = &sys->dma;
    for (;;) {
        int ch = dma_pick_highest_ready(d);
        if (ch < 0) {
            if (d->running_mask == 0u) {
                sched_cancel(sys, EV_DMA);
            }
            return;
        }
        /* If this channel is already awaiting EV_DMA, keep its scheduled tick */
        if ((d->running_mask & (1u << (u32)ch)) != 0u &&
            (sys->sched.active_mask & (1u << EV_DMA)) != 0u) {
            return;
        }
        u32 delay = dma_step_channel(sys, (u32)ch);
        if (delay > 0u) {
            sched_schedule(sys, EV_DMA, delay);
            return;
        }
    }
}

static void dma_event_cb(PsxSystem *sys, u32 late_cycles) {
    (void)late_cycles;
    PsxDma *d = &sys->dma;
    int ch = dma_pick_highest_ready(d);
    if (ch < 0) {
        d->running_mask = 0u;
        return;
    }
    u32 delay = dma_step_channel(sys, (u32)ch);
    if (delay > 0u) {
        sched_schedule(sys, EV_DMA, delay);
        return;
    }
    dma_schedule_or_run(sys);
}

static u32 dma_mmio_read(PsxSystem *sys, u32 addr, u32 width) {
    const PsxDma *d = &sys->dma;
    u32 word_addr = addr & ~3u;
    u32 shift = (addr & 3u) * 8u;
    u32 val = 0u;

    if (word_addr >= PSX_REG_DMA_BASE && word_addr < PSX_REG_DPCR) {
        u32 ch = (word_addr - PSX_REG_DMA_BASE) >> 4;
        u32 reg = (word_addr >> 2) & 3u;
        if (reg == 0u) {
            val = d->ch[ch].madr & 0x00FFFFFFu;
        } else if (reg == 1u) {
            val = d->ch[ch].bcr;
        } else if (reg == 2u) {
            val = d->ch[ch].chcr;
        }
    } else if (word_addr == PSX_REG_DPCR) {
        val = d->dpcr;
    } else if (word_addr == PSX_REG_DICR) {
        val = dma_compute_dicr(d->dicr);
    }

    return (val >> shift) & dma_width_mask(width);
}

static void dma_mmio_write(PsxSystem *sys, u32 addr, u32 v, u32 width) {
    PsxDma *d = &sys->dma;
    u32 word_addr = addr & ~3u;
    u32 shift = (addr & 3u) * 8u;
    u32 wmask = dma_width_mask(width) << shift;
    u32 wval = (v & dma_width_mask(width)) << shift;

    if (word_addr >= PSX_REG_DMA_BASE && word_addr < PSX_REG_DPCR) {
        u32 ch = (word_addr - PSX_REG_DMA_BASE) >> 4;
        u32 reg = (word_addr >> 2) & 3u;
        if (reg == 0u) {
            u32 next = (d->ch[ch].madr & ~wmask) | (wval & wmask);
            d->ch[ch].madr = next & 0x00FFFFFFu;
        } else if (reg == 1u) {
            d->ch[ch].bcr = (d->ch[ch].bcr & ~wmask) | (wval & wmask);
        } else if (reg == 2u) {
            u32 next = (d->ch[ch].chcr & ~wmask) | (wval & wmask);
            if (ch == PSX_DMA_CH_OTC) {
                d->ch[ch].chcr = (next & DMA_OTC_CHCR_RW_MASK) | DMA_OTC_CHCR_FIXED_1;
            } else {
                d->ch[ch].chcr = next & DMA_CHCR_RW_MASK;
            }
            if ((d->ch[ch].chcr & DMA_CHCR_BUSY) == 0u) {
                d->running_mask &= (u8)~(1u << ch);
                if (d->running_mask == 0u) {
                    sched_cancel(sys, EV_DMA);
                }
            }
            dma_schedule_or_run(sys);
        }
        return;
    }

    if (word_addr == PSX_REG_DPCR) {
        d->dpcr = (d->dpcr & ~wmask) | (wval & wmask);
        dma_schedule_or_run(sys);
        return;
    }

    if (word_addr == PSX_REG_DICR) {
        u32 old_dicr = d->dicr;
        u32 low_mask = wmask & DICR_LOW_RW_MASK;
        u32 new_low = (old_dicr & DICR_LOW_RW_MASK & ~low_mask) | (wval & low_mask);
        u32 ack_flags = wval & wmask & DICR_FLAGS_MASK;
        u32 new_flags = (old_dicr & DICR_FLAGS_MASK) & ~ack_flags;
        dma_update_dicr_and_irq(sys, old_dicr, new_low | new_flags);
    }
}

void dma_init(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    bus_mmio_register(sys, PSX_REG_DMA_BASE, 0x80u, dma_mmio_read, dma_mmio_write);
    sched_set_handler(&sys->sched, EV_DMA, dma_event_cb);
    dma_reset(sys);
}

void dma_reset(PsxSystem *sys) {
    if (!sys) {
        return;
    }
    PsxDma *d = &sys->dma;
    memset(d, 0, sizeof(*d));
    d->dpcr = PSX_DPCR_DEFAULT;
    d->dicr = 0u;
    d->running_mask = 0u;
    d->ch[PSX_DMA_CH_OTC].chcr = DMA_OTC_CHCR_FIXED_1;
}

void dma_state(PsxSystem *sys, StateIO *st) {
    if (!sys || !st) {
        return;
    }
    PsxDma *d = &sys->dma;
    for (u32 i = 0u; i < 7u; i++) {
        state_u32(st, &d->ch[i].madr);
        state_u32(st, &d->ch[i].bcr);
        state_u32(st, &d->ch[i].chcr);
    }
    state_u32(st, &d->dpcr);
    state_u32(st, &d->dicr);
    state_u8(st, &d->running_mask);
    if (!st->writing) {
        d->ch[PSX_DMA_CH_OTC].chcr =
            (d->ch[PSX_DMA_CH_OTC].chcr & DMA_OTC_CHCR_RW_MASK) | DMA_OTC_CHCR_FIXED_1;
        d->dicr = dma_compute_dicr(d->dicr);
    }
}

void dma_shutdown(PsxSystem *sys) {
    (void)sys;
}
