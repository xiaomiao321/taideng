# 六路电位器 ADC 采样与保护电路详细连接

> 主控：STM32F407VGT6  
> 传感器：6个中空电位编码器，实测阻值约45.7 kΩ  
> ADC：ADC1，六通道扫描 + DMA  
> 目的：为外接电位器提供限流、滤波、钳位和断线默认状态，并给出可直接照画的完整网络名。

## 1. 总体方案

每个电位器使用3根线：

```text
UP   ：电位器高端
MID  ：电位器滑动端
DOWN ：电位器低端
```

每一路完整结构：

```text
3V3_A ── 1k ── POTx_UP ─────────────── J_POTx pin 1
                   │
                 100nF
                   │
GND   ── 1k ── POTx_DOWN ───────────── J_POTx pin 3

J_POTx pin 2 ── POTx_MID ── 1k ── POTx_ADC ── MCU ADCx
                                           │
                                           ├── 100nF ── GND
                                           ├── 100k ─── GND
                                           └── BAT54S上下钳位到3V3_A和GND
```

其中 `x` 为1～6。

## 2. 为什么使用这套电路

各部分作用如下：

| 器件 | 作用 |
|---|---|
| UP端1 kΩ | 限制电位器线束误接或高端短路电流 |
| DOWN端1 kΩ | 限制低端误接或短路电流 |
| UP与DOWN之间100 nF | 给外接电位器提供局部高频旁路，降低线束干扰 |
| MID串联1 kΩ | 限制ADC钳位电流，并与100 nF组成低通滤波 |
| ADC端100 nF | 降低电机噪声并为STM32 ADC采样保持电容提供局部电荷 |
| ADC端100 kΩ下拉 | 滑动端断线时给ADC一个确定的低电平状态 |
| BAT54S | 将ADC节点钳位在接近0～3.3 V范围内，保护STM32 ADC输入 |

1 kΩ与100 nF的理论截止频率约为：

```text
fc = 1 / (2πRC) ≈ 1.59 kHz
```

机械臂角度变化远慢于该频率，因此不会影响正常位置采样。

## 3. 模拟电源网络

建议用 `3V3_A` 给电位器高端及STM32 VDDA/VREF+供电：

```text
3V3_MCU ── FB_ADC 或 10Ω ── 3V3_A
                               │
                               ├── 1uF ── GND
                               └── 100nF ── GND
```

| 网络 | 用途 |
|---|---|
| `3V3_MCU` | 主3.3 V数字电源 |
| `3V3_A` | ADC参考、电位器激励和ADC钳位上电源 |
| `GND` | 公共连续地平面 |

注意：

- PCB不单独切割模拟地平面，所有地仍使用完整连续的GND。
- `3V3_A`只在电源端进行滤波。
- `VDDA`、`VREF+`和6路电位器高端使用同一 `3V3_A`，有利于实现比例式采样。
- 六个约45.7 kΩ电位器总电流很小，`3V3_A`负载足够轻。

## 4. 六路ADC与STM32引脚分配

| 通道 | 电位器 | ADC网络 | MCU引脚 | ADC通道 |
|---:|---|---|---|---|
| 1 | `J_POT1` | `POT1_ADC` | PC0 | ADC1_IN10 |
| 2 | `J_POT2` | `POT2_ADC` | PC1 | ADC1_IN11 |
| 3 | `J_POT3` | `POT3_ADC` | PC2 | ADC1_IN12 |
| 4 | `J_POT4` | `POT4_ADC` | PC3 | ADC1_IN13 |
| 5 | `J_POT5` | `POT5_ADC` | PC4 | ADC1_IN14 |
| 6 | `J_POT6` | `POT6_ADC` | PC5 | ADC1_IN15 |

建议软件配置：

- ADC1六通道扫描模式；
- DMA循环采样；
- 12-bit分辨率；
- 采样时间先设为144 cycles；
- 软件对每一路进行滑动平均或IIR滤波；
- 每个关节单独标定最小值、中点和最大值。

## 5. 单路完整连接模板

以下以POT1为例，其余5路复制后只替换编号和MCU引脚。

### 5.1 电位器连接器

| 连接器 | 引脚 | 网络名 | 丝印建议 |
|---|---:|---|---|
| `J_POT1` | 1 | `POT1_UP` | `3V3/UP` |
| `J_POT1` | 2 | `POT1_MID` | `SIG/MID` |
| `J_POT1` | 3 | `POT1_DOWN` | `GND/DOWN` |

推荐使用带锁扣、防反插的3Pin连接器。所有6个接口必须使用相同线序。

### 5.2 电位器高端限流

```text
3V3_A ── R_POT1_UP 1k ── POT1_UP ── J_POT1 pin 1
```

| 器件 | 引脚 | 网络 |
|---|---:|---|
| `R_POT1_UP` 1 kΩ | 1 | `3V3_A` |
| `R_POT1_UP` 1 kΩ | 2 | `POT1_UP` |
| `J_POT1` | pin 1 | `POT1_UP` |

### 5.3 电位器低端限流

```text
GND ── R_POT1_DOWN 1k ── POT1_DOWN ── J_POT1 pin 3
```

| 器件 | 引脚 | 网络 |
|---|---:|---|
| `R_POT1_DOWN` 1 kΩ | 1 | `GND` |
| `R_POT1_DOWN` 1 kΩ | 2 | `POT1_DOWN` |
| `J_POT1` | pin 3 | `POT1_DOWN` |

### 5.4 电位器供电旁路电容

```text
POT1_UP ── C_POT1_SUPPLY 100nF ── POT1_DOWN
```

| 器件 | 引脚 | 网络 |
|---|---:|---|
| `C_POT1_SUPPLY` 100 nF | 1 | `POT1_UP` |
| `C_POT1_SUPPLY` 100 nF | 2 | `POT1_DOWN` |

该电容放在主板的电位器连接器附近。如果线束很长且电机噪声较大，也可以在电位器本体端再并联一只100 nF。

### 5.5 滑动端串联限流

```text
J_POT1 pin 2 ── POT1_MID ── R_POT1_ADC_SER 1k ── POT1_ADC ── PC0
```

| 器件 | 引脚 | 网络 |
|---|---:|---|
| `J_POT1` | pin 2 | `POT1_MID` |
| `R_POT1_ADC_SER` 1 kΩ | 1 | `POT1_MID` |
| `R_POT1_ADC_SER` 1 kΩ | 2 | `POT1_ADC` |
| STM32 PC0 | — | `POT1_ADC` |

### 5.6 ADC滤波电容

```text
POT1_ADC ── C_POT1_ADC 100nF ── GND
```

| 器件 | 引脚 | 网络 |
|---|---:|---|
| `C_POT1_ADC` 100 nF | 1 | `POT1_ADC` |
| `C_POT1_ADC` 100 nF | 2 | `GND` |

`C_POT1_ADC`尽量靠近STM32的PC0引脚，而不是靠近连接器。

### 5.7 断线默认下拉

```text
POT1_ADC ── R_POT1_ADC_PD 100k ── GND
```

| 器件 | 引脚 | 网络 |
|---|---:|---|
| `R_POT1_ADC_PD` 100 kΩ | 1 | `POT1_ADC` |
| `R_POT1_ADC_PD` 100 kΩ | 2 | `GND` |

作用：电位器滑动端断开或连接器脱落时，ADC不会悬空，而是逐渐回到接近0 V。

注意：100 kΩ会对约45.7 kΩ的电位器产生一定负载，使输出曲线略有变化。当前软件需要逐轴标定，因此可以通过标定吸收；若后续对线性度要求更高，可改为470 kΩ或1 MΩ，并重新设置断线阈值。

### 5.8 ADC上下钳位

使用一只双肖特基二极管 `BAT54S`，或两只独立BAT54系列二极管：

```text
                 3V3_A
                   │
           ADC节点 ─|<|─  高端钳位
                   │
POT1_ADC ──────────●────────── PC0
                   │
           GND ────|<|─  低端钳位
```

必须满足的电气方向：

- 高端钳位：二极管阳极接 `POT1_ADC`，阴极接 `3V3_A`。
- 低端钳位：二极管阳极接 `GND`，阴极接 `POT1_ADC`。

如果使用BAT54S常见串联型内部结构，应让中间公共节点连接 `POT1_ADC`，两端分别连接GND和 `3V3_A`；具体1、2、3脚编号必须按照所选厂家数据手册和嘉立创EDA符号核对，不能只凭封装外形判断。

为什么使用肖特基二极管：

- 肖特基正向压降低于1N4148；
- 能在STM32内部保护二极管大量导通之前分流；
- ADC采样频率不高，BAT54S的结电容对本应用影响很小。

BAT54S主要处理误接瞬态和低能量过压，不代替接口级IEC静电防护。如果电位器线束很长或经常插拔，可在连接器附近另预留低电容3.3 V ESD器件。

## 6. POT2～POT6完整网络替换表

所有通道的元件值完全相同。

### POT2

```text
3V3_A → 1k → POT2_UP → J_POT2 pin 1
GND   → 1k → POT2_DOWN → J_POT2 pin 3
POT2_UP ↔ 100nF ↔ POT2_DOWN
J_POT2 pin 2 / POT2_MID → 1k → POT2_ADC → PC1 / ADC1_IN11
POT2_ADC → 100nF → GND
POT2_ADC → 100k → GND
POT2_ADC → BAT54S钳位到3V3_A和GND
```

### POT3

```text
3V3_A → 1k → POT3_UP → J_POT3 pin 1
GND   → 1k → POT3_DOWN → J_POT3 pin 3
POT3_UP ↔ 100nF ↔ POT3_DOWN
J_POT3 pin 2 / POT3_MID → 1k → POT3_ADC → PC2 / ADC1_IN12
POT3_ADC → 100nF → GND
POT3_ADC → 100k → GND
POT3_ADC → BAT54S钳位到3V3_A和GND
```

### POT4

```text
3V3_A → 1k → POT4_UP → J_POT4 pin 1
GND   → 1k → POT4_DOWN → J_POT4 pin 3
POT4_UP ↔ 100nF ↔ POT4_DOWN
J_POT4 pin 2 / POT4_MID → 1k → POT4_ADC → PC3 / ADC1_IN13
POT4_ADC → 100nF → GND
POT4_ADC → 100k → GND
POT4_ADC → BAT54S钳位到3V3_A和GND
```

### POT5

```text
3V3_A → 1k → POT5_UP → J_POT5 pin 1
GND   → 1k → POT5_DOWN → J_POT5 pin 3
POT5_UP ↔ 100nF ↔ POT5_DOWN
J_POT5 pin 2 / POT5_MID → 1k → POT5_ADC → PC4 / ADC1_IN14
POT5_ADC → 100nF → GND
POT5_ADC → 100k → GND
POT5_ADC → BAT54S钳位到3V3_A和GND
```

### POT6

```text
3V3_A → 1k → POT6_UP → J_POT6 pin 1
GND   → 1k → POT6_DOWN → J_POT6 pin 3
POT6_UP ↔ 100nF ↔ POT6_DOWN
J_POT6 pin 2 / POT6_MID → 1k → POT6_ADC → PC5 / ADC1_IN15
POT6_ADC → 100nF → GND
POT6_ADC → 100k → GND
POT6_ADC → BAT54S钳位到3V3_A和GND
```

## 7. 六个连接器网络总表

| 连接器 | Pin 1 | Pin 2 | Pin 3 |
|---|---|---|---|
| `J_POT1` | `POT1_UP` | `POT1_MID` | `POT1_DOWN` |
| `J_POT2` | `POT2_UP` | `POT2_MID` | `POT2_DOWN` |
| `J_POT3` | `POT3_UP` | `POT3_MID` | `POT3_DOWN` |
| `J_POT4` | `POT4_UP` | `POT4_MID` | `POT4_DOWN` |
| `J_POT5` | `POT5_UP` | `POT5_MID` | `POT5_DOWN` |
| `J_POT6` | `POT6_UP` | `POT6_MID` | `POT6_DOWN` |

连接器旁逐脚标注 `UP / MID / DOWN` 或 `3V3 / SIG / GND`，不能只标一个接口名称。

## 8. 元件数量汇总

| 器件 | 单路数量 | 六路总数 | 规格 |
|---|---:|---:|---|
| 3Pin连接器 | 1 | 6 | 带锁扣、防反插 |
| UP限流电阻 | 1 | 6 | 1 kΩ，0603 |
| DOWN限流电阻 | 1 | 6 | 1 kΩ，0603 |
| 滑动端串联电阻 | 1 | 6 | 1 kΩ，0603 |
| ADC断线下拉 | 1 | 6 | 100 kΩ，0603 |
| 电位器供电旁路 | 1 | 6 | 100 nF，X7R |
| ADC滤波电容 | 1 | 6 | 100 nF，X7R |
| 双肖特基钳位 | 1 | 6 | BAT54S，SOT-23 |

若希望减少BAT54S数量，也可以使用多通道ESD/钳位阵列，但必须确认其工作电压、钳位方向和漏电不会影响ADC精度。

## 9. 软件标定和故障判断

### 9.1 角度标定

每个电位器实际安装角度、机械范围和阻值误差不同，不能统一使用理论0～4095映射。每个关节保存：

```text
adc_min[x]
adc_center[x]
adc_max[x]
angle_min[x]
angle_max[x]
```

角度换算基于实测端点，而不是电位器标称阻值。

### 9.2 断线和异常判断

100 kΩ下拉使滑动端断线时ADC趋近0。软件应设置合理阈值，例如：

- ADC持续低于标定最小值一定余量；
- ADC持续高于标定最大值一定余量；
- 单次变化速度超过机械结构可能达到的速度；
- 一段时间内读数完全不变但电机仍在运行。

检测到异常时：

1. 立即把对应电机FI、BI设为安全状态；
2. 停止闭环位置控制；
3. 上报故障给Linux；
4. 必要时关闭 `MOTOR_PWR_EN`。

阈值必须根据实际六个电位器标定数据确定，不能直接把0或4095作为唯一断线判断。

## 10. 原理图绘制步骤

1. 先画POT1的连接器、UP/DOWN限流和供电旁路电容。
2. 画MID到ADC的1 kΩ串阻。
3. 在ADC节点放100 nF、100 kΩ和BAT54S。
4. 连接 `POT1_ADC` 到PC0。
5. 按二极管电气方向检查BAT54S，而不是只看符号方向。
6. 对POT1运行ERC。
7. 复制5次并改为POT2～POT6。
8. 对照ADC引脚表逐路修改PC0～PC5。
9. 最后检查6个连接器的UP/MID/DOWN线序完全一致。

## 11. 原理图检查清单

- [ ] 每个连接器都是Pin 1 UP、Pin 2 MID、Pin 3 DOWN。
- [ ] 每一路UP和DOWN各有1 kΩ限流。
- [ ] 每一路UP与DOWN之间有100 nF。
- [ ] 每一路MID到ADC有1 kΩ串联电阻。
- [ ] 每一路ADC节点有100 nF到GND。
- [ ] 每一路ADC节点有100 kΩ到GND。
- [ ] 高端钳位阳极接ADC、阴极接 `3V3_A`。
- [ ] 低端钳位阳极接GND、阴极接ADC。
- [ ] BAT54S的实际引脚编号与所选数据手册一致。
- [ ] POT1～POT6分别接PC0～PC5，没有跨路误接。
- [ ] `VDDA`、`VREF+`与电位器高端使用同一 `3V3_A`。
- [ ] 所有连接器旁逐脚标注网络含义。

## 12. PCB布局布线要求

- 连接器侧的UP/DOWN限流和 `C_POTx_SUPPLY` 靠近对应连接器。
- `R_POTx_ADC_SER`、BAT54S、100 kΩ和 `C_POTx_ADC` 靠近STM32 ADC引脚。
- `C_POTx_ADC` 到MCU引脚和GND的回路尽量短。
- 六路ADC线远离RZ7899的FO/BO、电机连接器和DC/DC开关节点。
- 不要让电机功率回流从ADC区域下方穿过。
- 使用完整连续GND平面，不单独切割模拟地。
- 电位器线束尽量远离电机线；必须并行时保持间距。
- 长线条件下可以让MID与GND成对绞合，或使用带地参考的线束。
- 每个 `POTx_ADC` 建议留小测试焊盘，方便标定和观察噪声。

## 13. 与之前通用ADC保护图的区别

之前的通用保护图包含多级10 kΩ、100 kΩ下拉、两级100 nF和二极管钳位。它适合未知外部模拟信号，但直接用于约45.7 kΩ电位器会带来较大的分压误差和较高源阻抗。

本设计针对3.3 V供电电位器进行了简化：

- UP、DOWN各1 kΩ限流；
- MID只串1 kΩ；
- 保留100 kΩ断线下拉；
- 保留100 nF采样滤波；
- 保留肖特基上下钳位。

这样既保留保护功能，又减少对ADC精度和采样时间的影响。

