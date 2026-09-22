# MCU 接口位置与引脚分配（布局优化版）

> 适用器件：STM32F407VGT6，LQFP100   
> 版本：布局导向分配版  
> 说明：MCU 放在 PCB 底层，以下位置按照底层镜像视图规划。最终以 Pin 1 标记和 CubeMX 复核结果为准。

## 1. 接口位置总览

```text
PCB 上侧（从左到右）：
调试 UART → 上灯板 PWM → SWD 调试接口 → HOME 按键 → Linux UART

PCB 左侧（从上到下）：
电机 1 + 编码器 1
电机 2 + 编码器 2
电机 3 + 编码器 3

PCB 右侧（从上到下）：
电机 4 + 编码器 4
电机 5 + 编码器 5
电机 6 + 编码器 6

PCB 下侧（从左到右）：
电机 7 → 下灯板 PWM → 电机 8
```

## 2. 左侧电机接口

左侧三个电机对应驱动芯片 `U8、U9、U15`，从上到下为 M1、M2、M3。

| 电机 | 正向 PWM | 反向 PWM | MCU 引脚 | 定时器 |
|---|---|---|---|---|
| M1 | `M1_FI_PWM` | `M1_BI_PWM` | PA0、PA1 | TIM2_CH1、TIM2_CH2 |
| M2 | `M2_FI_PWM` | `M2_BI_PWM` | PB10、PB11(有问题，这两个在右上方。用PE2那几个行吗) | TIM2_CH3、TIM2_CH4 备用复用 |
| M3 | `M3_FI_PWM` | `M3_BI_PWM` | PE5、PE6 | TIM9_CH1、TIM9_CH2 |

说明：PA2/PA3 让给左上侧调试 UART，因此 M2 改用 TIM2 的 PB10/PB11 备用复用引脚。`PB10/PB11` 不再用于 I²C。

## 3. 右侧电机接口

右侧三个电机对应驱动芯片 `U13、U14、U16`，从上到下为 M4、M5、M6。

| 电机 | 正向 PWM | 反向 PWM | MCU 引脚 | 定时器 |
|---|---|---|---|---|
| M4 | `M4_FI_PWM` | `M4_BI_PWM` | PB14、PB15 |  |
| M5 | `M5_FI_PWM` | `M5_BI_PWM` | PC6、PC7 |  |
| M6 | `M6_FI_PWM` | `M6_BI_PWM` | PC8、PC9 |  |

## 4. 下侧电机接口

下侧两个电机对应驱动芯片 `U18、U17`，从左到右为 M7、M8。

| 电机 | 正向 PWM | 反向 PWM | MCU 引脚 | 定时器 |
|---|---|---|---|---|
| M7 | `M7_FI_PWM` | `M7_BI_PWM` | PB6、PB7 | TIM4_CH1、TIM4_CH2 |
| M8 | `M8_FI_PWM` | `M8_BI_PWM` | PB8、PB9 | TIM4_CH3、TIM4_CH4 |

## 5. 六个编码器 / 电位器接口

编码器接口均为三线：`UP / MID / DOWN`。ADC 采样端使用 MCU 的 ADC1。

| 编码器 | 接口位置 | ADC 网络 | MCU 引脚 | ADC 通道 |
|---|---|---|---|---|
| 编码器 1 | 左侧，电机 1 附近 | `POT1_ADC` | PC0 | ADC1_IN10 |
| 编码器 2 | 左侧，电机 2 附近 | `POT2_ADC` | PC1 | ADC1_IN11 |
| 编码器 3 | 左侧，电机 3 附近 | `POT3_ADC` | PC2 | ADC1_IN12 |
| 编码器 4 | 右侧，电机 4 附近 | `POT4_ADC` | PC3 | ADC1_IN13 |
| 编码器 5 | 右侧，电机 5 附近 | `POT5_ADC` | PC4 | ADC1_IN14 |
| 编码器 6 | 右侧，电机 6 附近 | `POT6_ADC` | PC5 | ADC1_IN15 |

每路编码器保留：

```text
UP/DOWN：1kΩ 串联电阻
MID：1kΩ 串联电阻 + 100nF 滤波电容
ADC：BAT54S 钳位保护
```

STM32F407 的 ADC 引脚主要集中在左侧和上侧，因此右侧三个 ADC 线需要沿板边或内层走线，不能强行穿过电机驱动区。

## 6. 上灯板接口

上灯板位于 PCB 上侧，使用 TIM1 的四个通道：

| 网络 | MCU 引脚 | 定时器 |
|---|---|---|
| `UP_PWM1` | PE9 | TIM1_CH1 |
| `UP_PWM2` | PE11 | TIM1_CH2 |
| `UP_PWM3` | PE13 | TIM1_CH3 |
| `UP_PWM4` | PE14 | TIM1_CH4 |

上灯板功率接口独立提供 `24V_UP_LAMP` 和 `GND`，PWM 接口只传输 3.3V 逻辑信号。

## 7. 下灯板接口

下灯板位于 PCB 下侧中央，使用 TIM3：

| 网络 | MCU 引脚 | 定时器 |
|---|---|---|
| `DOWN_PWM1` | PB0 不对 | TIM3_CH3 |
| `DOWN_PWM2` | PB1 不对 | TIM3_CH4 |
| `DOWN_PWM3` | PB4 | TIM3_CH1 |

下灯板功率接口独立提供 `24V_DOWN_LAMP` 和 `GND`。`PB5/TIM3_CH2` 可作为备用 PWM 或后续扩展。

这个有问题，PB0，PB1在上面

## 8. 顶部调试和控制接口

### 8.1 调试 UART

调试 UART 放在 PCB 上侧左部，使用 USART2：

| 信号 | MCU 引脚 | 说明 |
|---|---|---|
| `DBG_UART_TX` | PA2 | MCU 输出 |
| `DBG_UART_RX` | PA3 | MCU 输入 |
| `3V3_REF` | 3V3_MCU | 电平参考，不给整板供电 |
| `GND` | GND | 信号地 |

PA2/PA3 因此不再用于电机 PWM。

### 8.2 SWD 调试接口

SWD 接口放在 MCU 右上方：

| 信号 | MCU 引脚 |
|---|---|
| `SWDIO` | PA13 |
| `SWCLK` | PA14 |
| `3V3_REF` | 3V3_MCU |
| `GND` | GND |

可额外预留 `NRST` 测试点。NRST 已有独立复位按键，因此不强制放入 SWD 连接器。

### 8.3 HOME 按键接口

HOME 接口位于上侧中部：

| 信号 | MCU 引脚 |
|---|---|
| `HOME_KEY` | PE7 |
| `HOME_EXT` | 外部按键保护网络 |
| `3V3_MCU` | 上拉电源 |
| `GND` | 地 |

### 8.4 Linux UART

Linux UART 位于上侧右部，使用 USART1：

| 信号 | MCU 引脚 | 说明 |
|---|---|---|
| `LINUX_UART_TX` | PA9 有问题 | 接 Linux RX |
| `LINUX_UART_RX` | PA10 有问题 | 接 Linux TX |
| `GND` | GND | 必须连接 |
| `5V/VCC` | 默认不接 | 不用 USB-TTL 线给主板供电 |

TX/RX 建议各串联 47Ω 电阻。Linux 电源由独立的 24V→12V 支路提供。

有问题，PA9，PA10在右侧

## 9. 电源和开关接口

| 功能 | 网络/器件 | 位置建议 |
|---|---|---|
| 24V 总输入 | `DC1`、`VIN_24_RAW` | PCB 下侧电源区 |
| 总开关 | `U1` | 靠近 24V 输入 |
| 输入保险丝/TVS | `U2、D1、D6、D7` | 靠近输入和开关 |
| Linux 12V | `U19`、`12V_LINUX` | 电源区，靠近 Linux 电源连接器 |
| 电机 12V | `U21`、`12V_MOTOR` | 靠近电机驱动和 C1 |
| 5V 系统电源 | `U20`、`5V_SYS` | MCU/LDO 区域 |
| MCU 3.3V | `U23`、`3V3_MCU` | MCU 最小系统附近 |
| 上灯板电源 | `24V_UP_LAMP` | 上灯板功率接口 |
| 下灯板电源 | `24V_DOWN_LAMP` | 下灯板功率接口 |

## 10. 状态灯、蜂鸣器和控制 GPIO

| 功能 | 网络名 | MCU 引脚 | 说明 |
|---|---|---|---|
| 系统状态灯 | `LED_STATE` | PB12 | 低电平点亮 |
| 故障灯 | `LED_ERR` | PB13 | 低电平点亮 |
| 通信灯 | `LED_COMM` | PA4 | 低电平点亮 |
| 动作灯 | `LED_ACT` | PA5 | 低电平点亮 |
| Linux 电源脉冲 | `LINUX_PWR_PULSE` | PE8 | 由 MCU 输出控制 |
| Linux 开漏控制 | `LINUX_PWR_OD` | 外部 Q10 | 硬件开漏控制 |
| 蜂鸣器 | `BUZZER` | 建议使用空闲 GPIO/定时器 | 避免占用 PB8/PB9 电机 PWM |

如果蜂鸣器需要硬件 2.7kHz PWM，应在 CubeMX 中确认所选 GPIO 的定时器复用，不能与下侧电机 TIM4 冲突。

## 11. 必须同步修改的内容

1. 原理图中将 PA2/PA3 改为 `DBG_UART_TX/RX`。
2. 将 M2 PWM 改到 TIM2 的 PB10/PB11 备用复用。
3. 将 Linux UART 改为 USART1 的 PA9/PA10。
4. 上灯板改用 TIM1 的 PE9/PE11/PE13/PE14。
5. 下灯板改用 TIM3 的 PB0/PB1/PB4。
6. 下侧两个电机改用 TIM4 的 PB6～PB9。
7. 把 `LINUX_PWR_PULSE` 从 PE6 移到 PE8。
8. 在 STM32CubeMX 中选择 `Serial Wire`，释放 PA15/PB3 等 JTAG 复用资源。
9. 完成所有 PWM、ADC、UART 的复用冲突检查后，再更新 PCB 和网表。

