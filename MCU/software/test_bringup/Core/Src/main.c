/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "bsp_buzzer.h"
#include "bsp_common.h"
#include "bsp_key.h"
#include "bsp_led.h"
#include "bsp_light.h"
#include "bsp_linux_power.h"
#include "bsp_linux_uart.h"
#include "bsp_log.h"
#include "bsp_motor.h"
#include "task.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* ==================== 测试选择 ====================
   一次只编译一个测试：灯板 PWM 或电机 PWM。 */
#define BRINGUP_TEST_NONE  (0) /* 只留空循环 */
#define BRINGUP_TEST_LAMP  (1) /* 灯板七路 PWM */
#define BRINGUP_TEST_MOTOR (2) /* 电机十六路 PWM */

#define BRINGUP_TEST (BRINGUP_TEST_MOTOR)

/* 诊断开关：置 1 时跳过 PWM 测试，把 BRINGUP_GPIO_PINS 改成普通推挽输出并
   输出 1Hz 方波。默认为 UP1(PE9)+UP2(PE11)，可改成任意引脚（例如某路电机）。
   用于区分断点在「MCU 引脚」还是「走线/串阻/连接器」；测完置回 0。 */
#define BRINGUP_GPIO_PROBE (0)
#define BRINGUP_GPIO_PORT  GPIOE
#define BRINGUP_GPIO_PINS  (UP_PWM1_Pin | UP_PWM2_Pin)

#if BRINGUP_GPIO_PROBE
#define BRINGUP_MODE BRINGUP_TEST_NONE
#else
#define BRINGUP_MODE BRINGUP_TEST
#endif

/* ---- 灯板测试观察参数，单位毫秒 ----
   保持时间要足够把示波器探头移到目标引脚并读完频率/占空比。
   范围：先上灯板 UP1~UP4，再下灯板 DOWN1~DOWN3。两块板走不同定时器与链路
   （上：TIM1，U7→RN1→CN7；下：TIM3，U7→RN2→U29，见 MCU/hardware/已知硬件问题.md）。 */
#define LAMP_TEST_HOLD_MS  (10000U) /* 单路输出的保持时间（观察窗口） */
#define LAMP_TEST_GAP_MS   (2000U)  /* 两路之间的熄灭间隔 */
#define LAMP_TEST_SWEEP_MS (10000U) /* 每档占空比的保持时间 */
#define LAMP_TEST_FIRST_ID BSP_LIGHT_UP1
#define LAMP_TEST_LAST_ID  BSP_LIGHT_DOWN3

/* ---- 电机测试观察参数，单位毫秒 ----
   先全部 FI 50%、再全部 BI 50%，每相各保持 HOLD 毫秒，两次之间回滑行。
   全部同时输出时所有引脚都有波形，探头放在任意一路都能读到，便于逐脚确认；
   各电机的 MCU 引脚、串联 100Ω 电阻与 RZ7899 编号见 MCU/hardware/已知硬件问题.md；
   注意连接器编号与电机编号不对应（M1=CN9、M6=CN1）。
   @warning 全部同时输出时，若电机已接线且电机电源已上，八台电机会同时同向转动。
            首次确认 PWM 通路前请断开电机或断开电机电源。 */
#define MOTOR_TEST_HOLD_MS (10000U) /* 每一相的保持时间（观察窗口） */
#define MOTOR_TEST_GAP_MS  (2000U)  /* 两相之间回滑行的间隔 */
#define MOTOR_TEST_FIRST_ID BSP_MOTOR_M1
#define MOTOR_TEST_LAST_ID  BSP_MOTOR_M8

/* 电机测试方式：1 = 范围内全部电机同时输出（便于逐脚量）；
   0 = 一次只动一台的一个方向（某几路没波形时用来定位）。 */
#define MOTOR_TEST_ALL_AT_ONCE (1)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
#if BRINGUP_MODE == BRINGUP_TEST_LAMP
static void lamp_test_run(void);
#elif BRINGUP_MODE == BRINGUP_TEST_MOTOR
static void motor_test_run(void);
#endif
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#if BRINGUP_MODE == BRINGUP_TEST_LAMP
/**
 * @brief 逐路输出 PWM 并做三档占空比扫描，用于确认灯板 PWM 通路。
 *
 * @note 逐路用 50% 方波：0% 和 100% 都是直流，只有中间占空比才能同时量到
 *       频率和占空比。TIM1/TIM3 均为 20kHz。
 * @note 每路保持 LAMP_TEST_HOLD_MS 再切换，便于把示波器探头移到对应引脚。
 *       要延长或缩短观察窗口，只改本文件顶部的 LAMP_TEST_* 宏。
 * @note 三档扫描中 1000 对应引脚恒为高电平（直流），用于确认 100% 输出正确。
 * @note 测试范围由 LAMP_TEST_FIRST_ID / LAMP_TEST_LAST_ID 决定。
 */
static void lamp_test_run(void)
{
    static const char *const lamp_names[BSP_LIGHT_COUNT] = {
        "UP1", "UP2", "UP3", "UP4", "DOWN1", "DOWN2", "DOWN3"};
    static const uint16_t sweep_duty[3] = {250U, 500U, 1000U};

    const uint32_t first = (uint32_t)LAMP_TEST_FIRST_ID;
    const uint32_t last = (uint32_t)LAMP_TEST_LAST_ID;

    LOG_INFO("LAMP", "one-by-one, 50%%, expect 20kHz");
    for (uint32_t id = first; id <= last; ++id)
    {
        LOG_INFO("LAMP", "%s = 500 (50%%)", lamp_names[id]);
        (void)bsp_light_set_duty((bsp_light_id_t)id, 500U);
        HAL_Delay(LAMP_TEST_HOLD_MS);
        (void)bsp_light_stop((bsp_light_id_t)id);
        HAL_Delay(LAMP_TEST_GAP_MS);
    }

    LOG_INFO("LAMP", "duty sweep: 250 / 500 / 1000");
    for (uint32_t step = 0U; step < 3U; ++step)
    {
        for (uint32_t id = first; id <= last; ++id)
        {
            (void)bsp_light_set_duty((bsp_light_id_t)id, sweep_duty[step]);
        }
        LOG_INFO("LAMP", "all = %u (%u%%)",
                 (unsigned int)sweep_duty[step],
                 (unsigned int)(sweep_duty[step] / 10U));
        HAL_Delay(LAMP_TEST_SWEEP_MS);
    }

    for (uint32_t id = first; id <= last; ++id)
    {
        (void)bsp_light_stop((bsp_light_id_t)id);
    }
    LOG_INFO("LAMP", "done");
}
#elif BRINGUP_MODE == BRINGUP_TEST_MOTOR
/**
 * @brief 输出电机 PWM，用于在接电机、上电机电源之前确认十六路 PWM 通路。
 *
 * @note 默认 MOTOR_TEST_ALL_AT_ONCE = 1：范围内全部电机先一起 FI 50%、
 *       再一起 BI 50%，中间回滑行。此时所有被测引脚同时有波形，探头放在任意一路
 *       都能读到，便于逐脚确认；某一相没波形即可直接定位到该路。
 *       置 0 则一次只动一台的一个方向，用于进一步隔离。
 * @note 驱动的是 RZ7899 的逻辑输入；电机未接或电机电源未上时不会转动。
 * @note 逐路用 50% 方波：0% 和 100% 都是直流，量不到频率和占空比。四路定时器均为 20kHz。
 * @note 本测试也顺带核对 M6 的 FI/BI 反序：日志里 "FI" 应对应 PC9/U7.66，
 *       "BI" 应对应 PC8/U7.65。
 * @note 范围与时间由 MOTOR_TEST_* 宏控制。
 * @warning 全部同时输出时，若电机已接线且电机电源已上，八台电机会同时同向转动；
 *          首次确认 PWM 通路前请断开电机或断开电机电源。
 */
static void motor_test_run(void)
{
#if !MOTOR_TEST_ALL_AT_ONCE
    static const char *const motor_names[BSP_MOTOR_COUNT] = {
        "M1", "M2", "M3", "M4", "M5", "M6", "M7", "M8"};
#endif

    const uint32_t first = (uint32_t)MOTOR_TEST_FIRST_ID;
    const uint32_t last = (uint32_t)MOTOR_TEST_LAST_ID;

#if MOTOR_TEST_ALL_AT_ONCE
    LOG_INFO("MOTOR", "M%u..M%u FI = 500 (50%%), expect 20kHz",
             (unsigned int)(first + 1U), (unsigned int)(last + 1U));
    for (uint32_t id = first; id <= last; ++id)
    {
        (void)bsp_motor_set_duty((bsp_motor_id_t)id, 500);
    }
    HAL_Delay(MOTOR_TEST_HOLD_MS);
    for (uint32_t id = first; id <= last; ++id)
    {
        (void)bsp_motor_coast((bsp_motor_id_t)id);
    }
    HAL_Delay(MOTOR_TEST_GAP_MS);

    LOG_INFO("MOTOR", "M%u..M%u BI = 500 (50%%)",
             (unsigned int)(first + 1U), (unsigned int)(last + 1U));
    for (uint32_t id = first; id <= last; ++id)
    {
        (void)bsp_motor_set_duty((bsp_motor_id_t)id, -500);
    }
    HAL_Delay(MOTOR_TEST_HOLD_MS);
    for (uint32_t id = first; id <= last; ++id)
    {
        (void)bsp_motor_coast((bsp_motor_id_t)id);
    }
    HAL_Delay(MOTOR_TEST_GAP_MS);

    LOG_INFO("MOTOR", "done");
#else
    LOG_INFO("MOTOR", "one-by-one, 50%%, expect 20kHz");
    for (uint32_t id = first; id <= last; ++id)
    {
        LOG_INFO("MOTOR", "%s FI = 500 (50%%)", motor_names[id]);
        (void)bsp_motor_set_duty((bsp_motor_id_t)id, 500);
        HAL_Delay(MOTOR_TEST_HOLD_MS);
        (void)bsp_motor_coast((bsp_motor_id_t)id);
        HAL_Delay(MOTOR_TEST_GAP_MS);

        LOG_INFO("MOTOR", "%s BI = 500 (50%%)", motor_names[id]);
        (void)bsp_motor_set_duty((bsp_motor_id_t)id, -500);
        HAL_Delay(MOTOR_TEST_HOLD_MS);
        (void)bsp_motor_coast((bsp_motor_id_t)id);
        HAL_Delay(MOTOR_TEST_GAP_MS);
    }
    LOG_INFO("MOTOR", "done");
#endif /* MOTOR_TEST_ALL_AT_ONCE */
}
#endif /* BRINGUP_MODE */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM4_Init();
  MX_TIM8_Init();
  MX_TIM9_Init();
  MX_TIM12_Init();
  MX_TIM13_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */
    (void)bsp_led_init();  /* 出错指示先就绪 */
    if (bsp_log_init() != BSP_OK)
    {
        (void)bsp_led_set(BSP_LED_ID_ERROR, true);
        Error_Handler();
    }
    (void)bsp_key_init();
    (void)bsp_linux_power_init();

#if BRINGUP_GPIO_PROBE
    /* 诊断模式：把指定引脚从定时器复用改为普通推挽输出 */
    {
        GPIO_InitTypeDef gpio_probe = {0};

        gpio_probe.Pin = BRINGUP_GPIO_PINS;
        gpio_probe.Mode = GPIO_MODE_OUTPUT_PP;
        gpio_probe.Pull = GPIO_NOPULL;
        gpio_probe.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(BRINGUP_GPIO_PORT, &gpio_probe);
    }
    LOG_INFO("PROBE", "GPIO 1Hz toggle");
#elif BRINGUP_MODE == BRINGUP_TEST_LAMP
    bsp_status_t init_status = bsp_light_init();
    if (init_status != BSP_OK)
    {
        LOG_ERROR("LAMP", "init failed: %s", bsp_status_str(init_status));
        (void)bsp_led_set(BSP_LED_ID_ERROR, true);
        Error_Handler();
    }

    /* 电机不参与本次测试，但显式置为滑行（两路低电平），避免引脚状态不确定 */
    init_status = bsp_motor_init();
    if (init_status != BSP_OK)
    {
        LOG_WARN("MOTOR", "init failed: %s (ignored)",
                 bsp_status_str(init_status));
    }

    init_status = bsp_buzzer_init();
    if (init_status != BSP_OK)
    {
        LOG_ERROR("BUZZ", "init failed: %s", bsp_status_str(init_status));
        (void)bsp_led_set(BSP_LED_ID_ERROR, true);
        Error_Handler();
    }
    LOG_INFO("LAMP", "lamp bring-up test");
#elif BRINGUP_MODE == BRINGUP_TEST_MOTOR
    bsp_status_t init_status = bsp_motor_init();
    if (init_status != BSP_OK)
    {
        LOG_ERROR("MOTOR", "init failed: %s", bsp_status_str(init_status));
        (void)bsp_led_set(BSP_LED_ID_ERROR, true);
        Error_Handler();
    }

    /* 灯板不参与本次测试，但显式输出 0%，避免其输入悬空 */
    init_status = bsp_light_init();
    if (init_status == BSP_OK)
    {
        for (uint32_t i = 0U; i < (uint32_t)BSP_LIGHT_COUNT; ++i)
        {
            (void)bsp_light_set_duty((bsp_light_id_t)i, 0U);
        }
    }
    else
    {
        LOG_WARN("LAMP", "init failed: %s (ignored)",
                 bsp_status_str(init_status));
    }
    LOG_INFO("MOTOR", "motor PWM bring-up test");
#endif /* BRINGUP_MODE */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
    while (1)
    {
#if BRINGUP_GPIO_PROBE
        HAL_GPIO_TogglePin(BRINGUP_GPIO_PORT, BRINGUP_GPIO_PINS);
        LOG_INFO("PROBE", "toggle");
        HAL_Delay(500U);
#elif BRINGUP_MODE == BRINGUP_TEST_LAMP
        lamp_test_run();
        HAL_Delay(2000U);
#elif BRINGUP_MODE == BRINGUP_TEST_MOTOR
        motor_test_run();
        HAL_Delay(2000U);
#endif /* BRINGUP_MODE */
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* HAL 弱回调统一放在 Core/Src/hal_callback_dispatch.c，不在 main.c 定义。 */
/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
    bsp_led_set(BSP_LED_ID_ERROR, true);
    /* User can add his own implementation to report the HAL error return state
     */
    __disable_irq();
    while (1)
    {
    }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
    /* User can add his own implementation to report the file name and line
       number, ex: printf("Wrong parameters value: file %s on line %d\r\n",
       file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
