/**
 * @file joint_service.c
 * @brief 关节服务实现：采样滤波、闭环推进、标定存取，接到实际硬件。
 */

#include "joint_service.h"

#include "bsp_adc.h"
#include "bsp_motor.h"
#include "bsp_param_flash.h"
#include "main.h"

_Static_assert(JOINT_COUNT == BSP_ADC_CHANNEL_COUNT,
               "each joint needs one potentiometer channel");
_Static_assert(POT_OVERSAMPLE <= BSP_ADC_BURST_MAX_SCANS &&
                   POT_OVERSAMPLE <= JOINT_FILTER_MAX_SAMPLES,
               "oversampling must fit the ADC burst buffer and the filter");
_Static_assert(JOINT_CAL_BLOB_SIZE % 4U == 0U && JOINT_CAL_BLOB_SIZE <= 1024U &&
                   JOINT_CAL_BLOB_SIZE + 4U <= 1024U &&
                   JOINT_CAL_BLOB_V4_SIZE % 4U == 0U &&
                   JOINT_CAL_BLOB_V3_SIZE % 4U == 0U &&
                   JOINT_CAL_BLOB_V2_SIZE % 4U == 0U,
               "param flash records must be multiples of 4, at most 1024");

/**
 * @brief Flash 里一条记录的字节数：标定 blob 后面跟 4 字节小端的启动计数。
 *
 * 参数扇区只存一种定长记录，所以启动计数和标定放在同一条记录里。
 */
#define STORE_SIZE (JOINT_CAL_BLOB_SIZE + 4U)

/** @brief 单次六路 ADC 扫描的等待超时，单位毫秒。 */
#define ADC_TIMEOUT_MS (5U)

/**
 * @brief ADC 排名顺序到关节下标的映射，见 app_config.h 的 JOINT_ADC_ORDER。
 *
 * 默认恒等；板子实测 M1/M3 对调时只改这个宏。
 */
static const uint8_t k_adc_order[BSP_ADC_CHANNEL_COUNT] = JOINT_ADC_ORDER;

static joint_control_t s_joints;
static bool s_is_adc_ok = false;
/** @brief 标定参数的 Flash 存储是否可用。 */
static bool s_is_store_ok = false;
/** @brief Flash 里最新记录的启动计数；没有记录或旧格式记录时为 0。 */
static uint32_t s_boot_count = 0U;

static uint16_t s_pot_cache[BSP_ADC_CHANNEL_COUNT];
/** @brief 最近一次采样中每路样本的极差。 */
static uint16_t s_pot_spread[BSP_ADC_CHANNEL_COUNT];
/** @brief 前两次过采样结果，做时间上的三点中值；计数为已有的有效次数。 */
static uint16_t s_pot_history[2][BSP_ADC_CHANNEL_COUNT];
static unsigned s_pot_history_count = 0U;
static bool s_is_pot_valid = false;
static uint32_t s_last_adc_ms = 0U;

static bool joint_drive(unsigned index, int duty)
{
    return bsp_motor_set_duty((bsp_motor_id_t)index, (int16_t)duty) == BSP_OK;
}

static bool joint_brake(unsigned index)
{
    return bsp_motor_brake((bsp_motor_id_t)index) == BSP_OK;
}

/**
 * @brief 初始化 Flash 参数存储并恢复上次保存的标定和控制参数。
 *
 * 只有旧格式（不带启动计数的版本 5、版本 4、版本 3 或版本 2）记录时，恢复
 * 其中的参数并立即按新格式保存，免得刷新固件后丢失标定。
 *
 * @return 恢复的关节数。
 * @note 需要擦除备用扇区时会阻塞约 2 秒，只在初始化阶段调用。
 */
static unsigned load_calibration(void)
{
    /* 必须在 init 之前读旧记录：init 换用新长度后可能擦掉旧记录所在扇区。
     * 从新到旧依次找：不带启动计数的版本 5、版本 4、版本 3、版本 2。 */
    static const uint32_t k_old_sizes[] = {
        JOINT_CAL_BLOB_SIZE, JOINT_CAL_BLOB_V4_SIZE, JOINT_CAL_BLOB_V3_SIZE,
        JOINT_CAL_BLOB_V2_SIZE};
    static uint8_t old[JOINT_CAL_BLOB_SIZE];
    uint32_t old_size = 0U;
    bool has_old = false;
    for (unsigned i = 0U;
         !has_old && i < sizeof(k_old_sizes) / sizeof(k_old_sizes[0]); ++i)
    {
        old_size = k_old_sizes[i];
        has_old = bsp_param_flash_peek(old_size, old) == BSP_OK;
    }
    bsp_status_t status = bsp_param_flash_init(STORE_SIZE);
    s_is_store_ok = (status == BSP_OK);
    /* 备用扇区擦除失败（ERR_HAL）时已有记录仍可读，只是之后保存可能失败。 */
    static uint8_t record[STORE_SIZE];
    if ((status != BSP_OK) && (status != BSP_ERR_HAL))
    {
        return 0U;
    }
    if (bsp_param_flash_load(record) == BSP_OK)
    {
        const uint8_t *count = &record[JOINT_CAL_BLOB_SIZE];
        s_boot_count = (uint32_t)count[0] | ((uint32_t)count[1] << 8) |
                       ((uint32_t)count[2] << 16) | ((uint32_t)count[3] << 24);
        return joint_cal_import(&s_joints, record, JOINT_CAL_BLOB_SIZE);
    }
    unsigned restored =
        has_old ? joint_cal_import(&s_joints, old, old_size) : 0U;
    /* 版本 3 起即使没有已标定的关节也带着控制参数，同样要转存。 */
    if (restored > 0U || (has_old && old_size != JOINT_CAL_BLOB_V2_SIZE))
    {
        (void)joint_service_save();
    }
    return restored;
}

/**
 * @brief 过采样一次六路电位器，滤波后交给关节控制。
 *
 * @param[in] sample_ms 本次采样的起始时刻。关节控制用这个时刻算 dt 和制动
 *            距离：16 次过采样加两级滤波要花十几毫秒，用采完的时刻会把
 *            这段时间当成已经过去，dt 偏大、制动距离算得偏短（问题清单 B2）。
 */
static void sample(uint32_t sample_ms)
{
    static uint16_t samples[POT_OVERSAMPLE][BSP_ADC_CHANNEL_COUNT];
    s_is_pot_valid = s_is_adc_ok;
    if (s_is_pot_valid)
    {
        /* 一次连续扫完全部轮次：各轮间隔均匀，把参考电压上的纹波平均掉。 */
        s_is_pot_valid =
            bsp_adc_read_burst(samples, POT_OVERSAMPLE, ADC_TIMEOUT_MS) ==
            BSP_OK;
    }
    if (s_is_pot_valid)
    {
        uint16_t ch_raw[BSP_ADC_CHANNEL_COUNT];
        uint16_t ch_spread[BSP_ADC_CHANNEL_COUNT];
        uint16_t now_raw[BSP_ADC_CHANNEL_COUNT];
        joint_filter((const uint16_t (*)[JOINT_COUNT])samples, POT_OVERSAMPLE,
                     ch_raw, ch_spread);
        /* 换成关节顺序：以后发现某两路接反了只改 JOINT_ADC_ORDER。 */
        for (unsigned i = 0; i < BSP_ADC_CHANNEL_COUNT; ++i)
        {
            now_raw[k_adc_order[i]] = ch_raw[i];
            s_pot_spread[k_adc_order[i]] = ch_spread[i];
        }
        /* 再和前两次取中值，去掉单次 2～3° 的尖峰，代价是 10ms 延迟。 */
        for (unsigned i = 0; i < BSP_ADC_CHANNEL_COUNT; ++i)
        {
            s_pot_cache[i] = (s_pot_history_count >= 2U)
                                 ? joint_median3(s_pot_history[0][i],
                                                 s_pot_history[1][i], now_raw[i])
                                 : now_raw[i];
            s_pot_history[0][i] = s_pot_history[1][i];
            s_pot_history[1][i] = now_raw[i];
        }
        if (s_pot_history_count < 2U)
        {
            ++s_pot_history_count;
        }
    }
    else
    {
        s_pot_history_count = 0U;
    }
    joint_feed(&s_joints, s_pot_cache, s_pot_spread, s_is_pot_valid, sample_ms);
}

unsigned joint_service_init(void (*reply)(const char *line))
{
    joint_hw_t hw = {joint_drive, reply, joint_brake};
    joint_init(&s_joints, hw);
    unsigned restored = load_calibration();
    s_is_adc_ok = bsp_adc_init() == BSP_OK;
    return restored;
}

joint_control_t *joint_service_joints(void)
{
    return &s_joints;
}

bool joint_service_update(void)
{
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - s_last_adc_ms) >= POT_SAMPLE_PERIOD_MS)
    {
        s_last_adc_ms = now;
        /* 传采样开始时刻：滤波链有十几毫秒延迟，用结束时刻会让 dt 虚高。 */
        sample(now);
    }
    return joint_tick(&s_joints, HAL_GetTick());
}

bool joint_service_save(void)
{
    /* 标定和启动计数在同一条记录里，保存标定时把当前的启动计数原样带上。 */
    uint8_t record[STORE_SIZE];
    joint_cal_export(&s_joints, record);
    record[JOINT_CAL_BLOB_SIZE] = (uint8_t)(s_boot_count & 0xFFU);
    record[JOINT_CAL_BLOB_SIZE + 1U] = (uint8_t)((s_boot_count >> 8) & 0xFFU);
    record[JOINT_CAL_BLOB_SIZE + 2U] = (uint8_t)((s_boot_count >> 16) & 0xFFU);
    record[JOINT_CAL_BLOB_SIZE + 3U] = (uint8_t)((s_boot_count >> 24) & 0xFFU);
    return s_is_store_ok && bsp_param_flash_save(record) == BSP_OK;
}

bool joint_service_next_boot_id(uint32_t *boot_id)
{
    ++s_boot_count;
    if (!joint_service_save())
    {
        /* 没写进去的值下次启动还会再用一次，不能交出去。 */
        --s_boot_count;
        return false;
    }
    *boot_id = s_boot_count;
    return true;
}

bool joint_service_store_ok(void)
{
    return s_is_store_ok;
}

bool joint_service_adc_ok(void)
{
    return s_is_adc_ok;
}

bool joint_service_pot_valid(void)
{
    return s_is_pot_valid;
}

const uint16_t *joint_service_pot(void)
{
    return s_pot_cache;
}
