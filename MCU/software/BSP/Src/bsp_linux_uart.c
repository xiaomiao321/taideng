#include "bsp_linux_uart.h"

#include <string.h>

#include "usart.h"

/** @brief 没有待取接收块时的槽位编号。 */
#define SLOT_NONE (0xFFU)
/** @brief 发送环掩码。 */
#define TX_RING_MASK (BSP_LINUX_UART_TX_RING_SIZE - 1U)

static bool s_is_initialized = false;

/* ------------------------------ 接收（乒乓） ------------------------------ */

/** @brief 两块接收缓冲，HAL 写一块，主循环取另一块。 */
static uint8_t s_rx_block[2][BSP_LINUX_UART_RX_BLOCK_SIZE];
/** @brief HAL 当前正在写的槽。 */
static volatile uint8_t s_rx_fill = 0U;
/** @brief 已收完、等待主循环取走的槽；SLOT_NONE 表示没有。 */
static volatile uint8_t s_rx_ready = SLOT_NONE;
/** @brief 已收完的字节数。 */
static volatile uint16_t s_rx_ready_length = 0U;
/** @brief 上一块里已经被 take 走的位置。 */
static uint16_t s_rx_ready_offset = 0U;
static volatile bool s_is_rx_active = false;
static volatile bool s_has_rx_error = false;
static volatile uint32_t s_rx_overflow_count = 0U;
static volatile uint32_t s_error_count = 0U;

/* ------------------------------ 发送（队列） ------------------------------ */

static uint8_t s_tx_ring[BSP_LINUX_UART_TX_RING_SIZE];
static volatile uint16_t s_tx_head = 0U;
static volatile uint16_t s_tx_tail = 0U;
static volatile uint16_t s_tx_inflight = 0U;
static volatile bool s_is_tx_busy = false;
static volatile uint32_t s_tx_overflow_count = 0U;

/** @brief 把指定槽挂到 HAL 上接收。 */
static bsp_status_t start_block(uint8_t slot)
{
    s_rx_fill = slot;
    s_is_rx_active = true;
    HAL_StatusTypeDef status = HAL_UARTEx_ReceiveToIdle_IT(
        &huart1, s_rx_block[slot], (uint16_t)BSP_LINUX_UART_RX_BLOCK_SIZE);
    if (status != HAL_OK)
    {
        s_is_rx_active = false;
        ++s_error_count;
        return BSP_ERR_HAL;
    }
    return BSP_OK;
}

/** @brief 把发送环里从 tail 开始的一段连续数据挂到 HAL 上。 */
static void start_next_tx_chunk(void)
{
    if (s_is_tx_busy)
    {
        return;
    }
    uint16_t head = s_tx_head;
    uint16_t tail = s_tx_tail;
    if (head == tail)
    {
        return;
    }

    uint16_t contiguous =
        (uint16_t)((BSP_LINUX_UART_TX_RING_SIZE - tail) & TX_RING_MASK);
    if (contiguous == 0U)
    {
        contiguous = BSP_LINUX_UART_TX_RING_SIZE;
    }
    uint16_t available = (uint16_t)((head - tail) & TX_RING_MASK);
    uint16_t chunk = (available < contiguous) ? available : contiguous;

    s_tx_inflight = chunk;
    s_is_tx_busy = true;
    if (HAL_UART_Transmit_IT(&huart1, &s_tx_ring[tail], chunk) != HAL_OK)
    {
        s_tx_inflight = 0U;
        s_is_tx_busy = false;
        ++s_error_count;
    }
}

bsp_status_t bsp_linux_uart_init(void)
{
    s_is_initialized = false;
    if ((huart1.Instance != USART1) ||
        (huart1.gState == HAL_UART_STATE_RESET) ||
        (huart1.RxState == HAL_UART_STATE_RESET))
    {
        return BSP_ERR_CONFIG;
    }
    if ((huart1.Init.BaudRate != 115200U) ||
        (huart1.Init.WordLength != UART_WORDLENGTH_8B) ||
        (huart1.Init.StopBits != UART_STOPBITS_1) ||
        (huart1.Init.Parity != UART_PARITY_NONE) ||
        (huart1.Init.HwFlowCtl != UART_HWCONTROL_NONE))
    {
        return BSP_ERR_CONFIG;
    }
    if ((huart1.gState != HAL_UART_STATE_READY) ||
        (huart1.RxState != HAL_UART_STATE_READY))
    {
        /* 上电时 UART 刚由 MX_USART1_UART_Init() 初始化，两个状态都应是 READY。 */
        return BSP_BUSY;
    }

    s_rx_fill = 0U;
    s_rx_ready = SLOT_NONE;
    s_rx_ready_length = 0U;
    s_rx_ready_offset = 0U;
    s_has_rx_error = false;
    s_is_rx_active = false;
    s_rx_overflow_count = 0U;
    s_error_count = 0U;
    s_tx_head = 0U;
    s_tx_tail = 0U;
    s_tx_inflight = 0U;
    s_is_tx_busy = false;
    s_tx_overflow_count = 0U;

    s_is_initialized = true;
    return start_block(0U);
}

size_t bsp_linux_uart_tx_free(void)
{
    uint16_t used = (uint16_t)((s_tx_head - s_tx_tail) & TX_RING_MASK);
    return (size_t)(TX_RING_MASK - used);
}

bsp_status_t bsp_linux_uart_write(const uint8_t *data, size_t length,
                                  uint32_t timeout_ms)
{
    (void)timeout_ms; /* 非阻塞：保留参数只为兼容旧调用点。 */

    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }
    if ((data == NULL) || (length == 0U) || (length > UINT16_MAX))
    {
        return BSP_ERR_PARAM;
    }
    if (length > bsp_linux_uart_tx_free())
    {
        s_tx_overflow_count += (uint32_t)length;
        return BSP_ERR_TIMEOUT;
    }

    uint16_t head = s_tx_head;
    for (size_t i = 0U; i < length; ++i)
    {
        s_tx_ring[head] = data[i];
        head = (uint16_t)((head + 1U) & TX_RING_MASK);
    }
    s_tx_head = head;

    start_next_tx_chunk();
    return BSP_OK;
}

size_t bsp_linux_uart_read(uint8_t *dst, size_t cap)
{
    if (!s_is_initialized || (dst == NULL) || (cap == 0U))
    {
        return 0U;
    }

    uint8_t slot = s_rx_ready;
    if (slot == SLOT_NONE)
    {
        return 0U;
    }

    uint16_t length = s_rx_ready_length;
    uint16_t offset = s_rx_ready_offset;
    if (offset >= length)
    {
        /* 理论上不会发生：防御性清空，避免卡在这个槽上。 */
        s_rx_ready = SLOT_NONE;
        s_rx_ready_length = 0U;
        s_rx_ready_offset = 0U;
        return 0U;
    }

    size_t chunk = (size_t)(length - offset);
    if (chunk > cap)
    {
        chunk = cap;
    }
    memcpy(dst, &s_rx_block[slot][offset], chunk);
    offset = (uint16_t)(offset + chunk);

    /* 和接收中断抢状态：这里必须原子，否则中断刚挂上的新槽会被主循环清掉。 */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    bool is_released = false;
    if (s_rx_ready == slot)
    {
        if (offset >= length)
        {
            s_rx_ready = SLOT_NONE;
            s_rx_ready_length = 0U;
            s_rx_ready_offset = 0U;
            is_released = true;
        }
        else
        {
            s_rx_ready_offset = offset;
        }
    }
    __set_PRIMASK(primask);

    if (is_released && !s_is_rx_active)
    {
        /* 收到错误后接收停了：取空这一块就把链路拉起来。 */
        (void)start_block(slot);
    }
    return chunk;
}

bsp_status_t bsp_linux_uart_update(void)
{
    if (!s_is_initialized)
    {
        return BSP_ERR_NOT_INITIALIZED;
    }

    start_next_tx_chunk();

    if (s_has_rx_error)
    {
        s_has_rx_error = false;
        ++s_error_count;
        (void)HAL_UART_AbortReceive(&huart1);
        s_is_rx_active = false;
        /* 不能拿还等着主循环取走的那一块，选另一块重新开始。 */
        uint8_t slot = (s_rx_ready == 0U) ? 1U : 0U;
        return start_block(slot);
    }

    if (!s_is_rx_active && (s_rx_ready == SLOT_NONE))
    {
        return start_block(s_rx_fill);
    }
    return BSP_OK;
}

uint32_t bsp_linux_uart_rx_overflow_count(void)
{
    return s_rx_overflow_count;
}

uint32_t bsp_linux_uart_tx_overflow_count(void)
{
    return s_tx_overflow_count;
}

uint32_t bsp_linux_uart_error_count(void)
{
    return s_error_count;
}

void bsp_linux_uart_on_rx_event_from_isr(uint16_t size)
{
    if (!s_is_initialized || !s_is_rx_active)
    {
        return;
    }
    if ((size == 0U) || (size > BSP_LINUX_UART_RX_BLOCK_SIZE))
    {
        s_has_rx_error = true;
        return;
    }

    uint8_t done = s_rx_fill;
    if (s_rx_ready != SLOT_NONE)
    {
        /* 主循环还没来得及取上一块：两块都占着，只能丢掉旧数据继续收。 */
        ++s_rx_overflow_count;
    }
    s_rx_ready_length = size;
    s_rx_ready_offset = 0U;
    s_rx_ready = done;

    uint8_t next = (uint8_t)(1U - done);
    s_rx_fill = next;
    /* 在回调里直接重新挂载：HAL 进入本回调之前已把 RxState 恢复为 READY，
     * 因此这里不会返回 BUSY，也不必等主循环处理完才恢复接收。 */
    if (HAL_UARTEx_ReceiveToIdle_IT(&huart1, s_rx_block[next],
                                    (uint16_t)BSP_LINUX_UART_RX_BLOCK_SIZE) != HAL_OK)
    {
        s_is_rx_active = false;
        s_has_rx_error = true;
    }
}

void bsp_linux_uart_on_error_from_isr(void)
{
    if (!s_is_initialized)
    {
        return;
    }
    s_has_rx_error = true;
}

void bsp_linux_uart_on_tx_cplt_from_isr(void)
{
    uint16_t sent = s_tx_inflight;
    s_tx_inflight = 0U;
    if (sent != 0U)
    {
        s_tx_tail = (uint16_t)((s_tx_tail + sent) & TX_RING_MASK);
    }
    s_is_tx_busy = false;
}
