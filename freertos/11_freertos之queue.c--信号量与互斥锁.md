# FreeRTOS 之 queue.c -- 信号量与互斥锁

本次梳理同时关注 API 用法和内核实现。源码依据本仓库 FreeRTOS V11.3.0；阻塞、唤醒和中断切换的说明以常规单核 Cortex-M 场景为主。

相关笔记：[TCB](03_freertos之task.c--TCB.md)、[调度器链表](05_freertos之task.c--调度器链表.md)、[Delay](06_freertos之task.c--Delay.md)、[调度器挂起与临界区](07_freertos之task.c--调度器挂起与临界区.md)。

## 1. 信号量复用队列机制，但不保存实际数据

二值信号量可以理解为**长度为 1、数据项大小为 0 的特殊队列**。计数信号量的长度则对应最大计数，可以大于 1。

`semphr.h` 中，二值信号量的创建宏是：

```c
#define semSEMAPHORE_QUEUE_ITEM_LENGTH ((UBaseType_t)0U)

#define xSemaphoreCreateBinary() \
    xQueueGenericCreate((UBaseType_t)1, \
                        semSEMAPHORE_QUEUE_ITEM_LENGTH, \
                        queueQUEUE_TYPE_BINARY_SEMAPHORE)
```

信号量复用 `Queue_t` 中的计数和事件等待链表，不传递队列数据项：

| 字段 | 用于信号量时的含义 |
|---|---|
| `uxLength` | 最大计数，二值信号量为 1 |
| `uxItemSize` | 0，不保存数据项 |
| `uxMessagesWaiting` | 当前可用计数 |
| `xTasksWaitingToReceive` | 等待 Take 的任务链表 |

成功 Give 增加计数，成功 Take 减少计数。信号量中没有一份可以被新数据覆盖的串口数据。

## 2. 二值信号量：通知可能合并，数据需要另存

`xSemaphoreCreateBinary()` 创建后计数为 0。假设任务没有运行，中断连续 Give 三次，随后任务执行三次不等待的 Take：

| 操作 | 计数变化 | 结果 |
|---|---|---|
| 第一次 Give | 0 → 1 | 成功 |
| 第二次 Give | 1 → 1 | 已满，失败 |
| 第三次 Give | 1 → 1 | 已满，失败 |
| 第一次 Take | 1 → 0 | 成功 |
| 后两次 Take，等待时间为 0 | 0 → 0 | 无可用信号，立即失败 |

**后两次 Give 不会覆盖第一次留下的信号。**Take 检查的是当前计数，不记录历史上是否 Give 过。

### 串口的常见流程：先保存原始数据，再处理

```text
串口中断：将收到的原始数据存入 FIFO 接收缓冲区 → GiveFromISR
处理任务：Take 成功 → 从 FIFO 读取数据 → 解析、滤波等处理
```

假设缓冲区完整保存了 A、B、C 三份数据，信号量计数可以只有 1。一次 Take 成功后，处理任务通常应持续读取，直到当前没有数据可读，再回去等待信号。

```text
任务循环：
    Take，等待通知
    如果成功：
        循环读取并处理接收缓冲区中的数据
        直到当前缓冲区为空
```

这是常见的单个处理任务模式。缓冲区仍需要保证中断写入、任务读取的并发安全，并处理容量不足的情况。

如果每次 Take 只处理一份，处理 A 后计数已经为 0，B、C 可能仍留在缓冲区。没有新的 Give 时，任务会再次等待，造成数据积压；积压到缓冲区满后才可能进一步丢数据。

若缓冲区按 FIFO 正确读写，后来收到 D、E、F，任务仍先读到 B，是正常的接收顺序，本身不代表指针错位。

> **信号量只管理通知或许可的计数，不管理实际数据及其对应关系。**多次 Give 可能只留下一个待处理信号，但原始数据仍可完整保存在 FIFO 中。Take 成功不等于取得了某次 Give 对应的数据。

若使用覆盖、筛选、降采样或重排等特殊处理，应用需自行保证通知时机和读取规则一致。只保留最新值也可以是有意的设计，但前提是业务允许忽略旧值。

## 3. 计数信号量：初始值取决于计数表示什么

计数信号量的计数范围为 0 到最大计数 N。成功 Take 减 1，成功 Give 加 1；达到最大计数后继续 Give 会失败。

| 用途 | 计数含义 | 初始化时的典型状态 |
|---|---|---|
| 记录待处理事件 | 已发生但尚未处理的事件数 | 尚无事件，初始为 0 |
| 管理空闲资源 | 当前可申请的资源数 | 已有 N 份空闲资源，初始为 N |

例如已有三块空闲缓冲区：

```c
SemaphoreHandle_t available = xSemaphoreCreateCounting(3, 3);
/* 最大计数 3，初始计数 3。创建后还需检查 available 是否为 NULL。 */
```

```text
Take 成功 → 从空闲缓冲区表取出一块 → 使用
         → 放回空闲缓冲区表 → Give，归还一个资源
```

**信号量只记录数量，具体哪块缓冲区可用，需要另外的资源表管理。**数量管理也不自动保证资源表的并发读写安全。

若三块缓冲区都空闲，却错误地将初始计数设为 0，而所有任务都要先 Take、使用后才 Give，就可能全部卡在申请步骤。若没有其他 Give 且允许无限等待，后续使用和归还都不会执行。

计数信号量创建时可以自带可用计数，不要求必须先调用 Give 才能 Take。用于事件计数时，也要考虑最大计数不足造成的通知丢失，不能认为它可以无限累积事件。

## 4. Take、阻塞、唤醒与超时

### 4.1 API 到内核的路径

```text
xSemaphoreTake()        → xQueueSemaphoreTake()
xSemaphoreGive()        → xQueueGenericSend()，Give 的阻塞时间为 0
xSemaphoreGiveFromISR() → xQueueGiveFromISR()
```

`xQueueSemaphoreTake()` 检查 `uxMessagesWaiting`：大于 0 时减 1 并返回成功；为 0 且不允许等待时返回失败；为 0 且允许等待时，处理超时信息并进入阻塞流程。

阻塞等待不会持续占着 CPU 检查计数。调度器选择最高优先级的就绪任务运行；同优先级任务是否时间片轮转取决于配置。

### 4.2 唤醒不等于 Take 成功

```text
Give：增加计数，在有等待任务时解除一个任务的阻塞
Take：任务实际运行后检查计数，成功时减少计数
```

假设 A 正在等待，计数为 0：Give 后计数为 1，A 进入就绪态，但信号没有预留给 A。若其他任务 C 先成功 Take，计数回到 0，A 恢复执行后就不能直接返回成功。

如果 A 原来的等待时间尚未耗尽，它可以再次阻塞，使用剩余等待时间，而不是重新获得完整超时时间。

> “有菜了”的通知不等于把菜交到了等待者手中；别人先端走，等待者仍需继续等。

一次 Give 不是广播：它最多直接解除一个等待任务的阻塞。等待链表按任务优先级排序；同优先级通常按进入等待的顺序排列。

### 4.3 两个 TCB 链表项

假设计数为 0，A 在 tick 20 调用 Take，最多等待 80 tick，且没有 tick 回绕：

| TCB 节点 | 挂入哪里 | 作用 |
|---|---|---|
| `xStateListItem`，状态链表项 | 延时链表，到期时刻 tick 100 | 管理等待期限 |
| `xEventListItem`，事件链表项 | 信号量的 `xTasksWaitingToReceive` | 登记等待信号，按优先级排序 |

`vTaskPlaceOnEventList()` 将事件项插入对象等待链表，并通过延时机制安排状态项。节点的 `pvOwner` 指向所属 TCB，内核可据此找到任务。

**tick 50 时 Give，选中 A，且调度器没有挂起：**

```text
xEventListItem：从信号量等待链表摘下
xStateListItem：从延时链表摘下 → 加入 pxReadyTasksLists[A 的优先级]
```

**一直没有 Give，tick 100 到期：**内核从延时链表发现 A，摘下状态项，也摘下信号量等待链表中的事件项，再让 A 就绪。超时不增加计数。A 恢复运行后，如果确认已超时且计数仍为 0，Take 才返回失败。

```text
等什么：用事件项登记
什么时候到期：用状态项登记
恢复就绪：用状态项入队
```

调度器挂起时，事件唤醒可能先将 `xEventListItem` 放入 `xPendingReadyList`，恢复调度器后再完成状态项的迁移。详见已有链表笔记。

### 4.4 portMAX_DELAY 的配置条件

`portMAX_DELAY` 不在所有配置下都等于无限等待。允许无限阻塞且 `INCLUDE_vTaskSuspend == 1` 时，事件等待可把状态项放到 `xSuspendedTaskList`，对外仍属于阻塞等待。

本仓库 `FreeRTOSConfig.h` 没有定义 `INCLUDE_vTaskSuspend`，头文件默认值为 0，因此不能直接把本项目中的 `portMAX_DELAY` 当成无限等待。前面的无限等待例子是对行为的假设，不代表当前配置。

## 5. 中断中的 Give 与任务切换

中断通知部分的示意代码：

```c
BaseType_t xHigherPriorityTaskWoken = pdFALSE;

/* 先将收到的数据保存到接收缓冲区。 */

BaseType_t result =
    xSemaphoreGiveFromISR(rxSemaphore, &xHigherPriorityTaskWoken);

portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
```

| 结果 | 含义 |
|---|---|
| `result` | Give 是否成功；成功为 `pdTRUE`，已满为 `errQUEUE_FULL` |
| `xHigherPriorityTaskWoken` | 相关 ISR API 是否唤醒了比被中断任务优先级更高的任务 |

Give 成功但没有等待者时，刚初始化为 `pdFALSE` 的唤醒标志仍为 `pdFALSE`。它不是系统出现更高优先级任务的全局标志。

同一 ISR 内，多次 API 调用可以共用一个标志：先初始化为 `pdFALSE`，相关调用可将它置为 `pdTRUE`，最后统一请求切换。不要在每次调用之间清掉此前已经产生的切换需求。

### 嵌套中断中的局部变量相互独立

```text
任务 T，优先级 2，被中断 A 打断：flagA = FALSE
    A 又被中断 B 打断：flagB = FALSE
    B 唤醒任务 H，优先级 5：flagB = TRUE，并请求切换
    B 返回，A 的 Give 没有唤醒任务：flagA 仍为 FALSE
```

常规 Cortex-M 嵌套场景中，两次优先级比较的基准可以都是 T，但 A、B 的局部变量是两份不同的存储。即使变量名字相同，也不会自动同步。

若刻意共享同一个变量地址，B 写入后 A 当然可以读到变化，那是共享变量的结果。

在常规 Cortex-M port 中，`portYIELD_FROM_ISR()` 请求 PendSV；B 发出的请求不要求 A 的局部标志也变为 TRUE。任务切换需要等异常执行条件允许。

中断中应使用相应的 `FromISR` API，不能阻塞等待。Cortex-M 中断优先级还必须满足 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 的限制；中断优先级数值方向与 FreeRTOS 任务优先级不同，不能混用。互斥锁不能用于 ISR。

## 6. 互斥锁：持有者归还，支持优先级继承

| 比较项 | 二值信号量 | 普通互斥锁 |
|---|---|---|
| 常见用途 | 任务间、任务与中断间通知 | 共享资源互斥访问 |
| 创建后的可用状态 | `xSemaphoreCreateBinary()` 创建后为空 | 创建后可取得 |
| Take 与 Give 的执行者 | 可以不同；通知场景中消费者通常不归还信号 | 成功取得锁的任务负责归还 |
| 优先级继承 | 没有 | 有，采用基本、简化的实现 |
| ISR 使用 | 可用相应 FromISR API | 不允许 |

互斥锁还记录持有者，用于优先级继承等处理。`queue.c` 的 `prvInitialiseMutex()` 初始化持有者，并让锁处于可取得状态。

### 优先级继承提高的是持锁者，不是等待者

假设 L 优先级 1，M 优先级 3，H 优先级 5，且 L 只持有这一把锁：

```text
L 已经持锁
H 就绪并抢占 L，尝试 Take 同一把锁
    → H 阻塞
    → L 继承 H 的优先级，临时提高到 5
    → L 可继续执行，不再被 M 抢占
    → L 释放锁，H 有机会取得锁
```

`xTaskPriorityInherit()` 修改持锁任务的当前优先级；如果它在就绪链表中，还要迁移到新的优先级链表。任务的基础优先级与当前优先级是不同概念。

继承减少了其他中等优先级任务对持锁者的干扰，**不能消除高优先级任务等待锁释放的时间，也不能加速外设的物理传输速度**。持锁任务若还在等待其他事件，仅提高优先级也不能使它立即运行。

FreeRTOS 的继承是简化实现。持有多把互斥锁时，正常释放路径可能直到全部释放才恢复基础优先级；不能套用“释放其中一把就一定恢复”的简单模型，更不能直接套用完整天花板协议的时序保证。

锁不禁止任务抢占或中断；它阻止其他遵守同一把锁的任务进入资源操作。整帧不能交叉说明需要互斥，不能据此推出实时性要求低。

本仓库 `configMAX_PRIORITIES == 5`，有效任务优先级为 0～4。上述优先级 5 的例子是假设存在足够优先级等级的教学场景。

## 7. 经典例子：转账与查询使用同一把锁

用内存中的两个账户说明多步更新：初始 A、B 各有 1000 元，总金额 2000 元。任务 1 从 A 转给 B 100 元：

```c
A.balance -= 100;
B.balance += 100;
```

若在第一步之后被任务 2 抢占，任务 2 读取到 A 为 900、B 为 1000，总额得到 1900。它读到了尚未完成的修改。

```text
任务 1 转账：
    Take 同一把账户锁
    A 扣除 100
    B 增加 100
    Give

任务 2 查询：
    Take 同一把账户锁
    读取 A 和 B，计算总金额
    Give
```

查询只能看到转账前或转账后的完整状态。只给写入者加锁、读取者不遵守同一把锁，仍然可能读到中间状态。

这里假设多个模块直接同步读取或修改已有的共享内存，操作短小且不等待外设。直接加锁可以保留函数调用方式；队列加专用任务也能正确实现，但要让专用任务独占账户，并把查询、转账改成请求和回复。

> **转账要先扣款、再入账，别人不能在这两步之间查询余额。用同一把互斥锁保护转账和查询，就能让查询看到完整结果。对于这种短小、同步的共享内存操作，直接加锁通常比队列加专用任务更简单。**

这个例子说明实现复杂度的选择，不代表锁在任何场景下都比队列更好。

## 8. 外设例子：锁与队列加专用任务都可以合理

假设 I²C 总线上有 TCA9548A 多路复用器，通道 0 和通道 1 各接一颗地址为 0x48 的传感器。读取需要先选择通道，再访问传感器。

```text
A 选择通道 0 → 被 B 抢占
B 选择通道 1，完成读取
A 恢复，读取地址 0x48 → 实际读到通道 1 的传感器
```

各次通信都可能成功，但数据来源错了。因此，只保护单次底层传输还不够，应该保护“选通道 → 完成读取”的整个操作。硬件通道选择机制见 [TI 数据手册](https://www.ti.com/lit/ds/symlink/tca9548a.pdf)。

两种设计都可以保持操作完整：

- 多个任务直接调用同步驱动：共同取得总线互斥锁，完成整个操作后释放。
- 队列加专用 I²C 任务：调用者提交请求，只有该任务选择通道并读取，每次完整执行一个请求。

第二种是 Gatekeeper Task：资源由一个任务独占。队列本身只保存请求，只有在其他调用者不再直接操作总线时，才实现了集中所有权。

队列方案便于统一处理请求、重试和结果，但不自动消除等待。普通 FIFO 中，高优先级请求可能排在多个低优先级请求后面；仍需设计请求顺序、专用任务优先级和最坏等待时间。

## 9. 权威资料中的实时性设计观点

以下记录来自官方资料和经典研究，不把讨论中的具体优先级分界线作为通用规则。

1. **FreeRTOS 官方：优先级继承是补救机制。**它只减轻部分优先级反转影响；硬实时设计应尽量避免反转。不能用“开启继承”代替资源依赖和等待时间的设计。[互斥锁官方文档](https://freertos.org/Real-time-embedded-RTOS-mutexes.html)
2. **经典实时调度研究：共享资源可以使用锁，但要约束并分析最坏阻塞。**Sha、Rajkumar、Lehoczky 的 1990 年论文在其单处理器模型及协议条件下，证明优先级天花板协议可将最坏阻塞限制为至多一个低优先级临界区的执行时间，并避免死锁。FreeRTOS 普通互斥锁不是这一完整协议。[论文](https://www.math.unipd.it/~tullio/RTS/2009/SRL-1990.pdf)
3. **FreeRTOS 官方教材：条件允许时，从架构上减少资源共享。**尽可能让资源只由一个任务访问；采用专用任务时，同样要分析请求排队和结果等待。[官方教材第 8 章](https://github.com/FreeRTOS/FreeRTOS-Kernel-Book/blob/main/ch08.md)

由这些观点得到的工程判断是：关键实时路径应尽量避免阻塞式互斥和对低优先级任务的同步依赖；确需共享时，要分析最坏阻塞时间，确认仍能满足截止时间。

**高优先级保证任务就绪时优先获得 CPU，不保证等待的资源立即可用。**即使自己没有 Take，只要等待一个低优先级持锁任务的结果，锁造成的延迟仍可能传递过来。

例如，低优先级任务的一帧还要 5 ms 才能发完，高优先级请求要求 1 ms 内开始发送，而两者共用同一串口且不允许打断整帧，这组约束就可能无法同时满足。提高任务优先级不能突破波特率限制。

## 10. 源码阅读入口与参考资料

- [`semphr.h`](sourcecode/FreeRTOS/Source/include/semphr.h)：创建、Take、Give、FromISR 宏，以及 API 使用约定。
- [`queue.c`](sourcecode/FreeRTOS/Source/queue.c)：`xQueueSemaphoreTake()`、`xQueueGiveFromISR()`、`prvInitialiseMutex()`。
- [`tasks.c`](sourcecode/FreeRTOS/Source/tasks.c)：`vTaskPlaceOnEventList()`、`xTaskRemoveFromEventList()`、超时唤醒和优先级继承。
- [二值信号量官方说明](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/02-Queues-mutexes-and-semaphores/02-Binary-semaphores)：通知与任务同步。
- [计数信号量官方说明](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/02-Queues-mutexes-and-semaphores/03-Counting-semaphores)：事件计数与资源数量管理。

后续可以继续梳理递归互斥锁，以及队列锁定、临界区和重新检查计数如何避免等待过程中的竞态。
