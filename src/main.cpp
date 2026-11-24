// PreventSleepMode —— 防休眠助手
//
// 功能：
//   1. 周期性模拟轻微鼠标移动（默认每 2 分钟，间隔可调），防止系统因长时间
//      无操作而自动休眠/熄屏
//   2. 可设置运行时长，到时自动停止
//   3. 全局暂停/继续快捷键（默认 Ctrl+Alt+P，可自定义）
//
// 纯 Win32 API 实现，无第三方依赖。

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <cwchar>
#include <cstdlib>

// ---------------- 常量 ----------------

static const wchar_t APP_CLASS[] = L"PreventSleepModeWnd";
static const wchar_t APP_TITLE[] = L"防休眠助手 PreventSleepMode";
static const wchar_t APP_MUTEX[] = L"Local\\PreventSleepMode.SingleInstance";

enum ControlId {
    IDC_GRP_STATUS    = 1000,   // 分组框：运行状态
    IDC_STATUS,                 // 状态文本：已停止/运行中/已暂停
    IDC_COUNTDOWN,              // 倒计时文本
    IDC_GRP_SETTINGS,           // 分组框：设置
    IDC_LBL_INTERVAL,
    IDC_ED_INTERVAL,            // 移动间隔（秒）
    IDC_LBL_STOP,
    IDC_ED_STOP,                // 自动停止时长（分钟）
    IDC_NOTE_INTERVAL,          // 灰色说明：间隔范围
    IDC_NOTE_STOP,              // 灰色说明：0 表示不自动停止
    IDC_LBL_HOTKEY,
    IDC_HOTKEY,                 // 热键控件
    IDC_BTN_APPLYHK,            // 应用快捷键
    IDC_BTN_START,
    IDC_BTN_STOP,
    IDC_BTN_PAUSE,
    IDC_HINT,
};

static const int IDT_TICK   = 1;    // 1 秒心跳定时器
static const int IDH_TOGGLE = 1;    // 全局热键 ID

// 取值范围
static const int kIntervalMin = 5;      // 移动间隔下限（秒）
static const int kIntervalMax = 7200;   // 移动间隔上限（秒）
static const int kStopMinMin  = 0;      // 自动停時长下限（分钟），0 = 不限制
static const int kStopMinMax  = 10080;  // 自动停時长上限（分钟），一周

// ---------------- 状态 ----------------

static HINSTANCE g_hInst      = nullptr;
static HWND      g_wnd        = nullptr;
static HWND      g_edInterval = nullptr;
static HWND      g_edStop     = nullptr;
static HWND      g_hkCtrl     = nullptr;
static HWND      g_btnPause   = nullptr;
static HWND      g_status     = nullptr;
static HWND      g_countdown  = nullptr;
static HWND      g_hint       = nullptr;
static HFONT     g_font       = nullptr;

static UINT g_dpi = 96;

static bool g_running   = false;
static bool g_paused    = false;
static int  g_intervalSec = 120;    // 移动间隔（秒）
static int  g_moveRemain  = 0;      // 距下次移动（秒）
static int  g_stopRemain  = -1;     // 距自动停止（秒）；<0 表示未启用定时关闭
static int  g_jiggleToggle = 0;     // 鼠标轻移方向交替

static UINT g_hkMods        = MOD_CONTROL | MOD_ALT;
static UINT g_hkVk          = 'P';
static bool g_hkRegistered  = false;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

// ---------------- 布局表（96 DPI 基准坐标，客户区 460 x 324） ----------------

struct CtrlRect { int id, x, y, w, h; };

static const CtrlRect kLayout[] = {
    {IDC_GRP_STATUS,    14,  12, 432,  72},
    {IDC_STATUS,        30,  34, 400,  22},
    {IDC_COUNTDOWN,     30,  58, 400,  20},
    {IDC_GRP_SETTINGS,  14,  92, 432, 128},
    {IDC_LBL_INTERVAL,  30, 116, 104,  20},
    {IDC_ED_INTERVAL,  138, 113,  80,  24},
    {IDC_NOTE_INTERVAL,228, 117, 210,  20},
    {IDC_LBL_STOP,      30, 148, 132,  20},
    {IDC_ED_STOP,      164, 145,  80,  24},
    {IDC_NOTE_STOP,    254, 149, 190,  20},
    {IDC_LBL_HOTKEY,    30, 182,  96,  20},
    {IDC_HOTKEY,       130, 179, 140,  24},
    {IDC_BTN_APPLYHK,  280, 178,  70,  26},
    {IDC_BTN_START,     14, 234, 138,  34},
    {IDC_BTN_STOP,     161, 234, 138,  34},
    {IDC_BTN_PAUSE,    308, 234, 138,  34},
    {IDC_HINT,          14, 276, 432,  40},
};

// ---------------- 工具函数 ----------------

static void SetStatusText(const wchar_t* s) { SetWindowTextW(g_status, s); }

static void UpdateCountdown()
{
    wchar_t buf[160];
    if (!g_running) {
        lstrcpyW(buf, L"距下次移动：--:--");
    } else if (g_paused) {
        lstrcpyW(buf, L"已暂停，按快捷键或“继续”按钮恢复");
    } else {
        swprintf(buf, 160, L"距下次移动：%02d:%02d", g_moveRemain / 60, g_moveRemain % 60);
        if (g_stopRemain >= 0) {
            wchar_t b2[80];
            swprintf(b2, 80, L"    距自动停止：%02d:%02d:%02d",
                     g_stopRemain / 3600, g_stopRemain / 60 % 60, g_stopRemain % 60);
            lstrcatW(buf, b2);
        }
    }
    SetWindowTextW(g_countdown, buf);
}

// 模拟一次轻微鼠标移动：相对移动 1 像素，方向交替，肉眼几乎不可见且不影响使用
static void JiggleMouse()
{
    INPUT inp{};
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    inp.mi.dx = (g_jiggleToggle ^= 1) ? 1 : -1;
    SendInput(1, &inp, sizeof(inp));
}

static int ReadIntFromEdit(HWND ed, int minV, int maxV)
{
    wchar_t buf[32] = L"";
    GetWindowTextW(ed, buf, 32);
    wchar_t* end = nullptr;
    long v = wcstol(buf, &end, 10);
    if (end == buf) v = 0;              // 输入为空或非法
    if (v < minV) v = minV;
    if (v > maxV) v = maxV;
    return (int)v;
}

static void WriteIntToEdit(HWND ed, int v)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    SetWindowTextW(ed, buf);
}

// ---------------- 运行控制 ----------------

static void StartRunning()
{
    g_intervalSec = ReadIntFromEdit(g_edInterval, kIntervalMin, kIntervalMax);
    int stopMinutes = ReadIntFromEdit(g_edStop, kStopMinMin, kStopMinMax);
    WriteIntToEdit(g_edInterval, g_intervalSec);    // 回写规范化后的值
    WriteIntToEdit(g_edStop, stopMinutes);

    g_stopRemain = stopMinutes > 0 ? stopMinutes * 60 : -1;
    g_moveRemain = g_intervalSec;
    g_running = true;
    g_paused = false;

    // 双保险：模拟输入本身会重置系统空闲计时；此处再显式阻止系统休眠与熄屏
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    SetTimer(g_wnd, IDT_TICK, 1000, nullptr);

    SetStatusText(L"运行中");
    SetWindowTextW(g_btnPause, L"暂停");
    UpdateCountdown();
}

static void StopRunning(bool timedOut)
{
    KillTimer(g_wnd, IDT_TICK);
    SetThreadExecutionState(ES_CONTINUOUS);     // 清除阻止标志，恢复系统正常休眠
    g_running = false;
    g_paused = false;
    g_stopRemain = -1;
    SetStatusText(timedOut ? L"已定时停止" : L"已停止");
    SetWindowTextW(g_btnPause, L"暂停");
    UpdateCountdown();
}

static void TogglePause()
{
    if (!g_running) return;
    g_paused = !g_paused;
    // 暂停时清除阻止标志，允许系统正常休眠；继续时重新启用
    if (g_paused) SetThreadExecutionState(ES_CONTINUOUS);
    else          SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    SetStatusText(g_paused ? L"已暂停" : L"运行中");
    SetWindowTextW(g_btnPause, g_paused ? L"继续" : L"暂停");
    UpdateCountdown();
}

// ---------------- 全局热键 ----------------

static bool RegisterCurrentHotkey()
{
    if (g_hkRegistered) {
        UnregisterHotKey(g_wnd, IDH_TOGGLE);
        g_hkRegistered = false;
    }
    if (g_hkVk == 0) return false;
    g_hkRegistered = RegisterHotKey(g_wnd, IDH_TOGGLE, g_hkMods | MOD_NOREPEAT, g_hkVk) != 0;
    return g_hkRegistered;
}

static void ApplyHotkeyFromControl()
{
    DWORD hk = (DWORD)SendMessageW(g_hkCtrl, HKM_GETHOTKEY, 0, 0);
    UINT vk = LOBYTE(LOWORD(hk));
    UINT fl = HIBYTE(LOWORD(hk));
    if (vk == 0) {
        MessageBoxW(g_wnd, L"请先在框内点击并按下想要的快捷键组合（需包含 Ctrl/Alt/Shift/Win 至少一个修饰键）。",
                    APP_TITLE, MB_ICONINFORMATION);
        return;
    }
    UINT mods = 0;
    if (fl & HOTKEYF_SHIFT)   mods |= MOD_SHIFT;
    if (fl & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (fl & HOTKEYF_ALT)     mods |= MOD_ALT;

    g_hkVk = vk;
    g_hkMods = mods;
    if (!RegisterCurrentHotkey())
        MessageBoxW(g_wnd, L"该快捷键已被其他程序占用，注册失败，请换一个组合。",
                    APP_TITLE, MB_ICONWARNING);
}

// ---------------- 字体与布局（DPI 自适应） ----------------

static void MakeFont()
{
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    using SPIForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static SPIForDpiFn spiForDpi =
        (SPIForDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi");
    BOOL ok = FALSE;
    if (spiForDpi) ok = spiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    if (!ok) {
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    }
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
}

static void ApplyLayout()
{
    for (const CtrlRect& r : kLayout) {
        HWND h = GetDlgItem(g_wnd, r.id);
        if (h) SetWindowPos(h, nullptr, S(r.x), S(r.y), S(r.w), S(r.h),
                            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EnumChildWindows(g_wnd, [](HWND h, LPARAM) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
        return TRUE;
    }, 0);
    InvalidateRect(g_wnd, nullptr, TRUE);
}

// ---------------- 界面创建 ----------------

static HWND CreateCtrl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0)
{
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 0, 0, g_wnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

static void CreateControls()
{
    CreateCtrl(L"BUTTON", L"运行状态", BS_GROUPBOX, IDC_GRP_STATUS);
    g_status = CreateCtrl(L"STATIC", L"已停止", 0, IDC_STATUS);
    g_countdown = CreateCtrl(L"STATIC", L"距下次移动：--:--", 0, IDC_COUNTDOWN);

    CreateCtrl(L"BUTTON", L"设置", BS_GROUPBOX, IDC_GRP_SETTINGS);
    CreateCtrl(L"STATIC", L"移动间隔（秒）：", 0, IDC_LBL_INTERVAL);
    g_edInterval = CreateCtrl(L"EDIT", L"120", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, IDC_ED_INTERVAL, WS_EX_CLIENTEDGE);
    CreateCtrl(L"STATIC", L"范围 5 ~ 7200 秒", 0, IDC_NOTE_INTERVAL);
    CreateCtrl(L"STATIC", L"自动停止（分钟）：", 0, IDC_LBL_STOP);
    g_edStop = CreateCtrl(L"EDIT", L"0", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, IDC_ED_STOP, WS_EX_CLIENTEDGE);
    CreateCtrl(L"STATIC", L"0 表示不自动停止", 0, IDC_NOTE_STOP);
    CreateCtrl(L"STATIC", L"暂停快捷键：", 0, IDC_LBL_HOTKEY);
    g_hkCtrl = CreateCtrl(HOTKEY_CLASSW, nullptr, 0, IDC_HOTKEY);
    CreateCtrl(L"BUTTON", L"应用", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_APPLYHK);

    CreateCtrl(L"BUTTON", L"启动", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_BTN_START);
    CreateCtrl(L"BUTTON", L"停止", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_STOP);
    g_btnPause = CreateCtrl(L"BUTTON", L"暂停", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_PAUSE);

    g_hint = CreateCtrl(L"STATIC",
        L"提示：启动后每隔设定间隔轻移鼠标 1 像素，并阻止系统休眠与熄屏。\n"
        L"修改设置后重新点“启动”生效；暂停期间允许系统正常休眠。",
        0, IDC_HINT);

    // 默认快捷键 Ctrl+Alt+P
    SendMessageW(g_hkCtrl, HKM_SETHOTKEY, MAKEWORD('P', HOTKEYF_CONTROL | HOTKEYF_ALT), 0);
}

// ---------------- 窗口过程 ----------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        CreateControls();
        ApplyLayout();
        if (!RegisterCurrentHotkey()) {
            SetWindowTextW(g_hint,
                L"注意：默认快捷键 Ctrl+Alt+P 注册失败（可能被占用），请修改后点击“应用”。");
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_START:   StartRunning();           break;
        case IDC_BTN_STOP:    StopRunning(false);       break;
        case IDC_BTN_PAUSE:   TogglePause();            break;
        case IDC_BTN_APPLYHK: ApplyHotkeyFromControl(); break;
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        // 辅助说明文字用灰色，其余静态控件走默认
        int id = GetDlgCtrlID((HWND)lParam);
        if (id == IDC_HINT || id == IDC_NOTE_INTERVAL || id == IDC_NOTE_STOP) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(106, 106, 106));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }

    case WM_TIMER:
        if (wParam == IDT_TICK && g_running && !g_paused) {
            if (--g_moveRemain <= 0) {
                JiggleMouse();
                g_moveRemain = g_intervalSec;
            }
            if (g_stopRemain > 0) {
                if (--g_stopRemain <= 0) { StopRunning(true); return 0; }
            }
            UpdateCountdown();
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == IDH_TOGGLE) TogglePause();
        return 0;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        MakeFont();
        ApplyLayout();
        RECT* r = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_hkRegistered) UnregisterHotKey(g_wnd, IDH_TOGGLE);
        KillTimer(g_wnd, IDT_TICK);
        SetThreadExecutionState(ES_CONTINUOUS);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 入口 ----------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    // 单实例：重复启动时把已有窗口带到前台
    HANDLE mutex = CreateMutexW(nullptr, TRUE, APP_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(APP_CLASS, nullptr);
        if (prev) {
            ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        return 0;
    }

    g_hInst = hInst;

    // 初始 DPI 取主屏
    HDC dc = GetDC(nullptr);
    if (dc) {
        g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    MakeFont();

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{ 0, 0, S(460), S(324) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    int wndW = rc.right - rc.left;
    int wndH = rc.bottom - rc.top;

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - wndW) / 2;
    int y = wa.top  + ((wa.bottom - wa.top) - wndH) / 2;

    CreateWindowExW(0, APP_CLASS, APP_TITLE, style, x, y, wndW, wndH,
                    nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_wnd, nCmdShow);
    UpdateWindow(g_wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
