#include "bsp_uart_stream.h"

#include <stddef.h>

/** @brief 挂起一次单字节中断接收，不改动缓冲。 */
static bsp_status_t arm_receive(bsp_uart_stream_t *self)
{
    if (self->huart->RxState != HAL_UART_STATE_READY)
    {
        return BSP_BUSY;
    }

    self->has_error = false;
    /* 先置位再启动：第一个字节的中断可能在 HAL 返回之前就到来。 */
    self->is_active = true;
    if (HAL_UART_Receive_IT(self->huart, &self->rx_byte, 1U) != HAL_OK)
    {
        self->is_active = false;
        return BSP_ERR_HAL;
    }
    return BSP_OK;
}

bsp_status_t bsp_uart_stream_init(bsp_uart_stream_t *self,
                                  UART_HandleTypeDef *huart, uint8_t *ring,
                                  uint32_t size)
{
    if ((self == NULL) || (huart == NULL) || (ring == NULL) || (size < 2U) ||
        ((size & (size - 1U)) != 0U))
    {
        return BSP_ERR_PARAM;
    }
    if ((huart->Instance == NULL) || (huart->gState == HAL_UART_STATE_RESET))
    {
        return BSP_ERR_CONFIG;
    }

    self->huart = huart;
    self->ring = ring;
    self->mask = size - 1U;
    self->head = 0U;
    self->tail = 0U;
    self->is_enabled = false;
    self->is_active = false;
    self->has_error = false;
    self->overflow_count = 0U;
    self->error_count = 0U;
    return BSP_OK;
}

bsp_status_t bsp_uart_stream_start(bsp_uart_stream_t *self)
{
    if (self->is_enabled)
    {
        return BSP_OK;
    }

    self->tail = self->head;
    bsp_status_t status = arm_receive(self);
    if (status == BSP_OK)
    {
        self->is_enabled = true;
    }
    return status;
}

bsp_status_t bsp_uart_stream_stop(bsp_uart_stream_t *self)
{
    if (!self->is_enabled)
    {
        return BSP_OK;
    }

    /* 先清标志，使终止过程中到来的回调被忽略。 */
    self->is_enabled = false;
    self->is_active = false;
    HAL_StatusTypeDef status = HAL_UART_AbortReceive(self->huart);
    self->has_error = false;
    self->tail = self->head;
    return (status == HAL_OK) ? BSP_OK : BSP_ERR_HAL;
}

bool bsp_uart_stream_is_active(const bsp_uart_stream_t *self)
{
    return self->is_enabled;
}

bool bsp_uart_stream_read(bsp_uart_stream_t *self, uint8_t *byte)
{
    uint32_t tail = self->tail;
    if (tail == self->head)
    {
        return false;
    }
    *byte = self->ring[tail];
    /* 读出数据之后才释放位置，防止 ISR 提前覆盖。 */
    __DMB();
    self->tail = (tail + 1U) & self->mask;
    return true;
}

void bsp_uart_stream_flush(bsp_uart_stream_t *self)
{
    self->tail = self->head;
}

bsp_status_t bsp_uart_stream_update(bsp_uart_stream_t *self)
{
    if (!self->is_enabled || (self->is_active && !self->has_error))
    {
        return BSP_OK;
    }

    if (self->has_error)
    {
        ++self->error_count;
        self->is_active = false;
        (void)HAL_UART_AbortReceive(self->huart);
        self->has_error = false;
    }

    /* 重新挂起接收；失败时下次调用再试。缓冲中的字节保留给调用者。 */
    return (arm_receive(self) == BSP_OK) ? BSP_OK : BSP_ERR_HAL;
}

uint32_t bsp_uart_stream_error_count(const bsp_uart_stream_t *self)
{
    return self->error_count;
}

uint32_t bsp_uart_stream_overflow_count(const bsp_uart_stream_t *self)
{
    return self->overflow_count;
}

void bsp_uart_stream_on_rx_cplt_from_isr(bsp_uart_stream_t *self)
{
    if (!self->is_active)
    {
        return;
    }

    uint32_t head = self->head;
    uint32_t next = (head + 1U) & self->mask;
    if (next == self->tail)
    {
        ++self->overflow_count;
    }
    else
    {
        self->ring[head] = self->rx_byte;
        /* 数据写入之后才发布新位置，主循环不会读到旧字节。 */
        __DMB();
        self->head = next;
    }

    if (HAL_UART_Receive_IT(self->huart, &self->rx_byte, 1U) != HAL_OK)
    {
        self->has_error = true;
    }
}

void bsp_uart_stream_on_error_from_isr(bsp_uart_stream_t *self)
{
    if (self->is_active)
    {
        self->has_error = true;
    }
}
