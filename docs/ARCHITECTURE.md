# psx3ds — System Architecture Specification

> **Status**: normative engineering spec (v1). Every `coder-*` worker implements *exactly* the
> contracts here. Where this document and a research doc disagree, **this document wins**;
> where this document is silent, psx-spx (nocash) + Mednafen behaviour is ground truth.
>
> Inputs: [`research_modern_psx.md`](research_modern_psx.md), [`research_3ds_arm_opt.md`](research_3ds_arm_opt.md),
> [`toolchain_env.md`](toolchain_env.md).

---

## 0. Goals, Targets, Non-Goals

| Target | Macro set | Compiler / flags | What runs |
|---|---|---|---|
| **T1 — Nintendo 3DS `.3dsx`/`.elf`** | `PSX3DS_PLATFORM_3DS=1`, `PSX3DS_ARCH_ARMV6K=1`, `PSX3DS_HAVE_JIT=1`, `PSX3DS_HAVE_CITRO3D=1` | devkitARM `arm-none-eabi-gcc -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfpv2 -mtp=soft -marm -O3 -flto -mword-relocations -ffunction-sections -fdata-sections -D__3DS__` | `arm_jit` dynarec (dual-map → RWX → cached-interpreter fallback), `gpu_citro3d` + `gpu_soft` (ARMv6 SIMD), Core 1 SPU→`ndsp`, N3DS Core 2 CD-ROM readahead, dual-screen frontend |
| **T2 — ARM32 Linux QEMU harness** | `PSX3DS_PLATFORM_HOST=1`, `PSX3DS_ARCH_ARMV6K=1`, `PSX3DS_HAVE_JIT=1` | `arm-linux-gnueabihf-gcc -march=armv6k -mtune=mpcore -mfpu=vfpv2 -mfloat-abi=hard -marm -O3 -static` run under `qemu-arm -cpu arm11mpcore` | **byte-identical** `arm_jit` + all `.S` ARMv6 SIMD kernels, headless CLI, `mmap(RWX)` + `__builtin___clear_cache` |
| **T3 — x86_64 host CLI** | `PSX3DS_PLATFORM_HOST=1`, `PSX3DS_HAVE_JIT=0` | `gcc -O2 -g` (+ optional `-fsanitize=address,undefined`) | interpreter + cached-block interpreter, portable C `gpu_soft`, SPU/CD/MDEC/SIO, PPM/PNG/WAV dumps, benchmarks, golden tests |

Hard rules that apply to every target:

* **C11 (`-std=gnu11`) + ARM assembly only.** No C++, no dynamic allocation after `psx_system_init()` (one arena, see §12).
* **Little-endian only** (`_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)`).
* **No ARMv7 instructions anywhere** (no `MOVW/MOVT/UBFX/BFI/SDIV/UDIV/DMB/Thumb-2/NEON/VFPv3 d16+`). The QEMU harness `SIGILL`s on violations, so T2 is a mandatory CI gate for any `.S` or emitter change.
* One source of truth for every hardware fact: `include/psx3ds/hw_regs.h` (register addresses, bit masks, timing constants).
* Same emulated behaviour on all three targets except where this doc marks a *HW-approximation* (only inside `gpu_citro3d`).

Non-goals (v1): PGXP, upscaling, CHD, analog-stick rumble, netplay, Thumb code generation.

---

## 1. Directory Tree & Zero-Collision Parallel Partitioning

```
psx3ds/
├── CMakeLists.txt                     # single build system (host / arm-linux / 3DS via toolchain file)   [slice F]
├── cmake/
│   ├── arm11-linux.cmake              # T2 toolchain file                                                  [F]
│   └── options.cmake                  # PSX3DS_* cache options                                             [F]
├── include/psx3ds/                    # *** SHARED CONTRACT HEADERS — committed by `skeleton` FIRST ***
│   ├── types.h                        # fixed-width types, PSX_HOT/COLD/LIKELY/ALIGNED, static-assert helpers  [frozen]
│   ├── config.h                       # compile-time knobs + PsxConfig runtime struct                       [frozen]
│   ├── hw_regs.h                      # every MMIO address/bit/timing constant                               [frozen; A may append]
│   ├── cpu_layout.h                   # PSX_OFF_* byte offsets shared by C and .S                            [frozen]
│   ├── cpu.h                          # PsxCpu (hot layout) + interpreter API                                [A]
│   ├── bus.h                          # PsxBus, fastmem tables, slow-path API, DMA write hooks               [A]
│   ├── scheduler.h                    # downcounter scheduler                                                [A]
│   ├── arena.h                        # bump allocator                                                        [A]
│   ├── irq.h, dma.h, timers.h         # peripheral blocks owned by core                                      [A]
│   ├── gte.h                          # GTE C reference + shared tables (UNR, flags)                         [A]
│   ├── jit.h                          # JIT public API (enter/compile/invalidate/stats) + JitBlock            [B]
│   ├── jit_mem.h                      # W^X code-memory provider interface                                    [B decl / F+B impl]
│   ├── gpu.h                          # GP0/GP1 front-end, GpuPrim, GpuBackend vtable, shadow VRAM           [C]
│   ├── gpu_soft.h, gpu_hw.h           # backend constructors                                                  [C] / [D]
│   ├── spu.h, cdrom.h, mdec.h, sio.h  # peripherals                                                           [E]
│   ├── spsc.h                         # lock-free SPSC ring (header-only)                                     [frozen]
│   ├── state.h                        # save-state reader/writer interface                                    [F]
│   ├── platform.h                     # OS abstraction (time, threads, files, video, audio, input)            [F]
│   ├── system.h                       # PsxSystem aggregate + lifecycle (init/reset/run_frame/load)           [F]
│   └── stats.h                        # PsxStats telemetry counters                                           [frozen]
├── src/
│   ├── core/        [slice A]  bus.c fastmem.c scheduler.c irq.c dma.c timers.c cpu_interp.c cop0.c gte.c gte_tables.c arena.c
│   ├── jit/         [slice B]  jit_block.c cpu_cached.c jit_mem_host.c        (built on ALL targets: block tables, invalidation, cached interp)
│   │                           arm_jit.c arm_emit.c arm_emit.h jit_regalloc.c jit_gte.c jit_thunks.S   (PSX3DS_HAVE_JIT only)
│   ├── gpu/         [slice C]  gpu.c (GP0/GP1 parser+state) gpu_soft.c gpu_soft_span.c gpu_soft_span_armv6.S gpu_vram.c
│   ├── gpu_hw/      [slice D]  gpu_citro3d.c gpu_texcache.c gpu_morton.c psx_gpu.v.pica   (3DS only; stub gpu_hw_stub.c for host)
│   ├── periph/      [slice E]  spu.c spu_adpcm.c spu_adpcm_armv6.S spu_reverb.c spu_xa.c cdrom.c cdrom_image.c cdrom_readahead.c
│   │                           mdec.c mdec_idct_armv6.S sio0.c joypad.c memcard.c
│   ├── sys/         [slice F]  system.c bios.c hle_bios.c psexe.c savestate.c stats.c bench.c
│   └── platform/    [slice F]  host/{plat_host.c, main_cli.c, video_ppm.c, audio_wav.c}
│                               3ds/{plat_3ds.c, main_3ds.c, jit_mem_3ds.c, audio_ndsp.c, ui_bottom.c, font8x8.c, threads_3ds.c}
├── tests/           [slice F]  unit/ (ctest) rom_runner.sh golden/ trace_diff.py bench_report.py 3ds/ (azahar smoke)
├── tools/           env.sh, gen_offsets_check.sh
└── docs/
```

### 1.1 Slice ownership (the only rule that matters for collisions)

| Slice | Owner worker | Owns (exclusive write) | May read | Must not touch |
|---|---|---|---|---|
| **A `core`** | coder-core | `src/core/**`, `include/psx3ds/{cpu,bus,scheduler,irq,dma,timers,gte,arena}.h`, may *append* to `hw_regs.h` | all headers | any other `src/*` |
| **B `jit`** | coder-jit | `src/jit/**`, `include/psx3ds/{jit,jit_mem}.h` | all headers | `cpu.h` layout (request changes from A on the board) |
| **C `gpu`** | coder-gpu | `src/gpu/**`, `include/psx3ds/{gpu,gpu_soft}.h` | all headers | `gpu_hw/` |
| **D `gpu_hw`** | coder-gpuhw | `src/gpu_hw/**`, `include/psx3ds/gpu_hw.h` | all headers | `gpu.c` parser |
| **E `periph`** | coder-periph | `src/periph/**`, `include/psx3ds/{spu,cdrom,mdec,sio}.h` | all headers | `dma.c` (DMA engine is A; E only implements the device-side hooks declared in `dma.h`) |
| **F `sys`** | coder-sys | `CMakeLists.txt`, `cmake/`, `src/sys/**`, `src/platform/**`, `include/psx3ds/{state,platform,system}.h`, `tests/**`, `tools/**` | all headers | module internals |

Every slice may **read** every header in `include/psx3ds/` (that is what they are for); only *writing* is exclusive.

Rules of engagement:

1. The `skeleton` commit (one worker, before any fan-out) lands **every header in `include/psx3ds/` exactly as specified in this document** plus empty `.c` files with the agreed function signatures returning `PSX_ERR_NOT_IMPL`. After that commit, **a header is edited only by its owner**; any other slice needing a change posts `@owner` on the board with the exact diff.
2. Each slice works in its own `git worktree` on branch `slice/<name>`; merges to `main` are header-first (owner merges header change, others rebase).
3. Cross-slice calls go only through the public header of the callee. No `extern` declarations in `.c` files.
4. Every slice ships its own `tests/unit/test_<slice>_*.c` (ctest, host build) — no shared test files.
5. `src/platform/3ds/jit_mem_3ds.c` is F's file implementing B's `jit_mem.h` interface; B owns the host implementation `jit_mem_host.c`.

---

## 2. Build System, Macros, Compiler Flags

Single CMake project; `-DPSX3DS_TARGET` is inferred from the toolchain:

```cmake
# cmake/options.cmake (excerpt)
option(PSX3DS_ENABLE_ASAN       "ASan/UBSan (host only)"          OFF)
option(PSX3DS_ENABLE_JIT        "Build ARM11 dynarec"            ON)   # forced OFF on x86_64
option(PSX3DS_ENABLE_HW_GPU     "citro3d backend"                ON)   # forced OFF on host
option(PSX3DS_SMALL_MEM         "Old-3DS memory profile"         OFF)  # runtime-detected too, see §12
set(PSX3DS_CYCLE_MULT_DEFAULT 448 CACHE STRING "8.8 fixed-point cycles per MIPS insn (448 = 1.75)")
```

Platform/arch detection lives **only** in `include/psx3ds/config.h`:

```c
#if defined(__3DS__)
#  define PSX3DS_PLATFORM_3DS 1
#else
#  define PSX3DS_PLATFORM_HOST 1
#endif
#if defined(__arm__) && (defined(__ARM_ARCH_6K__) || defined(__ARM_ARCH_6ZK__) || (defined(__ARM_ARCH) && __ARM_ARCH == 6))
#  define PSX3DS_ARCH_ARMV6K 1
#  define PSX3DS_HAVE_JIT    1
#else
#  define PSX3DS_ARCH_ARMV6K 0
#  define PSX3DS_HAVE_JIT    0
#endif
#if PSX3DS_PLATFORM_3DS
#  define PSX3DS_HAVE_CITRO3D 1
#endif
_Static_assert(sizeof(void*) == 4 || !PSX3DS_HAVE_JIT, "arm_jit is 32-bit only");
```

Per-target flags (set in `CMakeLists.txt`, never in sources):

| | T1 3DS | T2 arm-linux | T3 x86_64 |
|---|---|---|---|
| C | `-O3 -flto -fno-strict-aliasing -fno-common -fomit-frame-pointer -Wall -Wextra -Werror=implicit-function-declaration -Werror=return-type` + `$CTR_CFLAGS` | same minus LTO/`-D__3DS__`, `-marm -static` | `-O2 -g` (+sanitizers) |
| ASM | `-x assembler-with-cpp $CTR_ARCH_FLAGS` | `-x assembler-with-cpp -march=armv6k -marm` | not compiled (every `.S` has a C fallback selected by `PSX3DS_ARCH_ARMV6K`) |
| Link | `-specs=3dsx.specs -lcitro3d -lctru -lm` | `-static -lm -lpthread` | `-lm -lpthread` |
| Shader | `picasso -o psx_gpu.shbin src/gpu_hw/psx_gpu.v.pica` → `bin2s` → `psx_gpu_shbin.o` | — | — |

All six slices compile into **one CMake `OBJECT` library `psx3ds_core_objs`** (sources listed per slice directory; `src/jit/{arm_*.c,jit_gte.c,jit_regalloc.c,jit_thunks.S}` are added only when `PSX3DS_HAVE_JIT`; `src/gpu_hw/*` only when `PSX3DS_HAVE_CITRO3D`, else `gpu_hw_stub.c`), linked into `psx3ds-cli`/`psx3ds-bench`/`psx3ds.3dsx`/unit tests. No per-slice static archives ⇒ no circular `ld` archive ordering problems (`core ↔ jit ↔ gpu ↔ periph ↔ sys` all reference each other). `jit_block.c` and `cpu_cached.c` are compiled on **all** targets (they provide `jit_invalidate_page/range`, `jit_flush_all`, `cpu_cached_run` used by the bus and system code). The shader is embedded as `psx_gpu_shbin.h` exposing `extern const u8 psx_gpu_shbin[]; extern const u32 psx_gpu_shbin_size;` (bin2s naming).

Do **not** use `-ffixed-r9/-ffixed-r11`: the JIT's register conventions apply only *inside generated code*; C helpers are ordinary AAPCS functions and preserve r4–r11 by contract.

Test entry points (all produced by slice F): `psx3ds-cli` (T2/T3 headless), `psx3ds-bench`, `psx3ds-unit-*`, `psx3ds.3dsx`.

---

## 3. Core Header Contracts

### 3.1 `types.h` (frozen)

```c
#include <stdint.h> <stdbool.h> <stddef.h> <stdalign.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t  s8; typedef int16_t  s16; typedef int32_t  s32; typedef int64_t  s64;
typedef u32 psx_tick_t;                       /* 32-bit wrapping CPU-cycle timestamp (see §4) */
#define PSX_HOT        __attribute__((hot))
#define PSX_COLD       __attribute__((cold, noinline))
#define PSX_LIKELY(x)  __builtin_expect(!!(x), 1)
#define PSX_UNLIKELY(x)__builtin_expect(!!(x), 0)
#define PSX_ALIGNED(n) __attribute__((aligned(n)))
#define PSX_CACHELINE  32                     /* ARM11 MPCore L1 line */
#define PSX_ASSERT_OFF(T, f, off) _Static_assert(offsetof(T, f) == (off), #T "." #f " offset drifted")
typedef enum { PSX_OK = 0, PSX_ERR_NOT_IMPL = -1, PSX_ERR_IO = -2, PSX_ERR_NOMEM = -3, PSX_ERR_BAD_ARG = -4, PSX_ERR_FORMAT = -5 } PsxResult;
typedef struct PsxSystem PsxSystem;   /* forward declarations so every header compiles in any include order */
typedef struct StateIO   StateIO;
typedef struct PsxCpu    PsxCpu;
```

### 3.2 `PsxCpu` hot layout (`cpu.h` + `cpu_layout.h`)

All JIT-visible state sits in the first **0x300 bytes** so every access is a single `LDR/STR rX, [r11, #imm12]` (imm12 ≤ 4095). Pointer fields use an 8-byte slot (`PsxPtrSlot`) so offsets are **identical on ARM32 and x86_64** and one `cpu_layout.h` serves C, `.S`, and the x86_64 unit test that checks the layout.

```c
/* include/psx3ds/cpu_layout.h — ONLY #defines, included by .S files */
#define PSX_OFF_GPR            0x000   /* u32[32]  gpr[n] at 4*n                               */
#define PSX_OFF_PC             0x080
#define PSX_OFF_NEXT_PC        0x084
#define PSX_OFF_HI             0x088
#define PSX_OFF_LO             0x08C
#define PSX_OFF_CYCLES_LEFT    0x090   /* s32 downcounter home (live in r10 inside JIT)        */
#define PSX_OFF_CYCLE_DEFICIT  0x094   /* s32 cycles consumed past the slice end                */
#define PSX_OFF_IRQ_PENDING    0x098   /* u32 SR.IEc && (SR & CAUSE & 0x0700) (CAUSE.IP2 mirrors I_STAT&I_MASK) */
#define PSX_OFF_IN_DELAY_SLOT  0x09C   /* u32 interpreter only                                  */
#define PSX_OFF_LOAD_DELAY_REG 0x0A0   /* u32 0 = none                                          */
#define PSX_OFF_LOAD_DELAY_VAL 0x0A4
#define PSX_OFF_JIT_ABORT      0x0A8   /* u32 0 = none, else PSX_EXC_* + 1 (set by helpers while in_jit)  */
#define PSX_OFF_EXIT_REASON    0x0AC   /* u32 PSX_EXIT_*                                        */
#define PSX_OFF_BRANCH_TARGET  0x0B0   /* interpreter                                           */
#define PSX_OFF_BRANCH_TAKEN   0x0B4
#define PSX_OFF_IDLE_SKIP      0x0B8   /* u32 JIT asks scheduler to jump to next event          */
#define PSX_OFF_SLICE_LEN      0x0BC   /* s32 cycles granted at slice start                      */
#define PSX_OFF_COP0           0x0C0   /* u32[16]  0x0C0..0x0FF ; SR=+0x30 CAUSE=+0x34 EPC=+0x38 */
#define PSX_OFF_GTE_D          0x100   /* u32[32]  0x100..0x17F                                  */
#define PSX_OFF_GTE_C          0x180   /* u32[32]  0x180..0x1FF ; FLAG = gte_c[31] = 0x1FC       */
#define PSX_OFF_FASTMEM_R      0x200   /* uintptr_t* (8-byte slot)                                */
#define PSX_OFF_FASTMEM_W      0x208   /* uintptr_t*  swapped on SR.IsC                           */
#define PSX_OFF_IBTC           0x210   /* JitIbtcEntry[1024]*                                     */
#define PSX_OFF_BLOCKTAB_RAM   0x218   /* void*[0x80000] indexed by byte offset (phys & 0x1FFFFF) */
#define PSX_OFF_BLOCKTAB_BIOS  0x220   /* void*[0x20000] indexed by (phys - 0x1FC00000)          */
#define PSX_OFF_RAM_BASE       0x228   /* u8* 2MB                                                 */
#define PSX_OFF_SCRATCH_BASE   0x230   /* u8* 4KB page (first 1KB = scratchpad)                   */
#define PSX_OFF_SYS            0x238   /* PsxSystem* back-pointer                                 */
#define PSX_OFF_IN_JIT         0x240   /* u32 1 while executing generated code (helpers branch on it) */
#define PSX_OFF_ABORT_BADVADDR 0x244   /* u32 BadVaddr captured by a helper that set jit_abort          */
#define PSX_OFF_GTE_SCRATCH    0x248   /* u32[8] temporaries for inline GTE sequences                   */
#define PSX_OFF_ICACHE_DIRTY   0x268   /* u32 deferred jit_mem_icache_flush_all() request               */
#define PSX_OFF_PSEXE_PENDING  0x26C   /* u32 PS-EXE injection armed at 0x80030000                      */
#define PSX_OFF_HOT_END        0x300
#define PSX_SR_ISC             (1u << 16)
```

```c
/* include/psx3ds/cpu.h */
typedef union { void *p; u64 _pad; } PsxPtrSlot;              /* 8 bytes on every ABI */
typedef struct PSX_ALIGNED(32) PsxCpu {
    u32 gpr[32];
    u32 pc, next_pc, hi, lo;
    s32 cycles_left, cycle_deficit;
    u32 irq_pending, in_delay_slot, load_delay_reg, load_delay_val;
    u32 jit_abort, exit_reason, branch_target, branch_taken, idle_skip;
    s32 slice_len;
    u32 cop0[16];
    u32 gte_d[32];
    u32 gte_c[32];
    PsxPtrSlot fastmem_r, fastmem_w, ibtc, blocktab_ram, blocktab_bios, ram_base, scratch_base, sys;
    u32 in_jit, abort_badvaddr;
    u32 gte_scratch[8];
    u32 icache_dirty, psexe_pending;
    u8  _hot_pad[PSX_OFF_HOT_END - 0x270];
    /* ---- cold ---- */
    u64 total_cycles;           /* for stats/benchmarks only */
    u32 cache_ctrl;             /* 0xFFFE0130 */
    u32 exc_pending;            /* interpreter: PSX_EXC_* + 1, 0 = none */
    u32 icache_tag_dummy[256];  /* IsC writes land here (no I-cache emulation) */
} PsxCpu;
PSX_ASSERT_OFF(PsxCpu, gpr, PSX_OFF_GPR);    PSX_ASSERT_OFF(PsxCpu, pc, PSX_OFF_PC);
PSX_ASSERT_OFF(PsxCpu, cycles_left, PSX_OFF_CYCLES_LEFT); PSX_ASSERT_OFF(PsxCpu, cop0, PSX_OFF_COP0);
PSX_ASSERT_OFF(PsxCpu, gte_d, PSX_OFF_GTE_D); PSX_ASSERT_OFF(PsxCpu, gte_c, PSX_OFF_GTE_C);
PSX_ASSERT_OFF(PsxCpu, fastmem_r, PSX_OFF_FASTMEM_R); PSX_ASSERT_OFF(PsxCpu, fastmem_w, PSX_OFF_FASTMEM_W);
PSX_ASSERT_OFF(PsxCpu, ibtc, PSX_OFF_IBTC); PSX_ASSERT_OFF(PsxCpu, blocktab_ram, PSX_OFF_BLOCKTAB_RAM);
PSX_ASSERT_OFF(PsxCpu, in_jit, PSX_OFF_IN_JIT); PSX_ASSERT_OFF(PsxCpu, gte_scratch, PSX_OFF_GTE_SCRATCH); PSX_ASSERT_OFF(PsxCpu, icache_dirty, PSX_OFF_ICACHE_DIRTY);
_Static_assert(offsetof(PsxCpu, total_cycles) >= PSX_OFF_HOT_END, "hot region overflow");
```

`tools/gen_offsets_check.sh` compiles `tests/unit/test_layout.c` on **all three** targets; CI fails if any assert fires.

COP0 register indices: `COP0_BPC=3, BDA=5, JUMPDEST=6, DCIC=7, BADVADDR=8, BDAM=9, BPCM=11, SR=12, CAUSE=13, EPC=14, PRID=15 (=0x00000002)`. Exception codes: `INT=0, ADEL=4, ADES=5, IBE=6, DBE=7, SYS=8, BP=9, RI=10, CPU=11, OV=12`.

Interpreter API (slice A, `cpu.h`):

```c
void cpu_init(PsxSystem*); void cpu_reset(PsxSystem*); void cpu_state(PsxSystem*, StateIO*);
void cpu_run_interp(PsxSystem*);          /* plain interpreter: runs until cycles_left <= 0 */
void cpu_step_one(PsxSystem*);            /* exactly one instruction incl. delay-slot/load-delay bookkeeping */
void cpu_step_until_clean(PsxSystem*);    /* do { cpu_step_one } while (in_delay_slot || load_delay_reg) — the ONLY JIT→interp handoff
                                             (a vector's `j handler; nop` or a `jr $ra; lw` pair must complete before generated code resumes) */
/* code: PSX_EXC_*; pc: address of the faulting instruction (the BRANCH when bd); ce: coprocessor # for PSX_EXC_CPU else 0 */
void cpu_raise_exception(PsxSystem*, u32 code, u32 pc, bool bd, u32 branch_target, u32 badvaddr, u32 ce);
void cpu_set_sr(PsxSystem*, u32 v);       /* ONLY writer of cop0[12]: IsC fastmem toggle + cpu_check_irq */
void cpu_set_cause(PsxSystem*, u32 v);    /* MTC0 CAUSE: only bits 9:8 (SW0/SW1) writable, then cpu_check_irq */
void cpu_check_irq(PsxSystem*);           /* irq_pending = SR.IEc && (SR & CAUSE & 0x0700); if set → cpu_request_exit */
void cpu_dispatch_irq(PsxSystem*);        /* enters vector: EPC = pc (or branch pc when in delay slot), Cause.BD, ExcCode=0 */
void cpu_request_exit(PsxSystem*);        /* slice_len -= cycles_left; cycles_left = 0 */
```

`cpu_raise_exception` and `cpu_dispatch_irq` **first commit any pending load** (`if (load_delay_reg) gpr[load_delay_reg] = load_delay_val;`) — instruction *i* (`LW`) will not re-execute after `RFE`, so an IRQ taken at *i+1*, or an exception raised by *i+1* (after it read the old `rt`), must not lose it. The JIT mirrors this: `exc_stub_{i+1}`/`abort_stub_{i+1}` write back *i*'s loaded value after *i+1*'s operands were read. Remaining `cpu_raise_exception` rules (asserted by `psxtest_cpu.exe`): `EPC = pc` (branch address when `bd`), `Cause.BD = bd`, `Cause.ExcCode = code`, `Cause.CE (bits 29:28) = ce` for `PSX_EXC_CPU` else 0, **`cop0[6] JUMPDEST = branch_target` when `bd`**, **`cop0[8] BADVADDR` written only for `ADEL/ADES`** (preserved otherwise), SR mode-stack push (`SR = (SR & ~0x3F) | ((SR << 2) & 0x3F)`), `pc = (SR & BEV) ? 0xBFC00180 : 0x80000080`, `load_delay_reg = 0`, `in_delay_slot = 0`.

### 3.3 Memory map & `PsxBus` (`bus.h`)

```c
typedef struct PsxBus {
    u8  *ram;                 /* 2 MiB, 4KB-aligned (arena) */
    u8  *scratch_page;        /* 4 KiB page; [0,0x400) is the real scratchpad, rest is harmless filler */
    u8  *bios;                /* 512 KiB */
    uintptr_t *fm_r;          /* 1M entries: read  table (never swapped)                 */
    uintptr_t *fm_w;          /* 1M entries: write table (RAM entries NULLed on code pages / while SR.IsC) */
    bool  isolated;           /* mirror of SR.IsC as applied to fm_w                       */
    u16  code_page_refs[512]; /* # live JIT blocks per physical 4KB RAM page              */
    u32  ram_size_reg;        /* 0x1F801060 */
    bool strict_ram_size;     /* copy of cfg.strict_ram_size, honoured by fastmem_build / fastmem_set_isolated / bus_code_page_unref */
    u32  mem_ctrl[9];         /* 0x1F801000..0x1F801020 (stored, mostly ignored)          */
    u32  exp2_post;           /* 0x1F802041 */
} PsxBus;
#define PSX_FM_SHIFT    12
#define PSX_FM_ENTRIES  (1u << (32 - PSX_FM_SHIFT))   /* 1,048,576 */
#define PSX_FM_BYTES    (PSX_FM_ENTRIES * sizeof(uintptr_t))   /* 4 MiB ARM32 / 8 MiB x86_64 */
```

Physical map (phys = `addr & 0x1FFFFFFF` for KUSEG/KSEG0/KSEG1):

| Range (phys) | Size | Device | `fm_r` | `fm_w` | Notes |
|---|---|---|---|---|---|
| `0x00000000-0x001FFFFF` | 2 MiB | RAM | bias | bias (NULL on code pages) | |
| `0x00200000-0x007FFFFF` | 6 MiB | RAM mirrors | bias (fold `& 0x1FFFFF`) | bias | Default `RAM_SIZE` (0xB88) makes these mirrors; strict mode (`cfg.strict_ram_size`) NULLs them → slow path raises DBE |
| `0x1F000000-0x1F7FFFFF` | 8 MiB | Expansion 1 | NULL → reads `0xFF` | NULL → ignored | |
| `0x1F800000-0x1F8003FF` | 1 KiB | **Scratchpad** | bias (`scratch_page`) | bias | **only in KUSEG and KSEG0**; `0xBF800000` is a bus error on real HW → left NULL |
| `0x1F801000-0x1F801FFF` | 4 KiB | **MMIO** | NULL | NULL | always slow path |
| `0x1F802000-0x1F802FFF` | 4 KiB | Expansion 2 (POST, debug UART) | NULL | NULL | |
| `0x1FA00000-0x1FBFFFFF` | 2 MiB | Expansion 3 | NULL | NULL | |
| `0x1FC00000-0x1FC7FFFF` | 512 KiB | BIOS | bias | **NULL** (ROM) | |
| `0xFFFE0130` (KSEG2) | 4 B | Cache Control | NULL | NULL | slow path |
| everything else | | unmapped | NULL → DBE | NULL → DBE | `0x20000000-0x7FFFFFFF`, `0xC0000000+` |

### 3.4 Bias-adjusted fastmem math — verified

Entry definition (all arithmetic in `uintptr_t`, i.e. modular — **never** `u8*` arithmetic, which is UB out of bounds and gets miscompiled):

```c
static void fm_map(uintptr_t *t, u32 guest_base, u32 size, const void *host, u32 wrap /*0 or power of 2*/) {
    for (u32 off = 0; off < size; off += 4096) {
        u32 g = guest_base + off;
        u32 h = wrap ? (off & (wrap - 1)) : off;
        t[g >> PSX_FM_SHIFT] = (uintptr_t)host + h - (uintptr_t)g;      /* host_page - guest_page */
    }
}
void fastmem_build(PsxBus *b) {
    memset(b->fm_r, 0, PSX_FM_BYTES); memset(b->fm_w, 0, PSX_FM_BYTES);
    const u32 seg[3] = { 0x00000000u, 0x80000000u, 0xA0000000u };
    for (int s = 0; s < 3; s++) {
        fm_map(b->fm_r, seg[s] | 0x00000000u, 0x800000, b->ram,  0x200000);   /* 2MB + 3 mirrors */
        fm_map(b->fm_w, seg[s] | 0x00000000u, 0x800000, b->ram,  0x200000);
        fm_map(b->fm_r, seg[s] | 0x1FC00000u, 0x080000, b->bios, 0);          /* read-only */
    }
    fm_map(b->fm_r, 0x1F800000u, 0x1000, b->scratch_page, 0); fm_map(b->fm_w, 0x1F800000u, 0x1000, b->scratch_page, 0);
    fm_map(b->fm_r, 0x9F800000u, 0x1000, b->scratch_page, 0); fm_map(b->fm_w, 0x9F800000u, 0x1000, b->scratch_page, 0);
    /* NOT 0xBF800000: scratchpad is not reachable through KSEG1 */
}
/* C fast path used by the interpreters and by every C-side device. Alignment and user-mode segment checks
   come FIRST (MIPS raises AdEL/AdES before any bus activity; also keeps T3 ASan/UBSan clean at 0x801FFFFE). */
static inline bool psx_addr_fault(PsxCpu *c, u32 a, u32 size) {
    return (a & (size - 1)) || ((c->cop0[12] & 0x02) && (a & 0x80000000u));   /* KUc=1 → kernel segments fault */
}
static inline u32 psx_read32(PsxCpu *c, u32 a) {
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 4))) return psx_bus_read32_slow(c, a);   /* slow path raises AdEL */
    uintptr_t e = ((uintptr_t *)c->fastmem_r.p)[a >> PSX_FM_SHIFT];
    return PSX_LIKELY(e) ? *(u32 *)(e + a) : psx_bus_read32_slow(c, a);     /* `a` zero-extends */
}
/* psx_read16/8, psx_write32/16/8 identical in shape. The JIT performs the alignment test inline (§5.9); the KUc
   segment test is interpreter-only (PS1 software runs in kernel mode; `cfg.jit_check_kuc` adds it for test runs). */
```

Correctness check of the bias trick (ARM32 modulo 2³²; x86_64 modulo 2⁶⁴ — `a` is `u32` so it zero-extends, never sign-extends):

| Guest access | page `a>>12` | entry | `entry + a` |
|---|---|---|---|
| `0x00123456` (KUSEG RAM) | `0x00123` | `ram + 0x123000 − 0x00123000 = ram` | `ram + 0x123456` ✓ |
| `0x80123456` (KSEG0) | `0x80123` | `ram − 0x80000000` (wraps) | `ram − 0x80000000 + 0x80123456 = ram + 0x123456` ✓ |
| `0xA0323456` (KSEG1, 2nd mirror) | `0xA0323` | `ram + 0x123000 − 0xA0323000 = ram − 0xA0200000` | `ram + 0x123456` ✓ |
| `0xBFC00180` (BIOS exception vector) | `0xBFC00` | `bios − 0xBFC00000` | `bios + 0x180` ✓ |
| `0x1F8000F0` (scratch) | `0x1F800` | `scratch_page − 0x1F800000` | `scratch_page + 0xF0` ✓ |
| `0x1F801814` (GPUSTAT) | `0x1F801` | `0` | slow path ✓ |
| `0xBF800010` | `0xBF800` | `0` | slow path → DBE ✓ |

No naturally-aligned MIPS access (LW 4-aligned, LH 2-aligned, LWL/LWR hit the aligned word) can cross a 4KB page, so a single table lookup is always sufficient.

> **The Scratchpad/MMIO trap, resolved.** With a 64KB LUT, page `0x1F80` holds both the 1KB scratchpad (`0x1F800000-3FF`) *and* every MMIO register (`0x1F801000-1FFF`), so it must be NULL and **every** scratchpad access (games use it as a fast stack: `$sp = 0x1F8003F0` is a classic) would take the ~40-cycle slow path. We therefore use **4KB granularity (1M entries)**: scratchpad gets its own page `0x1F800` with the same 2-instruction fast path as RAM, MMIO page `0x1F801` stays NULL. Cost: 4 MiB per table on ARM32 (same size Ari64's `memory_map[1M]` already uses in PCSX-ReARMed 3DS); the hot working set of entries is a few cache lines. The 4KB granularity is also exactly what SMC trapping needs (§3.6). Constant-address accesses (`lui`-based, the majority of scratchpad/MMIO traffic) are resolved at *compile time* by the JIT and never touch the table (§5.9).

### 3.5 `SR.IsC` (COP0 SR bit 16) — zero per-store cost

`cpu_set_sr()` is the **only** writer of `cop0[12]` (MTC0, RFE, exception entry all route through it):

```c
void cpu_set_sr(PsxSystem *sys, u32 v) {
    PsxCpu *c = &sys->cpu; u32 old = c->cop0[12]; c->cop0[12] = v;
    if ((old ^ v) & PSX_SR_ISC) fastmem_set_isolated(&sys->bus, (v & PSX_SR_ISC) != 0);
    cpu_check_irq(sys);
}
/* fastmem.c — rewrites ONLY the writable entries (3 segments × 2048 RAM pages + 2 scratch pages = 6146 words ≈ 25K cycles),
   honouring code_page_refs so SMC trapping survives the round trip. No third table, no per-store check. */
void fastmem_set_isolated(PsxBus *b, bool isc) {
    for (int s = 0; s < 3; s++) for (u32 p = 0; p < 2048; p++) {
        u32 vp = ((u32[]){0x00000u, 0x80000u, 0xA0000u})[s] + p;              /* 4KB page index */
        b->fm_w[vp] = (isc || b->code_page_refs[p & 0x1FF]) ? 0 : (uintptr_t)b->ram + ((p & 0x1FF) << 12) - (vp << 12);
    }
    b->fm_w[0x1F800] = isc ? 0 : (uintptr_t)b->scratch_page - 0x1F800000u;
    b->fm_w[0x9F800] = isc ? 0 : (uintptr_t)b->scratch_page - 0x9F800000u;
    b->isolated = isc;
}
```
(`cpu->fastmem_w` therefore never changes; the JIT still loads it per store because `cfg.isc_swap_table=1` — a debugging/benchmark variant — swaps to a `calloc`'d all-NULL table instead. Default is the in-place rewrite: it costs 4 MiB less FCRAM and the ~25K-cycle toggle happens only inside `FlushCache`.)

While isolated, every store hits `psx_bus_write*_slow()` whose first statement is `if (c->cop0[12] & PSX_SR_ISC) return;` (the BIOS' `0xA0000000..` zero-fill during boot is thereby *dropped*, which is what real hardware does — it only touches I-cache tags). Reads during IsC keep using `fm_r` (RAM data); no known software depends on reading cache contents.

### 3.6 SMC protection — NULL write entries for code pages

```c
/* bus.h — called by the JIT (slice B) via these two functions only */
void bus_code_page_ref  (PsxBus *b, u32 phys_page /*0..511*/);   /* ++refs; on 0→1 NULL all 24 aliases in fm_w */
void bus_code_page_unref(PsxBus *b, u32 phys_page);              /* --refs; on 1→0 restore bias in fm_w          */
/* aliases of RAM page p:  seg ∈ {0x0,0x8,0xA} << 28, mirror m ∈ 0..3 → ((seg | m<<21 | p<<12) >> 12)             */
```

Write slow path (word variant; 8/16-bit identical with masks). Helpers **never raise exceptions directly while `in_jit`** — they record `jit_abort`/`abort_badvaddr` and return, and the per-site abort stub (which knows the precise guest `pc`, `bd`, branch target and the dirty register set) raises it (§5.9). In interpreter context they raise immediately with the interpreter's precise state.

```c
static void bus_fault(PsxCpu *c, u32 code, u32 a) {                         /* AdEL/AdES/DBE from a helper */
    PsxSystem *sys = c->sys.p;
    if (c->in_jit) { c->jit_abort = code + 1; c->abort_badvaddr = a; return; }
    cpu_raise_exception(sys, code, c->pc, c->in_delay_slot, c->branch_target, a, 0);
}
PSX_COLD void psx_bus_write32_slow(PsxCpu *c, u32 a, u32 v) {
    PsxSystem *sys = c->sys.p;
    if (PSX_UNLIKELY(psx_addr_fault(c, a, 4))) { bus_fault(c, PSX_EXC_ADES, a); return; }   /* before IsC! */
    if (c->cop0[12] & PSX_SR_ISC) return;                                   /* cache isolated: dropped */
    bool seg_ok = (a < 0x20000000u) || (a >= 0x80000000u && a < 0xC0000000u);
    if (!seg_ok) { if (a == 0xFFFE0130u) { c->cache_ctrl = v; return; } bus_fault(c, PSX_EXC_DBE, a); return; }
    u32 ph = a & 0x1FFFFFFFu;
    if (ph < 0x00800000u) {                                                  /* RAM: code page, or strict-mode mirror */
        if (ph >= 0x00200000u && sys->cfg.strict_ram_size && !ram_size_allows(sys->bus.ram_size_reg, ph)) { bus_fault(c, PSX_EXC_DBE, a); return; }
        u32 off = ph & 0x1FFFFFu; u32 *p = (u32 *)(sys->bus.ram + off);
        if (*p != v) { *p = v; jit_invalidate_page(sys, off >> 12); }       /* identical rewrite: no invalidation */
        return;
    }
    if (ph < 0x1F000000u) { bus_fault(c, PSX_EXC_DBE, a); return; }          /* 0x00800000..0x1EFFFFFF unmapped */
    if (ph < 0x1F800000u) return;                                            /* EXP1: writes ignored */
    if (ph < 0x1F800400u && a < 0xA0000000u) { *(u32 *)(sys->bus.scratch_page + (ph & 0x3FF)) = v; return; }  /* only reached in strict/debug paths */
    if (ph < 0x1F801000u) { bus_fault(c, PSX_EXC_DBE, a); return; }          /* 0xBF800000 KSEG1 scratchpad, 0x1F800400.. */
    if (ph < 0x1F802000u) { mmio_write32(sys, ph, v); return; }              /* MMIO */
    if (ph < 0x1F803000u) { exp2_write(sys, ph, v); return; }                /* EXP2: 0x1F802023/0x1F802080 TTY char → plat_log, 0x1F802084 exit code, 0x1F802041 POST */
    if (ph >= 0x1FA00000u && ph < 0x1FC00000u) return;                       /* EXP3: ignored, no fault (io-access-bitwidth.exe) */
    if (ph >= 0x1FC00000u && ph < 0x1FC80000u) return;                       /* ROM */
    bus_fault(c, PSX_EXC_DBE, a);
}
```
Reads mirror this: EXP1/EXP2/EXP3 return `0xFFFFFFFF`-filled (except EXP2 `0x1F802021` DUART SRB → `0x04` TXRDY so OpenBIOS/Redux TTY polls succeed); unmapped → DBE; KSEG1 scratchpad → DBE.

Guarantees:

* Data pages: **zero overhead** (2-instruction fast path, no bitmap, no generation counter).
* Code pages: every write traps, performs the write, and invalidates *only that 4KB physical page's* blocks (§5.7). Pages are un-NULLed when their last block dies.
* **DMA** (channels 0–6) and the CD-ROM/MDEC/SPU device writers never touch RAM directly: they call `void bus_dma_write_words(PsxSystem*, u32 phys, const u32 *src, u32 n)` / `void bus_dma_fill_desc(PsxSystem*, u32 phys_last, u32 n)` (OTC) (slice A) which `memcpy` into `ram[phys & 0x1FFFFF]` (wrapping) then call `jit_invalidate_range(sys, phys, bytes)` for any page with `code_page_refs != 0`. DMA6 OTC and DMA2 reads use `void bus_dma_read_words(PsxSystem*, u32 phys, u32 *dst, u32 n)` (no SMC concern).
* Adaptive escape hatch (P2, hook provided now): `code_page_mode[512] ∈ {TRAP, VERIFY}`; a page that takes `> cfg.smc_trap_budget` trapped writes per frame without any invalidation flips to VERIFY (write entry restored; its blocks are compiled with a 4-instruction entry checksum of their source words, Ari64-style). Default: never flips (`budget = UINT32_MAX`) until benchmarks justify it.

### 3.7 Slow-path & MMIO API (slice A)

```c
u32  psx_bus_read32_slow (PsxCpu*, u32 a);  u16 psx_bus_read16_slow(PsxCpu*, u32 a);  u8 psx_bus_read8_slow(PsxCpu*, u32 a);
void psx_bus_write32_slow(PsxCpu*, u32 a, u32 v); /* 16, 8 */
u32  psx_bus_fetch32(PsxCpu*, u32 pc);     /* instruction fetch (see rules below) */
/* MMIO dispatch table built at init: 4KB page 0x1F801xxx split into 16-byte slots → handler index */
typedef u32  (*MmioRead)(PsxSystem*, u32 addr, u32 width);
typedef void (*MmioWrite)(PsxSystem*, u32 addr, u32 v, u32 width);
```

Peripherals register handlers with `bus_mmio_register(sys, base, size, read_fn, write_fn)` in their `*_init`. Width-mismatched accesses follow psx-spx (`io-access-bitwidth.exe` is the test).

**Instruction fetch rules** (`psx_bus_fetch32`, verified against `tests/roms/cpu/code-in-io.exe`): `pc & 3 != 0` → AdEL (BadVaddr = pc, EPC = pc); RAM (incl. mirrors) and BIOS → direct; **scratchpad `0x1F800000-0x1F800FFF` → IBE** (the D-cache RAM is not on the instruction bus); **MMIO `0x1F801000-0x1F801FFF` → fetch succeeds via `mmio_read32`** (e.g. `jr $ra; nop` fetched from DMA/SPU registers works on hardware); EXP1/EXP2/EXP3 → fetch returns the bus value (`0xFFFFFFFF` → RI); everything else → IBE. The JIT caches blocks only for RAM and BIOS pcs; any other pc is executed through `cpu_step_one()` by `jit_lookup_slow()` (never cached, never linked).

### 3.8 System aggregate, config, stats, arena, lifecycle contracts

Every subsystem is a plain struct embedded by value in `PsxSystem` (no hidden globals; the SPU/CD worker threads get `PsxSystem*`). Each subsystem exports exactly four lifecycle functions with this naming: `xxx_init(PsxSystem*)` (arena allocation + MMIO/event registration, once), `xxx_reset(PsxSystem*)` (power-on state), `xxx_state(PsxSystem*, StateIO*)` (save/load), `xxx_shutdown(PsxSystem*)` (threads/files only).

```c
/* include/psx3ds/arena.h  [A] */
typedef struct PsxArena { u8 *base; u32 size, used; } PsxArena;
void *arena_alloc(PsxArena*, u32 bytes, u32 align);           /* aborts on exhaustion; 4KB align for tables/code */

/* include/psx3ds/config.h  [frozen] — runtime knobs (CLI flags / 3DS menu write these before psx_system_init) */
typedef enum { CPU_AUTO, CPU_INTERP, CPU_CACHED, CPU_JIT } PsxCpuEngine;
typedef enum { GPU_BACKEND_SOFT, GPU_BACKEND_HW } PsxGpuBackendKind;
typedef enum { PSX_REGION_AUTO, PSX_REGION_NTSC_U, PSX_REGION_NTSC_J, PSX_REGION_PAL } PsxRegion;
typedef struct PsxConfig {
    PsxCpuEngine cpu_engine; PsxGpuBackendKind gpu_backend; PsxRegion region;
    u16 cycle_mult;            /* 8.8 fixed, default PSX3DS_CYCLE_MULT_DEFAULT (448) */
    u8  cycles_per_mem;        /* extra cycles per load/store, default 0 */
    u8  frameskip;             /* 0..3, 0xFF = auto */
    bool jit_ibtc, jit_link, jit_idle_skip, jit_check_align, jit_check_kuc, jit_inline_gte, jit_allow_hle_exec, isc_swap_table;
    bool strict_ram_size, gpu_timing, spu_async, spu_reverb, small_mem, hle_bios, dither_hw;
    u32  smc_trap_budget;      /* default UINT32_MAX (adaptive VERIFY mode disabled) */
    u32  jit_code_bytes, jit_max_blocks, dma_ll_max_nodes_per_tick, dma_cycles_per_word;
    char bios_path[256], memcard_path[2][256];
} PsxConfig;
void psx_config_defaults(PsxConfig*, bool is_n3ds /* or host */);

/* include/psx3ds/stats.h  [frozen] — all u32/u64, zeroed per frame by psx_stats_frame_begin() except *_total */
typedef struct PsxStats {
    u64 cycles_total, insns_total; u32 frames;
    u32 jit_blocks_live, jit_blocks_compiled_frame, jit_code_bytes, jit_flushes, jit_mem_mode;
    u32 ibtc_hits, ibtc_misses, dispatch_slow, slow_reads, slow_writes, smc_traps, smc_invalidations, step_one_fallbacks;
    u32 gte_inline, gte_helper, idle_skips, exceptions, irqs;
    u32 gpu_prims, gpu_batches, gpu_tex_uploads, gpu_tex_hits, gpu_readbacks;
    u32 spu_batches, spu_underruns, cd_ra_hits, cd_ra_misses, events_fired[16];
    u32 us_cpu, us_gpu, us_present, us_events, us_frame;   /* plat_time_us deltas */
} PsxStats;

/* include/psx3ds/platform.h  [F] */
typedef struct PsxPadState { u16 buttons /* active-low PS1 layout */; u8 lx, ly, rx, ry; u8 analog_mode; u8 connected; } PsxPadState;
typedef struct PsxHostKeys { u8 menu, save_state, load_state, toggle_renderer, fast_forward, quit; u8 slot; } PsxHostKeys;

/* include/psx3ds/system.h  [F] */
typedef struct PsxSystem {
    PsxConfig  cfg;  PsxStats stats;  PsxArena arena;
    PsxCpu     cpu;                 /* MUST stay first among the 32-byte-aligned members; cpu.sys points back */
    PsxBus     bus;  Scheduler sched;  PsxIrq irq;  PsxDma dma;  PsxTimers timers;
    PsxGte     gte;                 /* C reference state/tables (registers live in cpu.gte_d/gte_c) */
    JitState   jit;                 /* block pool, tables, code cache (also used by cpu_cached) */
    PsxGpu     gpu;  PsxSpu spu;  PsxCdrom cdrom;  PsxMdec mdec;  PsxSio sio;
    PsxBios    bios;                /* loader state, HLE tables, psexe pending info, TTY capture */
    u32  cpu_mode;                  /* resolved PsxCpuEngine */
    bool frame_done, quit;
    PsxPadState pads[2];
} PsxSystem;
PsxResult psx_system_init(PsxSystem*, const PsxConfig*);     /* arena → *_init in order: bus, sched, irq, cpu, gte, jit, dma, timers, gpu, spu, cdrom, mdec, sio, bios */
void      psx_system_reset(PsxSystem*);
PsxResult psx_load_bios(PsxSystem*, const char *path);        /* NULL → HLE */
PsxResult psx_load_exe(PsxSystem*, const char *path);         /* arms psexe_pending; injected at 0x80030000 or immediately under HLE */
PsxResult psx_load_cd(PsxSystem*, const char *path);
void      psx_run_frame(PsxSystem*);                          /* until EV_VBLANK sets frame_done */
PsxResult psx_state_save(PsxSystem*, StateIO*); PsxResult psx_state_load(PsxSystem*, StateIO*);
void      psx_system_shutdown(PsxSystem*);
```

Subsystem structs are declared in their owner's header: `PsxIrq { u32 stat, mask; }` (`irq.h`), `PsxTimers { struct { u32 mode, target; u32 base_value; psx_tick_t base_tick; u8 gated, reached_target, reached_ffff; } t[3]; }` (`timers.h`), `PsxDma` (§8.2), `JitState { JitBlock *blocks; u32 n_blocks, max_blocks; u16 page_head[512]; JitIbtcEntry *ibtc; void **tab_ram, **tab_bios; JitMem mem; u8 *code_ptr, *code_end; u32 generation; }` (`jit.h`), `PsxGte { u32 unr_table[257]; … }` (`gte.h`), `PsxGpu` (§7.1), `PsxSpu/PsxCdrom/PsxMdec/PsxSio` (§8), `PsxBios` (§9). The `skeleton` worker copies these declarations verbatim; fields may be *added* by owners later, never renamed.

---

## 4. Event-Driven Downcounter Scheduler (`scheduler.h`, slice A)

Design: **fixed-capacity sorted array** (≤ 16 live events — insertion sort beats a heap at this size and keeps the array in one cache line), 32-bit wrap-safe timestamps, one slot per event id (re-scheduling replaces).

```c
typedef enum {
    EV_HBLANK = 0,   /* every scanline: GPU line counter, Timer0/1 gating & HBlank clock, Timer1 source */
    EV_VBLANK,       /* IRQ0 + frame present                                                              */
    EV_TIMER0, EV_TIMER1, EV_TIMER2,   /* target/overflow IRQ4-6                                           */
    EV_CDROM,        /* INT1..INT5 delivery, seek/read pacing                                             */
    EV_DMA,          /* chunked DMA completion (DICR IRQ3)                                                */
    EV_SPU,          /* 32-sample batch (24,576 cycles) in sync mode; IRQ9 check                           */
    EV_SIO0,         /* byte transfer + /ACK (IRQ7)                                                       */
    EV_MDEC,         /* macroblock output pacing (DMA1 ready)                                             */
    EV_GPU,          /* GP0 command-time model: clears "busy", VRAM transfer completion                   */
    EV_IRQ_LATCH,    /* 1-cycle deferred I_STAT→COP0 Cause propagation                                    */
    EV_COUNT
} PsxEventId;
_Static_assert(EV_COUNT <= 16, "sorted-array capacity");

typedef void (*PsxEventFn)(struct PsxSystem *sys, u32 late_cycles);   /* late = now - when, phase-accurate reschedule */

typedef struct Scheduler {
    psx_tick_t now;                 /* advances only in sched_run() / sched_sync()  */
    psx_tick_t slice_start;
    u8  count;
    struct { psx_tick_t when; u8 id; } q[EV_COUNT];      /* ascending by (s32)(when - now) */
    psx_tick_t  when_by_id[EV_COUNT];                     /* for cancel/reschedule, 0 = inactive (+ active bitmask) */
    u16 active_mask;
    PsxEventFn  fn[EV_COUNT];
    u64 total;                      /* 64-bit monotonic counter (stats)            */
} Scheduler;

static inline bool tick_before(psx_tick_t a, psx_tick_t b) { return (s32)(a - b) < 0; }   /* wrap-safe, Δ < 2^31 = 63 s */

void sched_init(Scheduler*);
void sched_set_handler(Scheduler*, PsxEventId, PsxEventFn);
void sched_schedule(PsxSystem*, PsxEventId, u32 delta_cycles);   /* from now; replaces existing; may shorten the running slice */
void sched_cancel(PsxSystem*, PsxEventId);
psx_tick_t sched_now(const PsxSystem*);                          /* == sched.now + consumed_in_slice, valid inside MMIO handlers */
void sched_run(PsxSystem*);                                      /* called when cpu.cycles_left <= 0 */
```

Slice protocol (shared by interpreter, cached interpreter, JIT):

```c
void sched_run(PsxSystem *sys) {                      /* cpu.cycles_left <= 0 here */
    Scheduler *s = &sys->sched; PsxCpu *c = &sys->cpu;
    u32 consumed = (u32)(c->slice_len - c->cycles_left);          /* includes overshoot (cycles_left < 0) */
    if (c->idle_skip) { consumed = (u32)(s->q[0].when - s->slice_start); c->idle_skip = 0; }   /* jump time to next event */
    s->now = s->slice_start + consumed; s->total += consumed;
    while (s->count && !tick_before(s->now, s->q[0].when)) {      /* fire all due, in order */
        u8 id = s->q[0].id; psx_tick_t when = s->q[0].when;
        sched_pop_front(s); s->active_mask &= ~(1u << id);
        s->fn[id](sys, s->now - when);
    }
    cpu_check_irq(sys);                                            /* may queue an exception → cycles_left stays 0 next time */
    if (c->irq_pending) cpu_dispatch_irq(sys);                     /* enters exception vector now (block boundary = insn boundary) */
    s32 next = s->count ? (s32)(s->q[0].when - s->now) : 0x10000;
    c->slice_len = c->cycles_left = next > 0 ? next : 1;
    s->slice_start = s->now;
    c->cycle_deficit = 0;
}
```

* **Mid-slice time** for MMIO handlers (timer reads, CD status) is `sched_now()`; the JIT keeps `r10` (cycles_left) synced to memory before every C call precisely so this is exact.
* **Shortening a running slice**: `sched_schedule()` with a `when` earlier than the current slice end sets `c->cycles_left = min(cycles_left, when - sched_now())` and adds the difference to `c->cycle_deficit`-neutral bookkeeping (`slice_len` is reduced by the same amount) so `consumed` stays right. From C code this is safe because the JIT reloads `r10` after every helper.
* **Asking for an immediate exit** (IRQ became pending inside an MMIO write, SMC in current block, savestate request): `cpu_request_exit(sys)` does `slice_len -= cycles_left; cycles_left = 0;` — the next block entry check fails and falls into `sched_run`.
* Timing constants (`hw_regs.h`, 16.16 fixed-point accumulators, from psx-spx):
  `PSX_CPU_HZ = 33868800`, `PSX_CYCLES_PER_LINE_NTSC = 2152.56 (0x2188_8F5C)`, `PSX_LINES_NTSC = 263`, `PSX_VBLANK_LINE_NTSC = 240`; PAL: `2167.6`, `314`, `256`. Frame = 566,124 cycles (59.826 Hz) / 680,628 (49.76 Hz). GPU dot clock = `3413/2152.56 = 1.5856` video clocks per CPU cycle (NTSC), dividers 10/8/5/4/7 for 256/320/512/640/368 px. SPU: exactly **768** cycles per 44.1 kHz sample. CD-ROM: 1× sector = 451,584 cycles, 2× = 225,792; command ACK base delay `0xC4E1` (50,401) cycles, `Init` 0x13CCE, `Pause` 0x10BD93 at 1×.

The `EV_HBLANK` handler is ≤ 60 ARM instructions (increment line, toggle Timer0/1 gate state, bump Timer1 if HBlank-clocked, set `GPUSTAT.31` field line parity in interlace, reschedule `+cycles_per_line` relative to `when`); ~15.8K/s costs < 0.5 % of an O3DS core.

---

## 5. ARM11v6K Dynarec `arm_jit` (slice B)

### 5.1 Fixed register convention inside generated code

| ARM | Role | Notes |
|---|---|---|
| `r11` | `PsxCpu *` | every state access `[r11, #PSX_OFF_*]` |
| `r9`  | `fm_r` table base (`cpu->fastmem_r`) | never changes at runtime; still **reloaded after every C call** (cheap, defensive) |
| `r10` | `cycles_left` (signed, live) | **synced to `[r11,#CYCLES_LEFT]` before every C call, reloaded after** |
| `r4–r8` | 5 persistent MIPS register cache slots | callee-saved ⇒ survive C calls for free |
| `r12`, `lr` | 2 more persistent cache slots | **every slow-path stub starts with `PUSH {r12, lr}` and ends with `POP {r12, lr}`** (8 bytes ⇒ `sp` stays 8-aligned), so they are callee-saved *from the generated code's point of view* |
| `r0–r3` | scratch / C argument & return | never hold MIPS state across an instruction boundary |
| `sp` | untouched by block bodies | 8-byte aligned at every `BL` (AAPCS §5.2.1.2) |

⇒ **7 cached MIPS GPRs per block** (`r4-r8, r12, lr`), 4 scratch. Register `$zero` is never allocated.

**The single call primitive.** Because `BL` overwrites `lr` *before* any callee can save it, generated code is forbidden from emitting a bare `BL`. The emitter exposes exactly one call helper and every C/asm helper call (slow memory path, `gte_exec_c`, `jit_div_helper`, `jit_lwl_helper`, `cpu_set_sr`, …) uses it:

```c
void emit_call(Emitter *e, const void *fn, bool syncs_cycles) {
    emit_push(e, R12 | LR);                         /* PUSH {r12, lr}: saves the two cache slots, sp stays 8-aligned */
    if (syncs_cycles) emit_str(e, R10, R11, PSX_OFF_CYCLES_LEFT);
    emit_bl(e, fn);                                 /* the ONLY place a BL is emitted */
    if (syncs_cycles) emit_ldr(e, R10, R11, PSX_OFF_CYCLES_LEFT);
    emit_ldr(e, R9, R11, PSX_OFF_FASTMEM_R);
    emit_pop(e, R12 | LR);
}
```
There is no other `BL` anywhere in generated code (block transfers use `B`, `LDR pc`, `BX`). Generated code never touches VFP registers, so caller-saved `d0-d7` clobbers by C helpers are irrelevant. ARMv6 multiply operand rule honoured by the emitter: `RdLo != RdHi`, and neither equals `Rm` (required pre-v6, harmless to keep).

### 5.2 Entry / exit / AAPCS compliance (`jit_thunks.S`)

```armasm
@ int jit_enter(PsxCpu *cpu)  — returns cpu->exit_reason
jit_enter:
    push    {r4-r12, lr}                    @ 10 regs = 40 bytes → sp stays 8-byte aligned (entry sp is 8-aligned per AAPCS)
    mov     r11, r0
    ldr     r9,  [r11, #PSX_OFF_FASTMEM_R]
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r0,  [r11, #PSX_OFF_PC]
    b       jit_dispatch
jit_exit:                                   @ any exit path: r10 valid, all dirty MIPS regs already stored
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    mov     r0, #PSX_EXIT_EVENTS            @ (or reason set by caller in r0)
    str     r0, [r11, #PSX_OFF_EXIT_REASON]
    pop     {r4-r12, pc}

@ Calling a C helper from a stub: ALWAYS this exact shape
    push    {r12, lr}
    str     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    @ r0..r3 = args (r0 = cpu or addr per helper signature)
    bl      helper
    ldr     r10, [r11, #PSX_OFF_CYCLES_LEFT]
    ldr     r9,  [r11, #PSX_OFF_FASTMEM_R]
    pop     {r12, lr}
```

Main loop (slice F `system.c`, identical for interpreter/cached/JIT):

```c
for (;;) {
    sched_run(sys);
    switch (sys->cpu_mode) { case CPU_JIT: jit_run(sys); break; case CPU_CACHED: cpu_cached_run(sys); break; default: cpu_run_interp(sys); }
    if (sys->frame_done) break;
}
/* src/jit/arm_jit.c — the only caller of jit_enter() */
void jit_run(PsxSystem *sys) {
    PsxCpu *c = &sys->cpu;
    if (c->icache_dirty) { jit_mem_icache_flush_all(&sys->jit.mem); c->icache_dirty = 0; }
    if (c->in_delay_slot || c->load_delay_reg) cpu_step_until_clean(sys);     /* never enter generated code with live pipeline state */
    if (c->cycles_left <= 0) return;
    c->in_jit = 1; jit_enter(c); c->in_jit = 0;
}
```

### 5.3 Block cache & O(1) lookup

* **Code cache**: one append-only bump region from `jit_mem_alloc()` (N3DS 16 MiB, O3DS 6 MiB, host 32 MiB). When full (or block-pool full): **flush everything** (`jit_flush_all`): reset bump pointer, reset both block tables to `&jit_compile_stub`, zero `code_page_refs` (restoring `fm_w`), purge IBTC, invalidate I-cache (§5.12). No per-block freeing.
* **Block table RAM**: `void *blocktab_ram[0x80000]` — one slot per RAM **word**. `jit.h` defines `#define BLOCKTAB_RAM_IDX(pc)  (((pc) & 0x1FFFFFu) >> 2)` and `#define BLOCKTAB_BIOS_IDX(pc) ((((pc) & 0x1FFFFFFFu) - 0x1FC00000u) >> 2)`; **all C code indexes with these word indices** (`tab_ram[BLOCKTAB_RAM_IDX(pc)]`). Only the ARM32 asm dispatcher may use the byte offset `phys & 0x1FFFFC` directly because `sizeof(void*) == 4` there (`ldr pc, [r3, r2]`); on x86_64 (`cpu_cached`) the stride is 8 and the macros are the only correct form. 2 MiB on ARM32, 4 MiB on x86_64.
* **Block table BIOS**: `void *blocktab_bios[0x20000]` indexed by `phys - 0x1FC00000` (512 KiB). Code executing from MMIO / Expansion regions is stepped by `jit_lookup_slow()` through `cpu_step_one()` (never cached, correct, rare); scratchpad and unmapped pcs raise IBE there (§3.7).
* Table entries are **never NULL**: default `&jit_compile_stub`. **Pinned slots** (never compiled, never linked, never IBTC-cached, blocks end before reaching them): `tab_ram[BLOCKTAB_RAM_IDX(0xA0)] = …[IDX(0xB0)] = …[IDX(0xC0)] = &jit_bios_vec_stub` (BIOS call vectors — TTY capture for `--until-printf` under a real BIOS/OpenBIOS via `A0:0x3C`/`B0:0x3D` putchar, or `hle_bios_call()` when `cfg.hle_bios`; the stub then executes the vector's `j handler; nop` through `cpu_step_until_clean` or returns to `$ra`), and `tab_ram[BLOCKTAB_RAM_IDX(0x30000)] = &jit_psexe_stub` while `cpu.psexe_pending` (PS-EXE injection at the OpenBIOS/retail shell entry `0x80030000`). `jit_lookup_slow(cpu, pc)` installs/refreshes these pins after every `jit_flush_all`. All three engines share one hook so T3 (interpreter) passes gates G1/G2 too: `bool bios_pc_hook(PsxSystem*, u32 pc)` in `system.h` (slice F) returns `true` when it handled the pc (HLE call performed / TTY byte captured and `$ra` return done / PS-EXE injected and `pc` set) and `false` when the real vector code must run. `cpu_interp.c`/`cpu_cached.c` call it when `((pc & 0x1FFFFFFFu) == 0xA0 || == 0xB0 || == 0xC0) || (psexe_pending && (pc & 0x1FFFFFFFu) == 0x30000)` (one well-predicted compare on the fetch path, only in those two engines); `jit_bios_vec_stub`/`jit_psexe_stub` call it and, on `false`, run `cpu_step_until_clean()` before re-dispatching.
* **`jit.h` public API** (built on all targets; emitter-only functions compile to `PSX_ERR_NOT_IMPL` stubs when `!PSX3DS_HAVE_JIT`): `jit_init/jit_reset/jit_state/jit_shutdown(PsxSystem*)`, `void jit_run(PsxSystem*)`, `void cpu_cached_run(PsxSystem*)`, `void jit_invalidate_page(PsxSystem*, u32 phys_page)`, `void jit_invalidate_range(PsxSystem*, u32 phys, u32 bytes)`, `void jit_flush_all(PsxSystem*)`, `void *jit_lookup_slow(PsxCpu*, u32 pc)`, `void *jit_compile_at_pc(PsxCpu*)`, `int jit_enter(PsxCpu*)` (asm), `BLOCKTAB_*_IDX` macros, `JitBlock`, `JitIbtcEntry`, `JitState`.
* **JitBlock pool**: `JitBlock blocks[N]` (N = 65536 N3DS/host, 32768 O3DS), 32 bytes each:

```c
typedef struct JitBlock {
    PsxPtrSlot entry;        /* host RX entry (first insn = cycle check); 8-byte slot on every ABI */
    u32  pc;                 /* guest virtual start */
    u32  code_bytes;
    u16  n_insns, cycles;    /* cycles already scaled by cycle_mult */
    u16  phys_page[2];       /* RAM pages touched (0xFFFF = none); BIOS blocks: both 0xFFFF */
    u16  next_in_page[2];    /* intrusive per-page singly linked lists (block indices, 0xFFFF end) */
    u16  flags;              /* JB_IDLE_LOOP | JB_ENDS_LOAD_DS | JB_HAS_GTE | JB_DEAD | JB_PEEKED_TARGET */
    u16  _pad;
} JitBlock;  _Static_assert(sizeof(JitBlock) == 32, "32 bytes on ARM32 and x86_64");
u16 page_head[512];          /* first block index per physical RAM page */
```

Dispatcher (asm, `r0 = guest pc`):

```armasm
jit_dispatch:
    tst     r0, #3                          @ misaligned JR/JALR target → AdEL (EPC = BadVaddr = target), never LDR pc from it
    bne     jit_dispatch_slow
    bic     r2, r0, #0xE0000000             @ phys
    tst     r2, #0x10000000                 @ bit 28 set → BIOS / scratch / exp / MMIO
    bne     1f
    bic     r2, r2, #0x00600000             @ fold RAM mirrors: 0x000000..0x7FFFFF → 0x000000..0x1FFFFF
    cmp     r2, #0x00200000
    bhs     jit_dispatch_slow               @ anything >= 0x800000 (no bit 28) is unmapped KUSEG → C path raises IBE
    ldr     r3, [r11, #PSX_OFF_BLOCKTAB_RAM]
    ldr     pc, [r3, r2]                    @ entry(r0 = pc preserved for jit_compile_stub)
1:  sub     r3, r2, #0x1FC00000             @ 0x1FC00000 = 0x7F ror 10 → encodable
    cmp     r3, #0x00080000
    bhs     jit_dispatch_slow
    ldr     r2, [r11, #PSX_OFF_BLOCKTAB_BIOS]
    ldr     pc, [r2, r3]
jit_dispatch_slow:                          @ C: jit_lookup_slow(cpu, pc) → entry. No block state is live here, so it may call
                                            @ cpu_raise_exception directly (misaligned pc → AdEL with EPC=BadVaddr=pc; unmapped → IBE)
                                            @ and return the vector's entry; non-RAM/BIOS executable pcs run via cpu_step_one and
                                            @ return &jit_resume_trampoline (which re-dispatches cpu->pc).
    str     r0, [r11, #PSX_OFF_PC]
    push    {r12, lr}  ; str r10 ; mov r1, r0 ; mov r0, r11 ; bl jit_lookup_slow ; ldr r10 ; ldr r9 ; pop {r12, lr}
    bx      r0
jit_compile_stub:                           @ table default. r0 = pc
    str     r0, [r11, #PSX_OFF_PC]
    push    {r12, lr} ; str r10 ; mov r0, r11 ; bl jit_compile_at_pc ; ldr r10 ; ldr r9 ; pop {r12, lr}
    bx      r0                              @ r0 = new entry (or jit_exit trampoline if compile refused)
```

(Keep the mirror fold: `bic #0x00600000` then compare `< 0x200000`; addresses `0x800000..0xFFFFFFF` in KUSEG are invalid and go slow.)

### 5.4 Block layout, cycle check at entry, block linking

```
block(pc):
entry:
  cmp    r10, #0                             ; (1) out of time?  (CMP first: a skipped block must not consume its cycles —
  ble    exit_stub                           ;     sched_run derives `consumed = slice_len - cycles_left`, so r10 is only
  sub    r10, r10, #CYCLES                   ;     decremented when the block really executes; split SUB if CYCLES > 255)
  <body>                                     ; cached regs loaded lazily at first use
  <writeback dirty cached regs>              ; STR rX,[r11,#4*n]
  <terminator>                               ; §5.4.1
exit_stub:    ldr r0, =pc ; str r0,[r11,#PSX_OFF_PC] ; b jit_exit            ; nothing dirty yet at entry
exc_stub_i:   <STR dirty regs live at site i> ; add r10, r10, #(CYCLES - cycles_before_i) ; <set pc/bd/branch args> ; b jit_raise_exception_exit
slow_stub_j / abort_stub_j:  see §5.9 ; b cont_j
.ltorg                                       ; per-block literal pool (pc-relative ±4095 bytes)
```

Rules: block ≤ 128 MIPS instructions or until the first branch (+ its delay slot), and never across a pinned pc (§5.3); `CYCLES = (n_insns * cfg.cycle_mult) >> 8` (+ `cfg.cycles_per_mem` per load/store). Exception stubs **add back** the unexecuted remainder so time stays exact and `trace_diff` agrees with the interpreter cycle-for-cycle.

#### 5.4.1 Terminators and Direct Block Linking — **zero code patching after commit**

In-place patching of an already-executed `B` is unsafe on 3DS without an I-cache invalidate SVC (the stale `B link_stub` would be re-fetched from L1 I-cache forever, re-entering the linker and its `svcFlushProcessDataCache` on every loop iteration) and invisible to Azahar's Dynarmic. Therefore links are resolved **at compile time** or go through a **data word**, never by rewriting instructions:

| Case (static target `T`) | Emitted code | Cost |
|---|---|---|
| `T` == this block (self-loop) | `b entry` | 1 |
| `T` already compiled **and** `pages(T) ⊆ pages(this block)` (so any SMC that kills `T` also kills this block; BIOS blocks have no pages, so BIOS→BIOS and RAM→BIOS links are always direct) | `b T.entry` | 1 |
| otherwise (incl. `T` not yet compiled, or `T` in another 4KB page) | `ldr r0, =T_pc ; ldr r2, =&tab[BLOCKTAB_*_IDX(T)] ; ldr pc, [r2]` | 3 insns, 2 loads (the table slot *is* the per-target pointer: it becomes `T.entry` when `T` is compiled and reverts to `&jit_compile_stub` when `T` dies — no back-links, no patching) |
| `T` has no table slot (not RAM/BIOS, or a strict-mode mirror) | `ldr r0, =T_pc ; b jit_dispatch` | |
| conditional branch | `CMP` + `Bcc slot_taken` falling into `slot_not_taken`; each slot is one of the rows above | |
| `JR`/`JALR` | IBTC (§5.5) → dispatcher | |
| block ends with `MTC0 SR/CAUSE` or `RFE` **not** in a delay slot | `ldr r0,=pc+4` → `b jit_dispatch` (unlinked: the IRQ state may have changed) | |

To maximise the 1-insn case, `jit_compile_at_pc` **eagerly compiles direct same-page targets** (depth ≤ 4, budget ≤ 32 blocks) before emitting the predecessor's terminator; loops and if/else ladders inside a page therefore link directly. Headroom rule: the top-level compile checks `free_blocks >= 33 && free_code_bytes >= 33 * JIT_MAX_BLOCK_BYTES` **before** starting and calls `jit_flush_all()` there if needed; recursive (eager) compiles never flush — they simply stop recursing when headroom runs out (a flush inside recursion would destroy the half-built parent on the C stack). `cfg.jit_link = 0` forces the table-slot form everywhere (benchmark/diagnostic).

### 5.5 `JR`/`JALR` — 1024-entry IBTC

```c
typedef struct { u32 tag; void *entry; } JitIbtcEntry;   /* 8 KiB, cpu->ibtc, 8-byte aligned */
#define IBTC_EMPTY_TAG 1u                                /* every slot is initialised/purged to { 1, &jit_dispatch_slow } */
```
A guest `jr` to address `1` therefore "hits" the empty slot and lands in `jit_dispatch_slow`, which raises AdEL for it — correct by construction, no NULL entry can ever exist. The index computation clears `pc[1:0]` so the `LDM` is always 8-byte aligned even for a misaligned target (an `LDM` from a non-word-aligned address is an alignment fault on ARM11 regardless of `SCTLR.U`):

```armasm
@ r0 = target guest pc (already materialised)
    ldr     r3, [r11, #PSX_OFF_IBTC]
    mov     r2, r0, lsl #20                 @ pc[11:0] → bits 31:20
    bic     r2, r2, #0x00300000             @ drop pc[1:0]  (#0x300000 = 0x03 ror 12: encodable)
    add     r2, r3, r2, lsr #19             @ r3 + ((pc & 0xFFC) << 1) == &ibtc[(pc >> 2) & 0x3FF], 8-byte aligned
    ldm     r2, {r1, r3}                    @ r1 = tag, r3 = entry
    cmp     r1, r0                          @ a misaligned r0 can never equal an aligned tag (or 1) → falls through
    bxeq    r3                              @ hit (≈ 92-96 % for JR $ra)
    b       jit_dispatch                    @ miss: full lookup (alignment test, tables, compile)
```
Fills happen **only in C** (`jit_lookup_slow` / `jit_compile_at_pc`) and only with the entry of a real compiled `JitBlock` (RAM or BIOS pc) — never `&jit_compile_stub`, never a pinned stub (`0xA0/0xB0/0xC0/0x30000`: OpenBIOS calls the vectors through `li $t2,0xA0 ; jr $t2`), never a non-cacheable pc. Invariant: the IBTC is purged on every block invalidation and flush. `cfg.jit_ibtc = 0` disables it for A/B benchmarking.

### 5.6 Register allocator & constant propagation

Per block, two passes:

1. **Scan**: decode all instructions; count reads/writes per MIPS reg; propagate constants (`known[32]`, `kval[32]`: `LUI/ORI/ADDIU/ADDI/ANDI/XORI/SLL/SRL/SRA/OR/AND/ADDU/SUBU/SLT*` on known inputs stay known; `$zero` always known 0; any load, MFHI/LO, MFC*, or unknown op clears the dest). Record for each memory op whether its address is a compile-time constant and which region it hits.
2. **Allocate**: top-7 regs by `reads + 2*writes` get `r4,r5,r6,r7,r8,r12,lr` for the whole block (no live-range splitting in v1). Each slot has `state ∈ {UNLOADED, CLEAN, DIRTY}`. First read emits `LDR`; first write marks DIRTY without load. Writes to `$zero` are dropped. Reads of unallocated regs go through `r0-r3` temporaries (`LDR r1,[r11,#4*n]`), writes `STR` immediately.
3. **Materialising constants** (ARMv6K, no MOVW/MOVT): try `MOV #imm8-ror`, then `MVN`, then `MOV+ORR` (2 chunks), else `LDR rX, =const` from the block's literal pool. Emitter helper `emit_const(reg, value)` is the *only* way to load a constant.
4. **Dirty writeback** happens at: block end, every exception stub (only regs dirty at that site — the allocator records a 7-bit dirty mask per instruction), and *never* for plain slow-path memory stubs (helpers don't read `gpr[]`; if they raise an exception they set `jit_abort`, and the abort path in the stub performs the writeback itself).

Instruction selection notes (exact MIPS semantics):

* `ADD/ADDI/SUB` (trapping): `ADDS/SUBS` + `BVS exc_stub(OV)`; `ADDU/ADDIU/SUBU`: plain.
* `MULT/MULTU`: `SMULL/UMULL lo,hi` (1 insn!); `DIV/DIVU`: `emit_call(jit_div_helper, false)` (asm leaf, clobbers r0-r3 only, implements div-by-zero: `lo = rs<0 ? 1 : -1, hi = rs` for DIV, `lo = 0xFFFFFFFF, hi = rs` for DIVU; `INT_MIN/-1 → lo=INT_MIN, hi=0`). HI/LO live in memory (`[r11,#HI/LO]`); MFHI/MFLO after MULT read them back (cached value forwarding when adjacent).
* Shifts by register: `AND r1, rs, #31` then `LSL/LSR/ASR` (MIPS masks to 5 bits, ARM uses 8 ⇒ mask required).
* `SLT/SLTU/SLTI/SLTIU`: `CMP` + `MOVLT/MOVLO #1` / `MOVGE #0`.
* `LWL/LWR/SWL/SWR`: `emit_call(jit_lwl_helper, true)` with `(cpu, addr, old_rt)` in `r0-r2` (C, uses `psx_read32`; honours the pending-load bypass rule of §5.7).
* `LWC2/SWC2` (GTE load/store, used by every 3D game): same fast path as `LW/SW` with the data register being `gte_d[rt]` (`LWC2` applies the MTC2 write quirks for regs 15/28/30; `SWC2` the MFC2 read quirks). `LWC0/1/3`, `SWC0/1/3`, `COP1/COP3` ops: coprocessor-usable check below.
* **Coprocessor usability** (`cop.exe`): `COPn` ops (incl. `LWCn/SWCn`) require `SR.CUn` (bits 28+n); `COP0` is additionally usable when `SR.KUc == 0`. Violation → `PSX_EXC_CPU (11)` with `Cause.CE = n` (not RI). Enabled `COP1/COP3` ops are NOPs. The JIT emits **one** `SR.CU2` test (`ldr r2,[r11,#SR]; tst r2,#1<<30; beq exc_stub_cpu2`) at the first COP2 instruction of a `JB_HAS_GTE` block (SR cannot change inside a block: `MTC0 SR`/`RFE` terminate it).
* `JALR rd, rs` with `rd == rs`: read the target **before** writing `rd = pc + 8`. Link writes (`JAL/JALR/BLTZAL/BGEZAL`) are visible to the delay slot. `REGIMM` encodings with `(rt & 0x1E) == 0x10` link `$ra` unconditionally (even when not taken), after evaluating the condition on the old `$rs` (matters when `rs == $ra`).
* `SYSCALL/BREAK`: exception stub. `RI`: unknown opcodes → exception stub (`cop.exe` tests COP1/COP3 usability).
* COP0: `MTC0 SR` → `emit_call(cpu_set_sr, true)`; `MTC0 CAUSE` → `emit_call(cpu_set_cause, true)` (only bits 9:8 writable); `RFE` → inline `SR = (SR & ~0xF) | ((SR >> 2) & 0xF)` then `emit_call(cpu_check_irq, true)`. All three **terminate the block with an unlinked dispatch** (§5.4.1). **When they sit in a branch delay slot** (`jr $k0; rfe` is *every* exception return!) they are emitted in the reordered slot position and the enclosing branch's control transfer is still performed, but through the unlinked `jit_dispatch` (no IBTC fill, no direct link) — if `cpu_check_irq` found a newly unmasked IRQ it zeroed `r10`, so the target block's entry check exits immediately and `cpu_dispatch_irq` records `EPC = $k0`. `MFC0`, `MTC0` to other registers: inline (`cop0[6/8/15]` read-only, `DCIC/BPC/BDA…` stored).
* **IRQ delivery**: never checked in generated code. Peripherals that raise an IRQ call `cpu_request_exit()`; the next block entry fails its cycle check → `sched_run` → `cpu_dispatch_irq`.

### 5.7 Delay slots, load delays, exceptions

* A block always ends **after** the delay slot of its terminating branch; the slot instruction is emitted *before* the control transfer (Delay-Slot Reordering). If the slot writes a register the branch condition reads, the condition is evaluated first into a scratch flag register (`CMP` result kept in a GPR via `MOVxx`) — simpler: evaluate `CMP` and `MOVcc r3,#1` *before* emitting the slot, then branch on `r3`.
* **Load delay inside a block** (identical rules in `cpu_interp.c`, `cpu_cached.c`, `arm_jit.c`; the 48 `CPU MEM DLY` cases of `psxtest_cpu`): the pipeline state is one `pending = {reg, value}`. (a) Instruction *i+1* reads the **old** `rt` (incl. as a base register for its own effective address). (b) If *i+1* writes `rt` itself (ALU, `MFHI`, …), the **pending load is cancelled** (the ALU result wins). (c) Back-to-back loads into the same `rt`: load 1 commits after load 2 executes (load 2 computed its address with the old `rt`), load 2 commits one instruction later. (d) `LWL/LWR` into the same `rt` as a pending load **merge against `pending.value`** (MEM→MEM bypass) while still using the old `gpr[rs]` for the address. (e) A pending load at a block boundary is written to `load_delay_reg/val` and resolved per the next bullet. The JIT implements (a)–(d) by keeping the loaded value in a scratch/newly-mapped slot and committing it to the architectural slot after *i+1*.
* **Load in a branch delay slot** (`JB_ENDS_LOAD_DS`): the load's result is written back normally **unless the statically known target's first instruction reads `rt`**. For direct branches both targets are peeked at compile time (their pages are added to `phys_page[]` so SMC in the target invalidates this block too — `JB_PEEKED_TARGET`). If the first target instruction reads `rt`, or the target is indirect (`JR`), the block ends with `jit_exit_step_one` → `jit_run` executes `cpu_step_until_clean()` with `load_delay_*` populated, then re-enters. Measured frequency is < 0.5 % of branches.
* **Branch in a delay slot**: the JIT refuses the block (ends it before the first branch and emits `jit_exit_step_one`; `cpu_step_until_clean` then runs the whole branch/branch/slot sequence); the interpreter implements the R3000A behaviour: the second branch is executed, its delay slot is the first instruction at the first branch's target, and control then continues at the second branch's target (verify against Mednafen; test `cpu/psxtest_cpx.exe`).
* **Exceptions in delay slots**: `exc_stub` passes `bd=1` and the branch's pc (`pc_i - 4`) so `Cause.BD` and `EPC` are right.
* `GTE` in a delay slot: fine (no special case). `MTC2/CTC2` followed immediately by a GTE op: no hazard emulation (same as DuckStation default).

### 5.8 Invalidation (SMC) protocol — no code patching

```c
void jit_invalidate_page(PsxSystem *sys, u32 phys_page);            /* write slow path, bus_dma_write_words */
void jit_invalidate_range(PsxSystem *sys, u32 phys, u32 bytes);     /* loops over pages with code_page_refs != 0 */
void jit_flush_all(PsxSystem *sys);                                  /* cache full / savestate load / engine switch */
```
For each block `B` in `page_head[phys_page]`: `tab_ram[BLOCKTAB_RAM_IDX(B.pc)] = &jit_compile_stub`; unlink `B` from both page lists; `bus_code_page_unref` for each of its pages; `B.flags |= JB_DEAD`. Then purge the IBTC (all tags = 1) and `cpu_request_exit()`. Dead code stays in the cache unreachable until the next `jit_flush_all` (it is reachable only via (a) the table — now reset, (b) the IBTC — now purged, (c) direct `B`s from blocks whose pages ⊆ B's pages — those blocks are in the same page lists and die in the same call). No instruction is modified, so **no I-cache maintenance is needed for invalidation**; `jit_flush_all` sets `cpu.icache_dirty = 1` and the next `jit_enter` performs exactly one `jit_mem_icache_flush_all()` before reusing addresses. The currently executing block runs to its end (hardware keeps executing stale I-cache lines as well; software always `FlushCache`s after writing code). HLE `A0:0x44 FlushCache` is a **no-op for the JIT** (write-trapping is already exact) — in particular it must not drop `blocktab_bios`.

### 5.9 Memory access code generation

Dynamic address (base register not constant), **load word** — 6 instructions, branch-free on the RAM/BIOS/scratchpad path (`BEQ` not taken ≈ 1 cycle):

```armasm
    add     r1, rB, #imm                    @ omitted if imm == 0; emit_const + ADD if not encodable
    tst     r1, #3                          @ only if cfg.jit_check_align (default ON; AdEL)
    bne     exc_stub_adel_j
    lsr     r2, r1, #12
    ldr     r2, [r9, r2, lsl #2]            @ entry (0 = slow)
    cmp     r2, #0
    ldrne   rD, [r2, r1]                    @ 2-insn fast path proper
    beq     slow_stub_j
cont_j:
slow_stub_j:                                @ out of line, after the block
    push    {r12, lr} ; str r10,[r11,#CYC] ; mov r0, r11 ; @ r1 already = addr
    bl      psx_bus_read32_slow             @ u32 f(PsxCpu*, u32 addr)
    ldr     r10,[r11,#CYC] ; ldr r9,[r11,#FM_R] ; pop {r12, lr}
    ldr     r2, [r11, #PSX_OFF_JIT_ABORT] ; cmp r2, #0 ; bne abort_stub_j   @ helper recorded an exception (AdEL/AdES/DBE)
    mov     rD, r0
    b       cont_j
```

`abort_stub_j` (one per site that can fault; shares code with `exc_stub` generation): `<STR dirty regs live at site j>` → `add r10, r10, #(CYCLES - cycles_before_j)` → `ldr r2,[r11,#PSX_OFF_JIT_ABORT]; sub r1, r2, #1` (code) → `mov r0, #0; str r0,[r11,#PSX_OFF_JIT_ABORT]` → `ldr r2, =pc_j ; mov r3, #bd_j ; <branch_target of the enclosing branch in gte_scratch[0] if bd_j>` → `b jit_raise_exception_exit` (asm: syncs `r10`, calls `cpu_raise_exception(sys, code, pc, bd, branch_target, cpu->abort_badvaddr, 0)`, then `b jit_exit`). Precise `EPC/BD/JUMPDEST/BADVADDR` therefore come from the *site*, never from the stale `cpu->pc`. `jit_enter` sets `cpu->in_jit = 1`, `jit_exit` clears it, so the same helpers serve the interpreters.

`LH/LHU/LB/LBU` use `LDRSH/LDRH/LDRSB/LDRB rD,[r2,r1]` (register-offset forms exist for all). **Store word**:

```armasm
    add     r1, rB, #imm
    tst     r1, #3 ; bne exc_stub_ades_j    @ optional
    ldr     r3, [r11, #PSX_OFF_FASTMEM_W]   @ write table pointer (swapped on IsC) — 1 extra LDR vs loads
    lsr     r2, r1, #12
    ldr     r2, [r3, r2, lsl #2]
    cmp     r2, #0
    strne   rV, [r2, r1]
    beq     slow_stub_j                     @ MMIO / code page / IsC / ROM / unmapped
```

**Constant addresses** (`known[rB]`): compute `page = (base+imm) >> 12` at compile time.
* Read of RAM/BIOS/scratch page: `ldr r2, =host_addr ; ldr rD, [r2]` (host RAM/BIOS/scratch never move) — 2 instructions, no table.
* Read of MMIO: `emit_call(psx_bus_read32_slow, true)` with the constant address in `r1` (no table lookup), followed by the `jit_abort` test.
* Store to RAM: still goes through `fm_w` (page may become a code page or IsC may flip) but with the index pre-folded: `ldr r3,[r11,#FM_W] ; ldr r2,[r3,#imm12]` when `page*4 < 4096`, else `add r3, r3, #page*4` (rotated imm or 2 ops).
* Store to MMIO constant: `emit_call(psx_bus_write32_slow, true)` + `jit_abort` test.

### 5.10 Idle-loop skipping

Pattern detector at compile time (block flagged `JB_IDLE_LOOP`):
1. Block's terminator branches to its own start (or `B self; NOP`).
2. Body contains **no stores, no GTE, no COP0 writes, no SYSCALL**, at most one load, and that load's address is constant (MMIO or RAM) or the body has no load at all.
3. Registers written in the body are only used as branch inputs (e.g. `lw v0,GPUSTAT; andi v0,v0,0x400; beqz v0,loop`).

For such blocks the back-edge becomes: `mov r0,#1 ; str r0,[r11,#PSX_OFF_IDLE_SKIP] ; mov r10,#0 ; b exit_stub` — the scheduler then **jumps time to the next event** (§4). Counted delay loops are folded arithmetically **only** for the exact shape `loop: addiu rX,rX,-1 ; bnez rX,loop ; nop` (decrement *before* the branch; when the `addiu` is in the delay slot the loop exits with `rX == -1` and must not be folded) and only at runtime when `0 < rX <= 0xFFFF` and `rX*CYCLES <= r10` (else the block executes normally): `r10 -= rX*CYCLES ; rX = 0`. Both optimisations are disable-able (`cfg.jit_idle_skip`).

### 5.11 Inline GTE emitter (`jit_gte.c`) — exactness rules

Inlined: `RTPS, RTPT, NCLIP, AVSZ3, AVSZ4, MVMVA (mx ∈ 0..2 / v / cv / sf / lm specialised at compile time; `mx == 3` — the garbage-matrix encoding tested by `gte-test-all.exe` — is bounced to C), DPCS, INTPL, SQR, OP, GPF, GPL`. Everything else → `emit_call(gte_exec_c, false)` with `(cpu, opcode)` in `r0/r1` (slice A's C reference, already exact). Before any inline GTE sequence the emitter **spills DIRTY slots and marks all 7 slots UNLOADED** (they are reloaded lazily afterwards) — CLEAN slots would otherwise be silently clobbered by the GTE scratch usage. `MFC2/MTC2/CFC2/CTC2` inline (`LDR/STR [r11,#GTE_D/C+4*n]` with the per-register read/write quirks: `SXYP` push, `IRGB/ORGB` conversion, `LZCS→LZCR` via `CLZ`, sign-extension of 16-bit control regs on read, `FLAG` bit 31 derivation on write).

**44-bit MAC accumulation.** `SMLAD/SMUAD` sum two 16×16 products into 32 bits; the edge case `(-0x8000)*(-0x8000) + (-0x8000)*(-0x8000) = 0x80000000` **overflows** (Q set, result reads as −2³¹) and realistic inputs (TR·4096 + 3 products) exceed 32 bits anyway. Therefore every MAC1-3 computation uses the **64-bit dual-MAC family**: `SMLALD` (dual 16×16 into a 64-bit accumulator — the two products are added at 64-bit width, so the −0x8000² pair is exact) and `SMLALBB/BT/TB/TT`. MAC0 (NCLIP, projection, AVSZ) uses `SMLALD/SMLSLD/SMULL` the same way. 32-bit `SMLAD` is used only where the result is provably < 2³¹ (e.g. `IR·RGB` colour products ≤ 2²³).

Rotation row packing (GTE control regs): `c32 = R12:R11`, `c33 = R21:R13`, `c34 = R23:R22`, `c35 = R32:R31`, `c36 = ---:R33` ⇒

```armasm
@ After the spill, r4-r8, r12, lr are GTE scratch. Matrix words rC32..rC36 are loaded once per op with LDM from
@ [r11, #PSX_OFF_GTE_C]. V = r0 (VY:VX packed), r1 = VZ (sign-extended), TR_k in r2, 64-bit accumulator lo/hi = r3/r4.
@ Hardware checks 44-bit overflow after EVERY accumulation step; SMLALD would hide an overflow of the first step
@ (TR<<12 + product) that a later product cancels. Such inputs need |TR| >= 2^30, so each row starts with a 2-insn guard
@ that bounces the whole op to gte_exec_c (gte-test-all.exe exercises exactly this):
    adds    r5, r2, #0x40000000              @ (u32)(TR + 2^30) >= 2^31  ⇔  |TR| >= 2^30
    bmi     gte_bounce_k                     @ → emit_call(gte_exec_c) for this opcode, then continue after the op
@ MAC1 = TRX<<12 + R11*VX + R12*VY + R13*VZ
    mov     r3, r2, lsl #12
    mov     r4, r2, asr #20
    smlald  r3, r4, r0, rC32                 @ += R11*VX + R12*VY
    smlalbb r3, r4, r1, rC33                 @ += R13*VZ
@ MAC2 = TRY<<12 + R21*VX + R22*VY + R23*VZ
    smlalbt r3, r4, r0, rC33                 @ VX(lo) * R21(hi of c33)
    smlaltb r3, r4, r0, rC34                 @ VY(hi) * R22(lo of c34)
    smlalbt r3, r4, r1, rC34                 @ VZ    * R23(hi of c34)
@ MAC3 = TRZ<<12 + R31*VX + R32*VY + R33*VZ
    smlald  r3, r4, r0, rC35
    smlalbb r3, r4, r1, rC36
@ 44-bit overflow flag test (3 insns inline, bit set out of line):
    movs    r2, r4, asr #11                  @ Z → fits (positive)
    cmnne   r2, #1                           @ Z → fits (negative)
    bne     gte_ovf_stub_k                   @ sets FLAG bit 30/27 (MAC1) etc. by sign of r4, returns
@ shift (sf): sf=1: mac = (lo >> 12) | (hi << 20); sf=0: mac = lo
    mov     r2, r3, lsr #12
    orr     r2, r2, r4, lsl #20
@ IR saturation: lm=0 → SSAT 16 ; lm=1 → USAT 15 (0..0x7FFF) ; flag = (sat != mac)
    ssat    r0, #16, r2
    cmp     r0, r2
    orrne   rFLAG, rFLAG, #(1<<24)           @ IR1 flag (bit 24 / 23 / 22)
```

Other exactness points the emitter must honour (all verified by `gte/psxtest_gte.exe`, `gte-test-all.exe`, `gte-fuzz.exe`):
* RTPS/RTPT: **the IR3 saturation flag (bit 22) is computed from `raw44_3 >> 12` against −0x8000..0x7FFF ignoring both `sf` and `lm`**, while the IR3 *value* uses the real `sf` shift and `lm` clamp; **`SZ3 = raw44_3 >> 12` from the 44-bit sum** (not from the 32-bit `MAC3` when `sf=0`), saturated to 0..0xFFFF (flag 18).
* Division: `gte_div_unr(h, sz3)` is an **asm leaf** (`CLZ`-based normalisation, 257-entry table from `gte_tables.c`, Newton step exactly as research §3.3), returns 17-bit result and sets flag 17 on overflow (`h >= sz3*2`). Projection: `MAC0 = (s64)recip*IR1 + OFX` via `SMULL + ADDS/ADC`; flag 16/15 on 32-bit overflow of the 64-bit value; `SX2 = (s32)(MAC0 >> 16)` from the **64-bit** value, saturated to −0x400..0x3FF (`SSAT #11`, flags 14/13). Depth cue: `MAC0 = recip*DQA + DQB`, `IR0 = MAC0 >> 12` clamped to **0..0x1000 inclusive** (flag 12) — `USAT #12` gives 0..0xFFF and is wrong; use `cmp/movlt/movgt`.
* `NCLIP`: `MAC0 = SX0*(SY1-SY2) + SX1*(SY2-SY0) + SX2*(SY0-SY1)` in 64-bit (`SMLALBT/SMLALBB` on packed SXY), flags 16/15.
* `AVSZ3/4`: `MAC0 = ZSF3*(SZ1+SZ2+SZ3)` 64-bit, `OTZ = MAC0 >> 12` saturated 0..0xFFFF (flag 18).
* `MVMVA` with `cv=2` (Far Color) reproduces the hardware bug: FC added only to the *first* product then flushed through IR saturation (lm respected) before adding the remaining two products. `v=3` reads IR1-3.
* `FLAG` bit 31 = OR of bits 30..23 and 18..13 (mask `0x7F87E000` — **not an ARM imm8-rotate constant**: `ldr r2, =0x7F87E000 ; tst rFLAG, r2 ; orrne rFLAG, rFLAG, #0x80000000`), computed once at the end of each op; FLAG is reset to 0 at the start of every op. `CTC2 FLAG`: `FLAG = v & 0x7FFFF000`, then the bit-31 derivation. `CFC2` sign-extends the 16-bit control registers `c36 (R33), c44 (L33), c52 (LB3), c58 (H — yes, H reads back sign-extended), c59 (DQA), c61 (ZSF3), c62 (ZSF4)`; `MFC2` quirks: `r7 OTZ`, `r16-19 SZx` zero-extend, `r8-11 IRx` sign-extend, `r28 IRGB`/`r29 ORGB` compute from IR1-3 (`clamp(IRx>>7, 0, 0x1F)`), `r31 LZCR = CLZ/CLO of LZCS`.
* Colour FIFO push (`RGB0←RGB1←RGB2`), SXY FIFO push, SZ FIFO push: 3 `LDR/STR` pairs each (or `LDM/STM`).

Measured budget (ARM11, N3DS): inline `RTPS` ≈ 90 cycles vs ≈ 300 for a C call; `NCLIP` ≈ 20 vs 120.

### 5.12 Code memory & W^X (`jit_mem.h`; impl: `jit_mem_host.c` [B], `platform/3ds/jit_mem_3ds.c` [F])

```c
typedef enum { JIT_MEM_NONE = 0, JIT_MEM_MIRROR_RWRX, JIT_MEM_RWX_INPLACE, JIT_MEM_HOST_MMAP } JitMemMode;
typedef struct { u8 *rw; u8 *rx; u32 size; JitMemMode mode; bool luma_icache_svc; } JitMem;
PsxResult jit_mem_alloc(JitMem *m, u32 size);            /* ALWAYS provides a valid RW buffer m->rw (cpu_cached stores CachedOps there);
                                                            mode == JIT_MEM_NONE ⇒ rx == NULL ⇒ caller selects the cached interpreter */
void      jit_mem_free(JitMem *m);                       /* unmap mirror / free buffer (shutdown, engine switch) */
void      jit_mem_commit(JitMem *m, const void *rw_begin, u32 bytes);   /* make [rw,rw+bytes) executable at rx */
void      jit_mem_icache_flush_all(JitMem *m);           /* after jit_flush_all / block invalidation */
#define JIT_RW2RX(m, p) ((u8*)(p) + ((m)->rx - (m)->rw))
```

Host (T2): `mmap(NULL, size, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS)`, `rw == rx`, `commit = __builtin___clear_cache(rx, rx+bytes)` (QEMU TB invalidation + `cacheflush` syscall).

3DS (T1) — decision chain, **always compare `rc == 0`, never `R_SUCCEEDED()`** (Azahar leaves `r0 == handle` for unimplemented SVC 0x70, see toolchain_env §6.1):
1. `memalign(0x1000, size)` RW buffer; `svcGetProcessId` + `svcOpenProcess(&proc, pid)` (`CUR_PROCESS_HANDLE` is rejected by SVC 0x70).
2. Find a free VA window: walk `svcQueryMemory` from `0x04000000` upward in 1 MiB steps until a `MEMSTATE_FREE` run ≥ size (do **not** hardcode `0x07000000`/`0x08000000` — `0x08000000` is the heap). `svcControlProcessMemory(proc, va, rw, size, MEMOP_MAP, MEMPERM_READ|MEMPERM_EXECUTE)`; `rc == 0` ⇒ `JIT_MEM_MIRROR_RWRX`.
3. Else `svcControlProcessMemory(proc, rw, 0, size, MEMOP_PROT, RWX)`; `rc == 0` ⇒ `JIT_MEM_RWX_INPLACE`.
4. Else: if `svcGetSystemInfo(&out, 0x10000, 0) == 0` (Luma present) ⇒ `JIT_MEM_NONE` → **cached interpreter** (a prefetch abort is certain on real W^X kernels). If Luma is absent we are inside an HLE emulator (Azahar/Citra), where heap pages are executable: use `rw` as `rx` with `mode = RWX_INPLACE` only when `cfg.jit_allow_hle_exec` (default ON; `--no-hle-exec` for tests).
5. `jit_mem_commit`: `svcFlushProcessDataCache(CUR_PROCESS_HANDLE, rw, bytes)` (cleans L1D to L2/FCRAM and makes Azahar's Dynarmic drop stale translations for that range), then `mcr p15,0,r0,c7,c10,4` (DSB) + `mcr p15,0,r0,c7,c5,4` (prefetch flush) — both EL0-safe. **`c7,c5,0` (invalidate I-cache) faults in user mode; `__builtin___clear_cache` is NOT usable on 3DS.**
6. `jit_mem_icache_flush_all`: if Luma's extended `svcInvalidateEntireInstructionCache` (0x94) is available (probe once via the Luma system-info call) use it; otherwise execute `icache_evict_loop` once (a 128 KiB `nop` slide on N3DS = 4× the 32 KiB I-cache; 64 KiB on O3DS). It runs **only** when `cpu.icache_dirty` is set, i.e. once per `jit_flush_all`, at the next `jit_enter` — never per page invalidation and never on the hot path. Code memory is **append-only** between flushes and no committed instruction is ever rewritten (§5.4.1, §5.8), so freshly committed code can never be stale in the I-cache.

`jit_mem_alloc` and the mode are reported in telemetry (`stats.jit_mem_mode`) and on the bottom screen.

---

## 6. Interpreter & Cached-Block Interpreter (slices A and B)

* `cpu_interp.c` (A): the **reference**. Straight `switch` decode, exact load-delay pipeline (`load_delay_reg/val`), `in_delay_slot/branch_target/branch_taken`, exceptions with `BD`/`EPC`, GTE via `gte_exec_c`, memory via `psx_read*/psx_write*` inline fastmem. Runs on all targets; is the oracle for `trace_diff.py`.
* `cpu_cached.c` (B): pre-decodes blocks into `CachedOp { u8 op; u8 rs, rt, rd; u32 imm; }` arrays stored in the same append-only code region and dispatched with computed-goto threaded code; shares `blocktab_*`, `JitBlock`, page lists and the invalidation protocol with the JIT (so SMC handling is identical) and uses `sched_run` the same way. This is the 3DS fallback when `jit_mem_alloc` returns `JIT_MEM_NONE` and the fast engine on x86_64. Expected ≈ 2.5× the plain interpreter.
* Engine selection: `cfg.cpu_engine ∈ {AUTO, INTERP, CACHED, JIT}`; AUTO = JIT if available else CACHED.

---

## 7. Dual GPU Architecture

### 7.1 Shared GP0/GP1 front-end (`gpu.c`, slice C) — written once

```c
typedef struct GpuVertex { s16 x, y; u8 r, g, b, _pad; u8 u, v; u16 _pad2; } GpuVertex;      /* 12 bytes */
typedef enum { PRIM_TRI = 0, PRIM_QUAD, PRIM_LINE, PRIM_POLYLINE_SEG, PRIM_RECT } GpuPrimKind;
enum { PF_GOURAUD = 1, PF_TEXTURED = 2, PF_RAW_TEX = 4, PF_SEMI = 8, PF_RECT_FLIP_X = 16, PF_RECT_FLIP_Y = 32 };
typedef struct GpuPrim {
    u8  kind, flags, abr, tex_depth;   /* abr/tex_depth from texpage (E1 or per-prim for polys) */
    u16 texpage;                       /* raw 16-bit texpage attribute (bits 0-8 + 11) */
    u16 clut;                          /* raw 16-bit clut attribute */
    u16 w, h;                          /* PRIM_RECT size */
    GpuVertex v[4];
} GpuPrim;
typedef struct GpuDrawEnv {
    s16 clip_x0, clip_y0, clip_x1, clip_y1;   /* E3/E4 (inclusive) */
    s16 off_x, off_y;                         /* E5 (sign-extended 11-bit) */
    u8  tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;  /* E2 (×8 applied by backend) */
    u8  set_mask, check_mask, dither, draw_to_display;  /* E6 / E1 */
    u16 texpage_e1;                           /* last E1 */
} GpuDrawEnv;
typedef struct GpuDisplay { u16 vram_x, vram_y; u16 hres, vres; u8 interlace, color24, blank, pal; u16 x1, x2, y1, y2; } GpuDisplay;

typedef struct GpuBackend {
    const char *name;
    void (*reset)(void *self);
    void (*set_draw_env)(void *self, const GpuDrawEnv *env);
    void (*draw)(void *self, const GpuPrim *p, const GpuDrawEnv *env);
    void (*fill_rect)(void *self, u16 x, u16 y, u16 w, u16 h, u32 bgr24);          /* GP0(02): 16px x-aligned, no mask */
    void (*vram_written)(void *self, u16 x, u16 y, u16 w, u16 h);                  /* after shadow VRAM updated (A0 / fill) */
    void (*vram_copy)(void *self, u16 sx, u16 sy, u16 dx, u16 dy, u16 w, u16 h);   /* GP0(80) with mask semantics */
    void (*sync_to_shadow)(void *self, u16 x, u16 y, u16 w, u16 h);                /* make shadow VRAM authoritative (C0, savestate, soft display) */
    void (*present)(void *self, const GpuDisplay *d);                              /* VBlank */
    void (*destroy)(void *self);
} GpuBackend;

typedef struct PsxGpu {
    u16 *vram;                          /* 1024x512 shadow VRAM (authoritative for gpu_soft; CPU-side mirror for gpu_hw) */
    u32 gpustat;                        /* bits recomputed by gpu_update_stat() */
    GpuDrawEnv env; GpuDisplay disp;
    /* GP0 state machine */
    enum { GP0_IDLE, GP0_COLLECT, GP0_POLYLINE, GP0_IMG_LOAD, GP0_IMG_STORE } st;
    u32 fifo[16]; u8 fifo_n, fifo_need; u8 cmd;
    u16 xfer_x, xfer_y, xfer_w, xfer_h, xfer_cx, xfer_cy; u32 xfer_words_left;
    u32 gpuread_latch; bool gpuread_valid;
    u32 irq1;  u8 dma_dir;  u8 tex_disable;
    const GpuBackend *be; void *be_self;
    u32 frame; PsxStats *stats;
} PsxGpu;
void gpu_gp0_write(PsxSystem*, u32 word);     /* CPU write & DMA2 words both come here */
void gpu_dma_write_words(PsxSystem*, const u32*, u32 n);  /* DMA2 burst (linked-list payloads / block mode) — same decoder */
u32  gpu_dma_read_words(PsxSystem*, u32 *dst, u32 n);     /* DMA2 VRAM→CPU: returns words produced */
void gpu_gp1_write(PsxSystem*, u32 word);
u32  gpu_read(PsxSystem*);                    /* GPUREAD */
/* backends (gpu_soft.h [C] / gpu_hw.h [D]): */
PsxResult gpu_soft_backend_create(PsxSystem*, const GpuBackend **be, void **self);
PsxResult gpu_hw_backend_create  (PsxSystem*, const GpuBackend **be, void **self);   /* returns PSX_ERR_NOT_IMPL on hosts (gpu_hw_stub.c) */
u32  gpu_stat(PsxSystem*);                    /* GPUSTAT with live bits 26/27/28/31 */
```

GP0 decode table `gp0_len[256]` (words incl. command; 0 = variable): `0x00 nop`, `0x01 clear cache`, `0x02 fill (3)`, `0x1F irq1 (1)`, polygons `0x20-0x3F` = `1 + nverts*(1 + tex + gouraud) - gouraud` (gouraud colour of v0 is the command word: 4/5/7/9/6/8/9/12 per research §4.1), lines `0x40-0x5F` (poly-line variants terminate on `(w & 0xF000F000) == 0x50005000`), rects `0x60-0x7F` (2/3/4 words), `0x80 copy (4)`, `0xA0 load (3 + ceil(w*h/2))`, `0xC0 store (3)`, `0xE1-0xE6 (1)`. Quads are emitted as two `PRIM_TRI` (`v0,v1,v2` and `v1,v2,v3`) by the front-end so both backends see only triangles, lines, rects. Large-primitive rejection (`w > 1023 || h > 511` → skip) happens here. GP1: `00 reset`, `01 reset fifo`, `02 ack irq1`, `03 display enable`, `04 dma dir`, `05 display start`, `06/07 h/v range`, `08 display mode`, `09 texture disable`, `10 get info (texwindow, draw area, offset)`.

`GPUSTAT` bits: 26 (ready for cmd) = `st == GP0_IDLE || st == GP0_COLLECT`; 27 (VRAM→CPU ready) = `st == GP0_IMG_STORE`; 28 (DMA ready) = `st != GP0_IMG_STORE`; 25 per `dma_dir`; **31** = in interlaced 480-line mode the current *field*, in progressive mode `scanline & 1` during active display and `0` during VBlank (updated by `EV_HBLANK`; `VBLANK.exe` polls it); **13** = field in interlace, constant 1 in progressive mode; **never leave 26/28 at 0 after a command completes** (boot-loop trap). `EV_GPU` optionally models command duration (`cfg.gpu_timing`, default on: polygons `≈ pixels/2` GPU clocks) to keep DMA2-completion IRQ ordering realistic.

### 7.2 `gpu_soft` (slice C) — ARMv6-SIMD software rasterizer

* Fixed-point 12.4 edge functions, top-left fill rule matching PS1 (pixel centers at integer+0; PS1 omits bottom/right edges), draw offset applied before rasterisation, clip to `env.clip_*`, dithering (4×4 PS1 matrix) when `dither && (gouraud || (textured && !raw))`, mask set/check, texture window per research §4.3 formula, CLUT 4/8/16-bit fetch with texpage base, black-texel transparency (texel `0x0000` not drawn), STP bit gating semi-transparency per texel, 4 ABR modes with saturation.
* Hot spans in `gpu_soft_span_armv6.S` (C twin in `gpu_soft_span.c`, bit-identical): `span_flat`, `span_gouraud` (`UADD16` dual-channel steppers), `span_tex4/8/16_{flat,mod}`, each with `_blend` variant (`UHADD8`/`UQADD8`/`UQSUB8` on 0:B8:G8:R8 expanded pixels via the 32K-entry `rgb555_to_888[]` table; pack back with `USAT`+`PKHBT`), `span_fill16`. The ABR3 (`B + F/4`) path: `UQADD8 B, (F >> 2 & 0x3F3F3F)`.
* Display: `present()` copies the display rectangle (15/24-bit) from shadow VRAM into the platform frame buffer (`plat_video_present(u16 *rgb555, w, h, pitch)`); 3DS uses a GX display transfer (RGB565 → top LCD) — no CPU conversion.

### 7.3 `gpu_citro3d` (slice D) — batched PICA200 backend

**Decision table (HW approximation list)**: exact — geometry, flat/gouraud, 4/8/16-bit textures through the cache, texture window via vertex-side wrap when the primitive stays inside one window period else per-triangle UV split (research §4.4), draw area scissor, 24-bit display, VRAM uploads/fills, PS1 texture modulation (`texel × colour / 128`: TEV `GPU_MODULATE` + `GPU_TEVSCALE_2` on RGB for `!PF_RAW_TEX` textured draws, plain `GPU_REPLACE` for raw). Approximate — mask bit (stencil for `set_mask`/`check_mask`; texel-STP→mask propagation lost), dithering (off; PICA has none — toggleable fake dither via TEV noise texture later), per-texel semi-transparency (handled with the 2-pass split below).

* **Render target**: `C3D_RenderTargetCreate(512 /*h*/, 1024 /*w*/, GPU_RB_RGBA5551, GPU_RB_DEPTH24_STENCIL8)` in VRAM (1 MiB colour + 2 MiB DS; O3DS `PSX3DS_SMALL_MEM` → no DS buffer, mask-bit disabled) — the full PS1 VRAM at 1:1. Display = textured quad from this target to the top LCD framebuffer target (240×400 rotated) with `Mtx_OrthoTilt`; PICA clip space `z ∈ [-1, 0]`.
* **Vertex ring**: 4096 × 16-byte vertices in `linearAlloc` (`GPU_SHORT x2 pos`, `GPU_SHORT x2 uv`, `GPU_UNSIGNED_BYTE x4 rgba`, `GPU_UNSIGNED_BYTE x4 aux{texmode,abr,window,pad}`), **double-buffered per frame** (2 rings) so the GPU reads frame N while CPU fills N+1; `GSPGPU_FlushDataCache` once per batch flush.
* **Batching key**: `{texture cache entry, abr, semi, texmode, draw area, stencil state}`. Primitives append to the current batch; a key change flushes (`C3D_DrawArrays(GPU_TRIANGLES, first, count)`). Lines are expanded to 2 triangles (1px wide quads); rects to 2 triangles with per-prim UV flip flags.
* **Semi-transparency**: untextured semi prims → one pass with the ABR blend state (research §4.3: ABR0 constant 0.5/0.5; ABR1 ONE/ONE; ABR2 reverse-subtract ONE/ONE; ABR3 const 0.25 / ONE). Textured prims: `GPU_RGBA5551` has a **1-bit** alpha, so the texture cache classifies every entry at decode time as `ALL_OPAQUE` (no texel has STP=1), `ALL_STP` or `MIXED` (per-entry flag computed while expanding the CLUT/15-bit page, so it is free). `ALL_OPAQUE`/`ALL_STP` entries stay `RGBA5551` (α = 0 for texel `0x0000`, α = 1 otherwise; alpha-test `> 0` discards transparent black) and a semi-transparent prim draws in **one** pass with blending on only for `ALL_STP`. `MIXED` entries (< 5 % in practice) are stored as `GPU_RGBA8` with α ∈ {0 (black), 128 (STP=1), 255 (opaque)} and semi-transparent prims using them are drawn in **two passes over the same vertex range**: pass A alpha-test `== 255` (opaque texels, blend off), pass B alpha-test `== 128` (blend on); the batch is flushed around the split.
* **Texture cache** (`gpu_texcache.c`): 64 entries, key = `{texpage x/y, depth, clut x/y, clut_hash}`; each entry is a `C3D_Tex` 256×256 `GPU_RGBA5551` (128 KiB; `GPU_RGBA8` 256 KiB for `MIXED`) in **linear FCRAM** (PICA textures from FCRAM; VRAM is reserved for the render target). CLUT expansion on CPU writes directly in **Morton order** (`gpu_morton.c`: 8×8 tile LUT; 64 entries `morton8x8[y][x]`); 15-bit pages use `C3D_SyncDisplayTransfer` with `GX_TRANSFER_OUT_TILED` from shadow VRAM. Invalidation: `vram_written()`/`fill_rect()`/`vram_copy()` mark a 64×32 grid of 16×16 dirty cells; entries overlapping dirty cells are evicted on next use; CLUT rows are hashed (32 or 512 bytes FNV-1a) so a CLUT rewrite with identical contents does not evict.
* **Shadow VRAM coherence**: `sync_to_shadow(rect)` issues a display transfer from the render target region to a linear staging buffer then `memcpy` to shadow VRAM (rare: GP0 C0 reads, render-to-texture detection — if a texture cache fetch overlaps a region drawn by the GPU this frame (`gpu_dirty` grid), it syncs that region first). Save states always call `sync_to_shadow(full)`.
* **Frame flow**: `present()` → `C3D_FrameEnd(0)`; frameskip (`cfg.frameskip 0-3`, auto mode skips when the previous frame missed vsync) skips the display pass only — the render target is always kept correct.

Vertex shader `psx_gpu.v.pica` (uniform `proj` = `Mtx_OrthoTilt(0, 1024, 512, 0, 0, 1, true)`; `.constf`/`.fvec` declared **before** `.proc main`; **one statement per line — `;` starts a comment in picasso**; `GPU_UNSIGNED_BYTE` attributes arrive as *unnormalised* floats 0..255, hence the `1/255` scale). V-flip convention (pinned for slices C/D): PS1 VRAM row 0 is the top; texture cache tiles are swizzled with `y` increasing downward (`gpu_morton.c`), display transfers use **no** `GX_TRANSFER_FLIP_VERT`, and `o_uv.y = v / 256` unflipped — the `OrthoTilt(top=0, bottom=512)` matrix already maps PS1 `y` downward.

```
; psx_gpu.v.pica
.fvec proj[4]
.constf consts(0.0, 1.0, 0.00390625, 0.003921569)   ; 0, 1, 1/256, 1/255
.in v_pos v0
.in v_uv  v1
.in v_col v2
.in v_aux v3
.out o_pos position
.out o_uv  texcoord0
.out o_col color
.proc main
    mov r0.xy, v_pos.xy
    mov r0.zw, consts.xy
    dp4 o_pos.x, proj[0], r0
    dp4 o_pos.y, proj[1], r0
    dp4 o_pos.z, proj[2], r0
    dp4 o_pos.w, proj[3], r0
    mul o_uv, consts.zzzz, v_uv
    mul o_col, consts.wwww, v_col
    end
.end
```

---

## 8. Peripherals

### 8.1 Interrupt controller (`irq.c`, A)

`I_STAT (0x1F801070)`, `I_MASK (0x1F801074)`; `irq_raise(sys, IRQ_n)` sets the bit (edge: only the 0→1 transition is observable); write to `I_STAT` is `stat &= value` (**write-0-to-clear**); after any change: `cpu_check_irq()` recomputes `cop0[13].IP2 (bit 10)` and `irq_pending = IEc && (SR & CAUSE & 0x0700)`; if it became pending → `cpu_request_exit()`. IRQ bits: 0 VBLANK, 1 GPU, 2 CDROM, 3 DMA, 4-6 TMR0-2, 7 SIO0, 8 SIO1, 9 SPU, 10 PIO.

### 8.2 DMA (`dma.c`, A; device hooks in E/C)

```c
typedef struct DmaChannel { u32 madr, bcr, chcr; } DmaChannel;
typedef struct PsxDma { DmaChannel ch[7]; u32 dpcr, dicr; u8 running_mask; } PsxDma;
/* device hooks (declared in dma.h, implemented by owners) */
u32  mdec_dma_in_words(PsxSystem*, const u32*, u32 n);  u32 mdec_dma_out_words(PsxSystem*, u32*, u32 n);  /* E */
void gpu_dma_write_words(PsxSystem*, const u32*, u32 n); u32 gpu_dma_read_words(PsxSystem*, u32*, u32 n); /* C */
u32  cdrom_dma_read_words(PsxSystem*, u32*, u32 n);                                                         /* E */
void spu_dma_write_words(PsxSystem*, const u32*, u32 n); u32 spu_dma_read_words(PsxSystem*, u32*, u32 n); /* E */
```
Modes: 0 immediate (whole BCR; mode-0 channels need **CHCR bit 28 (trigger)** in addition to bit 24), 1 request/blocks (`BS*BA` words; transferred in chunks of ≤ 256 words per `EV_DMA` tick so timers/CD IRQs interleave), 2 linked list (DMA2 only): per node read header `w` at `madr & 0x1FFFFC`, **transfer `size = w >> 24` words first, then** `next = w & 0xFFFFFF`, stop when `next & 0x800000`. Linked lists are **chunked across `EV_DMA` ticks** (≤ `cfg.dma_ll_max_nodes_per_tick` = 64 nodes or 256 words per tick, cost model 1 cycle/word) so the CPU keeps running concurrently — `chain-looping.exe` builds a self-looping chain and asserts `finished == false && irq == false` until it writes `D2_CHCR = 0` to abort; a synchronous hop-count guard would break it. Register semantics: `D6_CHCR` writable mask `0x51000000`, bit 1 reads back as 1 (`otc-test.exe`); any channel runs only if `DPCR` enables it (bit `3+4*ch`); DMA6 OTC: `n = BCR ? BCR : 0x10000` words written descending from `madr`, last `0x00FFFFFF`. Completion: `chcr.bit24 = 0` (and bit 28), `dicr` flag + IRQ3 when enabled (bit 23 master, per-channel enable bits 16-22, bit 31 = master flag). CD-ROM DMA3 costs 40 cycles/16 words. All RAM writes go through `bus_dma_write_words` (SMC-safe).

### 8.3 Timers (`timers.c`, A)

Three root counters per research §4.6; implementation is **lazy**: each timer stores `(base_value, base_tick, rate_num/den)` and computes `value = base + (now - base_tick) * rate` on read; `EV_TIMERn` is scheduled for the next target/overflow IRQ only when the IRQ is enabled (mode bits 4/5), one-shot vs repeat (bit 6), toggle vs pulse (bit 7); sync modes (bits 0-2) handled in `EV_HBLANK`/`EV_VBLANK` callbacks (pause/reset/gate). Dotclock source for Timer0 uses the 16.16 video-clock ratio from §4. `timers.exe`, `VBLANK.exe` are the gates.

### 8.4 CD-ROM (`cdrom.c`, `cdrom_image.c`, `cdrom_readahead.c`, E)

* Registers `0x1F801800-03` with 4-bank index; parameter FIFO (16), response FIFO (16), data FIFO (sector buffer 2340/2048), `INT1..INT5` **two-deep queue**: a new interrupt waits until the previous is acknowledged (write to `1F801803.bank1`). State machine: `IDLE / SEEKING / READING(N or S) / PLAYING / PAUSED`, commands 0x01-0x1D per psx-spx incl. `GetID` licence responses (`SCEA/SCEE/SCEI` from image region or `cfg.region`), `Setmode` (double speed, XA-ADPCM, 2340 raw), `Setloc/SeekL/SeekP/ReadN/ReadS/Pause/Stop/Init/Mute/Demute/Setfilter/GetlocL/GetlocP/GetTN/GetTD/Test(0x20)/ReadTOC`. Timings via `EV_CDROM`: ACK `0xC4E1`, seek `= 0x10000 + distance_sectors * 15` cycles (capped), sector period 1×/2×.
* Images: `cdrom_image.c` — `.cue/.bin` (multi-track, audio tracks for CDDA, pregap handling), `.iso` (2048 → synthesized 2352 headers), raw `.img`. Interface: `bool img_read_sector(CdImage*, u32 lba, u8 out[2352])`, `track_of_lba`, TOC. XA-ADPCM sectors (`Setmode.bit6`, `Setfilter`) are decoded in `spu_xa.c` path and mixed by the SPU thread; CDDA likewise (`Play`) at 44.1 kHz.
* **Readahead**: `CdReadahead { u8 buf[128][2352]; u32 lba[128]; alignas(32) atomic u32 head, tail; u32 want_lba; }`. The I/O thread (N3DS Core 2, else Core 1 low priority, host: pthread) fills ahead of `want_lba`; `cdrom_fetch_sector(lba)` is a ring hit (memcpy 2352 B) or a synchronous fallback read + ring reseed (counted in `stats.cd_ra_miss`). Seeks reseed the ring. 128 sectors = 301 KiB ≈ 1.7 s at 1×.

### 8.5 SPU (`spu*.c`, E)

* 24 voices: ADPCM decoder with the 4-instruction ARMv6 filter step (`spu_adpcm_armv6.S`: `SMUAD/SSAT/PKHBT`, C twin), 512-entry Gaussian table (4-tap), pitch stepping (`pitch & 0x3FFF`, max 0x3FFF), ADSR (linear/exponential, psx-spx step tables), loop flags (start/repeat/end), KON/KOFF/ENDX, noise generator, PMON, volume sweep, main/CD/external volumes, **reverb** (psx-spx algorithm, 22.05 kHz, all 32 registers), SPU RAM 512 KiB with IRQ address compare (IRQ9), transfer FIFO & DMA4 (write/read), `SPUSTAT` busy/mode bits mirrored from `SPUCNT` (bit 5-0 copy).
* Rendering unit = 32 stereo samples (`SPU_BATCH = 32`, 24,576 cycles). Two execution modes selected at init (and switched automatically):
  * **sync** (host, tests, and whenever `SPUCNT.IRQ9 enable` is set): `EV_SPU` renders the batch on the main thread; sample-exact IRQ9.
  * **async** (3DS default): main thread pushes `SpuCmd { u32 tick; u16 reg; u16 val; }` register writes and a `SPU_CMD_ADVANCE(32)` per `EV_SPU` into an SPSC ring (4096 entries); Core 1 thread applies commands in order and renders into 735-sample `ndsp` wave buffers (3 buffers, `NDSP_FORMAT_STEREO_PCM16`, 44100 Hz, `DSP_FlushDataCache` before `ndspChnWaveBufAdd`). SPU RAM is written in place by the main thread (DMA4) — benign race, data precedes KON in practice; the ring carries the KON writes so ordering versus register state is exact. Underruns are counted and filled with the last sample (no clicks).
* Output: 44.1 kHz s16 stereo; host writes `.wav` (`audio_wav.c`).

### 8.6 MDEC (`mdec.c`, E)

Commands `1 decode macroblock (+ DMA0 input stream, output 4/8/15/24-bit)`, `2 set quant tables`, `3 set IDCT table`; status register bits (FIFO empty/full, busy, DMA requests, current block, output depth/sign/bit15). RLE → dequant → IDCT (reference: 64-bit exact separable; ARMv6 `SMLAD` version in `mdec_idct_armv6.S`, must match reference bit-for-bit — `mdec-frame-15bit-dma.exe`) → YUV→RGB (psx-spx coefficients, 4:2:0). Output FIFO is paced by `EV_MDEC` (≈ 1 macroblock per 4,096 cycles) so DMA1 and the `FMV` loops behave.

### 8.7 SIO0 — Joypad & Memory Card (`sio0.c`, `joypad.c`, `memcard.c`, E)

`SIO_DATA/STAT/MODE/CTRL/BAUD` (0x1F801040-4E); TX of one byte schedules `EV_SIO0` at `baud_reload*8` cycles (≈ 1,088 at 0x88); device responds (digital pad `0x41 0x5A lo hi`, DualShock `0x73` + 4 analog bytes, config mode 0x43/0x44/0x4D subset), `/ACK` pulse 100 cycles later raises IRQ7 unless last byte. Memory card: 128 KiB images `sdmc:/psx3ds/memcards/<slot>.mcd` (raw 131,072 B, also accepts `.mcr/.srm` same layout), commands `R 0x52 / W 0x57 / S 0x53`, flag byte, XOR checksum, write-back to disk debounced 1 s on the I/O thread. Input source: `plat_input_poll(PsxPadState[2])`.

### 8.8 Peripheral struct skeletons (copied verbatim by `skeleton`; owners may append fields)

```c
/* spu.h [E] */
typedef struct SpuVoice { u32 start_addr, repeat_addr, cur_addr; u16 vol_l, vol_r, pitch, adsr_lo, adsr_hi, adsr_vol;
    s16 hist[2]; s16 ring[32]; u32 counter; u8 adsr_phase, block_flags, keyed, reverb_on, noise_on, pmon; s32 env_level; } SpuVoice;
typedef struct SpuCmd { u32 tick; u16 reg; u16 val; } SpuCmd;
typedef struct PsxSpu {
    u8  *ram;                           /* 512 KiB */
    u16 regs[0x200];                    /* raw register image 0x1F801C00..0x1F801FFF */
    SpuVoice v[24];
    u32 spucnt, spustat, irq_addr, xfer_addr, xfer_ctrl, noise_lfsr, kon, koff, endx;
    s32 reverb_regs[32]; u32 reverb_pos; s16 *reverb_buf;
    u16 fifo[32]; u8 fifo_n;
    s16 *out_ring; u32 out_w, out_r, out_cap;          /* rendered stereo frames (consumer: ndsp / wav) */
    SpuCmd *cmd_ring; alignas(32) u32 cmd_head; alignas(32) u32 cmd_tail;
    s16 xa_ring[2][4096]; u32 xa_w, xa_r;               /* XA-ADPCM / CDDA input from cdrom */
    bool async_mode, irq_enabled; u32 batch_tick;
} PsxSpu;
void spu_init(PsxSystem*); void spu_reset(PsxSystem*); void spu_state(PsxSystem*, StateIO*); void spu_shutdown(PsxSystem*);
void spu_render_batch(PsxSystem*, u32 frames);           /* sync mode: EV_SPU; async: worker thread */
void spu_push_xa(PsxSystem*, const s16 *stereo, u32 frames);

/* cdrom.h [E] */
typedef struct CdImage CdImage;                          /* opaque, cdrom_image.c */
typedef struct CdReadahead { u8 (*buf)[2352]; u32 *lba; alignas(32) u32 head; alignas(32) u32 tail; u32 want_lba, cap; } CdReadahead;
typedef struct PsxCdrom {
    CdImage *img; CdReadahead ra;
    u8  index, status, mode, irq_enable, irq_flag;
    u8  param_fifo[16], resp_fifo[16]; u8 param_n, resp_r, resp_w;
    u8  sector[2352]; u8 data_fifo[2352]; u32 data_r, data_n; bool data_ready;
    u8  pending_int[2], pending_resp[2][16], pending_n[2], pending_count;   /* two-deep INT queue */
    u32 seek_lba, cur_lba, setloc_lba; bool setloc_pending, reading, playing, paused, muted, double_speed, xa_filter_on;
    u8  xa_file, xa_channel, cmd, state; u32 state_tick;
    u8  vol[4], vol_pending[4]; u8 region;
} PsxCdrom;
void cdrom_init(PsxSystem*); void cdrom_reset(PsxSystem*); void cdrom_state(PsxSystem*, StateIO*); void cdrom_shutdown(PsxSystem*);
PsxResult cdrom_load(PsxSystem*, const char *path);     /* .cue/.bin/.iso/.img; NULL = tray empty */
void cdrom_readahead_thread(void *sys);                  /* run by platform on I/O core */

/* mdec.h [E] */
typedef struct PsxMdec {
    u32 status, control; u8 iq_y[64], iq_uv[64]; s16 idct[64];
    u16 in_fifo[64]; u32 in_r, in_w; u32 out_fifo[768]; u32 out_r, out_w;
    s16 blocks[6][64]; u32 cur_block, cur_coeff, words_left; u8 out_depth, out_signed, out_bit15; bool decoding;
} PsxMdec;
void mdec_init(PsxSystem*); void mdec_reset(PsxSystem*); void mdec_state(PsxSystem*, StateIO*); void mdec_shutdown(PsxSystem*);

/* sio.h [E] */
typedef struct PsxMemcard { u8 *data /*128 KiB*/; u8 flag, state, cmd, sector_hi, sector_lo, checksum; u32 pos; bool dirty, present; char path[256]; } PsxMemcard;
typedef struct PsxSio {
    u16 mode, ctrl, baud; u32 stat; u8 tx_data, rx_fifo[8]; u8 rx_r, rx_w; bool tx_pending, selected;
    u8 device /*0 none, 1 pad, 2 card*/, port, seq; u8 pad_mode[2], pad_config[2]; PsxMemcard card[2];
} PsxSio;
void sio_init(PsxSystem*); void sio_reset(PsxSystem*); void sio_state(PsxSystem*, StateIO*); void sio_shutdown(PsxSystem*);
void sio_set_pad(PsxSystem*, int port, const PsxPadState*);

/* system.h / bios.c [F] */
typedef struct PsxBios {
    bool loaded, hle; u32 exe_pc, exe_gp, exe_sp; char exe_path[256]; bool psexe_pending;
    char tty_line[256]; u32 tty_len; char until_pattern[64]; bool until_matched;
    u32 hle_heap_ptr; u32 hle_events[32][4];                 /* HLE kernel state */
} PsxBios;
bool bios_pc_hook(PsxSystem*, u32 pc);
```

---

## 9. BIOS, HLE, PS-EXE, Save States (slice F)

* **Real BIOS**: any 512 KiB image (`scph1001/5501/7001/7502…`) from `sdmc:/psx3ds/bios/` or `--bios`. **OpenBIOS** (`tests/roms/bios/openbios.bin`) is the default for CI — same loader path.
* **PS-EXE** (`psexe.c`): parse 2 KiB header (`"PS-X EXE"`, `pc0 @0x10, gp0 @0x14, t_addr @0x18, t_size @0x1C, s_addr @0x30, s_size @0x34`), copy `t_size` bytes to `t_addr` (via `bus_dma_write_words` so code pages are tracked), set `pc/gp/sp(fp)`, `sp = s_addr+s_size` or `0x801FFF00` if 0. Two modes: (a) *BIOS-boot inject* — run the BIOS until `pc == 0x80030000` (shell entry) then inject (default, needed for games/tests that call kernel functions); (b) *HLE* — no BIOS image: `hle_bios.c` installs exception stub at `0x80000080` (`j 0x80000000+HLE_EXC`), A0/B0/C0 trap vectors, and implements the subset: `A0: 0x00-0x0F file stubs (fail), 0x13 setjmp, 0x14 longjmp, 0x25-0x2B str*, 0x2A-0x2B memcpy/memset family, 0x33 malloc/0x34 free (bump), 0x3E puts, 0x3F printf (→ `plat_log`), 0x44 FlushCache (→ jit_flush_all), 0x49 GPU_cw, 0x70-0x72 _bu_init/_96_init/_96_remove`; `B0: 0x07-0x0D events, 0x12 InitPAD, 0x13 StartPAD, 0x17 ReturnFromException, 0x18 ResetEntryInt, 0x19 HookEntryInt, 0x3D putchar, 0x5B ChangeClearPAD`; `C0: 0x0A..0x0C ChangeClearRCnt, 0x12 InstallExceptionHandlers, 0x1C AdjustA0Table`. The trap is the **pinned block-table slots** `tab_ram[0xA0/0xB0/0xC0]` (§5.3; the vectors are RAM addresses, so without pinning the JIT would happily compile and direct-link them). With a real BIOS/OpenBIOS the same pins implement TTY capture: OpenBIOS boots with a dummy TTY, so `printf` output is only observable as `A0:0x3C putchar` / `B0:0x3D putchar` calls (`$t1` holds the function number at the vector) — `jit_bios_vec_stub` → `bios_vector_hook(sys, vec)` logs the character for `--until-printf`, then continues with the real vector code via `cpu_step_until_clean` (zero cost for all other calls after the first-byte check). OpenBIOS' shell handoff jumps to `0x80030000` (from `0xBFC08714`); `tab_ram[BLOCKTAB_RAM_IDX(0x30000)]` is pinned to `jit_psexe_stub` while `psexe_pending` and unpinned after injection.
* **Built-in OpenBIOS + ISO9660 CD boot (`v0.2.2`)**: `src/sys/openbios_blob.S` `.incbin`s `tests/roms/bios/openbios.bin` (512 KiB) into every target, so `psx_load_bios(sys, "builtin")` (and `"auto"` when `psx_bios_find_external()` finds no `scph*/psxonpsp*/bios*/openbios.bin` 512 KiB dump on SD) never needs romfs or SD files. `src/sys/cdboot.c` reads 2048-byte user data from the mounted `CdImage` (CHD / CUE+BIN / ISO), validates the PVD at LBA 16, walks directories (case-insensitive, `;1`, sub-dirs), parses `SYSTEM.CNF` `BOOT = cdrom:\\...` (`cdrom:`/`cdrom0:`, blanks, args; `PSX.EXE` fallback) and reads the boot PS-X EXE. `psx_load_cd()` then: **HLE** → direct inject (+ HLE kernel `cdrom:` `open/read/lseek/close`, `Load`, `Exec`, `LoadExec`, `CdReadSector`); **real BIOS / OpenBIOS, native (default)** → arm the `0x80030000` hook to *return immediately* (skip the BIOS shell/logo), so the BIOS kernel reads `SYSTEM.CNF` + EXE through the CD-ROM controller (Setloc/ReadN/INT1/DMA3) and executes it; a per-frame watchdog (`psx_cdboot_watchdog`, 30 s) sideloads if the EXE never lands in RAM; **fast boot** (`cfg.cd_fast_boot`, or OpenBIOS + a `SYSTEM.CNF` without `TCB`/`EVENT`, which makes OpenBIOS configure 0 EvCBs and hang) → sideload the parsed EXE at `0x80030000`. All steps log via `plat_log` (`[bios]`, `[cdboot]`, `[cdrom]`, `[hle]`), shown in the 3DS `4:LOG` tab and `sdmc:/psx3ds/psx3ds.log`. Covered by `tests/unit/test_sys_cdboot.c` (discs generated by `tests/unit/cdboot_disc.h`).
* **Save states** (`savestate.c`, `state.h`): `StateIO { bool writing; u8 *buf; u32 pos, cap; }` with `state_u32/u8/bytes/skip`; chunked file `"PSX3DSS\0" ver u32` then `{tag u32, len u32, payload}` for `CPU `, `BUS ` (RAM, scratch, mem_ctrl), `SCHD` (events as `when - now`), `IRQ `, `DMA `, `TMR `, `GPU ` (shadow VRAM after `sync_to_shadow`, env, disp, state machine), `SPU ` (RAM + voices + reverb), `CDRM` (incl. image path hash, LBA, FIFOs), `MDEC`, `SIO0`. Each module implements `void xxx_state(PsxSystem*, StateIO*)` in its own file (owner slice). Load = `jit_flush_all()` + `fastmem_build()` + IsC re-sync + IBTC purge + texture cache purge. 3DS writes states asynchronously from a snapshot on the I/O thread (≈ 4.3 MiB, optional LZ4-style RLE off by default).

---

## 10. Platform Layer, Threads, Frontends (slice F)

```c
/* platform.h */
u64  plat_time_us(void);
/* JIT code memory is NOT part of platform.h: the jit_mem.h interface (§5.12) is implemented by src/jit/jit_mem_host.c on
   hosts and src/platform/3ds/jit_mem_3ds.c on 3DS — one symbol set, selected by CMake. */
void plat_video_present(const u16 *rgb555, u16 w, u16 h, u16 pitch_px, bool is24);   /* soft backend */
void plat_audio_push(const s16 *stereo, u32 frames);
void plat_input_poll(PsxPadState pads[2], PsxHostKeys *keys);
typedef struct PlatThread PlatThread; PlatThread *plat_thread_create(void (*fn)(void*), void*, u32 stack, int prio, int core /* -1 any */);
void plat_thread_join(PlatThread*); void plat_sleep_us(u32); void plat_yield(void);
typedef struct PlatFile PlatFile;
PlatFile *plat_fopen(const char *path, const char *mode);   /* "rb"/"wb"/"r+b"; sdmc:/ or host paths */
u32  plat_fread(PlatFile*, void *dst, u32 bytes);  u32 plat_fwrite(PlatFile*, const void *src, u32 bytes);
bool plat_fseek(PlatFile*, u64 off);  u64 plat_fsize(PlatFile*);  void plat_fclose(PlatFile*);
bool plat_mkdir_p(const char *path);  u32 plat_listdir(const char *path, char (*names)[256], u32 max);
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

### 10.1 3DS threading model

| Core | Thread | Prio | Work |
|---|---|---|---|
| 0 | main | 0x30 | JIT/CPU, scheduler, GP0 parsing, citro3d batching, bottom-screen UI (≤ 1 ms/frame, drawn once per 4 frames) |
| 1 | `spu_thread` | 0x18 | SPU async rendering → `ndsp` (needs `APT_SetAppCpuTimeLimit(80)`, fallback 30) |
| 2 (N3DS) / 1 (O3DS) | `io_thread` | 0x3F | CD readahead, memcard/savestate writes, texture pre-swizzle (P2) |

Init order: `osSetSpeedupEnable(true)` (N3DS) → `APT_SetAppCpuTimeLimit` → `gfxInitDefault / C3D_Init(0x40000)` → `romfsInit`, `sdmc` dirs → `ndspInit()` (guard `R_FAILED` → silent audio, warn on UI: needs `sdmc:/3ds/dspfirm.cdc`) → `jit_mem_alloc` → threads. `threadCreate(..., core=2)` failing → retry with core 1. SPSC rings are `alignas(32)` with head/tail on separate lines and `__atomic_*` RELEASE/ACQUIRE (GCC emits the CP15 `c7,c10,5` barrier on ARMv6K — never write `dmb` mnemonics, they do not assemble for armv6k).

### 10.2 3DS frontend

* Top: emulated display (default 1:1 centered 320×240 with 400-wide letterbox; `cfg.scale ∈ {1:1, stretch, 4:3-fit}`; linear filtering toggle).
* Bottom 320×240 (`ui_bottom.c`, citro2d-free: own 8×8 font blitting to a linear RGB565 buffer + display transfer): ROM browser (`sdmc:/psx3ds/roms/*.cue|*.bin|*.iso|*.exe|*.psx`), per-game menu: save/load state slots 0-9, renderer toggle (HW/SW), frameskip 0-3/auto, cycle multiplier, SPU sync/async, JIT engine (JIT/cached/interp), memcard slots; live telemetry overlay (FPS, emulated speed %, ms CPU/GPU/present, JIT blocks/code KiB, IBTC hit %, slow-path reads/writes per frame, SMC traps, SPU ring fill, CD readahead hit %, `jit_mem_mode`). Touch + D-pad navigation. `START+SELECT` hold 1 s → menu; `L+R+X` quick state.
* Input mapping: D-pad/ABXY/L/R/Start/Select direct; Circle Pad = left analog (DualShock mode selectable), C-stick (N3DS) = right analog, ZL/ZR = L2/R2, L+R+Y → L3/R3.

### 10.3 Host CLI (`main_cli.c`) — T2 and T3

```
psx3ds-cli [--bios F|--hle] (--exe F | --cd F) [--engine jit|cached|interp] [--gpu soft]
           [--frames N | --seconds S | --until-printf "PATTERN"] [--dump-frame out.ppm[:every]] [--png]
           [--dump-audio out.wav] [--trace-hash out.txt --hash-every CYCLES] [--stats out.json]
           [--cycle-mult 448] [--no-ibtc] [--no-link] [--no-idle-skip] [--no-align-check] [--no-hle-exec]
           [--input script.txt]   # "frame:buttons" lines for deterministic input
```
Exit codes: 0 ok, 2 guest reported failure (`--until-printf` pattern matched a FAIL line), 3 timeout. Determinism: no wall-clock in the emulation path; `--frames` counts VBlanks.

---

## 11. Testing & Benchmark Suite (slice F owns the harness; every slice adds cases)

| Layer | Tool | What |
|---|---|---|
| Unit (ctest, T3) | `tests/unit/test_*.c` | layout asserts, fastmem bias table (every mapped page round-trips; all NULL pages enumerated), scheduler wrap-around (events across `2^32`), I_STAT semantics, GP0 decode lengths, ADPCM/Gaussian vs reference vectors, IDCT exactness, UNR table, emitter encodings (compare bytes against hand-assembled expectations for every `emit_*`), regalloc invariants |
| ROM golden (T3 + T2) | `tests/rom_runner.sh` | runs every `tests/roms/**/*.exe` with `--until-printf` or `--frames`, compares `sha256(frame)` and printf transcript to `tests/golden/<rom>.{sha256,txt}`; T2 runs the **same** ROMs under `qemu-arm -cpu arm11mpcore` with `--engine jit` and must match T3's interpreter goldens bit-for-bit |
| JIT oracle | `tests/trace_diff.py` | runs `--engine interp` (T3) and `--engine jit` (T2) with `--trace-hash --hash-every 10000` (hash of `gpr/hi/lo/pc/cop0[12,13,14]`), reports the first divergent cycle; the primary JIT debugging tool |
| 3DS smoke | `tests/3ds/` + `azahar-headless --sdmc … --done-file` | boots `HelloWorld16BPP.exe` and `Cube.exe`, dumps `sdmc:/screen.ppm`, checks `jit_mem_mode` log line, compares frame hash to the soft-GPU golden with tolerance (HW backend) |
| Bench | `psx3ds-bench` (same binary, `--bench`) | per ROM in `tests/roms/benchmarks/` (`PSXNICCC.exe`, `Cube.exe`, `gpu-benchmark.exe`) prints JSON: `emu_fps`, `host_ms_per_frame`, `mips_insns_per_frame`, `cycles_per_frame`, `blocks_compiled`, `code_kib`, `ibtc_hit_pct`, `slow_rd/wr_per_frame`, `smc_traps`, `gte_inline_pct`, breakdown `{jit_exec, compile, events, gpu, spu}` from `plat_time_us` around each phase; under QEMU add `-plugin libinsn.so -d plugin` for host-instruction counts (deterministic, machine-independent regression metric) |

CI gate order: unit → T3 goldens → T2 goldens + trace_diff on `psxtest_cpu/psxtest_gte/gte-fuzz` → 3DS build + Azahar smoke → bench report diff (fail if any metric regresses > 5 %).

---

## 12. Memory Budget & Performance Targets

Single arena allocated once in `psx_system_init()` (`src/core/arena.c`, 4 KiB-aligned bump allocator; `PSX3DS_SMALL_MEM` chosen at runtime by `APT_CheckNew3DS` / `envGetHeapSize() < 64 MiB`).

**Startup heap split (`v0.2.1`)**: `main_3ds.c` overrides libctru's weak `__system_allocateHeaps` using `psx_ctr_compute_heap_split()` (`include/psx3ds/ctr_heap.h`): normal heap = `min(RESLIMIT_COMMIT remaining - 20 MiB, 80 MiB)` at `0x08000000` (must stay inside the Kernel11 heap VA window `[0x08000000, 0x0E000000)` = 96 MiB, else `svcControlMemory` fails and libctru panics before `main()` — the `v0.2.0` N3DS crash), linear heap = remainder (≥ 20 MiB). O3DS 64 MiB → 42.6 + 20 MiB; N3DS 124 MiB → 80 + 42.6 MiB. Covered by `tests/unit/test_ctr_heap.c`.

| Item | N3DS | O3DS (`SMALL_MEM`) | host |
|---|---|---|---|
| RAM + scratch + BIOS | 2.5 MiB | 2.5 MiB | 2.5 MiB |
| `fm_r` + `fm_w` (IsC handled in place, no third table) | 4+4 MiB | 4+4 MiB | 8+8 MiB |
| JIT code cache | 16 MiB | 6 MiB | 32 MiB |
| Block tables (RAM 2 MiB + BIOS 0.5 MiB) + pool | 2.5 + 2 MiB | 2.5 + 1 MiB | 5 + 2 MiB |
| IBTC | 8 KiB | 8 KiB | 16 KiB |
| Shadow VRAM | 1 MiB | 1 MiB | 1 MiB |
| HW GPU: render target (VRAM) / texture cache (linear) / vertex rings | 3 / 8 / 0.25 MiB | 1 / 4 / 0.25 MiB | — |
| SPU RAM + rings + ndsp buffers | 0.6 MiB | 0.6 MiB | 0.6 MiB |
| CD readahead | 0.3 MiB | 0.3 MiB | 0.3 MiB |
| **Total FCRAM** | **≈ 41 MiB** | **≈ 26 MiB** | n/a |

Targets: N3DS 60 fps in all 2D titles and the heavy-3D benchmark set at 1:1 with HW GPU; O3DS ≥ 45 fps 2D / ≥ 30 fps 3D with HW GPU; JIT ≥ 3.5× cached interpreter on `psxtest_cpu`; inline GTE ≥ 3× C GTE on `Cube.exe`.

---

## 13. Sharp Subtleties Resolved (checklist for reviewers)

1. **Scratchpad shares 64KB page `0x1F80` with MMIO** → 4KB-granular 1M-entry LUT; scratchpad fast, MMIO trapped; `0xBF800000` (KSEG1 scratchpad) deliberately unmapped → DBE.
2. **Bias math** is modular `uintptr_t`, verified for all 3 segments × 4 mirrors, BIOS and scratch; never `u8*` arithmetic; `u32` addresses zero-extend on x86_64.
3. **`SR.IsC`** handled by `fastmem_set_isolated()` rewriting the 6146 writable `fm_w` entries (≈25K cycles, only inside `FlushCache`; `cpu_set_sr` is the only SR writer) — zero per-store cost, no 4 MiB third table; isolated stores are dropped, never reach RAM (boot-time RAM-wipe trap).
4. **SMC**: NULL write entries only for 4KB pages holding compiled code; identical-value rewrites don't invalidate; invalidation only resets table slots + purges the IBTC — **no instruction is ever rewritten after commit** (direct `B` only to targets whose pages ⊆ the predecessor's pages, everything else jumps through the block-table slot), so no I-cache SVC is needed except once per full flush (deferred via `icache_dirty`); DMA writes funnel through `bus_dma_write_words`.
5. **`SMLAD` −0x8000² × 2 = 0x80000000 overflows (Q)**; all GTE MACs use 64-bit `SMLALD/SMLALxy`; 44-bit overflow flags from the high word in 3 inline instructions.
6. **RTPS IR3 flag quirk** (computed from `MAC3>>12` regardless of `sf`), 64-bit `MAC0` projection with `SX = MAC0_64 >> 16`, UNR division in a `CLZ` asm leaf, `MVMVA cv=2` bug reproduced.
7. **R3000A pipeline**: load-delay rules (a)–(e) incl. pending-load cancellation and `LWL/LWR` MEM→MEM bypass; load in a branch delay slot across blocks: static peek of the target's first instruction (page dependency recorded) or `cpu_step_one` fallback; **branch in delay slot** never compiled; `jr $k0 ; rfe` handled by emitting the COP0 op in the slot and dispatching unlinked; coprocessor-usable checks raise `CpU` with `Cause.CE`; `JUMPDEST`/`BADVADDR` rules; `LWC2/SWC2` on the fast path.
8. **IRQ latency without per-block checks**: peripherals call `cpu_request_exit()` → next block entry fails the cycle test; `r10` is synced across every helper so the scheduler's `sched_now()` is exact.
9. **AAPCS**: `jit_enter` pushes 10 registers (40 B) to keep `sp` 8-aligned; the single `emit_call` primitive wraps every `BL` in `PUSH/POP {r12, lr}` (8 B) which also makes `r12/lr` usable as cache slots → 7 cached MIPS GPRs; block entry is `cmp r10,#0 ; ble ; sub` so skipped blocks never consume cycles; misaligned `JR` targets are caught before any `LDR pc`.
10. **3DS W^X**: check `rc == 0` (not `R_SUCCEEDED`), `svcOpenProcess` handle, probe a free VA with `svcQueryMemory`, never `c7,c5,0` in EL0, never `__builtin___clear_cache` on 3DS, append-only code cache with **no post-commit instruction rewriting**, Luma 0x94 or NOP-eviction loop for I-cache exactly once per full flush (deferred `icache_dirty`), `svcFlushProcessDataCache` + DSB/ISB on every newly committed range.
11. **No `dmb` on ARMv6K** → `__atomic_*` (GCC emits CP15 `c7,c10,5`) in `spsc.h`; `alignas(32)` head/tail.
12. **GPUSTAT 26/28** derived from the parser state and never left low, bit 31/13 progressive-mode semantics; `I_STAT` is `&=`; CD-ROM INT queue is two-deep and acknowledge-gated; DMA2 linked lists transfer-then-advance and are chunked per `EV_DMA` tick (self-looping chains stay abortable); BIOS vectors `0xA0/B0/C0` and the shell entry `0x30000` are pinned table slots (TTY capture, HLE, PS-EXE injection); instruction fetch from scratchpad → IBE, from MMIO → allowed.
13. **PICA200**: `.constf` before `.proc`, one picasso statement per line, byte attributes are unnormalised (×1/255), `GPU_TEVSCALE_2` for PS1 modulation, `GPU_CULL_NONE`, `z ∈ [-1,0]` via `Mtx_OrthoTilt`, `RGBA5551` 1-bit alpha ⇒ texture entries classified `ALL_OPAQUE/ALL_STP/MIXED` with `RGBA8` + 2-pass split only for `MIXED`, texture cache in linear FCRAM not VRAM, `GSPGPU_FlushDataCache` on vertex rings.
14. **Timing constants** pinned: 2152.56 cycles/line NTSC, 263 lines (59.826 Hz); 768 cycles/sample; CD ACK 0xC4E1; all in `hw_regs.h`, nowhere else.

---

## 14. Delivery Order (for the coordinator)

1. `skeleton` (1 worker, ~2 h): all headers of §3–§10 verbatim, `cpu_layout.h` + `test_layout.c` passing on T1/T2/T3, CMake for the three targets, empty `.c` with signatures, `tests/rom_runner.sh` stub. **Nothing else starts before this is on `main`.**
2. Parallel wave 1 (6 worktrees): A core (bus/fastmem/scheduler/interp/GTE-C), B jit (emitter + thunks + dispatcher on T2 first), C gpu (parser + soft), D gpu_hw (citro3d, tested in Azahar), E periph (CD/SPU/SIO/MDEC/DMA hooks), F sys (loaders, CLI, platform, bench, goldens).
3. Integration gates: (G1) OpenBIOS boots to shell on T3 interpreter; (G2) `psxtest_cpu/gte` PASS on T3; (G3) same on T2 with JIT + trace_diff clean; (G4) `.3dsx` boots `Cube.exe` in Azahar with HW GPU; (G5) bench report baseline; (G6) real-hardware run (N3DS Luma) — `jit_mem_mode == MIRROR_RWRX` expected.
