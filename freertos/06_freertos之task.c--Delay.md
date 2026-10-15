# FreeRTOS 之 task.c -- Delay

延时/阻塞链表、溢出链表及 TCB 如何挂入各状态链表，见 [调度器链表](05_freertos之task.c--调度器链表.md)。

## `vTaskDelay()`、`xTaskDelayUntil()` 与 `prvAddCurrentTaskToDelayedList()`

命名说明：真正的函数是 `xTaskDelayUntil()`，返回本轮是否阻塞。`vTaskDelayUntil()` 在 `task.h` 里只是一个兼容宏，调用 `xTaskDelayUntil()` 后丢弃返回值。下文两个名字指同一套逻辑。

1. 关于vTaskDelay():
- 延时为0:<br>
`vTaskDelay(0)` 不进入延时链表，当前任务仍留在就绪链表中，只是强制执行一次 `taskYIELD_WITHIN_API()` → PendSV → `vTaskSwitchContext()`，让调度器按最高就绪优先级重新选择一次。<br>
**以下 PendSV 描述以 Cortex-M port 为例；只有启用 `configUSE_PORT_OPTIMISED_TASK_SELECTION` 且端口支持时，才通过位图找到最高优先级。未启用时使用通用选择逻辑，但 `vTaskDelay(0)` 仍是一次主动 yield。** 随后在选中的就绪链表上对 `pxIndex` 执行 `listGET_OWNER_OF_NEXT_ENTRY`，取下一个节点。具体如下。<br>
  - 该优先级就绪链表里只有自己：绕回自身。PendSV 仍会保存/恢复一次上下文，但接着跑的还是这个任务。<br>
  - 该优先级还有其他任务：换到同优先级链表中的下一个任务，调用者要等下一轮才回来。<br>
**在 `configUSE_PREEMPTION == 1` 的抢占式调度下**，能执行到 `vTaskDelay(0)` 的任务本来就是最高就绪优先级之一，因此不会把 CPU 让给更低优先级；结果只可能是继续自己，或交给同优先级的下一个。<br>
`configUSE_TIME_SLICING` 只决定 SysTick 到期时要不要在同优先级之间切换；主动 `vTaskDelay(0)` 不依赖该开关，只要 yield 了，同优先级链表就会前进一步。<br>

- 延时不为0:<br>

```c
void vTaskDelay( const TickType_t xTicksToDelay )
{
    /* 标记位：记录 xTaskResumeAll() 是否已经触发过一次任务切换。
     * 初始为 pdFALSE，表示"还没有发生过调度"。 */
    BaseType_t xAlreadyYielded = pdFALSE;

    /* 调试/跟踪宏，用于 Tracealyzer 等工具记录"进入了 vTaskDelay"，
     * 未开启 trace 时编译为空，不产生任何代码。 */
    traceENTER_vTaskDelay( xTicksToDelay );

    /* 延时 0 个 tick 没有阻塞的意义，只相当于主动让出一次 CPU（强制重新调度），
     * 所以只有 > 0 时才真正进入"挂到延时链表"的流程。 */
    if( xTicksToDelay > ( TickType_t ) 0U )
    {
        /* 挂起调度器（uxSchedulerSuspended++）。
         * 之后 SysTick 中断仍会发生，但不会执行任务切换，
         * 保证下面对延时链表的操作是"原子"的，不会被调度打断。 */
        vTaskSuspendAll();
        {
            /* 断言：此时调度器挂起计数必须恰好为 1，
             * 说明没有在已挂起调度器的情况下嵌套调用 vTaskDelay。 */
            configASSERT( uxSchedulerSuspended == 1U );

            /* 跟踪宏：记录"任务开始延时"这一事件。 */
            traceTASK_DELAY();

            /* 核心操作：把当前任务从就绪链表中移除，
             * 计算唤醒时刻 xTimeToWake = xTickCount + xTicksToDelay，
             * 按唤醒时间插入延时链表（xDelayedTaskList 或溢出链表）。
             *
             * 第二个参数 pdFALSE 表示"不能无限期等待"，
             * 即不使用 portMAX_DELAY 挂起语义，到点必须唤醒。
             *
             * 原注释的意思：调度器挂起期间即使有中断把任务从事件链表移除，
             * 该任务也只会进入 xPendingReadyList（待就绪链表），
             * 等调度器恢复时才真正回到就绪链表——而当前任务正在运行，
             * 不可能挂在任何事件链表上，所以这里操作是安全的。 */
            prvAddCurrentTaskToDelayedList( xTicksToDelay, pdFALSE );
        }
        /* 恢复调度器。xTaskResumeAll() 会：
         * 1. 把挂起期间进入 xPendingReadyList 的任务搬回就绪链表；
         * 2. 补上挂起期间丢失的 tick（xPendedTicks）；
         * 3. 如果发现有必要切换任务，就直接触发一次调度，并返回 pdTRUE。
         * 返回值存入 xAlreadyYielded，告诉后面"是否已经切换过了"。 */
        xAlreadyYielded = xTaskResumeAll();
    }
    else
    {
        /* 覆盖率测试用的空宏，正常编译不产生代码，
         * 仅用于单元测试时标记"这个分支被执行过"。 */
        mtCOVERAGE_TEST_MARKER();
    }

    /* 若 xTicksToDelay > 0，走到这里时当前任务已经被挂到延时链表里了
     * （逻辑上已"睡着"）；若延时为 0，它仍在就绪链表。
     * 若 xTaskResumeAll() 没有触发过切换（返回 pdFALSE），
     * 则必须手动触发一次调度。 */
    if( xAlreadyYielded == pdFALSE )
    {
        /* 触发任务切换。在 Cortex-M 上就是 portYIELD_WITHIN_API()，
         * 即置位 PendSV，让 PendSV 异常去做上下文切换。 */
        taskYIELD_WITHIN_API();
    }
    else
    {
        mtCOVERAGE_TEST_MARKER();
    }

    /* 跟踪宏：记录"从 vTaskDelay 返回"。
     * 注意：真正执行到这一行时，任务已经睡醒、被重新调度回来了。 */
    traceRETURN_vTaskDelay();
}
```

- 执行过程梳理（以任务 A 调用 `vTaskDelay(100)` 为例）：<br>

**第 1 步：挂起调度器。** `vTaskSuspendAll()` 核心动作只是把 `uxSchedulerSuspended` 加 1（前后各有一个内存屏障宏 `portSOFTWARE_BARRIER` / `portMEMORY_BARRIER`，Cortex-M 端口下展开为空或编译器屏障），中断照常响应，但 SysTick 中断里不会做任务切换。这是为了保护接下来对内核链表的修改——如果改到一半被切走，链表就会处于损坏状态。注意这里用挂起调度器而不是关中断，开销更小、中断实时性不受影响。<br>

**第 2 步：把自己挂到延时链表。** `prvAddCurrentTaskToDelayedList` 主要做三件事：
  1. 把任务 A 的状态链表项从就绪链表（`pxReadyTasksLists[优先级]`）中移除；若该优先级的就绪链表因此变空，调用 `portRESET_READY_PRIORITY()` 清掉位图里对应的位（通用选择方式下该宏为空）；
  2. 计算唤醒时刻：`xTimeToWake = 当前xTickCount + 100`；
  3. 把该时刻写进任务的链表项值，然后**按唤醒时间升序**插入 `pxDelayedTaskList`。如果 `xTimeToWake` 计算时发生了溢出（回绕），则插入 `pxOverflowDelayedTaskList`。**只有插入当前延时链表、且 `xTimeToWake < xNextTaskUnblockTime` 时才更新 `xNextTaskUnblockTime`**；进溢出链表的任务要等 tick 回绕换表后由 `prvResetNextTaskUnblockTime()` 重新算。这个全局变量让 tick 中断能快速判断"最近一个要醒的任务是什么时候"。

  另外，启用 `INCLUDE_xTaskAbortDelay` 时进入前还会把 `ucDelayAborted` 清为 `pdFALSE`；`vTaskDelay()` 传入的第二个参数是 `pdFALSE`，所以不会走到"无限期等待进挂起链表"的分支。

  此时任务 A 逻辑上已经是阻塞态了，但 CPU 还在跑它的代码——因为调度器被挂起，切换不了。<br>

**第 3 步：恢复调度器。** `xTaskResumeAll()` 把挂起期间攒下的待就绪任务、待处理 tick 都补上。如果补处理的过程中发现需要切换任务，它会顺手触发调度并返回 `pdTRUE`。<br>

**第 4 步：保底切换。** 如果第 3 步没有触发切换（大多数情况），`taskYIELD_WITHIN_API()` 手动置位 PendSV。PendSV 异常执行上下文切换：保存任务 A 的现场，从就绪链表选出最高优先级任务恢复执行。**任务 A 在这次切换处阻塞；重新被调度后，从 `taskYIELD_WITHIN_API()` 调用之后继续执行。**<br>

**第 5 步：100 个 tick 后被唤醒。** 每次 SysTick 中断里 `xTaskIncrementTick()` 检查 `xTickCount >= xNextTaskUnblockTime`，到点后把任务 A 从延时链表移回就绪链表。若 A 的优先级高于当前运行任务，则再次触发 PendSV。任务 A 恢复执行，从 `taskYIELD_WITHIN_API()` 之后继续跑，执行 `traceRETURN_vTaskDelay()` 并返回。<br>

两个容易忽略的要点：
- **`xAlreadyYielded` 的意义是避免重复调度**：若 `xTaskResumeAll()` 已经触发过一次切换，就无需在后面再次 yield。
- **`vTaskDelay` 是相对延时**：从"调用那一刻的 tick"起算 100 个 tick，任务被唤醒后如果还要等更高优先级任务让出 CPU，实际间隔会拉长、有累积误差。要做精确周期性任务应该用 `vTaskDelayUntil`（以绝对时刻为基准）。

2. 关于 `xTaskDelayUntil()`：

### 先看它解决什么问题

`vTaskDelay(10)` 表示：**从调用这一刻起再等 10 个 tick**。如果任务每轮先工作 3 个 tick，再调用 `vTaskDelay(10)`，两次运行之间实际约为 13 个 tick。

`xTaskDelayUntil()` 表示：**等到预先排好的下一个时刻**。常见写法：

```c
TickType_t xLastWakeTime = xTaskGetTickCount();

for( ;; )
{
    DoWork();
    xTaskDelayUntil( &xLastWakeTime, 10 );
}
```

假设初始化时 `xLastWakeTime = 100`，周期是 10：

```text
计划运行时刻：110、120、130、140……

本轮工作在 tick 104 结束：
目标时刻 = 100 + 10 = 110
还没到 110 → 阻塞 6 个 tick
醒来后 xLastWakeTime 已更新为 110

下一轮工作在 tick 117 结束：
目标时刻 = 110 + 10 = 120
还没到 120 → 阻塞 3 个 tick
```

不管每轮工作用了 4 个 tick 还是 7 个 tick，目标时刻始终沿着 110、120、130……前进，因此工作耗时不会累加到周期里。

`pxPreviousWakeTime` 这个名字容易误解。它记录的不是「任务实际获得 CPU 的时刻」，而是**上一次计划唤醒时刻**。函数每次都用：

```text
下一次计划唤醒时刻 = 上一次计划唤醒时刻 + 周期
```

### 延时链表与阻塞状态

通常所说的「延时链表」就是用于管理**有超时时刻的阻塞任务**的链表：

- `vTaskDelay()`、`xTaskDelayUntil()` 只等待时间，任务的 `xStateListItem` 挂入延时链表。
- 等队列、信号量等对象并设置超时时间时，`xStateListItem` 也挂入延时链表，用于处理超时；同时 `xEventListItem` 挂入该对象自己的事件等待链表，用于处理事件到达。
- 无限期等待没有唤醒时刻。`prvAddCurrentTaskToDelayedList()` 里的条件是三者同时满足：`INCLUDE_vTaskSuspend == 1`、`xTicksToWait == portMAX_DELAY`、调用方传入的 `xCanBlockIndefinitely != pdFALSE`，此时 `xStateListItem` 用 `listINSERT_END()` 放入挂起链表 `xSuspendedTaskList`。任一不满足，则把 `portMAX_DELAY` 当普通唤醒时刻挂进延时链表。本仓库 `FreeRTOSConfig.h` 未定义 `INCLUDE_vTaskSuspend`，默认为 0，因此实际编译的是后者。

所以延时链表可以称为「按唤醒 tick 排序的阻塞链表」，但不能反过来说所有阻塞任务都只在延时链表里。详见 [调度器链表](05_freertos之task.c--调度器链表.md)。

### 源码逐段说明

```c
BaseType_t xTaskDelayUntil( TickType_t * const pxPreviousWakeTime,
                            const TickType_t xTimeIncrement )
{
    TickType_t xTimeToWake;                  /* 本轮计划唤醒时刻。 */
    BaseType_t xAlreadyYielded, xShouldDelay = pdFALSE; /* 是否已经切换；本轮是否需要进入延时链表。 */

    traceENTER_xTaskDelayUntil( pxPreviousWakeTime, xTimeIncrement );

    configASSERT( pxPreviousWakeTime );             /* 基准时刻指针不能为 NULL。 */
    configASSERT( ( xTimeIncrement > 0U ) );        /* 周期必须大于 0。 */

    /* 防止下面读取 tick、判断时间和修改链表时发生任务切换。
     * 中断仍可响应；挂起期间到来的 tick 记入 xPendedTicks。 */
    vTaskSuspendAll();
    {
        /* 调度器挂起期间 xTickCount 保持不变，因此只读取一次。 */
        const TickType_t xConstTickCount = xTickCount;

        configASSERT( uxSchedulerSuspended == 1U );

        /* 关键：从上一次“计划时刻”计算下一次计划时刻，
         * 而不是从当前 xTickCount 重新起算。 */
        xTimeToWake = *pxPreviousWakeTime + xTimeIncrement;

        if( xConstTickCount < *pxPreviousWakeTime )
        {
            /* 当前 tick 比上次计划时刻小，说明无符号 tick 已经经过 0 回绕。
             * 此时只有 xTimeToWake 也完成回绕、并且仍大于当前 tick，
             * 才说明计划时刻还在未来。 */
            if( ( xTimeToWake < *pxPreviousWakeTime ) &&
                ( xTimeToWake > xConstTickCount ) )
            {
                xShouldDelay = pdTRUE;
            }
            else
            {
                mtCOVERAGE_TEST_MARKER();
            }
        }
        else
        {
            /* 当前 tick 尚未相对上次计划时刻回绕。
             * 计划时刻自身已经回绕，或者计划时刻仍大于当前 tick，
             * 都表示目标还在未来，需要阻塞。 */
            if( ( xTimeToWake < *pxPreviousWakeTime ) ||
                ( xTimeToWake > xConstTickCount ) )
            {
                xShouldDelay = pdTRUE;
            }
            else
            {
                /* 目标时刻等于或早于当前 tick：这一周期已经错过，不阻塞。 */
                mtCOVERAGE_TEST_MARKER();
            }
        }

        /* 不论本轮是否需要阻塞，都推进计划时间。
         * 这样下一轮仍沿用原来的固定周期，不会因本轮超时而整体后移。 */
        *pxPreviousWakeTime = xTimeToWake;

        if( xShouldDelay != pdFALSE )
        {
            traceTASK_DELAY_UNTIL( xTimeToWake );

            /* prvAddCurrentTaskToDelayedList() 接收相对等待时长。
             * 即使 xTimeToWake 发生回绕，无符号减法仍能得到剩余 tick。 */
            prvAddCurrentTaskToDelayedList( xTimeToWake - xConstTickCount, pdFALSE );
        }
        else
        {
            mtCOVERAGE_TEST_MARKER();
        }
    }

    /* 恢复调度器，同时处理挂起期间积累的 tick 和待就绪任务。 */
    xAlreadyYielded = xTaskResumeAll();

    /* 若 xTaskResumeAll() 尚未触发任务切换，则主动请求一次调度。
     * 已进入延时链表时，这一步让当前任务真正停止运行；
     * 未进入延时链表时，这相当于主动 yield 一次。 */
    if( xAlreadyYielded == pdFALSE )
    {
        taskYIELD_WITHIN_API();
    }
    else
    {
        mtCOVERAGE_TEST_MARKER();
    }

    traceRETURN_xTaskDelayUntil( xShouldDelay );

    return xShouldDelay; /* pdTRUE：本轮发生阻塞；pdFALSE：目标时刻已到或已错过。 */
}
```

### 正常情况下的完整流程

1. 根据上一次计划时刻和周期，算出下一次计划时刻。
2. 比较当前 tick 与计划时刻，得出本轮是否需要阻塞（`xShouldDelay`）。
3. 无论是否阻塞，先更新 `*pxPreviousWakeTime`，让它保存本轮计划时刻。
4. 如果计划时刻还在未来，将当前任务从就绪链表移入延时链表。
5. 恢复调度器并触发任务切换。
6. 到达计划 tick 后，`xTaskIncrementTick()` 将任务移回就绪链表；任务真正何时获得 CPU，还取决于任务优先级和其他就绪任务。

### 如果任务执行超时

仍以周期 10 为例。任务原计划在 tick 120 运行，但这一轮工作直到 tick 135 才调用 `xTaskDelayUntil()`：

```text
上次计划时刻 = 120
下一次计划时刻 = 130
当前时刻 = 135

130 已经过去 → 本轮不阻塞
基准仍更新为 130
```

函数返回后，任务可以立即开始下一轮。下一次再调用时，目标从 130 推到 140；如果当时还没到 140，任务就睡到 140。如果仍然超过 140，则再次不阻塞。也就是说，它会尝试追赶原来的时间表，而不是把周期改成「从 135 开始重新计时」。

即使本轮不阻塞，源码仍会在末尾请求一次调度；这不等于一定切换到别的任务，只是让调度器重新选择最高优先级的就绪任务。

### Tick 回绕为什么要分两组判断

`TickType_t` 是无符号整数，计到最大值后会回到 0。例如：

```text
上次计划时刻 = 0xFFFFFFFC
周期           = 10
下一计划时刻   = 6
```

数值 6 看起来比 `0xFFFFFFFC` 小，但在时间上其实位于未来。源码中的两个分支只是在区分「当前 tick 是否已经越过 0」，从而正确判断 `xTimeToWake` 是在未来还是已经错过。

平时理解主流程不需要记住这些条件，只需记住：**函数始终从上一次计划时刻推导下一次计划时刻；回绕判断只是为了让这个比较在无符号 tick 越过 0 时仍然正确。**

### 与 `vTaskDelay()` 的区别

- `vTaskDelay(10)`：从本次调用时刻开始再等待 10 个 tick，适合“本轮做完后再休息固定时间”。
- `xTaskDelayUntil(..., 10)`：按固定的计划时刻运行，适合周期采样、控制循环、周期刷新等。
- `xTaskDelayUntil()` 返回 `pdTRUE`，表示本轮进入了延时链表；返回 `pdFALSE`，表示目标时刻已经到达或错过，本轮没有阻塞。

## 待学习

### `xTaskIncrementTick()` 的 Tick 回绕换表与 `xPendedTicks` 补算细节
