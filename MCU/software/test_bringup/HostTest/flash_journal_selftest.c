/**
 * @file flash_journal_selftest.c
 * @brief flash_journal 的 PC 端自测：用内存模拟 NOR Flash，不需要 MCU。
 *
 * 模拟器按 NOR 语义工作：擦除置 0xFF，编程只能把 1 变 0（新值 = 旧值 & 写入值）。
 * 可以注入擦除/编程失败，或在写到第 N 个字时“掉电”，验证加载总能退回到
 * 最后一条完整写入的记录。
 *
 * 编译运行见同目录 run.ps1。
 *
 * @note 本文件不属于固件，不参与 arm 构建。输出一律 ASCII。
 */

#include <stdio.h>
#include <string.h>

#include "flash_journal.h"

/* -------------------------------------------------------------------------- */
/* 断言小工具                                                                  */
/* -------------------------------------------------------------------------- */

static unsigned s_passed = 0U;
static unsigned s_failed = 0U;

static void check(bool condition, const char *what)
{
    if (condition)
    {
        ++s_passed;
        printf("[PASS] %s\n", what);
    }
    else
    {
        ++s_failed;
        printf("[FAIL] %s\n", what);
    }
}

/* -------------------------------------------------------------------------- */
/* 模拟 Flash                                                                  */
/* -------------------------------------------------------------------------- */

/* 每块区 4 个槽多一点：参数 16 字节，每条记录 32 字节。 */
#define PAYLOAD_SIZE (16U)
#define AREA_SIZE (4U * (PAYLOAD_SIZE + FLASH_JOURNAL_OVERHEAD) + 8U)
#define SLOTS_PER_AREA (4U)

typedef struct
{
    uint32_t words[2][AREA_SIZE / 4U];
    unsigned erase_count[2];
    bool fail_erase;
    bool fail_program;
    /** @brief 大于 0 时，本次编程只写入这么多个字就“掉电”。 */
    uint32_t cut_after_words;
} fake_flash_t;

static fake_flash_t s_flash;

static bool fake_erase(void *ctx, unsigned area)
{
    fake_flash_t *f = ctx;
    if (f->fail_erase)
    {
        return false;
    }
    memset(f->words[area], 0xFF, sizeof(f->words[area]));
    ++f->erase_count[area];
    return true;
}

static bool fake_program(void *ctx, unsigned area, uint32_t offset,
                         const uint32_t *words, uint32_t count)
{
    fake_flash_t *f = ctx;
    if (f->fail_program)
    {
        return false;
    }
    if (f->cut_after_words > 0U && f->cut_after_words < count)
    {
        count = f->cut_after_words;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        f->words[area][offset / 4U + i] &= words[i];
    }
    return f->cut_after_words == 0U;
}

static const flash_journal_ops_t k_ops = {fake_erase, fake_program, &s_flash};

static void flash_reset(uint8_t fill)
{
    memset(&s_flash, 0, sizeof(s_flash));
    memset(s_flash.words, fill, sizeof(s_flash.words));
}

/** @brief 模拟重新上电：在当前 Flash 内容上重新初始化。 */
static void reboot(flash_journal_t *j)
{
    const uint8_t *areas[2] = {(const uint8_t *)s_flash.words[0],
                               (const uint8_t *)s_flash.words[1]};
    (void)flash_journal_init(j, k_ops, areas, AREA_SIZE, PAYLOAD_SIZE);
}

static void make_payload(uint8_t out[PAYLOAD_SIZE], uint8_t tag)
{
    for (unsigned i = 0; i < PAYLOAD_SIZE; ++i)
    {
        out[i] = (uint8_t)(tag + i);
    }
}

static bool loads_tag(const flash_journal_t *j, uint8_t tag)
{
    uint8_t expected[PAYLOAD_SIZE];
    uint8_t actual[PAYLOAD_SIZE];
    make_payload(expected, tag);
    return flash_journal_load(j, actual) == BSP_OK &&
           memcmp(expected, actual, PAYLOAD_SIZE) == 0;
}

/* -------------------------------------------------------------------------- */
/* 用例                                                                        */
/* -------------------------------------------------------------------------- */

static void test_bad_arguments(void)
{
    flash_journal_t j;
    const uint8_t *areas[2] = {(const uint8_t *)s_flash.words[0],
                               (const uint8_t *)s_flash.words[1]};
    check(flash_journal_init(&j, k_ops, areas, AREA_SIZE, 0U) == BSP_ERR_PARAM,
          "args: zero payload rejected");
    check(flash_journal_init(&j, k_ops, areas, AREA_SIZE, 6U) == BSP_ERR_PARAM,
          "args: payload not multiple of 4 rejected");
    check(flash_journal_init(&j, k_ops, areas, 16U, PAYLOAD_SIZE) ==
              BSP_ERR_PARAM,
          "args: area smaller than one record rejected");
}

static void test_empty_flash(void)
{
    flash_journal_t j;
    uint8_t buffer[PAYLOAD_SIZE] = {0};
    flash_reset(0xFF);
    reboot(&j);
    check(flash_journal_load(&j, buffer) == BSP_IDLE, "empty: nothing to load");
    check(flash_journal_prepare(&j) == BSP_OK &&
              s_flash.erase_count[0] + s_flash.erase_count[1] == 0U,
          "empty: prepare does not erase blank flash");
}

static void test_save_and_reload(void)
{
    flash_journal_t j;
    uint8_t payload[PAYLOAD_SIZE];
    flash_reset(0xFF);
    reboot(&j);
    make_payload(payload, 10U);
    check(flash_journal_save(&j, payload) == BSP_OK, "save: first record");
    check(loads_tag(&j, 10U), "save: loads back before reboot");
    make_payload(payload, 20U);
    check(flash_journal_save(&j, payload) == BSP_OK, "save: second record");
    reboot(&j);
    check(loads_tag(&j, 20U), "save: newest record survives reboot");
    check(s_flash.erase_count[0] + s_flash.erase_count[1] == 0U,
          "save: appending never erases");
}

static void test_rotation(void)
{
    flash_journal_t j;
    uint8_t payload[PAYLOAD_SIZE];
    flash_reset(0xFF);
    reboot(&j);
    /* 写满区 0 再写两条：第五条换到区 1，区 0 的旧记录保留到下次换区。 */
    for (uint8_t tag = 1U; tag <= SLOTS_PER_AREA + 2U; ++tag)
    {
        make_payload(payload, tag);
        (void)flash_journal_save(&j, payload);
    }
    check(j.latest_area == 1U && loads_tag(&j, SLOTS_PER_AREA + 2U),
          "rotate: switches to the second area when the first is full");
    check(s_flash.erase_count[0] + s_flash.erase_count[1] == 0U,
          "rotate: blank spare area is used without erasing");

    reboot(&j);
    check(loads_tag(&j, SLOTS_PER_AREA + 2U),
          "rotate: newest wins over older records in the other area");
    check(flash_journal_prepare(&j) == BSP_OK && s_flash.erase_count[0] == 1U,
          "rotate: prepare erases the stale spare area");

    /* 再写满区 1，回到已预擦除的区 0，不应在保存时擦除。 */
    for (uint8_t tag = 50U; tag < 50U + SLOTS_PER_AREA; ++tag)
    {
        make_payload(payload, tag);
        (void)flash_journal_save(&j, payload);
    }
    check(j.latest_area == 0U && loads_tag(&j, 50U + SLOTS_PER_AREA - 1U) &&
              s_flash.erase_count[0] == 1U,
          "rotate: wraps back to the prepared area without a runtime erase");
}

static void test_runtime_erase_when_spare_dirty(void)
{
    flash_journal_t j;
    uint8_t payload[PAYLOAD_SIZE];
    flash_reset(0xFF);
    reboot(&j);
    /* 不调用 prepare：写满两块区后，再写必须在保存时擦除旧区。 */
    for (uint8_t tag = 1U; tag <= 2U * SLOTS_PER_AREA + 1U; ++tag)
    {
        make_payload(payload, tag);
        (void)flash_journal_save(&j, payload);
    }
    check(s_flash.erase_count[0] == 1U &&
              loads_tag(&j, 2U * SLOTS_PER_AREA + 1U),
          "erase: dirty spare area is erased on demand");
    reboot(&j);
    check(loads_tag(&j, 2U * SLOTS_PER_AREA + 1U),
          "erase: newest record survives reboot after runtime erase");
}

static void test_power_loss_mid_write(void)
{
    flash_journal_t j;
    uint8_t payload[PAYLOAD_SIZE];
    flash_reset(0xFF);
    reboot(&j);
    make_payload(payload, 7U);
    (void)flash_journal_save(&j, payload);

    /* 每个可能的中断点都试一遍：都必须退回第一条完整记录。 */
    bool all_recovered = true;
    for (uint32_t cut = 1U; cut < (PAYLOAD_SIZE + FLASH_JOURNAL_OVERHEAD) / 4U;
         ++cut)
    {
        flash_reset(0xFF);
        reboot(&j);
        make_payload(payload, 7U);
        (void)flash_journal_save(&j, payload);
        s_flash.cut_after_words = cut;
        make_payload(payload, 8U);
        (void)flash_journal_save(&j, payload);
        s_flash.cut_after_words = 0U;
        reboot(&j);
        all_recovered = all_recovered && loads_tag(&j, 7U);
    }
    check(all_recovered, "power loss: half-written record is ignored");

    /* 残缺记录占住的槽不能再写；下一次保存要写到后面的空槽。 */
    make_payload(payload, 9U);
    check(flash_journal_save(&j, payload) == BSP_OK && loads_tag(&j, 9U),
          "power loss: next save skips the damaged slot");
    reboot(&j);
    check(loads_tag(&j, 9U), "power loss: recovered record survives reboot");
}

static void test_failures_keep_previous(void)
{
    flash_journal_t j;
    uint8_t payload[PAYLOAD_SIZE];
    flash_reset(0xFF);
    reboot(&j);
    make_payload(payload, 30U);
    (void)flash_journal_save(&j, payload);

    s_flash.fail_program = true;
    make_payload(payload, 31U);
    check(flash_journal_save(&j, payload) == BSP_ERR_HAL && loads_tag(&j, 30U),
          "failure: program error keeps the previous record");
    s_flash.fail_program = false;

    /* 区 0 已用掉 2 个槽（一条成功、一条编程失败）。再写 10 条：
     * 写满区 0 → 换到区 1 写满 → 擦除区 0 再写满。此时写入区已满、
     * 另一块区有旧数据，下一次保存必须擦除，让这次擦除失败。 */
    for (uint8_t tag = 40U; tag < 40U + 2U * SLOTS_PER_AREA + 2U; ++tag)
    {
        make_payload(payload, tag);
        (void)flash_journal_save(&j, payload);
    }
    uint8_t last = (uint8_t)(40U + 2U * SLOTS_PER_AREA + 1U);
    check(j.next_slot == SLOTS_PER_AREA, "failure: write area is full");
    s_flash.fail_erase = true;
    make_payload(payload, 99U);
    bsp_status_t status = flash_journal_save(&j, payload);
    s_flash.fail_erase = false;
    check(status == BSP_ERR_HAL && loads_tag(&j, last),
          "failure: erase error keeps the previous record");
}

static void test_garbage_flash(void)
{
    flash_journal_t j;
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t buffer[PAYLOAD_SIZE];
    /* 两块区都是非 0xFF 的旧内容：没有有效记录，保存时需擦除。 */
    flash_reset(0x5A);
    reboot(&j);
    check(flash_journal_load(&j, buffer) == BSP_IDLE,
          "garbage: random content is not a record");
    check(flash_journal_prepare(&j) == BSP_OK && s_flash.erase_count[1] == 1U,
          "garbage: prepare erases the spare area");
    make_payload(payload, 60U);
    check(flash_journal_save(&j, payload) == BSP_OK && loads_tag(&j, 60U),
          "garbage: first save lands in the prepared area");
    reboot(&j);
    check(loads_tag(&j, 60U), "garbage: record survives reboot");
}

int main(void)
{
    test_bad_arguments();
    test_empty_flash();
    test_save_and_reload();
    test_rotation();
    test_runtime_erase_when_spare_dirty();
    test_power_loss_mid_write();
    test_failures_keep_previous();
    test_garbage_flash();

    printf("\n%u passed, %u failed\n", s_passed, s_failed);
    return (s_failed == 0U) ? 0 : 1;
}
