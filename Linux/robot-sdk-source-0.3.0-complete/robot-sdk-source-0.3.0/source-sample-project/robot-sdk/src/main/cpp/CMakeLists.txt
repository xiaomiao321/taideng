cmake_minimum_required(VERSION 3.22.1)

project(rknn_pose LANGUAGES CXX)

add_library(rknnrt SHARED IMPORTED)
set_target_properties(
    rknnrt
    PROPERTIES IMPORTED_LOCATION
    "${CMAKE_CURRENT_SOURCE_DIR}/rknn/${ANDROID_ABI}/librknnrt.so"
)

add_library(pose_native SHARED pose_jni.cpp)

target_include_directories(pose_native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(pose_native PRIVATE cxx_std_17)
target_compile_options(pose_native PRIVATE -Wall -Wextra -Wpedantic)

find_library(log_library log)
target_link_libraries(pose_native PRIVATE rknnrt "${log_library}")
