/**
  ******************************************************************************
  * @file    freertos_assert.c
  * @brief   FreeRTOS configASSERT() 的项目级停机处理。
  ******************************************************************************
  */

#include <stdint.h>

#include "stm32h7xx.h"

/** @brief 最近一次触发 configASSERT 的源文件名，供调试器读取。 */
static const char *volatile assert_file;

/** @brief 最近一次触发 configASSERT 的源代码行号，供调试器读取。 */
static volatile uint32_t assert_line;

/**
  * @brief  保存 FreeRTOS 断言位置并停止系统。
  * @param  file 触发断言的源文件名。
  * @param  line 触发断言的源代码行号。
  * @retval None
  * @note   连接调试器时触发 BKPT；未连接时保持在死循环中供看门狗或人工复位。
  */
void vAssertCalled(const char *file, uint32_t line)
{
    assert_file = file;
    assert_line = line;

    __disable_irq();

    if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0u)
    {
        __BKPT(0);
    }

    while (1)
    {
    }
}
