/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define DOWN_PWM3_Pin GPIO_PIN_5
#define DOWN_PWM3_GPIO_Port GPIOE
#define LINUX_PWR_PULSE_Pin GPIO_PIN_6
#define LINUX_PWR_PULSE_GPIO_Port GPIOE
#define POT1_ADC_Pin GPIO_PIN_0
#define POT1_ADC_GPIO_Port GPIOC
#define POT2_ADC_Pin GPIO_PIN_1
#define POT2_ADC_GPIO_Port GPIOC
#define POT3_ADC_Pin GPIO_PIN_2
#define POT3_ADC_GPIO_Port GPIOC
#define POT4_ADC_Pin GPIO_PIN_3
#define POT4_ADC_GPIO_Port GPIOC
#define M3_FI_PWM_Pin GPIO_PIN_0
#define M3_FI_PWM_GPIO_Port GPIOA
#define M3_BI_PWM_Pin GPIO_PIN_1
#define M3_BI_PWM_GPIO_Port GPIOA
#define M4_FI_PWM_Pin GPIO_PIN_2
#define M4_FI_PWM_GPIO_Port GPIOA
#define M4_BI_PWM_Pin GPIO_PIN_3
#define M4_BI_PWM_GPIO_Port GPIOA
#define LED_COMM_Pin GPIO_PIN_4
#define LED_COMM_GPIO_Port GPIOA
#define LED_ACT_Pin GPIO_PIN_5
#define LED_ACT_GPIO_Port GPIOA
#define POT5_ADC_Pin GPIO_PIN_4
#define POT5_ADC_GPIO_Port GPIOC
#define POT6_ADC_Pin GPIO_PIN_5
#define POT6_ADC_GPIO_Port GPIOC
#define M6_FI_PWM_Pin GPIO_PIN_0
#define M6_FI_PWM_GPIO_Port GPIOB
#define M6_BI_PWM_Pin GPIO_PIN_1
#define M6_BI_PWM_GPIO_Port GPIOB
#define HOME_KEY_Pin GPIO_PIN_7
#define HOME_KEY_GPIO_Port GPIOE
#define HOME_KEY_EXTI_IRQn EXTI9_5_IRQn
#define M1_FI_PWM_Pin GPIO_PIN_9
#define M1_FI_PWM_GPIO_Port GPIOE
#define M1_BI_PWM_Pin GPIO_PIN_11
#define M1_BI_PWM_GPIO_Port GPIOE
#define M2_FI_PWM_Pin GPIO_PIN_13
#define M2_FI_PWM_GPIO_Port GPIOE
#define M2_BI_PWM_Pin GPIO_PIN_14
#define M2_BI_PWM_GPIO_Port GPIOE
#define LED_STATE_Pin GPIO_PIN_12
#define LED_STATE_GPIO_Port GPIOB
#define LED_ERR_Pin GPIO_PIN_13
#define LED_ERR_GPIO_Port GPIOB
#define DOWN_PWM1_Pin GPIO_PIN_14
#define DOWN_PWM1_GPIO_Port GPIOB
#define DOWN_PWM2_Pin GPIO_PIN_15
#define DOWN_PWM2_GPIO_Port GPIOB
#define LINUX_UART_TX_Pin GPIO_PIN_8
#define LINUX_UART_TX_GPIO_Port GPIOD
#define LINUX_UART_RX_Pin GPIO_PIN_9
#define LINUX_UART_RX_GPIO_Port GPIOD
#define M7_FI_PWM_Pin GPIO_PIN_12
#define M7_FI_PWM_GPIO_Port GPIOD
#define M7_BI_PWM_Pin GPIO_PIN_13
#define M7_BI_PWM_GPIO_Port GPIOD
#define M8_FI_PWM_Pin GPIO_PIN_14
#define M8_FI_PWM_GPIO_Port GPIOD
#define M8_BI_PWM_Pin GPIO_PIN_15
#define M8_BI_PWM_GPIO_Port GPIOD
#define UP_PWM1_Pin GPIO_PIN_6
#define UP_PWM1_GPIO_Port GPIOC
#define UP_PWM2_Pin GPIO_PIN_7
#define UP_PWM2_GPIO_Port GPIOC
#define UP_PWM3_Pin GPIO_PIN_8
#define UP_PWM3_GPIO_Port GPIOC
#define UP_PWM4_Pin GPIO_PIN_9
#define UP_PWM4_GPIO_Port GPIOC
#define DBG_UART_TX_Pin GPIO_PIN_5
#define DBG_UART_TX_GPIO_Port GPIOD
#define DBG_UART_RX_Pin GPIO_PIN_6
#define DBG_UART_RX_GPIO_Port GPIOD
#define M5_FI_PWM_Pin GPIO_PIN_4
#define M5_FI_PWM_GPIO_Port GPIOB
#define M5_BI_PWM_Pin GPIO_PIN_5
#define M5_BI_PWM_GPIO_Port GPIOB
#define BUZZER_Pin GPIO_PIN_8
#define BUZZER_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
