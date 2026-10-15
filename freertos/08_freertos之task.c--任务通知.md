# FreeRTOS 之 task.c -- 任务通知

通知状态和值在目标任务 TCB 里，等待通知时不加入外部对象的事件等待链表。TCB 字段见 [TCB](03_freertos之task.c--TCB.md)，链表关系见 [调度器链表](05_freertos之task.c--调度器链表.md)。

## 待学习

### 任务通知在 `tasks.c` 中的实现
