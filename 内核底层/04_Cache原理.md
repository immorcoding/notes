# Cache 原理：CPU 为什么需要它，以及它怎样工作

## 1. Cache 解决的是什么问题

CPU 的运算速度远高于主存（RAM）的访问速度。若 CPU 每执行一条指令、每读写一个变量，都必须等待 RAM 返回数据，CPU 的大量时间会处于等待状态。

因此，现代处理器在 CPU 核与 RAM 之间加入 Cache：它容量小、单 bit 成本高，但距离 CPU 很近、访问非常快。

```text
寄存器  →  L1 Cache  →  L2 Cache  →  L3 Cache  →  RAM  →  SSD
最快、最小                                                最慢、最大
```

可以把它类比成查资料：

```text
寄存器 = 手上正拿着的一张纸
Cache  = 书桌上摊开的常用资料
RAM    = 房间里的书架
SSD    = 图书馆仓库
```

Cache 不是让 RAM 变快，而是尽量让 CPU **不必每次都访问 RAM**。

## 2. Cache 为什么能有效：程序局部性

Cache 的设计依赖程序通常具有局部性（locality）。它不是绝对规律，但对绝大多数程序足够成立。

### 2.1 时间局部性（Temporal Locality）

刚被访问过的数据或指令，很可能很快再次被访问。

```c
uint32_t sum = 0;

for (uint32_t i = 0; i < 100000; ++i)
{
    sum += i;
}
```

循环变量 `i`、`sum` 以及循环体的指令会被反复使用，因此极适合留在寄存器和 Cache 中。

### 2.2 空间局部性（Spatial Locality）

访问一个地址后，附近地址也很可能被访问。

```c
for (size_t i = 0; i < 1024; ++i)
{
    sum += array[i];
}
```

数组元素连续存放。CPU 读取 `array[0]` 后，`array[1]`、`array[2]` 等通常马上也会被读取。

因此 Cache 不会只搬运一个字节，而是搬运一整条 **Cache Line（缓存行）**。例如 Cortex-M7 的 I-Cache / D-Cache 的 Cache Line 长度是 32 字节；许多桌面 CPU 的 Cache Line 长度是 64 字节。

```text
CPU 请求读取 0x20001005

真正载入 Cache 的可能是整行：
0x20001000 ~ 0x2000101F      Cortex-M7：32 字节
```

后续访问同一行中的其他数据时，就不需要再次等待 RAM。

## 3. 一次读取在 Cache 中如何发生

CPU 发出的是一个内存地址。Cache 会把地址逻辑上划分为三部分：

```text
地址 = Tag（标签） + Index（组号） + Offset（行内偏移）
```

- **Offset**：定位数据在某条 Cache Line 中的第几个字节。
- **Index**：定位应该去 Cache 中的哪一组（Set）查找。
- **Tag**：确认该 Cache Line 当前保存的是否就是目标内存地址的数据。

假设一个 Cache 的 Line 长度是 64 字节、共有 128 组：

```text
低 6 bit      = Offset，表示 64 字节中的位置
接下来 7 bit  = Index，表示 128 个 Set 中的哪一组
其余高位      = Tag，用来确认这行来自 RAM 的哪个位置
```

读取过程可概括为：

```text
CPU 请求地址 A
        ↓
由 Index 找到目标 Set
        ↓
比较该 Set 中每一行的 Tag 和 Valid 位
        ↓
匹配：Cache Hit，直接按 Offset 取数据
不匹配：Cache Miss，从更低一级 Cache 或 RAM 取回整条 Cache Line
```

### 3.1 Valid 位与 Dirty 位

每条 Cache Line 除了数据外，还有元数据。

- **Valid**：这一行是否含有有效数据。
- **Dirty**：这一行是否被 CPU 修改过、但尚未写回 RAM。
- **Tag**：该行对应的内存地址标记。

所谓 Cache 并不是“另一块独立 RAM”，而是 RAM 中部分数据的高速副本，加上这些状态信息。

## 4. 为什么有“组相联”：避免地址互相挤掉

最简单的 Cache 是直接映射（Direct Mapped）：每个内存地址只能映射到一个固定位置。

```text
RAM 地址 A ──┐
             ├── Cache 第 3 组
RAM 地址 B ──┘
```

即使 A 和 B 都很常用，只要它们映射到同一位置，就会反复互相替换，导致频繁 Miss。这称为 **冲突未命中（Conflict Miss）**。

现实 Cache 通常使用 N 路组相联（N-way Set Associative）：一个 Set 中可放 N 条不同的 Cache Line。

```text
Set 3
├── Way 0：Tag A
├── Way 1：Tag B
├── Way 2：Tag C
└── Way 3：Tag D
```

当一组放满且又需要装入新行时，硬件需要选择一行替换。常见思路是近似 LRU（Least Recently Used，最近最少使用），但具体算法由 CPU 微架构决定。

常见未命中类型：

| 类型 | 含义 |
|---|---|
| 强制未命中（Compulsory Miss） | 第一次访问某行，必然没有缓存 |
| 容量未命中（Capacity Miss） | 工作数据量超过 Cache 容量 |
| 冲突未命中（Conflict Miss） | 多个常用地址映射到同一组，互相替换 |

## 5. 读 Cache 与写 Cache

读数据相对直观；写数据还要考虑：CPU 写完后，什么时候让 RAM 也变成新值？

### 5.1 Write-through（直写）

CPU 写 Cache 时，同时写 RAM：

```text
CPU 写 x
  ├── 更新 Cache
  └── 同时更新 RAM
```

优点是 RAM 始终较新，逻辑简单；缺点是每次写都产生更慢的 RAM 总线访问，性能和带宽压力较大。

### 5.2 Write-back（回写）

CPU 先只修改 Cache Line，并标记为 Dirty；只有该行将被替换、或软件显式要求时，才写回 RAM。

```text
CPU 写 x
  ↓
更新 Cache，Dirty = 1
  ↓
稍后替换该行或执行 Clean
  ↓
写回 RAM
```

这能把多次连续写合并为一次 RAM 写入，通常性能更好。因此 D-Cache 常使用回写策略。

这也解释了一个重要事实：**在 Write-back Cache 中，CPU 刚写过的数据，RAM 中不一定立刻就是新值。**

### 5.3 Write Allocate

若 CPU 要写的地址不在 Cache 中，许多 CPU 会先把该地址所在整行读入 Cache，再进行修改；这叫 Write Allocate。这样若随后还会访问附近数据，可以继续受益于空间局部性。

## 6. Cache 一致性（Coherence）与内存顺序（Ordering）

这两个概念经常被混淆，但不是同一件事。

### 6.1 Cache 一致性：多个核心看到的同一地址是否一致

在 Ryzen、Core 等多核 CPU 中，每个 CPU 核都有自己的 L1/L2 Cache。同一变量可能同时存在于多个核心的私有 Cache 中。

```text
Core 0 的 L1：counter = 10
Core 1 的 L1：counter = 10

Core 0 写 counter = 11
```

若没有一致性机制，Core 1 可能永远继续读到 `10`。现代桌面 CPU 会用硬件 Cache Coherence 协议维护状态。协议名称和细节随厂商及代际变化，常见思想包括 MESI、MESIF、MOESI。

```text
Modified：本核修改过，RAM / 其他核可能还是旧值
Exclusive：仅本核持有，尚未修改
Shared：多个核心都可读
Invalid：该核心的这一行无效，不能继续使用
```

当 Core 0 修改某行后，硬件会让其他核心的相同行失效、更新或转移所有权。于是 Core 1 下次读取该变量时，能获得新数据。

### 6.2 内存顺序：不同变量的读写先后是否符合程序意图

即使硬件保证同一个地址最终一致，也不代表多个地址的观察顺序天然符合你的直觉。

```c
/* 生产者核心 */
data = 42;
ready = 1;

/* 消费者核心 */
if (ready == 1)
{
    use(data);
}
```

若没有同步机制，消费者不应仅凭看到 `ready == 1` 就假定一定能以预期顺序看到 `data == 42`。编译器优化、CPU 乱序执行、Store Buffer 等都可能影响可观察顺序。

正确做法是使用锁、信号量、原子变量的 release/acquire 语义或适当的内存屏障。`volatile` 只限制部分编译器优化，**不是多核同步或 Cache 一致性的通用解决方案**。

## 7. Cortex-M7、锐龙和酷睿的 Cache 有何不同

| 项目 | Cortex-M7（如 STM32H743） | Ryzen / Core 等桌面 CPU |
|---|---|---|
| CPU 核数 | 通常单核 | 通常多核 |
| 常见 Cache 层级 | L1 I-Cache + L1 D-Cache | 每核 L1 I/D、每核 L2、共享/分片 L3 |
| 多核一致性 | 无多核场景 | 硬件维护 CPU 核间一致性 |
| MMU / 虚拟内存 | 无 MMU，使用 MPU | 有 MMU、页表和 TLB |
| 外设 DMA 一致性 | 通常软件维护 | 平台与设备能力不同，驱动仍需处理 DMA 所有权和内存序 |

H743 的 Cortex-M7 有 I-Cache 与 D-Cache，但没有桌面 CPU 那种多核 L3 一致性网络。它更强调确定性、功耗和成本；桌面 CPU 更强调吞吐量和通用计算性能。

此外，PC CPU 中还有 **TLB**，它缓存的是“虚拟地址到物理地址”的页表转换结果；TLB 不是数据 Cache。Cortex-M7 没有虚拟内存 MMU，因此不具备这种典型的 TLB / 页表访问路径。

## 8. DMA 为什么会让 Cortex-M7 的 D-Cache 出问题

DMA 控制器和 CPU 一样，也能作为总线主设备读写 RAM。但普通 STM32 DMA 不会访问 CPU 的 D-Cache。

```text
CPU  ── 经 D-Cache ──→ RAM
DMA ─────────────────→ RAM
```

这会产生两类问题。

### 8.1 CPU 把数据交给 DMA：Clean

```text
CPU 修改 tx_buffer
        ↓
新数据可能只在 D-Cache，RAM 仍是旧数据
        ↓
DMA 直接读 RAM
        ↓
DMA 可能发送旧数据
```

启动 DMA 前，应对发送缓冲区执行 Clean，将 Dirty Cache Line 写回 RAM：

```text
CPU → DMA：Clean D-Cache
```

### 8.2 DMA 把数据交给 CPU：Invalidate

```text
CPU 的 D-Cache 中已有旧的 rx_buffer
        ↓
DMA 直接把新数据写到 RAM
        ↓
CPU 再次读取 rx_buffer
        ↓
CPU 可能仍读到 D-Cache 中的旧数据
```

DMA 接收完成后，应对接收缓冲区执行 Invalidate，丢弃旧 Cache Line，让 CPU 下次从 RAM 重新读取：

```text
DMA → CPU：Invalidate D-Cache
```

对于 DMA 接收缓冲区，较稳妥的流程是：

```text
DMA 开始前：Clean + Invalidate 接收缓冲区
DMA 接收中：CPU 不访问、不修改该缓冲区
DMA 完成后：Invalidate 接收缓冲区
CPU 开始读取新数据
```

开始前先 Clean 的原因是：若 CPU 曾修改过该 Cache Line，直接 Invalidate 会丢掉 CPU 的脏数据；同时可以避免 DMA 传输期间脏行被意外回写覆盖 DMA 的新数据。

### 8.3 Cache Line 对齐的重要性

H743 的 D-Cache 按 32 字节维护，维护操作会影响覆盖到的整行，而不是只影响某一个字节。

```c
#define CACHE_LINE_SIZE 32U

static uint8_t dma_rx_buffer[512] __attribute__((aligned(CACHE_LINE_SIZE)));
```

若 DMA 缓冲区没有 32 字节对齐，清理或失效缓存时可能同时影响相邻变量。实际调用 `SCB_CleanDCache_by_Addr()` 或 `SCB_InvalidateDCache_by_Addr()` 时，地址和长度应扩展到完整 Cache Line 边界。

另一条路线是使用 MPU 将 DMA 缓冲区所在区域配置为 Non-cacheable。它减少了手工维护的复杂度，但 CPU 访问该区域会失去 D-Cache 加速。

## 9. I-Cache 与 D-Cache

- **I-Cache**：缓存 CPU 要执行的指令，典型来源是 Flash 或外部存储器。
- **D-Cache**：缓存普通读写数据，如 SRAM 中的变量、数组、栈、堆。

大多数应用只需重点处理 D-Cache 与 DMA。I-Cache 通常无需日常手动维护。

例外是修改了即将执行的代码，或从外部存储器把新固件/新代码搬到可执行区域后，需要确保旧指令缓存不再被执行。这类场景属于自修改代码、Bootloader 或动态加载等高级场景。

## 10. Cache 的性能直觉与常见误区

### 10.1 连续访问通常比随机访问快

```c
/* 通常更容易利用 Cache Line 和硬件预取 */
for (size_t i = 0; i < n; ++i)
{
    sum += array[i];
}

/* 大步长或随机链表访问，空间局部性较差 */
for (size_t i = 0; i < n; i += 64)
{
    sum += array[i];
}
```

这里的差异在 Ryzen/Core 上尤其明显，因为其 Cache 层级深、乱序执行和预取器都在尝试隐藏内存延迟；在 MCU 上也存在差异，但表现通常更直接。

### 10.2 Cache 并非越大越好

Cache 更大可以减少 Miss，却可能增加访问延迟、功耗和芯片面积。因此 CPU 采用多级 Cache：L1 极快且小，L2/L3 更大但更慢。

### 10.3 不要把“关闭 D-Cache”当成默认解决方案

关闭 D-Cache 能让 DMA 问题暂时消失，但也会损失很多性能，尤其是访问外部 SDRAM、数组、文件缓冲区时。更合理的做法通常是：

- 仅对 DMA 缓冲区做正确的 Clean / Invalidate；或
- 仅将 DMA 缓冲区的 MPU 区域设为 Non-cacheable。

## 11. 一句话总结

> Cache 利用时间局部性和空间局部性，将 RAM 中可能马上再次使用的数据放到 CPU 附近；桌面多核 CPU 用硬件维护 CPU 核之间的一致性，而 Cortex-M7 的 DMA 不参与 CPU Cache，因此需要软件明确执行 Clean、Invalidate 或通过 MPU 配置非缓存区域。
