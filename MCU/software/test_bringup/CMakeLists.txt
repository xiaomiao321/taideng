cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME test_bringup)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# FreeRTOS kernel (vendor code, built with the toolchain's default warnings)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Src/tasks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Src/queue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Src/list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Src/timers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Src/event_groups.c
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Src/stream_buffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Src/port.c
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Src/heap_4.c
)

# Hand-written sources: HAL callback dispatch, BSP, protocol, application, tests
set(AITAID_SOURCES
    # HAL 弱回调的唯一实现位置（集成层，非 CubeMX 管理）
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/hal_callback_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_common.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_light_pwm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_light.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_motor_hbridge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_motor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_buzzer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_linux_power.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_linux_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_adc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_watchdog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_uart_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_debug_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/flash_journal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Src/bsp_param_flash.c
    # 协议层与命令层：不依赖 HAL，可在 PC 上用 HostTest/run.ps1 单测
    ${CMAKE_CURRENT_SOURCE_DIR}/Protocol/Src/mcu_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Protocol/Src/mcu_command.c
    # 应用层：串口收发与协议接线
    ${CMAKE_CURRENT_SOURCE_DIR}/App/src/app_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/src/debug_console.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/src/debug_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/src/debug_board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/src/joint_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Src/bringup_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Src/bringup_cases.c
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${AITAID_SOURCES})
# 只对手写代码加严警告；CubeMX、HAL 和 FreeRTOS 保持厂商原样，不加。
set_source_files_properties(${AITAID_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wextra")

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/FreeRTOS/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/App/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Protocol/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
$<$<CONFIG:Debug>:AITAID_LOG_LEVEL=4>
$<$<CONFIG:Release>:AITAID_LOG_LEVEL=3>
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
