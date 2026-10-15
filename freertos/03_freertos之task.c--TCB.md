# FreeRTOS 之 task.c -- TCB

## 已在其他笔记中记录的内容

- 任务创建时的初始 CPU 上下文、调度器启动和首个任务恢复：[FreeRTOS 运行机制：调度器启动与初始任务](02_freertos机制.md#1-调度器启动与初始任务)
- PendSV 上下文保存与恢复、`pxCurrentTCB` 和 `pxTopOfStack` 的作用：[FreeRTOS 运行机制：运行时上下文切换](02_freertos机制.md#2-运行时上下文切换)
- 任务函数代码位置、任务栈、函数栈帧及任务栈余量检查：[其他 FreeRTOS 相关](其他FreeRTOS相关.md)
- 任务状态、就绪链表、最高优先级选择和 API 唤醒高优先级任务：[其他 FreeRTOS 相关](其他FreeRTOS相关.md)
- 动态创建时 TCB 与任务栈的堆分配：[任务创建与内存管理](04_freertos之task.c--任务创建与内存管理.md)
- 就绪 / 延时 / 挂起 / 待删除 / 待就绪等调度器链表：[调度器链表](05_freertos之task.c--调度器链表.md)
- 延时 API、`vTaskDelay()` / `vTaskDelayUntil()`：[Delay](06_freertos之task.c--Delay.md)
- 尚未单独成篇的 tasks.c 主题：[待学习](00_待学习.md)
- Cortex-M 的通用寄存器、PSP/MSP、LR、PC、xPSR 与中断屏蔽寄存器：[指令集寄存器](../内核底层/01_指令集寄存器.md)
- Thread Mode、Handler Mode 与 FreeRTOS 任务状态的区别：[程序运行相关](../内核底层/02_程序运行相关.md)
- Cortex-M 中断响应、异常栈帧和 PendSV 中的 R4～R11 保存与恢复：[中断相关](../内核底层/03_中断相关.md)

## TCB 任务控制块
TCB（Task Control Block）是 FreeRTOS 中用于管理任务的核心数据结构。每个任务在创建时都会对应一个 TCB，其中包含了任务的状态、优先级、堆栈信息以及任务调度所需的其他信息。<br><br>

### TCB 的存储位置

TCB 不存放在任务栈中，而是任务栈之外独立的一块内存。`TaskHandle_t` 本质上就是对应 TCB 的指针；TCB 再通过 `pxTopOfStack` 和 `pxStack` 记录任务栈的位置。

```text
TaskHandle_t
    ↓
   TCB
   ├── xStateListItem / xEventListItem
   ├── pxTopOfStack ──→ 任务当前栈顶
   └── pxStack      ──→ 任务栈内存区域
```

- 使用 `xTaskCreate()` 动态创建时，TCB 和任务栈的存储均来自 FreeRTOS 堆；它们实际位于哪一块 RAM，由 `heap_x.c`、链接脚本和内存分区配置决定。细节见 [动态创建任务的内存管理](04_freertos之task.c--任务创建与内存管理.md)。
- 使用 `xTaskCreateStatic()` 静态创建时，应用分别提供 `StaticTask_t` 类型的 TCB 缓冲区和 `StackType_t` 类型的任务栈缓冲区；它们的位置由这两个变量所在的内存段决定。

### pxTopOfStack 与 pxStack

不要把这两个指针和 CPU 的 PSP 混在一起：

- `pxTopOfStack`：当前有效栈顶（最后一个已压入数据的位置）。任务被切出去时，PendSV 把保存完上下文后的地址写进这里；切回来时再读出来恢复。
- `pxStack`：**向下生长栈的缓冲区低地址端（malloc 返回的数组基址）**；初始栈顶在缓冲区高地址端。它用来做栈溢出和余量检查，不是当前栈顶。
- 正在跑的任务：真正跟着压栈变化的是 CPU 的 **PSP**。`pxTopOfStack` 只在切出时更新，运行中不会每压一次栈就改。未运行任务的栈顶只记在各自的 `pxTopOfStack` 里，PSP 不指向它们。

PSP / MSP 与切出切回的过程见 [FreeRTOS 运行机制：运行时上下文切换](02_freertos机制.md#2-运行时上下文切换)。链表如何从节点经 `pvOwner` 走到这块 TCB，见 [调度器链表](05_freertos之task.c--调度器链表.md)。

在freertos中，TCB 的定义如下：
```c
/*
 * 任务控制块（Task Control Block，TCB）。
 * 每创建一个任务，都会为该任务分配一个 TCB。
 * TCB 用于保存任务的状态信息，其中包括指向任务上下文的指针。
 * 任务上下文即任务的运行环境，其中包括寄存器值等信息。
 */
typedef struct tskTaskControlBlock       /* 使用旧的命名方式，以避免破坏支持 FreeRTOS 内核感知的调试器兼容性。 */
{
    volatile StackType_t * pxTopOfStack;    /**< 指向任务栈中最后一个已压入数据的位置，也就是当前任务栈顶。该成员必须是 TCB 的第一个成员。 */

    #if ( portUSING_MPU_WRAPPERS == 1 )     /* 如果当前移植层使用 MPU（Memory Protection Unit，内存保护单元）封装。 */
        xMPU_SETTINGS xMPUSettings;         /**< 保存该任务的 MPU 配置信息，由具体的 port 移植层定义。该成员必须是 TCB 的第二个成员。 */
    #endif

    #if ( configUSE_CORE_AFFINITY == 1 ) && ( configNUMBER_OF_CORES > 1 )    /* 多核系统并且启用了任务 CPU 核亲和性功能。 */
        UBaseType_t uxCoreAffinityMask;     /**< CPU 核亲和性掩码，用于限制该任务允许在哪些 CPU 核上运行；其位数必须能够覆盖 
                                                 configNUMBER_OF_CORES。 */
    #endif

    ListItem_t xStateListItem;              /**< 任务的“状态链表项”，用于将任务挂入 Ready、Blocked、Suspended 等状态链表，从所在链表可以判
                                                 断任务当前状态。 */
    ListItem_t xEventListItem;              /**< 任务的“事件链表项”，用于将任务挂入某个事件等待链表，例如等待队列、信号量、互斥量等。 */
    UBaseType_t uxPriority;                 /**< 任务当前优先级，数值 0 为最低优先级。 */
    StackType_t * pxStack;                  /**< 指向任务栈的起始位置，用于记录该任务栈所使用的内存区域。 */

    #if ( configNUMBER_OF_CORES > 1 )       /* 只有在多核 FreeRTOS 中才需要以下成员。 */
        volatile BaseType_t xTaskRunState;  /**< 记录任务当前运行在哪个 CPU 核上；如果任务没有运行，则用于表示未运行、正在让出 CPU 等运行状
                                                 态。 */
        UBaseType_t uxTaskAttributes;       /**< 保存任务属性，目前主要用于标识 Idle Task 等特殊任务。 */
    #endif

    char pcTaskName[ configMAX_TASK_NAME_LEN ];    /**< 创建任务时指定的任务名称，主要用于调试和任务状态查看，本身不参与调度。 */

    #if ( configUSE_TASK_PREEMPTION_DISABLE == 1 )    /* 如果启用了针对单个任务关闭抢占的功能。 */
        BaseType_t xPreemptionDisable;               /**< 用于标记当前任务是否禁止被其他任务抢占。 */
    #endif

    #if ( ( portSTACK_GROWTH > 0 ) || ( configRECORD_STACK_HIGH_ADDRESS == 1 ) )    /* 如果栈向高地址增长，或者要求记录任务栈最高地址。 */
        StackType_t * pxEndOfStack;                  /**< 指向任务栈最高的有效地址，用于栈边界管理和栈检查。 */
    #endif

    #if ( portCRITICAL_NESTING_IN_TCB == 1 )         /* 如果当前移植层选择在 TCB 中保存临界区嵌套计数。 */
        UBaseType_t uxCriticalNesting;               /**< 保存该任务进入临界区的嵌套深度，用于没有在 port 层单独维护临界区嵌套计数的架构。 */
    #endif

    #if ( configUSE_TRACE_FACILITY == 1 )            /* 如果启用了 FreeRTOS Trace 调试/跟踪功能。 */
        UBaseType_t uxTCBNumber;                     /**< 每创建一个 TCB 就递增的编号，可帮助调试器判断某个任务是否被删除后又重新创建。 */
        UBaseType_t uxTaskNumber;                    /**< 专门提供给第三方 Trace 工具使用的任务编号。 */
    #endif

    #if ( configUSE_MUTEXES == 1 )                   /* 如果启用了互斥量功能。 */
        UBaseType_t uxBasePriority;                  /**< 任务原本的基础优先级，用于优先级继承机制，在解除继承后恢复任务原优先级。 */
        UBaseType_t uxMutexesHeld;                   /**< 记录当前任务持有的互斥量数量，用于辅助优先级继承/解除继承机制。 */
    #endif

    #if ( configUSE_APPLICATION_TASK_TAG == 1 )      /* 如果启用了应用层 Task Tag 功能。 */
        TaskHookFunction_t pxTaskTag;                /**< 指向该任务关联的 Hook 函数，可由应用程序自定义使用。 */
    #endif

    #if ( configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0 )    /* 如果为每个任务配置了线程局部存储指针。 */
        void * pvThreadLocalStoragePointers[ configNUM_THREAD_LOCAL_STORAGE_POINTERS ];    /**< 每个任务独立拥有的线程局部存储指针数组，
                                                                                                可用于保存任务私有数据。 */
    #endif

    #if ( configGENERATE_RUN_TIME_STATS == 1 )       /* 如果启用了任务运行时间统计功能。 */
        configRUN_TIME_COUNTER_TYPE ulRunTimeCounter;    /**< 累计记录该任务处于 Running 状态所消耗的运行时间。 */
    #endif

    #if ( configUSE_C_RUNTIME_TLS_SUPPORT == 1 )     /* 如果启用了 C Runtime 的线程局部存储 TLS 支持。 */
        configTLS_BLOCK_TYPE xTLSBlock;              /**< 当前任务对应的 C Runtime Thread Local Storage（TLS）内存块。 */
    #endif

    #if ( configUSE_TASK_NOTIFICATIONS == 1 )        /* 如果启用了 FreeRTOS 任务通知功能。 */
        volatile uint32_t ulNotifiedValue[ configTASK_NOTIFICATION_ARRAY_ENTRIES ];    /**< 保存任务通知值；每个通知索引都有一个独立的 32 位通知值。 */
        volatile uint8_t ucNotifyState[ configTASK_NOTIFICATION_ARRAY_ENTRIES ];       /**< 保存每个任务通知项当前的通知状态，例如是否正在等待通知、是否已经收到通知。 */
    #endif

    /*
     * 关于 tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE 的详细说明，
     * 参见 FreeRTOS.h 中该宏定义附近的注释。
     */
    #if ( tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE != 0 )    /* 如果系统同时存在静态创建任务和动态创建任务的可能。 */
        uint8_t ucStaticallyAllocated;                         /**< 标记该任务是否由静态内存创建，防止删除任务时错误地释放由用户提供的静态
                                                                    内存。 */
    #endif

    #if ( INCLUDE_xTaskAbortDelay == 1 )       /* 如果启用了 xTaskAbortDelay() 功能。 */
        uint8_t ucDelayAborted;                /**< 标记任务的阻塞/延时状态是否是被 xTaskAbortDelay() 强制终止的。 */
    #endif

    #if ( configUSE_POSIX_ERRNO == 1 )         /* 如果启用了 FreeRTOS 对 POSIX errno 的每任务独立支持。 */
        int iTaskErrno;                        /**< 每个任务独立保存自己的 errno，防止不同任务之间共享 errno 产生相互影响。 */
    #endif

} tskTCB;                                      /* 定义任务控制块结构体类型 tskTCB。 */
```
补充说明：<br>
- MPU 配置：CubeMX 生成的 MPU_Config() 是对 Cortex-M7 硬件 MPU 进行系统级配置，通常用于规定某段内存的访问权限、是否允许执行，以及 Cacheable、Bufferable、Shareable 等内存属性；而 TCB_t 中的 xMPUSettings 属于 FreeRTOS MPU Port，用于保存每个任务各自的 MPU 访问配置，任务切换时可以随任务切换 MPU 权限，实现任务之间的内存隔离。两者使用的是同一个硬件 MPU，但用途和管理层级不同。
- CPU 核亲和性：uxCoreAffinityMask 用位掩码指定任务允许在哪些 CPU 核上运行，主要用于多核 FreeRTOS，H743 单核无需关注。
