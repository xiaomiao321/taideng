/**
 * @file usart.h
 * @brief PC 端自测用的假 HAL：只提供两个串口驱动用到的那一小部分。
 *
 * debug_uart_selftest.c 和 linux_uart_selftest.c 把本目录放在包含路径最前面，
 * 于是驱动里的 #include "usart.h" 找到的是这个文件，而不是 CubeMX 生成的那个。
 * 收发不会真的发生：HAL 函数由各个测试自己实现，只记下驱动交来的缓冲，由测试
 * 决定什么时候"发完"或"收到"。
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
        uint32_t WordLength;
        uint32_t StopBits;
        uint32_t Parity;
        uint32_t HwFlowCtl;
    } Init;
    HAL_UART_StateTypeDef gState;
    HAL_UART_StateTypeDef RxState;
} UART_HandleTypeDef;

typedef int IRQn_Type;
#define USART1_IRQn (37)
#define USART3_IRQn (39)

#define UART_WORDLENGTH_8B (0U)
#define UART_STOPBITS_1 (0U)
#define UART_PARITY_NONE (0U)
#define UART_HWCONTROL_NONE (0U)

/* 每个测试只定义自己用到的那个串口。 */
extern USART_TypeDef fake_usart1;
#define USART1 (&fake_usart1)
extern UART_HandleTypeDef huart1;
extern USART_TypeDef fake_usart3;
#define USART3 (&fake_usart3)
extern UART_HandleTypeDef huart3;

HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *huart);
HAL_UART_StateTypeDef HAL_UART_GetState(const UART_HandleTypeDef *huart);
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *huart,
                                       const uint8_t *data, uint16_t size);
HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *huart, uint8_t *data,
                                      uint16_t size);
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_IT(UART_HandleTypeDef *huart,
                                              uint8_t *data, uint16_t size);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *huart);
void HAL_NVIC_DisableIRQ(IRQn_Type irq);
void HAL_NVIC_EnableIRQ(IRQn_Type irq);
uint32_t HAL_GetTick(void);
uint32_t __get_PRIMASK(void);
void __set_PRIMASK(uint32_t value);
void __disable_irq(void);
uint32_t __get_IPSR(void);
void __DMB(void);

#endif /* AITAID_FAKE_USART_H */
