# FreeRTOS：任务、TCB、任务栈与就绪队列

本文梳理 FreeRTOS 中任务函数、任务控制块（TCB）、任务栈、就绪队列与任务切换之间的关系。示例以 Cortex-M7（如 STM32H743）端口为背景。

## 1. 任务函数与代码存储位置

FreeRTOS 的任务函数本质上仍是普通 C 函数。创建任务时，系统**不会**将任务函数的机器指令复制到任务栈中。

函数机器指令的运行位置由编译属性与链接脚本决定：

- 普通函数通常位于 Flash 的 `.text` 段；
- 放入 `.ramfunc` 等特殊段的函数，可以在启动时由 Flash 复制到 RAM 或 ITCM RAM 中执行；
- 函数代码是否在 RAM 中执行，与它使用哪个任务栈没有直接关系。

例如：

- `Calculate_Task` 的代码位于 Flash；
- `Fast_Calculate` 的代码位于 ITCM RAM；
- 两个函数运行时仍使用 `Calculate` 任务自己的任务栈。

```text
Calculate_Task 在 Flash 中执行
        ↓
调用 Fast_Calculate
        ↓
CPU 跳转到 ITCM RAM 执行
        ↓
函数返回后，继续执行 Flash 中的 Calculate_Task
```

## 2. 任务栈

任务栈在任务创建时便已分配，而不是等任务进入就绪状态后才创建。它用于保存函数运行所需的数据和现场，包括：

- 函数局部变量；
- 部分函数参数；
- 函数返回地址；
- 需要保护的 CPU 寄存器；
- 多层函数调用产生的栈帧；
- 中断现场；
- 任务切换时的 CPU 上下文。

任务栈**不保存函数机器指令**。

STM32 上还有一块**默认主栈**（从 `_estack` 往下长；链接期至少留 `_Min_Stack_Size`，运行时高度不固定）。上电和 `main()` 阶段全程使用 MSP 指向这块主栈；调度器启动后，任务改用各自的任务栈（PSP），默认主栈只服务中断和异常。主栈、标准 C heap、`.data` / `.bss` 在 RAM 里的关系见 [RAM 默认布局与标准 C heap](../内核底层/02_程序运行相关.md)；任务栈从堆里怎么切出来，见 [动态创建任务的内存管理](04_freertos之task.c--任务创建与内存管理.md)。

在 STM32H743 上，`StackType_t` 通常为 32 位，也就是 4 字节。例如：

```c
xTaskCreate(Task, "Task", 512, NULL, 2, NULL);
```

其中的 `512` 通常表示 512 个 `StackType_t`：

```text
512 × 4 字节 = 2,048 字节任务栈
```

## 3. 函数栈帧

每调用一层函数，编译器可能会在当前任务栈上为该函数建立一段栈空间，这段空间称为**栈帧**（Stack Frame）。一个函数的栈帧可能包含：

- 局部变量；
- 未放入寄存器的参数；
- 返回地址；
- 需要保存的寄存器；
- 寄存器不足时临时保存的数据；
- 栈对齐产生的填充。

例如，调用关系如下：

```text
Task_Function
    ↓
Function_A
    ↓
Function_B
    ↓
Function_C
```

当 `Function_C` 正在运行时，前面尚未返回的栈帧可能同时存在：

```text
Function_C 栈帧
Function_B 栈帧
Function_A 栈帧
Task_Function 栈帧
```

返回顺序则相反：

```text
Function_C
    ↓
Function_B
    ↓
Function_A
    ↓
Task_Function
```

这体现了栈的后进先出（LIFO）特性。编译器也可能将简单的局部变量直接保存在寄存器中，因此并非所有局部变量都会占用任务栈。

## 4. 任务栈大小

任务栈不需要保存音频文件、图片或大块数据，因此通常不会像数据缓冲区一样大。但它必须能容纳最坏情况下的总用量：

- 最深函数调用链的所有栈帧；
- 最大局部变量用量；
- 中断现场；
- 任务切换现场；
- 库函数使用的临时空间。

以下操作可能大量消耗任务栈：

- 函数局部大数组；
- 大型局部结构体；
- `printf`、`sprintf` 等格式化函数；
- 文件系统格式化函数；
- MP3 解码等深层函数调用；
- 递归调用。

音频、文件等大缓冲区通常应使用静态存储区、FreeRTOS 堆或专用 RAM；不应直接定义为任务函数内的局部大数组。

可以通过以下 API 检查任务运行以来的最小剩余栈空间：

```c
uxTaskGetStackHighWaterMark(task_handle);
```

再根据实际结果调整任务栈大小。

## 5. 任务状态管理

FreeRTOS 通过 TCB 中的 `xStateListItem`，将任务放入不同的状态链表：

- **就绪链表**：任务当前可以运行；
- **延时链表**：任务等待指定 Tick 到达；
- **挂起链表**：任务被主动挂起；

等待队列、信号量或互斥锁时，任务还会把 `xEventListItem` 挂入相应对象的事件等待链表。**事件等待链表不是与就绪、延时、挂起并列的互斥任务状态；同一个阻塞任务通常同时占用状态链表和事件等待链表。**任务通知的状态和值直接保存在 TCB 中，不依赖外部对象的事件等待链表。详细关系见 [调度器链表](05_freertos之task.c--调度器链表.md)。

任务进入就绪状态并不代表会立即执行，只代表它已经有资格被调度器选择。

## 6. FreeRTOS 就绪队列

FreeRTOS 不会将所有任务放进同一个按优先级排序的链表。它为**每个优先级**维护一个独立的就绪链表：

```text
pxReadyTasksLists[0]：优先级 0
pxReadyTasksLists[1]：优先级 1
pxReadyTasksLists[2]：优先级 2
…
```

任务进入就绪状态时，直接用任务优先级作为数组下标：

```text
优先级 2 的任务
      ↓
插入 pxReadyTasksLists[2]
```

因此不需要遍历全部任务来寻找插入位置。

## 7. 最高优先级的选择

开启以下配置后：

```c
configUSE_PORT_OPTIMISED_TASK_SELECTION = 1
```

Cortex-M7 端口通常会使用位图记录哪些优先级存在就绪任务：

```text
bit0 = 1：优先级 0 有就绪任务
bit1 = 1：优先级 1 有就绪任务
bit2 = 1：优先级 2 有就绪任务
bit3 = 1：优先级 3 有就绪任务
```

调度器通过处理器指令快速找到最高置位，从而确定最高的就绪优先级。过程如下：

1. 通过位图找到最高就绪优先级；
2. 进入对应优先级的就绪链表；
3. 从链表中选择下一个任务。

**在启用抢占式调度且 `configUSE_TIME_SLICING == 1` 时**，同一优先级有多个任务会按时间片轮转。

## 8. 任务状态变化

调用以下 API：

```c
vTaskDelay(...);
```

任务会从就绪链表移入延时链表。

调用：

```c
xQueueReceive(..., portMAX_DELAY);
```

若队列为空，任务会进入事件等待状态：`xEventListItem` 挂入该队列的等待链表；**当 `INCLUDE_vTaskSuspend == 1` 时，`portMAX_DELAY` 对应的 `xStateListItem` 位于挂起链表，对外仍属于阻塞等待。**

中断调用：

```c
vTaskNotifyGiveFromISR(...);
```

可以让等待通知的任务重新进入就绪链表；其他任务调用 `xTaskNotifyGive()` 也能达到相同目的。

SysTick 通常会：

1. 增加系统 Tick；
2. 检查延时任务是否到期；
3. 将到期任务移回就绪链表；
4. 判断是否需要触发任务切换。

## 9. 两种优先级链表方案

### 9.1 优先级有序链表

所有任务放在同一个链表中，并按优先级排序。

- 插入时需要查找位置，通常为 `O(n)`；
- 取得最高优先级任务通常为 `O(1)`。

常见名称：

- 优先级有序链表；
- 链表优先队列；
- Priority-ordered linked list；
- Linked-list priority queue。

### 9.2 FreeRTOS 的分优先级就绪链表

每个优先级拥有独立链表，并通过数组下标与位图定位。

- 插入对应优先级链表通常为 `O(1)`；
- 用位图寻找最高优先级通常为 `O(1)`；
- 同优先级任务可轮转执行。

从广义上看，FreeRTOS 仍会在任务进入就绪状态时完成优先级归类；但它并不是对一个链表排序，而是通过数组分桶实现。

## 10. 不要与多级反馈队列混淆

多级反馈队列（MLFQ）会根据任务行为动态调整优先级，例如：

- 长时间占用 CPU 后降低优先级；
- 频繁等待 I/O 后提高优先级。

FreeRTOS 默认使用固定优先级调度，任务优先级通常不会自动改变。除非应用主动调用：

```c
vTaskPrioritySet();
```

因此，FreeRTOS 默认不是多级反馈队列，而是固定优先级调度；**启用 `configUSE_PREEMPTION == 1` 时为抢占式调度**，同优先级任务之间是否时间片轮转还取决于 `configUSE_TIME_SLICING`。

## 11. 任务 API 触发抢占的过程

以 `xTaskNotifyGive()` 唤醒高优先级任务为例：

1. `xTaskNotifyGive()` 实际进入 `xTaskGenericNotify()`；
2. `xTaskGenericNotify()` 通过 `TaskHandle_t` 直接取得目标任务 TCB，更新目标任务的通知值与通知状态；
3. 若目标任务正在等待通知，调用 `listREMOVE_ITEM()` 把它的 `xStateListItem` 从所在链表摘下：有限超时时在延时链表；`INCLUDE_vTaskSuspend == 1` 且等待 `portMAX_DELAY` 时在挂起链表 `xSuspendedTaskList`；
4. 调用 `prvAddTaskToReadyList()`，按目标任务优先级将它加入对应的 `pxReadyTasksLists[]` 就绪链表；
5. `taskYIELD_ANY_CORE_IF_USING_PREEMPTION()` 比较目标任务与当前任务 `pxCurrentTCB` 的优先级；
6. 若目标任务优先级更高，则调用 `portYIELD_WITHIN_API()`；最终由 `portYIELD()` 设置 PendSV 挂起。

队列、信号量、事件组等解除任务阻塞时，也遵循类似流程：

```text
修改内核对象状态
        ↓
将等待任务加入就绪链表
        ↓
判断是否需要抢占
        ↓
必要时通过 portYIELD() 请求任务切换
```

在中断中使用 `FromISR` 版本 API 时，函数返回值仍是 `pdPASS` / `pdFAIL` 之类的操作结果；"是否唤醒了更高优先级任务"通过**出参指针** `pxHigherPriorityTaskWoken` 写回（内部依据 `xTaskRemoveFromEventList()` 的 `pdTRUE` / `pdFALSE` 置位），调用方再把它交给 `portYIELD_FROM_ISR()` 决定是否请求切换。PendSV 中保存和恢复任务上下文的细节见 [FreeRTOS 运行机制](02_freertos机制.md)。

## 12. `printf` 为什么会影响 FreeRTOS 任务运行

`printf` 本身不会停止 FreeRTOS 的调度器。在抢占式调度已正常开启的情况下，普通的格式化计算或串口轮询即使占用 CPU，也应当能够被 SysTick 和 PendSV 抢占。

实际项目中出现“打印后任务好像不再切换”时，通常是 `printf` 最终使用的输出路径产生了阻塞，或屏蔽了调度依赖的中断。

### 12.1 `printf` 的典型调用路径

```text
printf()
   ↓
C 标准库格式化字符串
   ↓
_write() / fputc() / __io_putchar()
   ↓
UART、SWO 或 semihosting 等输出后端
```

例如，底层若对每个字符调用轮询发送：

```c
HAL_UART_Transmit(&huart, &ch, 1, HAL_MAX_DELAY);
```

则每个字符都会等待 UART 发送完成。以 115200 波特率为例，发送 100 个字符大约需要 9 ms；若大量、频繁打印日志，会明显占用任务的运行时间。

轮询发送通常只会让**调用 `printf` 的任务**长时间执行或等待；只要 SysTick、PendSV 等中断仍未被屏蔽，其他更高优先级任务仍有机会抢占运行。

### 12.2 导致任务完全不能切换的常见原因

若任务确实完全不再切换，应重点检查下列情况：

- 输出后端在等待 UART、调试器或其他硬件时，长时间执行 `__disable_irq()`、`taskENTER_CRITICAL()` 或提高 `BASEPRI`；这样 SysTick 或 PendSV 可能无法执行；
- 在中断服务函数中调用 `printf`。中断未退出时，普通任务不会运行；而 `printf` 又可能包含长时间轮询、锁或动态内存操作；
- 启用了 semihosting。某些调试配置会在每次输出时让 CPU 停下，等待调试器处理；
- SysTick、PendSV 的优先级或 FreeRTOS 中断优先级配置错误，导致无法正常触发调度；
- 多个任务同时访问同一个输出后端，造成 UART 状态机、发送缓冲区或库内部锁出现竞争。

### 12.3 实时任务中的日志建议

- 不要在时间敏感任务中高频调用 `printf`，尤其避免打印浮点数、长字符串或大块内存；
- 不要在中断中调用 `printf`；中断中只投递日志事件或置位标志；
- 不要用长时间临界区来保护串口输出；
- 更适合的结构是：业务任务将日志复制到队列，由一个低优先级日志任务独占 UART，并使用 DMA 或中断方式发送。

使用 DMA 或中断发送时，必须保证发送缓冲区在传输完成前始终有效；不能将已经离开作用域的局部数组直接交给异步发送函数。

## 13. STM32 + FreeRTOS 中 `printf` 的线程安全

### 13.1 什么是线程安全

当多个任务同时调用一个函数时，若结果等价于这些调用以某种顺序逐个执行，并且不会破坏函数内部状态或共享数据，则这个函数可以视为线程安全。

在 FreeRTOS 中，任务切换会保存每个任务的寄存器和任务栈，因此任务的局部变量彼此独立；但 C 标准库状态、UART 外设和日志缓冲区都位于共享内存或共享硬件中，任务切换不会自动保护它们。

| 层次 | 多任务并发访问时的情况 |
| --- | --- |
| 任务栈与寄存器 | 每个任务独立，通常安全。 |
| `printf` 的 C 库内部状态 | 可能共享，默认不一定安全。 |
| `_write()` 的局部变量 | 每次调用各自在任务栈上保存，通常安全。 |
| `stdout`、输出缓冲区与 UART | 所有任务共享，需要额外同步。 |

### 13.2 C 标准库的共享状态

STM32 工程常使用 Newlib 或 Newlib-nano。`printf` 除了格式化参数外，还可能访问以下共享状态：

- `stdout` 对应的 `FILE` 对象及其缓冲区、读写指针和错误标志；
- `errno`；
- 数字转换、locale、宽字符处理等 C 库状态；
- 可能被格式化函数间接使用的堆与库锁。

如果任务 A 进入 `printf()` 后，在更新这些状态的中间被切换出去，任务 B 又进入 `printf()`，就可能出现状态竞争。结果可能是输出交错、输出丢失、错误码异常，或在特定 C 库配置下发生更严重的问题。

FreeRTOS 提供以下选项：

```c
#define configUSE_NEWLIB_REENTRANT 1
```

启用后，FreeRTOS 会为每个任务维护 Newlib 的重入状态，并在任务切换时切换该状态。这可以改善 `errno` 和一部分标准库内部状态的多任务使用问题，但会增加每个任务的 RAM 占用。

**它不能替代 UART 的互斥访问，也不能保证多次 `printf()` 的整行输出不会交错。**

### 13.3 UART 是额外的共享资源

即使 C 库已经可重入，多个任务最终仍会向同一个 UART 发送数据。典型交错过程如下：

```text
任务 A：printf("temperature = %d\r\n", temp)
        ↓ 输出 "tempera" 后被抢占
任务 B：printf("volume = %d\r\n", volume)
        ↓
任务 A：继续输出剩余字符
```

串口可能收到：

```text
temperavolume = 20
ture = 31
```

若底层使用 `HAL_UART_Transmit()`，另一个任务在 UART 忙时可能发送失败或返回 `HAL_BUSY`；若发送结果未检查，表现为日志丢失。若使用 UART DMA 或中断发送而复用了同一缓冲区，后一次格式化还可能覆盖前一次尚未发完的数据。

### 13.4 临时解决方案：用互斥锁串行化日志

在任务上下文中，可用互斥锁保护**格式化和发送的完整过程**：

```c
static SemaphoreHandle_t log_mutex;

void log_printf(const char *format, ...)
{
    char buffer[192];
    va_list args;

    xSemaphoreTake(log_mutex, portMAX_DELAY);

    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (length > 0) {
        size_t count = (size_t)length;
        if (count >= sizeof(buffer)) {
            count = sizeof(buffer) - 1;
        }

        (void)HAL_UART_Transmit(&huart, (uint8_t *)buffer, count, 50);
    }

    xSemaphoreGive(log_mutex);
}
```

这样可避免不同任务的日志在格式化或 UART 发送期间交错。该方案仍是轮询发送，因此频繁日志仍会占用 CPU；`buffer` 位于任务栈上，任务栈大小也需要包含这部分空间。

互斥锁不能在中断中使用。中断中若需要记录事件，应使用 `xQueueSendFromISR()` 等 `FromISR` API 将信息交给日志任务，并在必要时调用 `portYIELD_FROM_ISR()`。

### 13.5 推荐方案：单独的日志任务

更适合实时系统的方案是让所有业务任务只负责投递日志，而由一个低优先级日志任务独占输出设备：

```text
业务任务 / 中断
        ↓ 投递日志消息
FreeRTOS 队列或环形缓冲区
        ↓
低优先级日志任务
        ↓
UART DMA / UART 中断 / SWO
```

这种方式将日志 I/O 从业务任务中移出，避免串口低速发送拖慢实时逻辑，同时天然保证由单一发送者按顺序访问 UART。

## 待学习

### 任务状态查询、栈余量、运行时间统计与调试 API
