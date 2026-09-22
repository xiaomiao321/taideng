# MCU 接口与 PWM 分配（电机顺序更新版 v2）

STM32F407VGT6，MCU 位于 PCB 底层，位置按镜像视图描述。

## 电机 PWM

| 物理电机 | 正向 PWM | 反向 PWM | MCU 引脚 | 定时器 |
|---|---|---|---|---|
| M1 | `M1_FI_PWM` | `M1_BI_PWM` | PA2、PA3 | TIM2_CH3、CH4 |
| M2 | `M2_FI_PWM` | `M2_BI_PWM` | PA0、PA1 | TIM2_CH1、CH2 |
| M3 | `M3_FI_PWM` | `M3_BI_PWM` | PE5、PE6 | TIM9_CH1、CH2 |
| M4 | `M4_FI_PWM` | `M4_BI_PWM` | PB14、PB15 | TIM12_CH1、CH2 |
| M5 | `M5_FI_PWM` | `M5_BI_PWM` | PC6、PC7 | TIM8_CH1、CH2 |
| M6 | `M6_FI_PWM` | `M6_BI_PWM` | PC9、PC8 | TIM8_CH3、CH4 |
| M7 | `M7_FI_PWM` | `M7_BI_PWM` | PB6、PB7 | TIM4_CH1、CH2 |
| M8 | `M8_FI_PWM` | `M8_BI_PWM` | PB8、PB9 | TIM4_CH3、CH4 |

M4/M5/M6 为物理编号调整，网络名保持现有网表命名；软件需建立物理编号与网络名的映射。

## 灯板 PWM

| 功能 | 网络名 | MCU 引脚 | 定时器 |
|---|---|---|---|
| 上灯板 1～4 | `UP_PWM1`～`UP_PWM4` | PE9、PE11、PE14、PE13 | TIM1_CH1、CH2、CH4、CH3 |
| 下灯板 1～3 | `DOWN_PWM1`～`DOWN_PWM3` | PB0、PB1、PB4 | TIM3_CH3、CH4、CH1 |

PB6～PB9 连续用于下侧 M7/M8，便于底边电机接口布线。下灯板 PWM 从 PB0、PB1、PB4 通过内层或板边连接到下侧接口；功率线与 PWM 信号线分开。

## ADC 编码器

| 编码器 | 网络名 | MCU 引脚 | ADC 通道 |
|---|---|---|---|
| 1 | `POT1_ADC` | PC2 | ADC1_IN12 |
| 2 | `POT2_ADC` | PC1 | ADC1_IN11 |
| 3 | `POT3_ADC` | PC0 | ADC1_IN10 |
| 4 | `POT4_ADC` | PC3 | ADC1_IN13 |
| 5 | `POT5_ADC` | PC4 | ADC1_IN14 |
| 6 | `POT6_ADC` | PC5 | ADC1_IN15 |

## 通信与调试

| 功能 | 网络名 | MCU 引脚 | 位置 |
|---|---|---|---|
| 调试 UART | `DBG_UART_TX/RX` | PB10、PB11（USART3） | 上侧左部 |
| Linux UART | `LINUX_UART_TX/RX` | PA9、PA10（USART1） | 上侧右部 |
| SWD | `SWDIO/SWCLK` | PA13、PA14 | MCU 右下角 |
| HOME 按键 | `HOME_KEY` | PE15 | 上侧中部 |

## 指示与控制

| 功能 | 网络名 | MCU 引脚 |
|---|---|---|
| 系统状态灯 | `LED_STATE` | PA4 |
| 故障灯 | `LED_ERR` | PE7 |
| 通信灯 | `LED_COMM` | PA5 |
| 动作灯 | `LED_ACT` | PA7 |
| 蜂鸣器 | `BUZZER` | PA6 / TIM13_CH1 |
| Linux 电源脉冲 | `LINUX_PWR_PULSE` | PB12 |

冻结前请在 CubeMX 检查 TIM2、TIM3、TIM4、TIM8、TIM9、TIM12、TIM13 的复用冲突，并核对 M4/M5/M6 的物理编号映射。
