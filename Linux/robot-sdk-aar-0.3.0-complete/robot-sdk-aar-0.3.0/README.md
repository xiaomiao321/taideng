# Robot SDK AAR 0.3.0

目标设备：RK3566 / Android 11 / `arm64-v8a`。此包用于二进制 AAR 接入；安装到板卡的是示例或宿主 APK。

App团队首次接收请先读 [首轮交付说明](APP团队首轮交付说明_0.3.0.md)，再读AAR教程；交付说明包括分工、硬件清单和验收表。

- `robot-sdk-0.3.0.aar` 与 `sherpa-onnx-v1.13.6-arm64-rknn-no-runtime.aar`：需同时加入宿主。
- `robot-sdk-aar-sample-0.3.0-debug.apk`：AAR 示例，包名 `com.example.rk3566hardwarevalidator.sdksample`。
- `robot-sdk-aar-sample-0.3.0-project.zip`、`aar-sample-project/`：独立示例工程，SDK 仍为 AAR。
- `SDK_AAR接入指南.md`：依赖、角色绑定、业务结果/JSON、生命周期、ADB 安装与验收。
- `APP开发者教程_AAR.md`：开发者分步学习，含可编译的最小坐姿页面及语音、拍题、MCU和数据传输接法；优先阅读。
- `SDK数据与蓝牙对接.md`、`MCU通信协议.md`、`MCU扩展协议与安全控制.md`、`SHA256.txt`：数据边界、协议与递归文件校验值。

还需显式 Maven 依赖 `usb-serial-for-android:3.11.0`、`androidx.core:core-ktx:1.10.1`。SDK 不提供蓝牙传输、解题服务器或整机刷机；包含可选图片上传客户端，复制 JSON 仅展示宿主可用的数据接口。摄像头角色默认人工确认且仅会话有效，拍题不会借用坐姿设备。实测与待验收项见 `SDK0.3.0验证记录.md`；`mcu-tools/` 提供协议模拟器与测试，不驱动真实硬件。
