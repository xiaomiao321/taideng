# MCU 接口位置与引脚分配（布局优化版 v2）

> 器件：STM32F407VGT6，LQFP100  
> MCU 位于 PCB 底层，以下按底层镜像视图规划。  
> 本版修正了 M2、下灯板和 Linux UART 的物理位置问题。

## 1. 推荐接口位置

```text
PCB 上侧：上灯板 PWM、HOME、Linux UART
PCB 右上侧：SWD、调试 UART
PCB 左侧：电机 1～3及编码器 1～3
PCB 右侧：电机 4～6及编码器 4～6
PCB 下侧：电机 7、下灯板 PWM、电机 8
```

SWD 和调试 UART 建议放在右上侧，靠近 MCU 右侧的 PA13/PA14、PA9/PA10；Linux UART 放在上侧右部，靠近 PB10/PB11。

## 2. 左侧电机 M1～M3

对应驱动芯片 `U8、U9、U15`，从上到下为 M1、M2、M3。

| 电机 | 正向 PWM | 反向 PWM | MCU 引脚 | 定时器 |
|---|---|---|---|---|
| M1 | `M1_FI_PWM` | `M1_BI_PWM` | PA0、PA1 | TIM2_CH1、TIM2_CH2 |
| M2 | `M2_FI_PWM` | `M2_BI_PWM` | PA2、PA3 | TIM2_CH3、TIM2_CH4 |
| M3 | `M3_FI_PWM` | `M3_BI_PWM` | PE5、PE6 | TIM9_CH1、TIM9_CH2 |

PA2/PA3 不再用于调试 UART，优先保证左侧三个电机的 PWM 都位于 MCU 左侧附近。

## 3. 右侧电机 M4～M6

对应驱动芯片 `U13、U14、U16`，从上到下为 M4、M5、M6。

| 电机 | 正向 PWM | 反向 PWM | MCU 引脚 | 定时器 |
|---|---|---|---|---|
| M5 | `M4_FI_PWM` | `M4_BI_PWM` | PC6、PC7 | TIM8_CH1、TIM8_CH2 |
| M6 | `M5_FI_PWM` | `M5_BI_PWM` | PC8、PC9 | TIM8_CH3、TIM8_CH4 |
| M4 | `M6_FI_PWM` | `M6_BI_PWM` | PB14、PB15 | TIM12_CH1、TIM12_CH2 |

上述引脚均位于 MCU 右侧附近。

## 4. 下侧电机 M7、M8

对应驱动芯片 `U18、U17`，从左到右为 M7、M8。

| 电机 | 正向 PWM | 反向 PWM | MCU 引脚 | 定时器 |
|---|---|---|---|---|
| M7 | `M7_FI_PWM` | `M7_BI_PWM` | PB0、PB1 | TIM3_CH3、TIM3_CH4 |
| M8 | `M8_FI_PWM` | `M8_BI_PWM` | PB4、PB5 | TIM3_CH1、TIM3_CH2 |

TIM3 是下侧电机的折中方案：PB4/PB5 位于 MCU 下边，PB0/PB1 位于上边，需要从上边向下侧绕线。这样可以把下灯板 PWM 完整放到 MCU 下边，整体走线更容易。

## 5. 六路编码器 ADC

| 编码器 | ADC 网络 | MCU 引脚 | ADC 通道 |
|---|---|---|---|
| 编码器 1 | `POT1_ADC` | PC0 | ADC1_IN10 |
| 编码器 2 | `POT2_ADC` | PC1 | ADC1_IN11 |
| 编码器 3 | `POT3_ADC` | PC2 | ADC1_IN12 |
| 编码器 4 | `POT4_ADC` | PC3 | ADC1_IN13 |
| 编码器 5 | `POT5_ADC` | PC4 | ADC1_IN14 |
| 编码器 6 | `POT6_ADC` | PC5 | ADC1_IN15 |

PC0～PC3 在 MCU 左侧，PC4/PC5 在 MCU 上边。STM32F407 右侧没有连续 ADC 输入，因此右侧编码器的 ADC 线应沿板边或内层走线，避开电机 PWM 和电源回路。

## 6. 上灯板 PWM

上灯板在 PCB 上侧，使用 MCU 上边的 TIM1：

| 网络 | MCU 引脚 | 定时器 |
|---|---|---|
| `UP_PWM1` | PE9 | TIM1_CH1 |
| `UP_PWM2` | PE11 | TIM1_CH2 |
| `UP_PWM3` | PE13 | TIM1_CH3 |
| `UP_PWM4` | PE14 | TIM1_CH4 |

功率接口独立提供 `24V_UP_LAMP/GND`，PWM 接口只传输 3.3V 信号。

## 7. 下灯板 PWM

下灯板在 PCB 下侧中央，改用 MCU 下边的 TIM4：

| 网络 | MCU 引脚 | 定时器 |
|---|---|---|
| `DOWN_PWM1` | PB6 | TIM4_CH1 |
| `DOWN_PWM2` | PB7 | TIM4_CH2 |
| `DOWN_PWM3` | PB8 | TIM4_CH3 |

`PB9/TIM4_CH4` 保留。这样下灯板三路 PWM 都位于 MCU 下边，避免使用上边的 PB0/PB1。功率接口独立提供 `24V_DOWN_LAMP/GND`。

## 8. 顶部和右上接口

### 8.1 Linux UART

Linux UART 位于上侧右部，使用 USART3 的 PB10/PB11 复用：

| 信号 | MCU 引脚 | 说明 |
|---|---|---|
| `LINUX_UART_TX` | PB10 | 接 Linux RX |
| `LINUX_UART_RX` | PB11 | 接 Linux TX |
| `GND` | GND | 必须连接 |
| `5V/VCC` | 默认不接 | 不用 USB-TTL 给主板供电 |

PB10/PB11 位于 MCU 上边，比 PA9/PA10 更适合连接顶部 Linux 接口。TX/RX 各串联 47Ω 电阻。

### 8.2 调试 UART

调试 UART 建议放到 MCU 右上侧，使用 USART1：

| 信号 | MCU 引脚 |
|---|---|
| `DBG_UART_TX` | PA9 |
| `DBG_UART_RX` | PA10 |
| `3V3_REF` | 3V3_MCU |
| `GND` | GND |

### 8.3 SWD

SWD 接口放在调试 UART 附近：

| 信号 | MCU 引脚 |
|---|---|
| `SWDIO` | PA13 |
| `SWCLK` | PA14 |
| `3V3_REF` | 3V3_MCU |
| `GND` | GND |

NRST 已有独立复位按键，可单独放置 NRST 测试点。

### 8.4 HOME

| 信号 | MCU 引脚 |
|---|---|
| `HOME_KEY` | PE7 |
| `HOME_EXT` | 外部按键保护网络 |
| `3V3_MCU` | 上拉电源 |
| `GND` | GND |

## 9. 状态灯、蜂鸣器和控制信号

| 功能 | 网络名 | MCU 引脚 | 说明 |
|---|---|---|---|
| 系统状态灯 | `LED_STATE` | PB12 | 低电平点亮 |
| 故障灯 | `LED_ERR` | PB13 | 低电平点亮 |
| 通信灯 | `LED_COMM` | PA4 | 低电平点亮 |
| 动作灯 | `LED_ACT` | PA5 | 低电平点亮 |
| Linux 电源脉冲 | `LINUX_PWR_PULSE` | PE8 | MCU 输出 |
| Linux 开漏控制 | `LINUX_PWR_OD` | 外部 Q10 | 硬件开漏 |
| 蜂鸣器 | `BUZZER` | PA6 / TIM13_CH1 | 使用 Q11 驱动 |

蜂鸣器不再使用 PB8，因为 PB8 已分配给下灯板 PWM。

## 10. 电源接口

| 功能 | 网络/器件 |
|---|---|
| 24V 总输入 | `DC1`、`VIN_24_RAW` |
| 总开关 | `U1` |
| 输入保护 | `U2、D1、D6、D7` |
| Linux 12V | `U19`、`12V_LINUX` |
| 电机 12V | `U21`、`12V_MOTOR` |
| 5V 系统电源 | `U20`、`5V_SYS` |
| MCU 3.3V | `U23`、`3V3_MCU` |

## 11. 需要同步更新

1. M2 改回 PA2/PA3，不能使用 PB10/PB11。
2. Linux UART 改为 USART3 的 PB10/PB11。
3. 调试 UART 改为 USART1 的 PA9/PA10，并将接口移动到右上侧。
4. 下灯板改为 PB6/PB7/PB8，使用 TIM4。
5. 下侧电机改为 TIM3 的 PB0/PB1/PB4/PB5。
6. 蜂鸣器改到 PA6，避免占用 PB8。
7. 在 CubeMX 中确认 USART1/USART3、TIM2、TIM3、TIM4、TIM8、TIM9、TIM12 的复用无冲突。
8. 保持 `Serial Wire` 调试模式，确认 PA13/PA14 可用。

