# MCU 接口位置与引脚分配（电机顺序更新版）

适用器件：STM32F407VGT6，LQFP100。MCU 放置在 PCB 底层，以下边缘位置按底层镜像视图描述；最终以 Pin 1 标记和 CubeMX 复核结果为准。

## 1. 接口位置

```text
PCB 上侧（左→右）：调试串口、上灯板 PWM、SWD、HOME 按键、Linux 串口
PCB 左侧（上→下）：M1、M2、M3 及对应编码器
PCB 右侧（上→下）：M5、M6、M4 及对应编码器
PCB 下侧（左→右）：M8、下灯板 PWM、M7
```

镜像视图下：左边为 MCU 1～25 脚，顶部为 26～50 脚，右边为 51～75 脚，底部为 76～100 脚。SWD 接口建议放在 MCU 右下角，连接 PA13（SWDIO）和 PA14（SWCLK）。

## 2. 电机 PWM 分配

网络名保持原理图/网表命名；物理电机顺序按最新结构调整如下。

| 物理电机 | 正向 PWM | 反向 PWM | MCU 引脚 | 定时器 |
|---|---|---|---|---|
| M2 | `M1_FI_PWM` | `M1_BI_PWM` | PA0、PA1 | TIM2_CH1、TIM2_CH2 |
| M1 | `M2_FI_PWM` | `M2_BI_PWM` | PA2、PA3 | TIM2_CH3、TIM2_CH4 |
| M3 | `M3_FI_PWM` | `M3_BI_PWM` | PE5、PE6 | TIM9_CH1、TIM9_CH2 |
| M5 | `M4_FI_PWM` | `M4_BI_PWM` | PC6、PC7 | TIM8_CH1、TIM8_CH2 |
| M6 | `M5_FI_PWM` | `M5_BI_PWM` | PC8、PC9 | TIM8_CH3、TIM8_CH4 |
| M4 | `M6_FI_PWM` | `M6_BI_PWM` | PB14、PB15 | TIM12_CH1、TIM12_CH2 |
| M7 | `M7_FI_PWM` | `M7_BI_PWM` | PB0、PB1 | TIM3_CH3、TIM3_CH4 |
| M8 | `M8_FI_PWM` | `M8_BI_PWM` | PB4、PB5 | TIM3_CH1、TIM3_CH2 |

说明：M4/M5/M6 是物理接口顺序调整，网络名仍按现有网表的 `M4_*`、`M5_*`、`M6_*` 保持不变。软件中应建立“物理电机编号 ↔ PWM 网络名”的映射表，避免仅按网络名理解物理位置。

## 3. 灯板 PWM

| 功能 | 网络名 | MCU 引脚 | 定时器 |
|---|---|---|---|
| 上灯板 1～4 | `UP_PWM1`～`UP_PWM4` | PE9、PE11、PE13、PE14 | TIM1_CH1～CH4 |
| 下灯板 1～3 | `DOWN_PWM1`～`DOWN_PWM3` | PB6、PB7、PB8 | TIM4_CH1～CH3 |

PWM 接口只传输 3.3V 逻辑信号，灯板 24V 功率线单独布置；每路接口保留串联电阻和默认下拉。

## 4. 编码器/电位器 ADC

| 编码器 | 网络名 | MCU 引脚 | ADC 通道 |
|---|---|---|---|
| 1 | `POT1_ADC` | PC0 | ADC1_IN10 |
| 2 | `POT2_ADC` | PC1 | ADC1_IN11 |
| 3 | `POT3_ADC` | PC2 | ADC1_IN12 |
| 4 | `POT4_ADC` | PC3 | ADC1_IN13 |
| 5 | `POT5_ADC` | PC4 | ADC1_IN14 |
| 6 | `POT6_ADC` | PC5 | ADC1_IN15 |

每路 MID 建议采用 1kΩ 串联、100nF 对地电容，并在接口侧放置 BAT54S 钳位器件。ADC 线避开电机功率回路和开关节点。

## 5. 通信、调试和按键

| 功能 | 网络名 | MCU 引脚 | 位置/说明 |
|---|---|---|---|
| 调试 UART TX/RX | `DBG_UART_TX/RX` | PB10、PB11（USART3） | 上侧左部 |
| Linux UART TX/RX | `LINUX_UART_TX/RX` | PA9、PA10（USART1） | 上侧右部/右侧 |
| SWD | `SWDIO`、`SWCLK` | PA13、PA14 | MCU 右下角 |
| HOME 按键 | `HOME_KEY` | PE7 | 上侧中部 |

UART 的 TX/RX 各串联 47Ω；Linux 串口必须确认是 3.3V TTL。USB-TTL 线的 5V 默认不接主板供电，只连接 TX、RX、GND，除非经过确认的独立电源方案。

## 6. 状态灯、蜂鸣器和 Linux 控制

| 功能 | 网络名 | MCU 引脚 | 默认逻辑 |
|---|---|---|---|
| 系统状态灯 | `LED_STATE` | PA7 | 低电平点亮 |
| 故障灯 | `LED_ERR` | PE8 | 低电平点亮 |
| Linux 通信灯 | `LED_COMM` | PA4 | 低电平点亮 |
| 动作灯 | `LED_ACT` | PA5 | 低电平点亮 |
| 蜂鸣器 PWM | `BUZZER` | PA6 / TIM13_CH1 | 经 Q11 驱动 |
| Linux 电源脉冲 | `LINUX_PWR_PULSE` | PE10 | 由 MCU 输出 |

状态灯、蜂鸣器及控制信号尽量靠近 MCU 上边缘对应引脚放置，避免跨越 ADC 区。蜂鸣器不能直接由 MCU 脚驱动，使用 Q11、续流二极管 D20 和栅极/基极电阻组成驱动级。

## 7. 冻结前检查

1. 在 CubeMX 中确认 TIM2、TIM3、TIM4、TIM8、TIM9、TIM12、TIM13 复用无冲突。
2. 确认 `M4_*`、`M5_*`、`M6_*` 网络名与物理接口 M5、M6、M4 的软件映射一致。
3. SWD 使用 Serial Wire 模式；PA13、PA14 不分配给普通 GPIO。
4. ADC 采样线、晶振、VDDA 和 VCAP 区域保持短、安静，远离电机驱动和 DC-DC 开关节点。
5. PCB 完成后进行 ERC、DRC、飞线检查，并逐个核对连接器丝印和电机编号。
