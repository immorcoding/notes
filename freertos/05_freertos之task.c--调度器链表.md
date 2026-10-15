# FreeRTOS 之 task.c -- 调度器链表

## 状态链表的整体划分

FreeRTOS 并非只维护一条“任务状态链表”，而是用多条链表分别管理任务。主要链表及作用如下：

- **就绪链表** `pxReadyTasksLists[ configMAX_PRIORITIES ]`：每个优先级各有一条链表，任务按优先级放入对应链表。正在运行的任务也仍位于自己的就绪链表中，`pxCurrentTCB` 只是额外指出其中当前获得 CPU 的任务。
- **延时/阻塞链表**：存储是两个静态变量 `xDelayedTaskList1` 与 `xDelayedTaskList2`，内核平时通过两个指针访问：`pxDelayedTaskList` 指向"当前延时链表"，`pxOverflowDelayedTaskList` 指向"溢出延时链表"，`xTickCount` 回绕时 `taskSWITCH_DELAYED_LISTS()` 把两个指针互换。任务执行 `vTaskDelay()`，或等待队列、信号量、通知等事件且设置了超时时，都会按唤醒 tick 挂入其中：未回绕前到期的任务在当前延时链表，跨越回绕点才到期的任务在溢出延时链表。
- **挂起链表** `xSuspendedTaskList`：显式调用 `vTaskSuspend()` 的任务位于此处。**当 `INCLUDE_vTaskSuspend == 1`、等待时间为 `portMAX_DELAY`、且调用方允许无限期阻塞（`xCanBlockIndefinitely != pdFALSE`）三者同时满足时，无限期等待事件的任务也会使用该链表；否则把 `portMAX_DELAY` 当普通唤醒时刻放入延时链表。**这是一种内核存放策略，对外仍属于阻塞等待。本仓库 `FreeRTOSConfig.h` 未定义 `INCLUDE_vTaskSuspend`，默认为 0。
- **待删除链表** `xTasksWaitingTermination`：调用 `vTaskDelete()` 后，任务会先进入该链表，随后由 Idle 任务释放其 TCB 和动态分配的任务栈。该链表是否使用取决于 `INCLUDE_vTaskDelete` 配置。
- **待就绪链表** `xPendingReadyList`：这是调度器暂停期间使用的**过渡链表**，不是一种稳定任务状态。某个阻塞任务等待的事件在此期间到来时，内核先把它放到这里；`xTaskResumeAll()` 再将它真正迁入对应的就绪链表。

每个 TCB 用两个不同的链表项连接上述机制：

```c
ListItem_t xStateListItem;  // 加入就绪、延时/阻塞、挂起、待删除等状态链表
ListItem_t xEventListItem;  // 加入队列、信号量、互斥锁等对象的事件等待链表
```

因此，对队列、信号量、互斥锁等对象的阻塞等待通常同时涉及两条链表：`xStateListItem` 位于延时或挂起链表以处理超时，`xEventListItem` 位于相应对象的事件等待链表。`xPendingReadyList` 保存的通常是 `xEventListItem`；调度器恢复前，任务的 `xStateListItem` 可能仍留在延时/阻塞链表中。**任务通知是例外：通知状态和值直接位于目标任务的 TCB 中，等待通知时不加入外部对象的事件等待链表。**

`vTaskDelay()` / `vTaskDelayUntil()` 如何把任务挂入延时链表，见 [Delay](06_freertos之task.c--Delay.md)。

## 链表项如何走回 TCB：pvOwner

调度器链表里挂的不是整块 TCB，而是 TCB 内部的链表项；该项再通过 `pvOwner` 反指回 TCB。**只有调度器选中该任务并进入端口层上下文切换时，才会从 TCB 的 `pxTopOfStack` 恢复栈顶。**详见 [TCB](03_freertos之task.c--TCB.md)。

完整指向链：

```text
就绪 / 延时 / 挂起等链表
    └── 节点 = TCB 内部的 xStateListItem（ListItem_t）
            └── pvOwner ──────────→ 这块 TCB
                    ├── pxTopOfStack ──→ 当前栈顶（最后一个已压入数据的位置）
                    └── pxStack      ──→ 栈缓冲区起始（低地址端，不是栈顶）
```

对任务来说，两个链表项都这样用：

| 链表项 | 挂到哪 | `pvOwner` 指向 |
|--------|--------|----------------|
| `xStateListItem` | 就绪 / 延时 / 挂起 / 待删除 | 自己的 TCB |
| `xEventListItem` | 队列、信号量等事件等待链表 | 自己的 TCB |

创建时会把状态链表项的主人设成自己的 TCB：`listSET_LIST_ITEM_OWNER( &( pxNewTCB->xStateListItem ), pxNewTCB )`。调度器从链表取出一个节点后，走 `listGET_LIST_ITEM_OWNER()` 得到 TCB；**若该任务随后被选中运行，端口层才会读取 `pxTopOfStack` 做上下文恢复。**

`pvOwner` 是 `ListItem_t` 里的一个 `void *`，用来回答：这个链表节点属于谁。`xStateListItem` 和 `xEventListItem` 都是同一个 `ListItem_t`，字段相同，用途和 `xItemValue` 含义不同：

```c
struct xLIST_ITEM
{
    listFIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE            /* 完整性检查字段，默认配置下展开为空 */
    configLIST_VOLATILE TickType_t xItemValue;           /* 排序键，含义看挂到哪条链表 */
    struct xLIST_ITEM * configLIST_VOLATILE pxNext;
    struct xLIST_ITEM * configLIST_VOLATILE pxPrevious;
    void * pvOwner;                                      /* 回头指向自己的 TCB */
    struct xLIST * configLIST_VOLATILE pxContainer;      /* 当前挂在哪条链表上 */
    listSECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE           /* 完整性检查字段，默认配置下展开为空 */
};
typedef struct xLIST_ITEM ListItem_t;
```

两个 `list*_INTEGRITY_CHECK_VALUE` 宏只在 `configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES == 1` 时才展开成字段，默认为 0；`configLIST_VOLATILE` 默认也展开为空。因此默认配置下有效成员就是下面五个。

- `xItemValue`：排序键。`vListInsert()` 按它升序找插入位置。它本身只是一个 `TickType_t` 数字，**填什么值、代表什么意思，由把节点挂进链表的那段代码决定**，见下一节。
- `pxNext` / `pxPrevious`：串在同一条链表上。
- `pvOwner`：这个节点的主人。对任务而言就是对应 TCB。
- `pxContainer`：当前挂在哪条链表上。

链表只能看见 `ListItem`，看不见外面的 TCB。`pvOwner` 就是从节点走回 TCB 的回头路。写成 `void *` 是因为同一套链表也给队列、定时器等用，主人不一定是 TCB。

### `xItemValue` 在不同链表里各是什么

`list.c` 是通用容器，不认识"唤醒时刻""优先级"这些概念，只会拿 `xItemValue` 比大小。所以同一个字段，挂到不同链表时，`tasks.c` 会在插入前往里写不同的东西：

| 链表项 | 挂在哪条链表 | 插入前写入 `xItemValue` 的值 | 排序的作用 |
|--------|--------------|------------------------------|------------|
| `xStateListItem` | 就绪链表 `pxReadyTasksLists[prio]` | 不写、不用 | 就绪链表按优先级分成多条，同一条里都是同优先级，`prvAddTaskToReadyList()` 用宏 `listINSERT_END()`（内联版 `vListInsertEnd()`）尾插，不排序 |
| `xStateListItem` | 延时链表 / 溢出延时链表 | **绝对唤醒时刻** `xTimeToWake = xTickCount + 延时` | 按时间升序，表头就是最早要醒的任务 |
| `xStateListItem` | 挂起链表 / 待删除链表 | 不用 | 尾插，不排序 |
| `xEventListItem` | 队列、信号量等对象的等待链表 | **优先级倒数** `configMAX_PRIORITIES - uxPriority` | 数值小的排前面，即高优先级任务排在表头，事件到来时表头先被唤醒 |

事件链表为什么要写成 `configMAX_PRIORITIES - uxPriority` 而不是直接写 `uxPriority`：`vListInsert()` 只会升序排列，而内核希望**优先级高的任务先拿到事件**。FreeRTOS 里数值大 = 优先级高，直接写 `uxPriority` 会让高优先级排到表尾。取一次差值把大小颠倒过来，高优先级就变成小数值、排到表头。

举例：`configMAX_PRIORITIES = 8`，两个任务同时在等同一个队列，都没有超时。

```text
任务 X：uxPriority = 5 → xEventListItem.xItemValue = 8 - 5 = 3
任务 Y：uxPriority = 2 → xEventListItem.xItemValue = 8 - 2 = 6

队列的等待链表（升序）：
    [X: 3] → [Y: 6]
      ↑
   表头，高优先级的 X 在前，有数据时 X 先被唤醒
```

再举一个两个链表项同时在用的例子：`configMAX_PRIORITIES = 8`，任务 A（优先级 3）在 tick 20 调用 `xQueueReceive()`，超时 80 tick。

```text
A 的两个链表项此时分别是：

xStateListItem
    xItemValue  = 20 + 80 = 100      ← 唤醒时刻
    pxContainer = pxDelayedTaskList   ← 挂在当前延时链表（指向 xDelayedTaskList1 或 2）
    pvOwner     = A 的 TCB

xEventListItem
    xItemValue  = 8 - 3 = 5           ← 优先级排序键
    pxContainer = 队列的 xTasksWaitingToReceive
    pvOwner     = A 的 TCB
```

两个 `xItemValue` 是两个完全独立的数字，一个在延时链表里当时间比，一个在队列等待链表里当优先级比，互不影响。

- 有人往队列写数据（tick 50）：内核从 `xTasksWaitingToReceive` 表头取出 A 的 `xEventListItem`，顺着 `pvOwner` 找到 A 的 TCB，再把 A 的 `xStateListItem` 从延时链表摘掉、放回就绪链表。`xStateListItem.xItemValue = 100` 作废，不会再触发超时。
- 没人写数据：tick 100 时 `xTaskIncrementTick()` 从延时链表表头摘下 A 的 `xStateListItem`，同时把 A 的 `xEventListItem` 从队列等待链表摘掉，放回就绪链表。tick 中断到此为止。等 A 重新拿到 CPU、从 `vTaskPlaceOnEventList()` 之后继续跑时，`xQueueReceive()` 内部用 `xTaskCheckForTimeOut()` 判断出已超时且队列仍空，才 `return errQUEUE_EMPTY`。

如果 A 只是 `vTaskDelay(80)`，不涉及任何对象，那么只有 `xStateListItem` 挂进延时链表，`xEventListItem` 不挂任何链表，`pxContainer = NULL`。

### 为什么醒来时两条都要摘

上面两种结局有个共同点：**不管谁先叫醒 A，另一条链上的 A 也必须摘掉。**

一条是名单（「有货叫我」），一条是闹钟（「超时叫我」）。任务只需要被叫醒一次，谁先到，另一个就作废。

漏摘的后果不一样：

- **只摘状态钩、不摘事件钩**：A 已经在跑了，名字却还留在队列的等人名单上。下次有人写队列，内核翻名单又去「叫醒」一个根本没在等的 A，链表状态就坏了。所以超时路径上，`xTaskIncrementTick()` 摘完 `xStateListItem` 后会补一句判断：`xEventListItem` 还挂着东西（`listLIST_ITEM_CONTAINER() != NULL`）就一起摘。`vTaskSuspend()`、`vTaskDelete()`、`xTaskAbortDelay()` 里都有同样这一段。
- **只摘事件钩、不摘状态钩**：A 回到就绪链表跑起来了，`xStateListItem` 却还留在延时链表里。到了原来的唤醒时刻，tick 又会把它当成到期任务摘一遍。

事件先到时不需要额外判断：`xTaskRemoveFromEventList()` 本来就是从名单表头摘人，状态钩必然在延时或挂起链表上，直接摘。

例外是**任务通知**：它不进任何对象的名单，所以只摘状态钩，源码里还用 `configASSERT( listLIST_ITEM_CONTAINER( &( pxTCB->xEventListItem ) ) == NULL )` 断言这一点。

还有一种情况是事件到了、却暂时摘不动状态钩：调度器正被挂起。这时事件钩先寄放到 `xPendingReadyList`，见 [调度器挂起与临界区](07_freertos之task.c--调度器挂起与临界区.md)。

## 延时链表插入与 `xNextTaskUnblockTime`

如上一节所述，每个阻塞/延时任务自己的唤醒时刻写在 `xStateListItem.xItemValue` 里。那么内核怎么知道"最近一个要醒的是谁、什么时候"？

`xNextTaskUnblockTime` 是调度器里的**全局变量，全内核只有一份**。它不保存每个任务的延时，只缓存当前延时链表表头、也就是**最早那个唤醒时刻**，给 SysTick 做快路径判断。

```text
延时链表（按绝对唤醒 tick 升序，不是 Delay 参数本身）
    B @ tick 30    ← 表头，最早
    C @ tick 80
    A @ tick 100

xNextTaskUnblockTime = 30   ← 只缓存表头这一份
```

每个 SysTick 里 `xTaskIncrementTick()` 先把 `xTickCount++`，再和这个全局值比较：还没到，延时链表一眼都不看；到了，只从表头摘已经到期的任务放回就绪链表，再把 `xNextTaskUnblockTime` 改成新表头的唤醒时刻。链表空了则写成 `portMAX_DELAY`。睡着的任务不会每个 tick 重新进入 `vTaskDelay()` 看一眼再退出，现场冻在当初 yield 处，直到被搬回就绪链表并再次调度。

新任务挂入延时链表时，走 `vListInsert()`：从表头往后比 `xItemValue`，找到第一个比自己更晚醒的节点，插到它前面。排序键是 `xTimeToWake = 当前 xTickCount + 延时`，不是还剩几个 tick。

例如链表里已有唤醒时刻 10、20、100，再插入 30：

```text
[10] → [20] → [100]
               ↑
        新来的 30 插这里

插入后：
[10] → [20] → [30] → [100]
```

比较过程是 `10 <= 30` 继续、`20 <= 30` 继续、`100 <= 30` 为否，因此插到 100 前面。最早醒则第一次比较就停，**O(1)**；最晚醒则走完整条链表，**O(n)**。`n` 是这条延时链表上已有的任务数。嵌入式里同时阻塞的任务通常不多，内核用线性插入换实现简单、时间可预期。插入后若新任务比原来的 `xNextTaskUnblockTime` 更早醒，才更新该全局变量。

`vTaskDelay()` 如何计算唤醒时刻并触发切换，见 [Delay](06_freertos之task.c--Delay.md)。

## 已在其他笔记中记录的内容

- 任务状态、就绪队列、最高优先级选择、两种优先级链表方案、不要与多级反馈队列混淆、API 唤醒高优先级任务：[其他 FreeRTOS 相关](其他FreeRTOS相关.md)
- TCB 结构与 `pxTopOfStack` / `pxStack`：[TCB](03_freertos之task.c--TCB.md)
- 动态创建时如何申请 TCB/栈并挂入就绪链表：[动态创建任务的内存管理](04_freertos之task.c--任务创建与内存管理.md)

## `vTaskPrioritySet()` 与任务在就绪链表中的迁移

就绪链表按优先级分桶，`prvAddTaskToReadyList()` 用当时的 `uxPriority` 当下标尾插。改优先级只改 TCB 里的数字是不够的：`xStateListItem` 还留在旧桶，调度器却按新下标去新桶找人。所以要**换桶**。

`vTaskPrioritySet(xTask, uxNewPriority)` 需要 `INCLUDE_vTaskPrioritySet == 1`，`xTask == NULL` 表示改自己。本仓库未定义该宏，默认 0，当前固件没编进这个 API。下文只看单核、`configUSE_PORT_OPTIMISED_TASK_SELECTION == 0` 的路径。

整个函数按四步走：

1. 钳新优先级到 `< configMAX_PRIORITIES`，进临界区
2. 判断要不要 yield
3. 记下旧优先级，改 TCB 字段
4. 若在就绪链表则换桶，最后按第 2 步的结论 yield

### 换桶

判断依据是 `xStateListItem` 是否仍在旧桶里：

- **在就绪链表**：`uxListRemove()` 摘下，`prvAddTaskToReadyList()` 按新 `uxPriority` 尾插进新桶。
- **不在**（延时、挂起、待删除）：只改数字，节点不动。延时链表按唤醒时刻排，与优先级无关；醒来时再走 `prvAddTaskToReadyList()`，那时按新优先级进桶。

```c
#define listIS_CONTAINED_WITHIN( pxList, pxListItem ) \
    ( ( ( pxListItem )->pxContainer == ( pxList ) ) ? ( pdTRUE ) : ( pdFALSE ) )    /* 节点是否挂在指定链表上 */

#define prvAddTaskToReadyList( pxTCB )                                                                     \
    do {                                                                                                   \
        traceMOVED_TASK_TO_READY_STATE( pxTCB );                                                           \
        taskRECORD_READY_PRIORITY( ( pxTCB )->uxPriority );                                                /* 顺带抬扫描起点，见下一节 */ \
        listINSERT_END( &( pxReadyTasksLists[ ( pxTCB )->uxPriority ] ), &( ( pxTCB )->xStateListItem ) ); /* 用当前 uxPriority 当下标尾插 */ \
        tracePOST_MOVED_TASK_TO_READY_STATE( pxTCB );                                                      \
    } while( 0 )
```

```c
uxPriorityUsedOnEntry = pxTCB->uxPriority;    /* 先记旧下标，否则改完就找不到旧桶了 */

pxTCB->uxPriority = uxNewPriority;            /* 开互斥锁时这里还要处理 uxBasePriority，留到互斥锁章节 */

if( listIS_CONTAINED_WITHIN( &( pxReadyTasksLists[ uxPriorityUsedOnEntry ] ), &( pxTCB->xStateListItem ) ) != pdFALSE )
{
    if( uxListRemove( &( pxTCB->xStateListItem ) ) == ( UBaseType_t ) 0 )    /* 返回摘完后旧桶还剩几个 */
    {
        portRESET_READY_PRIORITY( uxPriorityUsedOnEntry, uxTopReadyPriority );    /* 本配置为空宏 */
    }

    prvAddTaskToReadyList( pxTCB );           /* 按新 uxPriority 进新桶 */
}
/* else：不在就绪链表，到此为止 */
```

`configMAX_PRIORITIES = 5`，A（2）正在跑，把 C 从 3 降到 1：

```text
改前                改后
[1] Idle            [1] Idle → C
[2] A ← 当前         [2] A
[3] C               [3] 空
```

### `uxTopReadyPriority` 是扫描起点

`uxTopReadyPriority` 不是"当前最高非空优先级"，而是**下次从哪一档开始往下扫**。选任务时空桶就 `--`，扫到第一个非空桶取任务，扫完才写回。

它只在任务挂进就绪链表时被比较一次，且**只升不降**；任务离开就绪链表时本配置不往下调。所以上例把 C 从 3 降到 1 后，起点仍是 3；下次选任务从 3 看到空、落到 2 选出 A，再写回 2。

```c
PRIVILEGED_DATA static volatile UBaseType_t uxTopReadyPriority = tskIDLE_PRIORITY;

#define taskRECORD_READY_PRIORITY( uxPriority ) \
    do {                                            \
        if( ( uxPriority ) > uxTopReadyPriority )   /* 只升不降 */ \
        {                                           \
            uxTopReadyPriority = ( uxPriority );    \
        }                                           \
    } while( 0 )

#define taskSELECT_HIGHEST_PRIORITY_TASK()                                       \
    do {                                                                                 \
        UBaseType_t uxTopPriority = uxTopReadyPriority;                                  /* 起点 */ \
                                                                                         \
        while( listLIST_IS_EMPTY( &( pxReadyTasksLists[ uxTopPriority ] ) ) != pdFALSE ) \
        {                                                                                \
            configASSERT( uxTopPriority );                                               /* 扫到 0 还空说明内核状态坏了 */ \
            --uxTopPriority;                                                             /* 空桶降一档 */ \
        }                                                                                \
                                                                                         \
        listGET_OWNER_OF_NEXT_ENTRY( pxCurrentTCB, &( pxReadyTasksLists[ uxTopPriority ] ) ); /* 同优先级轮转 */ \
        uxTopReadyPriority = uxTopPriority;                                                   /* 扫完写回 */ \
    } while( 0 )

#define taskRESET_READY_PRIORITY( uxPriority )                        /* 本配置为空 */
#define portRESET_READY_PRIORITY( uxPriority, uxTopReadyPriority )    /* 本配置为空 */
```

### 要不要立刻切任务

| 场景 | yield |
|------|-------|
| 把别的任务升到比当前任务更高 | 要 |
| 把当前任务升高 | 不要，它已经是最高 |
| 把正在跑的任务降低 | 要，可能有别人更高 |
| 把别的任务降低 | 不要 |

单核 yield 就是 `portYIELD_WITHIN_API()` 挂起 PendSV，在临界区退出后才真正切。

```c
if( uxNewPriority > uxCurrentBasePriority )               /* 升高 */
{
    if( pxTCB != pxCurrentTCB )                           /* 改的是别人 */
    {
        if( uxNewPriority > pxCurrentTCB->uxPriority )
        {
            xYieldRequired = pdTRUE;
        }
    }
}
else if( taskTASK_IS_RUNNING( pxTCB ) == pdTRUE )         /* 降低的是正在跑的任务 */
{
    xYieldRequired = pdTRUE;
}

/* 换桶之后： */
if( xYieldRequired != pdFALSE )
{
    taskYIELD_TASK_CORE_IF_USING_PREEMPTION( pxTCB );     /* 单核 = portYIELD_WITHIN_API() */
}
```

事件等待链表的排序键、以及互斥锁的 `uxBasePriority` 与优先级继承，留到信号量、互斥锁章节。
