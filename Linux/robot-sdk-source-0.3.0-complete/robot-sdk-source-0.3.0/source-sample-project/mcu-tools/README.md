# PC 端 MCU 模拟器

该工具用于在真实 MCU 固件完成前验证 Android UI、USB 串口链路、CRC、ACK 重试和状态回读。

硬件需要两只 3.3 V USB 转 TTL 模块：板卡连接模块 A，电脑连接模块 B；A.TX 接 B.RX、A.RX 接 B.TX、GND 相连，不接 VCC。板卡和电脑的两个 USB Host 口不能直接连接来模拟串口。

安装与运行：

```powershell
python -m pip install -r mcu-tools\requirements.txt
python -m serial.tools.list_ports
python mcu-tools\mcu_simulator.py --port COM7
```

运行协议测试：

```powershell
python -m unittest discover -s mcu-tools -p "test_*.py"
```

完整帧格式、接线和 MCU 实现要求见 [../docs/MCU通信协议.md](../docs/MCU通信协议.md)。
# SDK 0.3.0 extension-v1

默认模拟器只支持原0x02基本灯光/运动。显式添加`--extensions`启用测试夹具；`--ack-only`只接受命令、不改变实际状态，用于检验ACK_RECEIVED不会被显示为APPLIED。夹具范围、ID仅用于PC测试，不能用于真实机构。完整payload/能力启用/安全契约见`../docs/MCU扩展协议与安全控制.md`。

离线无硬件测试：`python -m unittest discover -s mcu-tools`（从项目根目录运行）。测试无需pyserial；真实串口模拟才需requirements.txt。
