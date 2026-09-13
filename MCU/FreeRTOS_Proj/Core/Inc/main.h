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
#define DOWN_LAMP_PWM3_Pin GPIO_PIN_5
#define DOWN_LAMP_PWM3_GPIO_Port GPIOE
#define ADC_IN1_Pin GPIO_PIN_0
#define ADC_IN1_GPIO_Port GPIOC
#define ADC_IN2_Pin GPIO_PIN_1
#define ADC_IN2_GPIO_Port GPIOC
#define ADC_IN3_Pin GPIO_PIN_2
#define ADC_IN3_GPIO_Port GPIOC
#define ADC_IN4_Pin GPIO_PIN_3
#define ADC_IN4_GPIO_Port GPIOC
#define MOTOR_PWM05_Pin GPIO_PIN_0
#define MOTOR_PWM05_GPIO_Port GPIOA
#define MOTOR_PWM06_Pin GPIO_PIN_1
#define MOTOR_PWM06_GPIO_Port GPIOA
#define MOTOR_PWM07_Pin GPIO_PIN_2
#define MOTOR_PWM07_GPIO_Port GPIOA
#define MOTOR_PWM08_Pin GPIO_PIN_3
#define MOTOR_PWM08_GPIO_Port GPIOA
#define ADC_IN5_Pin GPIO_PIN_4
#define ADC_IN5_GPIO_Port GPIOC
#define ADC_IN6_Pin GPIO_PIN_5
#define ADC_IN6_GPIO_Port GPIOC
#define MOTOR_PWM11_Pin GPIO_PIN_0
#define MOTOR_PWM11_GPIO_Port GPIOB
#define MOTOR_PWM12_Pin GPIO_PIN_1
#define MOTOR_PWM12_GPIO_Port GPIOB
#define MOTOR_PWM01_Pin GPIO_PIN_9
#define MOTOR_PWM01_GPIO_Port GPIOE
#define MOTOR_PWM02_Pin GPIO_PIN_11
#define MOTOR_PWM02_GPIO_Port GPIOE
#define MOTOR_PWM03_Pin GPIO_PIN_13
#define MOTOR_PWM03_GPIO_Port GPIOE
#define MOTOR_PWM04_Pin GPIO_PIN_14
#define MOTOR_PWM04_GPIO_Port GPIOE
#define EXT_I2C_SCL_Pin GPIO_PIN_10
#define EXT_I2C_SCL_GPIO_Port GPIOB
#define EXT_I2C_SDA_Pin GPIO_PIN_11
#define EXT_I2C_SDA_GPIO_Port GPIOB
#define DOWN_LAMP_PWM1_Pin GPIO_PIN_14
#define DOWN_LAMP_PWM1_GPIO_Port GPIOB
#define DOWN_LAMP_PWM2_Pin GPIO_PIN_15
#define DOWN_LAMP_PWM2_GPIO_Port GPIOB
#define MCU_LINUX_TX_Pin GPIO_PIN_8
#define MCU_LINUX_TX_GPIO_Port GPIOD
#define MCU_LINUX_RX_Pin GPIO_PIN_9
#define MCU_LINUX_RX_GPIO_Port GPIOD
#define MOTOR_PWM13_Pin GPIO_PIN_12
#define MOTOR_PWM13_GPIO_Port GPIOD
#define MOTOR_PWM14_Pin GPIO_PIN_13
#define MOTOR_PWM14_GPIO_Port GPIOD
#define MOTOR_PWM15_Pin GPIO_PIN_14
#define MOTOR_PWM15_GPIO_Port GPIOD
#define MOTOR_PWM16_Pin GPIO_PIN_15
#define MOTOR_PWM16_GPIO_Port GPIOD
#define UP_LAMP_PWM1_Pin GPIO_PIN_6
#define UP_LAMP_PWM1_GPIO_Port GPIOC
#define UP_LAMP_PWM2_Pin GPIO_PIN_7
#define UP_LAMP_PWM2_GPIO_Port GPIOC
#define UP_LAMP_PWM3_Pin GPIO_PIN_8
#define UP_LAMP_PWM3_GPIO_Port GPIOC
#define UP_LAMP_PWM4_Pin GPIO_PIN_9
#define UP_LAMP_PWM4_GPIO_Port GPIOC
#define DBG_UART_TX_Pin GPIO_PIN_5
#define DBG_UART_TX_GPIO_Port GPIOD
#define DBG_UART_RX_Pin GPIO_PIN_6
#define DBG_UART_RX_GPIO_Port GPIOD
#define MOTOR_PWM09_Pin GPIO_PIN_4
#define MOTOR_PWM09_GPIO_Port GPIOB
#define MOTOR_PWM10_Pin GPIO_PIN_5
#define MOTOR_PWM10_GPIO_Port GPIOB
#define LED1_Pin GPIO_PIN_1
#define LED1_GPIO_Port GPIOE

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
