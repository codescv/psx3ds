# PlayStation 1 (PSX) 现代模拟器深度架构分析与工程实现参考手册
**面向 3DS / ARM 资源受限平台的极端性能与高精度模拟架构调研**

*项目：psx3ds*  
*文档路径：`docs/research_modern_psx.md`*  
*状态：正式技术规格与调研报告*  

---

## 1. 模拟器架构对比矩阵与 3DS 移植经验提炼

### 1.1 主流 PlayStation 1 模拟器深度对比矩阵

| 模拟器 / 核心 | 核心作者 / 组织 | 模拟精度 (Accuracy) | CPU 执行后端 (CPU Backends) | GPU 渲染管线 (GPU Pipeline) | SPU / CD-ROM 保真度 | 内存 Footprint | 架构优势 (Pros) | 架构劣势 (Cons) | 对 3DS 移植的可借鉴点 (Takeaways for 3DS) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **DuckStation** | Stenzek | **极高** (98%+) | 解释器、缓存解释器、现代化高效 JIT (x86_64, AArch64, ARMv7, RV64) | 软渲染 (多线程精准光栅化)；硬件渲染 (Vulkan, D3D11/12, OpenGL)，支持 PGXP | 周期级精准 SPU (含完整高斯插值与混响)；精准驱动状态机与子通道读取 | 中等 (~50MB - 120MB) | 极度现代化的 C++20 代码库；下计数器事件调度器 (Downcounter Scheduler)；64KB Fastmem 内存架构；PGXP 几何修正；极其强劲的代码生成优化 | 依赖 64 位虚拟地址空间或大内存映射；重度 C++ 模板与现代库，在旧编译器/旧系统构建较重 | **必须借鉴**：事件驱动周期下计数器调度模型、直接块链接 (Direct Block Linking)、快速寄存器缓存分配算法、无分支 GTE 内联逻辑 |
| **Mednafen / Beetle PSX** | Ryphecha / Libretro | **参考级最高** (99.5%+) | 纯解释器 (Mednafen 官方)；Beetle PSX 移植了实验性 Dynarec | 极致像素级软渲染 (Pixel-accurate Software Rasterizer)；Beetle HW 支持 Vulkan/GL | 黄金标准 (Golden Standard)：总线周期级 SPU、CD-ROM 机械延迟与 FIFO 极度精确 | 较高 (~80MB - 200MB) | 边缘行为与硬件怪异行为 (Hardware Quirks) 的绝对真理来源；无需 BIOS 的开源实现参考 | CPU 吞吐要求极高；无底层 ARM 优化；无法在低主频处理器上全速运行 | **用作真值基准 (Ground Truth)**：验证复杂的 CD-ROM INT1~INT5 状态机转换、GPUSTAT 边沿行为与 SPU 混响系数 |
| **PCSX-ReARMed** | Notaz, Exophase, Ari64 | **良好至高** (~92%-95%) | **Ari64 ARM Dynarec** (纯 ARM32 汇编编写，极致轻量快速)、缓存解释器 | 优化极佳的 ARM 汇编软光栅器 (Unai / GPULIB)；部分硬件分支支持 GLES/citro3d | 中等保真度：P.E.Op.S 衍生 SPU (含高度 ARM 优化的混音器)；异步 CD 读取 | **极低** (~12MB - 28MB) | 专为 ARMv6/ARMv7 (Pandora, RPi, 3DS, Vita) 定制；Ari64 JIT 内存占用与生成代码密度惊人；全汇编核心 | 代码库历史悠久 (C/汇编混杂)；存在较多未清理的历史 Hack；GTE 精度与边缘指令有偏差 | **核心基础参考**：Ari64 Dynarec 的寄存器映射策略、Unai GPU 的 16-bit 定点跨距渲染、3DS `citro3d` 历史适配教训 |
| **PCSX-Redux** | GrumpyCoders 等 | **高** (~95%) | 现代化 C++ 解释器、重构版 JIT 试验分支 | 现代 OpenGL / Vulkan，内建反向工程与调试监视器 | 高保真度；完整实现 OpenBIOS 原生替换 | 中等 (~40MB - 80MB) | 配备世界一流的逆向工程与调试工具集；原生携带功能极其完善的开源 OpenBIOS (C/MIPS 源码) | 目标是现代桌面开发与自制软件调试，非嵌入式优化 | **吸纳 OpenBIOS**：直接复用其 OpenBIOS 源码或二进制，规避索尼专有 SCPH1001 依赖与法律风险 |
| **SwanStation** | libretro (fork from DuckStation) | **极高** (98%+) | 继承自 DuckStation 的 JIT 与解释器架构 | 软件与硬件 (OpenGL/Vulkan) 管线，深度整合 RetroArch API | 继承自 DuckStation | 中等 (~50MB - 100MB) | 将 DuckStation 核心无缝解耦，便于嵌入各类跨平台前端 | 同 DuckStation，对极度受限系统缺少针对性裁剪 | 学习其跨平台接口剥离与轻量化包装方案 |
| **Avocado / Rustation** | JaCzekanski / simias | **中至高** (~90%) | Avocado (现代 C++20 解释器)；Rustation (Rust 编写) | 现代软渲染与着色器架构，Rustation 首创 GPU 原始坐标缓存/抗抖动 | 标准保真度 | 较低至中等 | 架构设计极其清晰，模块化程度高；Avocado 附带极度清晰的自动化单测用例 | 缺少针对弱 CPU 的 Dynarec 支持，性能无法满足嵌入式设备 | **单测用例复用**：提取其 CPU、GTE、DMA 回归测试用例，集成至 3DS 验证管线 |
| **ePSXe / FPse** | ePSXe Team / FPse Team | **实用级** (~85%-90%) | 早期的 MIPS->x86 / MIPS->ARM32 JIT，大量硬编码游戏补丁 | 经典插件架构 (Pete's GPU, Peops SoftGPU) | 实用级 SPU 插件 | 极小 (~8MB - 20MB) | 在古老硬件 (如 200MHz ARM9/ARM11) 上能以牺牲精度换取可用帧率 | 闭源；充斥大量 Game-specific Hacks；破坏自制软件兼容性，行为不可预测 | **警惕负面教材**：绝不硬编码游戏 ID 补丁，走干净的微架构事件调度与通用 JIT 优化路径 |
| **no$psx / psx-spx** | Martin Korth (nocash) | **极高 (硬件规范参考)** | x86 汇编级高精准解释器与调试器 | 独创无插件全规格自研光栅器 | 周期级硬件行为匹配 | 极小 (<10MB) | **psx-spx 是业界权威硬件圣经**；详尽记录所有硬件未公开 Bug、UNR 表、延迟槽特例 | 闭源，仅支持 x86 Windows 平台 | **技术依据**：文档中所有底层寄存器行为、时序边界均以 psx-spx 为最终物理基准 |

---

### 1.2 针对 Nintendo 3DS 硬件约束的核心提炼与借鉴原则

Nintendo 3DS 的硬件配置极其严苛：
- **CPU**：Old 3DS 为双核 ARM11 MPCore @ 268 MHz (主核运行 App，副核系统占用大部分，用户仅能使用少量周期)；New 3DS 为四核 ARM11 MPCore @ 804 MHz。
- **架构**：ARMv6K 指令集，配备 VFPv2 (仅单精度/双精度基本浮点，**无 NEON 向量指令集**)。具备基本 ARMv6 SIMD 指令 (如 `UQADD8`, `UQSUB8`, `SMLAD`, `PKHBT`)。
- **内存**：Old 3DS 仅 128MB FCRAM (用户可用约 64MB~80MB)；New 3DS 为 256MB (用户可用约 178MB)。
- **GPU**：DMP PICA200 @ 134 MHz，采用定制顶点/几何着色器 + 固定纹理环境 (TEV) 阶段，**不支持任意片段着色器 (Fragment Shader)**，无法原生执行 PS1 特殊半透明 ABR 0~3 逻辑。

#### 核心借鉴法则：
1. **彻底摒弃单指令外设轮询，全面采用 DuckStation 风格的“事件驱动周期递减调度器 (Downcounter Scheduler)”**：
   在 268MHz 的 CPU 上，任何每条指令检查一次中断/定时器的开销都会直接吃掉 30% 以上的性能。必须采用以 CPU 周期为基准的下计数器，直接跳到下一个最近硬件事件 (GPU HBlank/VBlank、DMA 结束、CD-ROM 中断、Timer 匹配)。
2. **构建紧凑的 64KB 软件快速内存映射 (Software Fastmem LUT)**：
   3DS OS 无法支持完整的 4GB 虚拟内存 mmap 保护页陷阱 (Trap)。但可以采用 64KB 页大小的查找表：$2^{32} / 2^{16} = 65536$ 项。每项保存一个经过偏移计算的主机指针或 NULL/特殊标记。在 JIT 生成代码中仅需 3 条 ARM 指令即可完成 RAM 读写。
3. **GTE 高频指令全内联 (Inline JIT Emission)**：
   3D 游戏中 GTE 占用 40%~60% 的计算量。若将 GTE 指令作为 C 函数调用，函数调用上下文切换 (保存恢复 r0-r3, lr) 将造成巨大流水线气泡。必须将高频前 5 大指令 (`RTPS`, `RTPT`, `NCLIP`, `MVMVA`, `DPCS`) 直接用 ARMv6 汇编内联生成。
4. **利用 ARMv6 SIMD 加速软件光栅化与色彩混合**：
   利用 `UQADD8` (无符号饱和加法)、`UHADD8` (无符号减半加法，天然对应 50% 混合)、`PKHBT`/`PKHTB` (半字打包) 替代耗时的逐像素通道分离计算。
5. **双核/多线程分工 (Thread Architecture)**：
   Core 0 全力负责 MIPS CPU Dynarec + GTE + GPU 命令打包；Core 1 承载 SPU 24 通道 ADPCM 混音、高斯插值与 CD-ROM 扇区异步流式预读。

---

## 2. CPU (MIPS R3000A @ 33.8688 MHz) 与 内存体系架构

PlayStation 1 的中央处理器采用 LSI Logic 生产的 **LSI CoreWare CW33300** 宏单元，基于 MIPS I 指令集架构，集成系统控制协处理器 (COP0)、几何变换引擎 (COP2 / GTE)、指令缓存 (I-Cache 4KB) 及高速暂存器 (Scratchpad 1KB Data Cache)，运行于 **33.8688 MHz** (53.693175 MHz NTSC 主振荡器 $\times \frac{11}{7}$ 或 67.7376 MHz 主频 2 分频)。

```
       +-------------------------------------------------------------+
       |               MIPS R3000A CPU Core (33.8688 MHz)            |
       |  32 x 32-bit GPRs ($0..$31), PC, HI, LO, Shift/ALU/MUL/DIV |
       +------------------------------+------------------------------+
                                      |
       +------------------------------+------------------------------+
       | COP0 (System Control)        | COP2 (GTE - Geometry Engine) |
       | SR, Cause, EPC, PRId, BadA   | 32 Data Regs, 32 Control Regs|
       +------------------------------+------------------------------+
                                      |
       +------------------------------+------------------------------+
       | 4KB Direct-Mapped I-Cache    | 1KB Scratchpad D-RAM (D-Cache)|
       +------------------------------+------------------------------+
                                      |
                         32-bit Internal System Bus
                                      |
   +----------------+-----------------+-----------------+----------------+
   |  2MB Main RAM  | 512KB BIOS ROM  |  I/O Registers  | DMA Controller |
   | (Mirrored x4)  | (0x1FC00000)    | (0x1F801000...) |  (7 Channels)  |
   +----------------+-----------------+-----------------+----------------+
```

---

### 2.1 物理与虚拟内存映射表

MIPS R3000A 采用 32 位虚拟寻址空间，划分为 4 个主要内存段：
- **`KUSEG` (`0x00000000` - `0x7FFFFFFF`, 2GB)**：用户段，经缓存 (Cached)。在 PS1 上无 MMU TLB 映射，直接对应物理内存。
- **`KSEG0` (`0x80000000` - `0x9FFFFFFF`, 512MB)**：内核段 0，经缓存 (Cached)。通过将最高 3 位清零 (`addr & 0x1FFFFFFF`) 镜像至物理内存 `0x00000000` - `0x1FFFFFFF`。PS-EXE 游戏主程序与主内存堆栈几乎完全运行在此段。
- **`KSEG1` (`0xA0000000` - `0xBFFFFFFF`, 512MB)**：内核段 1，**未经缓存 (Uncached)**。同样通过 `addr & 0x1FFFFFFF` 映射至物理地址。所有硬件 MMIO 寄存器与 BIOS 引导代码均在此段访问，防止缓存造成读写陈旧。
- **`KSEG2` (`0xC0000000` - `0xFFFFFFFF`, 1GB)**：内核段 2，通常用于底层芯片特殊控制寄存器 (如 `0xFFFE0130` Cache Control)。

#### 精确物理内存地址映射空间 (Physical Address Space `0x1FFFFFFF`)

| 物理地址范围 (Physical) | KSEG0 镜像 (Cached) | KSEG1 镜像 (Uncached) | 大小 (Size) | 描述与硬件设备 | 模拟器处理与行为策略 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0x00000000 - 0x001FFFFF` | `0x80000000 - 0x801FFFFF` | `0xA0000000 - 0xA01FFFFF` | 2048 KB | **主内存 (Main RAM)** | 核心内存。JIT 与 Fastmem 直连区域 |
| `0x00200000 - 0x007FFFFF` | `0x80200000 - 0x807FFFFF` | `0xA0200000 - 0xA07FFFFF` | 6144 KB | **主内存镜像 (RAM Mirrors)** | 物理 2MB RAM 重复镜像 3 次至 8MB 边界。读取/写入均映射至 `addr & 0x1FFFFF` |
| `0x1F000000 - 0x1F07FFFF` | `0x9F000000 - 0x9F07FFFF` | `0xBF000000 - 0xBF07FFFF` | 512 KB | **扩展区 1 (Expansion Region 1)** | 外部扩展 ROM / 调试卡 (Action Replay / GameShark)。未接设备时读返回 `0xFF` |
| `0x1F800000 - 0x1F8003FF` | `0x9F800000 - 0x9F8003FF` | `0xBF800000 - 0xBF8003FF` | 1024 字节 | **高速暂存器 (Scratchpad D-Cache Data RAM)** | 快速本地 SRAM。游戏常用于放置快速临时几何缓冲区与快速栈 |
| `0x1F801000 - 0x1F801023` | - | `0xBF801000 - 0xBF801023` | 36 字节 | **内存控制寄存器 (Memory Control)** | 包含 `RAM_SIZE` (`0x1F801060`)、总线等待周期控制等 |
| `0x1F801040 - 0x1F80104F` | - | `0xBF801040 - 0xBF80104F` | 16 字节 | **SIO0 (手柄 Joypad / 存储卡 Memory Card)** | 串行外设总线接口 |
| `0x1F801070 - 0x1F801077` | - | `0xBF801070 - 0xBF801077` | 8 字节 | **中断控制器 (`I_STAT`, `I_MASK`)** | 核心 IRQ 锁存与屏蔽寄存器 |
| `0x1F801080 - 0x1F8010FF` | - | `0xBF801080 - 0xBF8010FF` | 128 字节 | **DMA 控制器 (DMA Channels 0..6, DPCR, DICR)** | 7 通道直接内存访问引擎 |
| `0x1F801100 - 0x1F801130` | - | `0xBF801100 - 0xBF801130` | 48 字节 | **根计数器 / 定时器 (Timers 0, 1, 2)** | 像素时钟、HBlank、系统时钟计时器 |
| `0x1F801800 - 0x1F801803` | - | `0xBF801800 - 0xBF801803` | 4 字节 | **CD-ROM 驱动控制器** | Bank 切换、命令/参数/结果 FIFO 与中断处理 |
| `0x1F801810 - 0x1F801817` | - | `0xBF801810 - 0xBF801817` | 8 字节 | **GPU 接口端口 (`GP0`, `GP1` / `GPUREAD`, `GPUSTAT`)** | 图形渲染与状态控制 |
| `0x1F801820 - 0x1F801827` | - | `0xBF801820 - 0xBF801827` | 8 字节 | **MDEC 宏块解码器 (MDEC Data / Status)** | 视频帧硬件解压 |
| `0x1F801C00 - 0x1F801E7F` | - | `0xBF801C00 - 0xBF801E7F` | 640 字节 | **SPU 声音处理单元寄存器** | 24 声道配置、音量、ADSR、混响 |
| `0x1FC00000 - 0x1FC7FFFF` | `0x9FC00000 - 0x9FC7FFFF` | `0xBFC00000 - 0xBFC7FFFF` | 512 KB | **BIOS ROM 空间** | 启动 ROM，镜像至 `0x1FDFFFFF` (2MB 范围) |
| `0xFFFE0130` | - | - | 4 字节 | **缓存控制寄存器 (Cache Control Register)** | 位于 KSEG2。控制缓存重填、标签锁与隔离模式 |

---

### 2.2 缓存控制端口 `0xFFFE0130` 与 COP0 SR `IsC` 隔离位

PS1 的 CPU 内建 4KB 指令缓存 (I-Cache)。没有数据缓存 (D-Cache)，原本用作 D-Cache 的 SRAM 硬件被直接重定向作为 1KB Scratchpad。

#### 核心机制：I-Cache 刷新与清空
在真实硬件中，若 CPU 执行了动态加载代码或自修改代码 (SMC)，BIOS 和内核必须刷新指令缓存。MIPS-I 缺乏专用的 `cache` 指令，Sony 硬件设计采用以下特定序列清空缓存：
1. **COP0 Status Register (SR, `r12`) Bit 16 (`IsC` - Isolate Cache)**：
   - 当 `IsC = 0` 时，主内存数据读写正常发往外部总线。
   - 当 `IsC = 1` 时，**外部数据总线被硬件切断**！所有 CPU 的内存写入 (`SW`, `SH`, `SB`) 和读取 (`LW`, `LH`, `LB`) 均**不会到达物理 RAM**，而是被强制导向内部指令缓存的 Tag/Data 阵列。
2. **端口 `0xFFFE0130` (Cache Control)**：
   - 寄存器位配置如下：
     - Bit 9: 启用内存抓取控制
     - Bit 11: 锁定 I-Cache 状态
     - 正常运行值：`0x0001E988`
     - 刷新操作值：`0x00000804` 或 `0x00000800`

```c
// [关键实现范例] 内存写入时的 IsC 状态拦截
void Write32(uint32_t vaddr, uint32_t data) {
    if (__builtin_expect((cop0_regs[COP0_SR] & SR_ISC) != 0, 0)) {
        // 当 IsC=1 时，写操作被缓存隔离拦截，绝不可写入主内存 RAM！
        // 真实硬件中，该操作用于将 I-Cache 对应的 Tag 清零或标记无效。
        // 若模拟器忽略 IsC 直接写入 RAM，BIOS 开机例程会直接覆盖破坏 0xA0000000 处的关键变量！
        InvalidateICacheLine(vaddr);
        return;
    }
    // 正常内存路由...
    FastmemWrite32(vaddr, data);
}
```

---

### 2.3 MIPS-I 硬件流水线与延迟槽语义

MIPS R3000A 采用经典的 5 级标量流水线：**IF (取指) -> ID (译码) -> EX (执行) -> MEM (访存) -> WB (写回)**。由于没有硬件互锁旁路 (Interlocking Hardware)，软件编译器必须感知硬件延迟。

#### 1. 负载延迟槽 (Load Delay Slot)
任何内存加载指令 (`LW`, `LH`, `LHU`, `LB`, `LBU`, `LWL`, `LWR`) 从数据总线取回数据必须经过 MEM 阶段。因此，**紧随加载指令之后的一条指令，无法立即读取目标寄存器的最新值**，读取到的依然是旧值。

```text
  指令 N   :  LW   $v0, 0($a0)     ; $v0 开始从内存加载
  指令 N+1 :  ADD  $v1, $v0, $zero ; [延迟槽] 此时读出的 $v0 是加载前的旧值！
  指令 N+2 :  ADD  $a1, $v0, $zero ; 此时读出的 $v0 才是从内存读出的新值！
```

*模拟器实现法则*：
在解释器或 JIT 中，采用两级加载延迟寄存器管线：
```c
struct CpuState {
    uint32_t regs[32];
    uint32_t load_reg_target; // 目标寄存器索引 (0..31)
    uint32_t load_reg_value;  // 待写回的新值
};

// 伪代码执行逻辑：
void ExecuteStep() {
    uint32_t pending_reg = state.load_reg_target;
    uint32_t pending_val = state.load_reg_value;
    state.load_reg_target = 0; // 清空当前步加载槽

    ExecuteInstruction(); // 当前指令执行，读取的是 state.regs[] (旧值)

    if (pending_reg != 0) {
        state.regs[pending_reg] = pending_val; // 上一条 LW 的延迟写入在此生效
    }
}
```

#### 2. 分支延迟槽 (Branch Delay Slot) 与边缘异常分支
所有跳转与分支指令 (`J`, `JAL`, `JR`, `JALR`, `BEQ`, `BNE`, `BLEZ`, `BGTZ`, `BLTZ`, `BGEZ`) 后的**紧邻后一条指令必定会被无条件执行**。

*致命特例与硬件未定义行为*：
1. **延迟槽中的分支指令 (Branch in Delay Slot)**：
   如果在分支延迟槽内又放置了一条跳转/分支指令 (例如 `BEQ` 的延迟槽是 `J`)，硬件行为是不可预测的。部分硬件会丢失第二个分支的跳转地址，产生随机 PC 漂移。通常禁止软件这样编写。
2. **分支延迟槽中的异常 (Exception in Delay Slot)**：
   若延迟槽指令触发了异常 (如非法内存访问、溢出、系统调用 `SYSCALL`、未定义操作码)：
   - **COP0 `Cause` 寄存器 的 Bit 31 (`BD` - Branch Delay) 必须被硬件置 1**！
   - **COP0 `EPC` (Exception Program Counter) 必须存储该分支指令的地址 (PC - 4)，而不是发生异常的延迟槽指令地址 (PC)**！
   - 原因：当异常处理程序返回执行 `RFE` 时，CPU 必须重新执行原始分支指令，以恢复分支跳转逻辑。

#### 3. GTE 硬件冒险 (GTE Hazards)
向 GTE 寄存器写入数据 (`CTC2`, `MTC2`) 或发起 GTE 矢量运算指令后，结果寄存器更新需要固定周期数。例如 `RTPS` 需要 15 周期，`MVMVA` 需要 8 周期。在延迟未耗尽前直接用 `MFC2` 读取，可能读出未完成计算的部分累加值。现代模拟器通常将 GTE 运算视为即时原子完成，但在高精度模拟时需模拟等待停顿。

---

### 2.4 COP0 异常处理与中断控制器 (`I_STAT` / `I_MASK`)

#### COP0 核心控制寄存器
- **`COP0 Register 12 (SR)` - 状态寄存器 (Status Register)**：
  - Bit 0: `IEc` (Interrupt Enable Current) - 当前中断使能 (0=禁用, 1=使能)
  - Bit 1: `KUc` (Kernel/User Current) - 当前特权模式 (0=内核态, 1=用户态)
  - Bit 2: `IEp` (Interrupt Enable Previous) - 上一次中断使能标志
  - Bit 3: `KUp` (Kernel/User Previous) - 上一次特权模式
  - Bit 4: `IEo` (Interrupt Enable Old) - 旧中断使能
  - Bit 5: `KUo` (Kernel/User Old) - 旧特权模式
  - Bits 8..15: `IM` (Interrupt Mask) - 8 位外部/软件中断屏蔽位 (Bit 10 为外部硬件 IRQ 线路)
  - Bit 16: `IsC` (Isolate Cache) - 缓存隔离使能
  - Bit 22: `BEV` - 引导异常向量位置 (0=异常基址 `0x80000080`, 1=ROM 基址 `0xBFC00180`)
- **`COP0 Register 13 (Cause)` - 异常原因寄存器**：
  - Bits 2..6: `ExcCode` (异常代码：0=INT外部中断, 4=ADDRL加载地址对齐错误, 5=ADDRS存储地址错误, 8=Syscall, 9=Breakpoint, 10=RI保留指令/非法指令, 12=Overflow整数溢出)
  - Bits 8..15: `IP` (Interrupt Pending - 挂起的中断线路，Bit 10 对应外部硬件中断)
  - Bit 31: `BD` (Branch Delay - 异常是否发生在分支延迟槽中)

#### 外部中断控制器 (`0x1F801070` / `0x1F801074`)
PlayStation 1 主板集成了专用的中断控制器，统一收集板载所有外设的边沿触发信号，汇聚成一条中断信号送入 CPU COP0 的硬件中断线 (Cause 寄存器 Bit 10，对应 SR 的 IM2)。

- **`I_STAT` (`0x1F801070`)**：中断状态寄存器 (Interrupt Status Register, Read/Write)
- **`I_MASK` (`0x1F801074`)**：中断屏蔽寄存器 (Interrupt Mask Register, Read/Write)

| IRQ 位 (Bit) | 标识符 (Symbol) | 触发源外设 (Interrupt Source) | 产生条件与时序 |
| :---: | :---: | :--- | :--- |
| **0** | `IRQ0` | **VBLANK** | 垂直消隐期间，GPU 进入 VBlank 时由扫描时序触发 (每场 50Hz/60Hz 触发一次) |
| **1** | `IRQ1` | **GPU** | GPU 内部中断，由 GP0 指令 `0x1F` 请求，或绘制区域限制/线段中断触发 |
| **2** | `IRQ2` | **CD-ROM** | CD-ROM 微控制器发出的 INT1..INT5 中断请求 |
| **3** | `IRQ3` | **DMA** | 任一启用了中断的 DMA 通道传输完成 (`DICR` 触发) |
| **4** | `IRQ4` | **Timer 0 (TMR0)** | 根计数器 0 (Dot Clock 像素时钟 / 系统时钟) 达到 Target 目标值或溢出 |
| **5** | `IRQ5` | **Timer 1 (TMR1)** | 根计数器 1 (HBlank 水平时钟 / 系统时钟) 达到 Target 目标值或溢出 |
| **6** | `IRQ6` | **Timer 2 (TMR2)** | 根计数器 2 (系统时钟 / SysClk除以8) 达到 Target 目标值或溢出 |
| **7** | `IRQ7` | **Controller / MemCard** | SIO0 串行接收到字节，或手柄/记忆卡拉低 `/ACK` 确认信号 |
| **8** | `IRQ8` | **SIO (Serial Port)** | 串口 SIO 通信中断 |
| **9** | `IRQ9` | **SPU** | SPU 硬件混音到达特定 IRQ 地址，或 SPU DMA 中断 |
| **10** | `IRQ10` | **Lightgun / PIO** | 光枪外设引脚或扩展口中断 |

#### 极其关键的中断确认语义 (Acknowledge Semantics)
`I_STAT` 是一个**写 0 清零 (Clear on Write 0)** 寄存器：
- **读取 `I_STAT`**：返回当前挂起中断标志 (`1` 表示有中断产生，`0` 表示无)。
- **写入 `I_STAT`**：**写入 `0` 会清除对应位，写入 `1` 保持不变**！
  - 核心逻辑等价于：`I_STAT = I_STAT & write_value;`
  - *致命陷阱*：绝对不能实现为写 1 清零 (Write 1 to clear) 或直接覆盖赋值 (`I_STAT = write_value`)，否则将导致中断漏发或立即误杀其他未处理的 IRQ！

#### 中断向 CPU 触发判定条件
```c
bool IsCpuInterruptPending() {
    // 1. 判断外设中断是否被 I_MASK 放行
    bool irq_active = (I_STAT & I_MASK) != 0;
    
    // 2. 将状态反映到 COP0 Cause 寄存器 Bit 10 (IP2)
    if (irq_active) {
        cop0_regs[COP0_CAUSE] |= (1 << 10);
    } else {
        cop0_regs[COP0_CAUSE] &= ~(1 << 10);
    }
    
    // 3. 判定 CPU 是否允许接收该中断 (全局使能 IEc=1，且屏蔽位 IM2 放行)
    uint32_t sr = cop0_regs[COP0_SR];
    return (sr & 0x01) && (sr & (1 << 10)) && irq_active;
}
```

---

### 2.5 现代 JIT 与 解释器的高性能极速调度架构

#### 1. 事件驱动周期递减调度器 (Event-Driven Cycle Downcounter Scheduler)
传统初级模拟器在每执行一条 MIPS 指令后，都会循环调用外设更新函数：
`StepCPU(1); StepTimers(1); StepDMA(); StepGPU();`  
这种模式在 3DS 的 268MHz ARM11 上会引发灾难性的函数调用开销与缓存抖动。

DuckStation 与顶级 JIT 采用全局**周期事件下计数器 (Downcounter)**：
- 维护一个调度事件优先队列，按触发的全局目标周期绝对值排序。
- 计算出当前最近事件距离当前的剩余周期 `slice_cycles = next_event_target - current_cycles`。
- 将 `downcounter` 寄存器加载为 `slice_cycles`。
- JIT 编译代码在每个基本块 (Basic Block) 头部仅执行一次递减：
  `downcounter -= block_cycle_count;`
  `bgt continue_block;` (若 `downcounter > 0`，全速跳转执行下一块，**没有任何外设调用！**)
- 只有当 `downcounter <= 0` 时，才退出 JIT 代码块，执行到期事件的回调处理，并重新装填下一个时间片。

```
[Event Queue]
  Event A: GPU Scanline End (at Cycle 1200)
  Event B: Timer 0 Target Match (at Cycle 1850)
  Event C: CD-ROM Sector Ready (at Cycle 4500)
      |
      v
  Next Target = 1200. Current Cycle = 1000.
  Slice = 200 cycles -> Load into Downcounter Register (r10 in ARM).
      |
      v
  [Native ARM JIT Execution Loop - Fast Path: 0 Peripheral Checks!]
  Block 1: -12 cycles
  Block 2: -28 cycles
  Block 3: -80 cycles
  ...
  Downcounter <= 0 -> Exit JIT -> Fire Event A (GPU Scanline) -> Reschedule!
```

#### 2. 软件快速内存映射 (Software Fastmem 64KB Page Table LUT)
3DS 的 32 位物理寻址限制，使我们无法像 x86_64 那样使用虚拟内存异常捕获 (SIGSEGV Guard Page)。最优方案是建立一个 64KB 粒度的页表指针查找表 (Page Table LUT)：
- 32 位地址空间划分为 $2^{32} / 2^{16} = 65536$ 个页。
- 建立两个指针数组：`void* read_lut[65536]` 与 `void* write_lut[65536]`。
- 对于 2MB 主内存 (及其 8MB 范围内的所有镜像) 和 1KB Scratchpad，对应页的指针初始化为：
  `lut[page] = (uint8_t*)host_ram_base - (guest_page_base);`
- 对于 MMIO 区域、ROM 写保护区域或无效区域，指针填充为 `NULL` 或专用 Trap 哨兵地址。

*在 ARM32 JIT 中生成的极速读写代码 (仅需 3 条指令)*：
```assembly
; 输入: r0 = guest_vaddr (MIPS 访存地址)
; 输出: r1 = 读取到的 32-bit 数据
LSR   r2, r0, #16              ; r2 = page index (vaddr >> 16)
LDR   r3, [r9, r2, LSL #2]     ; r9 为固定的 LUT 基地址寄存器，加载 host_offset 指针
CMP   r3, #0                   ; 检查是否为 NULL / MMIO
BEQ   slow_path_read_fallback  ; 若为 NULL，跳转慢速分支处理 MMIO / 外设读写
LDR   r1, [r3, r0]             ; [极速直接寻址] r1 = *(uint32_t*)(host_offset + guest_vaddr)
```
此结构使得 98% 以上的物理 RAM 读写直接被硬件指针解引用吞吐，完全绕过函数调用与边界判断。

#### 3. 直接块链接 (Direct Block Linking)
在 JIT 编译器中，每个基本块在编译结束时，如果已知其后继分支目标已完成编译，JIT **直接修改前驱块末尾的跳转机器码，将其直接指向目标基本块的主机代码入口**。
- 效果：执行流在连续编译的机器码之间以原生分支跳转高速穿梭，完全消除返回 JIT 调度分发器 (Dispatcher Loop) 的调度损耗。
- 失效处理：当某个块由于自修改代码 (SMC) 被清除时，反向解绑 (Unlink) 所有指向它的直接跳转分支，恢复为调用 JIT Dispatcher。

#### 4. 返回/间接跳转哈希缓存 (Return/Jump Hash Cache for `JR $ra`)
函数返回指令 `JR $ra` 或间接跳转 `JR $t0` 的目标地址在编译期未知，无法进行直接块链接。
- 设计一个极小容量、高命中率的直接映射哈希表 (例如 1024 项)：
  `struct JumpEntry { uint32_t guest_pc; const void* host_code_ptr; };`
  `index = (guest_pc >> 2) & 0x3FF;`
- JIT 生成的内联跳转逻辑：
  1. 计算 `index`；
  2. 比较 `cache[index].guest_pc == target_mips_pc`；
  3. 若匹配，直接 `BX cache[index].host_code_ptr` (命中率通常高达 90%~95%)；
  4. 若不匹配，回退至全局散列表查找并更新缓存。

#### 5. 延迟槽重排技术 (Delay-Slot Reordering)
MIPS-I 的分支指令后带延迟槽。对于条件分支指令：
```text
  BNE   $s0, $zero, target
  ADD   $v0, $v1, $a0        ; 延迟槽
```
如果延迟槽指令不影响分支判断寄存器 (`$s0`)，JIT 可以在发射机器码时将延迟槽指令**提前发射到分支指令之前**：
```assembly
  ADD   v0, v1, a0          ; 提前执行延迟槽
  CMP   s0, #0              ; 执行分支判断
  BNE   host_target
```
该技术完全消除了分支进入目标块后需要单独处理延迟槽状态的复杂度，且完美利用了 ARM 的条件执行流水线。

#### 6. 空转循环跳过 (Idle-Loop Skipping)
在许多 PS1 游戏中，主线程等待垂直消隐 (VBlank)、GPU 渲染完成或 CD-ROM 读取时，会执行紧凑的自旋循环：
```assembly
loop:
  LW    $v0, 0x1F801814     ; 读取 GPUSTAT
  ANDI  $v0, $v0, 0x0400    ; 检查某一位状态
  BEQ   $v0, $zero, loop    ; 自旋等待
```
JIT 或解释器通过静态模式匹配识别这种空转循环，一旦侦测到该循环在单步内未引起任何外部状态改变，直接将 CPU 的 `downcounter` 扣减至 0，**直接跳跃推进时间到下一个即将触发的外设硬件事件**，消除数以百万计的无意义自旋指令。

#### 7. 自修改代码 (SMC) 保护与位图失效
PS1 游戏经常通过覆盖内存动态生成代码 (如覆盖覆盖物、动态解压、重定位)：
- 将 2MB RAM 划分为 32 字节或 128 字节的小块，维护一个紧凑的代码脏位图 (Dirty Bitmap)。
- 当 Fastmem 写操作落入已被编译为 JIT 代码块的内存页时，标记对应页面脏。
- 采用世代计数器 (Generation Counter) 机制，仅在下次取指命中该块时触发 JIT 块失效与重编译，避免每次内存写入都进行高开销的 JIT 块哈希表清理。

---

## 3. GTE (COP2 几何变换引擎) 深度解析

几何变换引擎 (GTE) 是集成在 CPU 芯片内部的协处理器 2 (COP2)，具备专用定点数学 ALU，专门用于极速完成 3D 坐标空间变换、透视投影、光照着色与深度排序。PS1 惊艳的 3D 多边形吞吐全仰赖于此。

---

### 3.1 完整数据寄存器 (0..31) 与 控制寄存器 (32..63) 映射

#### GTE 数据寄存器 (Data Registers: COP2 `r0` .. `r31`)

| 寄存器号 | 符号 (Symbol) | 位宽 (Bits) | 格式 (Format) | 功能与用途描述 |
| :---: | :---: | :---: | :---: | :--- |
| **0** | `VXY0` | 32 (16+16) | 2x S16 | 顶点 0 的 X, Y 分量 (X: 低 16 位, Y: 高 16 位) |
| **1** | `VZ0` | 32 (16+16) | S16 + Pad | 顶点 0 的 Z 分量 (低 16 位为有效 Z) |
| **2** | `VXY1` | 32 (16+16) | 2x S16 | 顶点 1 的 X, Y 分量 |
| **3** | `VZ1` | 32 (16+16) | S16 + Pad | 顶点 1 的 Z 分量 |
| **4** | `VXY2` | 32 (16+16) | 2x S16 | 顶点 2 的 X, Y 分量 |
| **5** | `VZ2` | 32 (16+16) | S16 + Pad | 顶点 2 的 Z 分量 |
| **6** | `RGBC` | 32 (8x4) | 4x U8 | 颜色与代码 (R, G, B 各 8 位，高 8 位为 GPU 渲染指令 CODE) |
| **7** | `OTZ` | 16 | U16 | 平均屏幕 Z 值 (用于构建 Ordering Table 排序表索引) |
| **8** | `IR0` | 16 | S16 (1.3.12) | 中间累加器与临时矢量分量 0 |
| **9** | `IR1` | 16 | S16 (1.3.12) | 中间矢量分量 1 (常用作光照、临时 X 分量) |
| **10** | `IR2` | 16 | S16 (1.3.12) | 中间矢量分量 2 (常用作临时 Y 分量) |
| **11** | `IR3` | 16 | S16 (1.3.12) | 中间矢量分量 3 (常用作临时 Z 分量) |
| **12** | `SXY0` | 32 (16+16) | 2x S16 | 变换后的屏幕坐标 0 (SX0: 低 16 位, SY0: 高 16 位) |
| **13** | `SXY1` | 32 (16+16) | 2x S16 | 变换后的屏幕坐标 1 (SX1: 低 16 位, SY1: 高 16 位) |
| **14** | `SXY2` | 32 (16+16) | 2x S16 | 变换后的屏幕坐标 2 (SX2: 低 16 位, SY2: 高 16 位) |
| **15** | `SXYP` | 32 (16+16) | 2x S16 | 屏幕坐标先入先出推入缓冲 (向 SXY0 推送并移位) |
| **16** | `SZ0` | 16 | U16 | 屏幕 Z 坐标 0 (先入先出队列首项) |
| **17** | `SZ1` | 16 | U16 | 屏幕 Z 坐标 1 |
| **18** | `SZ2` | 16 | U16 | 屏幕 Z 坐标 2 |
| **19** | `SZ3` | 16 | U16 | 屏幕 Z 坐标 3 (当前透视投影计算产生的最新 Z 深度) |
| **20** | `RGB0` | 32 (8x4) | 4x U8 | 顶点 0 着色计算输出颜色 (R0, G0, B0, CODE) |
| **21** | `RGB1` | 32 (8x4) | 4x U8 | 顶点 1 着色计算输出颜色 (R1, G1, B1, CODE) |
| **22** | `RGB2` | 32 (8x4) | 4x U8 | 顶点 2 着色计算输出颜色 (R2, G2, B2, CODE) |
| **23** | `RES1` | 32 | - | 保留寄存器 (Reserved) |
| **24** | `MAC0` | 32 | S32 | 32 位通用累加器 0 (数学求和与除法结果) |
| **25** | `MAC1` | 32 | S32 (44-bit) | 44 位饱和累加器 1 (截断映射至 32 位，对应 X 轴通道) |
| **26** | `MAC2` | 32 | S32 (44-bit) | 44 位饱和累加器 2 (截断映射至 32 位，对应 Y 轴通道) |
| **27** | `MAC3` | 32 | S32 (44-bit) | 44 位饱和累加器 3 (截断映射至 32 位，对应 Z 轴通道) |
| **28** | `IRGB` | 32 | - | 15-bit 转换输入颜色 (数据转入 16 位格式) |
| **29** | `ORGB` | 32 | - | 15-bit 转换输出颜色 (5-5-5 格式) |
| **30** | `LZCS` | 32 | S32 | 前导零计数输入数据源 (Count Leading Zeros Data) |
| **31** | `LZCR` | 32 | U32 | 前导零计数计算结果 (Leading Zeros Count Result) |

#### GTE 控制寄存器 (Control Registers: COP2 `r32` .. `r63`)

| 寄存器号 | 符号 (Symbol) | 位宽 (Bits) | 格式 (Format) | 功能与用途描述 |
| :---: | :---: | :---: | :---: | :--- |
| **32** | `R11R12` | 32 (16+16) | 2x S16 | 旋转矩阵：第 1 行，第 1 列与第 2 列 (1.3.12 定点) |
| **33** | `R13R21` | 32 (16+16) | 2x S16 | 旋转矩阵：第 1 行第 3 列，第 2 行第 1 列 |
| **34** | `R22R23` | 32 (16+16) | 2x S16 | 旋转矩阵：第 2 行第 2 列，第 2 行第 3 列 |
| **35** | `R31R32` | 32 (16+16) | 2x S16 | 旋转矩阵：第 3 行第 1 列，第 3 行第 2 列 |
| **36** | `R33` | 16 | S16 | 旋转矩阵：第 3 行第 3 列 (高 16 位补零) |
| **37** | `TRX` | 32 | S32 (1.19.12) | 世界坐标到相机空间的 X 平移分量 |
| **38** | `TRY` | 32 | S32 (1.19.12) | 世界坐标到相机空间的 Y 平移分量 |
| **39** | `TRZ` | 32 | S32 (1.19.12) | 世界坐标到相机空间的 Z 平移分量 |
| **40..44** | `L11..L33` | 32x5 | 2x S16 | 光照方向矩阵 (Light Direction Matrix 3x3) |
| **45..47** | `RBK..BBK` | 32x3 | S32 | 背景环境光原色 (Background Color Red, Green, Blue) |
| **48..52** | `LR1..LB3` | 32x5 | 2x S16 | 光源颜色矩阵 (Light Color Matrix 3x3) |
| **53..55** | `RFC..BFC` | 32x3 | S32 (1.19.12) | 远景雾化原色 (Far Color Red, Green, Blue) |
| **56** | `OFX` | 32 | S32 (1.19.12) | 屏幕投影中心水平偏移量 (Screen Offset X, 对应显示区中心) |
| **57** | `OFY` | 32 | S32 (1.19.12) | 屏幕投影中心垂直偏移量 (Screen Offset Y) |
| **58** | `H` | 16 | U16 | 摄像机焦距投影距离 (Projection Distance, 类似视角 FOV 控制) |
| **59** | `DQA` | 16 | S16 (1.3.12) | 深度提示线性系数 A (Depth Cueing Factor A) |
| **60** | `DQB` | 32 | S32 (1.19.12) | 深度提示线性系数 B (Depth Cueing Factor B) |
| **61** | `ZSF3` | 16 | S16 | 3 点平均 Z 缩放因子 (Average Z Scale Factor 3) |
| **62** | `ZSF4` | 16 | S16 | 4 点平均 Z 缩放因子 (Average Z Scale Factor 4) |
| **63** | `FLAG` | 32 | Bitfield | **GTE 状态与算术溢出饱和标志寄存器** |

---

### 3.2 定点数格式、精度模型与 44-bit 饱和运算

GTE 采用纯整型定点运算：
1. **1.3.12 (S16)**：1 个符号位，3 个整数位，12 个小数位。表示范围为 $[-8.0, +7.999755]$。用于所有旋转矩阵、光照矩阵系数与中间分量 `IR1..IR3`。
2. **1.19.12 (S32)**：1 个符号位，19 个整数位，12 个小数位。用于平移向量、深度提示与中间乘加累加。
3. **44-bit 饱和累加器 (`MAC1..MAC3`)**：
   - 内部由 1 个符号位、31 个整数位、12 个小数位组成。
   - 在执行矩阵乘法 $\sum (Matrix[i][j] \times Vector[j])$ 时，每次乘法产生 30 位中间乘积，累加至 44 位累加器中。
   - 当累加结果超出 44 位范围时，触发相应溢出位，并饱和截断。

---

### 3.3 透视投影 `RTPS` / `RTPT` 与 UNR 倒数除法硬件表

在执行 `RTPS` (单点透视旋转变换) 或 `RTPT` (三点透视旋转变换) 时，核心步骤是将 3D 相机空间坐标 $(X, Y)$ 投影至 2D 屏幕坐标：
$$SX = OFX + X \times \frac{H}{SZ}, \quad SY = OFY + Y \times \frac{H}{SZ}$$

#### 硬件真实除法机制：UNR 查找表
真实硬件**没有任何浮点除法器**，甚至没有标准的 32 位整数除法单元！GTE 使用了一个 257 项的 8 位倒数查找表 (`unr_table`)，结合单步无符号牛顿-拉夫逊迭代 (Unsigned Newton-Raphson) 逼近 $\frac{1}{SZ}$。

#### 257 项 UNR 倒数表算法定义：
$$UNR\_TABLE[i] = \min\left(0x00, \left\lfloor\frac{0x40000}{i + 0x100} + 1\right\rfloor \div 2 - 0x101\right), \quad i \in [0, 256]$$

#### 硬件除法伪代码实现 (Bit-Identical to Hardware)：
```c
uint32_t GteDivide(uint16_t h, uint16_t sz) {
    if (sz == 0) {
        // 除以 0 处理：设置 FLAG 溢出位，返回饱和最大值
        gte_regs.flag |= GTE_FLAG_DIVIDE_OVERFLOW;
        return 0x1FFFF;
    }
    if (h < sz * 2) {
        // 正常范围计算
        int shift = __builtin_clz(sz) - 16;
        uint32_t d = sz << shift;
        uint32_t index = (d >> 7) & 0xFF;
        uint32_t s = (unr_table[index] + 0x101) << 1;
        uint32_t tmp = 0x2000000 - s * (d - 0x8000);
        uint32_t reciprocal = ((s * (tmp >> 16)) + ((s * (tmp & 0xFFFF)) >> 16)) >> 7;
        uint32_t res = ((uint64_t)h * reciprocal + 0x80000) >> 20;
        return (res > 0x1FFFF) ? 0x1FFFF : res;
    } else {
        // 溢出饱和
        gte_regs.flag |= GTE_FLAG_DIVIDE_OVERFLOW;
        return 0x1FFFF;
    }
}
```
*为什么必须精准模拟 UNR 表？*  
若模拟器直接调用 C 语言的 `h / sz` 浮点或整型除法，由于精度高于 PS1 真实硬件，在《古惑狼赛车 (CTR)》、《合金装备 (MGS)》、《寂静岭》等游戏中，多边形顶点会出现轻微错位与撕裂，部分基于深度边界的剔除测试会直接穿模！

---

### 3.4 核心 GTE 指令数学原理与硬件 Bug

#### 1. `NCLIP` (法线剪裁 / 2D 外积背面剔除)
通过计算屏幕坐标 3 点组成的三角形有向面积，判断其顺逆时针排列：
$$MAC0 = SX0 \times SY1 + SX1 \times SY2 + SX2 \times SY0 - SX0 \times SY2 - SX1 \times SY0 - SX2 \times SY1$$
- 若 $MAC0 > 0$：多边形面向摄像机，可见；
- 若 $MAC0 \le 0$：多边形背对摄像机，直接剔除，无需发往 GPU。

#### 2. `MVMVA` (矩阵向量乘法与累加) 及其硬件 Bug
`MVMVA` 编码格式中包含 3 组控制位：`mx` (选择矩阵: 0=旋转, 1=光照, 2=颜色), `v` (选择向量: 0=V0, 1=V1, 2=V2, 3=IR), `cv` (选择平移向量: 0=TR, 1=BK, 2=FC/Bugged, 3=None)。

*著名的 `cv = 2` (Far Color) 硬件 Bug*：
当程序设置 `cv = 2` 试图使用远景颜色 (RFC, GFC, BFC) 作为平移加数时，硬件实现出现内部总线竞争：
- 硬件不会将加数送入 MAC 累加器的高位，而是将 `RFC/GFC/BFC` 与当前内部暂存器进行错误的垃圾移位异或；
- 此外，当 `v = 3` 时，输入向量直接绑定到 `IR1..IR3`，而非常规的顶点寄存器。
- 模拟器必须精确复刻该 Bug，否则《铁拳 3》、《最终幻想 VIII》中的某些特殊光照与模型会出现严重颜色崩坏。

#### 3. `AVSZ3` / `AVSZ4` (平均 Z 深度计算)
用于为 GPU 排序表 (Ordering Table) 生成索引深度：
$$SZ_{avg} = ZSF3 \times (SZ1 + SZ2 + SZ3) \quad (\text{三点平均})$$
$$SZ_{avg} = ZSF4 \times (SZ0 + SZ1 + SZ2 + SZ3) \quad (\text{四点平均})$$
将累加结果右移 12 位后写入 `MAC0` 与 `OTZ` 寄存器。

#### 4. 光照着色指令 (`NCDS`, `NCDT`, `NCCS`)
- `NCDS` (Normal Color Depth Cue Single)：计算单顶点的法线光照向量乘法，结合光源颜色与背景色，最后应用远景深度雾化插值。
- `NCDT` (Normal Color Depth Cue Triple)：在一条指令内连续处理 3 个顶点的光照雾化，通常与 `RTPT` 成对出现在高性能 3D 引擎循环中。

---

### 3.5 `FLAG` 状态寄存器 (Bit 31 综合饱和机制)

`FLAG` (`COP2 r63`) 记录了各项运算的溢出、饱和与下溢情况。

| 位 (Bit) | 描述 (Description) | 触发条件 |
| :---: | :--- | :--- |
| **31** | **Error / Master Saturation Flag** | 当 Bits 30..23 或 Bits 18..13 中任一位为 1 时，硬件自动将 Bit 31 置 1！ |
| **30** | MAC1 44-bit 正溢出 | MAC1 结果大于 $+2^{43}-1$ |
| **29** | MAC2 44-bit 正溢出 | MAC2 结果大于 $+2^{43}-1$ |
| **28** | MAC3 44-bit 正溢出 | MAC3 结果大于 $+2^{43}-1$ |
| **27** | MAC1 44-bit 负溢出 | MAC1 结果小于 $-2^{43}$ |
| **26** | MAC2 44-bit 负溢出 | MAC2 结果小于 $-2^{43}$ |
| **25** | MAC3 44-bit 负溢出 | MAC3 结果小于 $-2^{43}$ |
| **24** | IR1 饱和溢出 | 结果超出 $[-0x8000, +0x7FFF]$ |
| **23** | IR2 饱和溢出 | 结果超出 $[-0x8000, +0x7FFF]$ |
| **22** | IR3 饱和溢出 | 结果超出 $[-0x8000, +0x7FFF]$ |
| **21..19** | 色彩分量 R, G, B 饱和 | 计算出的颜色分量超出 $[0, 255]$ (0x00..0xFF) |
| **18** | SZ3 / OTZ 屏幕 Z 溢出 | 屏幕深度超出 16 位范围 ($> 0xFFFF$) |
| **17** | 除法溢出 / 除以零 | 透视投影除法时 $H \ge SZ \times 2$ 或 $SZ=0$ |
| **16..13** | 屏幕坐标 SX, SY 饱和 | 投影后的屏幕坐标超出 $[-1024, +1023]$ 范围 |

---

### 3.6 热点 GTE 指令与 3DS ARM 内联代码生成策略

对上百款 PS1 商业游戏的性能剖析 (Profiling) 表明：**以下 6 条 GTE 指令占领了 95% 以上的 GTE 运行周期**：
1. `RTPT` (三点透视变换) - 约 35%
2. `RTPS` (单点透视变换) - 约 25%
3. `NCLIP` (背面剪裁) - 约 15%
4. `MVMVA` (通用矩阵乘法) - 约 10%
5. `DPCS` / `INTPL` (深度提示插值) - 约 6%
6. `AVSZ3` / `AVSZ4` (排序表计算) - 约 4%

#### 3DS ARM11 JIT 优化方案：
在 ARM11 上，如果通过 `BL gte_rtps` 调用 C 函数，进出函数需要保存恢复寄存器，至少耗费 30~50 周期。
而将 `NCLIP`、`AVSZ3/4` 与 `RTPS` 的矩阵乘法直接编译为 ARMv6 汇编内联：
- 充分利用 ARMv6 的双 16 位乘加指令：`SMLAD` (有符号双半字乘加累加至 32 位)；
- 使用 `USAT` / `SSAT` (单周期任意位宽饱和截断指令) 替代繁琐的 C 语言分支判断：
  `SSAT r1, #16, r0` (仅用 1 条指令即可将 32 位累加值饱和到 16 位有符号定点数！)；
- 这使得整个 GTE 流水线的吞吐提升 300% 以上。

---

## 4. GPU (`GP0` + `GP1`), DMA 与 定时器 (Root Counters)

PlayStation 1 的 GPU 负责所有 2D/3D 图形渲染，拥有 1MB (1024x512 像素，16-bit 像素深度) 的独立显存 (VRAM)。CPU 通过两组 32 位 I/O 端口与 GPU 交互：
- **`GP0` (`0x1F801810` Write)**：图形绘制命令与数据包发送端口。
- **`GP1` (`0x1F801814` Write)**：GPU 显示环境、控制与复位命令端口。
- **`GPUREAD` (`0x1F801810` Read)**：从 VRAM 读取像素数据回送 CPU 的端口。
- **`GPUSTAT` (`0x1F801814` Read)**：GPU 状态寄存器。

---

### 4.1 GP0 核心渲染与控制命令参考大全

GP0 指令以 32 位字 (Word) 发送，高 8 位 (`Bits 24..31`) 为命令操作码 (Opcode)。

```
GP0 命令字格式:
 31       24 23       16 15        8 7          0
+-----------+-----------+-----------+------------+
|  Command  |    Red    |   Green   |    Blue    |
+-----------+-----------+-----------+------------+
```

#### 1. 多边形绘制指令集 (`0x20` .. `0x3F`)
操作码格式定义 (二进制 `001_s_t_q_i_r`):
- Bit 28: 单色/纯色 (`0`) vs 带纹理 (`1`)
- Bit 27: 3 顶点三角形 (`0`) vs 4 顶点四边形 (`1`)
- Bit 26: 纯色 Flat (`0`) vs Gouraud 高洛德双线性颜色插值 (`1`)
- Bit 25: 调制混合 (`0`) vs 原始纯纹理 Raw Texture (`1`)
- Bit 24: 关闭半透明 (`0`) vs 开启半透明 Semi-Transparent (`1`)

| 操作码 (Hex) | 顶点数 | 纹理 | 着色模式 | 半透明 | 数据包总字数 (Words) | 参数序列 (Packet Sequence) |
| :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| `0x20` | 3 | 无 | Flat | 关 | 4 | `C0, V0, V1, V2` |
| `0x22` | 3 | 无 | Flat | **开** | 4 | `C0, V0, V1, V2` |
| `0x24` | 3 | **有** | Flat | 关 | 7 | `C0, V0, T0(CLUT), V1, T1(Page), V2, T2` |
| `0x26` | 3 | **有** | Flat | **开** | 7 | 同上 |
| `0x28` | 4 | 无 | Flat | 关 | 5 | `C0, V0, V1, V2, V3` |
| `0x2C` | 4 | **有** | Flat | 关 | 9 | `C0, V0, T0, V1, T1, V2, T2, V3, T3` |
| `0x30` | 3 | 无 | **Gouraud** | 关 | 6 | `C0, V0, C1, V1, C2, V2` |
| `0x34` | 3 | **有** | **Gouraud** | 关 | 9 | `C0, V0, T0, C1, V1, T1, C2, V2, T2` |
| `0x38` | 4 | 无 | **Gouraud** | 关 | 8 | `C0, V0, C1, V1, C2, V2, C3, V3` |
| `0x3C` | 4 | **有** | **Gouraud** | 关 | 12 | `C0, V0, T0, C1, V1, T1, C2, V2, T2, C3, V3, T3` |

#### 2. 直线渲染指令集 (`0x40` .. `0x5F`)
- `0x40`：单色单线 (Line: 2 顶点，3 Words)
- `0x48`：单色折线 (Poly-line: 多顶点，终止标志为顶点坐标出现 `0x55555555`)
- `0x50`：Gouraud 渐变色单线 (4 Words)
- `0x58`：Gouraud 渐变色折线

#### 3. 矩形与精灵指令集 (`0x60` .. `0x7F`)
- `0x60`：任意尺寸纯色矩形 (`C0, V0, Size(W, H)`)
- `0x64`：任意尺寸纹理矩形 (Sprite: `C0, V0, T0(CLUT), Size(W, H)`)
- `0x68`：单像素点 (1x1 Dot: `C0, V0`)
- `0x70`：8x8 纯色矩形
- `0x74`：8x8 纹理精灵
- `0x78`：16x16 纯色矩形
- `0x7C`：16x16 纹理精灵

#### 4. VRAM 传输与填充指令
- **`0x02` - 快速矩形填充 (FillRect in VRAM)**：
  - 参数：`C0 (RGB), (X, Y), (Width, Height)`
  - 功能：在显存内部以固定颜色填充矩形，常用于清屏。坐标按 16 像素对齐，尺寸按 16 像素对齐。
- **`0xA0` - CPU 到 VRAM 传输 (Image Load / Upload)**：
  - 参数：`0xA0, (DestX, DestY), (Width, Height), PixelData...`
  - 数据字数：$\lceil(Width \times Height) \div 2\rceil$ Words。常通过 DMA 通道 2 传输纹理至显存。
- **`0xC0` - VRAM 到 CPU 传输 (Image Store / Download)**：
  - 请求显存回传像素数据至 `GPUREAD` 端口。
- **`0x80` - VRAM 到 VRAM 拷贝 (Move Image / Copy)**：
  - 在显存内部快速搬运矩形块 (常用于双缓冲页面翻转或视频播放缓冲区移动)。

#### 5. 渲染环境控制指令 (`0xE1` .. `0xE6`)
- `0xE1`：设置绘图模式 (Draw Mode / TPage)
- `0xE2`：设置纹理窗口 (Texture Window Mask)
- `0xE3`：设置绘图区左上角边界 (Drawing Area Top-Left: $X_1, Y_1$)
- `0xE4`：设置绘图区右下角边界 (Drawing Area Bottom-Right: $X_2, Y_2$)
- `0xE5`：设置绘图原点偏移量 (Drawing Offset: $OffsetX, OffsetY$)
- `0xE6`：设置掩码位控制 (Mask Bit Setting: 写入是否强制打 Mask，绘制时是否检测 Mask)

---

### 4.2 GP1 状态控制与 `GPUSTAT` (`0x1F801814`) 全景定义

#### `GPUSTAT` 状态寄存器位图详解 (32-bit Read)

```
 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|IL|DMA |F |R |C |D |I |D |IL|C |P |V |HR |H2|Y2|R |F |M |M |D |D |TP |ABR|TY|  TexturePageX   |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
```

| 位 (Bit) | 字段名 | 含义与取值说明 |
| :---: | :--- | :--- |
| **0..3** | Texture Page X Base | 纹理页 X 基址 (每单位 64 字节: $N \times 64$) |
| **4** | Texture Page Y Base | 纹理页 Y 基址 (0=0, 1=256) |
| **5..6** | Semi-Transparency Mode (ABR) | 当前半透明混合模式 (0=0.5B+0.5F, 1=B+F, 2=B-F, 3=B+0.25F) |
| **7..8** | Texture Page Colors | 纹理色彩深度 (0=4-bit Indexed, 1=8-bit Indexed, 2=15-bit Direct) |
| **9** | Dither Enable | 抖动开关 (0=关闭, 1=开启 24-bit 到 15-bit 空间抖动) |
| **10** | Drawing to display allowed | 是否允许在当前正在显示的显存区域绘制 (0=禁止, 1=允许) |
| **11** | Set Mask-bit when drawing | 像素写入时是否强制将最高位 (Bit 15) 置 1 (0=否, 1=是) |
| **12** | Draw pixels relative to mask | 掩码检查 (0=总是绘制, 1=若目标像素 Bit 15 已被置 1 则禁止绘制) |
| **13** | Interlace Field | 隔行扫描场状态 (0=偶数场 Even/Bottom, 1=奇数场 Odd/Top) |
| **14** | Reverse Flag | 屏幕水平翻转标志 |
| **15** | Texture Page Y Base 2 | 纹理页 Y 基址扩展位 (对应 1MB VRAM 的下半部分) |
| **16** | Horizontal Resolution 2 | 水平分辨率标志 2 (1=368 像素宽度) |
| **17..18**| Horizontal Resolution 1 | 水平分辨率标志 1 (0=256, 1=320, 2=512, 3=640 像素) |
| **19** | Vertical Resolution | 垂直分辨率 (0=240 线, 1=480 线 - 仅在隔行使能时生效) |
| **20** | Video Standard | 视频制式 (0=NTSC 60Hz, 1=PAL 50Hz) |
| **21** | Color Depth | 显存输出色彩模式 (0=15-bit RGB, 1=24-bit True Color) |
| **22** | Vertical Interlace | 垂直隔行扫描使能 (0=关闭逐行扫描, 1=开启隔行扫描) |
| **23** | Display Disable | 显示使能/黑屏 (0=正常显示画面, 1=屏幕强制完全消隐黑屏) |
| **24** | Interrupt Request (IRQ1) | GPU 中断挂起标志位 (0=无中断, 1=IRQ1 已触发) |
| **25** | DMA Request / Ready | DMA 数据请求信号 (根据 GP1(04h) 设置反映 FIFO 准备状态) |
| **26** | **Ready to receive Command (GP0)** | **准备好接收新命令** (0=忙碌, 1=就绪)。**死锁核心关键位！** |
| **27** | **Ready to send VRAM to CPU** | **VRAM 读 FIFO 准备就绪** (0=无数据, 1=有数据可从 GPUREAD 读取) |
| **28** | **Ready to receive DMA Block** | **写 FIFO 空闲就绪** (0=忙碌, 1=就绪)。**死锁核心关键位！** |
| **29..30**| DMA Direction | 当前 DMA 方向 (0=关闭, 1=FIFO状态, 2=CPU到GP0, 3=GPUREAD到CPU) |
| **31** | **Drawing interlace field line** | 正在绘制奇数行还是偶数行 (逐行/隔行模式下随扫描线或场翻转) |

#### 致命死锁警示：为什么游戏会在 `GPUSTAT` 无限自旋？
游戏代码和 BIOS 中充满了形如以下的等待循环：
```c
// 等待 GPU 就绪接收下一个多边形数据包：
while (!(GPUSTAT & (1 << 26)));

// 等待 DMA 传输 FIFO 空闲：
while (!(GPUSTAT & (1 << 28)));
```
如果模拟器在处理完一个绘制命令后，未能及时将 Bit 26 与 Bit 28 恢复置 `1`，系统将在进入开机动画前发生**永久死锁 (Infinite Spinloop)**！

---

### 4.3 纹理管线、色彩查找表 (CLUT) 与 纹理窗口 (Texture Window)

#### 色彩深度解析
PS1 显存为统一的 16 位半字数组。纹理坐标 $(U, V)$ 访问方式由 TPage 指定：
1. **4-bit Indexed (16色)**：每个 16 位显存半字存放 4 个像素索引。必须结合由指令传入的 `CLUT` (Color Lookup Table) 坐标 $(X, Y)$，从显存指定位置读出 16 个 16-bit RGB555 颜色进行查找。
2. **8-bit Indexed (256色)**：每个显存半字存放 2 个像素索引，从 CLUT 查找 256 个 RGB555 颜色。
3. **15-bit Direct RGB555**：直接从显存获取 16-bit 像素数据：
   - Bits 0..4: Red (0..31)
   - Bits 5..9: Green (0..31)
   - Bits 10..14: Blue (0..31)
   - Bit 15: **Mask / Semi-Transparency 标志位**！

#### 纹理窗口掩码算法 (Texture Window Wrapping)
许多游戏 (如《铁拳》、《实况足球》地面纹理) 使用纹理窗口功能实现纹理无缝重复平铺，而无需将 UV 限制在 $[0, 255]$：
由 GP0(`0xE2`) 设置：`MaskX` (5 bits), `MaskY` (5 bits), `OffsetX` (5 bits), `OffsetY` (5 bits)。
实际采样点计算公式：
$$U_{final} = (U \ \& \ \sim(MaskX \times 8)) \ | \ ((OffsetX \ \& \ MaskX) \times 8)$$
$$V_{final} = (V \ \& \ \sim(MaskY \times 8)) \ | \ ((OffsetY \ \& \ MaskY) \times 8)$$

---

### 4.4 半透明混合公式 (ABR 0..3) 与 掩码位控制

PS1 硬件将背景像素记为 **$B$ (Back / Destination)**，当前绘制的图元前景像素记为 **$F$ (Front / Source)**。半透明混合由 TPage 中的 `ABR` (Bits 5..6) 决定：

| ABR 模式 | 混合数学公式 | 视觉效果与典型应用场景 |
| :---: | :--- | :--- |
| **0** | **$0.5 \times B + 0.5 \times F$** | **标准 50% 混合**。用于普通玻璃、半透明阴影、水面倒影 |
| **1** | **$1.0 \times B + 1.0 \times F$** | **加法混合 (Additive)**。用于火焰、激光、爆炸、魔法光晕 |
| **2** | **$1.0 \times B - 1.0 \times F$** | **减法混合 (Subtractive)**。用于暗影、夜视仪、黑洞、负光效果 |
| **3** | **$1.0 \times B + 0.25 \times F$** | **25% 增强混合**。用于高光反射、微光织物 |

*注*：所有计算按 RGB 各通道独立进行，若发生溢出必须饱和截断在 $[0, 255]$ 范围内，禁止回绕！

#### 像素 Bit 15 掩码语义 (Mask Bit Control)
- **GP0(`0xE6`) Bit 0 (`Set Mask Bit`)**：若置 1，所有绘制写入 VRAM 的像素，其最高位 Bit 15 强制被写入 `1`；否则写入 `0` (或保持纹理自带位)。
- **GP0(`0xE6`) Bit 1 (`Check Mask Bit`)**：若置 1，在绘制每个像素前，首先读取目标显存像素；**若目标像素的 Bit 15 已经为 1，则硬件放弃写入该像素**！常用于实现 3D 角色在 2D 预渲染背景掩码之后的遮挡剔除。

---

### 4.5 DMA 控制器 (7 大通道) 与 链表传输 (DMA2 & DMA6)

PS1 配备了 7 个独立的 DMA 通道，支持在外设与主 RAM 之间进行无需 CPU 干预的高速总线突发传输。

#### DMA 7 大通道职能清单

| 通道号 (Channel) | 寄存器基址 | 对应外设 (Device) | 常用同步模式 (Sync Mode) | 核心应用场景 |
| :---: | :---: | :--- | :---: | :--- |
| **DMA0** | `0x1F801080` | **MDEC In** | Mode 1 (Request) | 向 MDEC 解码器灌入宏块数据 |
| **DMA1** | `0x1F801090` | **MDEC Out** | Mode 1 (Request) | 从 MDEC 取出解压完成的 RGB 图像 |
| **DMA2** | `0x1F8010A0` | **GPU** | **Mode 2 (Linked List)** | 向 GPU 灌送渲染排序表 (Ordering Table) 数据包 |
| **DMA3** | `0x1F8010B0` | **CD-ROM** | Mode 1 (Request) | 从 CD-ROM 扇区高速读取数据至 RAM |
| **DMA4** | `0x1F8010C0` | **SPU** | Mode 1 (Request) | 向 SPU 声音缓存传输 ADPCM 音频样本 |
| **DMA5** | `0x1F8010D0` | **PIO / Expansion**| Mode 0 (Immediate) | 扩展口数据传输 |
| **DMA6** | `0x1F8010E0` | **OTC** | Mode 0 (Immediate) | **Ordering Table Clear (快速清空反向构建排序表)** |

#### DMA2 GPU 链表模式 (Linked-List Mode 2) 深度解析
这是 PS1 图形引擎的核心骨架：CPU 与 GTE 在 RAM 中计算出三角形数据后，将多个图元数据包组织为一个单向链表，称为 **Ordering Table (OT 排序表)**。
- 链表头指针存放在 `D2_MADR`。
- 每个数据包的第一个 32 位字为**包头 (Packet Header)**：
  - `Bits 0..23`：指向下一个数据包的物理 RAM 地址 (`Next Pointer`)。
  - `Bits 24..31`：当前数据包中包含的 32 位字数量 (`Packet Size` $N$)。
- DMA 引擎依次将后续的 $N$ 个字发送至 GPU `GP0` 端口。
- **终止条件**：当读取到的 `Next Pointer == 0x00FFFFFF` (或最高位标记结束) 时，链表传输彻底完成，硬件停止并拉高 `DICR` 相应中断位。

```
[RAM: Ordering Table]
Addr 0x00100000: [0x00100020 | (3 << 24)]  --> Packet 1 Header (Size = 3 Words)
Addr 0x00100004: GP0 0x200000FF (Draw Triangle Flat)
Addr 0x00100008: Vertex 0 (X, Y)
Addr 0x0010000C: Vertex 1 (X, Y)
Addr 0x00100010: Vertex 2 (X, Y)
      |
      +---> Addr 0x00100020: [0x00100040 | (2 << 24)] --> Packet 2 Header (Size = 2)
            Addr 0x00100024: GP0 0xE1000400 (Set Draw Mode)
            Addr 0x00100028: GP0 0xE3000000 (Set Clip Area)
                  |
                  +---> Addr 0x00100040: [0x00FFFFFF | (0 << 24)] --> End of List!
```

#### DMA6 OTC (Ordering Table Clear) 硬件倒序链表生成
在每帧开始绘制前，必须清空 OT 表。如果用 CPU 软件循环写入清空，需要成千上万个周期。PS1 硬件设计了 DMA6 专门用于硬件硬件生成这个链表：
- `D6_MADR` 设置为 OT 表尾地址。
- `D6_BCR` 设置为 OT 表长度。
- 方向设置为递减 (`Step = -4 bytes`)。
- DMA6 会以纯硬件总线速度，从表尾向表头自动填充每一个指针，使其准确指向前一个条目，并将表头条目填入 `0x00FFFFFF`！

---

### 4.6 根计数器 / 定时器 (Root Counters 0/1/2) 时钟与门控

PS1 拥有 3 个通用定时器：
- **Timer 0 (`0x1F801100`)**：
  - 时钟源 0：系统时钟 (33.8688 MHz)
  - 时钟源 1：**Dotclock (像素时钟)**，频率随当前水平分辨率改变 (例如 320 模式下为 53.69MHz/8 = 6.71MHz)
  - 门控模式：可配置为在 **HBlank** 期间暂停计数，或仅在 HBlank 期间计数。
- **Timer 1 (`0x1F801110`)**：
  - 时钟源 0：系统时钟 (33.8688 MHz)
  - 时钟源 1：**HBlank (水平消隐时钟)**，每行扫描线触发一次 (~15.734 kHz NTSC)
  - 门控模式：可配置为在 **VBlank** 期间暂停计数。常被游戏用作高精度垂直场计时器。
- **Timer 2 (`0x1F801120`)**：
  - 时钟源 0/1：系统时钟 (33.8688 MHz)
  - 时钟源 2/3：**系统时钟除以 8 (SysClk / 8 $\approx$ 4.2336 MHz)**
  - 自由运行定时器，没有外部门控引脚。常被用作通用操作系统时间基准 (`GetRCnt`)。

---

## 5. CD-ROM, SPU, MDEC, SIO0 与 BIOS 系统

### 5.1 CD-ROM 驱动控制器 (`0x1F801800..0x1F801803`)

CD-ROM 控制器由独立的 Motorola HC05 微控制器驱动，负责盘片寻道、纠错编码 (ECC/EDC) 与物理扇区提取。

#### 寄存器 Bank 索引机制
CPU 访问基址 `0x1F801800`，写入 `Bits 0..1` 可在 4 个不同的寄存器 Bank 之间切换：

| 偏移地址 | 读写 | Bank 0 | Bank 1 | Bank 2 | Bank 3 |
| :---: | :---: | :--- | :--- | :--- | :--- |
| `0x1F801800` | Read | `1F801800h` - Status Register (全部 Bank 共享) |
| `0x1F801801` | Read | Response FIFO | Response FIFO | - | - |
| `0x1F801802` | Read | Data FIFO | - | - | - |
| `0x1F801803` | Read | Interrupt Enable | Interrupt Flag | Interrupt Enable | Interrupt Flag |
| `0x1F801800` | Write| Bank Select | Bank Select | Bank Select | Bank Select |
| `0x1F801801` | Write| Command Register| - | Sound Map | Sound Map |
| `0x1F801802` | Write| Parameter FIFO | - | Sound Attenuation| Sound Attenuation |
| `0x1F801803` | Write| Request Register| Interrupt Acknowledge| Audio Attenuation| - |

#### CD-ROM INT1..INT5 中断与响应时序模型
CD-ROM 驱动器通过专用的中断多路复用报告状态：
- **`INT0` (`NoIntr`)**：无中断挂起。
- **`INT1` (`DataReady`)**：数据就绪。当执行 `ReadN` 或 `ReadS` 时，新扇区数据已载入数据缓存区，可发起 DMA3 提取。
- **`INT2` (`Complete`)**：第二阶段执行完毕。例如 `SeekL`、`Play`、`Init` 最终动作完成。
- **`INT3` (`Acknowledge`)**：命令接收确认。几乎所有命令在发出后，驱动器首先在极短延迟后触发 `INT3`，并在 Response FIFO 中写入 1 字节当前驱动状态。
- **`INT4` (`DataEnd`)**：数据轨道结束 (End of Disc / Auto-pause)。
- **`INT5` (`DiskError`)**：盘片读取错误、光驱盖被打开或防盗版 Wobble 校验失败。

*双阶段中断队列 (Queued Interrupts)*：
以 `Setloc` + `ReadN` 为例：
1. CPU 发送命令 `0x06` (`ReadN`)；
2. 驱动器在 ~30,000 周期后触发 **`INT3` (Acknowledge)**，返回状态字；
3. CPU 在中断处理例程中向 `0x1F801803` 写入 `0x40` 确认并清除 `INT3`；
4. 随后在经过盘片寻道与旋转延迟 (~200,000 周期) 后，当物理扇区到达磁头，控制器生成 **`INT1` (DataReady)**；
5. *规则*：后续中断必须被排队挂起，直到 CPU 确认上一个中断，绝对不能同时覆盖发送！

---

### 5.2 SPU (声音处理单元) 与 高斯插值模型

PS1 的 SPU 支持 **24 个独立的立体声硬件声道 (Hardware Voices)**，具备 512KB 的专用音频 SRAM，固定以 **44.1 kHz** 采样率混音输出。

#### 1. 16 字节 SPU-ADPCM (VAG) 格式
音频样本按 16 字节块进行压缩，每个块解压缩出 28 个 16 位 PCM 样本：
- **Byte 0**：`Shift` (Bits 0..3: 缩放位移) 与 `Filter` (Bits 4..6: 预测滤波器索引 0..4)
- **Byte 1**：标志字节 (`Flags`):
  - Bit 0: Loop End (单向循环结束)
  - Bit 1: Loop Repeat (是否重复播放)
  - Bit 2: Loop Start (循环起始点标记)
- **Bytes 2..15**：14 字节的压缩数据，每个字节包含 2 个 4 位 Nibble 样本 (低 4 位在前，高 4 位在后)。

#### 5 种预测滤波系数：
$$s[n] = sample \times 2^{12 - shift} + f_0 \times s[n-1] + f_1 \times s[n-2]$$
- Filter 0: $f_0 = 0, \quad f_1 = 0$
- Filter 1: $f_0 = +\frac{60}{64}, \quad f_1 = 0$
- Filter 2: $f_0 = +\frac{115}{64}, \quad f_1 = -\frac{52}{64}$
- Filter 3: $f_0 = +\frac{98}{64}, \quad f_1 = -\frac{55}{64}$
- Filter 4: $f_0 = +\frac{122}{64}, \quad f_1 = -\frac{60}{64}$

#### 2. 4 点高斯插值 (Gaussian Interpolation) 与 512 项加权表
当声道的音高 (Pitch) 寄存器不等于 `0x1000` (44.1kHz 基准) 时，重采样位置会出现小数分量。SPU 使用当前采样点以及前后共 4 个采样点，结合一个 **512 项的高斯权重查找表 (`gauss_table`)** 进行插值：
$$Out = \sum_{k=0}^{3} s[n-k] \times GaussWeight(k, frac)$$
高斯滤波赋予了 PS1 音频温暖、厚重且平滑的特有音质，同时彻底消除了高频数字折叠失真 (Aliasing)。

#### 3. ADSR 包络发生器
每个声道拥有独立的 Attack (上升), Decay (衰减), Sustain (维持), Release (释放) 硬件状态机，支持线性 (Linear) 与指数 (Exponential) 增减曲线。

---

### 5.3 MDEC (宏块解码器) 视频硬件加速

MDEC 是用于全动态视频 (FMV / CG 动画) 的硬件解压缩单元：
- 端口 `0x1F801820` (Data / Command) 与 `0x1F801824` (Status / Control)。
- 采用与 MPEG-1 / JPEG 类似的离散余弦变换 (DCT)：
  1. 接收变长霍夫曼编码或 Run-Length 对；
  2. 使用 64 项反量化表 (Quantization Table) 进行反量化；
  3. 执行 8x8 **反离散余弦变换 (IDCT)**；
  4. 将 $YC_bC_r$ 4:2:0 或 4:2:2 色彩空间实时转换为 15 位 RGB555 或 24 位 RGB888。
- 视频播放通常结合 DMA0 (灌入压缩码流) 与 DMA1 (提取解码像素)，极少占用 CPU 算力。

---

### 5.4 SIO0 (手柄与存储卡串行接口)

SIO0 采用类似 SPI 的全双工同步串行协议，时钟频率约为 250 kHz。

#### 1. 标准手柄轮询协议 (Joypad Communication)
- 主机拉低 `/SEL`，发送起始字节 `0x01`；手柄拉低 `/ACK`，返回 `0xFF`。
- 主机发送查询命令 `0x42`；手柄返回设备 ID：
  - `0x41`：标准数字手柄 (Digital Pad)
  - `0x73`：DualShock 双震动模拟手柄 (Analog Pad)
- 主机发送 `0x00`；手柄返回确认标志 `0x5A`。
- 主机连续发送 `0x00`；手柄返回 2 字节按键状态 (每一位代表一个按键，`0` 表示按下，`1` 表示弹起)。
- 若为 DualShock，后续继续回传 4 字节右/左摇杆的 X/Y 轴绝对位置值 ($0x00..0xFF$)。

#### 2. 存储卡协议 (Memory Card)
- 类似流程，选择命令为 `0x81`。
- 支持扇区读取 (`0x52` / 'R') 与扇区写入 (`0x57` / 'W')，每个扇区 128 字节，末尾附带 1 字节 XOR 校验和。

---

### 5.5 BIOS 启动流程、HLE 系统调用拦截与 PS-EXE 侧载

#### 1. SCPH1001 真实硬件冷启动流程 (Cold Boot)
1. CPU 上电复位，强制将 PC 置为 `0xBFC00000` (ROM 起始地址)；
2. 基础硬件自检，配置 `RAM_SIZE` (`0x1F801060`)，将主板芯片组总线等待时序写入 `0x1F801000..0x1F801020`；
3. 初始化 CPU 缓存：向 `0xFFFE0130` 写入控制字，置位 `IsC=1`，遍历写入清空 I-Cache，随后恢复 `IsC=0`；
4. 清空 2MB 主 RAM 低端 BSS 区域；
5. 在 `0x80000080` 处安装通用异常处理分发桩代码；
6. 挂载安装 `A0`, `B0`, `C0` 系统调用函数跳转表；
7. 启动 GPU，设置初始显示模式，播放 Sony 标志性的环境音效与立体声多边形开机动画；
8. 激活 CD-ROM 驱动器，向光驱发出初始化与许可校验命令；
9. 读取盘片第 16 扇区文件系统，解析 `SYSTEM.CNF` 文件，寻找主可执行文件路径 (如 `BOOT = cdrom:\SLUS_001.23;1`)；
10. 将游戏 PS-EXE 加载至 RAM，跳转至入口点执行。

#### 2. HLE (高级模拟) BIOS 系统调用拦截
在不合法分发专有 BIOS ROM 时，模拟器可采用 HLE 方案。MIPS 程序通过跳转至 `0xA0`, `0xB0`, `0xC0` 加上寄存器 `$t1` 或 `$v0` 中的功能号调用系统服务：
- **`A0` 表 (核心内核函数)**：
  - `0x13`: `setjmp`
  - `0x14`: `longjmp`
  - `0x3E`: `puts`
  - `0x3F`: `printf` (常用，可直接重定向至主机控制台日志输出)
  - `0x44`: `flushcache` (刷新 I-Cache)
- **`B0` 表 (驱动与事件调度函数)**：
  - `0x08`: `DeliverEvent`
  - `0x0A`: `WaitEvent`
  - `0x18`: `ReturnFromException`
  - `0x5B`: `ChangeClearPAD`
- **`C0` 表 (中断与内存配置)**：
  - `0x0A`: `SysInitMemory`
  - `0x1C`: `EnqueueSysCallHandler`

#### 3. PS-EXE 侧载格式解析 (Direct Sideloading)
直接运行 `.exe` / `.psx` 文件的结构定义：
- 文件前 2048 字节为标准头部：
  - `Offset 0x0000` (8 bytes)：固定魔数 ASCII 字符串 `"PS-X EXE"`
  - `Offset 0x0010` (4 bytes)：程序初始入口点 PC (`initial_pc`，通常为 `0x80010000`)
  - `Offset 0x0014` (4 bytes)：初始全局指针 GP (`initial_gp`)
  - `Offset 0x0018` (4 bytes)：目标主内存加载基地址 (`load_addr`，通常为 `0x80010000`)
  - `Offset 0x001C` (4 bytes)：二进制代码数据段长度 (`file_size`)
  - `Offset 0x0030` (4 bytes)：初始栈指针基地址 SP (`initial_sp`，通常为 `0x801FFF00`)
- 模拟器加载逻辑：解析头部后，直接将文件偏移 `0x0800` 开始的 `file_size` 字节复制至主内存 `load_addr` 处，设置 CPU 寄存器并直接将 PC 导向 `initial_pc` 即可秒开游戏。

---

## 6. 导致模拟器卡死与无法开机的经典陷阱清单 (Top Pitfalls)

### 陷阱 1：`GPUSTAT` 状态位未及时置位导致的死锁
- **现象**：游戏进入黑屏无声，CPU 占用率 100%，或卡在 PlayStation 菱形开机 Logo。
- **病因**：GPUSTAT 的 Bit 26 (`Ready for command`)、Bit 27 (`VRAM data ready`)、Bit 28 (`Ready for DMA`) 被错误置 0。许多游戏在发送每一个渲染小包前，都执行紧凑汇编循环检查 Bit 26。
- **解决方案**：在非阻塞软光栅器实现中，绘制命令通常同步完成，必须在执行完 GP0 命令后立即重新将 Bit 26 与 Bit 28 置 `1`。

### 陷阱 2：`I_STAT` 中断确认语义错误 (写 1 清零 vs 写 0 清零)
- **现象**：游戏开机播放了一小段音乐后，突然完全停止响应；或者手柄完全失效。
- **病因**：误将 `I_STAT` 实现为现代 x86 常见的写 1 清除 (Write-1-to-Clear) 或常规写覆盖。
- **解决方案**：严格实现写入逻辑：`I_STAT = I_STAT & write_data;`。必须是写入 `0` 的位被清除，写入 `1` 的位完全不受影响！

### 陷阱 3：CD-ROM 中断确认时序混乱与响应 FIFO 溢出
- **现象**：游戏能够显示开机画面，但进入主菜单时卡在 "Now Loading"，CD 磁头马达永无休止寻道。
- **病因**：CD-ROM 的 `INT3` (ACK) 与 `INT2` (Complete) 必须严格先后触发。若模拟器在发送 `INT3` 后，未等 CPU 在中断中确认清除，便立即覆盖写入 `INT2`，导致 CPU 丢失了 ACK 响应包，光驱固件驱动进入死循环挂起。
- **解决方案**：实现一个严格的 2 项 CD-ROM 中断等待队列。当前一阶段中断未被写入 `0x1F801803` 确认时，后续中断严禁覆盖，推迟在周期事件中下一次分发。

### 陷阱 4：开机时缓存隔离写入破坏物理 RAM
- **现象**：使用原版 SONY SCPH1001 BIOS 引导时，开机瞬间主内存大量数据被刷成 `0x00000000`，随后触发非法指令崩溃。
- **病因**：BIOS 开机自检代码会设置 `COP0 SR` 的 `IsC` 位为 1，然后向 `0xA0000000..0xA0000FFF` 连续写入零以刷新 I-Cache。如果模拟器忽视了 `IsC` 标志，直接将这些写操作导向了主 RAM，会导致主内存被直接洗白破坏。
- **解决方案**：在内存写入分发总线中，强制优先判定 `cop0_sr & (1 << 16)`，一旦为真立即短路丢弃写操作或重定向至 I-Cache Tag 阵列。

### 陷阱 5：DMA2 链表环路 (Infinite Linked-List Loops)
- **现象**：渲染某一帧时模拟器彻底冻结，UI 界面失去响应。
- **病因**：某些游戏在清屏或构建场景时，可能会在 RAM 中产生指向自身的链表节点或出现环路，或者将结束标记写为非标准的 `0x00FFFFFF` 变种 (如只看最高位)。
- **解决方案**：在遍历 DMA2 链表时增加单帧最大跳数限制 (如 `max_hops = 131072`)，一旦超过阈值强制跳出并警告，防止陷入无限死循环。

### 陷阱 6：MIPS 非对齐读写指令 (`LWL`, `LWR`, `SWL`, `SWR`) 逻辑错误
- **现象**：3D 模型多边形出现极其怪异的伸展拉扯畸变，或文本字体乱码。
- **病因**：MIPS 架构不支持硬件非对齐直接访存，必须通过 `LWL` (Load Word Left) 与 `LWR` (Load Word Right) 组合读取。由于小端序 (Little-Endian) 的移位方向非常反直觉，极易在合并字时发生移位错误。
- **解决方案**：严格按照标准小端序真值表实现：
  ```c
  // 以 LWL (小端序) 为例:
  case 0: reg = (reg & 0x00FFFFFF) | (mem << 24); break;
  case 1: reg = (reg & 0x0000FFFF) | (mem << 16); break;
  case 2: reg = (reg & 0x000000FF) | (mem << 8);  break;
  case 3: reg = mem; break;
  ```

---

## 7. 面向 3DS 的极限制约优化落地行动路线

针对 Nintendo 3DS 平台的性能短板与硬件特性，制定以下架构执行路线：

```
+---------------------------------------------------------------------------------+
|                                 CORE 0 (ARM11)                                  |
|  +---------------------------------------------------------------------------+  |
|  |             MIPS R3000A CPU Dynamic Recompiler (arm_jit)                  |  |
|  |  - 64KB Software Fastmem Table                                            |  |
|  |  - Direct Block Linking & Return Hash Cache                               |  |
|  |  - Inline Top 5 GTE Instructions (RTPS, RTPT, NCLIP, MVMVA, DPCS)          |  |
|  +---------------------------------------------------------------------------+  |
|                                       |                                         |
|                                       v                                         |
|  +---------------------------------------------------------------------------+  |
|  |                 GPU Command Parser & Geometry Processor                   |  |
|  |  - Triangles/Quads sent to PICA200 via citro3d command buffers            |  |
|  |  - ABR semi-transparency blended using ARMv6 SIMD in 16-bit software path |  |
|  +---------------------------------------------------------------------------+  |
+---------------------------------------+-----------------------------------------+
                                        | Inter-core Lock-free RingBuffer
                                        v
+---------------------------------------------------------------------------------+
|                                 CORE 1 (ARM11)                                  |
|  +---------------------------------------------------------------------------+  |
|  |                     SPU 24-Channel Audio Processing Core                  |  |
|  |  - ADPCM Decoding + Gaussian 512-LUT Pitch Interpolation                  |  |
|  |  - Output Streamed directly to 3DS hardware DSP / ndsp service            |  |
|  +---------------------------------------------------------------------------+  |
|  +---------------------------------------------------------------------------+  |
|  |                     CD-ROM Streaming & Sector Readahead                   |  |
|  |  - Asynchronous FAT32 SD Card Sector Prefetching (Eliminates Frame Stutter)|  |
|  +---------------------------------------------------------------------------+  |
+---------------------------------------------------------------------------------+
```

### 关键阶段性目标：
1. **内存与数据结构精简**：将所有模拟器核心上下文整合入静态分配结构，消除动态内存分配 (`malloc`)，确保在 Old 3DS 64MB 可用内存下常驻占用 $< 20$MB。
2. **纯汇编/紧凑 JIT 核心**：立足 Ari64 动态重编译器架构，剥离一切冗余抽象，直接将 MIPS 寄存器映射到 ARM 物理通用寄存器 (`r4`..`r8`, `r10`..`r11`)。
3. **PICA200 与 软件混合光栅化**：纯不透明 3D 几何图形交由 PICA200 硬件高速绘制；复杂的 ABR 半透明、Mask 判定与 VRAM Blit 操作保留在显存镜像并利用 ARMv6 指令高效就地处理。
4. **音频 DSP 硬件卸载**：通过 `libctru` 的 `ndsp` 接口，将混音完成的 44.1kHz 样本直接提交至 3DS 硬件音频管道，实现零额外 CPU 拷贝的平滑播放。
