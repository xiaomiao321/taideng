/**
 * @file usart.h
 * @brief PC 端自测用的假 HAL：只提供 bsp_debug_uart.c 用到的那一小部分。
 *
 * debug_uart_selftest.c 把本目录放在包含路径最前面，于是 bsp_debug_uart.c 里的
 * #include "usart.h" 找到的是这个文件，而不是 CubeMX 生成的那个。发送不会真的
 * 发生：HAL_UART_Transmit_IT() 只记下要发的那一段，由测试决定什么时候"发完"。
 *
 * @note 本文件不属于固件，不参与 arm 构建。
 */

#ifndef AITAID_FAKE_USART_H
#define AITAID_FAKE_USART_H

#include <stdint.h>

typedef enum
{
    HAL_OK = 0,
    HAL_ERROR,
    HAL_BUSY,
    HAL_TIMEOUT
} HAL_StatusTypeDef;

typedef enum
{
    HAL_UART_STATE_RESET = 0,
    HAL_UART_STATE_READY
} HAL_UART_StateTypeDef;

typedef struct
{
    int unused;
} USART_TypeDef;

typedef struct
{
    USART_TypeDef *Instance;
    struct
    {
        uint32_t BaudRate;
    } Init;
    HAL_UART_StateTypeDef gState;
    HAL_UART_StateTypeDef RxState;
} UART_HandleTypeDef;

typedef int IRQn_Type;
#define USART3_IRQn (39)

extern USART_TypeDef fake_usart3;
#define USART3 (&fake_usart3)
extern UART_HandleTypeDef huart3;

HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *huart);
HAL_UART_StateTypeDef HAL_UART_GetState(const UART_HandleTypeDef *huart);
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *huart,
                                       const uint8_t *data, uint16_t size);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *data,
                                      uint16_t size);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *huart);
void HAL_NVIC_DisableIRQ(IRQn_Type irq);
void HAL_NVIC_EnableIRQ(IRQn_Type irq);
uint32_t HAL_GetTick(void);
uint32_t __get_PRIMASK(void);
uint32_t __get_IPSR(void);
void __DMB(void);

#endif /* AITAID_FAKE_USART_H */
