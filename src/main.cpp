// PreventSleepMode —— 防休眠助手
//
// 功能：
//   1. 周期性模拟轻微鼠标移动（默认每 2 分钟，间隔可调），防止系统因长时间
//      无操作而自动休眠/熄屏
//   2. 可设置运行时长，到时自动停止
//   3. 全局暂停/继续快捷键（默认 Ctrl+Alt+P，可自定义）
//   4. 久坐提醒：运行期间每隔 N 分钟（默认 60，可调）在鼠标所在屏幕的底部
//      弹出 OSD 提示（文字可自定义），显示 3 秒后淡出
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
    IDC_SIT_COUNTDOWN,          // 久坐提醒倒计时文本
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
    IDC_LBL_SIT,
    IDC_ED_SIT,                 // 久坐提醒间隔（分钟）
    IDC_NOTE_SIT,               // 灰色说明：0 表示关闭提醒
    IDC_LBL_SITTEXT,
    IDC_ED_SITTEXT,             // 久坐提醒文字
    IDC_BTN_SITPREVIEW,         // 预览 OSD 提示
    IDC_BTN_START,
    IDC_BTN_STOP,
    IDC_BTN_PAUSE,
    IDC_HINT,
};

static const int IDT_TICK     = 1;    // 1 秒心跳定时器
static const int IDT_OSD_ANIM = 2;    // OSD 淡入/淡出动画定时器
static const int IDH_TOGGLE   = 1;    // 全局热键 ID

// 取值范围
static const int kIntervalMin = 5;      // 移动间隔下限（秒）
static const int kIntervalMax = 7200;   // 移动间隔上限（秒）
static const int kStopMinMin  = 0;      // 自动停時长下限（分钟），0 = 不限制
static const int kStopMinMax  = 10080;  // 自动停時长上限（分钟），一周
static const int kSitMinMin  = 0;      // 久坐提醒间隔下限（分钟），0 = 关闭提醒
static const int kSitMinMax  = 1440;   // 久坐提醒间隔上限（分钟）

static const wchar_t kDefaultSitText[] = L"起来走走、喝口水";
static const int kSitTextMax = 60;      // 久坐提醒文字最大长度（字符）

// ---------------- 状态 ----------------

static HINSTANCE g_hInst      = nullptr;
static HWND      g_wnd        = nullptr;
static HWND      g_edInterval = nullptr;
static HWND      g_edStop     = nullptr;
static HWND      g_edSit      = nullptr;
static HWND      g_edSitText  = nullptr;
static HWND      g_hkCtrl     = nullptr;
static HWND      g_btnPause   = nullptr;
static HWND      g_status     = nullptr;
static HWND      g_countdown  = nullptr;
static HWND      g_sitCountdown = nullptr;
static HWND      g_hint       = nullptr;
static HFONT     g_font       = nullptr;
static wchar_t   g_fontFace[LF_FACESIZE] = L"";   // 系统界面字体，OSD 沿用

static UINT g_dpi = 96;

static bool g_running   = false;
static bool g_paused    = false;
static int  g_intervalSec = 120;    // 移动间隔（秒）
static int  g_moveRemain  = 0;      // 距下次移动（秒）
static int  g_stopRemain  = -1;     // 距自动停止（秒）；<0 表示未启用定时关闭
static int  g_jiggleToggle = 0;     // 鼠标轻移方向交替
static int  g_sitIntervalSec = 0;   // 久坐提醒周期（秒）；0 = 关闭
static int  g_sitRemain      = 0;   // 距下次久坐提醒（秒）
static wchar_t g_sitText[kSitTextMax + 1] = L"";   // 启动时读取的提醒文字

static UINT g_hkMods        = MOD_CONTROL | MOD_ALT;
static UINT g_hkVk          = 'P';
static bool g_hkRegistered  = false;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

// ---------------- 布局表（96 DPI 基准坐标，客户区 460 x 430） ----------------

struct CtrlRect { int id, x, y, w, h; };

static const CtrlRect kLayout[] = {
    {IDC_GRP_STATUS,      14,  12, 432,  96},
    {IDC_STATUS,          30,  34, 400,  22},
    {IDC_COUNTDOWN,       30,  58, 400,  20},
    {IDC_SIT_COUNTDOWN,   30,  82, 400,  20},
    {IDC_GRP_SETTINGS,    14, 116, 432, 196},
    {IDC_LBL_INTERVAL,    30, 140, 104,  20},
    {IDC_ED_INTERVAL,    138, 137,  80,  24},
    {IDC_NOTE_INTERVAL,  228, 141, 210,  20},
    {IDC_LBL_STOP,        30, 172, 132,  20},
    {IDC_ED_STOP,        164, 169,  80,  24},
    {IDC_NOTE_STOP,      254, 173, 190,  20},
    {IDC_LBL_HOTKEY,      30, 206,  96,  20},
    {IDC_HOTKEY,         130, 203, 140,  24},
    {IDC_BTN_APPLYHK,    280, 202,  70,  26},
    {IDC_LBL_SIT,         30, 240, 132,  20},
    {IDC_ED_SIT,         164, 237,  80,  24},
    {IDC_NOTE_SIT,       254, 241, 190,  20},
    {IDC_LBL_SITTEXT,     30, 272, 132,  20},
    {IDC_ED_SITTEXT,     164, 269, 190,  24},
    {IDC_BTN_SITPREVIEW, 362, 268,  70,  26},
    {IDC_BTN_START,       14, 326, 138,  34},
    {IDC_BTN_STOP,       161, 326, 138,  34},
    {IDC_BTN_PAUSE,      308, 326, 138,  34},
    {IDC_HINT,            14, 368, 432,  54},
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

    if (!g_running)                 lstrcpyW(buf, L"距久坐提醒：--:--");
    else if (g_sitIntervalSec <= 0) lstrcpyW(buf, L"距久坐提醒：已关闭");
    else swprintf(buf, 160, L"距久坐提醒：%02d:%02d", g_sitRemain / 60, g_sitRemain % 60);
    SetWindowTextW(g_sitCountdown, buf);
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

    int sitMinutes = ReadIntFromEdit(g_edSit, kSitMinMin, kSitMinMax);
    WriteIntToEdit(g_edSit, sitMinutes);
    g_sitIntervalSec = sitMinutes * 60;
    g_sitRemain = g_sitIntervalSec;
    GetWindowTextW(g_edSitText, g_sitText, kSitTextMax + 1);
    if (!g_sitText[0]) lstrcpyW(g_sitText, kDefaultSitText);

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
    g_sitIntervalSec = 0;
    g_sitRemain = 0;
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

// ---------------- OSD 久坐提醒弹窗 ----------------
//
// 逐像素 alpha 的分层窗口。位图以 k 倍过采样用 GDI 绘制“彩色图”（形状外为黑）
// 与“黑白遮罩”，再盒式降采样合成预乘 BGRA，得到平滑的圆角与文字边缘；
// 淡入/淡出只改 UpdateLayeredWindow 的整体透明度，无需重绘。

static const wchar_t OSD_CLASS[] = L"PreventSleepModeOsd";
static const wchar_t OSD_TITLE[] = L"久坐提醒";

static const DWORD kOsdFadeInMs   = 180;
static const DWORD kOsdHoldMs     = 3000;
static const DWORD kOsdFadeOutMs  = 500;
static const UINT  kOsdAnimStepMs = 15;
static const LONGLONG kOsdMaxSamples = 8000000;   // 过采样位图像素数上限

static const COLORREF kOsdFill = RGB(255, 152, 0);   // 琥珀色底，在明/暗桌面背景上都醒目
static const COLORREF kOsdInk  = RGB(32, 24, 8);     // 深色描边与文字

static HWND      g_osdWnd   = nullptr;
static HDC       g_osdDc    = nullptr;   // 选入了 g_osdBmp 的内存 DC
static HBITMAP   g_osdBmp   = nullptr;
static int       g_osdW = 0, g_osdH = 0;
static ULONGLONG g_osdT0    = 0;         // 本轮动画起点（GetTickCount64）
static BYTE      g_osdAlpha = 255;       // 当前整体透明度

static int Px(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }

static void OsdFreeBitmap()
{
    if (g_osdDc)  { DeleteDC(g_osdDc); g_osdDc = nullptr; }
    if (g_osdBmp) { DeleteObject(g_osdBmp); g_osdBmp = nullptr; }
    g_osdW = g_osdH = 0;
}

static HBITMAP CreateDib32(HDC dc, int w, int h, void** bits)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;             // 负值：自上而下存储
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static HFONT CreateOsdFont(int px)
{
    LOGFONTW lf{};
    lf.lfHeight         = -px;
    lf.lfWeight         = FW_BOLD;
    lf.lfCharSet        = DEFAULT_CHARSET;
    lf.lfQuality        = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpyW(lf.lfFaceName, g_fontFace);
    return CreateFontIndirectW(&lf);
}

// 按 k 倍过采样排版并绘制 OSD，结果存入 g_osdBmp/g_osdDc；位图过大时返回 false
static bool OsdBuild(HDC dc, const wchar_t* text, UINT dpi, int maxWidth, int k)
{
    const int border = Px(3, dpi) * k;
    const int radius = Px(18, dpi) * k;
    const int padX   = Px(36, dpi) * k;
    const int padTop = Px(16, dpi) * k;
    const int padBot = Px(20, dpi) * k;

    HFONT fTitle = CreateOsdFont(Px(15, dpi) * k);
    HFONT fBody  = CreateOsdFont(Px(32, dpi) * k);
    HGDIOBJ oldFont = SelectObject(dc, fTitle);
    HBITMAP colorBmp = nullptr, maskBmp = nullptr;
    HDC drawDc = nullptr;
    bool ok = false;

    // 排版（以下坐标均为 k 倍像素）
    RECT rcTitle{}, rcBody{};
    DrawTextW(dc, OSD_TITLE, -1, &rcTitle, DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, fBody);
    rcBody.right = maxWidth * k - padX * 2;
    DrawTextW(dc, text, -1, &rcBody, DT_CALCRECT | DT_NOPREFIX | DT_WORDBREAK);

    int w = rcTitle.right > rcBody.right ? rcTitle.right : rcBody.right;
    w += padX * 2;
    if (w < Px(280, dpi) * k) w = Px(280, dpi) * k;
    if (w > maxWidth * k) w = maxWidth * k;
    int h = padTop + rcTitle.bottom + rcBody.bottom + padBot;
    w = (w + k - 1) / k * k;      // 取整到 k 的倍数，便于降采样
    h = (h + k - 1) / k * k;

    if ((LONGLONG)w * h <= kOsdMaxSamples) {
        void* colorBits = nullptr;
        void* maskBits  = nullptr;
        void* dstBits   = nullptr;
        colorBmp = CreateDib32(dc, w, h, &colorBits);
        maskBmp  = CreateDib32(dc, w, h, &maskBits);
        g_osdBmp = colorBmp && maskBmp ? CreateDib32(dc, w / k, h / k, &dstBits) : nullptr;

        if (g_osdBmp) {
            drawDc = CreateCompatibleDC(dc);
            HGDIOBJ oldBmp = SelectObject(drawDc, colorBmp);
            HBRUSH hFill = CreateSolidBrush(kOsdFill);
            HBRUSH hInk  = CreateSolidBrush(kOsdInk);
            HGDIOBJ oldBrush = SelectObject(drawDc, hInk);
            HGDIOBJ oldPen   = SelectObject(drawDc, GetStockObject(NULL_PEN));

            // 深色外框 + 琥珀色内底
            RoundRect(drawDc, 0, 0, w, h, radius * 2, radius * 2);
            SelectObject(drawDc, hFill);
            RoundRect(drawDc, border, border, w - border, h - border,
                      (radius - border) * 2, (radius - border) * 2);

            SetBkMode(drawDc, TRANSPARENT);
            SetTextColor(drawDc, kOsdInk);
            RECT rt{ padX, padTop, w - padX, padTop + rcTitle.bottom };
            HGDIOBJ oldDrawFont = SelectObject(drawDc, fTitle);
            DrawTextW(drawDc, OSD_TITLE, -1, &rt, DT_NOPREFIX | DT_SINGLELINE | DT_LEFT);
            RECT rb{ padX, padTop + rcTitle.bottom, w - padX, h - padBot };
            SelectObject(drawDc, fBody);
            DrawTextW(drawDc, text, -1, &rb, DT_NOPREFIX | DT_WORDBREAK | DT_LEFT);

            // 遮罩：同形状的白色实心，用于求边缘覆盖率
            SelectObject(drawDc, maskBmp);
            SelectObject(drawDc, GetStockObject(WHITE_BRUSH));
            RoundRect(drawDc, 0, 0, w, h, radius * 2, radius * 2);
            GdiFlush();            // GDI 有批处理，读取 DIB 位前必须刷

            SelectObject(drawDc, oldBrush);
            SelectObject(drawDc, oldPen);
            SelectObject(drawDc, oldDrawFont);
            SelectObject(drawDc, oldBmp);
            DeleteObject(hFill);
            DeleteObject(hInk);

            // 降采样：对 k*k 个采样取平均。彩色图形状外为黑，平均后即预乘 alpha 的颜色
            const BYTE* color = (const BYTE*)colorBits;
            const BYTE* mask  = (const BYTE*)maskBits;
            BYTE* dst = (BYTE*)dstBits;
            const int samples = k * k;
            for (int y = 0; y < h / k; y++) {
                for (int x = 0; x < w / k; x++) {
                    unsigned b = 0, g = 0, r = 0, a = 0;
                    for (int sy = 0; sy < k; sy++) {
                        const BYTE* c = color + ((size_t)(y * k + sy) * w + x * k) * 4;
                        const BYTE* m = mask  + ((size_t)(y * k + sy) * w + x * k) * 4;
                        for (int sx = 0; sx < k; sx++) {
                            b += c[0]; g += c[1]; r += c[2];
                            a += m[0];
                            c += 4; m += 4;
                        }
                    }
                    dst[0] = (BYTE)(b / samples);
                    dst[1] = (BYTE)(g / samples);
                    dst[2] = (BYTE)(r / samples);
                    dst[3] = (BYTE)(a / samples);
                    dst += 4;
                }
            }

            g_osdDc = CreateCompatibleDC(nullptr);
            SelectObject(g_osdDc, g_osdBmp);
            g_osdW = w / k;
            g_osdH = h / k;
            ok = true;
        }
    }

    if (drawDc) DeleteDC(drawDc);
    if (colorBmp) DeleteObject(colorBmp);
    if (maskBmp) DeleteObject(maskBmp);
    SelectObject(dc, oldFont);
    DeleteObject(fTitle);
    DeleteObject(fBody);
    if (!ok) OsdFreeBitmap();
    return ok;
}

static void OsdRender(const wchar_t* text, UINT dpi, int maxWidth)
{
    OsdFreeBitmap();
    HDC dc = CreateCompatibleDC(nullptr);
    // 默认 4 倍过采样；临时位图过大（超宽屏 + 长文字折行）时退回 2 倍
    if (!OsdBuild(dc, text, dpi, maxWidth, 4)) OsdBuild(dc, text, dpi, maxWidth, 2);
    DeleteDC(dc);
}

static UINT MonitorDpi(HMONITOR mon)
{
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn getDpiForMonitor =
        (GetDpiForMonitorFn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    UINT dx = 0, dy = 0;
    if (getDpiForMonitor) getDpiForMonitor(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy);
    return dx ? dx : 96;
}

static void OsdPresent(POINT* pos)   // pos 为 nullptr 时保持当前位置
{
    POINT src{};
    SIZE size{ g_osdW, g_osdH };
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = g_osdAlpha;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_osdWnd, nullptr, pos, &size, g_osdDc, &src, 0, &bf, ULW_ALPHA);
}

static void OsdAnimate()
{
    ULONGLONG t = GetTickCount64() - g_osdT0;
    BYTE alpha;
    if (t < kOsdFadeInMs) {
        alpha = (BYTE)(255 * t / kOsdFadeInMs);
    } else if (t < kOsdFadeInMs + kOsdHoldMs) {
        alpha = 255;
    } else if (t < kOsdFadeInMs + kOsdHoldMs + kOsdFadeOutMs) {
        alpha = (BYTE)(255 - 255 * (t - kOsdFadeInMs - kOsdHoldMs) / kOsdFadeOutMs);
    } else {
        KillTimer(g_osdWnd, IDT_OSD_ANIM);
        ShowWindow(g_osdWnd, SW_HIDE);
        OsdFreeBitmap();
        return;
    }
    if (alpha != g_osdAlpha) {
        g_osdAlpha = alpha;
        OsdPresent(nullptr);
    }
}

static LRESULT CALLBACK OsdWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TIMER && wParam == IDT_OSD_ANIM) {
        OsdAnimate();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 弹出久坐提醒：显示在鼠标所在屏幕底部、任务栏上方，水平居中
static void OsdShow(const wchar_t* text)
{
    POINT pt{};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetCursorPos(&pt)) return;
    // 鼠标落在显示器之间的空隙时，取距离最近的那块
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) return;

    if (!g_osdWnd) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = OsdWndProc;
        wc.hInstance     = g_hInst;
        wc.lpszClassName = OSD_CLASS;
        RegisterClassExW(&wc);
        // 点击穿透、不抢焦点、不占任务栏按钮
        g_osdWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                                   WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                   OSD_CLASS, nullptr, WS_POPUP, 0, 0, 0, 0,
                                   nullptr, nullptr, g_hInst, nullptr);
        if (!g_osdWnd) return;
    }

    UINT dpi = MonitorDpi(mon);
    int margin = Px(24, dpi);
    OsdRender(text, dpi, (mi.rcWork.right - mi.rcWork.left) - margin * 2);
    if (!g_osdBmp) return;

    KillTimer(g_osdWnd, IDT_OSD_ANIM);
    // rcWork 已排除任务栏，其底边即任务栏上沿
    POINT pos{ mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - g_osdW) / 2,
               mi.rcWork.bottom - g_osdH - margin };
    g_osdT0 = GetTickCount64();
    g_osdAlpha = 0;
    OsdPresent(&pos);
    ShowWindow(g_osdWnd, SW_SHOWNOACTIVATE);
    SetTimer(g_osdWnd, IDT_OSD_ANIM, kOsdAnimStepMs, nullptr);
}

// “预览”按钮：按输入框里的当前文字弹一次，不必等到下一个周期
static void PreviewOsd()
{
    wchar_t buf[kSitTextMax + 1] = L"";
    GetWindowTextW(g_edSitText, buf, kSitTextMax + 1);
    OsdShow(buf[0] ? buf : kDefaultSitText);
}

static void OsdShutdown()
{
    if (!g_osdWnd) return;
    KillTimer(g_osdWnd, IDT_OSD_ANIM);
    DestroyWindow(g_osdWnd);
    g_osdWnd = nullptr;
    OsdFreeBitmap();
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
    lstrcpyW(g_fontFace, ncm.lfMessageFont.lfFaceName);
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
    g_sitCountdown = CreateCtrl(L"STATIC", L"距久坐提醒：--:--", 0, IDC_SIT_COUNTDOWN);

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

    CreateCtrl(L"STATIC", L"久坐提醒（分钟）：", 0, IDC_LBL_SIT);
    g_edSit = CreateCtrl(L"EDIT", L"60", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, IDC_ED_SIT, WS_EX_CLIENTEDGE);
    CreateCtrl(L"STATIC", L"0 表示关闭提醒", 0, IDC_NOTE_SIT);
    CreateCtrl(L"STATIC", L"提醒文字：", 0, IDC_LBL_SITTEXT);
    g_edSitText = CreateCtrl(L"EDIT", kDefaultSitText, WS_BORDER | ES_AUTOHSCROLL, IDC_ED_SITTEXT, WS_EX_CLIENTEDGE);
    SendMessageW(g_edSitText, EM_LIMITTEXT, kSitTextMax, 0);
    CreateCtrl(L"BUTTON", L"预览", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_SITPREVIEW);

    CreateCtrl(L"BUTTON", L"启动", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_BTN_START);
    CreateCtrl(L"BUTTON", L"停止", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_STOP);
    g_btnPause = CreateCtrl(L"BUTTON", L"暂停", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_PAUSE);

    g_hint = CreateCtrl(L"STATIC",
        L"提示：启动后每隔设定间隔轻移鼠标 1 像素，并阻止系统休眠与熄屏。\n"
        L"久坐提醒到期后会在鼠标所在屏幕底部弹出提示，3 秒后自动淡出。\n"
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
        case IDC_BTN_SITPREVIEW: PreviewOsd();          break;
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        // 辅助说明文字用灰色，其余静态控件走默认
        int id = GetDlgCtrlID((HWND)lParam);
        if (id == IDC_HINT || id == IDC_NOTE_INTERVAL || id == IDC_NOTE_STOP || id == IDC_NOTE_SIT) {
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
            if (g_sitRemain > 0 && --g_sitRemain <= 0) {
                OsdShow(g_sitText);
                g_sitRemain = g_sitIntervalSec;
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
        OsdShutdown();
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
    RECT rc{ 0, 0, S(460), S(430) };
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
