# FreeRTOS 之 task.c -- 删除与 Idle

待删除链表 `xTasksWaitingTermination` 的位置见 [调度器链表](05_freertos之task.c--调度器链表.md)。动态创建时 TCB / 栈从哪来，见 [动态创建任务的内存管理](04_freertos之task.c--任务创建与内存管理.md)。

## 待学习

### `vTaskDelete()`、Idle 任务与已删除任务资源回收
