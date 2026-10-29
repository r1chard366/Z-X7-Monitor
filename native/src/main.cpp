// =============================================================================
//  Z-X7 PRO 显卡支架 · 第三方控制器（原生 Win32 + GDI+）
//
//  功能
//    · 每 200 ms 向支架写入 65 字节 HID 报告（Report ID 0x01 + 64 字节数据）
//    · CPU / GPU 温度来源：LibreHardwareMonitorLib + PawnIO（内置传感器宿主）
//    · macOS 风格界面：无边框圆角、交通灯、分段控件、滑块、圆角步进器、开关
//    · 浅色 / 深色主题（跟随系统，标题栏右侧切换，选择会记住）
//    · 查询间隔 0.5–60 秒：对数滑块、− / + 档位步进、点击数字直接输入
//    · 传感器宿主随软件启动，掉线后自动重启，退出软件才结束
//    · 托盘常驻、开机自启（登录计划任务，最高权限）
//
//  文件
//    src/main.cpp                 本文件
//    src/sensors/SensorHost.cs    传感器宿主（读 LHM 库，写温度到文本文件）
//    res/app.rc / res/app.ico     图标资源
//    build.bat                    一键编译
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;
using namespace std::chrono;

// ---------------------------------------------------------------- 常量
static const wchar_t* APP_NAME = L"Z-X7 PRO";
static const wchar_t* SENSOR_TASK = L"ZX7ProSensor";
static const int   IDI_APPICON = 101;
static const int   WM_APP_REFRESH = WM_APP + 1;
static const int   WM_APP_TRAY = WM_APP + 2;
static const int   WM_APP_TRAYINIT = WM_APP + 3;
static const int   TIMER_TRAY = 1;
static const int   TIMER_REFRESH = 2;   // 注册成功后再强制刷新一次
static const int   TIMER_SENSOR = 3;    // 传感器宿主掉线时自动拉起

static const float BASE_W = 760.0f;
static const float BASE_H = 600.0f;

static const int FRAME_MS = 200;          // 固件要求的帧间隔
static const int MIN_INTERVAL = 500;      // 传感器轮询下限 0.5 秒
static const int MAX_INTERVAL = 60000;    // 传感器轮询上限 60 秒

static inline Color Col(COLORREF c, BYTE a = 255) {
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// ---------------------------------------------------------------- 主题
// 所有界面颜色都从这里取，浅色 / 深色各一套，切换时只改 g_dark。
enum ThemeIndex {
    TC_BG, TC_CARD, TC_BORDER, TC_TEXT, TC_SUBTLE, TC_GREEN, TC_RED,
    TC_SEG, TC_SEG_SEL, TC_TRACK, TC_WARN, TC_BLUE, TC_FIELD, TC_HOVER,
    TC_KNOB, TC_SWITCH_OFF, TC_INV_TEXT,
    TC_COUNT
};

static const COLORREF THEME[2][TC_COUNT] = {
    {   // 浅色
        RGB(0xF5, 0xF5, 0xF7), RGB(0xFF, 0xFF, 0xFF), RGB(0xE3, 0xE3, 0xE8),
        RGB(0x1D, 0x1D, 0x1F), RGB(0x86, 0x86, 0x8B), RGB(0x28, 0xC8, 0x40),
        RGB(0xFF, 0x3B, 0x30), RGB(0xEF, 0xEF, 0xF2), RGB(0xFF, 0xFF, 0xFF),
        RGB(0xDC, 0xDC, 0xE1), RGB(0xC7, 0x7A, 0x00), RGB(0x0A, 0x84, 0xFF),
        RGB(0xF2, 0xF2, 0xF7), RGB(0xE4, 0xE4, 0xE9), RGB(0xFF, 0xFF, 0xFF),
        RGB(0xE3, 0xE3, 0xE8), RGB(0xFF, 0xFF, 0xFF)
    },
    {   // 深色
        RGB(0x1C, 0x1C, 0x1E), RGB(0x2C, 0x2C, 0x2E), RGB(0x3A, 0x3A, 0x3C),
        RGB(0xF2, 0xF2, 0xF7), RGB(0x98, 0x98, 0x9D), RGB(0x32, 0xD7, 0x4B),
        RGB(0xFF, 0x45, 0x3A), RGB(0x23, 0x23, 0x25), RGB(0x3E, 0x3E, 0x42),
        RGB(0x48, 0x48, 0x4A), RGB(0xFF, 0x9F, 0x0A), RGB(0x0A, 0x84, 0xFF),
        RGB(0x3A, 0x3A, 0x3C), RGB(0x48, 0x48, 0x4A), RGB(0xFF, 0xFF, 0xFF),
        RGB(0x48, 0x48, 0x4A), RGB(0x1C, 0x1C, 0x1E)
    }
};

static bool g_dark = false;

static inline Color Th(ThemeIndex i, BYTE a = 255) {
    COLORREF c = THEME[g_dark ? 1 : 0][i];
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

// ---------------------------------------------------------------- 状态
struct State {
    std::atomic<double> cpuTemp{ -1.0 };
    std::atomic<double> gpuTemp{ -1.0 };
    std::atomic<int>    cpuUse{ 0 };
    std::atomic<int>    gpuUse{ 0 };
    std::atomic<int>    cpuFreq{ 0 };
    std::atomic<int>    gpuFreq{ 0 };
    std::atomic<int>    memUse{ 0 };
    std::atomic<bool>   cpuOk{ false };
    std::atomic<bool>   gpuOk{ false };
    std::atomic<bool>   hidOpen{ false };
    std::atomic<bool>   streaming{ true };
    std::atomic<int>    channel{ 0 };      // 0 = CPU, 1 = GPU
    std::atomic<int>    intervalMs{ 1000 };
    std::atomic<bool>   appRunning{ true };
    std::atomic<bool>   sensorTask{ false };
};
static State g;

// ---- multi-GPU ----
static const int MAX_GPU = 8;
static std::atomic<double> g_gpuTemp[MAX_GPU];
static std::atomic<int>    g_gpuUse[MAX_GPU];
static std::atomic<int>    g_gpuClk[MAX_GPU];
static std::atomic<int>    g_gpuCount{ 0 };
static std::atomic<int>    g_gpuSel{ 0 };
static char                g_gpuNames[MAX_GPU][128] = {};

static int GpuIndex() {
    int n = g_gpuCount.load();
    int i = g_gpuSel.load();
    if (n <= 0) return 0;
    if (i < 0 || i >= n) i = 0;
    return i;
}
static double SelectedGpuTemp() {
    int n = g_gpuCount.load();
    if (n <= 0) return -1.0;
    return g_gpuTemp[GpuIndex()].load();
}
static bool SelectedGpuOk() { return SelectedGpuTemp() > 0.0; }
static int SelectedGpuUse() {
    if (g_gpuCount.load() <= 0) return 0;
    return g_gpuUse[GpuIndex()].load();
}
static int SelectedGpuClock() {
    if (g_gpuCount.load() <= 0) return 0;
    return g_gpuClk[GpuIndex()].load();
}
static const char* SelectedGpuName() {
    int n = g_gpuCount.load();
    if (n <= 0) return "";
    return g_gpuNames[GpuIndex()];
}

static HWND      g_hwnd = nullptr;
static HANDLE    g_hid = INVALID_HANDLE_VALUE;
static HANDLE    g_host = nullptr;          // 传感器宿主进程
static HANDLE    g_mutex = nullptr;
static ULONG_PTR g_gdiToken = 0;
static UINT      g_taskbarCreated = 0;
static std::atomic<ULONGLONG> g_pollWarmupUntil{ 0 };   // 刚拉起宿主时先按 1 秒轮询，别等用户设的 60 秒

static float g_scale = 1.0f;
static int   g_hover = -1;
static bool  g_dragSlider = false;
static bool  g_startHidden = false;
static bool  g_closeToTray = true;
static bool  g_trayAdded = false;
static int   g_trayTries = 0;
static bool  g_balloonShown = false;
static bool  g_topmost = false;
static int   g_refreshCount = 0;
static std::atomic<int> g_trayErr{ 0 };
static HICON g_iconBig = nullptr;
static HICON g_iconSmall = nullptr;

static RectF g_rSegCpu, g_rSegGpu, g_rSegBox, g_rButton, g_rSlider;
static RectF g_rChip[MAX_GPU];
static RectF g_rIntervalPill, g_rIntervalValue, g_rStepMinus, g_rStepPlus;
static RectF g_rThemeBox, g_rThemeLight, g_rThemeDark;
static HWND  g_edit = nullptr;
static WNDPROC g_editOldProc = nullptr;
static HFONT g_editFont = nullptr;
static HBRUSH g_brField = nullptr;
static bool  g_committing = false;
static bool  g_dirtyInterval = false;
static RectF g_rSwAuto, g_rSwTray, g_rLink, g_rClose, g_rMin, g_rMax;

static inline float S(float v) { return v * g_scale; }

// ================================================================ 小工具
static void LogLine(const char* msg, DWORD err) {
    wchar_t base[MAX_PATH] = { 0 };
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
    std::wstring dir = std::wstring(base) + L"\\ZX7Pro";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = dir + L"\\app.log";
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, file.c_str(), L"a") == 0 && fp) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d  %s err=%lu\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                msg, err);
        fclose(fp);
    }
}

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L'\\');
    return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

static bool FileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool ProcessExists(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static DWORD FindPid(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te{};
    DWORD sz = sizeof(te);
    bool ok = GetTokenInformation(tok, TokenElevation, &te, sz, &sz) && te.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

static int RunHidden(const std::wstring& cmd) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    if (!CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static std::wstring Widen(const char* s) {
    if (!s || !*s) return L"";
    wchar_t buf[256] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 255);
    return buf;
}

// ================================================================ 绘图工具
static void RoundRect(GraphicsPath& p, RectF r, float rad) {
    float d = rad * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    p.Reset();
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

static void FillRound(Graphics& gfx, RectF r, float rad, const Color& c) {
    GraphicsPath p; RoundRect(p, r, rad);
    SolidBrush b(c); gfx.FillPath(&b, &p);
}

static void StrokeRound(Graphics& gfx, RectF r, float rad, const Color& c, float w) {
    GraphicsPath p; RoundRect(p, r, rad);
    Pen pen(c, w); gfx.DrawPath(&pen, &p);
}

static void SoftShadow(Graphics& gfx, RectF r, float rad, int alpha = 6) {
    for (int i = 5; i >= 1; --i) {
        RectF rr(r.X - i, r.Y - i + 1.5f, r.Width + i * 2, r.Height + i * 2);
        GraphicsPath p; RoundRect(p, rr, rad + i);
        SolidBrush b(Color((BYTE)alpha, 0, 0, 0));
        gfx.FillPath(&b, &p);
    }
}

static void Text(Graphics& gfx, const wchar_t* s, const wchar_t* fam, float px,
                 const Color& c, RectF r,
                 StringAlignment h = StringAlignmentNear,
                 StringAlignment v = StringAlignmentCenter,
                 INT style = FontStyleRegular) {
    FontFamily ff(fam);
    Font f(&ff, px, style, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    gfx.DrawString(s, -1, &f, r, &sf, &b);
}

static float Measure(Graphics& gfx, const wchar_t* s, const wchar_t* fam, float px,
                     INT style = FontStyleRegular) {
    FontFamily ff(fam);
    Font f(&ff, px, style, UnitPixel);
    StringFormat sf; sf.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF box;
    gfx.MeasureString(s, -1, &f, PointF(0, 0), &sf, &box);
    return box.Width;
}

static void DrawSwitch(Graphics& gfx, RectF r, bool on) {
    FillRound(gfx, r, r.Height / 2, Th(on ? TC_GREEN : TC_SWITCH_OFF));
    if (on) {
        // 打开时给轨道一点点内阴影，视觉上更立体
        StrokeRound(gfx, r, r.Height / 2, Th(TC_GREEN, 160), 1.0f);
    }
    float knob = r.Height - S(4);
    float x = on ? r.GetRight() - knob - S(2) : r.X + S(2);
    SolidBrush sh(Color(g_dark ? 70 : 26, 0, 0, 0));
    gfx.FillEllipse(&sh, x, r.Y + S(2) + S(1), knob, knob);
    SolidBrush b(Th(TC_KNOB));
    gfx.FillEllipse(&b, x, r.Y + S(2), knob, knob);
}

// 标题栏右侧的浅色 / 深色切换：太阳 + 月亮
static void DrawSunIcon(Graphics& gfx, PointF c, float r, const Color& col) {
    SolidBrush b(col);
    gfx.FillEllipse(&b, c.X - r * 0.46f, c.Y - r * 0.46f, r * 0.92f, r * 0.92f);
    Pen p(col, S(1.3f));
    p.SetStartCap(LineCapRound);
    p.SetEndCap(LineCapRound);
    for (int i = 0; i < 8; ++i) {
        float a = (float)(i * 3.14159265 / 4.0);
        float x1 = c.X + cosf(a) * r * 0.74f, y1 = c.Y + sinf(a) * r * 0.74f;
        float x2 = c.X + cosf(a) * r * 1.02f, y2 = c.Y + sinf(a) * r * 1.02f;
        gfx.DrawLine(&p, x1, y1, x2, y2);
    }
}

static void DrawMoonIcon(Graphics& gfx, PointF c, float r, const Color& col,
                         const Color& cut) {
    SolidBrush b(col);
    gfx.FillEllipse(&b, c.X - r * 0.56f, c.Y - r * 0.56f, r * 1.12f, r * 1.12f);
    SolidBrush hole(cut);
    gfx.FillEllipse(&hole, c.X - r * 0.18f, c.Y - r * 0.78f, r * 1.12f, r * 1.12f);
}

// ================================================================ HID
static HANDLE OpenBracket() {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO devs = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    HANDLE found = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; i < 64; ++i) {
        SP_DEVICE_INTERFACE_DATA did{ sizeof(did) };
        if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &guid, i, &did)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &did, nullptr, 0, &need, nullptr);
        if (!need) continue;
        auto* detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(need);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devs, &did, detail, need, nullptr, nullptr)) {
            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attr{ sizeof(attr) };
                if (HidD_GetAttributes(h, &attr) && attr.VendorID == 0x5131 &&
                    attr.ProductID >= 0x2007 && attr.ProductID <= 0x2024) {
                    found = h;
                    free(detail);
                    break;
                }
                CloseHandle(h);
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

static BYTE ClampByte(double v, int hi = 255) {
    if (!(v == v)) return 0;
    int i = (int)v;
    if (i < 0) i = 0;
    if (i > hi) i = hi;
    return (BYTE)i;
}

static int SplitTemp(double v, BYTE& dec) {
    if (!(v == v) || v < 0) { dec = 0; return 0; }
    int whole = (int)floor(v);
    int frac = (int)floor((v - whole) * 100.0);
    if (frac < 0) frac = 0;
    if (frac > 99) frac = 99;
    dec = (BYTE)frac;
    return whole > 999 ? 999 : whole;
}

// 64 字节逻辑报文，与官方 device_hid.js 的 sendData 一一对应；
// 首字节 0x01 是 Report ID，写设备时再补一个尾部 0x00 凑成 65 字节。
static void BuildFrame(BYTE f[64], double cpuT, double gpuT, int cpuU, int gpuU,
                       int channel, int intervalSec) {
    memset(f, 0, 64);
    f[0] = 0x01; f[1] = 0x00; f[2] = 0x01; f[3] = 0x02;

    BYTE dec = 0;
    f[4] = (BYTE)SplitTemp(cpuT, dec); f[5] = dec; f[6] = 0;
    f[7] = ClampByte(cpuU, 99);

    f[14] = (BYTE)SplitTemp(gpuT, dec); f[15] = dec; f[16] = 0;
    f[17] = ClampByte(gpuU, 99);

    f[10] = ClampByte(g.cpuFreq / 100); f[11] = ClampByte(g.cpuFreq % 100);
    f[20] = ClampByte(g.gpuFreq / 100); f[21] = ClampByte(g.gpuFreq % 100);

    SYSTEMTIME st; GetLocalTime(&st);
    f[26] = (BYTE)(st.wYear / 100);
    f[27] = (BYTE)(st.wYear % 100);
    f[28] = (BYTE)st.wMonth; f[29] = (BYTE)st.wDay;
    f[30] = (BYTE)st.wHour;  f[31] = (BYTE)st.wMinute; f[32] = (BYTE)st.wSecond;
    f[33] = (BYTE)st.wDayOfWeek;
    f[34] = ClampByte(g.memUse, 99);
    f[37] = 0; f[38] = (BYTE)intervalSec; f[39] = (BYTE)channel;
    f[40] = 1; f[41] = 0; f[42] = 0; f[48] = 0;
}

static void WriteFrame(HANDLE hid) {
    if (hid == INVALID_HANDLE_VALUE) return;

    int ch = g.channel.load();
    double cpuT = g.cpuTemp.load();
    double gpuT = SelectedGpuTemp();
    bool gpuOk = gpuT > 0.0;
    if (ch == 0 && !g.cpuOk.load() && gpuOk) ch = 1;
    else if (ch == 1 && !gpuOk && g.cpuOk.load()) ch = 0;

    int intervalSec = (g.intervalMs.load() + 999) / 1000;
    if (intervalSec < 1) intervalSec = 1;

    BYTE frame[64];
    BuildFrame(frame, cpuT < 0 ? 0 : cpuT, gpuT < 0 ? 0 : gpuT,
               g.cpuUse.load(), g.gpuUse.load(), ch, intervalSec);

    BYTE wire[65];
    memcpy(wire, frame, 64);
    wire[64] = 0x00;
    DWORD written = 0;
    if (!WriteFile(hid, wire, 65, &written, nullptr)) g.hidOpen = false;
}

// ================================================================ 传感器
// 宿主把结果写在以下几个位置之一，按顺序找：
//   <exe>\sensors\LibreHardwareMonitor\zx7sensors.txt
//   <exe>\sensors\zx7sensors.txt
//   %ProgramData%\ZX7Pro\zx7sensors.txt
//   %TEMP%\zx7sensors.txt
static bool ReadSensorFile(double& cpu, int& admin) {
    wchar_t tmp[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tmp);
    wchar_t pd[MAX_PATH] = { 0 };
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    std::wstring dir = ExeDir();

    std::wstring candidates[] = {
        dir + L"\\sensors\\LibreHardwareMonitor\\zx7sensors.txt",
        dir + L"\\sensors\\zx7sensors.txt",
        std::wstring(pd) + L"\\ZX7Pro\\zx7sensors.txt",
        std::wstring(tmp) + L"zx7sensors.txt",
    };

    for (const auto& p : candidates) {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, p.c_str(), L"r") != 0 || !fp) continue;

        char buf[1024] = { 0 };
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        if (!n) continue;

        double c = -1.0;
        int adm = 0;
        long long ts = 0;
        double gt[MAX_GPU];
        int gu[MAX_GPU], gc[MAX_GPU];
        for (int i = 0; i < MAX_GPU; ++i) { gt[i] = -1.0; gu[i] = 0; gc[i] = 0; }
        int maxIdx = -1;
        char namesBuf[768] = { 0 };

        char* ctx = nullptr;
        for (char* tok = strtok_s(buf, ";", &ctx); tok; tok = strtok_s(nullptr, ";", &ctx)) {
            if (strncmp(tok, "cpu=", 4) == 0) {
                c = atof(tok + 4);
            } else if (strncmp(tok, "gpu", 3) == 0) {
                char* eq = strchr(tok, '=');
                if (eq) {
                    int idx = atoi(tok + 3);
                    if (idx >= 0 && idx < MAX_GPU) {
                        gt[idx] = atof(eq + 1);
                        if (idx > maxIdx) maxIdx = idx;
                    }
                }
            } else if (strncmp(tok, "use", 3) == 0) {
                char* eq = strchr(tok, '=');
                if (eq) { int idx = atoi(tok + 3); if (idx >= 0 && idx < MAX_GPU) gu[idx] = atoi(eq + 1); }
            } else if (strncmp(tok, "clk", 3) == 0) {
                char* eq = strchr(tok, '=');
                if (eq) { int idx = atoi(tok + 3); if (idx >= 0 && idx < MAX_GPU) gc[idx] = atoi(eq + 1); }
            } else if (strncmp(tok, "names=", 6) == 0) {
                strncpy_s(namesBuf, tok + 6, _TRUNCATE);
            } else if (strncmp(tok, "t=", 2) == 0) {
                ts = _atoi64(tok + 2);
            } else if (strncmp(tok, "adm=", 4) == 0) {
                adm = atoi(tok + 4);
            }
        }

        if (ts > 0) {
            long long now = (long long)time(nullptr);
            if (now - ts > 10) continue;                 // stale
        }

        cpu = c;
        admin = adm;

        int count = maxIdx + 1;
        if (count < 0) count = 0;
        if (count > MAX_GPU) count = MAX_GPU;

        // names: "Name0|Name1|..."
        int idx = 0;
        char* nctx = nullptr;
        for (char* nm = strtok_s(namesBuf, "|", &nctx); nm && idx < count; nm = strtok_s(nullptr, "|", &nctx)) {
            strncpy_s(g_gpuNames[idx], nm, _TRUNCATE);
            ++idx;
        }
        for (; idx < count; ++idx) sprintf_s(g_gpuNames[idx], "GPU %d", idx + 1);

        for (int i = 0; i < count; ++i) {
            g_gpuTemp[i] = gt[i];
            g_gpuUse[i] = gu[i];
            g_gpuClk[i] = gc[i];
        }
        static int lastLogged = -1;
        if (count != lastLogged) {
            char lb[64];
            sprintf_s(lb, "sensors: gpu count=%d", count);
            LogLine(lb, 0);
            lastLogged = count;
        }
        g_gpuCount = count;
        if (g_gpuSel.load() >= count) g_gpuSel = 0;
        return true;
    }
    return false;
}
static std::wstring FindHostExe() {
    std::wstring dir = ExeDir();
    std::wstring p1 = dir + L"\\sensors\\LibreHardwareMonitor\\ZX7SensorHost.exe";
    if (FileExists(p1)) return p1;
    std::wstring p2 = dir + L"\\sensors\\ZX7SensorHost.exe";
    if (FileExists(p2)) return p2;
    return L"";
}

static bool SensorTaskExists() {
    return RunHidden(std::wstring(L"schtasks /Query /TN ") + SENSOR_TASK) == 0;
}

static bool HostRunning() {
    return ProcessExists(L"ZX7SensorHost.exe");
}

static void MarkSensorWarmup() {
    // 刚启动宿主时数据还没写出来，先按 1 秒一次读，避免要等用户设置的 60 秒
    g_pollWarmupUntil = GetTickCount64() + 12000;
}

// Sensor host runs as a highest-privilege scheduled task so the main app
// can stay at normal user level (no UAC on every launch).
static void StopSensorHost() {
    if (SensorTaskExists()) {
        RunHidden(std::wstring(L"schtasks /End /TN ") + SENSOR_TASK);
    }
    if (g_host) {
        TerminateProcess(g_host, 0);
        CloseHandle(g_host);
        g_host = nullptr;
    }
}

// 软件启动、切到 CPU、看门狗发现掉线时都会调用。
// 宿主同时提供 CPU 和 GPU 数据，所以不管当前选的是哪个通道都要运行。
static void StartSensorHost() {
    if (HostRunning()) {
        g.sensorTask = true;
        MarkSensorWarmup();                  // 已经在跑：直接读最新数据
        return;
    }

    std::wstring exe = FindHostExe();
    if (exe.empty()) { LogLine("host: not found", 0); return; }

    if (!g.sensorTask.load()) g.sensorTask = SensorTaskExists();
    if (g.sensorTask.load()) {
        int rc = RunHidden(std::wstring(L"schtasks /Run /TN ") + SENSOR_TASK);
        if (rc == 0) {
            MarkSensorWarmup();
            LogLine("host: started via task", 0);
        } else {
            // 任务可能被删了；重新判断，别每 15 秒白试一次
            g.sensorTask = SensorTaskExists();
            LogLine("host: task run failed", (DWORD)rc);
        }
        return;
    }

    if (IsElevated()) {
        std::wstring tr = std::wstring(L"\\\"") + exe + L"\\\"";
        std::wstring cmd = std::wstring(L"schtasks /Create /TN ") + SENSOR_TASK +
                           L" /TR \"" + tr + L"\" /SC ONCE /ST 00:00 /RL HIGHEST /F";
        if (RunHidden(cmd) == 0) {
            RunHidden(std::wstring(L"schtasks /Run /TN ") + SENSOR_TASK);
            g.sensorTask = true;
            MarkSensorWarmup();
            LogLine("host: task installed", 0);
            return;
        }
    }
    LogLine("host: no task (run installer bat as admin once)", 0);
}
static HICON LoadAppIcon(int w, int h) {
    HICON ic = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                 IMAGE_ICON, w, h, LR_DEFAULTCOLOR);
    if (!ic) ic = (HICON)LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, w, h, LR_SHARED);
    return ic;
}

static void TrayBalloon(const wchar_t* text) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, APP_NAME);
    wcscpy_s(nid.szInfo, text);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static HICON MakeFallbackIcon() {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = 16;
    bi.bV5Height = -16;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(16, 16, 1, 1, nullptr);
    if (color && bits) {
        DWORD* px = (DWORD*)bits;
        for (int i = 0; i < 256; ++i) px[i] = 0xFF1D1D1F;   // 深色底
        for (int i = 0; i < 256; ++i) {
            int x = i % 16, y = i / 16;
            if (x >= 4 && x <= 11 && y >= 3 && y <= 12) px[i] = 0xFF28C840;  // 绿色块
        }
    }
    ReleaseDC(nullptr, dc);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

static void TrayAdd(HWND hwnd) {
    if (g_trayAdded) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_iconSmall ? g_iconSmall : LoadAppIcon(16, 16);
    wcscpy_s(nid.szTip, APP_NAME);

    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!g_trayAdded) {
        nid.cbSize = NOTIFYICONDATAW_V2_SIZE;
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }
    if (g_trayAdded) {
        g_trayErr = 0;
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        LogLine(IsElevated() ? "tray: ok (elevated)" : "tray: ok (standard)", 0);
        {
            NOTIFYICONIDENTIFIER ident{};
            ident.cbSize = sizeof(ident);
            ident.hWnd = hwnd;
            ident.uID = 1;
            RECT trc{};
            HRESULT hr = Shell_NotifyIconGetRect(&ident, &trc);
            char buf[160];
            sprintf_s(buf, "tray: rect hr=0x%08lX L=%ld T=%ld R=%ld B=%ld hwnd=%p icon=%p",
                      (unsigned long)hr, (long)trc.left, (long)trc.top,
                      (long)trc.right, (long)trc.bottom, (void*)hwnd, (void*)nid.hIcon);
            LogLine(buf, 0);
        }
        if (!g_balloonShown) {
            g_balloonShown = true;
            TrayBalloon(L"已驻留托盘。图标可能收在任务栏右下角的 ^ 里，"
                        L"可用 Ctrl+Alt+Z 唤回窗口。");
        }
    } else {
        g_trayErr = (int)GetLastError();
        LogLine("tray: NIM_ADD failed", (DWORD)g_trayErr.load());
    }
}

static void TrayRemove() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// ================================================================ 开机自启
static std::atomic<bool> g_autoOn{ false };

static bool QueryAutostart() {
    return RunHidden(L"schtasks /Query /TN ZX7Pro") == 0;
}

// Modify tasks created by an elevated process requires admin: run once with UAC.
static bool RunSchtasksElevated(const std::wstring& params) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = L"schtasks.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    WaitForSingleObject(sei.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

static void SetAutostart(bool on) {
    // Remove legacy Run entry (it can only start without elevation).
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"ZX7Pro");
        RegCloseKey(k);
    }

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring tr = std::wstring(L"\\\"") + path + L"\\\" -tray";

    if (on) {
        std::wstring cmd = L"schtasks /Create /TN ZX7Pro /TR \"" + tr + L"\" /SC ONLOGON /F";
        if (RunHidden(cmd) != 0) {
            std::wstring p = L"/Create /TN ZX7Pro /TR \"" + tr + L"\" /SC ONLOGON /F";
            if (!RunSchtasksElevated(p)) LogLine("autostart: create failed", 0);
        }
    } else {
        if (RunHidden(L"schtasks /Delete /TN ZX7Pro /F") != 0) {
            if (!RunSchtasksElevated(L"/Delete /TN ZX7Pro /F"))
                LogLine("autostart: delete failed", 0);
        }
    }

    g_autoOn = QueryAutostart();
}
// ================================================================ 布局
// ================================================================ 设置持久化
static bool SystemPrefersDark() {
    HKEY k;
    DWORD v = 1, sz = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type,
                             (LPBYTE)&v, &sz) != ERROR_SUCCESS) v = 1;
        RegCloseKey(k);
    }
    return v == 0;
}

static void LoadSettings() {
    g_dark = SystemPrefersDark();          // 未设置过就跟随系统
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ZX7Pro", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegQueryValueExW(k, L"interval", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS) {
        int iv = (int)v;
        if (iv < MIN_INTERVAL) iv = MIN_INTERVAL;
        if (iv > MAX_INTERVAL) iv = MAX_INTERVAL;
        g.intervalMs = iv;
    }
    sz = sizeof(v);
    if (RegQueryValueExW(k, L"dark", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS) g_dark = (v != 0);
    sz = sizeof(v);
    if (RegQueryValueExW(k, L"gpu", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS) g_gpuSel = (int)v;
    sz = sizeof(v);
    if (RegQueryValueExW(k, L"channel", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS) g.channel = (int)v;
    sz = sizeof(v);
    if (RegQueryValueExW(k, L"closeToTray", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
        g_closeToTray = (v != 0);
    RegCloseKey(k);
}

static void SaveSettings() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ZX7Pro", 0, nullptr, 0, KEY_WRITE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD v = (DWORD)g.intervalMs.load();
    RegSetValueExW(k, L"interval", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    v = g_dark ? 1 : 0;
    RegSetValueExW(k, L"dark", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    v = (DWORD)g_gpuSel.load();
    RegSetValueExW(k, L"gpu", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    v = (DWORD)g.channel.load();
    RegSetValueExW(k, L"channel", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    v = g_closeToTray ? 1 : 0;
    RegSetValueExW(k, L"closeToTray", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}
// ================================================================ GPU 选择 + 间隔输入
static int GpuChipCount() {
    if (g.channel.load() == 0) return 0;          // only in GPU channel
    int n = g_gpuCount.load();
    if (n > 3) n = 3;
    if (n < 0) n = 0;
    return n > 1 ? n : 0;
}

static RectF ChipRect(int i) {
    float x = g_rSegBox.GetRight() + S(16) + i * S(94);
    return RectF(x, g_rSegBox.Y, S(86), g_rSegBox.Height);
}

// ---- 间隔：0.5–60 秒。滑块用对数刻度，短间隔区段不会被挤成一条缝；
//      ± 按常用档位跳，点击数字可以直接输入。
static const int kIntervalSteps[] = {
    500, 600, 700, 800, 900, 1000, 1200, 1500, 2000, 2500, 3000,
    4000, 5000, 6000, 8000, 10000, 15000, 20000, 30000, 45000, 60000
};
static const int kIntervalStepCount =
    (int)(sizeof(kIntervalSteps) / sizeof(kIntervalSteps[0]));

static float IntervalPct(int ms) {
    if (ms < MIN_INTERVAL) ms = MIN_INTERVAL;
    if (ms > MAX_INTERVAL) ms = MAX_INTERVAL;
    double lo = log((double)MIN_INTERVAL), hi = log((double)MAX_INTERVAL);
    return (float)((log((double)ms) - lo) / (hi - lo));
}

static int IntervalFromPct(float pct) {
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;
    double lo = log((double)MIN_INTERVAL), hi = log((double)MAX_INTERVAL);
    int ms = (int)(exp(lo + pct * (hi - lo)) / 100.0 + 0.5) * 100;
    if (ms < MIN_INTERVAL) ms = MIN_INTERVAL;
    if (ms > MAX_INTERVAL) ms = MAX_INTERVAL;
    return ms;
}

static int StepInterval(int ms, int dir) {
    if (dir > 0) {
        for (int i = 0; i < kIntervalStepCount; ++i)
            if (kIntervalSteps[i] > ms) return kIntervalSteps[i];
        return MAX_INTERVAL;
    }
    for (int i = kIntervalStepCount - 1; i >= 0; --i)
        if (kIntervalSteps[i] < ms) return kIntervalSteps[i];
    return MIN_INTERVAL;
}

static void SetInterval(int ms, bool save) {
    if (ms < MIN_INTERVAL) ms = MIN_INTERVAL;
    if (ms > MAX_INTERVAL) ms = MAX_INTERVAL;
    g.intervalMs = ms;
    if (save) SaveSettings();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void HideIntervalEditor() {
    if (!g_edit || !IsWindowVisible(g_edit)) return;
    ShowWindow(g_edit, SW_HIDE);
    if (GetFocus() == g_edit) SetFocus(g_hwnd);
}

static void CommitIntervalEditor() {
    if (!g_edit || !IsWindowVisible(g_edit) || g_committing) return;
    g_committing = true;
    wchar_t buf[32] = { 0 };
    GetWindowTextW(g_edit, buf, 32);
    double sec = _wtof(buf);
    if (sec > 0.0) SetInterval((int)(sec * 1000.0 + 0.5), true);
    HideIntervalEditor();
    g_committing = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { CommitIntervalEditor(); return 0; }
        if (wp == VK_ESCAPE) {
            HideIntervalEditor();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CHAR: {
        wchar_t c = (wchar_t)wp;
        if (c == 8 || c == 1 || c == 3 || c == 22 || c == 24) break;   // 退格 / 全选 / 复制 / 粘贴 / 剪切
        if (c == L'.') {
            wchar_t cur[32] = { 0 };
            GetWindowTextW(hwnd, cur, 32);
            if (wcschr(cur, L'.')) return 0;
            break;
        }
        if (c >= L'0' && c <= L'9') break;
        return 0;                                    // 其余字符不接收
    }
    case WM_KILLFOCUS:
        CommitIntervalEditor();
        break;
    case WM_CONTEXTMENU:
        return 0;                                    // 深色下也不弹浅色右键菜单
    }
    return CallWindowProcW(g_editOldProc, hwnd, msg, wp, lp);
}

static void EnsureIntervalEditor() {
    if (g_edit) return;
    g_edit = CreateWindowExW(0, L"EDIT", L"",
                             WS_CHILD | ES_AUTOHSCROLL | ES_CENTER,
                             0, 0, 10, 10, g_hwnd, (HMENU)2001,
                             GetModuleHandleW(nullptr), nullptr);
    if (!g_edit) return;
    g_editFont = CreateFontW(-(int)S(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI Semibold");
    SendMessageW(g_edit, WM_SETFONT,
                 (WPARAM)(g_editFont ? g_editFont : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 5, 0);
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    g_editOldProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
}

static void ShowIntervalEditor() {
    EnsureIntervalEditor();
    if (!g_edit) return;
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", g.intervalMs.load() / 1000.0);
    SetWindowTextW(g_edit, buf);

    int x = (int)(g_rIntervalValue.X + S(2));
    int y = (int)(g_rIntervalValue.Y + S(6));
    int w = (int)(g_rIntervalValue.Width - S(4));
    int h = (int)(g_rIntervalValue.Height - S(12));
    SetWindowPos(g_edit, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    SetFocus(g_edit);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void SetDarkMode(bool dark) {
    if (g_dark == dark) return;
    g_dark = dark;
    SaveSettings();
    if (g_brField) { DeleteObject(g_brField); g_brField = nullptr; }
    if (g_hwnd) {
        BOOL d = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(g_hwnd, 20, &d, sizeof(d));
        if (g_edit) InvalidateRect(g_edit, nullptr, TRUE);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

struct Layout { RectF card1, card2; };

static Layout ComputeLayout(float w) {
    Layout L;
    float m = S(28);
    L.card1 = RectF(m, S(64), w - m * 2, S(272));
    L.card2 = RectF(m, S(352), w - m * 2, S(176));

    float innerL = m + S(32), innerR = w - m - S(32);

    g_rSegBox = RectF(innerL, S(370), S(220), S(38));
    g_rSegCpu = RectF(g_rSegBox.X + S(3), g_rSegBox.Y + S(3), S(107), S(32));
    g_rSegGpu = RectF(g_rSegBox.X + S(110), g_rSegBox.Y + S(3), S(107), S(32));
    g_rButton = RectF(innerR - S(112), S(370), S(112), S(38));

    g_rThemeBox = RectF(w - S(26) - S(66), S(9), S(66), S(28));
    g_rThemeLight = RectF(g_rThemeBox.X + S(2), g_rThemeBox.Y + S(2), S(31), S(24));
    g_rThemeDark = RectF(g_rThemeBox.X + S(33), g_rThemeBox.Y + S(2), S(31), S(24));

    g_rIntervalPill = RectF(innerR - S(150), S(430), S(150), S(34));
    g_rStepMinus = RectF(g_rIntervalPill.X, g_rIntervalPill.Y, S(34), S(34));
    g_rStepPlus = RectF(g_rIntervalPill.GetRight() - S(34), g_rIntervalPill.Y, S(34), S(34));
    g_rIntervalValue = RectF(g_rStepMinus.GetRight(), g_rIntervalPill.Y, S(82), S(34));
    g_rSlider = RectF(innerL, S(486), innerR - innerL, S(6));

    g_rSwAuto = RectF(innerL, S(544), S(38), S(21));
    g_rSwTray = RectF(m + S(232), S(544), S(38), S(21));
    g_rLink = RectF(L.card1.GetRight() - S(140), L.card1.GetBottom() - S(42), S(108), S(24));
    g_rClose = RectF(S(12), S(13), S(20), S(20));
    g_rMin = RectF(S(34), S(13), S(20), S(20));
    g_rMax = RectF(S(56), S(13), S(20), S(20));
    return L;
}

// ================================================================ 界面
static void DrawThemeToggle(Graphics& gfx) {
    FillRound(gfx, g_rThemeBox, g_rThemeBox.Height / 2, Th(TC_SEG));
    StrokeRound(gfx, g_rThemeBox, g_rThemeBox.Height / 2, Th(TC_BORDER), 1.0f);
    auto side = [&](RectF zone, bool sel, int id, bool sun) {
        RectF knob(zone.X + S(1), zone.Y + S(1), zone.Width - S(2), zone.Height - S(2));
        Color back = Th(TC_SEG);
        if (sel) {
            back = g_dark ? Th(TC_SEG_SEL) : Th(TC_CARD);
            FillRound(gfx, knob, knob.Height / 2, back);
            StrokeRound(gfx, knob, knob.Height / 2, Th(TC_BORDER), 1.0f);
        } else if (g_hover == id) {
            back = Th(TC_HOVER);
            FillRound(gfx, knob, knob.Height / 2, back);
        }
        Color fg = (sel || g_hover == id) ? Th(TC_TEXT) : Th(TC_SUBTLE);
        PointF c(zone.X + zone.Width / 2, zone.Y + zone.Height / 2);
        if (sun) DrawSunIcon(gfx, c, S(6.2f), fg);
        else DrawMoonIcon(gfx, c, S(6.4f), fg, back);
    };
    side(g_rThemeLight, !g_dark, 11, true);
    side(g_rThemeDark, g_dark, 12, false);
}

// 圆角步进器：− [ 1.0 秒 ] +，点中间数字进入编辑
static void DrawIntervalControl(Graphics& gfx) {
    bool editing = g_edit && IsWindowVisible(g_edit);
    RectF pill = g_rIntervalPill;
    FillRound(gfx, pill, pill.Height / 2, Th(TC_FIELD));
    StrokeRound(gfx, pill, pill.Height / 2,
                editing ? Th(TC_BLUE) : Th(TC_BORDER), editing ? S(1.6f) : 1.0f);

    auto stepper = [&](RectF z, const wchar_t* glyph, int id) {
        if (!editing && g_hover == id) {
            RectF hov(z.X + S(6), z.Y + S(6), z.Width - S(12), z.Height - S(12));
            FillRound(gfx, hov, hov.Height / 2, Th(TC_HOVER));
        }
        Color c = (!editing && g_hover == id) ? Th(TC_TEXT) : Th(TC_SUBTLE);
        Text(gfx, glyph, L"Segoe UI", S(18), c, z,
             StringAlignmentCenter, StringAlignmentCenter);
    };
    stepper(g_rStepMinus, L"\u2212", 13);
    stepper(g_rStepPlus, L"+", 14);

    Pen sep(Th(TC_BORDER), 1.0f);
    gfx.DrawLine(&sep, g_rStepMinus.GetRight(), pill.Y + S(9),
                 g_rStepMinus.GetRight(), pill.GetBottom() - S(9));
    gfx.DrawLine(&sep, g_rStepPlus.X, pill.Y + S(9),
                 g_rStepPlus.X, pill.GetBottom() - S(9));

    if (!editing) {
        wchar_t val[32];
        swprintf_s(val, L"%.1f", g.intervalMs.load() / 1000.0);
        float wv = Measure(gfx, val, L"Segoe UI Semibold", S(15));
        float wu = Measure(gfx, L"秒", L"Segoe UI", S(11));
        float total = wv + S(3) + wu;
        float x = g_rIntervalValue.X + (g_rIntervalValue.Width - total) / 2;
        Color vc = (g_hover == 10) ? Th(TC_BLUE) : Th(TC_TEXT);
        Text(gfx, val, L"Segoe UI Semibold", S(15), vc,
             RectF(x, g_rIntervalValue.Y, wv + S(4), g_rIntervalValue.Height));
        Text(gfx, L"秒", L"Segoe UI", S(11), Th(TC_SUBTLE),
             RectF(x + wv + S(3), g_rIntervalValue.Y, wu + S(4), g_rIntervalValue.Height));
        if (g_hover == 10) {
            Pen p(Th(TC_BLUE, 130), S(1.0f));
            gfx.DrawLine(&p, g_rIntervalValue.X + S(14), g_rIntervalValue.GetBottom() - S(5),
                         g_rIntervalValue.GetRight() - S(14), g_rIntervalValue.GetBottom() - S(5));
        }
    }
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) { EndPaint(hwnd, &ps); return; }

    Bitmap bmp(W, H, PixelFormat32bppPARGB);
    Graphics gfx(&bmp);
    gfx.SetSmoothingMode(SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    gfx.SetPixelOffsetMode(PixelOffsetModeHalf);

    SolidBrush bg(Th(TC_BG));
    gfx.FillRectangle(&bg, 0, 0, W, H);

    Layout L = ComputeLayout((float)W);

    // 标题栏：交通灯 + 标题 + 明暗切换
    float cy = S(23);
    const COLORREF dotColor[3] = {
        RGB(0xFF, 0x5F, 0x57), RGB(0xFE, 0xBC, 0x2E), RGB(0x28, 0xC8, 0x40)
    };
    const float dotX[3] = { S(22), S(44), S(66) };
    const int   dotId[3] = { 3, 8, 9 };
    for (int i = 0; i < 3; ++i) {
        bool hover = (g_hover == dotId[i]);
        Color c = Col(dotColor[i]);
        if (hover) c = Color(255, (BYTE)(GetRValue(dotColor[i]) * 0.86),
                                  (BYTE)(GetGValue(dotColor[i]) * 0.86),
                                  (BYTE)(GetBValue(dotColor[i]) * 0.86));
        SolidBrush b(c);
        gfx.FillEllipse(&b, dotX[i] - S(6), cy - S(6), S(12), S(12));
        if (hover) {                     // macOS 风格：悬停时显示小符号
            Pen glyph(Color(150, 0x1D, 0x1D, 0x1F), S(1.2f));
            glyph.SetStartCap(LineCapRound);
            glyph.SetEndCap(LineCapRound);
            float g = S(2.6f);
            if (i == 0) {
                gfx.DrawLine(&glyph, dotX[i] - g, cy - g, dotX[i] + g, cy + g);
                gfx.DrawLine(&glyph, dotX[i] - g, cy + g, dotX[i] + g, cy - g);
            } else if (i == 1) {
                gfx.DrawLine(&glyph, dotX[i] - g, cy, dotX[i] + g, cy);
            } else {
                gfx.DrawLine(&glyph, dotX[i] - g, cy, dotX[i] + g, cy);
                gfx.DrawLine(&glyph, dotX[i], cy - g, dotX[i], cy + g);
            }
        }
    }
    if (g_topmost) {
        Pen ring(Th(TC_TEXT), S(1.5f));
        gfx.DrawEllipse(&ring, S(66) - S(9), cy - S(9), S(18), S(18));
    }
    Text(gfx, APP_NAME, L"Segoe UI Semibold", S(13), Th(TC_SUBTLE),
         RectF(0, S(12), (float)W, S(22)), StringAlignmentCenter, StringAlignmentCenter);
    DrawThemeToggle(gfx);

    auto card = [&](RectF r) {
        SoftShadow(gfx, r, S(20), g_dark ? 16 : 6);
        FillRound(gfx, r, S(20), Th(TC_CARD));
        StrokeRound(gfx, r, S(20), Th(TC_BORDER), 1.0f);
    };
    card(L.card1);
    card(L.card2);

    // ---- 温度卡片 ----
    bool cpuSel = g.channel.load() == 0;
    double temp = cpuSel ? g.cpuTemp.load() : SelectedGpuTemp();
    bool ok = cpuSel ? g.cpuOk.load() : SelectedGpuOk();

    float x0 = L.card1.X + S(32);
    wchar_t title[192];
    if (cpuSel) {
        wcscpy_s(title, L"CPU 温度");
    } else {
        std::wstring nm = Widen(SelectedGpuName());
        if (nm.empty()) wcscpy_s(title, L"GPU 温度");
        else swprintf_s(title, L"GPU 温度 · %s", nm.c_str());
    }
    Text(gfx, title, L"Segoe UI", S(13), Th(TC_SUBTLE),
         RectF(x0, L.card1.Y + S(24), S(420), S(20)));

    wchar_t big[32], frac[8];
    if (temp >= 0) {
        int whole = (int)floor(temp);
        int f10 = (int)floor((temp - whole) * 10.0 + 0.5);
        if (f10 > 9) { whole += 1; f10 = 0; }
        swprintf_s(big, L"%d", whole);
        swprintf_s(frac, L".%d", f10);
    } else {
        wcscpy_s(big, L"--");
        frac[0] = 0;
    }

    float bigPx = S(94);
    float bigW = Measure(gfx, big, L"Segoe UI Semibold", bigPx);
    Text(gfx, big, L"Segoe UI Semibold", bigPx, Th(ok ? TC_TEXT : TC_SUBTLE),
         RectF(x0 - S(3), L.card1.Y + S(52), bigW + S(24), S(112)));

    float after = x0 + bigW + S(2);
    if (frac[0]) {
        float fw = Measure(gfx, frac, L"Segoe UI Semibold", S(32));
        Text(gfx, frac, L"Segoe UI Semibold", S(32), Th(TC_SUBTLE),
             RectF(after, L.card1.Y + S(94), fw + S(6), S(44)));
        after += fw + S(4);
    }
    Text(gfx, L"\u00B0C", L"Segoe UI", S(28), Th(TC_SUBTLE),
         RectF(after + S(6), L.card1.Y + S(94), S(48), S(44)));

    // 右侧信息
    float rx = L.card1.GetRight() - S(228);
    float ry = L.card1.Y + S(38);
    wchar_t buf[128];
    int use = cpuSel ? g.cpuUse.load() : SelectedGpuUse();
    int freq = cpuSel ? g.cpuFreq.load() : SelectedGpuClock();

    auto row = [&](const wchar_t* label, const wchar_t* value, float y) {
        Text(gfx, label, L"Segoe UI", S(12), Th(TC_SUBTLE), RectF(rx, y, S(90), S(18)));
        Text(gfx, value, L"Segoe UI Semibold", S(15), Th(TC_TEXT),
             RectF(rx + S(92), y, S(136), S(18)));
    };
    swprintf_s(buf, L"%d%%", use);               row(L"占用", buf, ry);
    swprintf_s(buf, L"%d MHz", freq);            row(L"频率", buf, ry + S(30));
    swprintf_s(buf, L"%d%%", g.memUse.load());   row(L"内存", buf, ry + S(60));
    swprintf_s(buf, L"%.1f 秒", g.intervalMs.load() / 1000.0);
    row(L"轮询", buf, ry + S(90));

    // 状态行
    bool hidOk = g.hidOpen.load();
    bool run = g.streaming.load();
    const wchar_t* status;
    ThemeIndex statusColor;
    if (!run) {
        status = L"已停止 · 屏幕几秒后熄灭";
        statusColor = TC_SUBTLE;
    } else if (!hidOk) {
        status = L"未找到支架 · 请插好并退出官方程序";
        statusColor = TC_RED;
    } else if (cpuSel && !g.cpuOk.load()) {
        status = g.sensorTask.load()
                     ? L"CPU：传感器服务已就绪，等待第一帧数据…"
                     : L"CPU：需一次性安装传感器服务（管理员运行 安装传感器服务.bat）";
        statusColor = TC_WARN;
    } else if (!cpuSel && !SelectedGpuOk()) {
        status = g.sensorTask.load()
                     ? L"GPU：传感器服务已启动，等待第一帧数据…"
                     : L"GPU：需一次性安装传感器服务（管理员运行 安装传感器服务.bat）";
        statusColor = TC_WARN;
    } else {
        status = L"已连接 · 5131:2007 · 200 ms / 帧";
        statusColor = TC_GREEN;
    }

    float sy = L.card1.GetBottom() - S(34);
    SolidBrush dot(Th(statusColor));
    gfx.FillEllipse(&dot, x0, sy + S(7), S(7), S(7));
    Text(gfx, status, L"Segoe UI", S(12), Th(TC_SUBTLE),
         RectF(x0 + S(14), sy, L.card1.Width - S(170), S(20)));

    if (run && hidOk && cpuSel && !g.cpuOk.load()) {
        Text(gfx, L"启用方法 ›", L"Segoe UI Semibold", S(12), Th(TC_BLUE),
             g_rLink, StringAlignmentFar, StringAlignmentCenter);
    }

    // ---- 控制卡片 ----
    FillRound(gfx, g_rSegBox, S(11), Th(TC_SEG));
    auto seg = [&](RectF r, const wchar_t* label, bool sel) {
        if (sel) {
            if (!g_dark) SoftShadow(gfx, r, S(8));
            FillRound(gfx, r, S(8), g_dark ? Th(TC_SEG_SEL) : Th(TC_CARD));
            if (g_dark) StrokeRound(gfx, r, S(8), Th(TC_BORDER), 1.0f);
        }
        Text(gfx, label, L"Segoe UI Semibold", S(14), Th(sel ? TC_TEXT : TC_SUBTLE),
             r, StringAlignmentCenter, StringAlignmentCenter);
    };
    seg(g_rSegCpu, L"CPU", cpuSel);
    seg(g_rSegGpu, L"GPU", !cpuSel);

    int chips = GpuChipCount();
    for (int i = 0; i < chips; ++i) {
        RectF r = ChipRect(i);
        bool sel = (i == GpuIndex());
        FillRound(gfx, r, S(10), sel ? (g_dark ? Th(TC_SEG_SEL) : Th(TC_CARD)) : Th(TC_SEG));
        if (sel) StrokeRound(gfx, r, S(10), Th(TC_BORDER), 1.0f);
        wchar_t t[24];
        swprintf_s(t, L"GPU %d", i + 1);
        Text(gfx, t, L"Segoe UI Semibold", S(12), Th(sel ? TC_TEXT : TC_SUBTLE),
             r, StringAlignmentCenter, StringAlignmentCenter);
    }

    // 分隔线，把开关行和间隔行分开
    Pen divider(Th(TC_BORDER), 1.0f);
    gfx.DrawLine(&divider, g_rSlider.X, S(422), g_rSlider.GetRight(), S(422));

    // 圆角步进器 + 滑块
    Text(gfx, L"查询间隔", L"Segoe UI", S(13), Th(TC_SUBTLE),
         RectF(g_rSlider.X, g_rIntervalPill.Y, S(200), g_rIntervalPill.Height));
    DrawIntervalControl(gfx);

    float pct = IntervalPct(g.intervalMs.load());
    float trackY = g_rSlider.Y + g_rSlider.Height / 2;

    Pen track(Th(TC_TRACK), S(5));
    track.SetStartCap(LineCapRound);
    track.SetEndCap(LineCapRound);
    gfx.DrawLine(&track, g_rSlider.X, trackY, g_rSlider.GetRight(), trackY);

    float tx = g_rSlider.X + g_rSlider.Width * pct;
    Pen prog(Th(TC_TEXT), S(5));
    prog.SetStartCap(LineCapRound);
    prog.SetEndCap(LineCapRound);
    gfx.DrawLine(&prog, g_rSlider.X, trackY, tx, trackY);

    float thumbR = (g_hover == 4 || g_dragSlider) ? S(10) : S(9);
    SolidBrush sh(Color(g_dark ? 110 : 30, 0, 0, 0));
    gfx.FillEllipse(&sh, tx - thumbR + S(0.5f), trackY - thumbR + S(2),
                    thumbR * 2, thumbR * 2);
    SolidBrush thumb(Th(TC_KNOB));
    Pen thumbPen(g_dark ? Th(TC_BORDER) : Col(RGB(0xD0, 0xD0, 0xD6)), S(1.2f));
    gfx.FillEllipse(&thumb, tx - thumbR, trackY - thumbR, thumbR * 2, thumbR * 2);
    gfx.DrawEllipse(&thumbPen, tx - thumbR, trackY - thumbR, thumbR * 2, thumbR * 2);

    Text(gfx, L"0.5 秒", L"Segoe UI", S(10.5f), Th(TC_SUBTLE),
         RectF(g_rSlider.X, S(498), S(80), S(16)));
    Text(gfx, L"60 秒", L"Segoe UI", S(10.5f), Th(TC_SUBTLE),
         RectF(g_rSlider.GetRight() - S(80), S(498), S(80), S(16)),
         StringAlignmentFar);
    Text(gfx, L"拖动滑块或点击数字输入", L"Segoe UI", S(10.5f), Th(TC_SUBTLE, 200),
         RectF(g_rSlider.X, S(498), g_rSlider.Width, S(16)),
         StringAlignmentCenter);

    Color btnBg = run ? Th(TC_SEG) : Th(TC_TEXT);
    Color btnFg = run ? Th(TC_TEXT) : Th(TC_INV_TEXT);
    if (g_hover == 2) {
        btnBg = run ? Th(TC_HOVER)
                    : (g_dark ? Col(RGB(0xDD, 0xDD, 0xE2)) : Col(RGB(0x33, 0x33, 0x36)));
    }
    FillRound(gfx, g_rButton, S(10), btnBg);
    Text(gfx, run ? L"停止" : L"开始", L"Segoe UI Semibold", S(14), btnFg,
         g_rButton, StringAlignmentCenter, StringAlignmentCenter);

    // ---- 底部开关 ----
    DrawSwitch(gfx, g_rSwAuto, g_autoOn.load());
    Text(gfx, L"开机自启动", L"Segoe UI", S(12), Th(TC_SUBTLE),
         RectF(g_rSwAuto.X + S(48), g_rSwAuto.Y - S(4), S(120), S(26)));

    DrawSwitch(gfx, g_rSwTray, g_closeToTray);
    if (g_trayAdded) {
        Text(gfx, L"关闭到托盘（图标在 ^ 内）", L"Segoe UI", S(12), Th(TC_SUBTLE),
             RectF(g_rSwTray.X + S(48), g_rSwTray.Y - S(4), S(240), S(26)));
    } else {
        wchar_t t[64];
        swprintf_s(t, L"托盘注册失败 %d", g_trayErr.load());
        Text(gfx, t, L"Segoe UI", S(12), Th(TC_RED),
             RectF(g_rSwTray.X + S(48), g_rSwTray.Y - S(4), S(200), S(26)));
    }
    Text(gfx, L"Ctrl+Alt+Z 唤回窗口", L"Segoe UI", S(11), Th(TC_SUBTLE, 220),
         RectF(L.card2.GetRight() - S(220), g_rSwTray.Y - S(4), S(220), S(26)),
         StringAlignmentFar);

    Graphics target(hdc);
    target.DrawImage(&bmp, 0, 0, W, H);
    EndPaint(hwnd, &ps);
}

// ================================================================ 交互
static void SetIntervalFromX(float x) {
    float pct = (x - g_rSlider.X) / g_rSlider.Width;
    SetInterval(IntervalFromPct(pct), false);
    g_dirtyInterval = true;
}

static LRESULT HitTest(int x, int y) {
    PointF p((float)x, (float)y);
    int chips = GpuChipCount();
    for (int i = 0; i < chips; ++i) if (ChipRect(i).Contains(p)) return 20 + i;
    if (g_rStepMinus.Contains(p)) return 13;
    if (g_rStepPlus.Contains(p)) return 14;
    if (g_rIntervalValue.Contains(p)) return 10;
    if (g_rThemeLight.Contains(p)) return 11;
    if (g_rThemeDark.Contains(p)) return 12;
    if (g_rSwAuto.Contains(p)) return 6;
    if (g_rSwTray.Contains(p)) return 7;
    if (g_rLink.Contains(p) && g.streaming.load() && g.hidOpen.load() &&
        g.channel.load() == 0 && !g.cpuOk.load()) return 5;
    if (g_rClose.Contains(p)) return 3;
    if (g_rMin.Contains(p)) return 8;
    if (g_rMax.Contains(p)) return 9;
    if (g_rSegCpu.Contains(p)) return 0;
    if (g_rSegGpu.Contains(p)) return 1;
    if (g_rButton.Contains(p)) return 2;
    RectF hit(g_rSlider.X - S(10), g_rSlider.Y - S(16), g_rSlider.Width + S(20), S(38));
    if (hit.Contains(p)) return 4;
    return -1;
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, IsWindowVisible(hwnd) ? L"隐藏窗口" : L"显示窗口");
    AppendMenuW(m, MF_STRING, 2, g.streaming.load() ? L"停止上报" : L"开始上报");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_autoOn.load() ? MF_CHECKED : 0), 3, L"开机自启动");
    AppendMenuW(m, MF_STRING | (g_closeToTray ? MF_CHECKED : 0), 4, L"关闭时最小化到托盘");
    AppendMenuW(m, MF_STRING | (g_dark ? MF_CHECKED : 0), 6, L"深色模式");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);

    switch (cmd) {
    case 1: CommitIntervalEditor();
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW); break;
    case 2:
        g.streaming = !g.streaming.load();
        if (g.streaming.load()) StartSensorHost();
        break;
    case 3: SetAutostart(!g_autoOn.load()); InvalidateRect(hwnd, nullptr, FALSE); break;
    case 4: g_closeToTray = !g_closeToTray; break;
    case 6: SetDarkMode(!g_dark); break;
    case 5: CommitIntervalEditor(); TrayRemove(); DestroyWindow(hwnd); return;
    default: break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ================================================================ 窗口过程
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {
        g_trayAdded = false;                 // explorer 就绪/重启后补注册
        TrayAdd(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        SetTimer(hwnd, TIMER_SENSOR, 15000, nullptr);
        return 0;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        if (g_edit && (HWND)lp == g_edit) {
            SetBkColor(dc, THEME[g_dark ? 1 : 0][TC_FIELD]);
            SetTextColor(dc, THEME[g_dark ? 1 : 0][TC_TEXT]);
            if (!g_brField) {
                g_brField = CreateSolidBrush(THEME[g_dark ? 1 : 0][TC_FIELD]);
            }
            return (LRESULT)g_brField;
        }
        break;
    }

    case WM_APP_TRAYINIT:
        TrayAdd(hwnd);
        if (g_trayAdded) SetTimer(hwnd, TIMER_REFRESH, 3000, nullptr);
        else SetTimer(hwnd, TIMER_TRAY, 3000, nullptr);
        return 0;

    case WM_APP_REFRESH:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_APP_TRAY: {
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_TRAY) {
            if (!g_trayAdded) { TrayAdd(hwnd); ++g_trayTries; }
            if (g_trayAdded) KillTimer(hwnd, TIMER_TRAY);
        } else if (wp == TIMER_REFRESH) {
            KillTimer(hwnd, TIMER_REFRESH);
            if (g_trayAdded) {
                TrayRemove();
                TrayAdd(hwnd);
                LogLine("tray: refreshed", 0);
            }
            if (++g_refreshCount < 2) SetTimer(hwnd, TIMER_REFRESH, 5000, nullptr);
        } else if (wp == TIMER_SENSOR) {
            // 宿主被任务计划/崩溃/休眠结束时，自动再拉起
            if (g.streaming.load() && g.sensorTask.load() && !HostRunning()) {
                LogLine("host: watchdog restart", 0);
                StartSensorHost();
            }
        }
        return 0;

    case WM_CLOSE:
        CommitIntervalEditor();
        if (g_closeToTray && g_trayAdded) {
            ShowWindow(hwnd, SW_HIDE);
            TrayBalloon(L"已最小化到托盘（图标可能在 ^ 里），Ctrl+Alt+Z 唤回窗口。");
            return 0;
        }
        DestroyWindow(hwnd);                 // 没有托盘就正常退出，绝不“关不掉”
        return 0;

    case WM_HOTKEY:
        if (wp == 1) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;

    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (HitTest(pt.x, pt.y) >= 0) return HTCLIENT;
        if (pt.y < (int)S(46)) return HTCAPTION;
        return HTCLIENT;
    }

    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        PointF p((float)pt.x, (float)pt.y);
        RectF sliderHit(g_rSlider.X - S(10), g_rSlider.Y - S(16),
                        g_rSlider.Width + S(20), S(38));
        if (sliderHit.Contains(p) || g_rIntervalPill.Contains(p)) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (delta) {
                SetInterval(StepInterval(g.intervalMs.load(), delta > 0 ? 1 : -1), true);
            }
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g_dragSlider) { SetIntervalFromX((float)x); return 0; }
        LRESULT h = HitTest(x, y);
        if (h != g_hover) { g_hover = (int)h; InvalidateRect(hwnd, nullptr, FALSE); }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hover != -1) { g_hover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        CommitIntervalEditor();              // 点到别处先保存正在输入的值
        LRESULT h = HitTest(x, y);
        if (h == 0) {
            HideIntervalEditor();
            g.channel = 0;
            SaveSettings();
            StartSensorHost();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (h == 1) {
            HideIntervalEditor();
            g.channel = 1;
            SaveSettings();
            // 不结束传感器宿主：GPU 温度也由它提供，切通道不能把数据源关掉
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (h == 2) {
            g.streaming = !g.streaming.load();
            if (g.streaming.load()) StartSensorHost();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (h == 3) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (h == 8) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (h == 9) {
            g_topmost = !g_topmost;
            SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (h == 10) {
            ShowIntervalEditor();
        } else if (h == 11) {
            HideIntervalEditor();
            SetDarkMode(false);
        } else if (h == 12) {
            HideIntervalEditor();
            SetDarkMode(true);
        } else if (h == 13) {
            HideIntervalEditor();
            SetInterval(StepInterval(g.intervalMs.load(), -1), true);
        } else if (h == 14) {
            HideIntervalEditor();
            SetInterval(StepInterval(g.intervalMs.load(), 1), true);
        } else if (h >= 20 && h < 20 + MAX_GPU) {
            HideIntervalEditor();
            g_gpuSel = (int)(h - 20);
            SaveSettings();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (h == 4) {
            HideIntervalEditor();
            g_dragSlider = true;
            SetCapture(hwnd);
            SetIntervalFromX((float)x);
        } else if (h == 5) {
            ShellExecuteW(nullptr, L"open",
                          L"https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases",
                          nullptr, nullptr, SW_SHOWNORMAL);
        } else if (h == 6) {
            SetAutostart(!g_autoOn.load());
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (h == 7) {
            g_closeToTray = !g_closeToTray;
            SaveSettings();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_dragSlider) {
            g_dragSlider = false;
            ReleaseCapture();
            if (g_dirtyInterval) { g_dirtyInterval = false; SaveSettings(); }
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (g_dragSlider) {                  // 拖动中被抢走鼠标：别让滑块继续跟手
            g_dragSlider = false;
            if (g_dirtyInterval) { g_dirtyInterval = false; SaveSettings(); }
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursorW(nullptr, HitTest(pt.x, pt.y) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_DESTROY:
        UnregisterHotKey(hwnd, 1);
        TrayRemove();
        g.appRunning = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ================================================================ 工作线程
static int CpuUsagePercent() {
    static ULONGLONG prevIdle = 0, prevKernel = 0, prevUser = 0;
    FILETIME idleFt, kernelFt, userFt;
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) return 0;
    ULONGLONG i = ((ULONGLONG)idleFt.dwHighDateTime << 32) | idleFt.dwLowDateTime;
    ULONGLONG k = ((ULONGLONG)kernelFt.dwHighDateTime << 32) | kernelFt.dwLowDateTime;
    ULONGLONG u = ((ULONGLONG)userFt.dwHighDateTime << 32) | userFt.dwLowDateTime;
    if (!prevIdle) { prevIdle = i; prevKernel = k; prevUser = u; return 0; }
    ULONGLONG di = i - prevIdle, dk = k - prevKernel, du = u - prevUser;
    prevIdle = i; prevKernel = k; prevUser = u;
    ULONGLONG total = dk + du;
    if (!total) return 0;
    int usage = (int)(100.0 - (double)di * 100.0 / (double)total);
    return usage < 0 ? 0 : (usage > 100 ? 100 : usage);
}

static int ReadBaseClock() {
    HKEY k;
    int mhz = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD v = 0, sz = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"~MHz", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS) mhz = (int)v;
        RegCloseKey(k);
    }
    return mhz;
}

static void Worker() {
    g.cpuFreq = ReadBaseClock();
    auto nextFrame = steady_clock::now();
    auto nextPoll = steady_clock::now();

    while (g.appRunning.load()) {
        bool run = g.streaming.load();

        if (!run) {
            if (g_hid != INVALID_HANDLE_VALUE) {
                CloseHandle(g_hid);
                g_hid = INVALID_HANDLE_VALUE;
                g.hidOpen = false;
            }
        } else if (g_hid == INVALID_HANDLE_VALUE) {
            g_hid = OpenBracket();
            g.hidOpen = (g_hid != INVALID_HANDLE_VALUE);
            if (g_hid != INVALID_HANDLE_VALUE) PostMessageW(g_hwnd, WM_APP_REFRESH, 0, 0);
        }

        auto now = steady_clock::now();

        if (run && now >= nextPoll) {
            double c = -1;
            int adm = 0;
            if (ReadSensorFile(c, adm)) {
                if (c > 0 && c < 130) { g.cpuTemp = c; g.cpuOk = true; }
            }
            g.cpuUse = CpuUsagePercent();
            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            if (GlobalMemoryStatusEx(&ms)) g.memUse = (int)ms.dwMemoryLoad;
            // 刚拉起宿主后的十几秒先按 1 秒读，别让用户等满一个 60 秒轮询
            int iv = g.intervalMs.load();
            if (GetTickCount64() < g_pollWarmupUntil.load() && iv > 1000) iv = 1000;
            nextPoll = steady_clock::now() + milliseconds(iv);
            PostMessageW(g_hwnd, WM_APP_REFRESH, 0, 0);
        }

        if (run && g_hid != INVALID_HANDLE_VALUE && now >= nextFrame) {
            WriteFrame(g_hid);
            nextFrame = steady_clock::now() + milliseconds(FRAME_MS);
            if (!g.hidOpen.load()) {
                CloseHandle(g_hid);
                g_hid = INVALID_HANDLE_VALUE;
            }
        }
        Sleep(15);
    }
    if (g_hid != INVALID_HANDLE_VALUE) CloseHandle(g_hid);
}

// ================================================================ 入口
static void KillOtherInstances() {
    const wchar_t* names[] = { L"ZX7Pro.exe", L"ZX7Pro.old.exe" };
    for (const wchar_t* n : names) {
        DWORD pid = FindPid(n);
        while (pid && pid != GetCurrentProcessId()) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) { TerminateProcess(h, 0); WaitForSingleObject(h, 3000); CloseHandle(h); }
            else break;
            pid = FindPid(n);
        }
    }
    Sleep(400);
}

static bool ClaimSingleInstance() {
    g_mutex = CreateMutexW(nullptr, FALSE, L"ZX7Pro_SingleInstance");
    if (!g_mutex) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 有旧实例残留（升级时文件被改名仍占用）：结束它并接管
        KillOtherInstances();
        CloseHandle(g_mutex);
        g_mutex = CreateMutexW(nullptr, FALSE, L"ZX7Pro_SingleInstance");
        return GetLastError() != ERROR_ALREADY_EXISTS;
    }
    return true;
}

static void StripMOTWTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            StripMOTWTree(p);
        } else {
            DeleteFileW((p + L":Zone.Identifier").c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void RemoveMOTW() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    DeleteFileW((std::wstring(path) + L":Zone.Identifier").c_str());
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    if (!ClaimSingleInstance()) return 0;
    RemoveMOTW();
    StripMOTWTree(ExeDir() + L"\\sensors");

    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI* PFN)(HANDLE);
        auto p = (PFN)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (p) p((HANDLE)-4);
    }

    LoadSettings();
    const wchar_t* cmd = GetCommandLineW();
    if (wcsstr(cmd, L"-gpu")) g.channel = 1;
    if (wcsstr(cmd, L"-cpu")) g.channel = 0;
    if (wcsstr(cmd, L"-tray") || wcsstr(cmd, L"--silent")) g_startHidden = true;

    GdiplusStartupInput gi;
    GdiplusStartup(&g_gdiToken, &gi, nullptr);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ZX7ProWindow";
    wc.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    g_iconBig = wc.hIcon;
    g_iconSmall = wc.hIconSm;
    RegisterClassExW(&wc);

    g_scale = GetDpiForSystem() / 96.0f;
    int W = (int)(BASE_W * g_scale), H = (int)(BASE_H * g_scale);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(0, L"ZX7ProWindow", APP_NAME, WS_POPUP | WS_SYSMENU,
                                (sw - W) / 2, (sh - H) / 2, W, H,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 0;

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_iconBig);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);

    DWORD corner = 2;                        // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
    BOOL dark = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT, 'Z');
    PostMessageW(hwnd, WM_APP_TRAYINIT, 0, 0);   // 必须在消息循环里注册，否则某些 shell 会“接受但丢弃”

    if (g_startHidden) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    LogLine(g_startHidden ? "window: hidden" : "window: shown", 0);


    g_autoOn = QueryAutostart();
    StartSensorHost();                  // CPU / GPU 数据都靠它，随软件启动，不再等切到 CPU

    std::thread(Worker).detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g.appRunning = false;
    StopSensorHost();
    GdiplusShutdown(g_gdiToken);
    return 0;
}
