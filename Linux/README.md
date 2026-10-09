# Robot SDK 0.3.0 交付包

打包日期：2026-10-07。面向原生 Android App 团队，目标为 RK3566 / Android 11 / arm64-v8a。

## 选择接入方式

- `robot-sdk-aar-0.3.0-complete.zip`：二进制 AAR、语音依赖 AAR、AAR 接入示例 APK、独立示例工程、AAR 接入指南及开发者教程。
- `robot-sdk-source-0.3.0-complete.zip`：可编辑 SDK 源码、模型和本地运行库、语音依赖 AAR、源码接入示例 APK、独立示例工程、源码接入指南及开发者教程。

两份包均包含首轮交付说明、历史验证记录、数据与蓝牙对接说明、MCU 协议、协议模拟工具和内部文件 SHA-256 清单。每份完整包可单独交付；先解压，再从包内 `README.md` 和 `APP团队首轮交付说明_0.3.0.md` 开始阅读。

包内同时保留展开的示例工程目录和对应的 `*-project.zip`，内容用途相同，选择一份导入 Android Studio 即可。工程 ZIP 不是完整交付包，请对外发送这里的 `*-complete.zip`。

## 校验及范围

同目录的 `SHA256.txt` 校验两份完整 ZIP 和本说明文件；解压后各包的 `SHA256.txt` 用于逐文件校验。Windows PowerShell 可用 `Get-FileHash -LiteralPath '<ZIP 路径>' -Algorithm SHA256` 检查外层 ZIP。

本次仅打包现有 0.3.0 交付物，不修改 SDK、APK、AAR 或版本号，不代表新增板卡测试、量产认证或待验收项已经完成。板卡测试范围及双摄、真实 MCU 等待验收项以包内验证记录和首轮交付说明为准。App 团队将获得同款板卡、摄像头及 USB 麦克风，具体硬件清单仍按交付说明确认。

本包不含个人 API Key、电脑服务器 `.env`、整机固件或 BSP；解题服务器、蓝牙传输以及产品业务 UI 的职责边界见包内文档。示例 APK 为调试版，独立工程首次构建需要另行配置 Android 构建环境并获取依赖。
