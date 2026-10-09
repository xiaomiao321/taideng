# main 测试归档

保存日期：2026-10-09。每个 `.c` 都包含完整的 main、时钟配置及 HAL 时间基准回调。
这些文件未加入 CMake，当前 `Core/Src/main.c` 和它的内容保持不变。

## 来源和限制

Git 导出和当前文件复制均逐字节保留，不修正历史代码。没有独立提交的阶段按对话流程重建，不能当作原始文件或新的实板验证记录。
原始字符串日志与 LED 合并在 01 中；独立格式化日志参考为 04；分级日志为 05。
10 使用已停用的阻塞接收接口，只作历史阅读。当前维护的是 11 的中断接收接口。
本次不保存已被评审指出有越界、回调错误等问题的中间草稿。

| 文件 | 来源 | 功能及注意事项 |
|---|---|---|
| [01_main_led_uart.c](01_main_led_uart.c) | Git 59e452e 原样导出 | 四灯轮流点亮及 USART3 原始字符串日志。 |
| [02_main_led_pa5_debug.c](02_main_led_pa5_debug.c) | Git 53776f2 原样导出 | PA5 虚焊排查阶段，包含直接 GPIO 操作和历史调试变量。 |
| [03_main_buzzer.c](03_main_buzzer.c) | Git 5155075 原样导出 | 蜂鸣器变频测试；原循环每次仅增加 1Hz，完整遍历耗时很长。 |
| [04_main_log_printf.c](04_main_log_printf.c) | 重建：当前 main 模板 + 对话中的测试流程 | 原始格式化日志参考，bsp_log_printf 不自动换行；未找到独立提交，非原样恢复。 |
| [05_main_log_levels.c](05_main_log_levels.c) | Git a25ae0e 原样导出 | ERROR/WARN/INFO/DEBUG 和 Debug/Release 编译期日志裁剪；WARN/ERROR 为测试内容。 |
| [06_main_home_poll.c](06_main_home_poll.c) | 重建：当前 main 模板 + 对话中的测试流程 | 原始电平轮询；采用评审建议的 5ms 周期，不保证软件消抖。未注册项目 EXTI 回调。 |
| [07_main_home_exti.c](07_main_home_exti.c) | 重建：当前 main 模板 + 对话中的测试流程 | 下降沿中断累计计数；BSP 仍为 20ms 边沿过滤，不保证一按一次。 |
| [08_main_linux_power.c](08_main_linux_power.c) | Git 11916e8 原样导出 | 启动一次 100ms 电源键脉冲，while 周期 update；先断开 Linux，观察 PB12。 |
| [09_main_uart1_loopback.c](09_main_uart1_loopback.c) | 重建：当前 main 模板 + 对话中的测试流程 | 先中断接收，再阻塞发送；短接 USART1 TX/RX，固定 8 字节比较。对应模块 `bsp_uart_loopback` 已移除，仅作历史记录。 |
| [10_main_linux_uart_blocking.c](10_main_linux_uart_blocking.c) | 重建：当前 main 模板 + 对话中的测试流程 | 旧阻塞 ReceiveToIdle 接收流程，仅作历史参考：bsp_linux_uart_read 已被当前接口替换，不能直接用当前 BSP 编译；不是即时回环方案。 |
| [11_main_linux_uart_it.c](11_main_linux_uart_it.c) | 2026-10-09 当前工作文件逐字节复制 | 中断变长接收，先挂接收再每秒发 Hello，收到后 USART3 原样输出。 |
| [12_main_adc_single.c](12_main_adc_single.c) | 单路 ADC 实板通过后的工作文件逐字节复制 | POT1 / PC2 / Channel 12 轮询采样，每约 100ms 打印；用户已确认信号源及电位器测试通过。 |
| [13_main_adc_scan_dma.c](13_main_adc_scan_dma.c) | 切换 PC5 单路定位测试前，main 工作文件逐字节复制 | 六路 DMA 的 main 测试逻辑；整组准确性仍在排查，尚未宣布六路通过。保存时 CubeMX 已切到单路 Channel 15，因此切回需另行恢复六路 ADC 配置。 |
| [14_main_adc_pc5_poll.c](14_main_adc_pc5_poll.c) | PC5 虚焊修复、用户确认读数恢复后的 main 原样复制 | 单路 Channel 15 轮询定位测试，每秒打印 POT6 原始值；恢复此 main 时需配套单路 ADC 配置。 |
| [15_main_adc_scan_dma_verified.c](15_main_adc_scan_dma_verified.c) | PC5 虚焊修复、恢复六路 DMA 后的 main 原样复制 | 用户已确认六路测试恢复正常；按 POT1～POT6 顺序每秒打印一组。恢复时需配套六路扫描、Normal DMA 配置。 |

12 对应的单路 BSP 头文件和实现也原样保存在 `12_adc_single_bsp/` 中。切回此阶段时还需恢复单路 ADC 的 CubeMX 配置；main 与 BSP 备份不包含旧 IOC。

## 如何切回某项测试

1. 先备份当前 `Core/Src/main.c`，将选中的完整归档文件复制到这个位置。一次只编译一个 main。
2. 检查 CMake 包含该版本使用的 BSP 源文件，不添加 `Tests/main_snapshots/*.c`。
3. 09 依赖已移除的 `bsp_uart_loopback` 模块，不能直接用当前 BSP 编译；如需 UART 回环，请基于 `bsp_linux_uart` 在 `Tests/` 里新增回环测试用例。11 使用当前维护的 `bsp_linux_uart` 中断接收接口。
4. HOME 中断需要 EXTI15_10；USART1 中断测试需要 USART1_IRQn 及 `HAL_UART_IRQHandler(&huart1)`。当前 IOC 已开启两者。
5. 10 是旧接口参考，不能直接替换后运行。历史版本应同时核对当时 BSP/IOC，不能只靠 main 完整就假定可以恢复全部运行行为。
6. Debug 构建后按现有 F5 烧录。正式运行电源脉冲或接电机之前检查接线和外设状态。

后续进入 ADC 测试前，11 已保留。完成新阶段时继续增加独立文件及本表记录。

## 校验记录

已使用当前 STM32 ARM 工具链及项目头文件对除 10 外的 10 个 main 做语法检查，全部通过，无警告。这不代表所有历史版本已重新链接或通过实板测试；10 因旧接口已停用而不参与检查。

以下 SHA-256 用于确认文件复制未发生变化，不代表实板验证结果。

```text
0ad69fa6ef8a32e0806f2230bd1201c9c27dc217b1a313979105e9dbe7073df3  01_main_led_uart.c
74a627c0d510899d954427f7dc1a93864557169bc49be3537650bd8cf1fbfade  02_main_led_pa5_debug.c
0a50be8764004bbf467628423ceeca57da641d491d6398e90160c75c6b83bc0e  03_main_buzzer.c
ddce46e2f576048481a37611bc4419683989b9d5cb6c6e417c57eccf9f2ed270  04_main_log_printf.c
6b9294fbb6b9e668ac741c2ced65bdd195e598f5d404540f90c1e30d6bcb3502  05_main_log_levels.c
02d582877afae1fb0da1334396eccf0ff0d4aa6ed0df7b393268a312c67ca694  06_main_home_poll.c
508bd02519457acd8e6bfb0c2498955f112c7b6c7227d0ce61490c6678601db0  07_main_home_exti.c
4dbed65134fcd69855e3387072dac5e5a7e9ba6053566b2c8fe2432eabaaca0f  08_main_linux_power.c
1e5fe1e1c6cfe019b36b643d164127ea5a0375dc38843c538f88615e2564b5be  09_main_uart1_loopback.c
157080a4d99f220e644680a0da343ab9ef0d80c4787b499c194f9d733dc2d40c  10_main_linux_uart_blocking.c
d6c45ac5c67eb5e1ab6a4bfa15a13896e8cb789132d6d64a48af25cd0b67e905  11_main_linux_uart_it.c
51b06fa39c47a1d7ed4d83719662c3ac689de708876d5800bcfddb0f89c7a8a1  12_main_adc_single.c
a85e8df736c9590e3fd1667b959c43361c9ddb1c9397d4c0156a5b4a5fd2ca85  13_main_adc_scan_dma.c
9cebb2a8d127c765c3b146f8c84dc209d9a7fb5ff4e980e8c340fc82043100db  14_main_adc_pc5_poll.c
c236da09195b6fb9e933a766e7f670cab89cc42f732d8c7552b033bc42efdd5a  15_main_adc_scan_dma_verified.c
```
