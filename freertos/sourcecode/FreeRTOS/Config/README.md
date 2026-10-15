# FreeRTOS Project Configuration

本目录保存本项目对 FreeRTOS Kernel 的配置、Hook 与断言处理，是第三方 Kernel 的唯一自维护接缝。

## 可维护文件

- `FreeRTOSConfig.h`：调度、任务诊断、Hook 和中断优先级配置；
- `freertos_hooks.c`：栈溢出、空闲任务等 Hook；
- `freertos_assert.c`：断言与故障停机策略。

## 约束

- 不修改相邻 `Source` 内核实现或 Cortex-M Port；
- Hook 不承载 APP、文件系统或硬件业务策略；
- 调整 `configMAX_SYSCALL_INTERRUPT_PRIORITY`、栈检查或任务统计宏时，同步更新任务/ISR README 与验证步骤。

## 任务通知索引

`FREERTOS_NOTIFY_INDEX_*` 是项目维护的任务通知资源命名。索引属于每个 Task 的通知数组，
并非全系统唯一编号：Storage Task 使用索引 0 表示卡检测、索引 1 表示 SD DMA；GUI Task 也可独立使用索引 0 表示 SPI DMA 最终完成或错误。新增通知必须按“任务所有者 + 事件语义”命名，不能因为数值相同而将不同任务的事件混为一谈。
