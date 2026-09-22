# STM32F407VGT6 引脚分配（当前版）

> 目标器件：STM32F407VGT6，LQFP-100  
> 网络名称以当前网表为准

## 1. 电机 PWM

每个 RZ7899 驱动器使用一对 PWM：FI 和 BI。每路驱动输入端保留 100Ω 串联电阻和 10kΩ 下拉。

| 电机 | FI 网络 / 引脚 | BI 网络 / 引脚 | 定时器 |
|---|---|---|---|
| M1 | `M1_FI_PWM` / PE9 | `M1_BI_PWM` / PE11 | TIM1_CH1/CH2 |
| M2 | `M2_FI_PWM` / PE13 | `M2_BI_PWM` / PE14 | TIM1_CH3/CH4 |
| M3 | `M3_FI_PWM` / PA0 | `M3_BI_PWM` / PA1 | TIM2_CH1/CH2 |
| M4 | `M4_FI_PWM` / PA2 | `M4_BI_PWM` / PA3 | TIM2_CH3/CH4 |
| M5 | `M5_FI_PWM` / PB4 | `M5_BI_PWM` / PB5 | TIM3_CH1/CH2 |
| M6 | `M6_FI_PWM` / PB0 | `M6_BI_PWM` / PB1 | TIM3_CH3/CH4 |
| M7 | `M7_FI_PWM` / PD12 | `M7_BI_PWM` / PD13 | TIM4_CH1/CH2 |
| M8 | `M8_FI_PWM` / PD14 | `M8_BI_PWM` / PD15 | TIM4_CH3/CH4 |

## 2. 灯板 PWM

灯板PWM使用100Ω串联电阻阵列和10kΩ独立下拉电阻阵列。

| 功能 | 网表网络名 | MCU 引脚 | 定时器 |
|---|---|---|---|
| 上灯板1 | `UP_PWM1` | PC6 | TIM8_CH1 |
| 上灯板2 | `UP_PWM2` | PC7 | TIM8_CH2 |
| 上灯板3 | `UP_PWM3` | PC8 | TIM8_CH3 |
| 上灯板4 | `UP_PWM4` | PC9 | TIM8_CH4 |
| 下灯板1 | `DOWN_PWM1` | PB14 | TIM12_CH1 |
| 下灯板2 | `DOWN_PWM2` | PB15 | TIM12_CH2 |
| 下灯板3 | `DOWN_PWM3` | PE5 | TIM9_CH1 |

功率连接器与PWM信号分开：上、下灯板功率均为24V/GND，PWM为3.3V逻辑信号。

## 3. 六路电位器 ADC

| 电机反馈 | 网表网络名 | MCU 引脚 | ADC通道 |
|---|---|---|---|
| 反馈1 | `POT1_ADC` | PC0 | ADC1_IN10 |
| 反馈2 | `POT2_ADC` | PC1 | ADC1_IN11 |
| 反馈3 | `POT3_ADC` | PC2 | ADC1_IN12 |
| 反馈4 | `POT4_ADC` | PC3 | ADC1_IN13 |
| 反馈5 | `POT5_ADC` | PC4 | ADC1_IN14 |
| 反馈6 | `POT6_ADC` | PC5 | ADC1_IN15 |

电位器连接器网络保持：`POTx_UP`、`POTx_MID`、`POTx_DOWN`。其中 `POTx_MID` 经过滤波、钳位后形成对应的 `POTx_ADC`。

## 4. Linux接口

| 功能 | 网表网络名 | MCU 引脚 | 说明 |
|---|---|---|---|
| MCU发送 | `LINUX_UART_TX` | PD8 / USART3_TX | 接Linux RX |
| MCU接收 | `LINUX_UART_RX` | PD9 / USART3_RX | 接Linux TX |
| Linux开关机脉冲 | `LINUX_PWR_PULSE` | PE6 / GPIO输出 | 经100Ω驱动Q10栅极 |
| 开漏输出 | `LINUX_PWR_OD` | 不接MCU | Q10漏极连接CN6 |

UART为3.3V TTL，TX/RX各经过47Ω串联电阻。`LINUX_PWR_PULSE`单次拉低有效，初始脉宽建议约200ms，由软件联调确定。

Linux供电网络：

```text
VIN_24_SW → F7 → 24V_LINUX_IN → U19 → 12V_LINUX → U1
```

U1为Linux板DC5525供电连接器，U1.1为`12V_LINUX`，U1.2为GND。

## 5. HOME按键

| 功能 | 网表网络名 | MCU 引脚 |
|---|---|---|
| HOME输入 | `HOME_KEY` | PE7 / GPIO、EXTI |
| 外部按键侧 | `HOME_EXT` | 不直接接MCU |

当前连接关系：

```text
CN9.5 → HOME_EXT → D19 → GND
HOME_EXT → R92(1kΩ) → HOME_KEY
HOME_KEY → R43(10kΩ) → 3V3_MCU
HOME_KEY → C27(100nF) → GND
```

按钮应连接为：`COM/C → GND`、`NO → HOME_EXT`、`NC悬空`。按钮LED需根据实物确认，若无内置限流电阻，应由3.3V经470Ω～1kΩ供电。

## 6. 调试和系统引脚

| 功能 | 网络名 | MCU 引脚 |
|---|---|---|
| SWD数据 | `SWDIO` | PA13 |
| SWD时钟 | `SWCLK` | PA14 |
| 调试串口发送 | `DBG_UART_TX` | PD5 / USART2_TX |
| 调试串口接收 | `DBG_UART_RX` | PD6 / USART2_RX |
| 8MHz晶振 | `OS_8M_IN/OUT` | PH0/PH1 |
| 32.768kHz晶振 | `OS_32K_IN/OUT` | PC14/PC15 |
| 复位 | `RST` | NRST |
| 启动配置 | `BOOT0` | BOOT0 |
| 启动配置2 | `BOOT1` | PB2 |

CubeMX中选择 `Serial Wire`，不要启用完整JTAG。

## 7. 电源网络

```text
VIN_24_RAW → U2保险丝/总开关 → VIN_24_FUSED → Q2 → VIN_24_SW
VIN_24_SW → F5 → 24V_UP_LAMP
VIN_24_SW → F6 → 24V_DOWN_LAMP
VIN_24_SW → F7 → 24V_LINUX_IN → U19 → 12V_LINUX
VIN_24_SW → F8 → 24V_MOTOR_IN → U21 → 12V_MOTOR
VIN_24_SW → F9 → U20 → 5V_SYS → U23 → 3V3_MCU
```

模拟电源使用：

```text
3V3_MCU → L1 → VDDA
```

`VDDA`同时供ADC参考和电位器上端，必须与数字PWM、DC-DC开关节点分区布线。

## 8. 指示灯
### 引脚分配

| 功能 | 网表网络名 | MCU引脚 | 颜色/类型 | 有效电平 |
|---|---|---|---|---|
| 系统状态灯 | `LED_STATE` | PB12 | 绿色0603 LED | 低电平点亮 |
| 故障指示灯 | `LED_ERR` | PB13 | 红色0603 LED | 低电平点亮 |
| Linux通信灯 | `LED_COMM` | PA4 | 蓝色0603 LED | 低电平点亮 |
| 动作指示灯 | `LED_ACT` | PA5 | 黄色0603 LED | 低电平点亮 |
| 蜂鸣器 | `BUZZER` | PB8 / TIM10_CH1 | 无源蜂鸣器 | 按驱动级确定 |

### 软件逻辑

- `LED_STATE`：启动完成常亮；正常运行1Hz慢闪；
- `LED_ERR`：正常熄灭；可恢复故障慢闪；严重故障快闪；
- `LED_COMM`：Linux收到有效UART数据时点亮约50ms；
- `LED_ACT`：电机或灯光动作时点亮；
- `BUZZER`：HOME按键反馈和故障提示。

