# Preserve JNI entry points if the consuming APK enables minification.
-keep class com.example.rk3566hardwarevalidator.hardware.pose.RknnPoseEngine { *; }
# The supplied sherpa AAR has an empty proguard.txt; JNI reflects these config fields/types.
-keep class com.k2fsa.sherpa.onnx.** { *; }
