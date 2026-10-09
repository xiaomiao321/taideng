# SDK 源码接入独立工程 0.3.0

App团队先读 [首轮交付说明](APP团队首轮交付说明_0.3.0.md) 确认范围和验收，再按 [源码开发者教程](APP开发者教程_源码.md) 接入自己的APK。

使用 JDK 25、SDK 37、NDK `27.2.12479018`、CMake `3.22.1`，执行 `.\gradlew.bat :robot-sdk:testDebugUnitTest :sample:assembleDebug`。输出 `sample/build/outputs/apk/debug/sample-debug.apk`，包名 `com.example.rk3566hardwarevalidator.sdksourcesample`。

`robot-sdk/src/main/` 内含 Kotlin、模型、JNI、RKNN Runtime；示例仅通过 `implementation project(':robot-sdk')` 接入。sherpa 供应方依赖位于 `robot-sdk/libs/`。模型及 CMake 均不引用其他仓库模块。接口样例、生命周期与板卡部署见同目录《SDK源码接入指南.md》。
