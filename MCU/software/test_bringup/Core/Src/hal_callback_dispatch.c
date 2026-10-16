/**
 * @file hal_callback_dispatch.c
 * @brief HAL 弱回调在本工程中的唯一实现位置。
 *
 * HAL 的 HAL_*_Callback 是全局弱符号，整个程序只能有一处强定义，
 * 且按名称由 HAL 的 IRQHandler 调用，不能声明为 static。
 * 因此这些回调属于集成层，不属于任何单个驱动：
 * 本文件只做“按外设实例路由”，把事件转发给对应驱动的 _from_isr() 接口；
 * 真正的数据处理仍留在各驱动的 poll/update 中（不在中断里做重活）。
 *
 * 驱动模块不再定义任何 HAL 回调，只提供 _from_isr() 形式的普通函数，
 * 因此每个驱动都能独立复用，并支持同型号外设挂多个实例。
 *
 * 新增外设中断时的约定：
 *   1. 在 HAL 的 HAL_*_Callback 里判断实例（huart->Instance / hadc->Instance 等）；
 *   2. 只调用驱动提供的 _from_isr()，不在此处打印、比较数据或调用阻塞接口；
 *   3. 数据处理、超时、清理放到驱动的 poll/update，由主循环或任务调用。
 */

#include "main.h"

#include "bsp_adc.h"
#include "debug_console.h"
#include "bsp_key.h"
#include "bsp_linux_uart.h"

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        bsp_adc_on_conv_cplt_from_isr();
    }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        bsp_adc_on_error_from_isr();
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance == USART1)
    {
        bsp_linux_uart_on_rx_event_from_isr(size);
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) debug_console_rx_from_isr();
    if (huart->Instance == USART1) debug_console_linux_rx_from_isr();
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) debug_console_error_from_isr();
    if (huart->Instance == USART1)
    {
        bsp_linux_uart_on_error_from_isr();
        debug_console_linux_error_from_isr();
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == HOME_KEY_Pin)
    {
        bsp_key_on_exti_from_isr();
    }
}
