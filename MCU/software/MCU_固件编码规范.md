# AItaid MCU 固件编码规范

> 版本：v0.1  
> 更新日期：2026-09-29  
> 适用范围：`MCU/software/test_bringup` 中由本项目自行编写的 C 代码  
> 不适用范围：STM32CubeMX 生成代码、STM32 HAL、CMSIS、FreeRTOS Kernel 和其他第三方库

## 1. 目的

本规范用于统一 AItaid MCU 固件的命名、格式、模块接口和基本安全规则，使代码便于阅读、调试、评审和实板验证。

本规范不追求形式上的“完全符合某个外部标准”，而是结合以下规范中适合 STM32F407、HAL 和 FreeRTOS 项目的部分：

- [BARR-C:2018 Embedded C Coding Standard](https://barrgroup.com/embedded-c-coding-standard)
- [Arm CMSIS Coding Rules](https://github.com/ARM-software/CMSIS_6/blob/main/CMSIS/Documentation/Doxygen/General/src/mainpage.md#coding-rules)
- [Zephyr Naming Conventions](https://docs.zephyrproject.org/latest/contribute/style/naming.html)
- [Zephyr C Code Style](https://docs.zephyrproject.org/latest/contribute/style/code.html)
- [MISRA C](https://misra.org.uk/)
- [SEI CERT C Coding Standard](https://wiki.sei.cmu.edu/confluence/display/c)

当本规范与现有代码冲突时，按以下顺序处理：

1. 不修改 CubeMX、HAL、CMSIS、FreeRTOS 和第三方库的既有命名。
2. 新增的项目代码遵守本规范。
3. 修改已有项目代码时，在不扩大改动范围的前提下逐步统一。
4. 安全性和正确性优先于格式一致性。

## 2. 基本原则

1. 优先让代码易于阅读和验证，而不是减少打字量。
2. 名称表达用途，不依赖注释解释含糊的缩写。
3. 一个模块只负责一类清晰的功能。
4. 硬件细节由 BSP 或驱动层封装，应用层不直接操作无关寄存器和引脚。
5. 所有外部输入均视为不可信，使用前检查范围和状态。
6. 电机等执行器默认采用安全状态；初始化失败、通信失联或发生故障时优先停止输出。
7. 中断保持短小，不在中断中执行阻塞或耗时操作。
8. 不宣称项目符合 MISRA，除非已经完成规则选择、静态检查、偏离记录和正式验证。

## 3. 文件与模块

### 3.1 文件命名

文件名使用小写字母、数字和下划线：

```text
bsp_led.c
bsp_led.h
motor_manager.c
motor_manager.h
protocol_parser.c
protocol_parser.h
```

不得使用空格、中文、大小写混合或含义不明的缩写：

```text
LEDDriver.c       // 不推荐
motorMgr.c        // 不推荐
功能测试.c         // 不推荐
utils.c           // 含义过于宽泛
```

每个模块通常由一个同名 `.c` 和 `.h` 文件组成。仅供模块内部使用的声明放在 `.c` 文件中，不放入公共头文件。

### 3.2 建议的模块层次

```text
App/        整机业务、运行模式和任务入口
Services/   安全管理、协议、状态汇总和多设备协调
Drivers/    RZ7899、电位器等具体器件驱动
BSP/        板载 LED、按键、蜂鸣器和板级连接封装
Core/       CubeMX 生成的初始化和中断入口
```

上层可以调用下层，下层不得依赖上层业务：

```text
App → Services → Drivers/BSP → HAL/CMSIS
```

## 4. 命名规范

### 4.1 总表

| 对象 | 规则 | 示例 |
|---|---|---|
| 文件 | 小写蛇形 | `bsp_led.c` |
| 公共函数 | `模块_动作()` | `bsp_led_toggle()` |
| 私有函数 | 小写蛇形并使用 `static` | `write_pin_level()` |
| 局部变量 | 小写蛇形 | `retry_count` |
| 函数参数 | 小写蛇形 | `timeout_ms` |
| 外部可见全局变量 | `g_` 前缀 | `g_system_state` |
| 文件内静态变量 | `s_` 前缀 | `s_tx_buffer` |
| 类型 | 小写蛇形加 `_t` | `motor_state_t` |
| 宏 | 大写蛇形并带模块前缀 | `LOG_BUFFER_SIZE` |
| 枚举值 | 大写蛇形并带类型或模块前缀 | `LED_ID_STATE` |
| 布尔变量 | `is_`、`has_`、`can_` | `is_initialized` |
| RTOS 任务入口 | 功能名加 `_task` | `communication_task()` |
| ISR 专用接口 | 名称加 `_from_isr` | `log_notify_from_isr()` |

### 4.2 函数

项目自行编写的函数使用小写蛇形，公共函数必须带模块前缀：

```c
void bsp_led_on(bsp_led_id_t led_id);
void bsp_led_off(bsp_led_id_t led_id);
motor_status_t motor_set_speed(motor_id_t motor_id,
                               int16_t speed_permille);
bool motor_is_running(motor_id_t motor_id);
```

函数名优先使用以下统一动词：

```text
init      初始化
deinit    反初始化
start     开始运行
stop      停止运行
enable    允许功能
disable   禁止功能
read      从硬件或流中读取
write     写入硬件或流
get       获取模块已经保存的值
set       修改目标值或配置
update    推进一次状态机或周期处理
reset     恢复初始状态
is/has/can 返回布尔判断
on        处理事件或回调
```

不要用多个近义词表达同一动作，例如同一个项目中混用 `get`、`fetch`、`obtain`。

只在当前 `.c` 文件使用的函数必须声明为 `static`：

```c
static void write_pin_level(bsp_led_id_t led_id, GPIO_PinState level);
```

### 4.3 变量与作用域

普通变量使用能够描述用途的名词：

```c
uint32_t retry_count;
uint16_t current_angle_deg;
uint16_t adc_raw;
```

外部可见的项目全局变量使用 `g_` 前缀：

```c
system_state_t g_system_state;
```

如果必须跨文件访问，在头文件中使用 `extern` 声明，并且只能在一个 `.c` 文件中定义：

```c
/* system_state.h */
extern system_state_t g_system_state;

/* system_state.c */
system_state_t g_system_state;
```

文件内静态变量使用 `s_` 前缀：

```c
static uint8_t s_tx_buffer[LOG_BUFFER_SIZE];
static bool s_is_initialized;
```

全局变量应尽量减少。能够通过模块接口访问的数据，不应直接暴露：

```c
/* 不推荐 */
extern int16_t g_motor_speed;

/* 推荐 */
motor_status_t motor_set_speed(motor_id_t motor_id,
                               int16_t speed_permille);
int16_t motor_get_speed(motor_id_t motor_id);
```

CubeMX 生成的 `huart3`、`htim2`、`hadc1` 等名称保持不变，不增加 `g_` 前缀。

### 4.4 类型

项目新定义的类型使用小写蛇形并以 `_t` 结尾：

```c
typedef enum
{
    MOTOR_STATE_IDLE = 0,
    MOTOR_STATE_RUNNING,
    MOTOR_STATE_BRAKING,
    MOTOR_STATE_FAULT
} motor_state_t;

typedef struct
{
    motor_state_t state;
    int16_t target_speed_permille;
    int16_t current_speed_permille;
} motor_control_t;
```

需要明确位宽的数据使用 `<stdint.h>` 类型：

```c
uint8_t
uint16_t
uint32_t
int16_t
int32_t
```

仅在位宽不重要、且与平台自然整数一致时使用 `int`。数组长度和内存大小使用 `size_t`。

### 4.5 宏、常量和枚举

宏使用全大写并带模块前缀：

```c
#define LOG_BUFFER_SIZE             (128U)
#define MOTOR_COUNT                 (8U)
#define MOTOR_SPEED_MAX_PERMILLE    (1000)
```

宏表达式和参数必须正确加括号：

```c
#define CLAMP(value, low, high) \
    (((value) < (low)) ? (low) : (((value) > (high)) ? (high) : (value)))
```

但能够使用普通函数或 `static inline` 函数完成时，不使用函数式宏。

枚举值使用大写并带模块或类型前缀，避免进入 C 全局命名空间后发生冲突：

```c
typedef enum
{
    BSP_LED_ID_STATE = 0,
    BSP_LED_ID_COMM,
    BSP_LED_ID_ACTIVITY,
    BSP_LED_ID_ERROR,
    BSP_LED_ID_COUNT
} bsp_led_id_t;
```

通信协议、故障码、Flash 数据格式中的枚举必须显式指定数值：

```c
typedef enum
{
    PROTOCOL_COMMAND_STOP = 0x01,
    PROTOCOL_COMMAND_SET_SPEED = 0x02,
    PROTOCOL_COMMAND_SET_LAMP = 0x03
} protocol_command_t;
```

### 4.6 单位、范围和缩写

物理量和时间名称必须包含单位或表示方法：

```c
timeout_ms
timeout_ticks
frequency_hz
voltage_mv
current_ma
angle_deg
speed_rpm
duty_permille
buffer_size_bytes
adc_raw
```

不要写成：

```c
timeout
frequency
voltage
duty
value
data
```

除通用缩写外，不自行创造缩写。以下缩写可以直接使用：

```text
ADC UART PWM DMA GPIO ISR RTOS CRC ID RX TX
```

同一个缩写不得出现多种形式，例如不能混用 `buf`、`buffer` 和 `buff`。本项目优先使用完整单词 `buffer`。

## 5. 代码格式

### 5.1 基本格式

- 使用 4 个空格缩进，不使用 Tab。
- 左大括号另起一行，与 CubeMX 生成代码保持一致。
- 每行建议不超过 100 个字符。
- 一个变量占一行。
- 二元运算符两侧保留空格。
- 文件末尾保留一个换行。
- 不保留行尾空格。

```c
if (status != HAL_OK)
{
    error_report(ERROR_UART_TRANSMIT);
}
```

所有控制结构都使用大括号，即使内部只有一条语句：

```c
if (is_ready)
{
    motor_start(motor_id);
}
```

### 5.2 声明与初始化

变量在尽量靠近首次使用的位置声明，并提供合理初值：

```c
HAL_StatusTypeDef status = HAL_ERROR;
uint32_t bytes_sent = 0U;
bool is_valid = false;
```

不要在同一行声明多个变量：

```c
uint16_t current_angle_deg;
uint16_t target_angle_deg;
```

整数常量使用与目标类型匹配的后缀：

```c
uint32_t timeout_ms = 100U;
int32_t direction = -1;
```

### 5.3 条件表达式

判断整数或位掩码时明确写出比较目标：

```c
if ((status_flags & MOTOR_FLAG_FAULT) != 0U)
{
    motor_stop_all();
}
```

布尔值可以直接判断：

```c
if (is_initialized)
{
    log_write("ready\r\n");
}
```

避免双重否定，例如 `if (!is_not_ready)`。

## 6. 头文件与接口

### 6.1 头文件必须可独立包含

头文件应直接包含自己所需的标准头文件，不依赖其他文件间接包含：

```c
#ifndef AITAID_BSP_LED_H
#define AITAID_BSP_LED_H

#include <stdbool.h>
#include <stdint.h>

/* Public declarations. */

#endif /* AITAID_BSP_LED_H */
```

头文件保护宏采用：

```text
AITAID_<PATH>_<FILE>_H
```

### 6.2 公共接口只暴露必要内容

引脚、电平和 HAL 句柄等板级细节通常放在 `.c` 文件中：

```c
/* bsp_led.h */
void bsp_led_on(bsp_led_id_t led_id);

/* bsp_led.c */
static GPIO_TypeDef *const s_led_ports[BSP_LED_ID_COUNT] = { /* ... */ };
```

上层不应知道 `LED_STATE` 使用 PA4、低电平点亮等细节。

### 6.3 指针和缓冲区

指针参数应说明：

- 是否允许为 `NULL`。
- 缓冲区需要多大。
- 数据由谁拥有。
- 函数是否保存该指针。
- 是否可以从中断调用。
- 函数是否阻塞。

不会修改输入数据时使用 `const`：

```c
log_status_t log_write(const uint8_t *data, size_t length);
```

缓冲区接口必须同时传递指针和长度，不能依赖调用方与被调用方默认相同大小。

## 7. 返回值与错误处理

只需要表达“是或否”时返回 `bool`：

```c
bool motor_is_running(motor_id_t motor_id);
```

可能存在多种错误原因时返回状态枚举：

```c
typedef enum
{
    MOTOR_STATUS_OK = 0,
    MOTOR_STATUS_INVALID_ID,
    MOTOR_STATUS_NOT_INITIALIZED,
    MOTOR_STATUS_BUSY,
    MOTOR_STATUS_FAULT
} motor_status_t;
```

调用可能失败的 HAL、驱动和服务函数时，应检查返回值：

```c
HAL_StatusTypeDef status;

status = HAL_UART_Transmit(&huart3, data, length, timeout_ms);
if (status != HAL_OK)
{
    error_report(ERROR_UART_TRANSMIT);
}
```

错误处理应明确选择以下行为之一：

- 重试。
- 返回错误。
- 进入安全状态。
- 锁定故障。
- 记录日志并继续。

不得无说明地忽略错误。

## 8. 中断与 FreeRTOS

### 8.1 中断规则

中断处理函数只进行必要工作：

1. 清除中断标志。
2. 保存少量数据或时间戳。
3. 设置标志、写入无阻塞缓冲区或通知任务。
4. 尽快退出。

中断中禁止：

- `HAL_Delay()`。
- 阻塞 UART 输出。
- `printf()` 或复杂格式化。
- 长循环。
- 浮点密集运算。
- 等待互斥锁。
- 执行复杂电机闭环逻辑。

只能从中断调用的接口使用 `_from_isr` 后缀：

```c
void communication_notify_from_isr(void);
```

### 8.2 共享数据

`volatile` 仅表示数据可能被异步修改，不提供原子性、互斥或内存同步。

任务和中断共享数据时，应根据数据大小和访问方式使用：

- 临界区。
- FreeRTOS 队列。
- 任务通知。
- 事件组。
- 明确可证明安全的单写单读结构。

每个公共接口应说明是否：

- 线程安全。
- 可重入。
- 可从 ISR 调用。
- 可能阻塞。

### 8.3 FreeRTOS 命名

FreeRTOS 自身的 `vTaskDelay()`、`xQueueReceive()`、`uxTaskPriorityGet()` 等名称保持原样。

项目代码不复制 FreeRTOS 的类型前缀风格，不使用以下形式：

```c
vLedToggle();
xMotorStart();
```

项目任务入口使用：

```c
static void communication_task(void *argument);
static void safety_task(void *argument);
```

## 9. MCU 安全规则

### 9.1 动态内存

关键控制路径默认不使用 `malloc()` 和 `free()`。FreeRTOS 对象优先在启动阶段统一创建；正式版本优先考虑静态分配。

### 9.2 外部输入校验

以下数据使用前必须检查：

- UART 帧长度、命令号、CRC 和参数范围。
- ADC 原始值和标定端点。
- 电机编号、速度、角度和模式。
- 数组索引和缓冲区长度。
- 来自 Linux 的目标值与状态切换请求。

Linux 命令不能绕过 MCU 的限位、堵转、失联和故障保护。

### 9.3 执行器默认安全

- 上电初始化期间，所有电机输出为停止状态。
- 启动 PWM 前先把比较值设置为安全值。
- 初始化失败时不得继续启动执行器。
- 通信超时或任务异常时进入可定义的安全停止状态。
- 闭环控制启用前必须确认反馈有效、方向正确且目标在允许范围内。

### 9.4 魔法数字

具有物理意义或多处使用的数值必须命名：

```c
#define MOTOR_COMM_TIMEOUT_MS       (500U)
#define MOTOR_MAX_DUTY_PERMILLE     (800)
#define JOINT_MAX_ANGLE_DEG          (260)
```

简单且含义明确的局部值可以直接使用，例如数组第一个元素的索引 `0U`。

## 10. 日志规则

日志等级统一为：

```text
ERROR   已发生故障或操作失败
WARN    异常但系统仍可继续
INFO    启动、模式变化和关键状态
DEBUG   调试细节，正式版本可关闭
```

日志应包含模块和事件，不使用含糊文本：

```text
[INFO][BOOT] firmware=0.1.0 clock_hz=168000000
[ERROR][MOTOR] id=2 fault=stall
```

日志不得改变实时控制结果。早期 Bring-up 阶段允许使用阻塞 UART；进入 FreeRTOS 和闭环控制阶段后，应限制日志长度、频率和调用环境。

禁止在高频中断中打印日志。

### 10.1 编译期日志等级

日志通过项目专属宏 `AITAID_LOG_LEVEL` 进行编译期裁剪。不得在业务代码中散布 `#ifdef DEBUG`，也不得使用 `NDEBUG` 控制日志；`NDEBUG` 通常用于控制标准 `assert()`。

日志等级使用预处理宏定义，不能使用枚举代替，因为 `#if` 在预处理阶段无法读取 C 枚举值：

```c
#define LOG_LEVEL_NONE     (0)
#define LOG_LEVEL_ERROR    (1)
#define LOG_LEVEL_WARN     (2)
#define LOG_LEVEL_INFO     (3)
#define LOG_LEVEL_DEBUG    (4)

#ifndef AITAID_LOG_LEVEL
#define AITAID_LOG_LEVEL LOG_LEVEL_INFO
#endif
```

各等级日志宏在日志模块的公共头文件中统一定义：

```c
#if AITAID_LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_ERROR(...) log_printf(LOG_LEVEL_ERROR, __VA_ARGS__)
#else
#define LOG_ERROR(...) ((void)0)
#endif

#if AITAID_LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_WARN(...) log_printf(LOG_LEVEL_WARN, __VA_ARGS__)
#else
#define LOG_WARN(...) ((void)0)
#endif

#if AITAID_LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(...) log_printf(LOG_LEVEL_INFO, __VA_ARGS__)
#else
#define LOG_INFO(...) ((void)0)
#endif

#if AITAID_LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) log_printf(LOG_LEVEL_DEBUG, __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif
```

业务代码只调用日志宏，不自行判断构建模式：

```c
LOG_DEBUG("adc_raw=%u", adc_raw);
LOG_INFO("system started");
LOG_ERROR("motor fault=%u", fault_code);
```

推荐构建配置：

| 构建模式 | `AITAID_LOG_LEVEL` | 编译进入固件的日志 |
|---|---:|---|
| Debug | `4` | ERROR、WARN、INFO、DEBUG |
| Release | `3` | ERROR、WARN、INFO |
| 最小正式版 | `2` | ERROR、WARN |
| 完全关闭 | `0` | 无 |

CMake 可以按构建类型传入日志等级：

```cmake
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:AITAID_LOG_LEVEL=4>
    $<$<CONFIG:Release>:AITAID_LOG_LEVEL=3>
)
```

被关闭的日志宏展开为 `((void)0)`，不执行参数求值，也不产生对应的日志调用。正式版本通常保留 ERROR 和 WARN，便于现场故障定位；只有在资源或时序要求明确时才完全关闭日志。

后续如需运行时调节等级，应采用“编译期最高等级 + 运行时过滤”的两层结构。编译期已经移除的日志不能在运行时重新开启。

## 11. CubeMX 与第三方代码

### 11.1 CubeMX 生成文件

在 CubeMX 管理的文件中，手写代码必须放在对应的用户区域：

```c
/* USER CODE BEGIN 2 */
app_init();
/* USER CODE END 2 */
```

较大的实现应放入独立模块，不应全部堆积在 `main.c` 的用户区域。

修改外设配置时优先修改 `.ioc` 并重新生成，而不是只修改生成的初始化代码。

### 11.2 第三方库

以下代码保留原有风格：

- STM32 HAL。
- CMSIS。
- FreeRTOS Kernel。
- 引入的第三方驱动库。

适配第三方库时，通过薄封装转换为本项目接口，不批量重命名或格式化第三方源码。

## 12. 注释与文档

注释解释“为什么”和约束条件，不重复代码已经表达的内容：

```c
/* Brake briefly before reversing to avoid H-bridge shoot-through. */
motor_brake(motor_id);
```

不推荐：

```c
/* Set speed to zero. */
speed_permille = 0;
```

公共接口的注释至少说明：

- 功能。
- 参数含义、单位和有效范围。
- 返回值。
- 阻塞行为。
- ISR/任务上下文限制。
- 可能触发的硬件动作。

### 12.1 Doxygen 公共接口注释

公共函数、公共类型、公共枚举、公共宏以及重要的任务或中断接口使用 Doxygen 注释。Doxygen 注释必须以 `/**` 开始；普通的 `/*` 块注释不作为公共接口文档。

```c
/**
 * @brief 打开指定的板载 LED。
 *
 * @param[in] led_id LED 编号。
 *
 * @note LED 的高低有效电平由 BSP 内部处理。
 */
void bsp_led_on(bsp_led_id_t led_id);
```

可能返回多个状态的函数使用 `@retval` 分别说明：

```c
/**
 * @brief 设置指定电机的目标速度。
 *
 * @param[in] motor_id 电机逻辑编号。
 * @param[in] speed_permille 目标速度，范围为 -1000～1000。
 *
 * @retval MOTOR_STATUS_OK 设置成功。
 * @retval MOTOR_STATUS_INVALID_ID 电机编号无效。
 * @retval MOTOR_STATUS_FAULT 电机处于故障锁定状态。
 *
 * @warning 本函数不会绕过限位和故障保护。
 */
motor_status_t motor_set_speed(motor_id_t motor_id,
                               int16_t speed_permille);
```

常用标签及用途：

| 标签 | 用途 |
|---|---|
| `@brief` | 一句话说明接口用途 |
| `@param[in]` | 输入参数 |
| `@param[out]` | 输出参数 |
| `@param[in,out]` | 输入输出参数 |
| `@return` | 返回值的总体含义 |
| `@retval` | 某个具体返回值的含义 |
| `@note` | 普通注意事项 |
| `@warning` | 硬件风险或重要限制 |

公共接口的完整注释写在 `.h` 声明处，`.c` 文件只解释内部实现中不明显的原因、算法或硬件约束。不得在 `.h` 和 `.c` 中复制两份相同的接口注释。

简单、含义明确的私有函数不强制使用 Doxygen 注释：

```c
static bool is_valid_led_id(bsp_led_id_t led_id);
```

复杂或有调用环境限制的私有函数仍应说明约束，特别是以下情况：

- 只能由任务调用。
- 可以或只能由 ISR 调用。
- 会阻塞。
- 要求调用方持有锁或处于临界区。
- 会直接启动电机、制动或改变电源状态。

可以使用 VS Code 的 Doxygen Documentation Generator 扩展辅助生成注释骨架，但生成结果必须人工补充参数范围、单位、上下文限制和硬件风险，不能把自动生成的占位文本直接保留在代码中。

`TODO` 必须写清要做什么以及完成条件：

```c
/* TODO: Replace polling with ADC DMA after all six channels are verified. */
```

不要只写：

```c
/* TODO: Fix later. */
```

## 13. 代码评审检查表

提交或请求评审前至少检查：

- [ ] 文件、函数、变量和类型名称符合本规范。
- [ ] 外部全局变量使用 `g_`，文件静态变量使用 `s_`。
- [ ] 新增公共符号带模块前缀。
- [ ] 时间和物理量名称包含单位。
- [ ] 数组访问和外部输入已经检查范围。
- [ ] HAL 和驱动返回值已处理。
- [ ] 中断中没有阻塞、日志或复杂计算。
- [ ] 任务与中断共享数据有明确同步方式。
- [ ] 电机和灯光在异常路径下回到安全状态。
- [ ] CubeMX 手写代码位于 USER CODE 区域。
- [ ] 没有无意修改生成代码或第三方库。
- [ ] 公共接口使用 `/** ... */` Doxygen 注释并说明参数范围和单位。
- [ ] 业务代码通过日志宏输出，没有散布 `#ifdef DEBUG`。
- [ ] 当前构建模式的 `AITAID_LOG_LEVEL` 设置符合交付要求。
- [ ] 编译无新增警告。
- [ ] 已说明实板验证方法和未验证项目。

## 14. 示例模块

```c
/* bsp_led.h */
#ifndef AITAID_BSP_LED_H
#define AITAID_BSP_LED_H

#include <stdbool.h>

typedef enum
{
    BSP_LED_ID_STATE = 0,
    BSP_LED_ID_COMM,
    BSP_LED_ID_ACTIVITY,
    BSP_LED_ID_ERROR,
    BSP_LED_ID_COUNT
} bsp_led_id_t;

void bsp_led_init(void);
void bsp_led_on(bsp_led_id_t led_id);
void bsp_led_off(bsp_led_id_t led_id);
void bsp_led_toggle(bsp_led_id_t led_id);
bool bsp_led_is_on(bsp_led_id_t led_id);

#endif /* AITAID_BSP_LED_H */
```

```c
/* bsp_led.c */
#include "bsp_led.h"

#include "gpio.h"

static bool s_is_initialized;

static bool is_valid_led_id(bsp_led_id_t led_id)
{
    return (led_id >= BSP_LED_ID_STATE) &&
           (led_id < BSP_LED_ID_COUNT);
}

void bsp_led_init(void)
{
    s_is_initialized = true;
}
```

示例只展示命名、格式和模块边界，不代表完整的 LED 驱动实现。
