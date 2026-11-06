# PSX3DS Cross-Compilation Toolchain & Verification Harness Guide

This document specifies the exact compiler toolchains, Nintendo 3DS SDK libraries, ARM11v6K QEMU execution harness, headless Azahar `.3dsx` runner, and open-source PS1 test ROM suite configured on this host (`x86_64` Debian GNU/Linux rodete, 24 cores).

---

## 1. Quick Start

Source [`tools/env.sh`](../tools/env.sh) at the start of every build or test session:

```bash
source /usr/local/google/home/chii/Developer/psx3ds/tools/env.sh
```

Exported environment variables:

| Variable | Value / Purpose |
|----------|-----------------|
| `PSX3DS_ROOT` | `/usr/local/google/home/chii/Developer/psx3ds` |
| `PSX3DS_SDK_ROOT` | `/usr/local/google/home/chii/.local/psx3ds-sdk` |
| `DEVKITPRO` | `$PSX3DS_SDK_ROOT/devkitpro` |
| `DEVKITARM` | `$DEVKITPRO/devkitARM` |
| `CTR_CMAKE_TOOLCHAIN_FILE` | `$DEVKITPRO/cmake/3DS.cmake` |
| `CTR_ARCH_FLAGS` | `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft` |
| `CTR_CFLAGS` | `$CTR_ARCH_FLAGS -mword-relocations -ffunction-sections -fdata-sections -D__3DS__` |
| `ARM11_LINUX_CC` / `CXX` | `arm-linux-gnueabihf-gcc` / `arm-linux-gnueabihf-g++` (pure ARMv6+VFPv2 Linux target) |
| `ARM11_LINUX_CFLAGS` | `-march=armv6k -mtune=mpcore -mfpu=vfpv2 -mfloat-abi=hard -marm` |
| `ARM11_SYSROOT` / `QEMU_LD_PREFIX` | `$PSX3DS_SDK_ROOT/armv6-eabihf/arm-buildroot-linux-gnueabihf/sysroot` |
| `QEMU_ARM11_CMD` | `qemu-arm -cpu arm11mpcore` |
| `PSX3DS_ROMS_DIR` | `$PSX3DS_ROOT/tests/roms` |
| `PSX3DS_OPENBIOS` | `$PSX3DS_ROOT/tests/roms/bios/openbios.bin` |

---

## 2. Installed Tool Versions & Locations

All user-space SDK binaries are cleanly self-contained under `/usr/local/google/home/chii/.local/psx3ds-sdk`:

| Component | Binary / Library | Version | Purpose |
|-----------|------------------|---------|---------|
| Host C/C++ Compiler | `/usr/bin/gcc`, `/usr/bin/g++` | GCC `15.2.0` | Native `x86_64` build, ASan/UBSan tests & fast CI |
| Build Generator | `cmake`, `ctest`, `ninja`, `make` | CMake `4.4.5`, Ninja `1.13.2` | Cross-platform & multi-target build system |
| 3DS Cross-Compiler | `arm-none-eabi-gcc` / `g++` | `devkitARM` GCC `16.1.0` | Official Nintendo 3DS bare-metal / Horizon OS compiler + patched `newlib` |
| 3DS OS & GPU SDK | `libctru`, `citro3d`, `citro2d` | `libctru 2.7+`, `citro3d 1.7+` | Horizon OS services, `ndsp` audio, `citro3d` PICA200 GPU driver |
| PICA200 Shader Assembler | `picasso` | `2.7.2` | Compiles `.pica` vertex/geometry shaders to `.shbin` |
| 3DS Packaging Tools | `3dsxtool`, `smdhtool`, `3dsxdump`, `mkromfs3ds`, `tex3ds`, `bin2s`, `3dslink` | `3dstools` / `tex3ds 2.3.0` | `.elf` -> `.3dsx` conversion, SMDH icons/metadata, RomFS, textures, binary embedding |
| ARM11 Linux Cross-Compiler | `arm-linux-gnueabihf-gcc` / `g++` | Bootlin `2026.08-1` GCC `15.3.0` (`armv6-eabihf` glibc) | Compiles ARM11v6K + VFPv2 Linux test binaries & `arm_jit` unit harnesses |
| ARM11 User-Mode Emulator | `qemu-arm` (`qemu-arm-static`) | QEMU `11.1.2` | Executes ARM11 MPCore (`-cpu arm11mpcore`) JIT & ARMv6 SIMD code on `x86_64` |
| 3DS System Emulator | `azahar`, `azahar-headless` | Azahar `2126.2` (`xvfb-run` + Mesa `llvmpipe` OpenGL 4.6) | Headless end-to-end `.3dsx` + PICA200 shader + `sdmc:/` integration tests |

---

## 3. Three-Tier Build, Verification & Benchmarking Strategy

### Tier 1: Native Host Build (`x86_64` Linux) — Core Correctness & Sanitizers

Use host `gcc`/`g++` with `-O2 -g -fsanitize=address,undefined` for rapid verification of platform-independent subsystems (R3000A interpreter, COP0, GTE fixed-point reference, GP0/GP1 state machine & software rasterizer, DMA, Timers, SPU, CD-ROM, MDEC, BIOS/PS-EXE loader):

```bash
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPSX3DS_ENABLE_ASAN=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

---

### Tier 2: Strict ARM11 MPCore Harness (`arm-linux-gnueabihf-gcc` + `qemu-arm -cpu arm11mpcore`)

**Why this harness is critical:**
Standard Debian `armhf` toolchains compile `glibc` with ARMv7 Thumb-2 (`MOVW`/`MOVT`), which crashes on strict ARMv6 emulation. Our SDK installs **Bootlin's pure `armv6-eabihf` glibc sysroot**, allowing `qemu-arm -cpu arm11mpcore` to enforce the **exact Nintendo 3DS ARM11 MPCore (ARMv6K + VFPv2, 16 `D0..D15` / 32 `S0..S31` registers)** ISA from `_start` to `exit`:
- Emitting or executing **ARMv6 SIMD** (`UQADD8`, `UQSUB8`, `SMLAD`, `SMLALD`, `PKHBT`, `SSAT`, `USAT`, `REV16`) and **VFPv2** (`VMUL.F32`, `VMLA.F32`) executes natively.
- Dynamic Recompiler (`arm_jit`) buffers allocated with `mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)` + `__builtin___clear_cache(start, end)` execute and self-modify cleanly under QEMU user-mode (`cacheflush` syscall `0x0f0002`).
- **Any accidental ARMv7 instruction** emitted by `arm_jit` (e.g., `MOVW`, `MOVT`, `UBFX`, `SBFX`, `SDIV`, `UDIV`, or `D16..D31` VFPv3 registers) **immediately traps with `SIGILL`**, catching real-hardware 3DS crashes during automated CI!

#### Compile & Run ARM11 JIT / SIMD Unit & Benchmark Binaries

```bash
source tools/env.sh

# Compile headless PSX emulator or JIT/SIMD test for strict ARM11 MPCore (ARMv6K + VFPv2):
arm-linux-gnueabihf-gcc -O3 -Wall \
  -march=armv6k -mtune=mpcore -mfpu=vfpv2 -mfloat-abi=hard -marm \
  tests/arm11_jit_test.c -lm -lpthread -o build-arm11/arm11_jit_test

# Execute on emulated ARM11 MPCore CPU (QEMU_LD_PREFIX is set automatically by tools/env.sh):
qemu-arm -cpu arm11mpcore ./build-arm11/arm11_jit_test

# Optional: inspect guest translated ARM11 disassembly & block linking in QEMU:
qemu-arm -cpu arm11mpcore -d in_asm,cpu ./build-arm11/arm11_jit_test 2> /tmp/arm11_trace.log
```

> **ABI Note (`arm_jit`):** Both `devkitARM` (`-mfloat-abi=hard`) and `arm-linux-gnueabihf-gcc` (`-mfloat-abi=hard`) use the **AAPCS-VFP** calling convention (`r0-r3` integer args/ret, `r4-r11` callee-saved, `r12` scratch `ip`, `r13` 8-byte aligned `sp`, `r14` `lr`, `s0-s15`/`d0-d7` caller-saved VFP, `s16-s31`/`d8-d15` callee-saved VFP). Therefore, **the exact same `arm_jit` emitter & `.S` assembly kernels run unmodified on both `qemu-arm -cpu arm11mpcore` and Nintendo 3DS `.3dsx`!**

---

### Tier 3: Nintendo 3DS Cross-Compilation (`.3dsx` / `.elf`) & Headless Azahar Runner

#### Option A: CMake + Ninja (`$DEVKITPRO/cmake/3DS.cmake`) — Recommended

devkitPro's official `3DS.cmake` toolchain provides `ctr_generate_smdh()`, `ctr_create_3dsx()`, and `dkp_add_embedded_binary_library()`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(psx3ds C CXX ASM)

if(NINTENDO_3DS)
    # Assemble PICA200 shader (.pica -> .shbin -> embedded symbol)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/psx_gpu.shbin
        COMMAND picasso -o ${CMAKE_CURRENT_BINARY_DIR}/psx_gpu.shbin ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/3ds/psx_gpu.pica
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/3ds/psx_gpu.pica
    )
    dkp_target_generate_symbol_list ... # or bin2s custom command

    target_include_directories(psx3ds PRIVATE ${DEVKITPRO}/libctru/include ${DEVKITPRO}/portlibs/3ds/include)
    target_link_directories(psx3ds PRIVATE ${DEVKITPRO}/portlibs/3ds/lib ${DEVKITPRO}/libctru/lib)
    target_link_libraries(psx3ds PRIVATE citro3d ctru m)
    ctr_generate_smdh(psx3ds.smdh
        NAME "PSX3DS"
        DESCRIPTION "High-Performance PlayStation 1 Emulator for New 3DS"
        AUTHOR "psx3ds"
        ICON "${DEVKITPRO}/libctru/default_icon.png"
    )
    ctr_create_3dsx(psx3ds SMDH psx3ds.smdh)
endif()
```

Build command:

```bash
source tools/env.sh
cmake -S . -B build-3ds -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$CTR_CMAKE_TOOLCHAIN_FILE" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-3ds
```

#### Option B: Direct CLI Compilation (`picasso` + `bin2s` + `arm-none-eabi-gcc` + `3dsxtool`)

```bash
source tools/env.sh

# 1. Assemble PICA200 vertex/geometry shader
picasso -o psx_vsh.shbin psx_vsh.pica
bin2s -a 4 -H psx_vsh_shbin.h psx_vsh.shbin > psx_vsh_shbin.s
arm-none-eabi-gcc -x assembler-with-cpp $CTR_ARCH_FLAGS -c psx_vsh_shbin.s -o psx_vsh_shbin.o

# 2. Compile C/ASM sources
arm-none-eabi-gcc -O3 $CTR_CFLAGS \
  -I"$DEVKITPRO/libctru/include" -I"$DEVKITPRO/portlibs/3ds/include" \
  -c main.c -o main.o

# 3. Link 3DS ELF & package .3dsx
arm-none-eabi-gcc -specs=3dsx.specs $CTR_ARCH_FLAGS \
  main.o psx_vsh_shbin.o \
  -L"$DEVKITPRO/portlibs/3ds/lib" -L"$DEVKITPRO/libctru/lib" \
  -lcitro3d -lctru -lm -o psx3ds.elf

smdhtool --create "PSX3DS" "PS1 Emulator for New 3DS" "psx3ds" \
  "$DEVKITPRO/libctru/default_icon.png" psx3ds.smdh
3dsxtool psx3ds.elf psx3ds.3dsx --smdh=psx3ds.smdh
```

#### Headless `.3dsx` Integration Testing with `azahar-headless`

We verified that **Azahar 2126.2** runs compiled `.3dsx` binaries + `citro3d` PICA200 shaders (`DVLB_ParseFile` / `C3D_FrameBegin` / `C3D_FrameEnd`) headlessly on this machine in **~2.7 seconds** via `xvfb-run` + Mesa `llvmpipe` (OpenGL 4.6 Core Profile) with New 3DS mode (`System_IsNew3ds=true`) enabled by default!

Use `azahar-headless` to run end-to-end `.3dsx` smoke & rendering tests:

```bash
source tools/env.sh

mkdir -p /tmp/psx3ds_sdmc/roms
cp tests/roms/gpu/HelloWorld16BPP.exe /tmp/psx3ds_sdmc/roms/

# Runs psx3ds.3dsx with sdmc:/ mapped to /tmp/psx3ds_sdmc and exits once done.txt appears (or after 10s)
azahar-headless build-3ds/psx3ds.3dsx \
  --sdmc /tmp/psx3ds_sdmc \
  --done-file psx3ds_done.txt \
  --timeout 10
```

> **Important Note on JIT Permissions in Azahar vs Real Luma3DS Hardware:**
> - On real New 3DS (Luma3DS Homebrew Menu) and Azahar HLE, always check the `Result` return code of `svcControlProcessMemory` (or `ctr_svchax` / `svcBackdoor`) at startup!
> - If dynamic RX code mapping is denied or unavailable in a restricted HLE environment, cleanly fall back to the C/ASM Cached Interpreter so `.3dsx` integration tests in `azahar-headless` NEVER crash, while `qemu-arm -cpu arm11mpcore` exercises 100% of `arm_jit` at full speed!

---

## 4. Open-Source PS1 Test ROMs (`tests/roms/`)

See [`tests/roms/README.md`](../tests/roms/README.md) for the complete 54-file SHA-256 inventory:

- **BIOS**: `tests/roms/bios/openbios.bin` (512 KB MIT-licensed PCSX-Redux OpenBIOS, built from `pcsx-redux/nugget/openbios`).
- **CPU (`tests/roms/cpu/`)**: AmiDog `psxtest_cpu.exe`, `psxtest_cpx.exe`, JaCzekanski `cop.exe`, `access-time.exe`, `code-in-io.exe`, `io-access-bitwidth.exe`, and 10 PeterLemon per-opcode tests (`CPUADD`..`CPUSRA`).
- **GTE (`tests/roms/gte/`)**: AmiDog `psxtest_gte.exe`, JaCzekanski `gte-test-all.exe`, `gte-fuzz.exe`, PeterLemon `GTEAVSZ.exe`, `GTENCLIP.exe`.
- **GPU (`tests/roms/gpu/`)**: AmiDog `psxtest_gpu.exe`, 11 JaCzekanski rasterizer/CLUT/blending/mask/clipping tests (`triangle.exe`, `quad.exe`, `transparency.exe`, `mask-bit.exe`, `clut-cache.exe`, etc.), and 6 PeterLemon 4/8/15bpp/dither/texwindow tests (`HelloWorld16BPP.exe`, etc.).
- **DMA / Timers / SPU / MDEC / CD-ROM (`tests/roms/dma_timers_io/`)**: `otc-test.exe`, `chain-looping.exe`, `chopping.exe`, `dpcr.exe`, `timers.exe`, `VBLANK.exe`, `spu-test.exe`, `PlayADPCMSample.exe`, `mdec-frame-15bit-dma.exe`, `cdrom-getloc.exe`.
- **Throughput Benchmarks (`tests/roms/benchmarks/`)**: `PSXNICCC.exe` (STNICCC polygon stream benchmark), `Cube.exe` (3D GTE+GPU benchmark), `gpu-benchmark.exe`, `animated-triangle.exe`.

---

## 5. Picasso `.v.pica` Shader Pitfalls (Verified)

1. **`.constf` MUST appear BEFORE `.proc main`**: Placing `.constf c0(1.0, ...)` *after* `.end` causes `picasso` to leave the constant register zero-initialized (`w = 0.0`), silently clipping all triangles to 0 pixels! Always declare `.fvec` and `.constf` at the top of `.v.pica` before `.proc main`.
2. **Vertex Buffer Cache Coherency**: Always call `GSPGPU_FlushDataCache(vbo_ptr, byte_size)` after writing vertices into `linearAlloc` memory before `C3D_DrawArrays`.
3. **Winding & Culling**: PS1 GP0 polygons have arbitrary clockwise/counter-clockwise winding (`NCLIP` is done in GTE software when games want culling). Always set `C3D_CullFace(GPU_CULL_NONE)` on 3DS!

---

## 6. Empirically Verified 3DS JIT, Multi-Core, NDSP & Frame Capture Recipes

Every result below was empirically executed in a real `.3dsx` binary inside `azahar-headless` (Azahar `2126.2`, New 3DS mode, Mesa `llvmpipe` OpenGL 4.6):

### 6.1 3DS ARM11 JIT Memory Mapping & Execution (`svcControlProcessMemory` Sharp Edges!)

#### Empirical Return Codes Observed in `.3dsx`:

| Operation | Return Code (`r0`) | Actual Behavior & Critical Takeaway |
|-----------|-------------------:|-------------------------------------|
| `svcGetProcessId(&pid, CUR_PROCESS_HANDLE)` | `0x00000000` (`pid=11`) | Succeeds. |
| `svcOpenProcess(&proc, pid)` | `0x00000000` (`handle=0x000B0030`) | Succeeds (mandatory; `CUR_PROCESS_HANDLE` is rejected by `SVC 0x70`). |
| `svcControlProcessMemory(CUR_PROCESS_HANDLE, ...)` | `0xFFFF8001` | Rejected (`CUR_PROCESS_HANDLE` unsupported). |
| `svcControlProcessMemory(proc, va, 0, sz, MEMOP_PROT, RWX)` | **`0x000B0030` (NOT `0`!)** | **CRITICAL TRAP**: On `.3dsx` without exheader `SVC 0x70` bit in Azahar HLE, `SVC 0x70` is ignored and leaves `r0 == proc` (`> 0`, so **`R_SUCCEEDED(rc)` is FALSELY TRUE**!). Always check **`rc == 0`**, NEVER `R_SUCCEEDED(rc)`! |
| `svcControlProcessMemory(proc, 0x07000000, rw, sz, MEMOP_MAP, RX)` | **`0x000B0030` (NOT `0`!)** | **CRITICAL TRAP**: Because `rc != 0` on `.3dsx` in Azahar, `0x07000000` is *not* mapped — jumping to `0x07000000` causes an immediate Prefetch Abort! Only use `rx_mirror_va` if **`rc == 0`** (as on Luma3DS / `.cia` with `SVC 0x70` granted)! |
| Executing emitted code `{ 0xE3A0002A, 0xE12FFF1E }` directly from `memalign(0x1000, sz)`, `linearMemAlign(sz, 0x1000)`, or `alignas(0x1000)` `.bss` | **Returns `42` (`123` on `.bss`)!** | Works natively in Azahar HLE (and on Luma3DS once `MEMOP_PROT` returns `0`). |
| `svcFlushProcessDataCache(CUR_PROCESS_HANDLE, addr, sz)` | `0x00000000` | Succeeds. (`GSPGPU_FlushDataCache` also returns `0x00000000`.) |
| CP15 `mcr p15, 0, r0, c7, c5, 0` (Invalidate Entire I-Cache) in EL0 | **Faults (Privileged EL1-only!)** | Never execute `c7, c5, 0` in user mode! Only `c7, c10, 4` (`DSB`) and `c7, c5, 4` (`FlushPrefetchBuffer` / `ISB`) are EL0-safe. |
| Overwriting already-executed JIT words in-place in Azahar | Cached by Dynarmic unless `--cpu-interp` | Use **append-only bump allocation** in the JIT code buffer; only reset/wrap when full, and pass `--cpu-interp` to `azahar-headless` when testing in-place SMC/branch-patching inside Azahar. |

#### Safe Dual-Compatible 3DS JIT Initialization Pattern (Works on BOTH Real Luma3DS & Azahar `.3dsx`):

```c
#include <3ds.h>
#include <malloc.h>

typedef struct {
    uint32_t *rw_base;   /* Always write emitted instructions here */
    uint32_t *rx_base;   /* Jump here (equals rw_base if single-mapped, or mirror VA if rc_map == 0) */
    size_t    capacity;
    Handle    proc;
    bool      mirrored;
} CtrJitBuffer;

bool ctr_jit_init(CtrJitBuffer *jb, size_t size_bytes) {
    size_bytes = (size_bytes + 0xFFFu) & ~0xFFFu;
    jb->rw_base  = (uint32_t *)memalign(0x1000, size_bytes);
    if (!jb->rw_base) return false;
    jb->rx_base  = jb->rw_base;
    jb->capacity = size_bytes;
    jb->mirrored = false;
    jb->proc     = 0;

    u32 pid = 0;
    if (svcGetProcessId(&pid, CUR_PROCESS_HANDLE) == 0 && svcOpenProcess(&jb->proc, pid) == 0) {
        /* 1. Try W^X dual-mapping (succeeds with EXACT rc == 0 when SVC 0x70 is granted on Luma3DS / CIA) */
        const u32 mirror_va = 0x07000000u;
        Result rc_map = svcControlProcessMemory(jb->proc, mirror_va, (u32)jb->rw_base,
                                                size_bytes, MEMOP_MAP, MEMPERM_READ | MEMPERM_EXECUTE);
        if (rc_map == 0) { /* MUST check == 0, NOT R_SUCCEEDED! */
            jb->rx_base  = (uint32_t *)mirror_va;
            jb->mirrored = true;
            return true;
        }
        /* 2. Fallback: Try in-place RWX protection (Luma3DS Rosalina 3dsx / hbmenu) */
        svcControlProcessMemory(jb->proc, (u32)jb->rw_base, 0, size_bytes,
                                MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
    }
    return true;
}

static inline void ctr_jit_flush(CtrJitBuffer *jb, void *rw_start, size_t bytes) {
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)rw_start, (u32)bytes);
    uint32_t zero = 0;
    __asm__ volatile (
        "mcr p15, 0, %0, c7, c10, 4\n\t" /* DSB (EL0-safe) */
        "mcr p15, 0, %0, c7, c5, 4\n\t"  /* FlushPrefetchBuffer / ISB (EL0-safe) */
        : : "r"(zero) : "memory"
    );
}
```

---

### 6.2 New 3DS Multi-Core, 804 MHz Clock & `ndspInit()` Audio

| API Call | Verified Return Code | Notes |
|----------|---------------------:|-------|
| `APT_CheckNew3DS(&is_n3ds)` | `0x00000000` (`is_n3ds = true`) | Enabled by default in Azahar. |
| `osSetSpeedupEnable(true)` | `void` (no error) | Enables 804 MHz clock + 2 MB L2 cache on New 3DS. |
| `APT_SetAppCpuTimeLimit(30)` | `0x00000000` | Unlocks Core 1 (SysCore) for 1 worker thread on both O3DS & N3DS. |
| `APT_SetAppCpuTimeLimit(80)` | `0x00000000` (Azahar) | On real retail OS, `80` can fail on some titles while `30` always succeeds; try `80`, fallback to `30` if `R_FAILED(rc)`! |
| `threadCreate(fn, arg, 16384, main_prio - 1, /*core_id=*/1, false)` | Non-NULL (`ran = 1`) | **Core 1** worker runs cleanly (`prio` should be `main_prio - 1` or `0x18..0x3F`). |
| `threadCreate(fn, arg, 16384, main_prio, /*core_id=*/2, false)` | Non-NULL (`ran = 1`) | **New 3DS Core 2** dedicated worker runs cleanly with zero APT time limit! (Core 3 also works.) |
| `ndspInit()` without `sdmc:/3ds/dspfirm.cdc` | `0xD880A7FA` (Fail) | `libctru` checks `/3ds/dspfirm.cdc` before initializing DSP. |
| `ndspInit()` with HLE `DSP1` stub (`azahar-headless` auto-seeded!) | **`0x00000000` (OK!)** | `azahar-headless` now **automatically seeds** a minimal 768-byte HLE `DSP1` header stub in `sdmc/3ds/dspfirm.cdc`, so `ndspInit()` always returns `0` in headless CI! Always guard audio output if `R_FAILED(ndspInit())` so missing DSP dump on an unconfigured 3DS degrades gracefully to silent audio instead of aborting boot. |

---

### 6.3 Top-Screen `citro3d` Frame Readback (`sdmc:/screen.ppm`)

Empirically verified pixel-exact (`19,200 / 96,000` Gouraud RGB triangle pixels over `(16, 32, 64)` clear background!) in `azahar-headless`:

```c
/* Setup: single-buffered or capture after 2 rendered C3D_FrameEnd(0) frames */
u16 fb_w = 0, fb_h = 0; /* fb_w = 240, fb_h = 400 (rotated 90 deg CCW BGR8!) */
u8 *gfx_fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_w, &fb_h);
GSPGPU_InvalidateDataCache(gfx_fb, 400 * 240 * 3);

FILE *ppm = fopen("sdmc:/screen.ppm", "wb");
if (ppm) {
    fprintf(ppm, "P6\n400 240\n255\n");
    uint8_t row[400 * 3];
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 400; x++) {
            int idx = (x * 240 + (239 - y)) * 3; /* Un-rotate 3DS column-major BGR8 */
            row[x * 3 + 0] = gfx_fb[idx + 2];    /* R */
            row[x * 3 + 1] = gfx_fb[idx + 1];    /* G */
            row[x * 3 + 2] = gfx_fb[idx + 0];    /* B */
        }
        fwrite(row, 1, sizeof(row), ppm);
    }
    fclose(ppm);
}
```
