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
_Static_assert(JOINT_CAL_BLOB_SIZE % 4U == 0U && JOINT_CAL_BLOB_SIZE <= 1024U &&
                   JOINT_CAL_BLOB_V3_SIZE % 4U == 0U &&
                   JOINT_CAL_BLOB_V2_SIZE % 4U == 0U,
               "param flash records must be multiples of 4, at most 1024");

/** @brief 单次六路 ADC 扫描的等待超时，单位毫秒。 */
#define ADC_TIMEOUT_MS (5U)

static joint_control_t s_joints;
static bool s_is_adc_ok = false;
/** @brief 标定参数的 Flash 存储是否可用。 */
static bool s_is_store_ok = false;

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
 * 只有旧格式（版本 3 或版本 2）记录时，恢复其中的参数并立即按新格式保存，
 * 免得刷新固件后丢失标定。
 *
 * @return 恢复的关节数。
 * @note 需要擦除备用扇区时会阻塞约 2 秒，只在初始化阶段调用。
 */
static unsigned load_calibration(void)
{
    /* 必须在 init 之前读旧记录：init 换用新长度后可能擦掉旧记录所在扇区。
     * 先找版本 3，没有再找版本 2。 */
    static uint8_t old[JOINT_CAL_BLOB_V3_SIZE];
    uint32_t old_size = JOINT_CAL_BLOB_V3_SIZE;
    bool has_old = bsp_param_flash_peek(old_size, old) == BSP_OK;
    if (!has_old)
    {
        old_size = JOINT_CAL_BLOB_V2_SIZE;
        has_old = bsp_param_flash_peek(old_size, old) == BSP_OK;
    }
    bsp_status_t status = bsp_param_flash_init(JOINT_CAL_BLOB_SIZE);
    s_is_store_ok = (status == BSP_OK);
    /* 备用扇区擦除失败（ERR_HAL）时已有记录仍可读，只是之后保存可能失败。 */
    uint8_t blob[JOINT_CAL_BLOB_SIZE];
    if ((status != BSP_OK) && (status != BSP_ERR_HAL))
    {
        return 0U;
    }
    if (bsp_param_flash_load(blob) == BSP_OK)
    {
        return joint_cal_import(&s_joints, blob, sizeof(blob));
    }
    unsigned restored =
        has_old ? joint_cal_import(&s_joints, old, old_size) : 0U;
    /* 版本 3 即使没有已标定的关节也带着控制参数，同样要转存。 */
    if (restored > 0U || (has_old && old_size == JOINT_CAL_BLOB_V3_SIZE))
    {
        (void)joint_service_save();
    }
    return restored;
}

/** @brief 过采样一次六路电位器，滤波后交给关节控制。 */
static void sample(void)
{
    static uint16_t samples[POT_OVERSAMPLE][BSP_ADC_CHANNEL_COUNT];
    s_is_pot_valid = s_is_adc_ok;
    for (unsigned i = 0; i < POT_OVERSAMPLE && s_is_pot_valid; ++i)
    {
        s_is_pot_valid = bsp_adc_read_all(samples[i], ADC_TIMEOUT_MS) == BSP_OK;
    }
    if (s_is_pot_valid)
    {
        uint16_t now_raw[BSP_ADC_CHANNEL_COUNT];
        joint_filter((const uint16_t (*)[JOINT_COUNT])samples, POT_OVERSAMPLE,
                     now_raw, s_pot_spread);
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
    joint_feed(&s_joints, s_pot_cache, s_pot_spread, s_is_pot_valid,
               HAL_GetTick());
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
        sample();
    }
    return joint_tick(&s_joints, HAL_GetTick());
}

bool joint_service_save(void)
{
    uint8_t blob[JOINT_CAL_BLOB_SIZE];
    joint_cal_export(&s_joints, blob);
    return s_is_store_ok && bsp_param_flash_save(blob) == BSP_OK;
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
