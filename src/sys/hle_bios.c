#include "psx3ds/system.h"

#include <stdio.h>
#include <string.h>

/*
 * Standalone BIOS-less HLE Kernel & Vector Stubs (ARCHITECTURE.md §9).
 *
 * Allows PS-X EXEs to boot, handle SYSCALLs (Enter/ExitCriticalSection),
 * hardware interrupts (VBlank, Timers, HookEntryInt, ReturnFromException),
 * kernel events (Open/Enable/Deliver/Test/WaitEvent), libc/string/heap
 * helpers, and formatted printf TTY output even when no BIOS file exists!
 */

#define HLE_EVENT_SLOTS      32u
#define HLE_EV_FREE          0u
#define HLE_EV_DISABLED      0x1000u
#define HLE_EV_ENABLED       0x2000u
#define HLE_EV_DELIVERED     0x4000u
#define HLE_EV_MODE_CB       0x0001u
#define HLE_EV_MODE_NOCB     0x0002u

/* Kernel scratch slot at 0x000004F0 preserves $t1 across 0x80000080 -> 0xC0 */
#define HLE_SCRATCH_T1_OFF   0x04F0u
#define HLE_IDLE_LOOP_ADDR   0x80001000u

static inline void hle_put_le32(u8 *dst, u32 val) {
    memcpy(dst, &val, sizeof(u32));
}

static inline u32 hle_get_le32(const u8 *src) {
    u32 val = 0u;
    memcpy(&val, src, sizeof(u32));
    return val;
}

static u8 hle_read8(const PsxSystem *sys, u32 vaddr) {
    u32 phys = vaddr & 0x1FFFFFFFu;
    if (phys < PSX_RAM_MIRROR_END && sys->bus.ram) {
        return sys->bus.ram[phys & PSX_RAM_MASK];
    }
    if (phys >= PSX_SCRATCH_PHYS && phys < (PSX_SCRATCH_PHYS + PSX_SCRATCH_SIZE) &&
        vaddr < 0xA0000000u && sys->bus.scratch_page) {
        return sys->bus.scratch_page[phys & PSX_SCRATCH_MASK];
    }
    if (phys >= PSX_BIOS_PHYS && phys < (PSX_BIOS_PHYS + PSX_BIOS_SIZE) && sys->bus.bios) {
        return sys->bus.bios[phys & PSX_BIOS_MASK];
    }
    return 0u;
}

static u32 hle_read32(const PsxSystem *sys, u32 vaddr) {
    u32 b0 = hle_read8(sys, vaddr + 0u);
    u32 b1 = hle_read8(sys, vaddr + 1u);
    u32 b2 = hle_read8(sys, vaddr + 2u);
    u32 b3 = hle_read8(sys, vaddr + 3u);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static void hle_write8(PsxSystem *sys, u32 vaddr, u8 val) {
    u32 phys = vaddr & 0x1FFFFFFFu;
    if (phys < PSX_RAM_MIRROR_END && sys->bus.ram) {
        u32 off = phys & PSX_RAM_MASK;
        if (sys->bus.ram[off] != val) {
            sys->bus.ram[off] = val;
            if (sys->bus.code_page_refs[off >> 12] != 0u) {
                jit_invalidate_page(sys, off >> 12);
            }
        }
        return;
    }
    if (phys >= PSX_SCRATCH_PHYS && phys < (PSX_SCRATCH_PHYS + PSX_SCRATCH_SIZE) &&
        vaddr < 0xA0000000u && sys->bus.scratch_page) {
        sys->bus.scratch_page[phys & PSX_SCRATCH_MASK] = val;
    }
}

static void hle_write16(PsxSystem *sys, u32 vaddr, u16 val) {
    hle_write8(sys, vaddr + 0u, (u8)(val & 0xFFu));
    hle_write8(sys, vaddr + 1u, (u8)((val >> 8) & 0xFFu));
}

static void hle_write32(PsxSystem *sys, u32 vaddr, u32 val) {
    u32 phys = vaddr & 0x1FFFFFFFu;
    if ((vaddr & 3u) == 0u && phys < PSX_RAM_MIRROR_END && sys->bus.ram) {
        u32 off = phys & PSX_RAM_MASK;
        u32 prev = hle_get_le32(sys->bus.ram + off);
        if (prev != val) {
            hle_put_le32(sys->bus.ram + off, val);
            if (sys->bus.code_page_refs[off >> 12] != 0u) {
                jit_invalidate_page(sys, off >> 12);
            }
        }
        return;
    }
    hle_write8(sys, vaddr + 0u, (u8)(val & 0xFFu));
    hle_write8(sys, vaddr + 1u, (u8)((val >> 8) & 0xFFu));
    hle_write8(sys, vaddr + 2u, (u8)((val >> 16) & 0xFFu));
    hle_write8(sys, vaddr + 3u, (u8)((val >> 24) & 0xFFu));
}

void hle_bios_install_rom_stub(PsxSystem *sys) {
    if (!sys || !sys->bus.bios) {
        return;
    }
    memset(sys->bus.bios, 0, PSX_BIOS_SIZE);

    /*
     * 0xBFC00000 Reset Bootstrap Stub:
     *   lui   $sp, 0x801F
     *   ori   $sp, $sp, 0xFF00
     *   lui   $k0, 0x8003       ; jump to 0x80030000 (triggers psexe_pending or idle loop)
     *   jr    $k0
     *   nop
     */
    const u32 reset_stub[] = {
        0x3C1D801Fu, /* lui $sp, 0x801F       */
        0x37BDFF00u, /* ori $sp, $sp, 0xFF00  */
        0x3C1A8003u, /* lui $k0, 0x8003       */
        0x03400008u, /* jr  $k0               */
        0x00000000u  /* nop                   */
    };
    for (size_t i = 0; i < sizeof(reset_stub) / sizeof(reset_stub[0]); i++) {
        hle_put_le32(sys->bus.bios + i * 4u, reset_stub[i]);
    }

    /*
     * 0xBFC00180 ROM Exception Vector Trampoline (in case SR.BEV == 1):
     *   sw    $t1, 0x04F0($zero)
     *   lui   $k0, 0x8000
     *   ori   $k0, $k0, 0x00C0
     *   jr    $k0
     *   addiu $t1, $zero, 0x00FF
     */
    const u32 rom_exc_stub[] = {
        0xAC0904F0u, /* sw    $t1, 0x04F0($zero) */
        0x3C1A8000u, /* lui   $k0, 0x8000        */
        0x375A00C0u, /* ori   $k0, $k0, 0x00C0   */
        0x03400008u, /* jr    $k0                */
        0x240900FFu  /* addiu $t1, $zero, 0x00FF */
    };
    for (size_t i = 0; i < sizeof(rom_exc_stub) / sizeof(rom_exc_stub[0]); i++) {
        hle_put_le32(sys->bus.bios + 0x0180u + i * 4u, rom_exc_stub[i]);
    }
}

void hle_bios_install_ram_vectors(PsxSystem *sys) {
    if (!sys || !sys->bus.ram) {
        return;
    }

    /*
     * 0x80000080 RAM Exception Vector:
     *   sw    $t1, 0x04F0($zero)  ; save user $t1 with zero register clobber
     *   j     0x800000C0          ; jump to pinned C0 vector hook
     *   addiu $t1, $zero, 0x00FF  ; HLE exception tag in delay slot
     */
    hle_write32(sys, 0x80000080u, 0xAC0904F0u);
    hle_write32(sys, 0x80000084u, 0x08000030u);
    hle_write32(sys, 0x80000088u, 0x240900FFu);
    hle_write32(sys, 0x8000008Cu, 0x00000000u);

    /* 0xA0 / 0xB0 / 0xC0 fallback `jr $ra; nop` instructions */
    const u32 vecs[3] = { 0x800000A0u, 0x800000B0u, 0x800000C0u };
    for (int i = 0; i < 3; i++) {
        hle_write32(sys, vecs[i] + 0u, 0x03E00008u); /* jr $ra */
        hle_write32(sys, vecs[i] + 4u, 0x00000000u); /* nop    */
    }

    /* 0x80001000 & 0x80030000 safe self-loop (`1: b 1b; nop`, eligible for JIT idle-skip) */
    hle_write32(sys, HLE_IDLE_LOOP_ADDR + 0u, 0x1000FFFFu);
    hle_write32(sys, HLE_IDLE_LOOP_ADDR + 4u, 0x00000000u);
    if (!sys->bios.psexe_pending) {
        hle_write32(sys, PSX_SHELL_PC + 0u, 0x1000FFFFu);
        hle_write32(sys, PSX_SHELL_PC + 4u, 0x00000000u);
    }

    if (sys->bios.hle_heap_ptr < 0x80010000u) {
        sys->bios.hle_heap_ptr = 0x80180000u;
    }
}

static void hle_return_to_ra(PsxSystem *sys) {
    PsxCpu *c = &sys->cpu;
    if (c->load_delay_reg != 0u) {
        if (c->load_delay_reg < 32u) {
            c->gpr[c->load_delay_reg] = c->load_delay_val;
        }
        c->load_delay_reg = 0u;
    }
    c->in_delay_slot = 0u;
    c->branch_taken = 0u;
    c->gpr[0] = 0u;
    c->pc = c->gpr[31];
    c->next_pc = c->pc + 4u;
}

static u32 hle_printf_arg(const PsxSystem *sys, u32 idx) {
    if (idx < 3u) {
        return sys->cpu.gpr[5u + idx]; /* $a1, $a2, $a3 */
    }
    u32 sp = sys->cpu.gpr[29];
    return hle_read32(sys, sp + (idx + 1u) * 4u); /* sp+16, sp+20, ... */
}

static u32 hle_format_printf(PsxSystem *sys, u32 fmt_addr) {
    u32 arg_idx = 0u;
    u32 emitted = 0u;

    for (u32 i = 0; i < 4096u; i++) {
        char ch = (char)hle_read8(sys, fmt_addr + i);
        if (ch == '\0') {
            break;
        }
        if (ch != '%') {
            bios_tty_putc(sys, ch);
            emitted++;
            continue;
        }

        i++;
        ch = (char)hle_read8(sys, fmt_addr + i);
        if (ch == '\0') {
            break;
        }
        if (ch == '%') {
            bios_tty_putc(sys, '%');
            emitted++;
            continue;
        }

        /* Flags: '-', '+', ' ', '0', '#' */
        bool left_align = false;
        bool zero_pad = false;
        while (ch == '-' || ch == '+' || ch == ' ' || ch == '0' || ch == '#') {
            if (ch == '-') {
                left_align = true;
            } else if (ch == '0') {
                zero_pad = true;
            }
            i++;
            ch = (char)hle_read8(sys, fmt_addr + i);
        }

        /* Width */
        int width = 0;
        if (ch == '*') {
            width = (int)hle_printf_arg(sys, arg_idx++);
            if (width < 0) {
                left_align = true;
                width = -width;
            }
            i++;
            ch = (char)hle_read8(sys, fmt_addr + i);
        } else {
            while (ch >= '0' && ch <= '9') {
                width = width * 10 + (ch - '0');
                i++;
                ch = (char)hle_read8(sys, fmt_addr + i);
            }
        }
        if (width > 128) {
            width = 128;
        }

        /* Precision */
        int prec = -1;
        if (ch == '.') {
            prec = 0;
            i++;
            ch = (char)hle_read8(sys, fmt_addr + i);
            if (ch == '*') {
                prec = (int)hle_printf_arg(sys, arg_idx++);
                i++;
                ch = (char)hle_read8(sys, fmt_addr + i);
            } else {
                while (ch >= '0' && ch <= '9') {
                    prec = prec * 10 + (ch - '0');
                    i++;
                    ch = (char)hle_read8(sys, fmt_addr + i);
                }
            }
        }

        /* Length modifier ('l', 'h') */
        while (ch == 'l' || ch == 'h') {
            i++;
            ch = (char)hle_read8(sys, fmt_addr + i);
        }

        char num_buf[160];
        num_buf[0] = '\0';

        if (ch == 's') {
            u32 s_addr = hle_printf_arg(sys, arg_idx++);
            char s_tmp[256];
            u32 slen = 0u;
            if (s_addr == 0u) {
                memcpy(s_tmp, "(null)", 7);
                slen = 6u;
            } else {
                u32 max_s = (prec >= 0 && prec < 255) ? (u32)prec : 255u;
                while (slen < max_s) {
                    u8 sc = hle_read8(sys, s_addr + slen);
                    if (sc == 0u) {
                        break;
                    }
                    s_tmp[slen++] = (char)sc;
                }
                s_tmp[slen] = '\0';
            }
            int pad = width - (int)slen;
            if (!left_align) {
                while (pad-- > 0) {
                    bios_tty_putc(sys, ' ');
                    emitted++;
                }
            }
            for (u32 k = 0; k < slen; k++) {
                bios_tty_putc(sys, s_tmp[k]);
                emitted++;
            }
            if (left_align) {
                while (pad-- > 0) {
                    bios_tty_putc(sys, ' ');
                    emitted++;
                }
            }
            continue;
        } else if (ch == 'c') {
            char out_c = (char)(hle_printf_arg(sys, arg_idx++) & 0xFFu);
            bios_tty_putc(sys, out_c);
            emitted++;
            continue;
        } else if (ch == 'd' || ch == 'i') {
            s32 v = (s32)hle_printf_arg(sys, arg_idx++);
            snprintf(num_buf, sizeof(num_buf), left_align ? "%-*d" : (zero_pad ? "%0*d" : "%*d"), width, (int)v);
        } else if (ch == 'u') {
            u32 v = hle_printf_arg(sys, arg_idx++);
            snprintf(num_buf, sizeof(num_buf), left_align ? "%-*u" : (zero_pad ? "%0*u" : "%*u"), width, (unsigned)v);
        } else if (ch == 'x' || ch == 'p') {
            u32 v = hle_printf_arg(sys, arg_idx++);
            snprintf(num_buf, sizeof(num_buf), left_align ? "%-*x" : (zero_pad ? "%0*x" : "%*x"), width, (unsigned)v);
        } else if (ch == 'X') {
            u32 v = hle_printf_arg(sys, arg_idx++);
            snprintf(num_buf, sizeof(num_buf), left_align ? "%-*X" : (zero_pad ? "%0*X" : "%*X"), width, (unsigned)v);
        } else {
            bios_tty_putc(sys, ch);
            emitted++;
            continue;
        }

        for (size_t k = 0; num_buf[k] != '\0'; k++) {
            bios_tty_putc(sys, num_buf[k]);
            emitted++;
        }
    }
    return emitted;
}

static void hle_deliver_matching_events(PsxSystem *sys, u32 ev_class, u32 ev_spec) {
    PsxBios *b = &sys->bios;
    for (u32 i = 0; i < HLE_EVENT_SLOTS; i++) {
        /* [0]=class, [1]=spec|(mode<<16), [2]=status, [3]=callback */
        u32 cls = b->hle_events[i][0];
        u32 spec = b->hle_events[i][1] & 0xFFFFu;
        u32 mode = b->hle_events[i][1] >> 16;
        u32 st = b->hle_events[i][2];
        if (st == HLE_EV_ENABLED && cls == ev_class && spec == ev_spec) {
            if (mode == HLE_EV_MODE_NOCB || b->hle_events[i][3] == 0u) {
                b->hle_events[i][2] = HLE_EV_DELIVERED;
            }
        }
    }
}

static void hle_update_pad_buffers(PsxSystem *sys) {
    PsxBios *b = &sys->bios;
    if (b->hle_pad_buf1_addr != 0u) {
        if (sys->pads[0].connected) {
            hle_write8(sys, b->hle_pad_buf1_addr + 0u, 0x00u); /* status OK */
            hle_write8(sys, b->hle_pad_buf1_addr + 1u, 0x41u); /* digital pad ID */
            hle_write16(sys, b->hle_pad_buf1_addr + 2u, sys->pads[0].buttons);
        } else {
            hle_write8(sys, b->hle_pad_buf1_addr + 0u, 0xFFu);
        }
    }
    if (b->hle_pad_dest_addr != 0u) {
        /* B0:15 legacy pad driver: raw active-low buttons, pad1 = bits 0-15, pad2 = 16-31 */
        u32 p1 = sys->pads[0].connected ? sys->pads[0].buttons : 0xFFFFu;
        u32 p2 = sys->pads[1].connected ? sys->pads[1].buttons : 0xFFFFu;
        hle_write32(sys, b->hle_pad_dest_addr, (p2 << 16) | (p1 & 0xFFFFu));
    }
    if (b->hle_pad_buf2_addr != 0u) {
        if (sys->pads[1].connected) {
            hle_write8(sys, b->hle_pad_buf2_addr + 0u, 0x00u);
            hle_write8(sys, b->hle_pad_buf2_addr + 1u, 0x41u);
            hle_write16(sys, b->hle_pad_buf2_addr + 2u, sys->pads[1].buttons);
        } else {
            hle_write8(sys, b->hle_pad_buf2_addr + 0u, 0xFFu);
        }
    }
}

static void hle_handle_exception(PsxSystem *sys) {
    PsxCpu *c = &sys->cpu;
    PsxBios *b = &sys->bios;

    /* Restore user $t1 saved by `sw $t1, 0x04F0($zero)` at 0x80000080 */
    if (sys->bus.ram) {
        c->gpr[9] = hle_get_le32(sys->bus.ram + HLE_SCRATCH_T1_OFF);
    }

    u32 sr = c->cop0[COP0_SR];
    u32 cause = c->cop0[COP0_CAUSE];
    u32 epc = c->cop0[COP0_EPC];
    u32 exc_code = (cause >> PSX_CAUSE_EXCCODE_SHIFT) & 0x1Fu;

    if (exc_code == PSX_EXC_SYS) {
        u32 fn = c->gpr[4];
        u32 ret_pc = (cause & PSX_CAUSE_BD) ? c->cop0[COP0_JUMPDEST] : (epc + 4u);
        u32 rfe_sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);

        switch (fn) {
            case 0x01u: { /* EnterCriticalSection */
                bool prev_on = ((sr & PSX_SR_IEP) != 0u) && ((sr & 0x0400u) != 0u);
                c->gpr[2] = prev_on ? 1u : 0u;
                rfe_sr &= ~(PSX_SR_IEC | 0x0400u);
                break;
            }
            case 0x02u: /* ExitCriticalSection */
                rfe_sr |= (PSX_SR_IEC | 0x0400u);
                break;
            case 0x04u: /* DeliverEvent(ev, spec) via syscall */
                hle_deliver_matching_events(sys, c->gpr[5], c->gpr[6] & 0xFFFFu);
                break;
            default:
                break;
        }

        cpu_set_sr(sys, rfe_sr);
        c->in_delay_slot = 0u;
        c->branch_taken = 0u;
        c->pc = ret_pc;
        c->next_pc = ret_pc + 4u;
        return;
    }

    if (exc_code == PSX_EXC_INT) {
        u32 pending = sys->irq.stat & sys->irq.mask;

        if (pending & (1u << PSX_IRQ_VBLANK)) {
            hle_update_pad_buffers(sys);
            hle_deliver_matching_events(sys, 0xF0000001u, 0x0002u);
            hle_deliver_matching_events(sys, 0xF2000003u, 0x0002u);
            if (b->hle_pad_auto_ack) {
                sys->irq.stat &= ~(1u << PSX_IRQ_VBLANK);
            }
        }
        if (pending & (1u << PSX_IRQ_TMR0)) {
            hle_deliver_matching_events(sys, 0xF2000000u, 0x0002u);
            if (b->hle_rcnt_auto_ack & (1u << 0)) {
                sys->irq.stat &= ~(1u << PSX_IRQ_TMR0);
            }
        }
        if (pending & (1u << PSX_IRQ_TMR1)) {
            hle_deliver_matching_events(sys, 0xF2000001u, 0x0002u);
            if (b->hle_rcnt_auto_ack & (1u << 1)) {
                sys->irq.stat &= ~(1u << PSX_IRQ_TMR1);
            }
        }
        if (pending & (1u << PSX_IRQ_TMR2)) {
            hle_deliver_matching_events(sys, 0xF2000002u, 0x0002u);
            if (b->hle_rcnt_auto_ack & (1u << 2)) {
                sys->irq.stat &= ~(1u << PSX_IRQ_TMR2);
            }
        }

        /* Dispatch user HookEntryInt jmp_buf if installed and not already nested */
        if (b->hle_hook_entry_int != 0u && !b->hle_in_intr_hook) {
            b->hle_in_intr_hook = true;
            memcpy(b->hle_saved_gpr, c->gpr, sizeof(c->gpr));
            b->hle_saved_hi = c->hi;
            b->hle_saved_lo = c->lo;
            b->hle_saved_epc = epc;
            b->hle_saved_sr = sr;

            u32 jb = b->hle_hook_entry_int;
            u32 hook_pc = hle_read32(sys, jb + 0x00u);
            if (hook_pc != 0u && (hook_pc & 3u) == 0u) {
                c->gpr[29] = hle_read32(sys, jb + 0x04u); /* sp */
                c->gpr[30] = hle_read32(sys, jb + 0x08u); /* fp */
                for (u32 r = 0; r < 8u; r++) {
                    c->gpr[16u + r] = hle_read32(sys, jb + 0x0Cu + r * 4u); /* s0..s7 */
                }
                c->gpr[28] = hle_read32(sys, jb + 0x2Cu); /* gp */
                c->gpr[2] = 1u;
                /* Point $ra to B0:0x17 ReturnFromException stub if hook returns normally */
                c->in_delay_slot = 0u;
                c->branch_taken = 0u;
                c->pc = hook_pc;
                c->next_pc = hook_pc + 4u;
                cpu_check_irq(sys);
                return;
            }
            b->hle_in_intr_hook = false;
        }

        /* Default kernel IRQ handler acknowledges handled lines to avoid IRQ storm */
        sys->irq.stat &= ~pending;
        u32 rfe_sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
        cpu_set_sr(sys, rfe_sr);
        c->in_delay_slot = 0u;
        c->branch_taken = 0u;
        c->pc = epc;
        c->next_pc = epc + 4u;
        return;
    }

    /* Other exceptions (BREAK / unhandled): skip faulting insn and RFE */
    u32 rfe_sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
    cpu_set_sr(sys, rfe_sr);
    c->in_delay_slot = 0u;
    c->branch_taken = 0u;
    c->pc = epc + 4u;
    c->next_pc = epc + 8u;
}

static void hle_note_unimplemented(u32 vec, u32 fn);

/* --------------------------------------------------------------------------
 * HLE "cdrom:" file I/O on top of the ISO9660 reader (src/sys/cdboot.c, v0.2.2)
 * ------------------------------------------------------------------------ */
#define HLE_FD_BASE 3u /* fds 0..1 are tty; cdrom handles are 3..10 */

static void hle_read_cstr(const PsxSystem *sys, u32 addr, char *out, u32 cap) {
    u32 i = 0u;
    for (; i + 1u < cap; i++) {
        u8 ch = hle_read8(sys, addr + i);
        if (ch == 0u) {
            break;
        }
        out[i] = (char)ch;
    }
    out[i] = '\0';
}

static bool hle_is_cdrom_path(const char *p) {
    return (p[0] == 'c' || p[0] == 'C') && (p[1] == 'd' || p[1] == 'D') &&
           (p[2] == 'r' || p[2] == 'R') && (p[3] == 'o' || p[3] == 'O') &&
           (p[4] == 'm' || p[4] == 'M');
}

static PsxHleFile *hle_fd(PsxSystem *sys, u32 fd) {
    if (fd < HLE_FD_BASE || fd >= HLE_FD_BASE + PSX_HLE_MAX_FDS) {
        return NULL;
    }
    PsxHleFile *f = &sys->bios.hle_files[fd - HLE_FD_BASE];
    return f->used ? f : NULL;
}

static u32 hle_file_open(PsxSystem *sys, u32 name_addr) {
    char name[128];
    hle_read_cstr(sys, name_addr, name, sizeof(name));
    u32 lba = 0u;
    u32 size = 0u;
    if (!hle_is_cdrom_path(name) || !psx_cdboot_find_file(sys, name, &lba, &size)) {
        plat_log("[hle] open('%s') failed\n", name);
        return 0xFFFFFFFFu;
    }
    for (u32 i = 0; i < PSX_HLE_MAX_FDS; i++) {
        PsxHleFile *f = &sys->bios.hle_files[i];
        if (!f->used) {
            f->used = true;
            f->lba = lba;
            f->size = size;
            f->pos = 0u;
            plat_log("[hle] open('%s') -> fd %u (lba=%u size=%u)\n", name,
                     (unsigned)(HLE_FD_BASE + i), (unsigned)lba, (unsigned)size);
            return HLE_FD_BASE + i;
        }
    }
    return 0xFFFFFFFFu;
}

static u32 hle_file_read(PsxSystem *sys, u32 fd, u32 dst, u32 len) {
    PsxHleFile *f = hle_fd(sys, fd);
    if (!f) {
        return 0xFFFFFFFFu;
    }
    if (f->pos >= f->size) {
        return 0u;
    }
    if (len > f->size - f->pos) {
        len = f->size - f->pos;
    }
    u8 buf[2048];
    u32 done = 0u;
    while (done < len) {
        u32 n = len - done > sizeof(buf) ? (u32)sizeof(buf) : len - done;
        if (!psx_cdboot_read_data(sys, f->lba, f->pos + done, buf, n)) {
            break;
        }
        for (u32 i = 0; i < n; i++) {
            hle_write8(sys, dst + done + i, buf[i]);
        }
        done += n;
    }
    f->pos += done;
    return done;
}

static u32 hle_file_lseek(PsxSystem *sys, u32 fd, u32 off, u32 whence) {
    PsxHleFile *f = hle_fd(sys, fd);
    if (!f) {
        return 0xFFFFFFFFu;
    }
    f->pos = (whence == 1u) ? f->pos + off : off;
    return f->pos;
}

static u32 hle_file_close(PsxSystem *sys, u32 fd) {
    PsxHleFile *f = hle_fd(sys, fd);
    if (!f) {
        return 0xFFFFFFFFu;
    }
    f->used = false;
    return fd;
}

/* A0:42 Load(filename, headerbuf): load PS-X EXE from cdrom:, fill 0x3C-byte exec header */
static bool hle_load_exe(PsxSystem *sys, u32 name_addr, u32 hdrbuf) {
    char name[128];
    hle_read_cstr(sys, name_addr, name, sizeof(name));
    u32 lba = 0u;
    u32 size = 0u;
    PsxExeHeader h;
    if (!psx_cdboot_find_file(sys, name, &lba, &size) ||
        psx_cdboot_load_exe(sys, lba, size, &h) != PSX_OK) {
        plat_log("[hle] Load('%s') failed\n", name);
        return false;
    }
    plat_log("[hle] Load('%s') pc0=0x%08X t_addr=0x%08X t_size=0x%X\n", name,
             (unsigned)h.pc0, (unsigned)h.t_addr, (unsigned)h.t_size);
    if (hdrbuf != 0u) {
        const u32 words[10] = { h.pc0,    h.gp0,    h.t_addr, h.t_size, h.d_addr,
                                h.d_size, h.b_addr, h.b_size, h.s_addr, h.s_size };
        for (u32 i = 0; i < 10u; i++) {
            hle_write32(sys, hdrbuf + i * 4u, words[i]);
        }
        for (u32 i = 10u; i < 15u; i++) {
            hle_write32(sys, hdrbuf + i * 4u, 0u);
        }
    }
    return true;
}

/* A0:43 Exec(headerbuf, param1, param2) */
static void hle_exec_header(PsxSystem *sys, u32 hdrbuf, u32 argc, u32 argv) {
    PsxExeHeader h;
    memset(&h, 0, sizeof(h));
    h.pc0 = hle_read32(sys, hdrbuf + 0x00u);
    h.gp0 = hle_read32(sys, hdrbuf + 0x04u);
    h.s_addr = hle_read32(sys, hdrbuf + 0x20u);
    h.s_size = hle_read32(sys, hdrbuf + 0x24u);
    u32 sp = (h.s_addr != 0u) ? (h.s_addr + h.s_size) : 0x801FFF00u;
    psx_exe_apply_entry(sys, &h, sp);
    sys->cpu.gpr[4] = argc;
    sys->cpu.gpr[5] = argv;
}

static void hle_dispatch_a0(PsxSystem *sys, u32 fn) {
    PsxCpu *c = &sys->cpu;
    PsxBios *b = &sys->bios;
    u32 a0 = c->gpr[4];
    u32 a1 = c->gpr[5];
    u32 a2 = c->gpr[6];

    switch (fn) {
        case 0x00u: /* open(filename, accessmode) */
            c->gpr[2] = hle_file_open(sys, a0);
            break;
        case 0x01u: /* lseek(fd, offset, seektype) */
            c->gpr[2] = hle_file_lseek(sys, a0, a1, a2);
            break;
        case 0x02u: /* read(fd, dst, length) */
            c->gpr[2] = hle_file_read(sys, a0, a1, a2);
            break;
        case 0x04u: /* close(fd) */
            c->gpr[2] = hle_file_close(sys, a0);
            break;
        case 0x03u: case 0x05u: case 0x06u: /* write / ioctl / exit on files */
            c->gpr[2] = 0xFFFFFFFFu;
            break;
        case 0x42u: /* Load(filename, headerbuf) */
            c->gpr[2] = hle_load_exe(sys, a0, a1) ? 1u : 0u;
            break;
        case 0x43u: /* Exec(headerbuf, param1, param2) -- does not return */
            hle_exec_header(sys, a0, a1, a2);
            return;
        case 0x51u: { /* LoadExec(filename, stackbase, stackoffset) -- does not return */
            u32 hdr = 0x801FFE00u; /* scratch exec header below the default stack */
            if (hle_load_exe(sys, a0, hdr)) {
                if (a1 != 0u) {
                    hle_write32(sys, hdr + 0x20u, a1);
                    hle_write32(sys, hdr + 0x24u, a2);
                }
                hle_exec_header(sys, hdr, 1u, 0u);
                return;
            }
            c->gpr[2] = 0u;
            break;
        }
        case 0xA5u: { /* CdReadSector(count, sector, buffer) */
            u32 done = 0u;
            u8 sec[2048];
            for (; done < a0; done++) {
                if (!psx_cdboot_read_data(sys, a1 + done, 0u, sec, sizeof(sec))) {
                    break;
                }
                for (u32 i = 0; i < sizeof(sec); i++) {
                    hle_write8(sys, a2 + done * 2048u + i, sec[i]);
                }
            }
            c->gpr[2] = done == a0 ? a0 : 0xFFFFFFFFu;
            break;
        }
        case 0xA6u: /* CdGetStatus */
            c->gpr[2] = 0x02u; /* motor on, shell closed */
            break;

        case 0x13u: { /* setjmp(jmp_buf) */
            hle_write32(sys, a0 + 0x00u, c->gpr[31]); /* ra */
            hle_write32(sys, a0 + 0x04u, c->gpr[29]); /* sp */
            hle_write32(sys, a0 + 0x08u, c->gpr[30]); /* fp */
            for (u32 r = 0; r < 8u; r++) {
                hle_write32(sys, a0 + 0x0Cu + r * 4u, c->gpr[16u + r]);
            }
            hle_write32(sys, a0 + 0x2Cu, c->gpr[28]); /* gp */
            c->gpr[2] = 0u;
            break;
        }
        case 0x14u: { /* longjmp(jmp_buf, val) */
            u32 ra = hle_read32(sys, a0 + 0x00u);
            c->gpr[29] = hle_read32(sys, a0 + 0x04u);
            c->gpr[30] = hle_read32(sys, a0 + 0x08u);
            for (u32 r = 0; r < 8u; r++) {
                c->gpr[16u + r] = hle_read32(sys, a0 + 0x0Cu + r * 4u);
            }
            c->gpr[28] = hle_read32(sys, a0 + 0x2Cu);
            c->gpr[31] = ra;
            c->gpr[2] = (a1 == 0u) ? 1u : a1;
            break;
        }
        case 0x15u: { /* strcat */
            u32 d = a0;
            while (hle_read8(sys, d) != 0u) {
                d++;
            }
            u32 s = a1;
            for (;;) {
                u8 ch = hle_read8(sys, s++);
                hle_write8(sys, d++, ch);
                if (ch == 0u) {
                    break;
                }
            }
            c->gpr[2] = a0;
            break;
        }
        case 0x16u: { /* strncat */
            u32 d = a0;
            while (hle_read8(sys, d) != 0u) {
                d++;
            }
            for (u32 i = 0; i < a2; i++) {
                u8 ch = hle_read8(sys, a1 + i);
                if (ch == 0u) {
                    break;
                }
                hle_write8(sys, d++, ch);
            }
            hle_write8(sys, d, 0u);
            c->gpr[2] = a0;
            break;
        }
        case 0x17u: { /* strcmp */
            u32 i = 0u;
            for (;;) {
                u8 c1 = hle_read8(sys, a0 + i);
                u8 c2 = hle_read8(sys, a1 + i);
                if (c1 != c2 || c1 == 0u) {
                    c->gpr[2] = (u32)((s32)c1 - (s32)c2);
                    break;
                }
                i++;
            }
            break;
        }
        case 0x18u: { /* strncmp */
            c->gpr[2] = 0u;
            for (u32 i = 0; i < a2; i++) {
                u8 c1 = hle_read8(sys, a0 + i);
                u8 c2 = hle_read8(sys, a1 + i);
                if (c1 != c2 || c1 == 0u) {
                    c->gpr[2] = (u32)((s32)c1 - (s32)c2);
                    break;
                }
            }
            break;
        }
        case 0x19u: { /* strcpy */
            u32 i = 0u;
            for (;;) {
                u8 ch = hle_read8(sys, a1 + i);
                hle_write8(sys, a0 + i, ch);
                if (ch == 0u) {
                    break;
                }
                i++;
            }
            c->gpr[2] = a0;
            break;
        }
        case 0x1Au: { /* strncpy */
            u32 i = 0u;
            for (; i < a2; i++) {
                u8 ch = hle_read8(sys, a1 + i);
                hle_write8(sys, a0 + i, ch);
                if (ch == 0u) {
                    i++;
                    break;
                }
            }
            for (; i < a2; i++) {
                hle_write8(sys, a0 + i, 0u);
            }
            c->gpr[2] = a0;
            break;
        }
        case 0x1Bu: { /* strlen */
            u32 len = 0u;
            while (len < PSX_RAM_SIZE && hle_read8(sys, a0 + len) != 0u) {
                len++;
            }
            c->gpr[2] = len;
            break;
        }
        case 0x1Cu: { /* index / strchr */
            u8 target = (u8)(a1 & 0xFFu);
            u32 p = a0;
            for (;;) {
                u8 ch = hle_read8(sys, p);
                if (ch == target) {
                    c->gpr[2] = p;
                    break;
                }
                if (ch == 0u) {
                    c->gpr[2] = 0u;
                    break;
                }
                p++;
            }
            break;
        }
        case 0x27u: { /* bcopy(src, dst, len) */
            if (a2 > 0u && a2 <= PSX_RAM_SIZE) {
                if (a1 < a0) {
                    for (u32 i = 0; i < a2; i++) {
                        hle_write8(sys, a1 + i, hle_read8(sys, a0 + i));
                    }
                } else if (a1 > a0) {
                    for (u32 i = a2; i > 0u; i--) {
                        hle_write8(sys, a1 + i - 1u, hle_read8(sys, a0 + i - 1u));
                    }
                }
            }
            c->gpr[2] = a1;
            break;
        }
        case 0x28u: { /* bzero(dst, len) */
            u32 limit = a1 > PSX_RAM_SIZE ? PSX_RAM_SIZE : a1;
            for (u32 i = 0; i < limit; i++) {
                hle_write8(sys, a0 + i, 0u);
            }
            c->gpr[2] = a0;
            break;
        }
        case 0x2Au: { /* memcpy(dst, src, len) */
            u32 limit = a2 > PSX_RAM_SIZE ? PSX_RAM_SIZE : a2;
            if (a0 < a1) {
                for (u32 i = 0; i < limit; i++) {
                    hle_write8(sys, a0 + i, hle_read8(sys, a1 + i));
                }
            } else if (a0 > a1) {
                for (u32 i = limit; i > 0u; i--) {
                    hle_write8(sys, a0 + i - 1u, hle_read8(sys, a1 + i - 1u));
                }
            }
            c->gpr[2] = a0;
            break;
        }
        case 0x2Bu: { /* memset(dst, c, len) */
            u32 limit = a2 > PSX_RAM_SIZE ? PSX_RAM_SIZE : a2;
            u8 val = (u8)(a1 & 0xFFu);
            for (u32 i = 0; i < limit; i++) {
                hle_write8(sys, a0 + i, val);
            }
            c->gpr[2] = a0;
            break;
        }
        case 0x2Cu: { /* memmove(dst, src, len) */
            u32 limit = a2 > PSX_RAM_SIZE ? PSX_RAM_SIZE : a2;
            if (a0 < a1) {
                for (u32 i = 0; i < limit; i++) {
                    hle_write8(sys, a0 + i, hle_read8(sys, a1 + i));
                }
            } else if (a0 > a1) {
                for (u32 i = limit; i > 0u; i--) {
                    hle_write8(sys, a0 + i - 1u, hle_read8(sys, a1 + i - 1u));
                }
            }
            c->gpr[2] = a0;
            break;
        }
        case 0x2Du: { /* memcmp */
            u32 limit = a2 > PSX_RAM_SIZE ? PSX_RAM_SIZE : a2;
            c->gpr[2] = 0u;
            for (u32 i = 0; i < limit; i++) {
                u8 c1 = hle_read8(sys, a0 + i);
                u8 c2 = hle_read8(sys, a1 + i);
                if (c1 != c2) {
                    c->gpr[2] = (u32)((s32)c1 - (s32)c2);
                    break;
                }
            }
            break;
        }
        case 0x33u: { /* malloc(size) */
            u32 sz = (a0 + 15u) & ~15u;
            if (sz == 0u) {
                sz = 16u;
            }
            if ((b->hle_heap_ptr & 0x1FFFFFFFu) + sz < 0x001FC000u) {
                c->gpr[2] = b->hle_heap_ptr;
                b->hle_heap_ptr += sz;
            } else {
                c->gpr[2] = 0u;
            }
            break;
        }
        case 0x34u: /* free(ptr) */
            break;
        case 0x39u: /* InitHeap(addr, size) */
            b->hle_heap_ptr = (a0 + 15u) & ~15u;
            break;
        case 0x3Cu: /* std_out_putchar */
            bios_tty_putc(sys, (char)(a0 & 0xFFu));
            break;
        case 0x3Eu: { /* std_out_puts */
            for (u32 i = 0; i < 4096u; i++) {
                u8 ch = hle_read8(sys, a0 + i);
                if (ch == 0u) {
                    break;
                }
                bios_tty_putc(sys, (char)ch);
            }
            bios_tty_putc(sys, '\n');
            break;
        }
        case 0x3Fu: /* printf(fmt, ...) */
            c->gpr[2] = hle_format_printf(sys, a0);
            break;
        case 0x44u: /* FlushCache */
            /* Per ARCHITECTURE.md §5.8: write-trapping SMC is exact; no-op for BIOS blocks */
            break;
        case 0x48u: /* SendGP1Command(cmd) */
            gpu_gp1_write(sys, a0);
            break;
        case 0x49u: /* GPU_cw(gp0_cmd) */
            gpu_gp0_write(sys, a0);
            c->gpr[2] = 0u;
            break;
        case 0x4Au: { /* GPU_cwp(src, num) */
            u32 count = a1 > 65536u ? 65536u : a1;
            for (u32 i = 0; i < count; i++) {
                gpu_gp0_write(sys, hle_read32(sys, a0 + i * 4u));
            }
            c->gpr[2] = 0u;
            break;
        }
        case 0x4Cu: /* gpu_sync */
            c->gpr[2] = 0u;
            break;
        case 0x70u: /* _bu_init */
        case 0x71u: /* _96_init */
        case 0x72u: /* _96_remove */
        case 0x9Fu: /* SetMemSize */
            c->gpr[2] = 1u;
            break;
        default:
            hle_note_unimplemented(0xA0u, fn);
            break;
    }

    hle_return_to_ra(sys);
}

static void hle_dispatch_b0(PsxSystem *sys, u32 fn) {
    PsxCpu *c = &sys->cpu;
    PsxBios *b = &sys->bios;
    u32 a0 = c->gpr[4];
    u32 a1 = c->gpr[5];
    u32 a2 = c->gpr[6];
    u32 a3 = c->gpr[7];

    switch (fn) {
        case 0x00u: { /* alloc_kernel_memory */
            u32 sz = (a0 + 15u) & ~15u;
            c->gpr[2] = b->hle_heap_ptr;
            b->hle_heap_ptr += sz;
            break;
        }
        case 0x07u: /* DeliverEvent(class, spec) */
            hle_deliver_matching_events(sys, a0, a1 & 0xFFFFu);
            break;
        case 0x08u: { /* OpenEvent(class, spec, mode, func) */
            u32 handle = 0xFFFFFFFFu;
            for (u32 i = 0; i < HLE_EVENT_SLOTS; i++) {
                if (b->hle_events[i][2] == HLE_EV_FREE) {
                    u32 mode = (a2 == 0x2000u) ? HLE_EV_MODE_NOCB : HLE_EV_MODE_CB;
                    b->hle_events[i][0] = a0;
                    b->hle_events[i][1] = (a1 & 0xFFFFu) | (mode << 16);
                    b->hle_events[i][2] = HLE_EV_DISABLED;
                    b->hle_events[i][3] = a3;
                    handle = 0xF1000000u | (i << 4);
                    break;
                }
            }
            c->gpr[2] = handle;
            break;
        }
        case 0x09u: { /* CloseEvent(ev) */
            u32 idx = (a0 >> 4) & 0x1Fu;
            if ((a0 & 0xFF000000u) == 0xF1000000u && idx < HLE_EVENT_SLOTS) {
                memset(b->hle_events[idx], 0, sizeof(b->hle_events[idx]));
                c->gpr[2] = 1u;
            } else {
                c->gpr[2] = 0u;
            }
            break;
        }
        case 0x0Au: { /* WaitEvent(ev) */
            u32 idx = (a0 >> 4) & 0x1Fu;
            if ((a0 & 0xFF000000u) == 0xF1000000u && idx < HLE_EVENT_SLOTS) {
                if (b->hle_events[idx][2] == HLE_EV_DELIVERED) {
                    b->hle_events[idx][2] = HLE_EV_ENABLED;
                }
                c->gpr[2] = 1u;
            } else {
                c->gpr[2] = 0u;
            }
            break;
        }
        case 0x0Bu: { /* TestEvent(ev) */
            u32 idx = (a0 >> 4) & 0x1Fu;
            if ((a0 & 0xFF000000u) == 0xF1000000u && idx < HLE_EVENT_SLOTS &&
                b->hle_events[idx][2] == HLE_EV_DELIVERED) {
                b->hle_events[idx][2] = HLE_EV_ENABLED;
                c->gpr[2] = 1u;
            } else {
                c->gpr[2] = 0u;
            }
            break;
        }
        case 0x0Cu: { /* EnableEvent(ev) */
            u32 idx = (a0 >> 4) & 0x1Fu;
            if ((a0 & 0xFF000000u) == 0xF1000000u && idx < HLE_EVENT_SLOTS) {
                b->hle_events[idx][2] = HLE_EV_ENABLED;
                c->gpr[2] = 1u;
            } else {
                c->gpr[2] = 0u;
            }
            break;
        }
        case 0x0Du: { /* DisableEvent(ev) */
            u32 idx = (a0 >> 4) & 0x1Fu;
            if ((a0 & 0xFF000000u) == 0xF1000000u && idx < HLE_EVENT_SLOTS) {
                b->hle_events[idx][2] = HLE_EV_DISABLED;
                c->gpr[2] = 1u;
            } else {
                c->gpr[2] = 0u;
            }
            break;
        }
        case 0x12u: /* InitPAD(buf1, siz1, buf2, siz2) */
            b->hle_pad_buf1_addr = a0;
            b->hle_pad_buf2_addr = a2;
            hle_update_pad_buffers(sys);
            c->gpr[2] = 1u;
            break;
        case 0x13u: /* StartPAD */
            sys->irq.mask |= (1u << PSX_IRQ_VBLANK);
            b->hle_pad_auto_ack = 1u;
            cpu_check_irq(sys);
            c->gpr[2] = 1u;
            break;
        case 0x14u: /* StopPAD */
            b->hle_pad_auto_ack = 0u;
            break;
        case 0x15u: /* OutdatedPadInitAndStart(type, button_dest, unused, unused) */
            b->hle_pad_dest_addr = a1;
            sys->irq.mask |= (1u << PSX_IRQ_VBLANK);
            b->hle_pad_auto_ack = 1u;
            hle_update_pad_buffers(sys);
            cpu_check_irq(sys);
            c->gpr[2] = 2u;
            break;
        case 0x16u: /* PAD_dr */
            c->gpr[2] = (u32)sys->pads[0].buttons | 0xFFFF0000u;
            break;
        case 0x17u: { /* ReturnFromException */
            if (b->hle_in_intr_hook) {
                b->hle_in_intr_hook = false;
                memcpy(c->gpr, b->hle_saved_gpr, sizeof(c->gpr));
                c->hi = b->hle_saved_hi;
                c->lo = b->hle_saved_lo;
                u32 rfe_sr = (b->hle_saved_sr & ~0x0Fu) | ((b->hle_saved_sr >> 2) & 0x0Fu);
                cpu_set_sr(sys, rfe_sr);
                c->in_delay_slot = 0u;
                c->branch_taken = 0u;
                c->pc = b->hle_saved_epc;
                c->next_pc = c->pc + 4u;
            } else {
                u32 sr = c->cop0[COP0_SR];
                u32 rfe_sr = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
                cpu_set_sr(sys, rfe_sr);
                c->in_delay_slot = 0u;
                c->branch_taken = 0u;
                c->pc = c->cop0[COP0_EPC];
                c->next_pc = c->pc + 4u;
            }
            return;
        }
        case 0x18u: /* ResetEntryInt */
            b->hle_hook_entry_int = 0u;
            break;
        case 0x19u: /* HookEntryInt(jmp_buf) */
            b->hle_hook_entry_int = a0;
            break;
        case 0x20u: { /* UnDeliverEvent(class, spec) */
            for (u32 i = 0; i < HLE_EVENT_SLOTS; i++) {
                if (b->hle_events[i][0] == a0 &&
                    (b->hle_events[i][1] & 0xFFFFu) == (a1 & 0xFFFFu) &&
                    b->hle_events[i][2] == HLE_EV_DELIVERED) {
                    b->hle_events[i][2] = HLE_EV_ENABLED;
                }
            }
            break;
        }
        case 0x32u: /* open(filename, accessmode) */
            c->gpr[2] = hle_file_open(sys, a0);
            break;
        case 0x33u: /* lseek(fd, offset, seektype) */
            c->gpr[2] = hle_file_lseek(sys, a0, a1, a2);
            break;
        case 0x34u: /* read(fd, dst, length) */
            c->gpr[2] = hle_file_read(sys, a0, a1, a2);
            break;
        case 0x36u: /* close(fd) */
            c->gpr[2] = hle_file_close(sys, a0);
            break;
        case 0x35u: { /* write(fd, buf, n) */
            if (a0 == 1u || a0 == 2u) {
                u32 limit = a2 > 65536u ? 65536u : a2;
                for (u32 i = 0; i < limit; i++) {
                    u8 ch = hle_read8(sys, a1 + i);
                    if (ch != 0u) {
                        bios_tty_putc(sys, (char)ch);
                    }
                }
                c->gpr[2] = a2;
            } else {
                c->gpr[2] = 0xFFFFFFFFu;
            }
            break;
        }
        case 0x3Du: /* std_out_putchar */
            bios_tty_putc(sys, (char)(a0 & 0xFFu));
            break;
        case 0x3Fu: { /* std_out_puts */
            for (u32 i = 0; i < 4096u; i++) {
                u8 ch = hle_read8(sys, a0 + i);
                if (ch == 0u) {
                    break;
                }
                bios_tty_putc(sys, (char)ch);
            }
            bios_tty_putc(sys, '\n');
            break;
        }
        case 0x5Bu: { /* ChangeClearPAD(int) */
            u32 prev = b->hle_pad_auto_ack;
            b->hle_pad_auto_ack = (u16)(a0 != 0u ? 1u : 0u);
            c->gpr[2] = prev;
            break;
        }
        default:
            hle_note_unimplemented(0xB0u, fn);
            break;
    }

    hle_return_to_ra(sys);
}

static void hle_dispatch_c0(PsxSystem *sys, u32 fn) {
    PsxCpu *c = &sys->cpu;
    PsxBios *b = &sys->bios;

    if (fn == 0xFFu) {
        hle_handle_exception(sys);
        return;
    }

    u32 a0 = c->gpr[4];
    u32 a1 = c->gpr[5];

    switch (fn) {
        case 0x00u: /* EnqueueTimerAndVblankIrqs */
        case 0x01u: /* EnqueueSyscallHandler */
        case 0x02u: /* SysEnqIntRP */
        case 0x03u: /* SysDeqIntRP */
        case 0x08u: /* SysInitMemory */
        case 0x12u: /* InstallDevices */
        case 0x1Cu: /* AdjustA0Table */
            c->gpr[2] = 1u;
            break;
        case 0x0Au: { /* ChangeClearRCnt(t, flag) */
            if (a0 < 4u) {
                u32 prev = (b->hle_rcnt_auto_ack >> a0) & 1u;
                if (a1 != 0u) {
                    b->hle_rcnt_auto_ack |= (u16)(1u << a0);
                } else {
                    b->hle_rcnt_auto_ack &= (u16)~(1u << a0);
                }
                c->gpr[2] = prev;
            } else {
                c->gpr[2] = 0u;
            }
            break;
        }
        default:
            hle_note_unimplemented(0xC0u, fn);
            break;
    }

    hle_return_to_ra(sys);
}

/* One-shot diagnostics for HLE kernel calls we do not emulate (shown in the 3DS 4:LOG tab) */
static u8 g_hle_unimpl_seen[3][32];

static void hle_note_unimplemented(u32 vec, u32 fn) {
    u32 v = (vec == 0xA0u) ? 0u : (vec == 0xB0u) ? 1u : 2u;
    u8 bit = (u8)(1u << (fn & 7u));
    if ((g_hle_unimpl_seen[v][(fn >> 3) & 31u] & bit) == 0u) {
        g_hle_unimpl_seen[v][(fn >> 3) & 31u] |= bit;
        plat_log("[hle] unimplemented BIOS call %02X:%02X\n", (unsigned)vec, (unsigned)fn);
    }
}

bool hle_bios_dispatch(PsxSystem *sys, u32 vec) {
    if (!sys) {
        return false;
    }
    u32 fn = sys->cpu.gpr[9] & 0xFFu;
    switch (vec & 0xFFu) {
        case 0xA0u:
            hle_dispatch_a0(sys, fn);
            return true;
        case 0xB0u:
            hle_dispatch_b0(sys, fn);
            return true;
        case 0xC0u:
            hle_dispatch_c0(sys, fn);
            return true;
        default:
            return false;
    }
}
