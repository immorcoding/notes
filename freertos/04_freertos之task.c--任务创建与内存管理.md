# FreeRTOS 之 task.c -- 任务创建与内存管理

每个任务都要两块独立内存：**TCB** 和 **任务栈**。

- 动态：从 FreeRTOS 堆 `ucHeap` 申请
- 静态：你自己准备两块，一般在 `.bss`

链表怎么从节点走回 TCB，见 [调度器链表](05_freertos之task.c--调度器链表.md)；`pxTopOfStack` / `pxStack` 见 [TCB](03_freertos之task.c--TCB.md)；PSP 何时设置、切出时怎么写回，见 [FreeRTOS 运行机制](02_freertos机制.md)。

本文只记一件事：这两块内存从哪来、在 RAM 里长什么样。动态见第 1～4 节，静态见第 5 节。

## 已在其他笔记中记录的内容

- 初始上下文、第一次 `msr psp`：[调度器启动与初始任务](02_freertos机制.md#1-调度器启动与初始任务)
- 切出写回 `pxTopOfStack`、切回恢复 PSP：[运行时上下文切换](02_freertos机制.md#2-运行时上下文切换)
- `.data` / `.bss` / 主栈 / 标准 C heap：[RAM 默认布局](../内核底层/02_程序运行相关.md)

## 1. xTaskCreate -- 动态创建

从堆里申请 TCB 和任务栈，填好后挂就绪链表。栈向下长。**创建时 PSP 不动。**

```c
/**
 * @brief 动态创建任务：堆里申请 TCB 和栈，初始化后挂就绪链表。
 *
 * @param[in]  pxTaskCode     任务函数入口。
 * @param[in]  pcName         任务名，只给调试用。
 * @param[in]  uxStackDepth   栈深度，单位是 StackType_t 个数，不是字节。
 * @param[in]  pvParameters   传给任务的参数，放进初始栈帧的 R0。
 * @param[in]  uxPriority     优先级，越大越高。
 * @param[out] pxCreatedTask  成功后写入 TCB 指针。句柄不走返回值。
 *
 * @return pdPASS 成功；errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY 堆不够。
 */
BaseType_t xTaskCreate( TaskFunction_t pxTaskCode,
                        const char * const pcName,
                        const configSTACK_DEPTH_TYPE uxStackDepth,
                        void * const pvParameters,
                        UBaseType_t uxPriority,
                        TaskHandle_t * const pxCreatedTask )
{
    TCB_t * pxNewTCB;
    BaseType_t xReturn;

    traceENTER_xTaskCreate( pxTaskCode, pcName, uxStackDepth, pvParameters, uxPriority, pxCreatedTask );

    /* 申请内存、填 TCB、伪造初始栈帧。这里还没挂就绪链表。
     * 句柄在 prvInitialiseNewTask() 里写入 *pxCreatedTask。 */
    pxNewTCB = prvCreateTask( pxTaskCode, pcName, uxStackDepth, pvParameters, uxPriority, pxCreatedTask );

    if( pxNewTCB != NULL )
    {
        #if ( ( configNUMBER_OF_CORES > 1 ) && ( configUSE_CORE_AFFINITY == 1 ) )
        {
            /* 多核亲和性。H743 单核不会编这段。 */
            pxNewTCB->uxCoreAffinityMask = configTASK_DEFAULT_CORE_AFFINITY;
        }
        #endif

        /* 挂进对应优先级的就绪链表。调度器已启动且新任务更高，可能立刻切换。 */
        prvAddNewTaskToReadyList( pxNewTCB );
        xReturn = pdPASS;
    }
    else
    {
        /* 堆不够。任务不存在，句柄也不会写成有效值。 */
        xReturn = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }

    traceRETURN_xTaskCreate( xReturn );
    return xReturn;
}
```

## 2. prvCreateTask：两次独立的堆分配

`xTaskCreate()` 几乎不干活。真正申请内存的是 `prvCreateTask()`，对堆做**两次独立 malloc**，不是一次连续分配：

- 栈：`pvPortMallocStack( uxStackDepth * sizeof(StackType_t) )`
- TCB：`pvPortMalloc( sizeof(TCB_t) )`

`configSTACK_ALLOCATION_FROM_SEPARATE_HEAP == 0` 时，`pvPortMallocStack` 就是 `pvPortMalloc`，两块都来自 `ucHeap`。

Cortex-M 栈向下长（`portSTACK_GROWTH < 0`），所以**先申请栈、再申请 TCB**，避免栈往低地址长时压到自己的 TCB。这只决定谁先从堆里出来，两块仍然独立。TCB 用 `pxStack` / `pxTopOfStack` 指回栈。局部变量在任务栈里，不在 TCB 里。

图上常见的「上面 TCB、下面栈」，只是「先栈后 TCB + heap_4 从低往高分配」的结果，**不是一块从 TCB 往下连着长出来的对象**。

两块都拿到之后：

1. 打动态标记（`tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB`）。以后 `vTaskDelete()` 看这个标记，决定能不能 `vPortFree`
2. `prvInitialiseNewTask()` 填 TCB，`pxPortInitialiseStack()` 在栈高地址端伪造初始上下文，见 [调度器启动与初始任务](02_freertos机制.md)
3. 回到 `xTaskCreate()`，`prvAddNewTaskToReadyList()` 挂就绪链表。链表项的 `pvOwner` 指回自己的 TCB，见 [调度器链表](05_freertos之task.c--调度器链表.md)

## 3. 动态创建后堆里的布局

整片 RAM 怎么排，见 [RAM 默认布局](../内核底层/02_程序运行相关.md)。下面只把 FreeRTOS 叠上去。**图上方是高地址**，栈向下长 = 在图上往下走。

以 STM32 动态创建任务 A、B、C 为例：`main` 里建完再 `vTaskStartScheduler()`，调度器已启动，当前跑任务 B。括弧里的块不一定有，任务之间也不一定贴死。

**CubeMX 默认 `configSUPPORT_STATIC_ALLOCATION == 1`。** 这时 Idle 走 `vApplicationGetIdleTaskMemory()`，Timer 走 `vApplicationGetTimerTaskMemory()`，都不在 `ucHeap`。本图里的 Idle / Timer 位置只适用于动态创建它们的情况。

```text
以默认 STM32H743 ld 为例：_estack 在 DTCMRAM 最高处；.bss / ucHeap 常在另一块 RAM（如 AXI SRAM）。
图里叠成一列只是对照，不是同一段连续 RAM。
高地址
│
│  _estack
│  ┌──────────────────────────┐
│  │ 默认主栈（MSP）           │  链接期至少 _Min_Stack_Size，高度不固定
│  │ MSP → 当前主栈顶          │  上电和 main() 用；启动后只服务中断
│  └──────────────────────────┘
│           ↕ 中间空隙：主栈向下、C heap 向上
│  ┌──────────────────────────┐
│  │ 标准 C heap（malloc）     │  从 _end 向上；没人 malloc 时可以是空的
│  └──────────────────────────┘
│
│  ┌──────────────────────────┐
│  │ .bss / .data             │  低地址 .data，接着 .bss
│  │ 全局 / 静态变量           │
│  │ pxCurrentTCB、就绪链表等  │
│  │                          │
│  │  ucHeap（FreeRTOS 堆）    │  heap_4.c 的大数组，本身在 .bss 里
│  │  ┌──────────────────┐    │
│  │  │ 尚未分配的空闲块   │    │
│  │  ├ ─ ─ ─ ─ ─ ─ ─ ─ ┤    │
│  │  │ (TCB_D / 任务 D 栈)│   │  不一定有；启动后才可能再 xTaskCreate
│  │  ├ ─ ─ ─ ─ ─ ─ ─ ─ ┤    │
│  │  │ (队列/缓冲/碎片) │    │  任务之间可以夹别的 pvPortMalloc，或 free 后的空洞
│  │  ├ ─ ─ ─ ─ ─ ─ ─ ─ ┤    │
│  │  │ (TCB_Timer / Timer 栈)│ （没开软件定时器则无；静态时不在 ucHeap）
│  │  ├──────────────────┤    │
│  │  │ TCB_Idle / Idle 栈 │  动态时启动调度器才申请，常比 A/B/C 靠高端
│  │  ├ ─ ─ ─ ─ ─ ─ ─ ─ ┤    │  静态 Idle 不在本图
│  │  │ (队列/缓冲/碎片) │    │
│  │  ├──────────────────┤    │
│  │  │ TCB_C / 任务 C 栈 │
│  │  ├ ─ ─ ─ ─ ─ ─ ─ ─ ┤    │
│  │  │ (队列/缓冲/碎片) │    │
│  │  ├──────────────────┤    │
│  │  │ TCB_B / 任务 B 栈 │
│  │  ├ ─ ─ ─ ─ ─ ─ ─ ─ ┤    │
│  │  │ (队列/缓冲/碎片) │    │
│  │  ├──────────────────┤    │
│  │  │ TCB_A / 任务 A 栈 │  ← 最先创建，常在堆低端
│  │  └──────────────────┘    │
│  └──────────────────────────┘
│
低地址
```

三个方向别混：

1. **整片 RAM**：上高下低。主栈在 `_estack` 往下。`.data` / `.bss` 在低端。`ucHeap` 嵌在 `.bss` 里，不是主栈下面另开的一段。
2. **任何一个栈内部**：压栈时指针变小，图上往下走。
3. **堆的分配顺序**：heap_4 从 `ucHeap` **低往高**分。先申请的靠低端。图里「TCB 在栈上方」只是先栈后 TCB 的结果，不是规定。任务删了之后，堆可能复用任意位置，别再按这张图猜新任务地址。

### 3.1 调度器启动前

`main` 里已经 `xTaskCreate()`，但还没 `vTaskStartScheduler()` 时：

- Thread Mode 仍用 **MSP**。PSP 还没设过，第一次 `msr psp` 在 `vPortSVCHandler`
- 堆里可以已有用户任务的 TCB 和栈。栈里只有伪造的初始上下文，任务还没跑
- **还没有 Idle / Timer**。它们在启动调度器时才创建。动态就这时才 malloc；静态用的是你给的 buffer，本来就不在 `ucHeap`
- 全芯片只有一个 PSP。启动后也只有正在跑的那个任务，PSP 才指到自己的栈；其余任务看各自 TCB 的 `pxTopOfStack`

### 3.2 heap_4：谁先申请、谁靠前

`pvPortMalloc` 从低往高扫，碰到第一块**连续且够大**的就切（first-fit）。块太大就拆开，剩下留在更高地址。`vPortFree` 时，地址挨着的空闲块会合并。

所以：

- **Idle 不一定在堆高端。** 地址只跟谁先申请有关，跟优先级无关。`main` 里任务都建完再启动 → Idle 靠后（高端）。像 mp3 工程那样只先建 Create Task 就启动，Idle / Timer 会夹在 Create Task 和后面的 Log / GUI 之间，比那些应用任务更靠**低地址**。Create Task 自删后，低处还会留个洞。
- **任务之间可以夹别的堆内存。** 中间 `xQueueCreate`、或跑起来再 `pvPortMalloc`，都会插进去。插的是堆上那块；局部变量仍在各自任务栈里。
- **频繁、大小不一、寿命不一的 malloc/free 会碎。** 空闲总量够，但没有一块连续空间够大，就分配失败。heap_4 只能合并相邻空闲块，中间卡着一块还活着的缓冲，两边的小洞合并不了。更稳的办法：启动时一次分完、用内存池、或静态创建。
- `ucHeap` 上限是 `configTOTAL_HEAP_SIZE`，硬顶。标准 C heap 的 `_Min_Heap_Size` 是「至少」，两回事。

单看任务 B 那块栈（仍是上高下低）。初始上下文压了哪些寄存器，见 [运行机制](02_freertos机制.md)；`pxStack` 和当前栈顶的区别，见 [TCB](03_freertos之task.c--TCB.md)。

```text
高地址  pxStack[uxStackDepth-1]（这块的上限）
        ┌────────────────────┐
        │ 初始上下文          │  创建时预写
        ├────────────────────┤
        │ 函数调用栈帧        │  跑起来后往下压
        ├────────────────────┤
        │ 未使用（栈余量）    │
低地址  └────────────────────┘
        pxStack（缓冲区起点，不是当前栈顶）
```

| 区域 | 位置 | 用途 |
|------|------|------|
| 默认主栈 | RAM 最高处，`_estack` 向下 | 上电、`main()`；启动后只给 MSP/中断 |
| 标准 C heap | `_end` 向上，和主栈对长 | `malloc` / `free` |
| `ucHeap` | 一般在 `.bss` | `pvPortMalloc` 的来源，动态 TCB 和栈从这里切 |
| 任务栈 | 堆里独立的一块 | 该任务的 PSP 栈，局部变量在这里 |
| TCB | 堆里另一块，不在任务栈里 | 内核管理；`TaskHandle_t` 就是 TCB 指针 |

## 4. PSP 不负责分配任务栈

容易想成：「PSP 往下挪，腾出的空间就是任务栈」。顺序反了。

1. **创建时**：先从 `ucHeap` malloc 一块。栈顶算在这块的高地址端，用普通指针从高往低写初始上下文，结果存进 `pxTopOfStack`。**PSP 没被碰过。**
2. **第一次跑**：SVC → `vPortSVCHandler` 从 TCB 读 `pxTopOfStack`，恢复寄存器后再 `msr psp`。写入 PSP 的是递增后的 R0，指向硬件异常帧，不是 `pxTopOfStack` 原值。见 [调度器启动](02_freertos机制.md#1-调度器启动与初始任务)。
3. **运行中**：压栈让 PSP 变小，但始终待在创建时那块里。切出时把当前栈顶写回 `pxTopOfStack`，切回再恢复。见 [上下文切换](02_freertos机制.md#2-运行时上下文切换)。

一句话：**分配任务栈 = malloc 一块 RAM；PSP 下移 = 任务已经在这块里压栈。** PSP 只记录「当前任务用到哪」，不是分配器。静态创建只是换一种拿到这块 RAM 的方式，PSP 照旧。

## 5. xTaskCreateStatic -- 静态创建

前提：`configSUPPORT_STATIC_ALLOCATION == 1`（CubeMX 默认开）。还是 TCB + 栈各一块，但**你提前备好，内核不再 malloc**。后面填栈、挂链表，和动态同一套。

和动态的差别：

- 多两个参数：`puxStackBuffer`（栈数组）、`pxTaskBuffer`（TCB 占位）
- **返回值就是句柄**，失败返回 `NULL`
- 失败原因是某个 buffer 为 `NULL`，不是堆不够

```c
/**
 * @brief 静态创建任务：用你给的 TCB 和栈，初始化后挂就绪链表。
 *
 * @param[in]  pxTaskCode      任务函数入口。
 * @param[in]  pcName          任务名，只给调试用。
 * @param[in]  uxStackDepth    栈深度，StackType_t 个数。数组长度必须 >= 这个值。
 * @param[in]  pvParameters    传给任务的参数，放进初始栈帧的 R0。
 * @param[in]  uxPriority      优先级，越大越高。
 * @param[in]  puxStackBuffer  你提供的栈数组。
 * @param[in]  pxTaskBuffer    你提供的 StaticTask_t，内核当 TCB 用。
 *
 * @return 成功返回句柄；任一 buffer 为 NULL 则返回 NULL。
 */
TaskHandle_t xTaskCreateStatic( TaskFunction_t pxTaskCode,
                                const char * const pcName,
                                const configSTACK_DEPTH_TYPE uxStackDepth,
                                void * const pvParameters,
                                UBaseType_t uxPriority,
                                StackType_t * const puxStackBuffer,
                                StaticTask_t * const pxTaskBuffer )
{
    TaskHandle_t xReturn = NULL;
    TCB_t * pxNewTCB;

    traceENTER_xTaskCreateStatic( pxTaskCode, pcName, uxStackDepth, pvParameters, uxPriority, puxStackBuffer, pxTaskBuffer );

    /* 不申请堆。成功时句柄写进 xReturn。 */
    pxNewTCB = prvCreateStaticTask( pxTaskCode, pcName, uxStackDepth, pvParameters,
                                    uxPriority, puxStackBuffer, pxTaskBuffer, &xReturn );

    if( pxNewTCB != NULL )
    {
        #if ( ( configNUMBER_OF_CORES > 1 ) && ( configUSE_CORE_AFFINITY == 1 ) )
        {
            pxNewTCB->uxCoreAffinityMask = configTASK_DEFAULT_CORE_AFFINITY;
        }
        #endif

        prvAddNewTaskToReadyList( pxNewTCB );
    }

    traceRETURN_xTaskCreateStatic( xReturn );
    return xReturn;
}
```

调用时两个 buffer 用 `static` / 全局，编译期就占着，一般在 `.bss`：

```c
#define STACK_SIZE 256

static StackType_t  xStack[ STACK_SIZE ];
static StaticTask_t xTaskBuffer;

TaskHandle_t xHandle = xTaskCreateStatic( vTaskCode, "NAME", STACK_SIZE, NULL,
                                          tskIDLE_PRIORITY, xStack, &xTaskBuffer );
```

**别做成会返回的局部变量。** 函数一返回，内存没了，任务还指着它。

## 6. prvCreateStaticTask：不申请堆

它对堆**一次 malloc 都不做**：

```c
static TCB_t * prvCreateStaticTask( ... ,
                                    StackType_t * const puxStackBuffer,
                                    StaticTask_t * const pxTaskBuffer,
                                    TaskHandle_t * const pxCreatedTask )
{
    TCB_t * pxNewTCB;

    configASSERT( puxStackBuffer != NULL );
    configASSERT( pxTaskBuffer != NULL );
    configASSERT( sizeof( StaticTask_t ) == sizeof( TCB_t ) );  /* 空壳必须和真 TCB 一样大 */

    if( ( pxTaskBuffer != NULL ) && ( puxStackBuffer != NULL ) )
    {
        pxNewTCB = ( TCB_t * ) pxTaskBuffer;   /* 你的 StaticTask_t 直接当 TCB */
        memset( pxNewTCB, 0, sizeof( TCB_t ) );
        pxNewTCB->pxStack = puxStackBuffer;

        /* 打静态标记。vTaskDelete() 看到后，不会 free 你的两块内存。 */
        pxNewTCB->ucStaticallyAllocated = tskSTATICALLY_ALLOCATED_STACK_AND_TCB;

        /* 后面和动态同一套：填 TCB，在栈高地址端伪造初始上下文。 */
        prvInitialiseNewTask( pxTaskCode, pcName, uxStackDepth, pvParameters,
                              uxPriority, pxCreatedTask, pxNewTCB, NULL );
    }
    else
    {
        pxNewTCB = NULL;
    }

    return pxNewTCB;
}
```

记三点：

- **`StaticTask_t` 不是第二种 TCB。** 内核不把真 `TCB_t` 暴露给你，又要你能占一块够大的内存，所以给了个成员全是 dummy 的同尺寸结构。内核 `(TCB_t *)` 转过去就用，别自己抠 dummy。真字段见 [TCB](03_freertos之task.c--TCB.md)。
- **没有「先栈后 TCB」这回事。** 那是动态申请顺序的问题。静态两块在哪，看你变量写在哪，不必相邻。
- **删任务不会把 `.bss` 还回去。** `ucStaticallyAllocated` 就是防误 `vPortFree` 的。要拿同一块 buffer 再建任务，得先保证旧任务已经删干净。MPU 还有「栈静态、TCB 动态」这种组合，H743 普通任务用不到。

初始化仍是 `prvInitialiseNewTask()` → 挂就绪链表，和动态同一条路。

## 7. 静态创建后 RAM 里的布局

静态任务的 TCB 和栈**不进 `ucHeap`**。`static` / 全局就是 `.bss`（带初值则在 `.data`）里的普通变量，链接期地址就定了。

Idle / Timer 在开了静态分配时也一样：应用交出 buffer，内核再调 `xTaskCreateStatic()`。第 3 节堆图里的 Idle / Timer 位置不适用。

```text
高地址
│
│  _estack
│  ┌──────────────────────────┐
│  │ 默认主栈（MSP）           │
│  └──────────────────────────┘
│           ↕ 主栈向下、C heap 向上
│  ┌──────────────────────────┐
│  │ 标准 C heap（malloc）     │
│  └──────────────────────────┘
│
│  ┌──────────────────────────┐
│  │ .bss / .data             │
│  │ 全局 / 静态变量           │
│  │                          │
│  │  xTaskBuffer_A / xStack_A │  ← 你提供的，链接期就占着；不必和 B 相邻
│  │  xTaskBuffer_B / xStack_B │
│  │  Idle / Timer 的静态 TCB、栈│
│  │                          │
│  │  ucHeap                  │  静态任务不从这里切
│  │  ┌──────────────────┐    │
│  │  │ 空闲 / 动态对象   │    │  还有 xTaskCreate / xQueueCreate 才用到
│  │  └──────────────────┘    │  纯静态时可以一直空着
│  └──────────────────────────┘
│
低地址
```

和动态图对照：

1. **栈内部照旧**：高地址端写初始上下文，`pxStack` 指低端。单看一块 `xStack[]`，和第 3 节那张局部图一样。
2. **「TCB 在栈上方」不再成立。** 那是动态分配顺序的结果。静态谁地址高，看链接器怎么排。
3. **`ucHeap` 可以完全不参与任务创建。** 删任务只是从调度器摘掉，`.bss` 里的数组还在。

调度器启动前的状态（仍用 MSP、PSP 未设、栈里只有初始上下文）和第 3.1 节一样。差别只是：这时 `ucHeap` 里可以没有用户任务。

| 区域 | 位置 | 用途 |
|------|------|------|
| 主栈 / 标准 C heap | 同第 3 节 | 和创建方式无关 |
| `xTaskBuffer` | 通常 `.bss` | 内核当 TCB；句柄就是这块的指针 |
| `xStack[]` | 通常 `.bss`，和 TCB 独立 | 该任务的 PSP 栈 |
| Idle / Timer 静态 buffer | 应用提供，不在 `ucHeap` | 开了静态分配就有 |
| `ucHeap` | 一般在 `.bss` | 只给动态 `pvPortMalloc` 用 |
