/**
  ******************************************************************************
  * @file    FreeRTOSConfig.h
  * @brief   本项目的 FreeRTOS 内核配置。
  *
  * @details
  *          本文件属于 FreeRTOS 与当前固件之间的项目配置接缝，而不是第三方
  *          内核源码。FreeRTOS Kernel 通过 CMake 提供的 Config include path
  *          读取本文件。
  ******************************************************************************
  */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* 调度器、Tick、任务基础能力与对象数量配置。 */
#define configUSE_PREEMPTION                                        1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION                     0
#define configCPU_CLOCK_HZ                                          480000000
#define configTICK_RATE_HZ                                          1000
#define configMAX_PRIORITIES                                        5
#define configMINIMAL_STACK_SIZE                                    128
#define configMAX_TASK_NAME_LEN                                     16
#define configUSE_16_BIT_TICKS                                      0
#define configIDLE_SHOULD_YIELD                                     1
#define configUSE_TASK_NOTIFICATIONS                                1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES                       3

/*
 * 任务通知索引是“每个任务各自拥有”的资源，不同任务可复用同一索引值。
 * Storage 的索引 0 用于卡检测消抖，索引 1 用于 SD DMA 完成；两者独立，避免
 * DMA 完成被误解释为热插拔边沿。GUI Task 的索引 0 是其独立的 SPI DMA 结果。
 */
#define FREERTOS_NOTIFY_INDEX_STORAGE_SD_DETECT                     0U
#define FREERTOS_NOTIFY_INDEX_STORAGE_SD_TRANSFER                   1U

#define FREERTOS_NOTIFY_INDEX_GUI_LCD_TRANSFER                      0U

#define configUSE_MUTEXES                                           1
#define configUSE_RECURSIVE_MUTEXES                                 0
#define configUSE_COUNTING_SEMAPHORES                               1
#define configUSE_ALTERNATIVE_API                                   0 /* Deprecated! */
#define configQUEUE_REGISTRY_SIZE                                   10
#define configUSE_QUEUE_SETS                                        0
#define configUSE_TIME_SLICING                                      1
#define configUSE_NEWLIB_REENTRANT                                  0
#define configENABLE_BACKWARD_COMPATIBILITY                         0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS                     1
#define configUSE_MINI_LIST_ITEM                                    1
#define configSTACK_DEPTH_TYPE                                      uint16_t
#define configMESSAGE_BUFFER_LENGTH_TYPE                            size_t
#define configHEAP_CLEAR_MEMORY_ON_FREE                             0

/* 动态/静态内存分配策略；当前由 heap_4.c 提供 32 KiB 内核堆。 */
#define configSUPPORT_STATIC_ALLOCATION                             0
#define configSUPPORT_DYNAMIC_ALLOCATION                            1
#define configTOTAL_HEAP_SIZE                                       32768
#define configAPPLICATION_ALLOCATED_HEAP                            0
#define configSTACK_ALLOCATION_FROM_SEPARATE_HEAP                   0

/* Hook 函数和运行期故障检测；启用后必须提供对应函数实现。 */
#define configUSE_IDLE_HOOK                                         0
#define configUSE_TICK_HOOK                                         0
#define configCHECK_FOR_STACK_OVERFLOW                              2
#define configUSE_MALLOC_FAILED_HOOK                                0
#define configUSE_DAEMON_TASK_STARTUP_HOOK                          0
#define configUSE_SB_COMPLETED_CALLBACK                             0

/* 运行时间统计和任务状态查询支持。 */
#define configUSE_TRACE_FACILITY                                    1
#define configUSE_STATS_FORMATTING_FUNCTIONS                        1

/* debug期间保留即可 */
#define configRECORD_STACK_HIGH_ADDRESS                             1
#define configGENERATE_RUN_TIME_STATS                               1

void RuntimeStatsTimer_Start(void);
uint32_t RuntimeStatsTimer_GetCount(void);

#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()                    RuntimeStatsTimer_Start()
#define portGET_RUN_TIME_COUNTER_VALUE()                            RuntimeStatsTimer_GetCount()

/* 已弃用的 Co-routine 功能；新代码使用普通 Task。 */
#define configUSE_CO_ROUTINES                                       0
#define configMAX_CO_ROUTINE_PRIORITIES                             1

/* 软件定时器及其守护任务配置。 */
#define configUSE_TIMERS                                            1
#define configTIMER_TASK_PRIORITY                                   3
#define configTIMER_QUEUE_LENGTH                                    10
#define configTIMER_TASK_STACK_DEPTH                                configMINIMAL_STACK_SIZE

void vAssertCalled(const char *file, uint32_t line);

/* 开发期断言：保存文件和行号后进入不可恢复停机流程。 */
#define configASSERT(x)                                                    \
    do                                                                     \
    {                                                                      \
        if ((x) == 0)                                                      \
        {                                                                  \
            vAssertCalled(__FILE__, (uint32_t)__LINE__);                   \
        }                                                                  \
    } while (0)

/* Cortex-M7 NVIC 优先级位数和 FreeRTOS 可调用 FromISR API 的阈值。 */
#define configPRIO_BITS                                     4U
/* 数值最大的 NVIC 优先级，也就是逻辑上的最低抢占优先级。 */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY             0xF

/*
 * 允许调用 xxxFromISR() 的最高逻辑中断优先级。数值小于 5 的 ISR 优先级
 * 更高，禁止调用任何 FreeRTOS API。
 */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY        5

/* 把 CMSIS 风格的 4 位优先级转换成写入 Cortex-M 寄存器的左对齐值。 */
#define configKERNEL_INTERRUPT_PRIORITY                     ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )
/* 该值绝对不能为 0，否则 BASEPRI 无法屏蔽内核可管理的中断。 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY                ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) )

/* 把 FreeRTOS Port 层处理函数映射到启动文件使用的 CMSIS 异常入口名称。 */
#define vPortSVCHandler                                     SVC_Handler
#define xPortPendSVHandler                                  PendSV_Handler
#define xPortSysTickHandler                                 SysTick_Handler

#define INCLUDE_vTaskDelay                                  1
#define INCLUDE_vTaskDelete                                 1

#endif /* FREERTOS_CONFIG_H */
