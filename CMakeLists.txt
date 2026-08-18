cmake_minimum_required(VERSION 3.22)

project(FullMapOracleStoneLocator LANGUAGES CXX)

# Anomaly 插件是 Windows x64 原生动态库。
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Anomaly plugins target Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 消费已安装的 Anomaly SDK 包。配置时指向 SDK 的 cmake 目录，例如：
#   cmake -S . -B build -DAnomalySDK_DIR=C:/path/to/anomaly-sdk/lib/cmake/AnomalySDK
find_package(AnomalySDK CONFIG REQUIRED)

set(ORACLE_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package/FullMapOracleStoneLocator")

# anomaly_add_plugin 由 SDK 提供：编译成 plugin.dll，并把 manifest.json 复制到包目录。
anomaly_add_plugin(anomaly_oracle_stone_locator
    SOURCES src/plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME FullMapOracleStoneLocator
    OUTPUT_DIRECTORY "${ORACLE_PACKAGE_DIR}")

target_compile_definitions(anomaly_oracle_stone_locator PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# 源码以仓库根为 include 根（#include "plugins/common/localization.hpp"）。
target_include_directories(anomaly_oracle_stone_locator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")

# 源文件含中文注释/字符串，强制 MSVC 以 UTF-8 读取并生成 UTF-8 执行字符集，
# 否则在中文 Windows（代码页 936）上会把 UTF-8 误当作 GBK 解析而编译失败。
if(MSVC)
    target_compile_options(anomaly_oracle_stone_locator PRIVATE /utf-8)
endif()

# 把本地化文件一并放进包里，得到一个完整可加载的插件包。
add_custom_command(TARGET anomaly_oracle_stone_locator POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/locales"
            "${ORACLE_PACKAGE_DIR}/locales"
    VERBATIM)
