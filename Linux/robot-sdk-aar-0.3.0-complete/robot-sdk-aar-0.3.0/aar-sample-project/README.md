# AAR 接入独立示例工程 0.3.0

App团队先读 [首轮交付说明](APP团队首轮交付说明_0.3.0.md) 确认范围和验收，再按 [AAR开发者教程](APP开发者教程_AAR.md) 接入自己的APK。

解压后使用 JDK 25、Android SDK Platform 37，从本目录执行 `.\gradlew.bat :sample:assembleDebug`。两个 AAR 在 `libs/`；本工程不包含 Robot SDK 实现源码。输出 `sample/build/outputs/apk/debug/sample-debug.apk`，包名 `com.example.rk3566hardwarevalidator.sdksample`。

权限、接口样例、两颗摄像头首配、生命周期、板卡 ADB 安装与验收见同目录《SDK_AAR接入指南.md》。不要把 `local.properties`、`.env`、API Key 或缓存加入交付包。
