# 前置工作 #
## 1. FreeRTOS 介绍 ##
FreeRTOS 是一个开源的实时操作系统 (RTOS)，它为嵌入式系统提供了多任务调度、任务管理、时间管理、内存管理和中断处理等功能。FreeRTOS 具有小巧、可移植、易于使用和高效的特点，广泛应用于各种嵌入式设备中。

## 2. FreeRTOS 的移植 ##
具体的目录分级需求参照官网要求：https://www.freertos.org/zh-cn-cmn-s/Documentation/01-FreeRTOS-quick-start/01-Beginners-guide/03-Build-your-first-project

一个最小的 FreeRTOS 移植需要内核源码、与编译器/架构匹配的移植层，以及启动代码。下列是常见组成；**软件定时器和堆实现是否需要，取决于启用的功能和内存分配方式。**

目录：
- portable：包含与特定硬件平台相关的移植层代码。
 - 目录下包括
   - GCC 或其他编译器目录：GCC 或其他编译器相关的移植层代码。
   - ARM_CM4F 或其他架构目录：针对 ARM Cortex-M4/M7 架构或其他架构的移植层代码。
- include：包含 FreeRTOS 的头文件。

文件：
- FreeRTOSConfig.h：配置文件，用于定义 FreeRTOS 的各种参数和选项。
- tasks.c：实现任务管理功能的源文件。
- list.c：实现链表管理功能的源文件。
- queue.c：实现队列、信号量和互斥锁等功能的源文件。
- `portable/<compiler>/<arch>/port.c` 与对应的 `portmacro.h`：实现上下文切换、Tick 等架构相关功能。
- 启动文件、向量表和链接脚本：提供复位入口、中断入口和内存布局。
- `heap_x.c`：**仅动态创建任务或使用 FreeRTOS 动态内存分配时需要。**
- timers.c：**仅启用软件定时器时需要。**

其他的文件按需求加入，移植时候详细阅读目录内的说明文件。