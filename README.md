# xLighter — 屏幕亮度调节助手

基于 [Core UI](https://ghboke.github.io/core-ui/) 的 Windows 屏幕亮度调节工具，Fluent 2 设计。

## 功能

- **亮度滑块**：0~100 实时调节（`SetDeviceGammaRamp` gamma 软调，逐显示器生效）
- **四种模式**：标准 / 防蓝光 / 护眼 / 游戏（不同 gamma 曲线：压蓝、暖色降亮、拉对比）
- **设置页**：开机自启（静默启动，写 `HKCU\...\Run` 带 `/silent`）
- **托盘图标**：左键显示/隐藏主窗口，右键菜单（显示、开机自启、退出）
- **单实例**：重复启动唤起已开窗口；关闭窗口 → 最小化到托盘
- **配置持久化**：亮度 / 模式存 `%APPDATA%\xLighter\config.ini`；退出恢复原始 gamma

## 技术栈

- **UI**：Core UI（纯 C API + `.uix` Vue3 风格单文件组件，Direct2D/D3D11 硬件加速，Fluent 2）
- **语言**：C++17，`.uix` 脚本走 QuickJS-NG
- **构建**：CMake + Ninja + MSVC（静态链接 core-ui，产出单 exe）

## 目录结构

```
xLighter/
├── src/main.cpp            C 胶水：加载 .uix、开窗口、单例/托盘/自启/gamma 引擎
├── ui/app.uix              界面（Vue3 SFC 风格：window + script + style + template）
├── resources/              app.ico（logo.png 生成）+ app.rc（资源 ID=1）
├── third_party/core-ui/    core-ui 静态库 + 头文件 + cmake helpers
├── scripts/build.ps1       一键构建脚本（自动配好 MSVC 环境）
├── CMakeLists.txt
└── logo.png                原始图标素材
```

## 构建

需要 Windows 10+ · CMake 3.20+ · MSVC 2019+ · Ninja。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
```

产物是 `build/xLighter.exe`（**单文件**，core-ui 已静态链入，无需额外 DLL）。

> 注：`build.ps1` 里的 MSVC / Windows SDK 路径是按作者本机硬编码的（VS18 + D 盘 SDK），
> 换机器请按注释改这几行。

## 已知限制

- 亮度是 **gamma 软调**（改显卡 gamma 表），不改变显示器背光，因此：
  - 不省电、黑色压不到纯黑；
  - 约 40% 以下因 8-bit 色阶塌缩，再往下调观感变化很小。
- 要做到「一路暗到底」，需接硬件亮度（DDC/CI `SetVCPFeature` 或 dxva2 `SetMonitorBrightness`），
  后续可加。
