# FreeRTOS 之 task.c -- 调度器挂起与临界区

就绪 / 延时 / 待就绪等链表划分见 [调度器链表](05_freertos之task.c--调度器链表.md)。任务身上两个链表项（状态钩 `xStateListItem`、事件钩 `xEventListItem`）的分工，以及醒来时为什么两条都要摘，也在那里。

## 1. `xPendingReadyList`：调度器挂起期间的寄放盒

### 起因：一件事必须做，一件事不能做

`vTaskSuspendAll()` 挂起调度器后，内核正在改就绪 / 延时链表，这两条链**别人不能碰**。

但**中断没关**。这时 ISR 往队列写了数据，等这个队列的任务 A 就该醒了。内核面对两件事：

| 事情 | 能不能现在做 | 为什么 |
|------|--------------|--------|
| 把 A 的事件钩从队列等人名单摘掉 | **必须马上做** | 不摘，下一个写队列的人翻名单会重复叫醒 A |
| 把 A 的状态钩从延时链表摘掉、放进就绪链表 | **现在不能做** | 这两条链有人正在改 |

一个要马上，一个不能做。于是内核先把摘下来的事件钩**寄放**到 `xPendingReadyList`，等调度器恢复再补完剩下的动作。

`xPendingReadyList` 因此**不是一种任务状态**，只是个中转位置。

### 寄放：`xTaskRemoveFromEventList()`

事件到来走的都是这个函数，它按 `uxSchedulerSuspended` 分两条路：

```c
BaseType_t xTaskRemoveFromEventList( const List_t * const pxEventList )
{
    TCB_t * pxUnblockedTCB;
    BaseType_t xReturn;

    /* 名单按优先级排序，表头就是该醒的那个。 */
    pxUnblockedTCB = listGET_OWNER_OF_HEAD_ENTRY( pxEventList );
    configASSERT( pxUnblockedTCB );

    /* 事件钩无论如何都先摘掉：不摘会被重复叫醒。 */
    listREMOVE_ITEM( &( pxUnblockedTCB->xEventListItem ) );

    if( uxSchedulerSuspended == ( UBaseType_t ) 0U )
    {
        /* 调度器在跑：一次做完，摘状态钩、进就绪链表。 */
        listREMOVE_ITEM( &( pxUnblockedTCB->xStateListItem ) );
        prvAddTaskToReadyList( pxUnblockedTCB );

        #if ( configUSE_TICKLESS_IDLE != 0 )
        {
            /* 这个任务的超时时刻可能正是 xNextTaskUnblockTime，重算一下。 */
            prvResetNextTaskUnblockTime();
        }
        #endif
    }
    else
    {
        /* 调度器挂起：延时链表和就绪链表都不能动，
         * 把已经摘下来的事件钩寄放到 xPendingReadyList。 */
        listINSERT_END( &( xPendingReadyList ), &( pxUnblockedTCB->xEventListItem ) );
    }

    /* 单核：叫醒的任务比当前任务优先级高，就告诉调用者该切了。 */
    if( pxUnblockedTCB->uxPriority > pxCurrentTCB->uxPriority )
    {
        xReturn = pdTRUE;
        xYieldPendings[ 0 ] = pdTRUE;
    }
    else
    {
        xReturn = pdFALSE;
    }

    return xReturn;
}
```

寄放期间，A 的两个钩子分家了：

```text
xEventListItem  → xPendingReadyList    ← 已经不算「在等队列」了
xStateListItem  → 还留在延时链表        ← 现在动不了，先留着
```

注意这时候 A 的状态钩还挂着原来的唤醒时刻。所以 `eTaskGetState()` 有一条专门的判断：**事件钩落在 `xPendingReadyList` 上，就一律报 `eReady`**，不看状态钩在哪条链表上。

### 收尾：`xTaskResumeAll()`

`uxSchedulerSuspended` 减到 0 时，把寄放盒清空，每个任务补完剩下的动作：

```c
while( listLIST_IS_EMPTY( &xPendingReadyList ) == pdFALSE )
{
    pxTCB = listGET_OWNER_OF_HEAD_ENTRY( ( &xPendingReadyList ) );

    listREMOVE_ITEM( &( pxTCB->xEventListItem ) );   /* 从寄放盒取出 */
    portMEMORY_BARRIER();
    listREMOVE_ITEM( &( pxTCB->xStateListItem ) );   /* 现在才能摘延时链表 */
    prvAddTaskToReadyList( pxTCB );                  /* 真正进就绪桶 */

    if( pxTCB->uxPriority > pxCurrentTCB->uxPriority )
    {
        xYieldPendings[ xCoreID ] = pdTRUE;          /* 记一笔要切 */
    }
}

if( pxTCB != NULL )
{
    /* 挂起期间有人被叫醒，最早唤醒时刻可能已经不准了。 */
    prvResetNextTaskUnblockTime();
}
```

绕完这一圈，做的事和调度器没挂起时**完全一样**，只是分成了两个时间点：事件钩当场摘、状态钩延后摘。

### 一个具体例子

接 [调度器链表](05_freertos之task.c--调度器链表.md) 里那个例子：任务 A 优先级 3，tick 20 调 `xQueueReceive(q, &buf, 80)`，超时时刻 100。

```text
tick 20   A 阻塞
          延时链表          ：A @ 100
          q 的等人名单       ：A (5)

tick 50   某任务 vTaskSuspendAll()，正在改链表
          此时 ISR 往 q 写数据 → xTaskRemoveFromEventList()

          q 的等人名单       ：空          ← 事件钩当场摘掉
          xPendingReadyList ：A 的事件钩   ← 寄放
          延时链表          ：A @ 100 还在 ← 动不了

tick 50   xTaskResumeAll()
          xPendingReadyList ：空
          延时链表          ：空          ← 这时才摘
          就绪链表[3]        ：A
          A 优先级高于当前任务 → 记 xYieldPendings，退出时切过去
```

A 醒来后从 `xQueueReceive()` 里继续跑，队列里有数据，正常返回。那个「@100」始终没派上用场。

### 几个容易混的点

- **挂起调度器 ≠ 关中断。** 中断照常进、照常给事件，只是不做任务切换。所以才需要寄放盒；如果连中断都关了，就不会有人在这期间动名单。
- **寄放盒里放的是事件钩，不是状态钩。** 状态钩此刻还在延时链表上，等 `xTaskResumeAll()` 才摘。
- **只有等外部对象的任务会用到它。** 任务通知不进任何名单，从 ISR 通知一个挂起期间的任务，走的是另一段代码（同样往 `xPendingReadyList` 插事件钩，但不需要先从名单摘）。
- **队列被 lock 时不走这个函数。** 那种情况改的是队列自己的 lock 计数，属于 `queue.c` 的话题。
- 挂起期间丢掉的 tick 记在 `xPendedTicks` 里，`xTaskResumeAll()` 一并补上，见 [Delay](06_freertos之task.c--Delay.md)。

## 待学习

### `vTaskSuspendAll()`、`xTaskResumeAll()` 与临界区的区别

- 挂起调度器与 `taskENTER_CRITICAL()` / 关中断各禁止什么、各不禁止什么
