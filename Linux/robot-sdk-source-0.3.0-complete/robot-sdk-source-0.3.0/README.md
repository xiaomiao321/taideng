# Robot SDK 源码接入 0.3.0

目标设备：RK3566 / Android 11 / `arm64-v8a`。此包包含 Robot SDK 可编辑源码和独立宿主示例；示例依赖 `project(':robot-sdk')`，不使用预编译 Robot SDK AAR。

App团队首次接收请先读 [首轮交付说明](APP团队首轮交付说明_0.3.0.md)，再读源码教程；交付说明包括分工、硬件清单和验收表。

- `robot-sdk-source-sample-0.3.0-debug.apk`：源码示例，包名 `com.example.rk3566hardwarevalidator.sdksourcesample`。
- `robot-sdk-source-sample-0.3.0-project.zip`、`source-sample-project/`：可独立构建工程。SDK Kotlin、模型、JNI、RKNN Runtime 均位于自身 `robot-sdk/src/main/`。
- `sherpa-onnx-v1.13.6-arm64-rknn-no-runtime.aar`：供应方语音依赖，其第三方实现源码不包含在本包中。
- `SDK源码接入指南.md`、`SDK数据与蓝牙对接.md`、`MCU通信协议.md`、`MCU扩展协议与安全控制.md`、`SHA256.txt`：源码构建、接口、板卡安装、协议与递归文件校验值。
- `APP开发者教程_源码.md`：开发者分步学习，含可编译的最小坐姿页面及语音、拍题、MCU和数据传输接法；优先阅读。

构建需要 JDK 25、SDK 37、NDK `27.2.12479018`、CMake `3.22.1`，工程自带 Gradle Wrapper。示例可与 AAR 示例并存。SDK 不提供蓝牙传输、解题服务器或固件刷写；包含可选图片上传客户端。实测与待验收项见 `SDK0.3.0验证记录.md`；`mcu-tools/` 提供协议模拟器与测试，不驱动真实硬件。
