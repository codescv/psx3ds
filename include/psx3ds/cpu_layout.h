#ifndef PSX3DS_CPU_LAYOUT_H
#define PSX3DS_CPU_LAYOUT_H

/*
 * Shared byte offsets into struct PsxCpu (include/psx3ds/cpu.h).
 * ONLY preprocessor #defines live here so this header may be included directly
 * from ARM assembly (.S) files as well as C sources.
 *
 * Plain hex literals (no 'u' suffix) are used intentionally for GAS compatibility.
 */

#define PSX_OFF_GPR            0x000 /* u32[32]  gpr[n] at 4*n                                 */
#define PSX_OFF_PC             0x080 /* u32      current instruction PC                        */
#define PSX_OFF_NEXT_PC        0x084 /* u32      interpreter next PC                           */
#define PSX_OFF_HI             0x088 /* u32      MULT/DIV HI                                   */
#define PSX_OFF_LO             0x08C /* u32      MULT/DIV LO                                   */
#define PSX_OFF_CYCLES_LEFT    0x090 /* s32      downcounter home (live in r10 inside JIT)     */
#define PSX_OFF_CYCLE_DEFICIT  0x094 /* s32      cycles consumed past slice end                */
#define PSX_OFF_IRQ_PENDING    0x098 /* u32      SR.IEc && (SR & CAUSE & 0x0700)               */
#define PSX_OFF_IN_DELAY_SLOT  0x09C /* u32      interpreter delay-slot active                 */
#define PSX_OFF_LOAD_DELAY_REG 0x0A0 /* u32      0 = none, else target GPR index 1..31         */
#define PSX_OFF_LOAD_DELAY_VAL 0x0A4 /* u32      pending load value                            */
#define PSX_OFF_JIT_ABORT      0x0A8 /* u32      0 = none, else PSX_EXC_* + 1                  */
#define PSX_OFF_EXIT_REASON    0x0AC /* u32      PSX_EXIT_*                                    */
#define PSX_OFF_BRANCH_TARGET  0x0B0 /* u32      interpreter branch target                     */
#define PSX_OFF_BRANCH_TAKEN   0x0B4 /* u32      interpreter branch taken flag                 */
#define PSX_OFF_IDLE_SKIP      0x0B8 /* u32      JIT requests scheduler jump to next event     */
#define PSX_OFF_SLICE_LEN      0x0BC /* s32      cycles granted at slice start                 */
#define PSX_OFF_COP0           0x0C0 /* u32[16]  0x0C0..0x0FF; SR=+0x30 CAUSE=+0x34 EPC=+0x38  */
#define PSX_OFF_GTE_D          0x100 /* u32[32]  0x100..0x17F                                  */
#define PSX_OFF_GTE_C          0x180 /* u32[32]  0x180..0x1FF; FLAG = gte_c[31] = 0x1FC        */
#define PSX_OFF_FASTMEM_R      0x200 /* PsxPtrSlot (8B): uintptr_t* read LUT                   */
#define PSX_OFF_FASTMEM_W      0x208 /* PsxPtrSlot (8B): uintptr_t* write LUT                  */
#define PSX_OFF_IBTC           0x210 /* PsxPtrSlot (8B): JitIbtcEntry[1024]*                   */
#define PSX_OFF_BLOCKTAB_RAM   0x218 /* PsxPtrSlot (8B): void*[0x80000]                        */
#define PSX_OFF_BLOCKTAB_BIOS  0x220 /* PsxPtrSlot (8B): void*[0x20000]                        */
#define PSX_OFF_RAM_BASE       0x228 /* PsxPtrSlot (8B): u8* 2 MiB RAM                         */
#define PSX_OFF_SCRATCH_BASE   0x230 /* PsxPtrSlot (8B): u8* 4 KiB scratch page                */
#define PSX_OFF_SYS            0x238 /* PsxPtrSlot (8B): PsxSystem* back-pointer               */
#define PSX_OFF_IN_JIT         0x240 /* u32      1 while executing generated ARM code          */
#define PSX_OFF_ABORT_BADVADDR 0x244 /* u32      BadVaddr recorded by faulting slow helper     */
#define PSX_OFF_GTE_SCRATCH    0x248 /* u32[8]   0x248..0x267 inline GTE scratch               */
#define PSX_OFF_ICACHE_DIRTY   0x268 /* u32      deferred jit_mem_icache_flush_all() flag      */
#define PSX_OFF_PSEXE_PENDING  0x26C /* u32      PS-EXE injection armed at 0x80030000          */
#define PSX_OFF_HOT_END        0x300 /* end of hot JIT-addressable region                      */

/* Convenience COP0 byte offsets from r11 (PsxCpu*) */
#define PSX_OFF_COP0_SR        (PSX_OFF_COP0 + 12 * 4) /* 0x0F0 */
#define PSX_OFF_COP0_CAUSE     (PSX_OFF_COP0 + 13 * 4) /* 0x0F4 */
#define PSX_OFF_COP0_EPC       (PSX_OFF_COP0 + 14 * 4) /* 0x0F8 */

/* SR isolate-cache bit */
#define PSX_SR_ISC             0x00010000

/* JIT / CPU slice exit reasons (stored in cpu->exit_reason) */
#define PSX_EXIT_EVENTS        0
#define PSX_EXIT_EXCEPTION     1
#define PSX_EXIT_STEP_ONE      2
#define PSX_EXIT_HALT          3

#endif /* PSX3DS_CPU_LAYOUT_H */
