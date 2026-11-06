#include "psx3ds/system.h"

#include <string.h>

#define PSX_EXE_HDR_BYTES 0x0800u

static u32 psexe_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

PsxResult psx_exe_parse_header(const u8 *buf, u32 buf_len, PsxExeHeader *out) {
    if (!buf || !out) {
        return PSX_ERR_BAD_ARG;
    }
    if (buf_len < PSX_EXE_HDR_BYTES) {
        return PSX_ERR_FORMAT;
    }
    if (memcmp(buf, "PS-X EXE", 8) != 0) {
        return PSX_ERR_FORMAT;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->magic, buf, 8);
    out->pc0    = psexe_le32(buf + 0x10u);
    out->gp0    = psexe_le32(buf + 0x14u);
    out->t_addr = psexe_le32(buf + 0x18u);
    out->t_size = psexe_le32(buf + 0x1Cu);
    out->d_addr = psexe_le32(buf + 0x20u);
    out->d_size = psexe_le32(buf + 0x24u);
    out->b_addr = psexe_le32(buf + 0x28u);
    out->b_size = psexe_le32(buf + 0x2Cu);
    out->s_addr = psexe_le32(buf + 0x30u);
    out->s_size = psexe_le32(buf + 0x34u);

    if (out->pc0 == 0u || (out->pc0 & 3u) != 0u) {
        return PSX_ERR_FORMAT;
    }
    if (out->t_size > PSX_RAM_SIZE) {
        return PSX_ERR_FORMAT;
    }
    return PSX_OK;
}

static void psexe_write_ram_region(PsxSystem *sys, u32 phys_start, const u8 *src, u32 len) {
    if (!sys || !sys->bus.ram || len == 0u) {
        return;
    }
    /* Always populate RAM directly (works even if bus_dma_write_words is stubbed) */
    for (u32 i = 0; i < len; i++) {
        u32 off = (phys_start + i) & PSX_RAM_MASK;
        sys->bus.ram[off] = src ? src[i] : 0u;
    }

    /* Also route aligned words through bus_dma_write_words + jit_invalidate_range for SMC */
    if (src && (phys_start & 3u) == 0u && (len & 3u) == 0u) {
        u32 words[256];
        u32 done = 0u;
        while (done < len) {
            u32 chunk_bytes = (len - done) > sizeof(words) ? (u32)sizeof(words) : (len - done);
            memcpy(words, src + done, chunk_bytes);
            bus_dma_write_words(sys, (phys_start + done) & PSX_RAM_MASK, words, chunk_bytes >> 2);
            done += chunk_bytes;
        }
    }
    jit_invalidate_range(sys, phys_start & PSX_RAM_MASK, len);
}

PsxResult psx_exe_inject_now(PsxSystem *sys) {
    if (!sys || !sys->bus.ram || sys->bios.exe_path[0] == '\0') {
        return PSX_ERR_BAD_ARG;
    }

    PlatFile *f = plat_fopen(sys->bios.exe_path, "rb");
    if (!f) {
        return PSX_ERR_IO;
    }

    u8 hdr_raw[PSX_EXE_HDR_BYTES];
    if (plat_fread(f, hdr_raw, PSX_EXE_HDR_BYTES) != PSX_EXE_HDR_BYTES) {
        plat_fclose(f);
        return PSX_ERR_FORMAT;
    }

    PsxExeHeader hdr;
    PsxResult prc = psx_exe_parse_header(hdr_raw, PSX_EXE_HDR_BYTES, &hdr);
    if (prc != PSX_OK) {
        plat_fclose(f);
        return prc;
    }

    /* Optional BSS zero-fill before loading text/data */
    if (hdr.b_addr != 0u && hdr.b_size != 0u && hdr.b_size <= PSX_RAM_SIZE) {
        psexe_write_ram_region(sys, hdr.b_addr & PSX_RAM_MASK, NULL, hdr.b_size);
    }

    /* Copy 0x800-offset payload into RAM */
    u64 fsize = plat_fsize(f);
    u64 avail = (fsize > PSX_EXE_HDR_BYTES) ? (fsize - PSX_EXE_HDR_BYTES) : 0u;
    u32 to_read = (avail < (u64)hdr.t_size) ? (u32)avail : hdr.t_size;

    if (!plat_fseek(f, PSX_EXE_HDR_BYTES)) {
        plat_fclose(f);
        return PSX_ERR_IO;
    }

    u8 chunk[4096];
    u32 offset = 0u;
    u32 t_phys = hdr.t_addr & PSX_RAM_MASK;
    while (offset < to_read) {
        u32 step = (to_read - offset) > (u32)sizeof(chunk) ? (u32)sizeof(chunk) : (to_read - offset);
        u32 got = plat_fread(f, chunk, step);
        if (got == 0u) {
            break;
        }
        psexe_write_ram_region(sys, t_phys + offset, chunk, got);
        offset += got;
        if (got < step) {
            break;
        }
    }
    plat_fclose(f);

    if (offset < hdr.t_size) {
        psexe_write_ram_region(sys, t_phys + offset, NULL, hdr.t_size - offset);
    }

    /* Set architectural registers per PS-X EXE spec */
    u32 sp_val = (hdr.s_addr != 0u) ? (hdr.s_addr + hdr.s_size) : 0x801FFF00u;
    sys->bios.exe_pc = hdr.pc0;
    sys->bios.exe_gp = hdr.gp0;
    sys->bios.exe_sp = sp_val;

    PsxCpu *c = &sys->cpu;
    c->gpr[0]  = 0u;
    c->gpr[28] = hdr.gp0; /* $gp */
    c->gpr[29] = sp_val;  /* $sp */
    c->gpr[30] = sp_val;  /* $fp */
    c->pc      = hdr.pc0;
    c->next_pc = hdr.pc0 + 4u;
    c->in_delay_slot  = 0u;
    c->branch_taken   = 0u;
    c->load_delay_reg = 0u;
    c->load_delay_val = 0u;

    if (sys->bios.hle) {
        /* Standalone BIOS-less HLE hardware setup (COP2 GTE + IRQ2 + DMA DPCR + unblank GP1) */
        hle_bios_install_ram_vectors(sys);
        c->gpr[31] = 0x80001000u; /* safe return loop if main() returns */
        cpu_set_sr(sys, PSX_SR_CU2 | PSX_SR_IEC | 0x0400u);
        sys->dma.dpcr = PSX_DPCR_DEFAULT;
        psx_bus_write32_slow(c, 0xBF8010F0u, PSX_DPCR_DEFAULT);
        sys->bus.ram_size_reg = PSX_RAM_SIZE_DEFAULT;
        gpu_gp1_write(sys, 0x00000000u); /* reset GPU */
        gpu_gp1_write(sys, 0x03000000u); /* display enable (unblank) */
    }

    sys->bios.psexe_pending = false;
    c->psexe_pending = 0u;

    jit_flush_all(sys);
    c->icache_dirty = 1u;
    cpu_request_exit(sys);
    return PSX_OK;
}

PsxResult psx_load_exe(PsxSystem *sys, const char *path) {
    if (!sys || !path || path[0] == '\0') {
        return PSX_ERR_BAD_ARG;
    }

    PlatFile *f = plat_fopen(path, "rb");
    if (!f) {
        return PSX_ERR_IO;
    }
    u8 hdr_raw[PSX_EXE_HDR_BYTES];
    u32 got = plat_fread(f, hdr_raw, PSX_EXE_HDR_BYTES);
    plat_fclose(f);

    PsxExeHeader hdr;
    PsxResult prc = psx_exe_parse_header(hdr_raw, got, &hdr);
    if (prc != PSX_OK) {
        return prc;
    }

    size_t plen = strlen(path);
    if (plen >= sizeof(sys->bios.exe_path)) {
        plen = sizeof(sys->bios.exe_path) - 1u;
    }
    memcpy(sys->bios.exe_path, path, plen);
    sys->bios.exe_path[plen] = '\0';

    sys->bios.exe_pc = hdr.pc0;
    sys->bios.exe_gp = hdr.gp0;
    sys->bios.exe_sp = (hdr.s_addr != 0u) ? (hdr.s_addr + hdr.s_size) : 0x801FFF00u;

    /* If no real BIOS is loaded, boot directly via standalone HLE */
    if (sys->bios.hle || !sys->bios.loaded) {
        if (!sys->bios.hle) {
            PsxResult brc = psx_load_bios(sys, NULL);
            if (brc != PSX_OK) {
                return brc;
            }
        }
        return psx_exe_inject_now(sys);
    }

    /* Real BIOS / OpenBIOS loaded: arm injection hook at shell entry 0x80030000 */
    sys->bios.psexe_pending = true;
    sys->cpu.psexe_pending = 1u;
    jit_flush_all(sys);
    return PSX_OK;
}
