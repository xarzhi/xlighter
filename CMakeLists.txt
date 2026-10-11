# xLighter — 屏幕亮度调整助手
# 基于 Core UI（纯 C API + .uix 单文件组件）。
#
# 依赖 core-ui 的 release 包（已拷进 third_party/core-ui/）：
#   include/ui_core.h           头文件
#   lib/dynamic/core-ui.dll     动态库（运行时需和 exe 同目录）
#   lib/dynamic/core-ui.lib     导入库
#   lib/static/core-ui.lib      静态库（单 exe 分发用，见下方说明）
#
# 构建需要：Windows 10+ · CMake 3.20+ · MSVC 2019+（或 clang-cl）+ Ninja。
# 直接跑 scripts/build.ps1 即可（它会自动加载 vcvars64 环境）。

cmake_minimum_required(VERSION 3.20)
project(xLighter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源文件含 UTF-8 中文注释；MSVC 默认按代码页 936 (GBK) 读，会误解析多字节
# 字符导致语法错乱（C4819 / C2059）。显式声明 UTF-8。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# core-ui 头文件 + 嵌入辅助函数
set(CORE_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/core-ui")
include("${CORE_UI_DIR}/cmake/UiCoreHelpers.cmake")

add_executable(xLighter WIN32
    src/main.cpp
    resources/app.rc)   # 资源 ID=1 应用图标（core-ui / 托盘 / 任务栏共用）

target_include_directories(xLighter PRIVATE "${CORE_UI_DIR}/include")

# 把 ui/app.uix 烤进可执行文件，生成 app_uix.embed.h
ui_core_embed_text(xLighter
    FILE ui/app.uix
    OUT  app_uix.embed.h
    VAR  k_app_uix)

# 静态链接 core-ui（单 exe，无需 core-ui.dll）。
# 注意：这份 core-ui.lib 是用 /MD（动态 CRT）编译的，内部函数以 __imp_* 形式
# 导入 CRT，所以**不能**用 /MT（否则 LNK2038 CRT 不匹配 + LNK2019 __imp_sinf 等
# 一堆 unresolved）。保持默认 /MD 即可 —— 最终 exe 依赖系统自带的
# vcruntime140.dll / msvcp140.dll，但 core-ui.dll（4.4MB）已经整个链进 exe 了。
ui_core_link_static(xLighter "${CORE_UI_DIR}/lib/static/core-ui.lib")
# core-ui 静态库用了 IME 组合窗口 API（ImmGetContext 等），在 imm32.lib 里；
# 它自带的系统库清单漏了这个，手动补上。
target_link_libraries(xLighter PRIVATE imm32)
