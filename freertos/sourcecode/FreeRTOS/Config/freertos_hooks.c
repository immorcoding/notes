/**
  ******************************************************************************
  * @file    freertos_hooks.c
  * @brief   本项目启用的 FreeRTOS Hook 函数。
  ******************************************************************************
  */

#include "Middlewares/Third_Party/FreeRTOS/Source/include/FreeRTOS.h"
#include "Middlewares/Third_Party/FreeRTOS/Source/include/task.h"

/**
  * @brief  FreeRTOS 检测到任务栈溢出时的停机 Hook。
  * @param  task 发生溢出的任务句柄。
  * @param  task_name 发生溢出的任务名称。
  * @retval None
  */
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    (void)task_name;

    taskDISABLE_INTERRUPTS();

    while (1)
    {
    }
}
