#ifndef PSX3DS_HW_REGS_H
#define PSX3DS_HW_REGS_H

/*
 * Single source of truth for PlayStation 1 hardware addresses, register bits,
 * COP0/exception constants, and master timing constants (ARCHITECTURE.md §0, §4, §8).
 * Slice A may append additional bit masks as needed.
 */

#include "psx3ds/cpu_layout.h"

/* --------------------------------------------------------------------------
 * Physical Memory Map & Sizes
 * -------------------------------------------------------------------------- */
#define PSX_RAM_PHYS             0x00000000u
#define PSX_RAM_SIZE             0x00200000u /* 2 MiB */
#define PSX_RAM_MASK             0x001FFFFFu
#define PSX_RAM_MIRROR_END       0x00800000u /* 8 MiB KUSEG/KSEG0/KSEG1 mirror window */

#define PSX_EXP1_PHYS            0x1F000000u
#define PSX_EXP1_SIZE            0x00800000u /* 8 MiB */

#define PSX_SCRATCH_PHYS         0x1F800000u
#define PSX_SCRATCH_SIZE         0x00000400u /* 1 KiB real D-cache scratchpad */
#define PSX_SCRATCH_PAGE_SIZE    0x00001000u /* 4 KiB fastmem backing page */
#define PSX_SCRATCH_MASK         0x000003FFu

#define PSX_MMIO_PHYS            0x1F801000u
#define PSX_MMIO_SIZE            0x00001000u /* 4 KiB */

#define PSX_EXP2_PHYS            0x1F802000u
#define PSX_EXP2_SIZE            0x00001000u /* 4 KiB */

#define PSX_EXP3_PHYS            0x1FA00000u
#define PSX_EXP3_SIZE            0x00200000u /* 2 MiB */

#define PSX_BIOS_PHYS            0x1FC00000u
#define PSX_BIOS_SIZE            0x00080000u /* 512 KiB */
#define PSX_BIOS_MASK            0x0007FFFFu

#define PSX_RESET_PC             0xBFC00000u
#define PSX_EXC_VEC_RAM          0x80000080u
#define PSX_EXC_VEC_ROM          0xBFC00180u
#define PSX_SHELL_PC             0x80030000u
#define PSX_CACHE_CTRL_ADDR      0xFFFE0130u

/* --------------------------------------------------------------------------
 * COP0 Register Indices & Bit Masks
 * -------------------------------------------------------------------------- */
#define COP0_BPC                 3
#define COP0_BDA                 5
#define COP0_JUMPDEST            6
#define COP0_DCIC                7
#define COP0_BADVADDR            8
#define COP0_BDAM                9
#define COP0_BPCM                11
#define COP0_SR                  12
#define COP0_CAUSE               13
#define COP0_EPC                 14
#define COP0_PRID                15

#define PSX_COP0_PRID_VALUE      0x00000002u

/* COP0 SR (reg 12) */
#define PSX_SR_IEC               (1u << 0)
#define PSX_SR_KUC               (1u << 1)
#define PSX_SR_IEP               (1u << 2)
#define PSX_SR_KUP               (1u << 3)
#define PSX_SR_IEO               (1u << 4)
#define PSX_SR_KUO               (1u << 5)
/* PSX_SR_ISC (0x00010000) is defined in cpu_layout.h */
#define PSX_SR_SWC               (1u << 17)
#define PSX_SR_PZ                (1u << 18)
#define PSX_SR_CM                (1u << 19)
#define PSX_SR_PE                (1u << 20)
#define PSX_SR_TS                (1u << 21)
#define PSX_SR_BEV               (1u << 22)
#define PSX_SR_RE                (1u << 25)
#define PSX_SR_CU0               (1u << 28)
#define PSX_SR_CU1               (1u << 29)
#define PSX_SR_CU2               (1u << 30)
#define PSX_SR_CU3               (1u << 31)

/* COP0 CAUSE (reg 13) */
#define PSX_CAUSE_EXCCODE_SHIFT  2
#define PSX_CAUSE_EXCCODE_MASK   (0x1Fu << 2)
#define PSX_CAUSE_SW0            (1u << 8)
#define PSX_CAUSE_SW1            (1u << 9)
#define PSX_CAUSE_SW_MASK        0x00000300u
#define PSX_CAUSE_IP2            (1u << 10)
#define PSX_CAUSE_IRQ_MASK       0x00000700u
#define PSX_CAUSE_CE_SHIFT       28
#define PSX_CAUSE_CE_MASK        (0x3u << 28)
#define PSX_CAUSE_BT             (1u << 30)
#define PSX_CAUSE_BD             (1u << 31)

/* R3000A Exception Codes */
#define PSX_EXC_INT              0u
#define PSX_EXC_ADEL             4u
#define PSX_EXC_ADES             5u
#define PSX_EXC_IBE              6u
#define PSX_EXC_DBE              7u
#define PSX_EXC_SYS              8u
#define PSX_EXC_BP               9u
#define PSX_EXC_RI               10u
#define PSX_EXC_CPU              11u
#define PSX_EXC_OV               12u

/* --------------------------------------------------------------------------
 * MMIO Register Addresses (0x1F801000..0x1F801FFF & Expansion 2)
 * -------------------------------------------------------------------------- */
/* Memory Control */
#define PSX_REG_MEM_CTRL_BASE    0x1F801000u
#define PSX_REG_MEM_CTRL_END     0x1F801024u
#define PSX_REG_RAM_SIZE         0x1F801060u
#define PSX_RAM_SIZE_DEFAULT     0x00000B88u

/* SIO0 (Joypad & Memory Card) & SIO1 */
#define PSX_REG_SIO0_BASE        0x1F801040u
#define PSX_REG_SIO0_DATA        0x1F801040u
#define PSX_REG_SIO0_STAT        0x1F801044u
#define PSX_REG_SIO0_MODE        0x1F801048u
#define PSX_REG_SIO0_CTRL        0x1F80104Au
#define PSX_REG_SIO0_BAUD        0x1F80104Eu
#define PSX_REG_SIO1_BASE        0x1F801050u

/* Interrupt Controller */
#define PSX_REG_I_STAT           0x1F801070u
#define PSX_REG_I_MASK           0x1F801074u

#define PSX_IRQ_VBLANK           0u
#define PSX_IRQ_GPU              1u
#define PSX_IRQ_CDROM            2u
#define PSX_IRQ_DMA              3u
#define PSX_IRQ_TMR0             4u
#define PSX_IRQ_TMR1             5u
#define PSX_IRQ_TMR2             6u
#define PSX_IRQ_SIO0             7u
#define PSX_IRQ_SIO1             8u
#define PSX_IRQ_SPU              9u
#define PSX_IRQ_PIO              10u

/* DMA Controller (channels 0..6: 0x1F801080 + 0x10*ch) */
#define PSX_REG_DMA_BASE         0x1F801080u
#define PSX_REG_DMA_MADR(ch)     (0x1F801080u + ((u32)(ch) << 4))
#define PSX_REG_DMA_BCR(ch)      (0x1F801084u + ((u32)(ch) << 4))
#define PSX_REG_DMA_CHCR(ch)     (0x1F801088u + ((u32)(ch) << 4))
#define PSX_REG_DPCR             0x1F8010F0u
#define PSX_REG_DICR             0x1F8010F4u

#define PSX_DMA_CH_MDEC_IN       0u
#define PSX_DMA_CH_MDEC_OUT      1u
#define PSX_DMA_CH_GPU           2u
#define PSX_DMA_CH_CDROM         3u
#define PSX_DMA_CH_SPU           4u
#define PSX_DMA_CH_PIO           5u
#define PSX_DMA_CH_OTC           6u

#define PSX_DPCR_DEFAULT         0x07654321u

/* Root Counters / Timers (0..2: 0x1F801100 + 0x10*n) */
#define PSX_REG_TIMERS_BASE      0x1F801100u
#define PSX_REG_TMR_VAL(n)       (0x1F801100u + ((u32)(n) << 4))
#define PSX_REG_TMR_MODE(n)      (0x1F801104u + ((u32)(n) << 4))
#define PSX_REG_TMR_TARGET(n)    (0x1F801108u + ((u32)(n) << 4))

/* CD-ROM (0x1F801800..0x1F801803) */
#define PSX_REG_CDROM_BASE       0x1F801800u
#define PSX_REG_CDROM_STATUS     0x1F801800u
#define PSX_REG_CDROM_REG1       0x1F801801u
#define PSX_REG_CDROM_REG2       0x1F801802u
#define PSX_REG_CDROM_REG3       0x1F801803u

/* GPU (GP0/GPUREAD at 0x1F801810, GP1/GPUSTAT at 0x1F801814) */
#define PSX_REG_GP0              0x1F801810u
#define PSX_REG_GPUREAD          0x1F801810u
#define PSX_REG_GP1              0x1F801814u
#define PSX_REG_GPUSTAT          0x1F801814u

#define PSX_VRAM_W               1024u
#define PSX_VRAM_H               512u
#define PSX_VRAM_PIXELS          (PSX_VRAM_W * PSX_VRAM_H)
#define PSX_VRAM_BYTES           (PSX_VRAM_PIXELS * 2u)

/* MDEC (0x1F801820, 0x1F801824) */
#define PSX_REG_MDEC_BASE        0x1F801820u
#define PSX_REG_MDEC_CMD         0x1F801820u
#define PSX_REG_MDEC_DATA        0x1F801820u
#define PSX_REG_MDEC_CTRL        0x1F801824u
#define PSX_REG_MDEC_STAT        0x1F801824u

/* SPU (0x1F801C00..0x1F801FFF) */
#define PSX_REG_SPU_BASE         0x1F801C00u
#define PSX_REG_SPU_SIZE         0x00000400u
#define PSX_REG_SPU_MAIN_VOL_L   0x1F801D80u
#define PSX_REG_SPU_MAIN_VOL_R   0x1F801D82u
#define PSX_REG_SPU_KON_LO       0x1F801D88u
#define PSX_REG_SPU_KON_HI       0x1F801D8Au
#define PSX_REG_SPU_KOFF_LO      0x1F801D8Cu
#define PSX_REG_SPU_KOFF_HI      0x1F801D8Eu
#define PSX_REG_SPU_PMON_LO      0x1F801D90u
#define PSX_REG_SPU_PMON_HI      0x1F801D92u
#define PSX_REG_SPU_NON_LO       0x1F801D94u
#define PSX_REG_SPU_NON_HI       0x1F801D96u
#define PSX_REG_SPU_EON_LO       0x1F801D98u
#define PSX_REG_SPU_EON_HI       0x1F801D9Au
#define PSX_REG_SPU_ENDX_LO      0x1F801D9Cu
#define PSX_REG_SPU_ENDX_HI      0x1F801D9Eu
#define PSX_REG_SPU_IRQ_ADDR     0x1F801DA4u
#define PSX_REG_SPU_XFER_ADDR    0x1F801DA6u
#define PSX_REG_SPU_XFER_FIFO    0x1F801DA8u
#define PSX_REG_SPUCNT           0x1F801DAAu
#define PSX_REG_SPU_XFER_CTRL    0x1F801DACu
#define PSX_REG_SPUSTAT          0x1F801DAEu

#define PSX_SPU_RAM_BYTES        (512u * 1024u)

/* Expansion 2 Debug / DUART / PCSX-Redux Exit */
#define PSX_REG_DUART_SRB        0x1F802021u
#define PSX_REG_DUART_THR        0x1F802023u
#define PSX_REG_EXP2_POST        0x1F802041u
#define PSX_REG_PCSX_STDOUT      0x1F802080u
#define PSX_REG_PCSX_EXIT        0x1F802084u

/* --------------------------------------------------------------------------
 * Master Timing Constants (ARCHITECTURE.md §4 & §13.14)
 * -------------------------------------------------------------------------- */
#define PSX_CPU_HZ                   33868800u

/* NTSC: 2152.56 cycles/line (16.16 fixed = 0x08688F5C), 263 lines, VBlank @ 240, 566,124 cycles/frame (59.826 Hz) */
#define PSX_CYCLES_PER_LINE_NTSC     2153u
#define PSX_CYCLES_PER_LINE_NTSC_Q16 0x08688F5Cu
#define PSX_LINES_NTSC               263u
#define PSX_VBLANK_LINE_NTSC         240u
#define PSX_CYCLES_PER_FRAME_NTSC    566124u

/* PAL: 2167.60 cycles/line (16.16 fixed = 0x0877999A), 314 lines, VBlank @ 256, 680,628 cycles/frame (49.76 Hz) */
#define PSX_CYCLES_PER_LINE_PAL      2168u
#define PSX_CYCLES_PER_LINE_PAL_Q16  0x0877999Au
#define PSX_LINES_PAL                314u
#define PSX_VBLANK_LINE_PAL          256u
#define PSX_CYCLES_PER_FRAME_PAL     680628u

/* GPU video clock ratio (NTSC: 3413 / 2152.56 = 1.585554 -> 16.16 fixed = 0x000195E7) */
#define PSX_GPU_CLK_RATIO_NTSC_Q16   0x000195E7u
#define PSX_GPU_CLK_RATIO_PAL_Q16    0x00019363u

/* SPU: 33,868,800 / 44,100 = 768 CPU cycles/sample; 32-sample batch = 24,576 cycles */
#define PSX_SPU_CYCLES_PER_SAMPLE    768u
#define PSX_SPU_BATCH_FRAMES         32u
#define PSX_SPU_BATCH_CYCLES         (PSX_SPU_CYCLES_PER_SAMPLE * PSX_SPU_BATCH_FRAMES)

/* CD-ROM sector & command ACK delays */
#define PSX_CD_SECTOR_CYCLES_1X      451584u /* 33868800 / 75 */
#define PSX_CD_SECTOR_CYCLES_2X      225792u /* 33868800 / 150 */
#define PSX_CD_ACK_CYCLES            0x0000C4E1u
#define PSX_CD_INIT_CYCLES           0x00013CCEu
#define PSX_CD_PAUSE_CYCLES_1X       0x0010BD93u
#define PSX_CD_SEEK_BASE_CYCLES      0x00010000u
#define PSX_CD_SEEK_PER_SECTOR       15u

/* MDEC & SIO0 nominal pacing */
#define PSX_MDEC_BLOCK_CYCLES        4096u
#define PSX_SIO0_ACK_DELAY_CYCLES    100u

#endif /* PSX3DS_HW_REGS_H */
