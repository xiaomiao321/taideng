/**
 * @file linux_uart_selftest.c
 * @brief bsp_linux_uart 的 PC 端自测：双缓冲接收、排队发送和出错后的恢复。
 *
 * 用 fake_hal/usart.h 里的假 HAL 代替真正的 USART1：接收时测试把字节写进驱动
 * 交给 HAL 的那块缓冲，再调用接收事件回调；发送时 HAL_UART_Transmit_IT() 只
 * 记下要发的一段，测试再调用发送完成回调。
 *
 * @note 本文件不属于固件，不参与 arm 构建；输出一律 ASCII。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bsp_linux_uart.h"
#include "usart.h"

/* -------------------------------------------------------------------------- */
/* 假 HAL                                                                      */
/* -------------------------------------------------------------------------- */

USART_TypeDef fake_usart1;
UART_HandleTypeDef huart1;

static uint8_t *rx_buffer; /**< 驱动挂上来等待接收的缓冲，NULL 表示没在收。 */
static uint16_t rx_capacity;
static HAL_StatusTypeDef rx_start_result = HAL_OK;
static unsigned abort_calls;
static const uint8_t *tx_data; /**< 正在"发送"的一段，NULL 表示空闲。 */
static uint16_t tx_size;
static HAL_StatusTypeDef tx_start_result = HAL_OK;
static char wire[4096]; /**< 已经发到线路上的全部字节。 */
static size_t wire_length;
static uint32_t primask;

HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_IT(UART_HandleTypeDef *huart,
                                              uint8_t *data, uint16_t size)
{
    (void)huart;
    if (rx_start_result != HAL_OK)
    {
        return rx_start_result;
    }
    rx_buffer = data;
    rx_capacity = size;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *huart)
{
    (void)huart;
    ++abort_calls;
    rx_buffer = NULL;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *huart,
                                       const uint8_t *data, uint16_t size)
{
    (void)huart;
    assert(tx_data == NULL && size > 0U);
    if (tx_start_result != HAL_OK)
    {
        return tx_start_result;
    }
    tx_data = data;
    tx_size = size;
    return HAL_OK;
}

uint32_t __get_PRIMASK(void)
{
    return primask;
}

void __set_PRIMASK(uint32_t value)
{
    primask = value;
}

void __disable_irq(void)
{
    primask = 1U;
}

/* -------------------------------------------------------------------------- */
/* 小工具                                                                      */
/* -------------------------------------------------------------------------- */

/** @brief 模拟线路上来了一批字节，以空闲结束：HAL 写进缓冲后调用事件回调。 */
static void receive(const char *text)
{
    size_t length = strlen(text);
    assert(rx_buffer != NULL && length <= rx_capacity);
    uint8_t *target = rx_buffer;
    memcpy(target, text, length);
    rx_buffer = NULL; /* HAL 在回调之前已经结束这一次接收 */
    bsp_linux_uart_on_rx_event_from_isr((uint16_t)length);
}

/** @brief 把驱动里现有的接收数据全部读出来，接到 out 后面。 */
static size_t read_all(char *out, size_t chunk)
{
    size_t total = 0U;
    size_t got;
    while ((got = bsp_linux_uart_read((uint8_t *)&out[total], chunk)) > 0U)
    {
        total += got;
    }
    out[total] = '\0';
    return total;
}

/** @brief 模拟当前这一段发送完成。 */
static void complete_tx(void)
{
    assert(tx_data != NULL);
    memcpy(&wire[wire_length], tx_data, tx_size);
    wire_length += tx_size;
    wire[wire_length] = '\0';
    tx_data = NULL;
    bsp_linux_uart_on_tx_cplt_from_isr();
}

/** @brief 像主循环那样反复 update 并完成发送，直到发空。 */
static void drain_tx(void)
{
    for (unsigned i = 0U; i < 100U; ++i)
    {
        assert(bsp_linux_uart_update() == BSP_OK);
        if (tx_data == NULL)
        {
            return;
        }
        complete_tx();
    }
    assert(false);
}

static void reset(void)
{
    huart1.Instance = USART1;
    huart1.gState = HAL_UART_STATE_READY;
    huart1.RxState = HAL_UART_STATE_READY;
    huart1.Init.BaudRate = 115200U;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    rx_buffer = NULL;
    rx_start_result = HAL_OK;
    tx_data = NULL;
    tx_start_result = HAL_OK;
    abort_calls = 0U;
    wire_length = 0U;
    wire[0] = '\0';
    primask = 0U;
    assert(bsp_linux_uart_init() == BSP_OK);
}

int main(void)
{
    char out[1024];
    uint8_t byte;

    /* 初始化之前什么都不做；配置不对拒绝初始化。 */
    assert(bsp_linux_uart_read(&byte, 1U) == 0U);
    assert(bsp_linux_uart_write((const uint8_t *)"x", 1U, 0U) ==
           BSP_ERR_NOT_INITIALIZED);
    assert(bsp_linux_uart_update() == BSP_ERR_NOT_INITIALIZED);
    reset();
    huart1.Init.BaudRate = 9600U;
    assert(bsp_linux_uart_init() == BSP_ERR_CONFIG);

    /* 初始化后立即开始接收；收到一批后不等主循环，马上在另一块缓冲上接着收。 */
    reset();
    assert(rx_buffer != NULL && rx_capacity == BSP_LINUX_UART_RX_BLOCK_SIZE);
    uint8_t *first_block = rx_buffer;
    receive("frame-one");
    assert(rx_buffer != NULL && rx_buffer != first_block);
    assert(read_all(out, sizeof(out)) == 9U && !strcmp(out, "frame-one"));
    assert(bsp_linux_uart_read(&byte, 1U) == 0U);

    /* 小块读取：一批数据分多次读完，顺序不乱；读的过程中中断照常。 */
    receive("0123456789ABCDEF");
    assert(bsp_linux_uart_read((uint8_t *)out, 5U) == 5U);
    assert(!memcmp(out, "01234", 5U) && primask == 0U);
    assert(read_all(&out[5], 4U) == 11U && !strcmp(out, "0123456789ABCDEF"));

    /* 两块缓冲轮流用，连续很多批都不丢。 */
    for (unsigned i = 0U; i < 50U; ++i)
    {
        char text[32];
        (void)snprintf(text, sizeof(text), "batch-%u", i);
        receive(text);
        assert(read_all(out, 7U) == strlen(text) && !strcmp(out, text));
    }
    assert(bsp_linux_uart_rx_overflow_count() == 0U);

    /* 主循环一直没来读、又到了第二批：旧的一批被覆盖，计一次溢出，接收不停。 */
    receive("old");
    receive("new");
    assert(bsp_linux_uart_rx_overflow_count() == 1U && rx_buffer != NULL);
    assert(read_all(out, sizeof(out)) == 3U && !strcmp(out, "new"));

    /* 收满一整块也是一批。 */
    static char full[BSP_LINUX_UART_RX_BLOCK_SIZE + 1U];
    memset(full, 'f', BSP_LINUX_UART_RX_BLOCK_SIZE);
    receive(full);
    assert(read_all(out, 64U) == BSP_LINUX_UART_RX_BLOCK_SIZE);

    /* 串口报错：下一次 update 中止并重新开始接收，计一次错误，之后照常收。 */
    uint32_t errors = bsp_linux_uart_error_count();
    bsp_linux_uart_on_error_from_isr();
    rx_buffer = NULL; /* HAL 出错时已经停止接收 */
    assert(bsp_linux_uart_update() == BSP_OK);
    assert(abort_calls == 1U && rx_buffer != NULL);
    assert(bsp_linux_uart_error_count() == errors + 1U);
    receive("after-error");
    assert(read_all(out, sizeof(out)) == 11U && !strcmp(out, "after-error"));

    /* 出错时还有一批没读：重新接收不能占用那一块，数据读出来仍然完整。 */
    receive("unread");
    bsp_linux_uart_on_error_from_isr();
    rx_buffer = NULL;
    assert(bsp_linux_uart_update() == BSP_OK && rx_buffer != NULL);
    memset(rx_buffer, '#', 16U); /* 新一批正在写入另一块 */
    assert(read_all(out, sizeof(out)) == 6U && !strcmp(out, "unread"));

    /* 重新开始接收失败：update 报错，下一次再试。 */
    bsp_linux_uart_on_error_from_isr();
    rx_buffer = NULL;
    rx_start_result = HAL_BUSY;
    assert(bsp_linux_uart_update() == BSP_ERR_HAL && rx_buffer == NULL);
    rx_start_result = HAL_OK;
    assert(bsp_linux_uart_update() == BSP_OK && rx_buffer != NULL);

    /* 发送：入队立即返回并开始发；发送期间入队的排在后面，由 update 接着发。 */
    reset();
    assert(bsp_linux_uart_tx_free() == BSP_LINUX_UART_TX_RING_SIZE - 1U);
    assert(bsp_linux_uart_write((const uint8_t *)"ACK-1;", 6U, 100U) == BSP_OK);
    assert(tx_data != NULL && tx_size == 6U);
    assert(bsp_linux_uart_write((const uint8_t *)"STATUS-2;", 9U, 100U) ==
           BSP_OK);
    assert(tx_size == 6U);
    complete_tx();
    assert(tx_data == NULL); /* 中断里不接着发，等主循环的 update */
    drain_tx();
    assert(!strcmp(wire, "ACK-1;STATUS-2;"));
    assert(bsp_linux_uart_tx_free() == BSP_LINUX_UART_TX_RING_SIZE - 1U);

    /* 放不下的一帧整帧丢弃并计数，不发半帧；腾出位置后可以再发。 */
    static uint8_t frame[100];
    memset(frame, 'F', sizeof(frame));
    unsigned accepted = 0U;
    while (bsp_linux_uart_write(frame, sizeof(frame), 0U) == BSP_OK)
    {
        ++accepted;
    }
    assert(accepted == (BSP_LINUX_UART_TX_RING_SIZE - 1U) / sizeof(frame));
    assert(bsp_linux_uart_tx_overflow_count() == sizeof(frame));
    size_t before = wire_length;
    drain_tx();
    assert(wire_length - before == accepted * sizeof(frame));
    assert(bsp_linux_uart_write(frame, sizeof(frame), 0U) == BSP_OK);
    drain_tx();

    /* 跨过队列末尾很多次：线路上的内容与入队的完全一致。 */
    reset();
    static char expected[4096];
    size_t expected_length = 0U;
    for (unsigned i = 0U; i < 120U; ++i)
    {
        char text[40];
        int n = snprintf(text, sizeof(text), "reply-%u-abcdefghij;", i);
        assert(bsp_linux_uart_write((const uint8_t *)text, (size_t)n, 0U) ==
               BSP_OK);
        memcpy(&expected[expected_length], text, (size_t)n);
        expected_length += (size_t)n;
        if ((i % 3U) == 2U)
        {
            drain_tx();
        }
    }
    drain_tx();
    assert(wire_length == expected_length &&
           !memcmp(wire, expected, expected_length));

    /* 启动发送失败（HAL 忙）：数据留在队列里，下一次 update 发出。 */
    reset();
    tx_start_result = HAL_BUSY;
    assert(bsp_linux_uart_write((const uint8_t *)"late;", 5U, 0U) == BSP_OK);
    assert(tx_data == NULL);
    tx_start_result = HAL_OK;
    drain_tx();
    assert(!strcmp(wire, "late;"));

    /* 参数检查。 */
    assert(bsp_linux_uart_write(NULL, 3U, 0U) == BSP_ERR_PARAM);
    assert(bsp_linux_uart_write(frame, 0U, 0U) == BSP_ERR_PARAM);
    assert(bsp_linux_uart_read(NULL, 4U) == 0U);
    assert(bsp_linux_uart_read(&byte, 0U) == 0U);

    puts("linux_uart: ping-pong receive, chunked read, overflow, error "
         "recovery, queued transmit, whole-frame drop and wrap-around passed");
    return 0;
}
