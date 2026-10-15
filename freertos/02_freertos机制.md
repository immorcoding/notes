# FreeRTOS 运行机制

> **适用范围：以下启动和切换汇编以 ARM Cortex-M3/M4/M7 的常规 GCC FreeRTOS port 为例。M0/M0+、MPU port、SMP port 或其他架构的实现及可用寄存器可能不同。**

## 1. 调度器启动与初始任务
从内核入口到首个任务的启动路径大致是 `vTaskStartScheduler()` → `xPortStartScheduler()` → `prvPortStartFirstTask()` → `svc 0` → `vPortSVCHandler()`。下面是逐步的详细流程：
1. **xTaskCreate() 创建；**<br>
   xTaskCreate() 创建时，任务其实已经提前准备好了上下文。<br><br>
   对 `xTaskCreate()` 而言，任务栈这块 RAM 本身是创建时用 `pvPortMalloc()` 从 FreeRTOS 堆（`heap_4.c` 的 `ucHeap`）分配出来的，不是 C 库的 `malloc`，见 [动态创建任务的内存管理](04_freertos之task.c--任务创建与内存管理.md)。移植层的 `pxPortInitialiseStack()` 只在这块缓冲区的高地址端预先伪造初始上下文（包括 PC、LR、xPSR 等寄存器值），不会去动 CPU 的 PSP。**PSP 首次在后面的 `vPortSVCHandler()` 中被设置：先从 TCB 读出 `pxTopOfStack`，恢复 R4～R11 和 EXC_RETURN 后，再把递增后的 R0 写入 PSP；该值指向硬件异常帧，不是 `pxTopOfStack` 的原值。**<br><br>
   整体栈顶结构大致如下：(M3/M4/M7 Cortex-M 系列为例)
   ```
   |-------------------| 高地址
   | xPSR              |
   | PC                |
   | LR                |
   | R12               |
   | R3                |
   | R2                |
   | R1                |
   | R0                |
   |-------------------|
   | EXC_RETURN        |
   | R11               |
   | R10               |
   | R9                |
   | R8                |
   | R7                |
   | R6                |
   | R5                |
   | R4                |
   |-------------------| 低地址
   ```

   此外官方的宏定义如下：
   ```c
   #define portINITIAL_XPSR        0x01000000UL  /* 任务初始 xPSR。bit24(T位)=1，表示 Cortex-M 以 Thumb 状态 (Thumb 是 ARM 指令集机器
                                                    编码的一种) 执行；其余状态位初始化为 0。 */
   #define portINITIAL_EXC_RETURN  0xFFFFFFFDUL  /* 任务初始 EXC_RETURN：异常返回到 Thread Mode，使用 PSP 恢复任务上下文，并使用基本异
                                                    常栈帧（无扩展FPU栈帧）。 */
   ```

2. **内核层：vTaskStartScheduler()：**<br>
   任务都创建完成后，调用 vTaskStartScheduler() 准备启动调度器，从这一刻开始，FreeRTOS 的调度器正式接管 CPU 的执行权，开始按照任务优先级进行任
   务调度。<br><br>
   内核层主要会做以下几件事（顺序按源码）：
   1. 创建空闲任务（Idle Task），该任务在系统中始终处于最低优先级的就绪状态，当没有其他任务可运行时，空闲任务会被调度执行。**Idle 的 TCB 和栈从哪来取决于配置：`configSUPPORT_STATIC_ALLOCATION == 1` 时，`prvCreateIdleTasks()` 一律走 `vApplicationGetIdleTaskMemory()` 拿应用提供的静态缓冲区（CubeMX 工程默认就是这样），不占 `ucHeap`；只有未开静态分配时才 `xTaskCreate()`，此时 Idle 的栈和 TCB 才从 `ucHeap` 申请，位置取决于前面已经分配了多少。**详见 [动态创建任务的内存管理](./04_freertos之task.c--任务创建与内存管理.md)。
   2. 如果启用软件定时器（`configUSE_TIMERS == 1`），`xTimerCreateTimerTask()` 创建定时器服务任务（Timer Service Task），用于管理和执行软件定时器回调函数。同样分静态 / 动态两种：静态时走 `vApplicationGetTimerTaskMemory()`；动态时在 Idle 之后申请，相对位置只取决于此时堆里已经分配了多少。
   3. `portDISABLE_INTERRUPTS()`。这一步在 Idle / Timer 任务创建**之后**，目的是保证进入 `xPortStartScheduler()` 之前和期间不会来一个 tick。第一个任务启动路径里 `prvPortStartFirstTask()` 的 `cpsie i` 和 `vPortSVCHandler()` 的 `basepri = 0` 会重新放开中断。**在此 Cortex-M port 中，它通过 BASEPRI 屏蔽优先级不高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 的中断；这些中断才允许调用 FreeRTOS API。它不是用 PRIMASK 禁止所有可屏蔽中断，高于该阈值的中断仍可响应。**
   ```c
   #define portDISABLE_INTERRUPTS()                  vPortRaiseBASEPRI() // 设置 BASEPRI 中断优先级阈值
   ```
   4. 置调度器状态：`xNextTaskUnblockTime = portMAX_DELAY`、`xSchedulerRunning = pdTRUE`、`xTickCount = configINITIAL_TICK_COUNT`。**就绪链表等结构不是这时才建的**：第一个任务被加入就绪链表时（`prvAddNewTaskToReadyList()` 内 `uxCurrentNumberOfTasks == 1` 的分支，`xTaskCreate()` 与 `xTaskCreateStatic()` 都会走到）`prvInitialiseTaskLists()` 就建好了；`pxCurrentTCB` 也是每次创建任务时被维护成「当前最高优先级的就绪任务」，这里不再重新选。
   5. 调用移植层 `xPortStartScheduler()`，正常情况下不返回；它最后再调 `prvPortStartFirstTask()`。

3. **移植层：xPortStartScheduler()：**<br>
   xPortStartScheduler() 是 FreeRTOS 在不同平台上启动调度器的底层实现函数，通常由 vTaskStartScheduler() 调用，port.c 提供具体实现。其主要
   工作包括：
   1. 设置 PendSV、SysTick、SVC 优先级
   ```c
       /* 设置 PendSV 和 SysTick 为最低优先级，SVC 为最高优先级。 */
       portNVIC_SHPR3_REG |= portNVIC_PENDSV_PRI;
       portNVIC_SHPR3_REG |= portNVIC_SYSTICK_PRI;
       portNVIC_SHPR2_REG = 0;
   ```
   2. 调用 vPortSetupTimerInterrupt() 配置并启动系统滴答定时器，用于周期性产生 SysTick 中断，驱动系统节拍。
   3. uxCriticalNesting = 0，初始化临界区嵌套计数器，表示当前不在临界区。
   4. 初始化 FPU，开启 FPU，并启用浮点上下文的 lazy stacking（如果 Task 使用浮点寄存器，FreeRTOS/Cortex-M 还要额外管理 FPU 上下文）。
   5. 调用 `prvPortStartFirstTask()`。此函数是 `__attribute__((naked))`，表示编译器不会为它生成函数入口和出口的栈操作，整个上下文切换过程由汇编手动管理。**MSP（主栈指针）会被重置为系统启动时的初始值；主栈 RAM 中原有内容并未清零，只是不再由任务执行路径使用。**最后触发 SVC 异常，进入 FreeRTOS 的任务调度流程。<br><br>
   具体如下：
   ```c
   static void prvPortStartFirstTask( void )
   {
       /* 启动第一个任务，同时清除可能已经置位的 FPU 使用标志，避免进入 SVC 时额外为 FPU 上下文预留栈空间。 */
       __asm volatile
       (
           " ldr r0, =0xE000ED08   \n" /* r0 = 0xE000ED08，也就是 VTOR 寄存器本身的地址 */
           " ldr r0, [r0]          \n" /* r0 = *(0xE000ED08)，读出 VTOR 的值，也就是“向量表基地址” */
           " ldr r0, [r0]          \n" /* r0 = *(向量表基地址)，读出向量表第 0 项存的地址，也就是 MSP 的初始值（ld文件规定的_estack） */
           " msr msp, r0           \n" /* 将 MSP 恢复为系统启动时的初始主栈指针。 */
           " mov r0, #0            \n" /* 将 r0 清零，准备写入 CONTROL 寄存器。 */
           " msr control, r0       \n" /* CONTROL = 0：特权模式、Thread 模式使用 MSP，并清除 FPCA 浮点上下文活动标志。 */
           " cpsie i               \n" /* 清除 PRIMASK，重新使能普通可屏蔽中断。 */
           " cpsie f               \n" /* 清除 FAULTMASK，重新使能除 NMI 外的异常。 */
           " dsb                   \n" /* 数据同步屏障，确保前面的寄存器和内存操作全部完成。 */
           " isb                   \n" /* 指令同步屏障，刷新流水线，使前面对 CPU 状态的修改立即生效。 */
           " svc 0                 \n" /* 触发 SVC 0 异常，进入 FreeRTOS 的 vPortSVCHandler()，开始恢复第一个任务。 */
           " nop                   \n" /* 空操作；正常情况下 SVC 异常返回后已经进入第一个任务，通常不会继续执行这里。 */
           " .ltorg                \n" /* 在此处生成 literal pool 常量池，供 ldr r0, =0xE000ED08 之类的伪指令使用。 */
       );
   }
   ```

4. **进入 vPortSVCHandler()：**<br>
   现在的 cpu 触发了 SVC 异常，进入 Handler Mode，SP = MSP，PC = vPortSVCHandler。<br><br>
   这个 vPortSVCHandler 同样是 __attribute__((naked)) 的函数，由汇编手动管理上下文切换，负责恢复第一个任务的上下文并开始任务调度。<br><br>

   vPortSVCHandler 中进行了这些操作：
   1. 从 pxCurrentTCB 获取当前任务的栈顶指针。
   2. 从任务栈中恢复 r4-r11 和 r14(EXC_RETURN)。
   3. 将恢复后的栈顶写入 PSP。
   4. 清除 BASEPRI，允许低优先级中断。
   5. 通过 EXC_RETURN 返回，进入第一个任务。
   
   具体实现如下：
   ```c
   void vPortSVCHandler( void )
   {
       __asm volatile (
           "   ldr r3, =pxCurrentTCB           \n" /* 从常量池读取 pxCurrentTCB 变量本身的地址，r3 = &pxCurrentTCB。 */
           "   ldr r1, [r3]                    \n" /* 读取 pxCurrentTCB 的值，r1 = pxCurrentTCB，即当前任务 TCB 的地址。 */
           "   ldr r0, [r1]                    \n" /* 读取 TCB 的第一个成员 pxTopOfStack，r0 = 当前任务保存的栈顶地址。 */
           "   ldmia r0!, {r4-r11, r14}        \n" /* 从任务栈依次恢复 r4-r11 和 r14(EXC_RETURN)，同时 r0 自动向高地址移动。 */
           "   msr psp, r0                     \n" /* 将恢复后的栈顶写入 PSP，此时 PSP 指向由硬件自动保存的异常栈帧。 */
           "   isb                             \n" /* 指令同步屏障，保证 PSP 等寄存器的修改对后续指令立即生效。 */
           "   mov r0, #0                      \n" /* r0 清零，准备清除 BASEPRI 中断优先级屏蔽。 */
           "   msr basepri, r0                 \n" /* BASEPRI = 0，取消 FreeRTOS 对低优先级中断的屏蔽。 */
           "   bx r14                          \n" /* 跳转到 r14 中的 EXC_RETURN，触发异常返回，硬件自动从 PSP 恢复剩余寄存器进入任务。*/
           "                                   \n"
           "   .ltorg                          \n" /* 让汇编器在此处放置常量池，供上面 ldr =pxCurrentTCB 取地址用。 */
       );
   }
   ```
   旧版本这里写成 `ldr r3, pxCurrentTCBConst2` 加一行 `pxCurrentTCBConst2: .word pxCurrentTCB`，手工定义常量；V11 改为 `ldr =符号` 由汇编器自动生成常量池，两者语义相同。

5. **退出 vPortSVCHandler：**<br>
   当第一个任务的上下文恢复完成后，vPortSVCHandler 通过 EXC_RETURN 返回，CPU 进入 Thread Mode，此时硬件会自动从 PSP 中恢复 **R0 ~ R3、R12、LR、PC、xPSR** 寄存器，任务开始执行。


## 2. 运行时上下文切换
实时操作系统在 Cortex-M 平台上通常使用 **PSP（Process Stack Pointer）作为任务运行时的栈指针**，而 **MSP（Main Stack Pointer）主要用于异常和中断处理程序**。<br>
关于 Cortex-M 指令集寄存器的定义见 [01_指令集寄存器.md](../内核底层/01_指令集寄存器.md)。<br>
上下文切换是指在多任务环境下，操作系统将 CPU 的执行权从一个任务切换到另一个任务的过程。以任务 A 切换到任务 B 为例，主要过程如下：<br>

1. **保存当前任务 A 的上下文：** <br>
   当发生任务切换时，系统触发 **PendSV 异常**。此时任务 A 运行在 Thread Mode，并使用 **PSP** 作为任务栈指针。Cortex-M 进入异常时，硬件首先自动将 **R0 ~ R3、R12、LR、PC、xPSR** 压入任务 A 的任务栈。由于 Cortex-M 的栈采用 **向低地址方向生长（Full Descending）** 的方式，因此压栈过程中 **PSP** 的值不断减小。<br><br>
   进入 `PendSV_Handler` 后，CPU 进入 Handler Mode，此时异常处理程序自身使用 **MSP**，但 ***任务 A 的 **PSP** 仍然保存着任务 A 当前任务栈的位置***。随后 FreeRTOS 通过软件继续将 **R4 ~ R11 以及 EXC_RETURN(此时的 EXC_RETURN 被 LR 保存)** 压入任务 A 的任务栈。<br><br>
   上下文保存完成后，**R0 模拟的 PSP** 指向当前已保存完整上下文的最低地址，FreeRTOS 将该 **R0** 中保存的任务栈顶地址写入当前任务 TCB 的 pxTopOfStack。此时真正的 PSP 寄存器仍指向 Cortex-M 硬件自动生成的异常栈顶位置。<br>
   **而为什么不用直接使用 PSP 呢**???<br><br>
   核心原因就是 **PSP** 属于特殊寄存器，不能像 **R0 ~ R12** 这种通用寄存器一样直接拿来做 STMDB/LDMIA 的基址操作；而且 PendSV 处于 Handler Mode，此时 SP/R13 实际对应 **MSP**，此时使用的是 **MSP** 指向的栈空间，而不是 **PSP**。<br><br>
   
   >此外关于 EXC_RETURN 作用有以下三点：
   >1. 指示异常返回时使用的栈指针（PSP 或 MSP）。
   >2. 指示返回到 Thread Mode 还是 Handler Mode。
   >3. 指示是否需要恢复浮点寄存器上下文。

   官方的具体操作如下：
   ```c
   void xPortPendSVHandler( void )
   {
       /* naked 函数：编译器不会自动生成函数入栈/出栈代码，整个现场由下面的汇编手动管理。 */
       __asm volatile
       (
           "   mrs r0, psp                         \n" /* 将当前任务的 PSP 栈顶地址读取到 R0，后续使用 R0 作为任务栈指针的工作副本。 */
           "   isb                                 \n" /* 指令同步屏障，确保前面对处理器状态的操作完成后再继续执行后续指令。*/
           "                                       \n"
           "   ldr r3, =pxCurrentTCB               \n" /* R3 = &pxCurrentTCB，即取得“当前任务 TCB 指针变量”本身的地址。 */
           "   ldr r2, [r3]                        \n" /* R2 = pxCurrentTCB，即取得当前任务 A 的 TCB 地址。 */
           "                                       \n"
           "   tst r14, #0x10                      \n" /* 检查 EXC_RETURN 的 bit4，判断当前任务是否存在需要保存的浮点上下文。 */
           "   it eq                               \n" /* 如果上面的测试结果为 0（EQ），则执行下一条带 EQ 条件的指令。 */
           "   vstmdbeq r0!, {s16-s31}             \n" /* 若任务使用 FPU，则将 S16~S31 压入任务 PSP 栈；R0 同时向低地址移动。 */
           "                                       \n"
           "   stmdb r0!, {r4-r11, r14}            \n" /* 将 R4~R11 和 R14 压入任务栈；此时 R14 保存 EXC_RETURN。R0 继续向低地址移动。 */
           "   str r0, [r2]                        \n" /* 将完整上下文保存后的新栈顶地址 R0 写入当前任务 TCB 的pxTopOfStack。 */
           "                                       \n"
           "   stmdb sp!, {r0, r3}                 \n" /* 将 R0、R3 临时压入当前 Handler 的 MSP 栈，因为接下来调用 C 函数可能破坏它们。 */
           "   mov r0, %0                          \n" /* R0 = configMAX_SYSCALL_INTERRUPT_PRIORITY，准备设置 BASEPRI。
                                                          %0 不是 ARM 汇编本身的语法，而是 GCC 内联汇编的第 0 个输入/输出操作数占位符。
                                                          这里对应于 configMAX_SYSCALL_INTERRUPT_PRIORITY。下面有写。*/
           "   msr basepri, r0                     \n" /* 设置 BASEPRI，屏蔽优先级不高于 FreeRTOS 系统调用阈值的中断，保护调度器临界区。*/
           "   dsb                                 \n" /* 数据同步屏障，确保前面的内存/系统状态操作已经完成。 */
           "   isb                                 \n" /* 指令同步屏障，确保新的 BASEPRI 屏蔽状态立即作用于后续指令。 */
           "   bl vTaskSwitchContext               \n" /* 调用任务调度器，选择下一个要运行的任务，并修改 pxCurrentTCB 指向任务 B。 */
           "   mov r0, #0                          \n" /* R0 = 0，准备清除 BASEPRI。 */
           "   msr basepri, r0                     \n" /* BASEPRI = 0，解除 FreeRTOS 临界区的中断优先级屏蔽。 */
                                                       /* 为什么要先开启优先级屏蔽后又解除呢？*/
                                                       /* 先设置 BASEPRI(BASEPRI = configMAX_SYSCALL_INTERRUPT_PRIORITY)，屏蔽优先级不
                                                        * 高于 FreeRTOS 系统调用阈值(configMAX_SYSCALL_INTERRUPT_PRIORITY)的中断，此时
                                                        * 进入调度器临界区。vTaskSwitchContext()会修改 FreeRTOS 调度器内部数据，并修改 
                                                        * pxCurrentTCB 指向下一个要运行的任务，然后退出调度器临界区(BASEPRI 恢复为 0)。*/
           "   ldmia sp!, {r0, r3}                 \n" /* 从 MSP 栈恢复之前临时保存的 R0 和 R3；R3 仍然是 &pxCurrentTCB。*/
           "                                       \n"
           "   ldr r1, [r3]                        \n" /* R1 = pxCurrentTCB。由于调度已经完成，此时取得的是任务 B 的 TCB 地址。 */
           "   ldr r0, [r1]                        \n" /* R0 = Task B 的 pxTopOfStack，即取得任务 B 保存上下文的栈顶地址。*/
           "                                       \n"
           "   ldmia r0!, {r4-r11, r14}            \n" /* 从任务 B 的栈恢复 R4~R11 和 EXC_RETURN 到 R14；R0 向高地址移动。*/
           "                                       \n"
           "   tst r14, #0x10                      \n" /* 再次检查任务 B 的 EXC_RETURN bit4，判断任务 B 是否具有浮点上下文。 */
           "   it eq                               \n" /* 如果 bit4 = 0，则执行下一条浮点寄存器恢复指令。 */
           "   vldmiaeq r0!, {s16-s31}             \n" /* 若任务 B 使用 FPU，则从任务栈恢复 S16 ~ S31；R0 继续向高地址移动。*/
           "                                       \n"
           "   msr psp, r0                         \n" /* 将恢复软件上下文后的 R0 写回 PSP，此时 PSP 指向任务 B 的硬件异常帧 R0 位置。 */
           "   isb                                 \n" /* 指令同步屏障，确保 PSP 的更新对后续异常返回立即生效。 */
           "                                       \n"
           #ifdef WORKAROUND_PMU_CM001                 /* XMC4000 Cortex-M4 特定芯片勘误 PMU_CM001 的规避代码。 */
           #if WORKAROUND_PMU_CM001 == 1
                   "   push { r14 }                \n" /* 将 EXC_RETURN 临时压入当前 Handler 使用的 MSP 栈。 */
                   "   pop { pc }                  \n" /* 将 EXC_RETURN 弹入 PC，以规避该芯片的特定异常返回硬件问题。 */
           #endif
           #endif
           "                                       \n"
           "   bx r14                              \n" /* 跳转到 R14 中的 EXC_RETURN，触发 Cortex-M Exception Return。 */
                                                       /* 硬件随后从 PSP 自动恢复 R0 ~ R3、R12、LR、PC、xPSR，并返回任务 B。 */
           "                                       \n"
           "   .ltorg                              \n" /* 在当前位置生成 literal pool，供 ldr rX, =xxx 等伪指令加载常量/地址使用。 */
           ::"i" ( configMAX_SYSCALL_INTERRUPT_PRIORITY ) /* 将该宏作为立即数操作数 %0 传给内联汇编。 */
       );
   }
   ```

2. **恢复下一个任务 B 的上下文：**<br>
   调度器确定下一个需要运行的是任务 B 后，先从任务 B 对应的 TCB 中读取此前保存的栈顶指针 `pxTopOfStack` 到 R0。**此时 R0 指向完整上下文的最低地址，即软件保存区的 R4 槽。**FreeRTOS 随后从低地址向高地址恢复 R4～R11 和 EXC_RETURN，R0 随之增大；接着执行 `msr psp, r0`，**此时 PSP 才指向硬件异常帧的 R0 槽，而不是 `pxTopOfStack` 原先指向的位置。**最后异常返回，Cortex-M 硬件从 PSP 指向的任务栈恢复 R0～R3、R12、LR、PC、xPSR。<br><br>
   因此可以记为：***保存上下文时 **PSP** 向低地址移动，恢复上下文时 **PSP** 向高地址移动。***<br><br>

3. **继续执行任务 B：** <br>
   PC（Program Counter）同样属于 Cortex-M 硬件自动保存的异常上下文之一。任务 B 之前被切出时，其 PC 已经被保存到任务 B 的任务栈中；任务 B 再次被调度运行时，异常返回过程会自动恢复 PC，因此 CPU 会重新回到任务 B 原先被中断的位置继续执行。***从任务 B 自身的视角来看，它通常并不知道自己曾经被切换出去过。***
<br><br>

### 上下文切换整体流程
```text
Task A 运行（Thread Mode，使用 PSP）
        ↓
触发 PendSV
        ↓
Cortex-M 硬件自动压栈 R0~R3、R12、LR、PC、xPSR，PSP 向低地址移动
举例
高地址
0x20000FFC  xPSR
0x20000FF8  PC
0x20000FF4  LR
0x20000FF0  R12
0x20000FEC  R3
0x20000FE8  R2
0x20000FE4  R1
0x20000FE0  R0   ← PSP
低地址
        ↓
进入 PendSV_Handler（Handler Mode，使用 MSP）
        ↓
FreeRTOS 软件保存 R4~R11 以及 EXC_RETURN，
官方使用 R0 保存 PSP，R0 继续向低地址移动，R0 现在模拟的 PSP 指针
举例
高地址
0x20000FDC  EXC_RETURN
0x20000FD8  R11
0x20000FD4  R10
0x20000FD0  R9
0x20000FCC  R8
0x20000FC8  R7
0x20000FC4  R6
0x20000FC0  R5
0x20000FBC  R4   ← R0 模拟 PSP
低地址
        ↓
将 R0 保存到 Task A 的 TCB，此时R0 中保存的就是任务 A 的栈顶指针（模拟的 PSP）。
        ↓
调度器选择 Task B
        ↓
从 Task B 的 TCB 读取栈顶指针，恢复为 R0（模拟的 PSP）。
        ↓
FreeRTOS 软件恢复 R4~R11 及 EXC_RETURN ，R0 模拟的 PSP 向高地址移动
        ↓
执行异常返回
        ↓
Cortex-M 硬件自动恢复 R0~R3、R12、LR、PC、xPSR，PSP 向高地址移动
        ↓
恢复 Task B 的 PC
        ↓
Task B 继续运行
```
最容易混淆的四点：
1. ***任务上下文主要保存在任务栈中，TCB 主要保存任务栈顶指针 pxTopOfStack。***
2. ***PendSV_Handler 自身使用 **MSP**，但它保存和恢复的是任务对应的 **R0 模拟的 PSP** 栈。***
3. ***压栈时 **PSP** 向低地址移动，恢复时 **PSP** 向高地址移动。***
4. ***正在运行的任务，真正跟着压栈变化的是 PSP。`pxTopOfStack` 只在切出时由 PendSV 写入；运行中不会每压一次栈就改。未在跑的任务，PSP 不指向它们的栈，它们的栈顶只记在各自的 `pxTopOfStack` 里。***