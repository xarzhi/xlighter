// xLighter — 屏幕亮度调节助手
//
// 功能：
//   - 主界面：亮度滑块 + 标准/防蓝光/护眼/游戏 四种模式（gamma 软调）
//   - 设置页：开机自启（静默启动，写 HKCU\...\Run）
//   - 托盘图标：左键显示/隐藏主窗口，右键菜单（显示/自启/退出）
//   - 关闭窗口 → 最小化到托盘；单实例（第二个实例唤起第一个）
//   - 静默启动（命令行带 /silent）：开隐藏窗口，只留托盘
//
// .uix 侧把 brightness / mode / autoStart 写进 reactive 状态，这里用
// 200ms SetTimer 轮询应用；初始值在开窗口前 set 进去。

#include <ui_core.h>
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cmath>

#include "app_uix.embed.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

// ------------------------------------------------------------------ 常量

#define WM_TRAYICON      (WM_APP + 1)      // 托盘图标回调消息
#define IDM_SHOW         2001              // 托盘菜单：显示/隐藏
#define IDM_AUTOSTART    2002              // 托盘菜单：开机自启
#define IDM_EXIT         2003              // 托盘菜单：退出

static const wchar_t* K_MUTEX_NAME   = L"Local\\xLighter.SingleInstance";
static const wchar_t* K_MAIN_TITLE   = L"xLighter";
static const wchar_t* K_RUN_KEY      = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* K_RUN_VALUE    = L"xLighter";
static const wchar_t* K_APP_NAME     = L"xLighter";

// ------------------------------------------------------------------ 全局状态

struct Config {
    int brightness;   // 15~100
    int mode;         // 0标准 1防蓝光 2护眼 3游戏
    int autoStart;    // 0/1
};
static Config g_cfg = { 80, 0, 0 };

static UiPage   g_page   = 0;
static UiWindow g_win    = 0;
static HWND     g_mainHwnd  = nullptr;   // 主窗口（core-ui 创建的）
static HWND     g_trayHwnd  = nullptr;   // 托盘消息窗口（隐藏的顶层窗口）
static NOTIFYICONDATAW g_nid = {};
static UINT     g_taskbarCreatedMsg = 0; // "TaskbarCreated"，explorer 重启后补图标

// 轮询去重：只在实际变化时应用（写配置 / 调 API）
static int  g_appliedBrightness = -1;
static int  g_appliedMode       = -1;
static int  g_appliedAutoStart  = -1;

// 原始 gamma（退出时恢复，避免程序异常退出后屏幕一直带着滤镜）
static WORD g_rampOrig[3][256];
static BOOL g_rampOrigSaved = FALSE;

// ------------------------------------------------------------------ 工具

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// 配置文件：%APPDATA%\xLighter\config.ini，纯 文本 key=value
static void ConfigPath(wchar_t* out, size_t cap) {
    wchar_t appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) ||
        !SUCCEEDED(StringCchPrintfW(out, cap, L"%s\\xLighter", appdata))) {
        // 退回 exe 旁边
        GetModuleFileNameW(nullptr, out, (DWORD)cap);
        PathRemoveFileSpecW(out);
        return;
    }
    CreateDirectoryW(out, nullptr);
    StringCchCatW(out, cap, L"\\config.ini");
}

static void ConfigLoad(void) {
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rt, ccs=UTF-8") != 0 || !f) return;
    wchar_t line[256];
    while (fgetws(line, 256, f)) {
        int v = 0;
        if (swscanf_s(line, L"brightness=%d", &v) == 1)
            g_cfg.brightness = ClampInt(v, 15, 100);
        else if (swscanf_s(line, L"mode=%d", &v) == 1)
            g_cfg.mode = ClampInt(v, 0, 3);
        else if (swscanf_s(line, L"autoStart=%d", &v) == 1)
            g_cfg.autoStart = v ? 1 : 0;
    }
    fclose(f);
}

static void ConfigSave(void) {
    wchar_t path[MAX_PATH];
    ConfigPath(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wt, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"brightness=%d\nmode=%d\nautoStart=%d\n",
             g_cfg.brightness, g_cfg.mode, g_cfg.autoStart);
    fclose(f);
}

// ------------------------------------------------------------------ 开机自启（注册表）

static void AutostartApply(int enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, K_RUN_KEY, 0, KEY_SET_VALUE,
                      &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // 路径加引号（防带空格），/silent = 静默启动（开隐藏窗口只留托盘）
        wchar_t cmd[MAX_PATH + 16];
        StringCchPrintfW(cmd, MAX_PATH + 16, L"\"%s\" /silent", exe);
        RegSetValueExW(key, K_RUN_VALUE, 0, REG_SZ,
                       (const BYTE*)cmd,
                       (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, K_RUN_VALUE);  // 不存在也无妨
    }
    RegCloseKey(key);
}

static int AutostartQuery(void) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, K_RUN_KEY, 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(key, K_RUN_VALUE, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) ? 1 : 0;
}

// ------------------------------------------------------------------ 亮度引擎（gamma 软调）

// 按 亮度 + 模式 生成三条 256 级 gamma 曲线。
// 亮度：线性缩放，地板 15%（避免拖到 0 变全黑找不回来）
// 模式：在亮度之上再做通道 tint / 对比度
static void BuildRamp(int brightness, int mode, WORD out[3][256]) {
    const double scale = 0.15 + 0.85 * (brightness / 100.0);
    for (int i = 0; i < 256; ++i) {
        double v = (i / 255.0) * scale;   // 亮度
        double r = v, g = v, b = v;

        switch (mode) {
            case 1:                        // 防蓝光：压蓝绿，留暖色
                r *= 1.00; g *= 0.86; b *= 0.68;
                break;
            case 2:                        // 护眼：更暖 + 整体再降一档
                r *= 0.98; g *= 0.90; b *= 0.76;
                r *= 0.92; g *= 0.92; b *= 0.92;
                break;
            case 3:                        // 游戏：拉对比度 + 微提亮
                v = (v - 0.5) * 1.14 + 0.5;
                r = v * 1.03; g = v * 1.01; b = v * 1.00;
                break;
            default:                       // 标准：原样
                break;
        }

        out[0][i] = (WORD)(ClampInt((int)(r * 255.0 + 0.5), 0, 255) * 257);
        out[1][i] = (WORD)(ClampInt((int)(g * 255.0 + 0.5), 0, 255) * 257);
        out[2][i] = (WORD)(ClampInt((int)(b * 255.0 + 0.5), 0, 255) * 257);
    }
}

static void ApplyRampToAll(WORD ramp[3][256]) {
    // SetDeviceGammaRamp 作用于 DC；主屏 DC(NULL) 覆盖主显示器。
    // 多屏：逐屏建 DC 再写。这里先覆盖主屏，再枚举其余屏。
    HDC hdc = GetDC(nullptr);
    if (hdc) {
        SetDeviceGammaRamp(hdc, (LPWORD)ramp);
        ReleaseDC(nullptr, hdc);
    }
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hmon, &mi)) return TRUE;
            if (mi.szDevice[0] == L'\0') return TRUE;
            HDC mdc = CreateDCW(mi.szDevice, nullptr, nullptr, nullptr);
            if (mdc) {
                SetDeviceGammaRamp(mdc, (LPWORD)param);
                DeleteDC(mdc);
            }
            return TRUE;
        }, (LPARAM)(LPWORD)ramp);
}

static void ApplyBrightnessNow(void) {
    WORD ramp[3][256];
    BuildRamp(g_cfg.brightness, g_cfg.mode, ramp);
    ApplyRampToAll(ramp);
}

static void SaveOriginalRamp(void) {
    HDC hdc = GetDC(nullptr);
    if (hdc) {
        g_rampOrigSaved = GetDeviceGammaRamp(hdc, (LPWORD)g_rampOrig) != FALSE;
        ReleaseDC(nullptr, hdc);
    }
}

static void RestoreOriginalRamp(void) {
    if (!g_rampOrigSaved) return;
    HDC hdc = GetDC(nullptr);
    if (hdc) {
        SetDeviceGammaRamp(hdc, (LPWORD)g_rampOrig);
        ReleaseDC(nullptr, hdc);
    }
}

// ------------------------------------------------------------------ 托盘

// 用 GDI 现画一个「太阳」托盘图标：黄色圆盘 + 橙色描边 + 高光，
// 免去带 ico 资源。窗口图标复用同一 HICON。
static HICON MakeSunIcon(int size) {
    BITMAPV5HEADER bi = {};
    bi.bV5Size           = sizeof(bi);
    bi.bV5Width          = size;
    bi.bV5Height         = size;
    bi.bV5Planes         = 1;
    bi.bV5BitCount       = 32;
    bi.bV5Compression    = BI_BITFIELDS;
    bi.bV5RedMask        = 0x00FF0000;
    bi.bV5GreenMask      = 0x0000FF00;
    bi.bV5BlueMask       = 0x000000FF;
    bi.bV5AlphaMask      = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) return nullptr;

    const float c = size * 0.5f, R = size * 0.36f;
    const float rIn = size * 0.30f, rOut = size * 0.42f;
    DWORD* px = (DWORD*)bits;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float d = sqrtf(dx * dx + dy * dy);
            DWORD argb;
            if (d <= rIn) {
                // 盘体：黄到亮黄的径向渐变 + 左上高光
                float t = d / rIn;
                int rr = (int)(255 - 20 * t);
                int gg = (int)(200 - 24 * t);
                int bb = (int)(60 - 20 * t);
                float hl = 1.0f - (dx + dy) / (2.0f * rIn) * 0.35f;
                rr = (int)(rr * hl + 40 * (1 - hl));
                gg = (int)(gg * hl + 40 * (1 - hl));
                argb = 0xFF000000u | ((DWORD)ClampInt(rr,0,255) << 16)
                     | ((DWORD)ClampInt(gg,0,255) << 8) | (DWORD)ClampInt(bb,0,255);
            } else if (d <= rOut && d > rIn + size * 0.03f) {
                // 描边
                argb = 0xFFD97706;   // amber-600
            } else if (d <= rOut) {
                argb = 0xFFF8C63F;
            } else {
                argb = 0x00000000;
            }
            // premultiply（CreateIconIndirect 的 32bpp 需要 premultiplied alpha）
            DWORD a = (argb >> 24) & 0xFF;
            DWORD r8 = ((argb >> 16) & 0xFF) * a / 255;
            DWORD g8 = ((argb >> 8) & 0xFF) * a / 255;
            DWORD b8 = (argb & 0xFF) * a / 255;
            px[y * size + x] = (a << 24) | (r8 << 16) | (g8 << 8) | b8;
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// 给 frameless 窗口加 Windows 11 圆角 + DWM 阴影。
// DWMWA_WINDOW_CORNER_PREFERENCE = 33（Win11 22000+），DWMWCP_ROUND = 2。
// 圆角由 DWM 裁剪并自动带投影；Win10 上该属性无效（返回失败静默忽略，
// 仍保留 WS_THICKFRAME 带来的系统阴影）。
static void ApplyWindowChrome(HWND hwnd) {
    const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    const int DWMWCP_ROUND = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &DWMWCP_ROUND, sizeof(DWMWCP_ROUND));
    // 确保 frameless 窗口仍被 DWM 视作「有边框窗口」以获得阴影：
    // 若 lib 已去掉 WS_THICKFRAME 就补回来。
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_THICKFRAME)) {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_THICKFRAME);
    }
}

// 加载应用图标：优先用 exe 里嵌入的资源 ID=1（resources/app.rc → logo.png
// 生成的多尺寸 ico，core-ui 标题栏也用同一个资源），拿不到退回代码画的太阳。
// LoadIcon 返回的是共享图标，不要 DestroyIcon。
static HICON LoadAppIcon(int size) {
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   size, size, LR_DEFAULTCOLOR);
    return icon ? icon : MakeSunIcon(size);
}

static void TrayAdd(void) {
    if (!g_trayHwnd) return;
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_trayHwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    if (!g_nid.hIcon) g_nid.hIcon = LoadAppIcon(32);
    wcsncpy_s(g_nid.szTip, L"xLighter 屏幕亮度调节助手", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void TrayRemove(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// 主窗口显示 / 隐藏（走 lib 的 API，避免绕过它的状态）
static void ShowMain(void) {
    if (g_win) {
        ui_window_show(g_win);
        if (g_mainHwnd) SetForegroundWindow(g_mainHwnd);
    }
}
static void HideMain(void) {
    if (g_win) ui_window_hide(g_win);
}

static void QuitApp(void) {
    RestoreOriginalRamp();
    TrayRemove();
    ui_quit(0);
}

// 托盘消息窗口过程
static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreatedMsg && msg == g_taskbarCreatedMsg) {
        // explorer 重启了：图标丢了，补一个（共享图标不销毁，直接换引用）
        g_nid.hIcon = LoadAppIcon(32);
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP) {
            // 左键：切换主窗口显示 / 隐藏
            if (g_mainHwnd && IsWindowVisible(g_mainHwnd)) HideMain();
            else ShowMain();
        } else if (lp == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            if (menu) {
                BOOL visible = g_mainHwnd && IsWindowVisible(g_mainHwnd);
                AppendMenuW(menu, MF_STRING, IDM_SHOW,
                            visible ? L"隐藏主窗口" : L"显示主窗口");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING | (g_cfg.autoStart ? MF_CHECKED : 0),
                            IDM_AUTOSTART, L"开机自启（静默）");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
                SetForegroundWindow(hwnd);   // TrackPopupMenu 前置（Win32 惯例）
                int cmd = TrackPopupMenu(menu,
                                         TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                         pt.x, pt.y, 0, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(menu);
                switch (cmd) {
                case IDM_SHOW:      visible ? HideMain() : ShowMain(); break;
                case IDM_AUTOSTART:
                    g_cfg.autoStart = !g_cfg.autoStart;
                    AutostartApply(g_cfg.autoStart);
                    ConfigSave();
                    if (g_page) ui_page_set_bool(g_page, "autoStart", g_cfg.autoStart);
                    break;
                case IDM_EXIT:      QuitApp(); break;
                }
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CreateTrayWindow(HINSTANCE hinst) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = TrayWndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = L"xLighterTrayWnd";
    RegisterClassW(&wc);
    // 隐藏的普通顶层窗口（HWND_MESSAGE 的 message-only 窗口收不到
    // TaskbarCreated 广播，explorer 重启后就补不了图标）
    g_trayHwnd = CreateWindowExW(0, L"xLighterTrayWnd", L"", WS_OVERLAPPED,
                                 0, 0, 0, 0, nullptr, nullptr, hinst, nullptr);
    if (g_trayHwnd) {
        g_nid.hIcon = LoadAppIcon(32);   // 用 logo.png 生成的资源图标
        TrayAdd();
    }
}

// ------------------------------------------------------------------ 单实例

// 第二个实例：唤起已在跑的那个（显示主窗口）然后自己退出。
static void RaiseExistingInstance(void) {
    HWND hwnd = FindWindowW(nullptr, K_MAIN_TITLE);
    if (hwnd) {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        else ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
}

static BOOL ClaimSingleInstance(void) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, K_MUTEX_NAME);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        RaiseExistingInstance();
        return FALSE;
    }
    // 不 CloseHandle：句柄活着 = 锁活着，进程退出系统自动收
    return TRUE;
}

// ------------------------------------------------------------------ 关闭 → 托盘

static int OnCloseRequest(UiWindow, void*) {
    HideMain();          // 拦下关闭，藏到托盘
    return 0;            // 0 = 取消默认 DestroyWindow
}

// ------------------------------------------------------------------ 轮询（.uix → C）

static VOID CALLBACK OnPoll(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_page) return;

    // 亮度
    if (char* j = ui_page_get_json(g_page, "brightness")) {
        int v = ClampInt(atoi(j), 15, 100);
        ui_page_free(j);
        if (v != g_cfg.brightness) {
            g_cfg.brightness = v;
            ConfigSave();
        }
    }
    // 模式（.uix 里叫 modeIdx，避开可能的内部状态名冲突）
    if (char* j = ui_page_get_json(g_page, "modeIdx")) {
        int v = ClampInt(atoi(j), 0, 3);
        ui_page_free(j);
        if (v != g_cfg.mode) {
            g_cfg.mode = v;
            ConfigSave();
        }
    }
    // 开机自启（设置页 toggle / 托盘菜单都会写）
    if (char* j = ui_page_get_json(g_page, "autoStart")) {
        int v = strstr(j, "true") ? 1 : 0;
        ui_page_free(j);
        if (v != g_cfg.autoStart) {
            g_cfg.autoStart = v;
            AutostartApply(v);
            ConfigSave();
        }
    }

    // 实际变了才重写 gamma（避免空转重复写显存）
    if (g_cfg.brightness != g_appliedBrightness ||
        g_cfg.mode != g_appliedMode) {
        ApplyBrightnessNow();
        g_appliedBrightness = g_cfg.brightness;
        g_appliedMode = g_cfg.mode;
    }
    // 托盘菜单改了自启后，把 toggle 状态回写 UI（在托盘菜单分支里 set 了）
}

// ------------------------------------------------------------------ WinMain

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE, LPSTR cmdLine, int) {
    // 单实例：第二个实例唤起第一个就退出
    if (!ClaimSingleInstance()) return 0;

    const BOOL silent = cmdLine && strstr(cmdLine, "silent") != nullptr;

    ConfigLoad();
    // 配置里的自启标记只作 UI 初值；注册表才是事实。两边不一致以注册表为准。
    g_cfg.autoStart = AutostartQuery();

    SaveOriginalRamp();

    ui_init_with_theme(UI_THEME_LIGHT);

    UiPage page = ui_page_load_string(k_app_uix);
    if (!page) { ui_shutdown(); return 1; }
    g_page = page;

    // 初始 reactive 状态（开窗口前 set，首帧就是对的）
    ui_page_set_int (page, "brightness", g_cfg.brightness);
    ui_page_set_int (page, "mode",       g_cfg.mode);
    ui_page_set_bool(page, "autoStart",  g_cfg.autoStart ? 1 : 0);

    // 静默启动：prepare 开隐藏窗口；正常启动：直接开
    UiWindow win = silent ? ui_page_prepare_window(page, nullptr)
                          : ui_page_open_window(page, nullptr);
    if (!win) { ui_page_destroy(page); ui_shutdown(); return 2; }
    g_win = win;

    // 托盘（无论是否静默都要有，它是静默态下唯一的入口）
    CreateTrayWindow(hinst);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // 关闭按钮 → 藏到托盘（不真关）
    ui_window_on_close_request(win, OnCloseRequest, nullptr);

    g_mainHwnd = (HWND)ui_window_hwnd(win);
    // 统一 Win32 窗口标题（单实例 FindWindow 靠它找）
    if (g_mainHwnd) SetWindowTextW(g_mainHwnd, K_MAIN_TITLE);

    // 圆角 + 阴影（Windows 11 DWM）：frameless 窗口默认直角、无阴影，
    // 显式把角设成 ROUND，DWM 会顺带补上系统级投影。
    if (g_mainHwnd) ApplyWindowChrome(g_mainHwnd);

    // 开窗口后立刻应用一次亮度（静默启动也要生效）
    ApplyBrightnessNow();
    g_appliedBrightness = g_cfg.brightness;
    g_appliedMode = g_cfg.mode;

    // 40ms 轮询：滑块拖动 / 点模式按钮要「跟手」，200ms 能感觉到迟滞；
    // 只有配置真的变了才写 gamma / 注册表 / 配置文件，空转成本极低。
    if (g_mainHwnd) SetTimer(g_mainHwnd, 1, 40, OnPoll);

    int exitCode = ui_run();

    if (g_mainHwnd) KillTimer(g_mainHwnd, 1);
    // g_nid.hIcon 是 LoadIcon 的共享图标（或代码画的），进程退出系统统一回收
    TrayRemove();
    RestoreOriginalRamp();
    ui_page_destroy(page);
    ui_shutdown();
    return exitCode;
}
