# PSX3DS Open-Source Test ROM & BIOS Inventory

All binaries in `tests/roms/` are **100% legal, freely redistributable open-source / homebrew PlayStation 1 test suites and BIOS firmware** (no proprietary Sony `SCPH*.BIN` files are stored or tracked).

## 1. Open-Source PS1 BIOS (`bios/`)

| File | Size | Load Physical Addr | Reset Vector | License | SHA-256 |
|------|-----:|-------------------:|-------------:|---------|---------|
| `bios/openbios.bin` | 524,288 B (512 KB) | `0x1FC00000` | `0xBFC00000` | MIT (`pcsx-redux/nugget`) | `e12d5790e19bc8a8...` |

- **Origin**: Built directly from [`pcsx-redux/nugget/openbios`](https://github.com/pcsx-redux/nugget/tree/main/openbios) (`OPENBIOS_SHELL=shell`) using `mipsel-linux-gnu-gcc` 14.2.0.
- **Capabilities**: Full MIPS R3000A kernel initialization (`A0h`/`B0h`/`C0h` syscall tables, exception handlers, IRQ dispatch, PS-X EXE sideloading, pad/memcard/CD-ROM drivers, TTY console).

## 2. Automated Headless Verification Protocol (PS-X EXE Sideloading & BIOS TTY)

1. **PS-X EXE Binary Format (`0x800`-byte header)**:
   - Offset `0x00..0x07`: ASCII magic `"PS-X EXE"`
   - Offset `0x10` (`u32 LE`): Initial `PC` (`pc0`)
   - Offset `0x14` (`u32 LE`): Initial `$gp` (`gp0`)
   - Offset `0x18` (`u32 LE`): Destination RAM virtual address (`t_addr`, e.g. `0x80010000`)
   - Offset `0x1C` (`u32 LE`): Payload byte size (`t_size`, copied from file offset `0x800`)
   - Offset `0x30` (`u32 LE`), `0x34` (`u32 LE`): Initial `$sp`/`$fp` base + offset (if base != 0, set `$sp = $fp = base + offset`)
2. **Fast Boot + Sideload Hook**:
   - Boot `bios/openbios.bin` at `0xBFC00000` until `PC == 0x80030000` (shell entry after kernel/heap/IRQ table setup completes), then copy the PS-EXE payload to `t_addr & 0x1FFFFF`, flush JIT I-cache, set `PC = pc0`, `$gp = gp0`, `$sp/$fp` (if non-zero), and resume execution.
3. **Capturing Automated Pass/Fail via BIOS TTY Syscalls**:
   - Intercept `PC == 0x000000A0` (`$t1 == 0x3C`, `putchar($a0)`) or `PC == 0x000000B0` (`$t1 == 0x3D`, `putchar($a0)`) (and DUART `0x1F802023` / `0x1F802080` debug write ports) to capture stdout from `psxtest_cpu.exe`, `psxtest_gte.exe`, `psxtest_cpx.exe`, and `JaCzekanski/ps1-tests` without needing OCR!

## 3. PS-X EXE Test & Benchmark Inventory (53 Binaries)

### 3.1 CPU (`R3000A` & `COP0`) Compliance (`tests/roms/cpu/`)

| File | Size | Entry `PC` | Load Addr | Output Mode | Origin | Description | SHA-256 |
|------|-----:|-----------:|----------:|-------------|--------|-------------|---------|
| `cpu/psxtest_cpu.exe` | 1,013,760 B | `0x80010000` | `0x80010000` | BIOS TTY + Screen | AmiDog | Comprehensive R3000A CPU compliance suite (ALU, shifts, branches, delay slots, load delays) | `12486b5f7d52` |
| `cpu/psxtest_cpx.exe` | 34,816 B | `0x80040CE8` | `0x80040808` | BIOS TTY + Screen | AmiDog | COP0 system control registers, exceptions, and coprocessor hazard compliance | `ff75bda24f66` |
| `cpu/cop.exe` | 16,384 B | `0x80011BF8` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | COP0/COP2 exception vectors, BD bit in branch delay slots, unusable coprocessor | `d31d3cec66ab` |
| `cpu/access-time.exe` | 30,720 B | `0x80015230` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | Memory region read/write cycle latency check | `09cd64de3207` |
| `cpu/code-in-io.exe` | 12,288 B | `0x80011684` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | Instruction fetch from Scratchpad / I/O regions | `e53275e60a72` |
| `cpu/io-access-bitwidth.exe` | 30,720 B | `0x80015078` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | 8/16/32-bit MMIO bus width access semantics | `22b2be221fa6` |
| `cpu/CPUADD.exe` | 63,488 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX (Unlicense/MIT) | R3000A signed ADD & overflow trap unit test | `bfbcfb3868de` |
| `cpu/CPUSUB.exe` | 63,488 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A signed SUB & overflow trap unit test | `4836868566e2` |
| `cpu/CPUMULT.exe` | 69,632 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A signed 32x32->64 MULT HI/LO unit test | `136deb3910fd` |
| `cpu/CPUMULTU.exe` | 69,632 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A unsigned 32x32->64 MULTU HI/LO unit test | `8d3b03da6142` |
| `cpu/CPUDIV.exe` | 69,632 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A signed DIV (incl. divide-by-zero & INT_MIN/-1) unit test | `91c2f2bb40c7` |
| `cpu/CPUDIVU.exe` | 69,632 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A unsigned DIVU edge-case unit test | `ee3feaafeb68` |
| `cpu/CPULW.exe` | 79,872 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A 32-bit Load Word (LW) unit test | `a52eed5502a8` |
| `cpu/CPUSW.exe` | 79,872 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A 32-bit Store Word (SW) unit test | `29c3f01484ac` |
| `cpu/CPUSLL.exe` | 88,064 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A Shift Left Logical (SLL) unit test | `aea82c78e8b6` |
| `cpu/CPUSRA.exe` | 88,064 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | R3000A Shift Right Arithmetic (SRA) unit test | `264295611181` |

### 3.2 GTE (`COP2` Geometry Transform Engine) Compliance (`tests/roms/gte/`)

| File | Size | Entry `PC` | Load Addr | Output Mode | Origin | Description | SHA-256 |
|------|-----:|-----------:|----------:|-------------|--------|-------------|---------|
| `gte/psxtest_gte.exe` | 63,488 B | `0x8003F664` | `0x8003F008` | BIOS TTY + Screen | AmiDog | Gold-standard COP2 GTE opcode, MAC/IR saturation, UNR division & FLAG compliance suite | `3efcc3f714d5` |
| `gte/gte-test-all.exe` | 671,744 B | `0x80011898` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | Exhaustive GTE command & FLAG bit-exact verification suite | `479adc925da6` |
| `gte/gte-fuzz.exe` | 32,768 B | `0x800120C4` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | Randomized GTE register/opcode fuzz verification | `19eff4a537a5` |
| `gte/GTEAVSZ.exe` | 59,392 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | GTE AVSZ3 / AVSZ4 Z-averaging OT calculation test | `c9091cd4c143` |
| `gte/GTENCLIP.exe` | 59,392 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | GTE NCLIP 2D winding cross-product test | `16e29780c59b` |

### 3.3 GPU (`GP0`/`GP1` Rasterizer, CLUT, Blending & VRAM) (`tests/roms/gpu/`)

| File | Size | Entry `PC` | Load Addr | Output Mode | Origin | Description | SHA-256 |
|------|-----:|-----------:|----------:|-------------|--------|-------------|---------|
| `gpu/psxtest_gpu.exe` | 333,824 B | `0x80010000` | `0x80010000` | BIOS TTY + Screen | AmiDog | GPU status register, FIFO, and transfer compliance test | `e50ea281bd57` |
| `gpu/triangle.exe` | 12,288 B | `0x80010E94` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | Flat & Gouraud shaded triangle rasterization accuracy | `f035e4893cdb` |
| `gpu/quad.exe` | 12,288 B | `0x80010F54` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | Quad splitting & UV texture coordinate interpolation | `2a5d3bc0cdf8` |
| `gpu/rectangles.exe` | 47,104 B | `0x8001127C` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | GP0 sprite/rectangle primitives & textured sprites | `6eaac3628dee` |
| `gpu/lines.exe` | 14,336 B | `0x80011330` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | Monochrome, Gouraud shaded, and polyline rasterization | `0e68352be925` |
| `gpu/transparency.exe` | 12,288 B | `0x80010E84` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | Semi-transparency ABR modes 0..3 blending verification | `82a71923f5d9` |
| `gpu/mask-bit.exe` | 14,336 B | `0x800112C8` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | GP0(E6h) VRAM bit-15 mask write/preserve behavior | `6617ed6ac43c` |
| `gpu/clut-cache.exe` | 16,384 B | `0x80011D10` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | GPU CLUT cache invalidation and coherency | `528d1b318da2` |
| `gpu/gp0-e1.exe` | 16,384 B | `0x8001166C` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | GP0(E1h) Draw Mode texpage/ABR/dither state bits | `50929eb07049` |
| `gpu/clipping.exe` | 14,336 B | `0x800111A0` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | Drawing area scissor clipping & large vertex clamping | `45817b0abfe7` |
| `gpu/uv-interpolation.exe` | 12,288 B | `0x80010FDC` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | Affine UV stepping precision across spans | `8b93fc4fbc41` |
| `gpu/vram-to-vram-overlap.exe` | 26,624 B | `0x80011694` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | GP0(80h) overlapping VRAM-to-VRAM copy edge cases | `5f3d560f503f` |
| `gpu/HelloWorld16BPP.exe` | 36,864 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | Minimal 16bpp framebuffer bring-up & text smoke test | `1aa4b58348b7` |
| `gpu/RenderPolygonDither16BPP.exe` | 4,096 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | 24-bit to 15-bit hardware 4x4 ordered dither test | `0b95eb252d3b` |
| `gpu/RenderTexturePolygonCLUT4BPP.exe` | 8,192 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | 4bpp CLUT paletted textured polygon rendering | `7e751e37253e` |
| `gpu/RenderTexturePolygonCLUT8BPP.exe` | 12,288 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | 8bpp CLUT paletted textured polygon rendering | `6e8df48c14fc` |
| `gpu/RenderTexturePolygon15BPP.exe` | 16,384 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | 15bpp direct-color textured polygon rendering | `9372ba961807` |
| `gpu/RenderTextureWindowRectangleCLUT4BPP.exe` | 8,192 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | GP0(E2h) Texture Window UV mask/offset repeating tile test | `e7bf5700333f` |

### 3.4 DMA, Root Counters, IRQ, SPU, MDEC & CD-ROM (`tests/roms/dma_timers_io/`)

| File | Size | Entry `PC` | Load Addr | Output Mode | Origin | Description | SHA-256 |
|------|-----:|-----------:|----------:|-------------|--------|-------------|---------|
| `dma_timers_io/otc-test.exe` | 22,528 B | `0x80012D8C` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | DMA6 Ordering Table Clear (reverse linked-list initialization) | `e69038d45056` |
| `dma_timers_io/chain-looping.exe` | 14,336 B | `0x800113E8` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | DMA2 GPU linked-list loop termination & edge cases | `8c4c28ca7372` |
| `dma_timers_io/chopping.exe` | 12,288 B | `0x800117B8` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | DMA chopping / CPU bus sharing window verification | `eb297da6af45` |
| `dma_timers_io/dpcr.exe` | 12,288 B | `0x80011510` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | DPCR DMA channel priority arbitration | `38e1b5d3ef58` |
| `dma_timers_io/timers.exe` | 16,384 B | `0x80011998` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | Root Counters 0/1/2 (dotclock, HBlank, sysclk/8, gate & IRQ) | `21cfe908f61c` |
| `dma_timers_io/VBLANK.exe` | 4,096 B | `0x80010000` | `0x80010000` | VRAM Framebuffer | PeterLemon/PSX | VBlank IRQ (I_STAT/I_MASK bit 0) frame counter demo | `71b62a656c4b` |
| `dma_timers_io/spu-test.exe` | 12,288 B | `0x80010D10` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | SPU register read/write, voice status & SPU IRQ | `6e88b3ab528c` |
| `dma_timers_io/PlayADPCMSample.exe` | 14,336 B | `0x80010000` | `0x80010000` | Audio + VRAM | PeterLemon/PSX | SPU ADPCM upload & voice playback smoke test | `744a6118ade8` |
| `dma_timers_io/mdec-frame-15bit-dma.exe` | 71,680 B | `0x80011570` | `0x80010000` | VRAM Framebuffer | JaCzekanski/ps1-tests (MIT) | MDEC macroblock IDCT + YUV->RGB15 decode via DMA0/DMA1 | `2d7805965781` |
| `dma_timers_io/cdrom-getloc.exe` | 16,384 B | `0x8001236C` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | CD-ROM GetlocL / GetlocP command & response FIFO | `ce1e2904caa1` |

### 3.5 Throughput & Frame-Rate Benchmarks (`tests/roms/benchmarks/`)

| File | Size | Entry `PC` | Load Addr | Output Mode | Origin | Description | SHA-256 |
|------|-----:|-----------:|----------:|-------------|--------|-------------|---------|
| `benchmarks/PSXNICCC.exe` | 708,608 B | `0x80010000` | `0x80010000` | Continuous Render | PeterLemon/PSX | STNICCC 2000 2D polygon stream benchmark (CPU + DMA2 + GP0 throughput) | `3df32ccdd420` |
| `benchmarks/Cube.exe` | 796,672 B | `0x80010000` | `0x80010000` | Continuous Render | PeterLemon/PSX | Real-time 3D GTE RTPT/NCLIP/AVSZ + GPU shaded/textured cube benchmark | `a57d10a88554` |
| `benchmarks/gpu-benchmark.exe` | 75,776 B | `0x800154FC` | `0x80010000` | BIOS TTY + Screen | JaCzekanski/ps1-tests (MIT) | GP0 polygon & fillrate throughput benchmark | `372255c4d617` |
| `benchmarks/animated-triangle.exe` | 16,384 B | `0x80011AC0` | `0x80010000` | Continuous Render | JaCzekanski/ps1-tests (MIT) | Continuous per-frame animated Gouraud triangle benchmark | `9b30c615dfd1` |

