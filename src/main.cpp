// FPS Overlay — Lightweight DirectX 11 + ImGui performance monitor (Customized)
//
// Customizations:
//   - Current FPS + 1% Low FPS via ETW frame-time percentile tracking
//   - Clean numeric CPU & GPU frequencies (MHz) without sparkline graphs
//   - Larger default overlay text scale (125%)

#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <evntrace.h>
#include <evntcons.h>
#include <psapi.h>
#include <wbemidl.h>
#include <comdef.h>
#include <winhttp.h>
#include <wincodec.h>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <ctime>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <utility>
#include <string>
#include <map>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")

#include "imgui.h"
#include "imgui_internal.h"
#include "lhwm-cpp-wrapper.h"
#include <tuple>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "locale/locale.h"
#include "locale/rtl_shape.h"

// ═══════════════════════════════════════════════════════════════════════════
// Constants & safety defines
// ═══════════════════════════════════════════════════════════════════════════
#define WM_TRAYICON   (WM_USER + 1)
#define IDM_SETTINGS  1001
#define IDM_EXIT      1002
#define IDM_SHOW      1003
#define IDM_HIDE      1004
#define IDM_UPDATE    1005
#define IDM_RESET_POS 1006

#define APP_VERSION "v1.8.0-custom"

static const int kConfigDlgOuterW = 680;
static const int kConfigDlgMinOuterH = 560;
static const float kSettingsSidebarW = 168.f;

enum SettingsTab {
    SETTINGS_TAB_DISPLAY = 0,
    SETTINGS_TAB_GPU,
    SETTINGS_TAB_FREQUENCY,
    SETTINGS_TAB_APPEARANCE,
    SETTINGS_TAB_TEMPERATURE,
    SETTINGS_TAB_HOTKEYS,
    SETTINGS_TAB_STARTUP,
    SETTINGS_TAB_LANGUAGE,
    SETTINGS_TAB_ABOUT,
    SETTINGS_TAB_COUNT
};

#define IDR_PAWNIO_SETUP 101
#define IDR_GITHUB_ICON  102
#define IDR_KOFI_ICON    103

#ifndef PROCESS_TRACE_MODE_EVENT_RECORD
#define PROCESS_TRACE_MODE_EVENT_RECORD 0x10000000
#endif
#ifndef PROCESS_TRACE_MODE_REAL_TIME
#define PROCESS_TRACE_MODE_REAL_TIME    0x00000100
#endif
#ifndef EVENT_CONTROL_CODE_ENABLE_PROVIDER
#define EVENT_CONTROL_CODE_ENABLE_PROVIDER 1
#endif
#ifndef TRACE_LEVEL_INFORMATION
#define TRACE_LEVEL_INFORMATION 4
#endif

static const GUID DXGI_PROVIDER =
    { 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };

static const GUID D3D9_PROVIDER =
    { 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA850511C6B9 } };

static const GUID DXGKRNL_PROVIDER =
    { 0x802EC45A, 0x1E99, 0x4B83, { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D } };

static const ULONGLONG DXGKRNL_KEYWORD_PRESENT = 0x8000000;
static const ULONGLONG DXGKRNL_KEYWORD_BASE    = 0x1;

static const USHORT DXGKRNL_EVENT_PRESENT_INFO = 0x00B8;
static const USHORT DXGKRNL_EVENT_FLIP_INFO    = 0x00A8;
static const USHORT DXGKRNL_EVENT_BLIT_INFO    = 0x00A6;

static const char* ETW_SESSION_NAME = "FPSOverlay_ETW";

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════
#define LAYOUT_VERTICAL   0
#define LAYOUT_HORIZONTAL 1
#define LAYOUT_STEAM      2

#define POS_TOP_LEFT       0
#define POS_TOP_CENTER     1
#define POS_TOP_RIGHT      2
#define POS_BOTTOM_LEFT    3
#define POS_BOTTOM_CENTER  4
#define POS_BOTTOM_RIGHT   5

#define TIME_FORMAT_24H    0
#define TIME_FORMAT_12H    1

#define FREQ_PATH_MAX   260

struct OverlayConfig {
    bool showFPS  = true;
    bool show1PercentLow = true; // 1% Low FPS
    bool showCpuUsage = true;
    bool showCpuTemp = true;
    bool showGpuUsage = true;
    bool showGpuTemp = true;
    bool showVRAM = true;
    bool showRAM  = true;
    bool showProcessName = false;
    bool showTime = false;
    int  timeFormat = TIME_FORMAT_24H;
    bool timeShowSeconds = true;
    int  layoutStyle = LAYOUT_VERTICAL;
    bool useFahrenheit = false;
    bool autoStart = false;
    int  position = POS_TOP_LEFT;
    int  opacity  = 85;
    int  textOpacity = 100;
    bool transparentBackground = false;
    int  toggleKey = VK_INSERT;
    int  exitKey   = VK_END;
    float customX = -1.0f;
    float customY = -1.0f;
    int  selectedGpu = 0;
    int  overlayScale = 125;  // Larger default text scale (125%)
    bool showCpuPower = false;
    bool showGpuPower = false;
    bool showCpuFan = false;
    bool showGpuFan = false;
    bool showCpuFreq = true;     // Enabled by default
    bool showGpuCoreFreq = true; // Enabled by default
    char cpuFreqPath[FREQ_PATH_MAX] = "";
    char gpuCoreFreqPath[FREQ_PATH_MAX] = "";
    char language[32] = "en-US";
    int  settingsTab = SETTINGS_TAB_DISPLAY;
};

// ═══════════════════════════════════════════════════════════════════════════
// GPU list
// ═══════════════════════════════════════════════════════════════════════════
#define MAX_GPUS 8

struct GpuInfo {
    char name[256];
    std::string tempPath;
    std::string loadPath;
    int         loadPathPri  = -1;
    std::string vramUsedPath;
    std::string vramTotalPath;
    int         vramTotalPri = -1;
    std::string powerPath;
    std::string fanPath;
    std::vector<std::pair<std::string, std::string>> coreClockOpts;
};
static GpuInfo g_gpuList[MAX_GPUS];
static int g_gpuCount = 0;

inline float ToDisplayTemp(float celsius, bool useFahrenheit) {
    return useFahrenheit ? (celsius * 9.0f / 5.0f + 32.0f) : celsius;
}

static void FormatOverlayTime(const OverlayConfig& cfg, char* out, size_t outLen)
{
    if (!out || outLen == 0) return;
    out[0] = '\0';

    SYSTEMTIME st;
    GetLocalTime(&st);

    if (cfg.timeFormat == TIME_FORMAT_12H) {
        int h = (int)st.wHour % 12;
        if (h == 0) h = 12;
        const char* ampm = (st.wHour < 12) ? locale::T("AM") : locale::T("PM");
        if (cfg.timeShowSeconds)
            snprintf(out, outLen, "%d:%02u:%02u %s", h, st.wMinute, st.wSecond, ampm);
        else
            snprintf(out, outLen, "%d:%02u %s", h, st.wMinute, ampm);
    } else {
        if (cfg.timeShowSeconds)
            snprintf(out, outLen, "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
        else
            snprintf(out, outLen, "%02u:%02u", st.wHour, st.wMinute);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Configuration file (INI)
// ═══════════════════════════════════════════════════════════════════════════
static char g_configPath[MAX_PATH] = "";
static char g_pawnioRebootStatePath[MAX_PATH] = "";

static void InitConfigPath()
{
    if (g_configPath[0] != '\0') return;
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    snprintf(g_configPath, MAX_PATH, "%sconfig.ini", exePath);
    snprintf(g_pawnioRebootStatePath, MAX_PATH, "%sfpsoverlay-pawnio-reboot.state", exePath);
}

static int ReadIniInt(const char* section, const char* key, int defaultVal)
{
    return GetPrivateProfileIntA(section, key, defaultVal, g_configPath);
}

static float ReadIniFloat(const char* section, const char* key, float defaultVal)
{
    char buf[32];
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_configPath);
    if (buf[0] == '\0') return defaultVal;
    return (float)atof(buf);
}

static void WriteIniInt(const char* section, const char* key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    WritePrivateProfileStringA(section, key, buf, g_configPath);
}

static void WriteIniFloat(const char* section, const char* key, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", value);
    WritePrivateProfileStringA(section, key, buf, g_configPath);
}

static void ReadIniStr(const char* section, const char* key, char* out, size_t cap)
{
    if (!cap) return;
    GetPrivateProfileStringA(section, key, "", out, (DWORD)cap, g_configPath);
    out[cap - 1] = '\0';
}

static void WriteIniStr(const char* section, const char* key, const char* value)
{
    WritePrivateProfileStringA(section, key, value ? value : "", g_configPath);
}

static bool WritePawnIORebootPendingStateFile(const char* hex16)
{
    if (!hex16 || strlen(hex16) != 16) return false;
    InitConfigPath();
    char line[96];
    snprintf(line, sizeof(line), "FPSOVERLAY_PAWNIO_REBOOT 1 %s\n", hex16);
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH;
    HANDLE h = CreateFileA(g_pawnioRebootStatePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const size_t n = strlen(line);
    const BOOL ok = WriteFile(h, line, (DWORD)n, &written, nullptr) && written == (DWORD)n;
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok != FALSE;
}

static bool ReadPawnIORebootPendingStateFile(char* hexOut, size_t cap)
{
    if (!hexOut || cap < 17) return false;
    hexOut[0] = '\0';
    InitConfigPath();
    HANDLE h = CreateFileA(g_pawnioRebootStatePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[256] = {};
    DWORD rd = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd == 0) return false;
    buf[rd] = '\0';
    int ver = 0;
    char hexBuf[24] = {};
    if (sscanf_s(buf, "FPSOVERLAY_PAWNIO_REBOOT %d %23s", &ver, hexBuf, (unsigned)sizeof(hexBuf)) < 2 || ver != 1)
        return false;
    if (strlen(hexBuf) != 16) return false;
    unsigned long long v = 0;
    if (sscanf_s(hexBuf, "%llx", &v) != 1) return false;
    (void)v;
    snprintf(hexOut, cap, "%s", hexBuf);
    return true;
}

static void DeletePawnIORebootPendingStateFile()
{
    InitConfigPath();
    DeleteFileA(g_pawnioRebootStatePath);
}

static void LoadConfig(OverlayConfig& cfg)
{
    InitConfigPath();
    DWORD attrib = GetFileAttributesA(g_configPath);
    if (attrib == INVALID_FILE_ATTRIBUTES) return;

    cfg.showFPS         = ReadIniInt("Display", "showFPS", 1) != 0;
    cfg.show1PercentLow = ReadIniInt("Display", "show1PercentLow", 1) != 0;
    {
        const int legacyCpu = ReadIniInt("Display", "showCPU", 1);
        int cu = ReadIniInt("Display", "showCpuUsage", -1);
        if (cu < 0) cu = legacyCpu;
        cfg.showCpuUsage = cu != 0;
        int ct = ReadIniInt("Display", "showCpuTemp", -1);
        if (ct < 0) ct = legacyCpu;
        cfg.showCpuTemp = ct != 0;
        const int legacyGpu = ReadIniInt("Display", "showGPU", 1);
        int gu = ReadIniInt("Display", "showGpuUsage", -1);
        if (gu < 0) gu = legacyGpu;
        cfg.showGpuUsage = gu != 0;
        int gt = ReadIniInt("Display", "showGpuTemp", -1);
        if (gt < 0) gt = legacyGpu;
        cfg.showGpuTemp = gt != 0;
    }
    cfg.showVRAM        = ReadIniInt("Display", "showVRAM", 1) != 0;
    cfg.showRAM         = ReadIniInt("Display", "showRAM", 1) != 0;
    cfg.showProcessName = false; // Minimal overlay: never show the current app/game name.
    cfg.showTime        = ReadIniInt("Display", "showTime", 0) != 0;
    cfg.timeFormat      = ReadIniInt("Display", "timeFormat", TIME_FORMAT_24H);
    cfg.timeShowSeconds = ReadIniInt("Display", "timeShowSeconds", 1) != 0;

    cfg.showCpuPower    = ReadIniInt("Display", "showCpuPower", 0) != 0;
    cfg.showGpuPower    = ReadIniInt("Display", "showGpuPower", 0) != 0;
    cfg.showCpuFan      = ReadIniInt("Display", "showCpuFan", 0) != 0;
    cfg.showGpuFan      = ReadIniInt("Display", "showGpuFan", 0) != 0;
    cfg.showCpuFreq     = ReadIniInt("Frequency", "showCpuFreq", 1) != 0;
    cfg.showGpuCoreFreq = ReadIniInt("Frequency", "showGpuCoreFreq", 1) != 0;
    ReadIniStr("Frequency", "cpuFreqPath", cfg.cpuFreqPath, sizeof(cfg.cpuFreqPath));
    ReadIniStr("Frequency", "gpuCoreFreqPath", cfg.gpuCoreFreqPath, sizeof(cfg.gpuCoreFreqPath));

    {
        int ls = ReadIniInt("Layout", "layoutStyle", -1);
        if (ls < 0)
            ls = ReadIniInt("Layout", "horizontal", 0) ? LAYOUT_HORIZONTAL : LAYOUT_VERTICAL;
        if (ls < LAYOUT_VERTICAL || ls > LAYOUT_STEAM)
            ls = LAYOUT_VERTICAL;
        cfg.layoutStyle = ls;
    }
    cfg.useFahrenheit = ReadIniInt("Layout", "useFahrenheit", 0) != 0;
    cfg.autoStart     = ReadIniInt("Layout", "autoStart", 0) != 0;
    {
        const int posVer = ReadIniInt("Layout", "positionVer", 0);
        int pos = ReadIniInt("Layout", "position", 0);
        if (posVer < 2) {
            static const int kLegacyToPos[] = { POS_TOP_LEFT, POS_TOP_RIGHT, POS_BOTTOM_LEFT, POS_BOTTOM_RIGHT };
            if (pos >= 0 && pos <= 3) pos = kLegacyToPos[pos];
            WriteIniInt("Layout", "positionVer", 2);
            WriteIniInt("Layout", "position", pos);
        }
        cfg.position = pos;
    }
    cfg.opacity       = ReadIniInt("Layout", "opacity", 85);
    cfg.textOpacity   = ReadIniInt("Layout", "textOpacity", 100);
    cfg.transparentBackground = ReadIniInt("Layout", "transparentBackground", 0) != 0;
    cfg.customX       = ReadIniFloat("Layout", "customX", -1.0f);
    cfg.customY       = ReadIniFloat("Layout", "customY", -1.0f);
    {
        int sc = ReadIniInt("Layout", "overlayScale", -1);
        if (sc < 0) sc = ReadIniInt("Layout", "steamBarScale", 125);
        cfg.overlayScale = sc;
    }

    ReadIniStr("App", "language", cfg.language, sizeof(cfg.language));
    if (cfg.language[0] == '\0') snprintf(cfg.language, sizeof(cfg.language), "en-US");
    cfg.settingsTab = ReadIniInt("App", "settingsTab", SETTINGS_TAB_DISPLAY);

    cfg.toggleKey   = ReadIniInt("Hotkeys", "toggleKey", VK_INSERT);
    cfg.exitKey     = ReadIniInt("Hotkeys", "exitKey", VK_END);
    cfg.selectedGpu = ReadIniInt("GPU", "selectedGpu", 0);

    if (cfg.position < POS_TOP_LEFT || cfg.position > POS_BOTTOM_RIGHT) cfg.position = POS_TOP_LEFT;
    if (cfg.opacity < 0) cfg.opacity = 0;
    if (cfg.opacity > 100) cfg.opacity = 100;
    if (cfg.textOpacity < 20) cfg.textOpacity = 20;
    if (cfg.textOpacity > 100) cfg.textOpacity = 100;
    if (cfg.selectedGpu < 0) cfg.selectedGpu = 0;
    if (cfg.layoutStyle < LAYOUT_VERTICAL || cfg.layoutStyle > LAYOUT_STEAM) cfg.layoutStyle = LAYOUT_VERTICAL;
    if (cfg.overlayScale < 50) cfg.overlayScale = 50;
    if (cfg.overlayScale > 200) cfg.overlayScale = 200;
    if (cfg.timeFormat != TIME_FORMAT_24H && cfg.timeFormat != TIME_FORMAT_12H) cfg.timeFormat = TIME_FORMAT_24H;
    if (cfg.settingsTab < 0 || cfg.settingsTab >= SETTINGS_TAB_COUNT) cfg.settingsTab = SETTINGS_TAB_DISPLAY;
}

static bool HasWelcomeBeenShown()
{
    InitConfigPath();
    return ReadIniInt("App", "welcomeShown", 0) != 0;
}

static void MarkWelcomeShown()
{
    InitConfigPath();
    WriteIniInt("App", "welcomeShown", 1);
}

static void ShowWelcomeMessage()
{
    if (HasWelcomeBeenShown()) return;
    MessageBoxW(
        nullptr,
        locale::ToWide(locale::DialogBody(
            "welcome",
            "Welcome to FPS Overlay!\n\n"
            "For the best experience, it is recommended to disable other FPS overlays:\n\n"
            "  - Steam Overlay (Steam > Settings > In-Game)\n"
            "  - Xbox Game Bar (Windows Settings > Gaming)\n"
            "  - NVIDIA GeForce Experience Overlay/NVIDIA ShadowPlay/NVIDIA App\n"
            "  - AMD Radeon Software Overlay\n"
            "  - Discord Overlay\n\n"
            "This prevents conflicts and ensures accurate FPS readings.\n\n"
            "Enjoy!")).c_str(),
        locale::ToWide(locale::DialogTitle("welcome", "FPS Overlay")).c_str(),
        MB_OK | MB_ICONINFORMATION | MB_TOPMOST
    );
    MarkWelcomeShown();
}

static void SaveConfig(const OverlayConfig& cfg)
{
    InitConfigPath();

    int pawnioRb = ReadIniInt("App", "PawnIORequiresReboot", 0);
    char pawnioHex[48] = {};
    ReadIniStr("App", "PawnIOInstallUtcHex", pawnioHex, sizeof(pawnioHex));
    char sidecarHex[48] = {};
    if (ReadPawnIORebootPendingStateFile(sidecarHex, sizeof(sidecarHex))) {
        pawnioRb = 1;
        snprintf(pawnioHex, sizeof(pawnioHex), "%s", sidecarHex);
    }

    WriteIniInt("Display", "showFPS", cfg.showFPS ? 1 : 0);
    WriteIniInt("Display", "show1PercentLow", cfg.show1PercentLow ? 1 : 0);
    WriteIniInt("Display", "showCpuUsage", cfg.showCpuUsage ? 1 : 0);
    WriteIniInt("Display", "showCpuTemp", cfg.showCpuTemp ? 1 : 0);
    WriteIniInt("Display", "showGpuUsage", cfg.showGpuUsage ? 1 : 0);
    WriteIniInt("Display", "showGpuTemp", cfg.showGpuTemp ? 1 : 0);
    WriteIniInt("Display", "showCPU", (cfg.showCpuUsage || cfg.showCpuTemp) ? 1 : 0);
    WriteIniInt("Display", "showGPU", (cfg.showGpuUsage || cfg.showGpuTemp) ? 1 : 0);
    WriteIniInt("Display", "showVRAM", cfg.showVRAM ? 1 : 0);
    WriteIniInt("Display", "showRAM", cfg.showRAM ? 1 : 0);
    WriteIniInt("Display", "showProcessName", 0);
    WriteIniInt("Display", "showTime", cfg.showTime ? 1 : 0);
    WriteIniInt("Display", "timeFormat", cfg.timeFormat);
    WriteIniInt("Display", "timeShowSeconds", cfg.timeShowSeconds ? 1 : 0);
    WriteIniInt("Display", "showCpuPower", cfg.showCpuPower ? 1 : 0);
    WriteIniInt("Display", "showGpuPower", cfg.showGpuPower ? 1 : 0);
    WriteIniInt("Display", "showCpuFan", cfg.showCpuFan ? 1 : 0);
    WriteIniInt("Display", "showGpuFan", cfg.showGpuFan ? 1 : 0);

    WriteIniInt("Frequency", "showCpuFreq", cfg.showCpuFreq ? 1 : 0);
    WriteIniInt("Frequency", "showGpuCoreFreq", cfg.showGpuCoreFreq ? 1 : 0);
    WriteIniStr("Frequency", "cpuFreqPath", cfg.cpuFreqPath);
    WriteIniStr("Frequency", "gpuCoreFreqPath", cfg.gpuCoreFreqPath);

    WriteIniInt("Layout", "layoutStyle", cfg.layoutStyle);
    WriteIniInt("Layout", "horizontal", cfg.layoutStyle == LAYOUT_HORIZONTAL ? 1 : 0);
    WriteIniInt("Layout", "useFahrenheit", cfg.useFahrenheit ? 1 : 0);
    WriteIniInt("Layout", "autoStart", cfg.autoStart ? 1 : 0);
    WriteIniInt("Layout", "position", cfg.position);
    WriteIniInt("Layout", "positionVer", 2);
    WriteIniInt("Layout", "opacity", cfg.opacity);
    WriteIniInt("Layout", "textOpacity", cfg.textOpacity);
    WriteIniInt("Layout", "transparentBackground", cfg.transparentBackground ? 1 : 0);
    WriteIniFloat("Layout", "customX", cfg.customX);
    WriteIniFloat("Layout", "customY", cfg.customY);
    WriteIniInt("Layout", "overlayScale", cfg.overlayScale);

    WriteIniInt("Hotkeys", "toggleKey", cfg.toggleKey);
    WriteIniInt("Hotkeys", "exitKey", cfg.exitKey);
    WriteIniInt("GPU", "selectedGpu", cfg.selectedGpu);

    WriteIniStr("App", "language", cfg.language);
    WriteIniInt("App", "settingsTab", cfg.settingsTab);

    if (pawnioRb != 0) {
        WriteIniInt("App", "PawnIORequiresReboot", 1);
        if (pawnioHex[0] != '\0')
            WriteIniStr("App", "PawnIOInstallUtcHex", pawnioHex);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// App state
// ═══════════════════════════════════════════════════════════════════════════
enum AppMode    { MODE_CONFIG, MODE_OVERLAY };
enum PendingCmd { CMD_NONE, CMD_START_OVERLAY, CMD_SHOW_SETTINGS, CMD_EXIT };

static OverlayConfig g_Config;
static AppMode       g_Mode       = MODE_CONFIG;
static PendingCmd    g_Pending    = CMD_NONE;
static bool          g_Running    = true;
static bool          g_OvlVisible = true;

static ImVec4 OvCol(float r, float g, float b, float a = 1.f)
{
    (void)r; (void)g; (void)b;
    const float ta = (g_Config.textOpacity / 100.f) * a;
    // Fixed overlay colour: #00E5FF.
    return ImVec4(0.f, 229.f / 255.f, 1.f, ta);
}

static ImVec4 OvTempCol(float celsius, float warningThreshold)
{
    const float ta = g_Config.textOpacity / 100.f;
    if (celsius > warningThreshold)
        return ImVec4(1.f, 1.f, 0.f, ta); // Yellow only above the requested limit.
    return ImVec4(0.f, 229.f / 255.f, 1.f, ta);
}

static ImVec4 OvColV(const ImVec4& c)
{
    return OvCol(c.x, c.y, c.z, c.w);
}

static void AppendMenuUtf8(HMENU m, UINT flags, UINT_PTR id, const char* utf8)
{
    const std::wstring w = locale::ToWide(utf8);
    AppendMenuW(m, flags, id, w.c_str());
}

static float OverlayBgAlpha(bool ctrlHeld)
{
    if (g_Config.transparentBackground)
        return ctrlHeld ? 0.35f : 0.f;
    const float a = g_Config.opacity / 100.f;
    return ctrlHeld ? (a < 0.55f ? 0.55f : a) : a;
}

static bool g_fontsNeedReload = false;

static void LoadAppFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontGlyphRangesBuilder glyphBuilder;
    glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    glyphBuilder.AddChar((ImWchar)0x2122);
    glyphBuilder.AddChar((ImWchar)0x00A9);
    glyphBuilder.AddChar((ImWchar)0x00AE);
    {
        static const ImWchar kLatinExtA[] = { 0x0100, 0x017F, 0 };
        glyphBuilder.AddRanges(kLatinExtA);
    }
    const bool wantCjkZh = (_strnicmp(g_Config.language, "zh", 2) == 0);
    const bool wantCjkJa = (_strnicmp(g_Config.language, "ja", 2) == 0);
    const bool wantCjkKo = (_strnicmp(g_Config.language, "ko", 2) == 0);
    const bool wantCyrillic =
        (_strnicmp(g_Config.language, "ru", 2) == 0) ||
        (_strnicmp(g_Config.language, "uk", 2) == 0) ||
        (_strnicmp(g_Config.language, "bg", 2) == 0);
    const bool wantRtlScript =
        (_strnicmp(g_Config.language, "ar", 2) == 0) ||
        (_strnicmp(g_Config.language, "fa", 2) == 0) ||
        (_strnicmp(g_Config.language, "ur", 2) == 0) ||
        (_strnicmp(g_Config.language, "he", 2) == 0) ||
        (_strnicmp(g_Config.language, "iw", 2) == 0);
    if (wantCjkZh) glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (wantCjkJa) glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
    if (wantCjkKo) glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesKorean());
    if (wantCyrillic) glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    if (wantRtlScript) {
        static const ImWchar kRtlRanges[] = {
            0x0590, 0x05FF, 0x0600, 0x06FF, 0x0750, 0x077F, 0x08A0, 0x08FF,
            0xFB1D, 0xFB4F, 0xFB50, 0xFDFF, 0xFE70, 0xFEFF, 0
        };
        glyphBuilder.AddRanges(kRtlRanges);
    }

    static ImVector<ImWchar> s_imguiGlyphRanges;
    s_imguiGlyphRanges.clear();
    glyphBuilder.BuildRanges(&s_imguiGlyphRanges);

    ImFont* font = nullptr;
    if (wantCjkZh) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr, s_imguiGlyphRanges.Data);
        if (!font) font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttf", 18.0f, nullptr, s_imguiGlyphRanges.Data);
        if (!font) font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 18.0f, nullptr, s_imguiGlyphRanges.Data);
    } else if (wantCjkJa) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msgothic.ttc", 18.0f, nullptr, s_imguiGlyphRanges.Data);
        if (!font) font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\YuGothM.ttc", 18.0f, nullptr, s_imguiGlyphRanges.Data);
        if (!font) font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\meiryo.ttc", 18.0f, nullptr, s_imguiGlyphRanges.Data);
    } else if (wantCjkKo) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 18.0f, nullptr, s_imguiGlyphRanges.Data);
    }
    if (!font)
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f, nullptr, s_imguiGlyphRanges.Data);
    if (!font && wantRtlScript) {
        font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 18.0f, nullptr, s_imguiGlyphRanges.Data);
        if (!font) font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 18.0f, nullptr, s_imguiGlyphRanges.Data);
    }
    if (!font) {
        ImFontConfig fc;
        fc.SizePixels = 17;
        io.Fonts->AddFontDefault(&fc);
    }
}

static void ApplyLanguage(const char* lang)
{
    if (!lang || !lang[0]) lang = "en-US";
    snprintf(g_Config.language, sizeof(g_Config.language), "%s", lang);
    locale::Load(g_Config.language);
    locale::ApplyImGuiRtlFlags();
    g_fontsNeedReload = true;
}

static HINSTANCE      g_hInstance = nullptr;
static HWND           g_hwnd     = nullptr;
static NOTIFYICONDATA g_nid      = {};
static RECT           g_overlayBounds = {0, 0, 0, 0};
static bool           g_isDragging = false;
static bool           g_overlayForceCornerSnap = false;

static char g_cpuName[256] = "Unknown";
static char g_gpuName[256] = "Unknown";

static std::atomic<bool> g_updateAvailable{false};
static std::atomic<bool> g_updateCheckDone{false};
static char g_latestVersion[32] = "";

static void CheckForUpdatesAsync()
{
    std::thread([]() {
        HINTERNET hSession = WinHttpOpen(L"FPS-Overlay/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { g_updateCheckDone = true; return; }
        HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); g_updateCheckDone = true; return; }
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/repos/aneeskhan47/fps-overlay/releases/latest", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); g_updateCheckDone = true; return; }
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); g_updateCheckDone = true; return;
        }
        std::string response;
        DWORD bytesRead = 0;
        char buffer[4096];
        while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        const char* tagKey = "\"tag_name\"";
        size_t pos = response.find(tagKey);
        if (pos != std::string::npos) {
            pos = response.find(':', pos);
            if (pos != std::string::npos) {
                size_t start = response.find('"', pos + 1);
                size_t end = response.find('"', start + 1);
                if (start != std::string::npos && end != std::string::npos && end > start) {
                    std::string latestTag = response.substr(start + 1, end - start - 1);
                    snprintf(g_latestVersion, sizeof(g_latestVersion), "%s", latestTag.c_str());
                    if (strcmp(g_latestVersion, APP_VERSION) != 0) g_updateAvailable = true;
                }
            }
        }
        g_updateCheckDone = true;
    }).detach();
}

static float g_gpuUsage = 0.0f;
static float g_gpuTemp  = 0.0f;
static float g_vramUsed  = 0.0f;
static float g_vramTotal = 0.0f;

// ── ETW state (Current FPS + 1% Low FPS) ──
static TRACEHANDLE      g_etwSession = 0;
static TRACEHANDLE      g_etwTrace   = 0;
static std::thread      g_etwThread;
static std::atomic<bool>  g_etwRunning{false};
static std::atomic<float> g_gameFps{0.0f};
static std::atomic<float> g_game1PercentLow{0.0f};
static std::atomic<DWORD> g_targetPid{0};
static DWORD              g_lastTargetPid = 0;
static bool               g_etwAvailable = false;
static bool               g_isAdmin = false;
static double              g_qpcFreq     = 1.0;
static char               g_targetProcessName[768] = "";

static float g_cpuTemp = 0.0f;
static bool  g_cpuTempAvailable = false;

static bool  g_lhwmAvailable = false;
static std::string g_lhwmCpuTempPath;
static std::string g_lhwmGpuTempPath;
static std::string g_lhwmGpuLoadPath;
static std::string g_lhwmGpuVramUsedPath;
static std::string g_lhwmGpuVramTotalPath;
static std::string g_lhwmCpuPowerPath;
static std::string g_lhwmGpuPowerPath;
static std::string g_lhwmCpuFanPath;
static std::string g_lhwmGpuFanPath;
static float g_lhwmCpuTemp = 0.0f;
static float g_cpuPower = 0.0f;
static float g_gpuPower = 0.0f;
static float g_cpuFanRpm = 0.0f;
static float g_gpuFanRpm = 0.0f;

// CPU / GPU core clock options and live values (MHz) — clean numeric display, no sparklines
static std::vector<std::pair<std::string, std::string>> g_cpuClockOpts;
static float g_cpuClockMHz = 0.f;
static float g_gpuCoreClockMHz = 0.f;

static bool FreqPathValid(const char* p, const std::vector<std::pair<std::string, std::string>>& opts)
{
    if (!p || !p[0]) return false;
    for (const auto& e : opts)
        if (e.second == p) return true;
    return false;
}

static void ValidateFrequencyPaths()
{
    if (!FreqPathValid(g_Config.cpuFreqPath, g_cpuClockOpts)) {
        if (!g_cpuClockOpts.empty())
            snprintf(g_Config.cpuFreqPath, sizeof(g_Config.cpuFreqPath), "%s", g_cpuClockOpts[0].second.c_str());
        else
            g_Config.cpuFreqPath[0] = '\0';
    }
    if (g_gpuCount > 0 && g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount) {
        GpuInfo& g = g_gpuList[g_Config.selectedGpu];
        if (!FreqPathValid(g_Config.gpuCoreFreqPath, g.coreClockOpts)) {
            if (!g.coreClockOpts.empty())
                snprintf(g_Config.gpuCoreFreqPath, sizeof(g_Config.gpuCoreFreqPath), "%s", g.coreClockOpts[0].second.c_str());
            else
                g_Config.gpuCoreFreqPath[0] = '\0';
        }
    } else {
        g_Config.gpuCoreFreqPath[0] = '\0';
    }
}

static void PollClockSensors()
{
    if (!g_lhwmAvailable) return;
    try {
        if (g_Config.showCpuFreq && g_Config.cpuFreqPath[0])
            g_cpuClockMHz = LHWM::GetSensorValue(std::string(g_Config.cpuFreqPath));
        else
            g_cpuClockMHz = 0.f;

        if (g_Config.showGpuCoreFreq && g_Config.gpuCoreFreqPath[0])
            g_gpuCoreClockMHz = LHWM::GetSensorValue(std::string(g_Config.gpuCoreFreqPath));
        else
            g_gpuCoreClockMHz = 0.f;
    } catch (...) {
    }
}

// Clean numeric MHz readout without sparkline graph
static void DrawCleanFreqMHz(float mhz, const ImVec4& col = ImVec4(0.f, 229.f / 255.f, 1.f, 1.f))
{
    if (mhz > 0.f)
        ImGui::TextColored(OvColV(col), "%s", locale::TF("%.0f MHz", mhz));
    else
        ImGui::TextColored(OvCol(.45f, .45f, .50f, 1.f), "%s", locale::T("--- MHz"));
}

// ── DX11 ──
static ID3D11Device*           g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext  = nullptr;
static IDXGISwapChain*         g_pSwapChain        = nullptr;
static ID3D11RenderTargetView* g_pRenderTargetView = nullptr;

static ID3D11ShaderResourceView* g_texGitHub = nullptr;
static ID3D11ShaderResourceView* g_texKofi   = nullptr;

static bool LoadTextureFromMemory(const unsigned char* data, size_t dataSize, ID3D11ShaderResourceView** outSrv)
{
    if (!data || dataSize == 0 || !g_pd3dDevice || !outSrv) return false;
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;
    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) { factory->Release(); return false; }
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)dataSize);
    if (FAILED(hr)) { stream->Release(); factory->Release(); return false; }
    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    stream->Release();
    if (FAILED(hr)) { factory->Release(); return false; }
    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr)) { factory->Release(); return false; }
    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) { frame->Release(); factory->Release(); return false; }
    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);
    frame->Release();
    if (FAILED(hr)) { converter->Release(); factory->Release(); return false; }
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (w == 0 || h == 0) { converter->Release(); factory->Release(); return false; }
    std::vector<BYTE> pixels((size_t)w * (size_t)h * 4);
    hr = converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    converter->Release(); factory->Release();
    if (FAILED(hr)) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub = {};
    sub.pSysMem = pixels.data(); sub.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr;
    hr = g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex);
    if (FAILED(hr)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = g_pd3dDevice->CreateShaderResourceView(tex, &srvDesc, outSrv);
    tex->Release();
    return SUCCEEDED(hr);
}

static bool LoadTextureFromResource(int resId, ID3D11ShaderResourceView** outSrv)
{
    if (!g_hInstance || !outSrv) return false;
    HRSRC res = FindResource(g_hInstance, MAKEINTRESOURCE(resId), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(g_hInstance, res);
    if (!mem) return false;
    DWORD size = SizeofResource(g_hInstance, res);
    const void* data = LockResource(mem);
    if (!data || size == 0) return false;
    return LoadTextureFromMemory(static_cast<const unsigned char*>(data), size, outSrv);
}

static void InitHeaderIconTextures()
{
    if (!g_texGitHub) LoadTextureFromResource(IDR_GITHUB_ICON, &g_texGitHub);
    if (!g_texKofi)   LoadTextureFromResource(IDR_KOFI_ICON, &g_texKofi);
}

static void ReleaseHeaderIconTextures()
{
    if (g_texGitHub) { g_texGitHub->Release(); g_texGitHub = nullptr; }
    if (g_texKofi)   { g_texKofi->Release();   g_texKofi   = nullptr; }
}

static int g_listeningFor = 0;

bool    CreateDeviceD3D(HWND);
void    CleanupDeviceD3D();
void    CreateRenderTarget();
void    CleanupRenderTarget();
void    AddTrayIcon();
void    RemoveTrayIcon();
void    UpdateTrayTooltip();
void    SwitchToOverlay();
void    SwitchToConfig();
void    ShutdownBackends();
void    InitBackends();
void    ApplyStyle();
static float GetCpuUsage();

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static const char* GetKeyName(int vk)
{
    static char buf[64];
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    LONG lp = sc << 16;
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:
        case VK_LEFT:   case VK_RIGHT:  case VK_UP:   case VK_DOWN:
        case VK_NUMLOCK: case VK_SNAPSHOT: case VK_CANCEL:
            lp |= (1 << 24);
            break;
    }
    if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0) return buf;
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

static bool IsRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

static void QueryCpuName()
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD sz = sizeof(g_cpuName);
        RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr, reinterpret_cast<LPBYTE>(g_cpuName), &sz);
        RegCloseKey(hKey);
        char* p = g_cpuName;
        while (*p == ' ') p++;
        if (p != g_cpuName) memmove(g_cpuName, p, strlen(p) + 1);
    }
}

static void QueryGpuName()
{
    if (!g_pd3dDevice) return;
    IDXGIDevice* dxgiDev = nullptr;
    g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev));
    if (!dxgiDev) return;
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_gpuName, sizeof(g_gpuName), nullptr, nullptr);
        adapter->Release();
    }
    dxgiDev->Release();
}

static void WideToUtf8(const wchar_t* w, char* out, size_t outBytes)
{
    if (!out || outBytes == 0) return;
    out[0] = '\0';
    if (!w || !w[0]) return;
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, (int)outBytes, nullptr, nullptr);
    if (n <= 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outBytes, nullptr, nullptr);
}

static void GetFileDescriptionUtf8FromPathW(const wchar_t* filePathW, char* outDesc, size_t maxLen)
{
    outDesc[0] = '\0';
    if (!filePathW || !filePathW[0]) return;
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(filePathW, &dummy);
    if (size == 0) return;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(filePathW, 0, size, data.data())) return;
    struct LANGANDCODEPAGE { WORD wLanguage; WORD wCodePage; } *lpTranslate = nullptr;
    UINT cbTranslate = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate) || !lpTranslate || cbTranslate < sizeof(LANGANDCODEPAGE)) return;
    wchar_t subBlock[72];
    _snwprintf_s(subBlock, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\FileDescription", lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);
    wchar_t* description = nullptr;
    UINT descLen = 0;
    if (!VerQueryValueW(data.data(), subBlock, reinterpret_cast<LPVOID*>(&description), &descLen) || !description || !description[0]) return;
    WideToUtf8(description, outDesc, maxLen);
}

static void GetProcessName(DWORD pid, char* outName, size_t maxLen)
{
    outName[0] = '\0';
    if (pid == 0 || maxLen == 0) return;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return;
    wchar_t fullPathW[MAX_PATH] = {};
    DWORD dw = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, fullPathW, &dw)) {
        const wchar_t* slash = wcsrchr(fullPathW, L'\\');
        const wchar_t* exeW = slash ? (slash + 1) : fullPathW;
        char exeUtf8[320] = {};
        char descUtf8[512] = {};
        WideToUtf8(exeW, exeUtf8, sizeof(exeUtf8));
        GetFileDescriptionUtf8FromPathW(fullPathW, descUtf8, sizeof(descUtf8));
        if (descUtf8[0]) snprintf(outName, maxLen, "%s (%s)", exeUtf8, descUtf8);
        else snprintf(outName, maxLen, "%s", exeUtf8);
    }
    CloseHandle(hProc);
}

static IWbemLocator*   g_pWbemLocator  = nullptr;
static IWbemServices*  g_pWbemServices = nullptr;
static bool            g_wmiInitialized = false;

static bool InitWMI()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&g_pWbemLocator));
    if (FAILED(hr)) return false;
    hr = g_pWbemLocator->ConnectServer(_bstr_t(L"ROOT\\OpenHardwareMonitor"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_pWbemServices);
    if (FAILED(hr)) hr = g_pWbemLocator->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_pWbemServices);
    if (FAILED(hr)) { g_pWbemLocator->Release(); g_pWbemLocator = nullptr; return false; }
    CoSetProxyBlanket(g_pWbemServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    g_wmiInitialized = true;
    return true;
}

static void ShutdownWMI()
{
    if (g_pWbemServices) { g_pWbemServices->Release(); g_pWbemServices = nullptr; }
    if (g_pWbemLocator)  { g_pWbemLocator->Release();  g_pWbemLocator  = nullptr; }
    g_wmiInitialized = false;
}

static float QueryCpuTemperature()
{
    if (!g_wmiInitialized || !g_pWbemServices) return 0.0f;
    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hr = g_pWbemServices->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND Name LIKE '%CPU%'"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator);
    if (FAILED(hr)) hr = g_pWbemServices->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator);
    if (FAILED(hr)) return 0.0f;
    float temp = 0.0f;
    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    if (pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && returned > 0) {
        VARIANT vtProp;
        VariantInit(&vtProp);
        hr = pObj->Get(L"Value", 0, &vtProp, nullptr, nullptr);
        if (SUCCEEDED(hr) && vtProp.vt == VT_R4) temp = vtProp.fltVal;
        else {
            VariantClear(&vtProp);
            hr = pObj->Get(L"CurrentTemperature", 0, &vtProp, nullptr, nullptr);
            if (SUCCEEDED(hr) && (vtProp.vt == VT_I4 || vtProp.vt == VT_UI4)) temp = (vtProp.lVal / 10.0f) - 273.15f;
        }
        VariantClear(&vtProp);
        pObj->Release();
    }
    pEnumerator->Release();
    return temp;
}

// ═══════════════════════════════════════════════════════════════════════════
// PawnIO Driver Checks
// ═══════════════════════════════════════════════════════════════════════════
static HKEY OpenPawnIOUninstallKeyRead()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO", 0, KEY_READ, &hKey) == ERROR_SUCCESS) return hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) return hKey;
    return nullptr;
}

static bool RegQuerySzA(HKEY hKey, const char* valueName, char* buf, DWORD cap)
{
    if (!cap) return false;
    DWORD sz = cap, typ = 0;
    if (RegQueryValueExA(hKey, valueName, nullptr, &typ, reinterpret_cast<LPBYTE>(buf), &sz) != ERROR_SUCCESS) return false;
    if (typ != REG_SZ && typ != REG_EXPAND_SZ) return false;
    buf[cap - 1] = '\0';
    return buf[0] != '\0';
}

static bool IsPawnIOInstalled()
{
    HKEY hKey = OpenPawnIOUninstallKeyRead();
    if (!hKey) return false;
    char versionStr[64] = {0};
    bool ok = RegQuerySzA(hKey, "DisplayVersion", versionStr, sizeof(versionStr));
    RegCloseKey(hKey);
    return ok;
}

static bool GetFileVersionQuad(const char* path, DWORD* verMS, DWORD* verLS)
{
    if (!path || !path[0] || !verMS || !verLS) return false;
    DWORD dummy = 0, verSize = GetFileVersionInfoSizeA(path, &dummy);
    if (!verSize) return false;
    std::vector<BYTE> data(verSize);
    if (!GetFileVersionInfoA(path, 0, verSize, data.data())) return false;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<void**>(&ffi), &ffiLen) || !ffi || ffiLen < sizeof(VS_FIXEDFILEINFO)) return false;
    *verMS = ffi->dwFileVersionMS;
    *verLS = ffi->dwFileVersionLS;
    return (*verMS | *verLS) != 0;
}

static bool WriteEmbeddedPawnIOSetupToPath(const char* destPath)
{
    HMODULE hModule = GetModuleHandle(nullptr);
    HRSRC hResource = FindResource(hModule, MAKEINTRESOURCE(IDR_PAWNIO_SETUP), RT_RCDATA);
    if (!hResource) return false;
    HGLOBAL hLoadedResource = LoadResource(hModule, hResource);
    if (!hLoadedResource) return false;
    LPVOID pResourceData = LockResource(hLoadedResource);
    DWORD dwResourceSize = SizeofResource(hModule, hResource);
    if (!pResourceData || dwResourceSize == 0) return false;
    HANDLE hFile = CreateFileA(destPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD bytesWritten = 0;
    BOOL writeResult = WriteFile(hFile, pResourceData, dwResourceSize, &bytesWritten, nullptr);
    CloseHandle(hFile);
    if (!writeResult || bytesWritten != dwResourceSize) { DeleteFileA(destPath); return false; }
    return true;
}

static bool GetBundledPawnIOSetupVersion(DWORD* verMS, DWORD* verLS)
{
    char tempPath[MAX_PATH], tempFile[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) == 0) return false;
    snprintf(tempFile, MAX_PATH, "%sFPSOverlay_PawnIO_setup_%llu.exe", tempPath, (unsigned long long)GetTickCount64());
    if (!WriteEmbeddedPawnIOSetupToPath(tempFile)) return false;
    bool ok = GetFileVersionQuad(tempFile, verMS, verLS);
    DeleteFileA(tempFile);
    return ok;
}

static void StripPathFromDisplayIcon(const char* raw, char* out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    if (!raw || !raw[0]) return;
    const char* p = raw;
    if (*p == '"') {
        ++p;
        const char* q = strchr(p, '"');
        if (q) { size_t n = (size_t)(q - p); if (n >= cap) n = cap - 1; memcpy(out, p, n); out[n] = '\0'; }
        return;
    }
    const char* comma = strchr(p, ',');
    size_t n = comma ? (size_t)(comma - p) : strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void ExtractFirstQuotedPath(const char* raw, char* out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    if (!raw || !raw[0]) return;
    while (*raw == ' ' || *raw == '\t') ++raw;
    if (*raw == '"') { StripPathFromDisplayIcon(raw, out, cap); return; }
    const char* sp = raw;
    const char* end = raw;
    while (*end && *end != ' ' && *end != '\t') ++end;
    size_t n = (size_t)(end - sp);
    if (n >= cap) n = cap - 1;
    memcpy(out, sp, n);
    out[n] = '\0';
}

static bool PathFileExistsA_(const char* p) { return p && p[0] && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static bool TryJoinExe(char* out, size_t cap, const char* dir, const char* exeName)
{
    if (!dir || !dir[0]) return false;
    size_t len = strlen(dir);
    while (len > 0 && (dir[len - 1] == '\\' || dir[len - 1] == '/')) len--;
    int n = snprintf(out, cap, "%.*s\\%s", (int)len, dir, exeName);
    return n > 0 && (size_t)n < cap && PathFileExistsA_(out);
}

static bool ParseDisplayVersionToQuad(const char* s, DWORD* verMS, DWORD* verLS)
{
    if (!s || !verMS || !verLS) return false;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == 'v' || *s == 'V') ++s;
    int v[4] = {0, 0, 0, 0}, n = 0;
    const char* p = s;
    while (n < 4 && *p) {
        char* end = nullptr;
        long x = strtol(p, &end, 10);
        if (end == p) break;
        v[n++] = (int)x;
        p = end;
        if (*p == '.' || *p == ',') ++p;
        else if (*p == '-' || *p == '+') break;
        else if (*p && (*p < '0' || *p > '9')) break;
    }
    if (n == 0) return false;
    *verMS = ((DWORD)(WORD)(n >= 1 ? v[0] : 0) << 16) | (DWORD)(WORD)(n >= 2 ? v[1] : 0);
    *verLS = ((DWORD)(WORD)(n >= 3 ? v[2] : 0) << 16) | (DWORD)(WORD)(n >= 4 ? v[3] : 0);
    return true;
}

static bool GetInstalledPawnIOVersionQuad(DWORD* verMS, DWORD* verLS)
{
    if (!verMS || !verLS) return false;
    *verMS = *verLS = 0;
    HKEY hKey = OpenPawnIOUninstallKeyRead();
    if (!hKey) return false;
    char buf512[512], path[MAX_PATH];
    auto tryPath = [&](const char* candidate) -> bool {
        if (!candidate || !candidate[0] || !PathFileExistsA_(candidate)) return false;
        return GetFileVersionQuad(candidate, verMS, verLS);
    };
    bool got = false;
    if (RegQuerySzA(hKey, "DisplayIcon", buf512, sizeof(buf512))) {
        StripPathFromDisplayIcon(buf512, path, sizeof(path));
        got = tryPath(path);
    }
    if (!got) {
        DWORD typ = 0, sz = sizeof(buf512);
        if (RegQueryValueExA(hKey, "InstallLocation", nullptr, &typ, reinterpret_cast<LPBYTE>(buf512), &sz) == ERROR_SUCCESS &&
            (typ == REG_SZ || typ == REG_EXPAND_SZ) && buf512[0]) {
            buf512[sizeof(buf512) - 1] = '\0';
            if (typ == REG_EXPAND_SZ) {
                char expanded[512];
                if (ExpandEnvironmentStringsA(buf512, expanded, sizeof(expanded)) > 1) snprintf(buf512, sizeof(buf512), "%s", expanded);
            }
            if (TryJoinExe(path, sizeof(path), buf512, "PawnIO.exe") && tryPath(path)) got = true;
            if (!got && TryJoinExe(path, sizeof(path), buf512, "PawnIO_setup.exe") && tryPath(path)) got = true;
        }
    }
    if (!got && RegQuerySzA(hKey, "UninstallString", buf512, sizeof(buf512))) {
        ExtractFirstQuotedPath(buf512, path, sizeof(path));
        if (!got && tryPath(path)) got = true;
    }
    if (!got) got = tryPath("C:\\Windows\\System32\\drivers\\PawnIO.sys");
    char disp[64] = {0};
    if (!got && RegQuerySzA(hKey, "DisplayVersion", disp, sizeof(disp))) got = ParseDisplayVersionToQuad(disp, verMS, verLS);
    RegCloseKey(hKey);
    return got && (*verMS != 0 || *verLS != 0);
}

static int CompareFileVersionQuad(DWORD aMS, DWORD aLS, DWORD bMS, DWORD bLS)
{
    if (aMS != bMS) return (aMS > bMS) ? 1 : -1;
    if (aLS != bLS) return (aLS > bLS) ? 1 : -1;
    return 0;
}

static bool CimDateTimeStringToFileTimeUtc(const wchar_t* wsz, FILETIME* pft)
{
    if (!wsz || !pft) return false;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (swscanf_s(wsz, L"%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6) return false;
    int biasMin = 0;
    const wchar_t* dot = wcschr(wsz, L'.');
    if (dot) {
        const wchar_t* p = dot + 1;
        while (*p && iswdigit(*p)) ++p;
        if (*p == L'+' || *p == L'-') biasMin = _wtoi(p);
    } else {
        const wchar_t* q = wsz + 14;
        while (*q && *q != L'+' && *q != L'-') ++q;
        if (*q == L'+' || *q == L'-') biasMin = _wtoi(q);
    }
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    time_t tt = _mkgmtime(&t);
    if (tt == (time_t)-1) return false;
    tt -= (time_t)biasMin * 60;
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)(tt + 11644473600LL) * 10000000ULL;
    *pft = *(FILETIME*)&u;
    return true;
}

static bool WmiQueryLastBootUtcFileTime(FILETIME* pft)
{
    if (!pft) return false;
    pft->dwLowDateTime = pft->dwHighDateTime = 0;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comNeedsUninit = SUCCEEDED(hrInit);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) return false;
    IWbemLocator* pLoc = nullptr; IWbemServices* pSvc = nullptr; IEnumWbemClassObject* pEnum = nullptr; IWbemClassObject* pObj = nullptr;
    VARIANT vt; VariantInit(&vt); ULONG ret = 0; bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&pLoc));
    if (FAILED(hr) || !pLoc) goto wmi_cleanup;
    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pSvc);
    if (FAILED(hr) || !pSvc) goto wmi_cleanup;
    hr = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) goto wmi_cleanup;
    hr = pSvc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT LastBootUpTime FROM Win32_OperatingSystem"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    if (FAILED(hr) || !pEnum) goto wmi_cleanup;
    if (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) != S_OK || !pObj) goto wmi_cleanup;
    hr = pObj->Get(L"LastBootUpTime", 0, &vt, nullptr, nullptr);
    if (FAILED(hr)) goto wmi_cleanup;
    if (vt.vt == VT_BSTR && vt.bstrVal) ok = CimDateTimeStringToFileTimeUtc(vt.bstrVal, pft);
wmi_cleanup:
    VariantClear(&vt);
    if (pObj) pObj->Release(); if (pEnum) pEnum->Release(); if (pSvc) pSvc->Release(); if (pLoc) pLoc->Release();
    if (comNeedsUninit) CoUninitialize();
    return ok;
}

static bool CommitPawnIORebootPendingToIni()
{
    InitConfigPath();
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    char hex[20];
    snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)u.QuadPart);
    for (int attempt = 0; attempt < 12; attempt++) {
        if (!WritePawnIORebootPendingStateFile(hex)) { Sleep(30); continue; }
        WriteIniStr("App", "PawnIOInstallUtcHex", hex);
        WriteIniInt("App", "PawnIORequiresReboot", 1);
        char verifyFile[48] = {};
        if (ReadPawnIORebootPendingStateFile(verifyFile, sizeof(verifyFile)) && (_stricmp(verifyFile, hex) == 0)) return true;
        Sleep(30);
    }
    return false;
}

static void ClearPawnIORebootPendingAll()
{
    InitConfigPath();
    WriteIniInt("App", "PawnIORequiresReboot", 0);
    WriteIniStr("App", "PawnIOInstallUtcHex", "");
    DeletePawnIORebootPendingStateFile();
}

static bool ApproxLastBootFromUptimeFileTime(FILETIME* pft)
{
    if (!pft) return false;
    FILETIME nowFt = {};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime; now.HighPart = nowFt.dwHighDateTime;
    const ULONGLONG uptime100ns = GetTickCount64() * 10000ULL;
    if (uptime100ns == 0 || now.QuadPart <= uptime100ns) return false;
    ULARGE_INTEGER boot;
    boot.QuadPart = now.QuadPart - uptime100ns;
    pft->dwLowDateTime = boot.LowPart; pft->dwHighDateTime = boot.HighPart;
    return true;
}

static bool LastBootUtcPlausiblyAfterPawnIOInstall(const FILETIME* bootFt, const FILETIME* markerFt, ULONGLONG minSlack100ns)
{
    ULARGE_INTEGER b, m;
    b.LowPart = bootFt->dwLowDateTime; b.HighPart = bootFt->dwHighDateTime;
    m.LowPart = markerFt->dwLowDateTime; m.HighPart = markerFt->dwHighDateTime;
    if (b.QuadPart <= m.QuadPart) return false;
    return (b.QuadPart - m.QuadPart) >= minSlack100ns;
}

static bool ReadPawnIOInstallMarkerFileTime(FILETIME* pft)
{
    if (!pft) return false;
    char hex[32] = {};
    ReadIniStr("App", "PawnIOInstallUtcHex", hex, sizeof(hex));
    if (strlen(hex) != 16) return false;
    unsigned long long v = 0;
    if (sscanf_s(hex, "%llx", &v) != 1) return false;
    ULARGE_INTEGER u;
    u.QuadPart = v;
    pft->dwLowDateTime = u.LowPart; pft->dwHighDateTime = u.HighPart;
    return true;
}

static bool AcquireShutdownPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return false;
    TOKEN_PRIVILEGES tkp = {};
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(nullptr, "SeShutdownPrivilege", &tkp.Privileges[0].Luid)) { CloseHandle(hToken); return false; }
    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, nullptr);
    CloseHandle(hToken);
    return ok != FALSE;
}

static void ForceShowPawnIORestartRequiredDialogThenExit(const char* situationLeadId, const char* situationLeadEn)
{
    const UINT kMb = MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL;
    const std::wstring lead = locale::ToWide(locale::DialogBody(situationLeadId, situationLeadEn));
    const std::wstring tail = locale::ToWide(locale::DialogBody(
        "restart_required",
        "Important: save your work in other apps before restarting. Unsaved data may be lost.\n\n"
        "A full system restart is required before FPS Overlay can run.\n\n"
        "Yes \xE2\x80\x94 restart this PC now (FPS Overlay will close first)\n"
        "No \xE2\x80\x94 restart later (FPS Overlay will close; use Start \xE2\x86\x92 Power \xE2\x86\x92 Restart when ready)\n\n"));
    wchar_t body[2048];
    _snwprintf_s(body, _TRUNCATE, L"%s\n\n%s", lead.c_str(), tail.c_str());
    const int r = MessageBoxW(nullptr, body, locale::ToWide(locale::DialogTitle("restart_required", "FPS Overlay \xE2\x80\x94 Restart required")).c_str(), kMb);
    if (r == IDYES) {
        if (AcquireShutdownPrivilege())
            ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION | SHTDN_REASON_FLAG_PLANNED);
        MessageBoxW(nullptr, locale::ToWide(locale::DialogBody("restart_failed", "Could not start an automatic restart. Please restart your PC manually.")).c_str(),
                    locale::ToWide(locale::DialogTitle("restart_failed", "FPS Overlay")).c_str(), MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL);
    }
    ExitProcess(0);
}

static void CheckPawnIORebootGateOrExit()
{
    InitConfigPath();
    char hex[48] = {}, fileHex[48] = {};
    ReadIniStr("App", "PawnIOInstallUtcHex", hex, sizeof(hex));
    const bool fileOk = ReadPawnIORebootPendingStateFile(fileHex, sizeof(fileHex));
    int rb = ReadIniInt("App", "PawnIORequiresReboot", 0);
    if (fileOk) {
        if (rb == 0 || _stricmp(hex, fileHex) != 0 || strlen(hex) != 16) {
            WriteIniStr("App", "PawnIOInstallUtcHex", fileHex);
            WriteIniInt("App", "PawnIORequiresReboot", 1);
        }
        snprintf(hex, sizeof(hex), "%s", fileHex);
        rb = 1;
    }
    if (rb == 0) return;
    FILETIME marker = {};
    if (!ReadPawnIOInstallMarkerFileTime(&marker)) {
        ForceShowPawnIORestartRequiredDialogThenExit("restart_marker_invalid", "FPS Overlay is waiting for a system restart after PawnIO was installed or updated.");
        return;
    }
    FILETIME bootApprox = {};
    if (ApproxLastBootFromUptimeFileTime(&bootApprox) && LastBootUtcPlausiblyAfterPawnIOInstall(&bootApprox, &marker, 3ULL * 10000000ULL)) {
        ClearPawnIORebootPendingAll();
        return;
    }
    FILETIME bootWmi = {};
    if (WmiQueryLastBootUtcFileTime(&bootWmi) && LastBootUtcPlausiblyAfterPawnIOInstall(&bootWmi, &marker, 45ULL * 10000000ULL)) {
        ClearPawnIORebootPendingAll();
        return;
    }
    ForceShowPawnIORestartRequiredDialogThenExit("must_restart_windows", "You must restart Windows before using FPS Overlay.");
}

static bool ExtractAndRunPawnIOSetup()
{
    char tempPath[MAX_PATH], tempFile[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) == 0) return false;
    snprintf(tempFile, MAX_PATH, "%sFPSOverlay_PawnIO_setup_run_%llu.exe", tempPath, (unsigned long long)GetTickCount64());
    if (!WriteEmbeddedPawnIOSetupToPath(tempFile)) return false;
    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas"; sei.lpFile = tempFile; sei.lpParameters = "-install"; sei.nShow = SW_HIDE; sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExA(&sei) || !sei.hProcess) { DeleteFileA(tempFile); return false; }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = (DWORD)-1;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    DeleteFileA(tempFile);
    return exitCode == 0;
}

static bool IsPawnIOOutdatedVsBundled()
{
    DWORD bundledMS = 0, bundledLS = 0, installedMS = 0, installedLS = 0;
    if (!GetBundledPawnIOSetupVersion(&bundledMS, &bundledLS)) return false;
    if (!GetInstalledPawnIOVersionQuad(&installedMS, &installedLS)) return false;
    return CompareFileVersionQuad(installedMS, installedLS, bundledMS, bundledLS) < 0;
}

static void EnforcePawnIOOrExit()
{
    CheckPawnIORebootGateOrExit();
    for (;;) {
        if (!IsPawnIOInstalled()) {
            int r = MessageBoxW(nullptr, locale::ToWide(locale::DialogBody("pawnio_required", "The PawnIO driver is required for FPS Overlay. Click OK to install PawnIO.")).c_str(),
                                locale::ToWide(locale::DialogTitle("pawnio_required", "FPS Overlay")).c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST);
            if (r != IDOK) ExitProcess(1);
            if (!ExtractAndRunPawnIOSetup()) continue;
            CommitPawnIORebootPendingToIni();
            ForceShowPawnIORestartRequiredDialogThenExit("pawnio_installed_success", "PawnIO was installed or updated successfully.");
        }
        if (IsPawnIOOutdatedVsBundled()) {
            int r = MessageBoxW(nullptr, locale::ToWide(locale::DialogBody("pawnio_outdated", "Your PawnIO driver is older than bundled. Click OK to update now.")).c_str(),
                                locale::ToWide(locale::DialogTitle("pawnio_outdated", "FPS Overlay")).c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST);
            if (r != IDOK) ExitProcess(1);
            if (!ExtractAndRunPawnIOSetup()) continue;
            CommitPawnIORebootPendingToIni();
            ForceShowPawnIORestartRequiredDialogThenExit("pawnio_installed_success", "PawnIO was installed or updated successfully.");
        }
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// LibreHardwareMonitor (LHWM)
// ═══════════════════════════════════════════════════════════════════════════
static bool IsLhwmGpuHardwareNode(const std::string& name)
{
    if (name.find("GeForce") != std::string::npos || name.find("RTX") != std::string::npos ||
        name.find("GTX") != std::string::npos || name.find("Radeon RX") != std::string::npos ||
        name.find("Radeon Pro") != std::string::npos || name.find("NVIDIA") != std::string::npos) return true;
    if (name.find("Intel") != std::string::npos && name.find("Arc") != std::string::npos) return true;
    if (name.find("Radeon") != std::string::npos && name.find("Radeon RX") == std::string::npos && name.find("Radeon Pro") == std::string::npos) {
        if (name.find("Graphics") != std::string::npos || name.find("780M") != std::string::npos ||
            name.find("760M") != std::string::npos || name.find("680M") != std::string::npos) return true;
    }
    if (name.find("Intel") != std::string::npos && name.find("Graphics") != std::string::npos) {
        if (name.find("UHD") != std::string::npos || name.find("HD Graphics") != std::string::npos || name.find("Iris") != std::string::npos) return true;
    }
    return false;
}

static int FindGpuByName(const char* name) {
    for (int i = 0; i < g_gpuCount; i++) if (strcmp(g_gpuList[i].name, name) == 0) return i;
    return -1;
}

static bool IsGpuMemoryClockSensor(const std::string& name)
{
    std::string n;
    for (char c : name) n += (char)tolower((unsigned char)c);
    return (n.find("memory") != std::string::npos && n.find("clock") != std::string::npos) ||
           (n.find("hbm") != std::string::npos) ||
           (n.find("vram") != std::string::npos && n.find("clock") != std::string::npos);
}

static int VramTotalSensorPriority(const std::string& sensorName)
{
    std::string n;
    for (char c : sensorName) n += (char)tolower((unsigned char)c);
    if (n.find("shared") != std::string::npos) return -1;
    if (n.find("gpu memory total") != std::string::npos) return 100;
    if (n.find("dedicated memory total") != std::string::npos || n.find("d3d dedicated") != std::string::npos) return 80;
    if (n.find("memory total") != std::string::npos) return 50;
    return -1;
}

static bool IsAmdSocCpuTempSensorName(const std::string& sensorName)
{
    std::string n;
    for (char c : sensorName) n += (char)tolower((unsigned char)c);
    if (n.find("socket") != std::string::npos) return false;
    return n.find("soc") != std::string::npos;
}

static int CpuTempFallbackPriority(const std::string& sensorName)
{
    std::string n;
    for (char c : sensorName) n += (char)tolower((unsigned char)c);
    bool hasCore = (n.find("core") != std::string::npos);
    bool hasTctl = (n.find("tctl") != std::string::npos);
    bool hasTdie = (n.find("tdie") != std::string::npos);
    bool hasCcd  = (n.find("ccd") != std::string::npos);
    if (hasCore && (hasTctl || hasTdie)) return 100;
    if (hasCcd) return 60;
    if (hasTctl || hasTdie) return 80;
    return 10;
}

static bool InitLHWM()
{
    try {
        auto sensors = LHWM::GetHardwareSensorMap();
        if (sensors.empty()) return false;
        g_cpuClockOpts.clear();
        for (int gi = 0; gi < MAX_GPUS; gi++) g_gpuList[gi].coreClockOpts.clear();
        std::string cpuTempFallbackPath;
        int cpuTempFallbackPri = -1;
        g_gpuCount = 0;

        for (const auto& [hardwareName, sensorList] : sensors) {
            bool isCpuHardware = (hardwareName.find("Ryzen") != std::string::npos || hardwareName.find("Intel") != std::string::npos ||
                                  hardwareName.find("CPU") != std::string::npos || hardwareName.find("Core") != std::string::npos);
            bool isGpuHardware = IsLhwmGpuHardwareNode(hardwareName);
            int gpuIndex = -1;
            if (isGpuHardware && g_gpuCount < MAX_GPUS) {
                std::string cleanName = hardwareName;
                size_t colonPos = cleanName.find(" : ");
                if (colonPos != std::string::npos) cleanName = cleanName.substr(0, colonPos);
                gpuIndex = FindGpuByName(cleanName.c_str());
                if (gpuIndex < 0) {
                    gpuIndex = g_gpuCount;
                    snprintf(g_gpuList[gpuIndex].name, sizeof(g_gpuList[gpuIndex].name), "%s", cleanName.c_str());
                    g_gpuList[gpuIndex].loadPathPri = -1;
                    g_gpuList[gpuIndex].vramTotalPri = -1;
                    g_gpuCount++;
                }
            }

            for (const auto& sensorInfo : sensorList) {
                const auto& [sensorName, sensorType, sensorPath] = sensorInfo;
                bool isCpuPath = (sensorPath.find("/amdcpu/") != std::string::npos || sensorPath.find("/intelcpu/") != std::string::npos);
                bool isGpuPath = (sensorPath.find("/gpu-nvidia/") != std::string::npos || sensorPath.find("/gpu-amd/") != std::string::npos || sensorPath.find("/gpu-intel/") != std::string::npos);

                if (sensorType == "Fan" && !isGpuPath && g_lhwmCpuFanPath.empty()) {
                    std::string nl;
                    for (char c : sensorName) nl += (char)tolower((unsigned char)c);
                    if (nl.find("cpu") != std::string::npos) g_lhwmCpuFanPath = sensorPath;
                }
                if ((isCpuHardware || isCpuPath) && !isGpuPath && sensorType == "Power") {
                    if (sensorName.find("Package") != std::string::npos || g_lhwmCpuPowerPath.empty()) g_lhwmCpuPowerPath = sensorPath;
                }
                if ((isCpuHardware || isCpuPath) && sensorType == "Temperature") {
                    if (IsAmdSocCpuTempSensorName(sensorName)) g_lhwmCpuTempPath = sensorPath;
                    else if (g_lhwmCpuTempPath.empty() && sensorName.find("Package") != std::string::npos) g_lhwmCpuTempPath = sensorPath;
                    else if (g_lhwmCpuTempPath.empty()) {
                        int p = CpuTempFallbackPriority(sensorName);
                        if (p > cpuTempFallbackPri) { cpuTempFallbackPath = sensorPath; cpuTempFallbackPri = p; }
                    }
                }
                if (gpuIndex >= 0) {
                    if (sensorType == "Temperature") {
                        bool isHotSpot = (sensorName.find("Hot Spot") != std::string::npos || sensorName.find("Hotspot") != std::string::npos);
                        bool isGpuCore = (sensorName == "GPU Core" || sensorName.find("GPU Core") != std::string::npos);
                        if (isGpuCore && !isHotSpot) g_gpuList[gpuIndex].tempPath = sensorPath;
                        else if (g_gpuList[gpuIndex].tempPath.empty() && !isHotSpot) g_gpuList[gpuIndex].tempPath = sensorPath;
                    } else if (sensorType == "Load") {
                        int pri = (sensorName == "GPU Core") ? 2 : (sensorName.find("GPU Core") != std::string::npos) ? 1 : 0;
                        if (pri > g_gpuList[gpuIndex].loadPathPri) { g_gpuList[gpuIndex].loadPath = sensorPath; g_gpuList[gpuIndex].loadPathPri = pri; }
                    } else if (sensorType == "SmallData" || sensorType == "Data") {
                        if (sensorName.find("Memory Used") != std::string::npos || sensorName.find("GPU Memory Used") != std::string::npos) g_gpuList[gpuIndex].vramUsedPath = sensorPath;
                        else {
                            int totPri = VramTotalSensorPriority(sensorName);
                            if (totPri >= 0 && totPri > g_gpuList[gpuIndex].vramTotalPri) { g_gpuList[gpuIndex].vramTotalPath = sensorPath; g_gpuList[gpuIndex].vramTotalPri = totPri; }
                        }
                    } else if (sensorType == "Clock") {
                        if (IsGpuMemoryClockSensor(sensorName)) continue;
                        bool dup = false;
                        for (const auto& e : g_gpuList[gpuIndex].coreClockOpts) if (e.second == sensorPath) { dup = true; break; }
                        if (!dup) g_gpuList[gpuIndex].coreClockOpts.push_back({ sensorName, sensorPath });
                    } else if (sensorType == "Power") {
                        if (sensorName.find("Package") != std::string::npos || g_gpuList[gpuIndex].powerPath.empty()) g_gpuList[gpuIndex].powerPath = sensorPath;
                    } else if (sensorType == "Fan") {
                        if (sensorName == "GPU Fan" || g_gpuList[gpuIndex].fanPath.empty()) g_gpuList[gpuIndex].fanPath = sensorPath;
                    }
                }
                if (sensorType == "Clock" && (isCpuHardware || isCpuPath) && !isGpuPath) {
                    bool dup = false;
                    for (const auto& e : g_cpuClockOpts) if (e.second == sensorPath) { dup = true; break; }
                    if (!dup) g_cpuClockOpts.push_back({ sensorName, sensorPath });
                }
            }
        }
        if (g_lhwmCpuTempPath.empty() && !cpuTempFallbackPath.empty()) g_lhwmCpuTempPath = cpuTempFallbackPath;
        if (g_Config.selectedGpu >= g_gpuCount) g_Config.selectedGpu = 0;
        if (g_gpuCount > 0) {
            int idx = g_Config.selectedGpu;
            g_lhwmGpuTempPath = g_gpuList[idx].tempPath;
            g_lhwmGpuLoadPath = g_gpuList[idx].loadPath;
            g_lhwmGpuVramUsedPath = g_gpuList[idx].vramUsedPath;
            g_lhwmGpuVramTotalPath = g_gpuList[idx].vramTotalPath;
            g_lhwmGpuPowerPath = g_gpuList[idx].powerPath;
            g_lhwmGpuFanPath = g_gpuList[idx].fanPath;
            snprintf(g_gpuName, sizeof(g_gpuName), "%s", g_gpuList[idx].name);
        }
        ValidateFrequencyPaths();
        return !g_lhwmCpuTempPath.empty() || g_gpuCount > 0;
    } catch (...) { return false; }
}

static std::atomic<bool> g_lhwmInitFinished{false};
static void LhwmBackgroundInitThread()
{
    bool ok = false;
    try { ok = InitLHWM(); } catch (...) { ok = false; }
    g_lhwmAvailable = ok;
    g_lhwmInitFinished.store(true, std::memory_order_release);
}

static void PollLHWMStats()
{
    if (!g_lhwmAvailable) return;
    try {
        if (!g_lhwmCpuTempPath.empty()) g_lhwmCpuTemp = LHWM::GetSensorValue(g_lhwmCpuTempPath);
        if (!g_lhwmGpuTempPath.empty()) g_gpuTemp = LHWM::GetSensorValue(g_lhwmGpuTempPath);
        if (!g_lhwmGpuLoadPath.empty()) g_gpuUsage = LHWM::GetSensorValue(g_lhwmGpuLoadPath);
        if (!g_lhwmGpuVramUsedPath.empty()) g_vramUsed = LHWM::GetSensorValue(g_lhwmGpuVramUsedPath) / 1024.0f;
        if (!g_lhwmGpuVramTotalPath.empty()) g_vramTotal = LHWM::GetSensorValue(g_lhwmGpuVramTotalPath) / 1024.0f;
        if (!g_lhwmCpuPowerPath.empty()) g_cpuPower = LHWM::GetSensorValue(g_lhwmCpuPowerPath);
        if (!g_lhwmGpuPowerPath.empty()) g_gpuPower = LHWM::GetSensorValue(g_lhwmGpuPowerPath);
        if (!g_lhwmCpuFanPath.empty()) g_cpuFanRpm = LHWM::GetSensorValue(g_lhwmCpuFanPath);
        if (!g_lhwmGpuFanPath.empty()) g_gpuFanRpm = LHWM::GetSensorValue(g_lhwmGpuFanPath);
    } catch (...) {}
}

static void SelectGpu(int index)
{
    if (index < 0 || index >= g_gpuCount) return;
    g_Config.selectedGpu = index;
    g_lhwmGpuTempPath = g_gpuList[index].tempPath;
    g_lhwmGpuLoadPath = g_gpuList[index].loadPath;
    g_lhwmGpuVramUsedPath = g_gpuList[index].vramUsedPath;
    g_lhwmGpuVramTotalPath = g_gpuList[index].vramTotalPath;
    g_lhwmGpuPowerPath = g_gpuList[index].powerPath;
    g_lhwmGpuFanPath = g_gpuList[index].fanPath;
    snprintf(g_gpuName, sizeof(g_gpuName), "%s", g_gpuList[index].name);
    ValidateFrequencyPaths();
}

// ═══════════════════════════════════════════════════════════════════════════
// ETW — Current FPS & 1% Low FPS Capture
// ═══════════════════════════════════════════════════════════════════════════
static void WINAPI EtwCallback(PEVENT_RECORD pEvent)
{
    if (!g_etwRunning.load(std::memory_order_relaxed)) return;

    DWORD pid = pEvent->EventHeader.ProcessId;
    DWORD target = g_targetPid.load(std::memory_order_relaxed);
    if (target == 0 || pid != target) return;

    bool isValidPresentEvent = false;
    bool isDxgiEvent = false;
    bool isD3D9Event = false;
    bool isDxgKrnlOnlyEvent = false;

    if (memcmp(&pEvent->EventHeader.ProviderId, &DXGI_PROVIDER, sizeof(GUID)) == 0) {
        if (pEvent->EventHeader.EventDescriptor.Id == 42) {
            isValidPresentEvent = true;
            isDxgiEvent = true;
        }
    } else if (memcmp(&pEvent->EventHeader.ProviderId, &D3D9_PROVIDER, sizeof(GUID)) == 0) {
        if (pEvent->EventHeader.EventDescriptor.Id == 1) {
            isValidPresentEvent = true;
            isD3D9Event = true;
        }
    } else if (memcmp(&pEvent->EventHeader.ProviderId, &DXGKRNL_PROVIDER, sizeof(GUID)) == 0) {
        USHORT eventId = pEvent->EventHeader.EventDescriptor.Id;
        if (eventId == DXGKRNL_EVENT_PRESENT_INFO ||
            eventId == DXGKRNL_EVENT_FLIP_INFO ||
            eventId == DXGKRNL_EVENT_BLIT_INFO) {
            isValidPresentEvent = true;
            isDxgKrnlOnlyEvent = true;
        }
    }

    if (!isValidPresentEvent) return;

    double ts = (double)pEvent->EventHeader.TimeStamp.QuadPart / g_qpcFreq;

    static DWORD  s_lastPid      = 0;
    static double s_startTs      = 0.0;
    static double s_prevDxgiTs   = 0.0;
    static double s_prevD3D9Ts   = 0.0;
    static double s_prevDxgTs    = 0.0;
    static std::vector<double> s_dxgiDts;
    static std::vector<double> s_d3d9Dts;
    static std::vector<double> s_dxgDts;

    if (pid != s_lastPid) {
        s_lastPid = pid;
        s_startTs = ts;
        s_prevDxgiTs = s_prevD3D9Ts = s_prevDxgTs = 0.0;
        s_dxgiDts.clear();
        s_d3d9Dts.clear();
        s_dxgDts.clear();
        return;
    }

    if (isDxgiEvent) {
        if (s_prevDxgiTs > 0.0 && ts > s_prevDxgiTs)
            s_dxgiDts.push_back(ts - s_prevDxgiTs);
        s_prevDxgiTs = ts;
    } else if (isD3D9Event) {
        if (s_prevD3D9Ts > 0.0 && ts > s_prevD3D9Ts)
            s_d3d9Dts.push_back(ts - s_prevD3D9Ts);
        s_prevD3D9Ts = ts;
    } else if (isDxgKrnlOnlyEvent) {
        if (s_prevDxgTs > 0.0 && ts > s_prevDxgTs)
            s_dxgDts.push_back(ts - s_prevDxgTs);
        s_prevDxgTs = ts;
    }

    double elapsed = ts - s_startTs;
    if (elapsed >= 1.0) {
        const std::vector<double>* activeDts = nullptr;
        int frameCount = 0;

        if (!s_d3d9Dts.empty()) {
            activeDts = &s_d3d9Dts;
            frameCount = (int)s_d3d9Dts.size();
        } else if (!s_dxgiDts.empty()) {
            activeDts = &s_dxgiDts;
            frameCount = (int)s_dxgiDts.size();
        } else if (!s_dxgDts.empty()) {
            float potentialFps = (float)s_dxgDts.size() / (float)elapsed;
            if (potentialFps >= 20.0f) {
                activeDts = &s_dxgDts;
                frameCount = (int)s_dxgDts.size();
            }
        }

        if (activeDts && frameCount > 0) {
            float avgFps = (float)frameCount / (float)elapsed;
            g_gameFps.store(avgFps, std::memory_order_relaxed);

            // Calculate 1% Low FPS (99th percentile slowest frame time)
            std::vector<double> sorted = *activeDts;
            std::sort(sorted.begin(), sorted.end());
            size_t idx99 = (size_t)((double)(sorted.size() - 1) * 0.99);
            double dt99 = sorted[idx99];
            float onePercentLow = (dt99 > 0.0001) ? (float)(1.0 / dt99) : avgFps;
            if (onePercentLow > avgFps) onePercentLow = avgFps;
            g_game1PercentLow.store(onePercentLow, std::memory_order_relaxed);
        } else {
            g_gameFps.store(0.0f, std::memory_order_relaxed);
            g_game1PercentLow.store(0.0f, std::memory_order_relaxed);
        }

        s_dxgiDts.clear();
        s_d3d9Dts.clear();
        s_dxgDts.clear();
        s_startTs = ts;
    }
}

static bool StartEtwSession()
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = (double)freq.QuadPart;

    struct { EVENT_TRACE_PROPERTIES p; char name[256]; } buf;
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize = sizeof(buf);
    buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
    ControlTraceA(0, ETW_SESSION_NAME, &buf.p, EVENT_TRACE_CONTROL_STOP);

    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize    = sizeof(buf);
    buf.p.Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    buf.p.Wnode.ClientContext = 1;
    buf.p.LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    buf.p.LoggerNameOffset    = offsetof(decltype(buf), name);

    ULONG rc = StartTraceA(&g_etwSession, ETW_SESSION_NAME, &buf.p);
    if (rc != ERROR_SUCCESS) return false;

    rc = EnableTraceEx2(g_etwSession, &DXGI_PROVIDER, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        ZeroMemory(&buf, sizeof(buf));
        buf.p.Wnode.BufferSize = sizeof(buf);
        buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
        ControlTraceA(g_etwSession, nullptr, &buf.p, EVENT_TRACE_CONTROL_STOP);
        g_etwSession = 0;
        return false;
    }

    EnableTraceEx2(g_etwSession, &D3D9_PROVIDER, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    EnableTraceEx2(g_etwSession, &DXGKRNL_PROVIDER, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, DXGKRNL_KEYWORD_PRESENT | DXGKRNL_KEYWORD_BASE, 0, 0, nullptr);

    EVENT_TRACE_LOGFILEA logFile = {};
    logFile.LoggerName          = const_cast<LPSTR>(ETW_SESSION_NAME);
    logFile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = EtwCallback;

    g_etwTrace = OpenTraceA(&logFile);
    if (g_etwTrace == (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        ZeroMemory(&buf, sizeof(buf));
        buf.p.Wnode.BufferSize = sizeof(buf);
        buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
        ControlTraceA(g_etwSession, nullptr, &buf.p, EVENT_TRACE_CONTROL_STOP);
        g_etwSession = 0;
        return false;
    }

    g_etwRunning.store(true);
    g_etwThread = std::thread([]() {
        TRACEHANDLE h = g_etwTrace;
        ProcessTrace(&h, 1, nullptr, nullptr);
    });
    return true;
}

static void StopEtwSession()
{
    if (!g_etwRunning.load()) return;
    g_etwRunning.store(false);
    if (g_etwTrace != 0 && g_etwTrace != (TRACEHANDLE)INVALID_HANDLE_VALUE) { CloseTrace(g_etwTrace); g_etwTrace = 0; }
    if (g_etwThread.joinable()) g_etwThread.join();
    struct { EVENT_TRACE_PROPERTIES p; char name[256]; } buf;
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize = sizeof(buf);
    buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
    ControlTraceA(g_etwSession, ETW_SESSION_NAME, &buf.p, EVENT_TRACE_CONTROL_STOP);
    g_etwSession = 0;
    g_gameFps.store(0.0f);
    g_game1PercentLow.store(0.0f);
}

static float GetCpuUsage()
{
    static ULARGE_INTEGER sI = {}, sK = {}, sU = {};
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return 0;
    ULARGE_INTEGER i, k, u;
    i.LowPart = fi.dwLowDateTime; i.HighPart = fi.dwHighDateTime;
    k.LowPart = fk.dwLowDateTime; k.HighPart = fk.dwHighDateTime;
    u.LowPart = fu.dwLowDateTime; u.HighPart = fu.dwHighDateTime;
    ULONGLONG di = i.QuadPart - sI.QuadPart, dk = k.QuadPart - sK.QuadPart, du = u.QuadPart - sU.QuadPart;
    sI = i; sK = k; sU = u;
    ULONGLONG total = dk + du;
    return total ? (1.0f - (float)di / (float)total) * 100.0f : 0.0f;
}

void AddTrayIcon()
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpy(g_nid.szTip, locale::T("FPS Overlay"));
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() { Shell_NotifyIcon(NIM_DELETE, &g_nid); }

void UpdateTrayTooltip()
{
    if (g_updateAvailable) snprintf(g_nid.szTip, sizeof(g_nid.szTip), "%s", locale::TF("FPS Overlay - Update available! (%s)", g_latestVersion));
    else lstrcpy(g_nid.szTip, locale::T("FPS Overlay"));
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10; s.FrameRounding = 6; s.GrabRounding = 6;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(14, 10);
    s.FramePadding  = ImVec2(8, 5);
    s.ItemSpacing   = ImVec2(10, 8);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.08f,0.08f,0.10f,1);
    c[ImGuiCol_Border]           = ImVec4(0.25f,0.27f,0.32f,0.6f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.14f,0.14f,0.17f,1);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.20f,0.20f,0.24f,1);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.26f,0.26f,0.30f,1);
    c[ImGuiCol_CheckMark]        = ImVec4(0.f,229.f/255.f,1.f,1);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.f,229.f/255.f,1.f,1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.f,229.f/255.f,1.f,1);
    c[ImGuiCol_Button]           = ImVec4(0.16f,0.16f,0.20f,1);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.22f,0.22f,0.28f,1);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.28f,0.28f,0.34f,1);
    c[ImGuiCol_Separator]        = ImVec4(0.22f,0.24f,0.28f,1);
}

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Denominator = 1; sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &got, &g_pd3dDeviceContext)))
        return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain        = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice        = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* buf = nullptr;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buf));
    if (buf) { g_pd3dDevice->CreateRenderTargetView(buf, nullptr, &g_pRenderTargetView); buf->Release(); }
}

void CleanupRenderTarget()
{
    if (g_pRenderTargetView) { g_pRenderTargetView->Release(); g_pRenderTargetView = nullptr; }
}

void ShutdownBackends()
{
    ReleaseHeaderIconTextures();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    CleanupDeviceD3D();
}

void InitBackends()
{
    CreateDeviceD3D(g_hwnd);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    InitHeaderIconTextures();
}

static void SetClickThrough(bool enable)
{
    LONG_PTR style = GetWindowLongPtr(g_hwnd, GWL_EXSTYLE);
    if (enable) style |= WS_EX_TRANSPARENT;
    else style &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtr(g_hwnd, GWL_EXSTYLE, style);
}

void SwitchToOverlay()
{
    ShutdownBackends();
    DestroyWindow(g_hwnd);
    RECT wa;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = wa.right - wa.left, h = wa.bottom - wa.top;
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    g_hwnd = CreateWindowEx(exStyle, "FPSOverlay", "FPS Overlay", WS_POPUP, wa.left, wa.top, w, h, nullptr, nullptr, g_hInstance, nullptr);
    SetLayeredWindowAttributes(g_hwnd, RGB(0,0,0), 255, LWA_ALPHA);
    MARGINS m = { -1 }; DwmExtendFrameIntoClientArea(g_hwnd, &m);
    InitBackends();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    AddTrayIcon();
    g_etwAvailable = StartEtwSession();
    g_Mode = MODE_OVERLAY;
    g_OvlVisible = true;
}

void SwitchToConfig()
{
    StopEtwSession();
    RemoveTrayIcon();
    ShutdownBackends();
    DestroyWindow(g_hwnd);
    const int ch = 820;
    int cx = (GetSystemMetrics(SM_CXSCREEN) - kConfigDlgOuterW) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    g_hwnd = CreateWindowEx(0, "FPSOverlay", "FPS Overlay", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME, cx, cy, kConfigDlgOuterW, ch, nullptr, nullptr, g_hInstance, nullptr);
    InitBackends();
    ShowWindow(g_hwnd, SW_SHOW);
    g_Mode = MODE_CONFIG;
}

static void Present(float r, float g, float b, float a)
{
    ImGui::Render();
    const float c[4] = { r, g, b, a };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_pRenderTargetView, c);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(g_isDragging ? 0 : 1, 0);
}

static ImVec4 ColorByLoad(float v, float warn = 70, float crit = 90)
{
    (void)v; (void)warn; (void)crit;
    return ImVec4(0.f, 229.f / 255.f, 1.f, 1.f);
}

static ImVec4 ColorByFps(float fps)
{
    (void)fps;
    return ImVec4(0.f, 229.f / 255.f, 1.f, 1.f);
}

static void TooltipWrapped(const char* text)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 260.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

static void TooltipWrappedFmt(const char* fmt, ...)
{
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    TooltipWrapped(buf);
}

static void RtlAlignNext(float itemWidth)
{
    if (!locale::IsRtl()) return;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > itemWidth) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - itemWidth));
}

static void RtlSectionTitle(const char* text)
{
    const ImVec4 col(.55f, .70f, .95f, 1.f);
    if (locale::IsRtl()) RtlAlignNext(ImGui::CalcTextSize(text).x);
    ImGui::TextColored(col, "%s", text);
}

static void RtlMutedText(const char* text)
{
    if (locale::IsRtl()) RtlAlignNext(ImGui::CalcTextSize(text).x);
    ImGui::TextColored(ImVec4(.45f, .45f, .50f, 1), "%s", text);
}

static bool RtlRadioButton(const char* label, int* v, int v_button)
{
    if (!locale::IsRtl()) return ImGui::RadioButton(label, v, v_button);
    ImGui::PushID(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float radioSz = ImGui::GetFrameHeight();
    const ImVec2 labelSz = ImGui::CalcTextSize(label, nullptr, true);
    const float total = labelSz.x + st.ItemInnerSpacing.x + radioSz;
    const float lineStartX = ImGui::GetWindowContentRegionMin().x;
    if (ImGui::GetCursorPosX() <= lineStartX + 1.0f) RtlAlignNext(total);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    bool pressed = ImGui::RadioButton("##rb", *v == v_button);
    if (pressed) *v = v_button;
    ImGui::PopID();
    return pressed;
}

static bool RtlCheckbox(const char* label, bool* v)
{
    if (!locale::IsRtl()) return ImGui::Checkbox(label, v);
    ImGui::PushID(label);
    ImGui::PushTextWrapPos(-1.0f);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float boxSz = ImGui::GetFrameHeight();
    const ImVec2 labelSz = ImGui::CalcTextSize(label, nullptr, true);
    const float total = labelSz.x + st.ItemInnerSpacing.x + boxSz;
    const float lineStartX = ImGui::GetWindowContentRegionMin().x;
    if (ImGui::GetCursorPosX() <= lineStartX + 1.0f) RtlAlignNext(total);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    bool pressed = ImGui::Checkbox("##cb", v);
    ImGui::PopTextWrapPos();
    ImGui::PopID();
    return pressed;
}

static bool RtlCheckboxHint(const char* label, bool* v, const char* hint, const ImVec4& hintCol)
{
    if (!hint || !hint[0]) return RtlCheckbox(label, v);
    if (!locale::IsRtl()) {
        const bool pressed = ImGui::Checkbox(label, v);
        ImGui::SameLine();
        ImGui::TextColored(hintCol, "%s", hint);
        return pressed;
    }
    ImGui::PushID(label);
    ImGui::PushTextWrapPos(-1.0f);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float boxSz = ImGui::GetFrameHeight();
    const float labelW = ImGui::CalcTextSize(label, nullptr, true).x;
    const float hintW = ImGui::CalcTextSize(hint).x;
    const float gap = st.ItemSpacing.x;
    const float total = hintW + gap + labelW + st.ItemInnerSpacing.x + boxSz;
    const float lineStartX = ImGui::GetWindowContentRegionMin().x;
    if (ImGui::GetCursorPosX() <= lineStartX + 1.0f) RtlAlignNext(total);
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, hintCol);
    ImGui::TextUnformatted(hint);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, gap);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    const bool pressed = ImGui::Checkbox("##cb", v);
    ImGui::EndGroup();
    ImGui::PopTextWrapPos();
    ImGui::PopID();
    return pressed;
}

static constexpr float kHdrBtnIconSz     = 11.f;
static constexpr float kHdrBtnPadX       = 6.f;
static constexpr float kHdrBtnPadY       = 2.f;
static constexpr float kHdrBtnGapIcon    = 4.f;
static constexpr float kHdrBtnStackGap   = 3.f;
static constexpr float kHdrBtnFontScale  = 0.78f;

static float CalcHeaderLinkButtonWidth(const char* label, float iconSz, float padX, float gapAfterIcon)
{
    ImGui::SetWindowFontScale(kHdrBtnFontScale);
    const float w = padX * 2.f + iconSz + gapAfterIcon + ImGui::CalcTextSize(label).x;
    ImGui::SetWindowFontScale(1.0f);
    return w;
}

static float CalcHeaderLinkButtonHeight(float iconSz)
{
    ImGui::SetWindowFontScale(kHdrBtnFontScale);
    const float textH = ImGui::CalcTextSize("Buy me a coffee").y;
    ImGui::SetWindowFontScale(1.0f);
    return (iconSz > textH ? iconSz : textH) + kHdrBtnPadY * 2.f;
}

static bool DrawHeaderLinkButton(const char* id, ImTextureID iconTex, float iconSz, const char* label, const char* url, float forcedW = 0.f)
{
    const float padX = kHdrBtnPadX, padY = kHdrBtnPadY, gapAfterIcon = kHdrBtnGapIcon;
    const bool rtl = locale::IsRtl();
    ImGui::SetWindowFontScale(kHdrBtnFontScale);
    const ImVec2 textSz = ImGui::CalcTextSize(label);
    ImGui::SetWindowFontScale(1.0f);
    const float btnW = forcedW > 0.f ? forcedW : (padX * 2.f + iconSz + gapAfterIcon + textSz.x);
    const ImVec2 btnSz(btnW, (iconSz > textSz.y ? iconSz : textSz.y) + padY * 2.f);
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.14f, 0.17f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.17f, 0.18f, 0.22f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.11f, 0.12f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.24f, 0.28f, 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    const bool pressed = ImGui::Button("##btn", btnSz);
    const ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float midY = (bb.Min.y + bb.Max.y) * 0.5f;
    float iconX, textX;
    if (rtl) { textX = bb.Min.x + padX; iconX = bb.Max.x - padX - iconSz; }
    else { iconX = bb.Min.x + padX; textX = bb.Min.x + padX + iconSz + gapAfterIcon; }
    if (iconTex) {
        dl->AddImage(iconTex, ImVec2(iconX, midY - iconSz * 0.5f), ImVec2(iconX + iconSz, midY + iconSz * 0.5f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(170, 175, 185, 255));
    }
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * kHdrBtnFontScale, ImVec2(textX, midY - textSz.y * 0.5f), IM_COL32(165, 170, 180, 255), label);
    if (pressed) ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::PopStyleVar(3); ImGui::PopStyleColor(4); ImGui::PopID();
    return pressed;
}

static float CalcHeaderLinkButtonsWidth()
{
    const float githubW = CalcHeaderLinkButtonWidth(locale::T("View on GitHub"), kHdrBtnIconSz, kHdrBtnPadX, kHdrBtnGapIcon);
    const float kofiW   = CalcHeaderLinkButtonWidth(locale::T("Buy me a coffee"), kHdrBtnIconSz, kHdrBtnPadX, kHdrBtnGapIcon);
    return githubW > kofiW ? githubW : kofiW;
}

static float CalcHeaderLinkButtonsHeight()
{
    return CalcHeaderLinkButtonHeight(kHdrBtnIconSz) * 2.f + kHdrBtnStackGap;
}

static void DrawHeaderExternalLinkButtonsAt(float x, float y)
{
    const float btnW = CalcHeaderLinkButtonsWidth();
    const float btnH = CalcHeaderLinkButtonHeight(kHdrBtnIconSz);
    ImGui::SetCursorPos(ImVec2(x, y));
    DrawHeaderLinkButton("github", (ImTextureID)g_texGitHub, kHdrBtnIconSz, locale::T("View on GitHub"), "https://github.com/aneeskhan47/fps-overlay", btnW);
    ImGui::SetCursorPos(ImVec2(x, y + btnH + kHdrBtnStackGap));
    DrawHeaderLinkButton("kofi", (ImTextureID)g_texKofi, kHdrBtnIconSz, locale::T("Buy me a coffee"), "https://ko-fi.com/aneeskhan47", btnW);
}

static void DrawDeveloperAttributionLine()
{
    if (locale::IsRtl()) {
        static char line[384];
        const std::string left = locale::ShapeRtlForImGui(locale::TRaw("& contributors"));
        const std::string right = locale::ShapeRtlForImGui(locale::TRaw("Developed by"));
        snprintf(line, sizeof(line), "%s aneeskhan47 %s", left.c_str(), right.c_str());
        ImGui::PushTextWrapPos(-1.0f);
        RtlAlignNext(ImGui::CalcTextSize(line).x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.55f, .75f, 1.f, 1.f));
        ImGui::TextUnformatted(line);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                ShellExecuteA(nullptr, "open", "https://github.com/aneeskhan47/fps-overlay/graphs/contributors?all=1", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    const char* developed = locale::T("Developed by aneeskhan47 & ");
    const char* contrib = locale::T("contributors");
    ImGui::TextColored(ImVec4(.45f,.45f,.5f,1), "%s", developed);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.55f,.75f,1.f,1));
    ImGui::TextUnformatted(contrib);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            ShellExecuteA(nullptr, "open", "https://github.com/aneeskhan47/fps-overlay/graphs/contributors?all=1", nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::PopStyleColor();
}

static void RtlLabeledValue(const char* labelKey, const char* valueUtf8)
{
    const char* label = locale::T(labelKey);
    const char* value = valueUtf8 ? valueUtf8 : "";
    const ImVec4 col(.50f, .50f, .55f, 1.f);
    if (!locale::IsRtl()) { ImGui::TextColored(col, "%s%s", label, value); return; }
    const float gap = ImGui::CalcTextSize(" ").x;
    const float total = ImGui::CalcTextSize(value).x + gap + ImGui::CalcTextSize(label).x;
    ImGui::PushTextWrapPos(-1.0f);
    RtlAlignNext(total);
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(value);
    ImGui::SameLine(0, gap);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::PopTextWrapPos();
}

static void DrawHotkeyRow(const char* labelKey, int listenSlot, int& keyVk)
{
    const char* label = locale::T(labelKey);
    const bool listening = (g_listeningFor == listenSlot);
    const char* listenHint = locale::T("Press any key...  ");
    const char* changeLbl = (listenSlot == 1) ? locale::T("Change##1") : locale::T("Change##2");
    const char* cancelLbl = (listenSlot == 1) ? locale::T("Cancel##1") : locale::T("Cancel##2");
    const char* keyName = listening ? listenHint : GetKeyName(keyVk);
    if (!locale::IsRtl()) {
        ImGui::Text("%s", label);
        ImGui::SameLine(90);
        if (listening) {
            ImGui::TextColored(ImVec4(1, .8f, .2f, 1), "%s", listenHint);
            ImGui::SameLine();
            if (ImGui::SmallButton(cancelLbl)) g_listeningFor = 0;
        } else {
            ImGui::Text("%s", keyName);
            ImGui::SameLine();
            if (ImGui::SmallButton(changeLbl)) g_listeningFor = listenSlot;
        }
        return;
    }
    const char* btnLbl = listening ? cancelLbl : changeLbl;
    const float gap = 8.f;
    const float labelW = ImGui::CalcTextSize(label).x;
    const float keyW = ImGui::CalcTextSize(keyName).x;
    const float btnW = ImGui::CalcTextSize(btnLbl).x + ImGui::GetStyle().FramePadding.x * 2.f;
    RtlAlignNext(btnW + gap + keyW + gap + labelW);
    ImGui::BeginGroup();
    if (listening) {
        if (ImGui::SmallButton(btnLbl)) g_listeningFor = 0;
        ImGui::SameLine(0, gap);
        ImGui::TextColored(ImVec4(1, .8f, .2f, 1), "%s", keyName);
    } else {
        if (ImGui::SmallButton(btnLbl)) g_listeningFor = listenSlot;
        ImGui::SameLine(0, gap);
        ImGui::TextUnformatted(keyName);
    }
    ImGui::SameLine(0, gap);
    ImGui::TextUnformatted(label);
    ImGui::EndGroup();
}

// ═══════════════════════════════════════════════════════════════════════════
// WinMain
// ═══════════════════════════════════════════════════════════════════════════
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    g_hInstance = hInst;
    LoadConfig(g_Config);

    // Direct-launch mode: clicking overlay.exe starts the overlay immediately.
    // All display and sensor choices continue to come from the saved INI file.
    g_Config.autoStart = true;

    locale::Load(g_Config.language);
    locale::ApplyImGuiRtlFlags();
    CheckForUpdatesAsync();
    ShowWelcomeMessage();
    QueryCpuName();

    HICON hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));
    if (!hIcon) hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = hIcon; wc.hIconSm = hIcon;
    wc.lpszClassName = "FPSOverlay";
    RegisterClassEx(&wc);

    const int ch = 820;
    int cx = (GetSystemMetrics(SM_CXSCREEN) - kConfigDlgOuterW) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    g_hwnd = CreateWindowEx(0, wc.lpszClassName, "FPS Overlay",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
        cx, cy, kConfigDlgOuterW, ch, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    g_isAdmin = IsRunningAsAdmin();
    if (!CreateDeviceD3D(g_hwnd)) {
        MessageBoxW(g_hwnd, locale::ToWide(locale::DialogBody("directx_init_failed", "DirectX 11 initialisation failed.")).c_str(),
                    locale::ToWide(locale::DialogTitle("directx_init_failed", "FPS Overlay")).c_str(), MB_OK | MB_ICONERROR);
        CleanupDeviceD3D(); return 1;
    }

    QueryGpuName();
    EnforcePawnIOOrExit();

    g_lhwmAvailable = false;
    g_lhwmInitFinished.store(false, std::memory_order_relaxed);
    std::thread(LhwmBackgroundInitThread).detach();

    g_cpuTempAvailable = InitWMI();
    if (g_cpuTempAvailable) {
        float testTemp = QueryCpuTemperature();
        g_cpuTempAvailable = (testTemp > 0.0f && testTemp < 150.0f);
    }

    if (!g_Config.autoStart) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;

    LoadAppFonts();
    ApplyStyle();
    locale::ApplyImGuiRtlFlags();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    InitHeaderIconTextures();
    g_fontsNeedReload = false;

    using Clock = std::chrono::high_resolution_clock;
    auto lastCpuTime = Clock::now();
    auto lastGpuTime = lastCpuTime;
    float cpuUsage = 0;
    GetCpuUsage();

    if (g_Config.autoStart) g_Pending = CMD_START_OVERLAY;

    MSG msg = {};
    while (g_Running)
    {
        // Do not show the setup window on normal launch. Wait until sensor discovery
        // finishes, persist the loaded selections, then enter overlay mode.
        if (g_Pending == CMD_START_OVERLAY &&
            g_lhwmInitFinished.load(std::memory_order_acquire)) {
            g_Pending = CMD_NONE;
            SaveConfig(g_Config);
            SwitchToOverlay();
        }
        if (g_Pending == CMD_SHOW_SETTINGS) { g_Pending = CMD_NONE; SwitchToConfig();  }
        if (g_Pending == CMD_EXIT)          { g_Running = false; break; }

        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
        }
        if (!g_Running) break;

        if (g_fontsNeedReload) {
            ImGui_ImplDX11_InvalidateDeviceObjects();
            LoadAppFonts();
            ImGui_ImplDX11_CreateDeviceObjects();
            locale::ApplyImGuiRtlFlags();
            g_fontsNeedReload = false;
        }

        static bool s_appliedLhwmCpuTempLift = false;
        if (g_lhwmInitFinished.load(std::memory_order_acquire) && !s_appliedLhwmCpuTempLift) {
            s_appliedLhwmCpuTempLift = true;
            if (g_lhwmAvailable && !g_lhwmCpuTempPath.empty())
                g_cpuTempAvailable = true;
        }

        // ══════════════════════════════════════════════════════════════
        // CONFIG MODE
        // ══════════════════════════════════════════════════════════════
        if (g_Mode == MODE_CONFIG)
        {
            if (g_listeningFor != 0) {
                if (GetAsyncKeyState(VK_ESCAPE) & 1) {
                    g_listeningFor = 0;
                } else {
                    for (int vk = 1; vk < 256; vk++) {
                        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                        if (vk == VK_ESCAPE || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) continue;
                        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) continue;
                        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) continue;
                        if (GetAsyncKeyState(vk) & 1) {
                            if (g_listeningFor == 1) g_Config.toggleKey = vk;
                            if (g_listeningFor == 2) g_Config.exitKey   = vk;
                            g_listeningFor = 0;
                            break;
                        }
                    }
                }
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("##cfg", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings);

            const float headerY = ImGui::GetCursorPosY();
            const bool rtl = locale::IsRtl();
            const float headerButtonsW = CalcHeaderLinkButtonsWidth();
            const float headerButtonsH = CalcHeaderLinkButtonsHeight();
            float titleRowBottom = headerY;
            if (rtl) {
                DrawHeaderExternalLinkButtonsAt(ImGui::GetWindowContentRegionMin().x, headerY);
                ImGui::SetWindowFontScale(1.4f);
                const char* title = locale::T("FPS Overlay");
                const float titleW = ImGui::CalcTextSize(title).x;
                ImGui::SetWindowFontScale(1.0f);
                const float verW = ImGui::CalcTextSize(APP_VERSION).x + ImGui::CalcTextSize(" ").x;
                ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - titleW - verW, headerY));
                ImGui::SetWindowFontScale(1.4f);
                ImGui::TextColored(ImVec4(.35f,.78f,1,1), "%s", title);
                ImGui::SetWindowFontScale(1.0f);
                ImGui::SameLine(0, 0);
                ImGui::TextColored(ImVec4(.45f,.45f,.5f,1), " %s", APP_VERSION);
                titleRowBottom = ImGui::GetCursorPosY();
            } else {
                ImGui::SetWindowFontScale(1.4f);
                ImGui::TextColored(ImVec4(.35f,.78f,1,1), "%s", locale::T("FPS Overlay"));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::SameLine(); ImGui::TextColored(ImVec4(.45f,.45f,.5f,1), " %s", APP_VERSION);
                titleRowBottom = ImGui::GetCursorPosY();
                const float headerButtonsX = ImGui::GetWindowContentRegionMax().x - headerButtonsW;
                DrawHeaderExternalLinkButtonsAt(headerButtonsX, headerY);
            }

            const float headerButtonsBottom = headerY + headerButtonsH;
            float headerNextY = (titleRowBottom > headerButtonsBottom ? titleRowBottom : headerButtonsBottom) + 4.f;
            ImGui::SetCursorPosY(headerNextY);
            DrawDeveloperAttributionLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().ItemSpacing.y);
            ImGui::Separator();

            const float footerH = 56.f;
            const float bodyH = ImGui::GetContentRegionAvail().y - footerH;
            const float sideW = kSettingsSidebarW;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float pageW = ImGui::GetContentRegionAvail().x - sideW - gap;

            auto drawSidebar = [&]() {
                ImGui::BeginChild("##settings_nav", ImVec2(sideW, bodyH), true);
                const char* tabs[] = {
                    "Display", "GPU", "Frequency", "Appearance", "Temperature",
                    "Hotkeys", "Startup", "Language", "About"
                };
                for (int i = 0; i < SETTINGS_TAB_COUNT; ++i) {
                    bool sel = (g_Config.settingsTab == i);
                    if (ImGui::Selectable(locale::T(tabs[i]), sel)) g_Config.settingsTab = i;
                }
                ImGui::EndChild();
            };

            auto drawPage = [&](float width) {
                ImGui::BeginChild("##settings_page", ImVec2(width, bodyH), true);

                if (g_Config.settingsTab == SETTINGS_TAB_DISPLAY) {
                    RtlSectionTitle(locale::T("DISPLAY"));
                    ImGui::Spacing();
                    if (!g_isAdmin)
                        RtlCheckboxHint(locale::T("  FPS Counter (game)"), &g_Config.showFPS, locale::T("(needs admin!)"), ImVec4(.9f,.4f,.2f,1));
                    else
                        RtlCheckbox(locale::T("  FPS Counter (game)"), &g_Config.showFPS);
                    RtlCheckbox("  1% Low FPS", &g_Config.show1PercentLow);
                    RtlCheckbox(locale::T("  CPU Usage"), &g_Config.showCpuUsage);
                    RtlCheckbox(locale::T("  CPU Temp"), &g_Config.showCpuTemp);
                    {
                        const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                        const bool lhwmBad = !g_lhwmAvailable || g_lhwmCpuPowerPath.empty();
                        if (lhwmBusy || lhwmBad)
                            RtlCheckboxHint(locale::T("  CPU Power (W)"), &g_Config.showCpuPower, locale::T(lhwmBusy ? "(loading…)" : "(unavailable)"), lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1));
                        else
                            RtlCheckbox(locale::T("  CPU Power (W)"), &g_Config.showCpuPower);
                    }
                    {
                        const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                        const bool lhwmBad = !g_lhwmAvailable || g_lhwmCpuFanPath.empty();
                        if (lhwmBusy || lhwmBad)
                            RtlCheckboxHint(locale::T("  CPU Fan (RPM)"), &g_Config.showCpuFan, locale::T(lhwmBusy ? "(loading…)" : "(unavailable)"), lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1));
                        else
                            RtlCheckbox(locale::T("  CPU Fan (RPM)"), &g_Config.showCpuFan);
                    }
                    RtlCheckbox(locale::T("  GPU Usage"), &g_Config.showGpuUsage);
                    {
                        const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                        const bool lhwmBad = !g_lhwmAvailable || g_gpuCount == 0;
                        if (lhwmBusy || lhwmBad)
                            RtlCheckboxHint(locale::T("  GPU Temp"), &g_Config.showGpuTemp, locale::T(lhwmBusy ? "(loading…)" : "(unavailable)"), lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1));
                        else
                            RtlCheckbox(locale::T("  GPU Temp"), &g_Config.showGpuTemp);
                    }
                    {
                        const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                        const bool lhwmBad = !g_lhwmAvailable || g_gpuCount == 0 || g_lhwmGpuPowerPath.empty();
                        if (lhwmBusy || lhwmBad)
                            RtlCheckboxHint(locale::T("  GPU Power (W)"), &g_Config.showGpuPower, locale::T(lhwmBusy ? "(loading…)" : "(unavailable)"), lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1));
                        else
                            RtlCheckbox(locale::T("  GPU Power (W)"), &g_Config.showGpuPower);
                    }
                    {
                        const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                        const bool lhwmBad = !g_lhwmAvailable || g_gpuCount == 0 || g_lhwmGpuFanPath.empty();
                        if (lhwmBusy || lhwmBad)
                            RtlCheckboxHint(locale::T("  GPU Fan (RPM)"), &g_Config.showGpuFan, locale::T(lhwmBusy ? "(loading…)" : "(unavailable)"), lhwmBusy ? ImVec4(.55f,.55f,.58f,1) : ImVec4(.9f,.4f,.2f,1));
                        else
                            RtlCheckbox(locale::T("  GPU Fan (RPM)"), &g_Config.showGpuFan);
                    }
                    {
                        const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                        const bool lhwmBad = !g_lhwmAvailable || g_gpuCount == 0;
                        if (lhwmBusy) RtlCheckboxHint(locale::T("  GPU VRAM Usage"), &g_Config.showVRAM, locale::T("(loading…)"), ImVec4(.55f,.55f,.58f,1));
                        else if (lhwmBad) RtlCheckboxHint(locale::T("  GPU VRAM Usage"), &g_Config.showVRAM, locale::T("(unavailable)"), ImVec4(.9f,.4f,.2f,1));
                        else RtlCheckbox(locale::T("  GPU VRAM Usage"), &g_Config.showVRAM);
                    }
                    RtlCheckbox(locale::T("  RAM Usage"), &g_Config.showRAM);
                    RtlCheckbox(locale::T("  Show Time"), &g_Config.showTime);
                    if (g_Config.showTime) {
                        ImGui::Indent(16.f);
                        const char* timeFormats[] = { locale::T("24 Hour"), locale::T("12 Hour (AM/PM)") };
                        ImGui::SetNextItemWidth(-1);
                        ImGui::Combo("##timefmt", &g_Config.timeFormat, timeFormats, 2);
                        RtlCheckbox(locale::T("  Show Seconds"), &g_Config.timeShowSeconds);
                        ImGui::Unindent(16.f);
                    }
                }
                else if (g_Config.settingsTab == SETTINGS_TAB_GPU) {
                    RtlSectionTitle(locale::T("GPU SELECTION"));
                    ImGui::Spacing();
                    if (g_gpuCount > 0) {
                        const char* previewName = (g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount) ? g_gpuList[g_Config.selectedGpu].name : locale::T("Select GPU...");
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::BeginCombo("##gpuselect", previewName)) {
                            for (int i = 0; i < g_gpuCount; i++) {
                                bool isSelected = (g_Config.selectedGpu == i);
                                if (ImGui::Selectable(g_gpuList[i].name, isSelected)) SelectGpu(i);
                                if (isSelected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    } else {
                        ImGui::TextColored(ImVec4(.55f,.55f,.58f,1), "%s", locale::T(!g_lhwmInitFinished.load(std::memory_order_acquire) ? "(loading…)" : "(unavailable)"));
                    }
                }
                else if (g_Config.settingsTab == SETTINGS_TAB_FREQUENCY) {
                    RtlSectionTitle(locale::T("FREQUENCY"));
                    ImGui::Spacing();
                    if (!g_lhwmInitFinished.load(std::memory_order_acquire)) {
                        ImGui::TextColored(ImVec4(.55f,.55f,.58f,1), "%s", locale::T("Initializing LibreHardwareMonitor…"));
                    } else if (!g_lhwmAvailable) {
                        ImGui::TextColored(ImVec4(.55f,.55f,.58f,1), "%s", locale::T("Requires LibreHardwareMonitor."));
                    } else {
                        RtlCheckbox(locale::T("  Show CPU frequency"), &g_Config.showCpuFreq);
                        if (g_Config.showCpuFreq) {
                            ImGui::Indent();
                            const char* cpuPrev = locale::T("(select sensor)");
                            for (const auto& o : g_cpuClockOpts) {
                                if (strcmp(g_Config.cpuFreqPath, o.second.c_str()) == 0) { cpuPrev = o.first.c_str(); break; }
                            }
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::BeginCombo("##cpuclkcombo", cpuPrev)) {
                                for (const auto& o : g_cpuClockOpts) {
                                    bool isSel = (strcmp(g_Config.cpuFreqPath, o.second.c_str()) == 0);
                                    if (ImGui::Selectable(o.first.c_str(), isSel)) snprintf(g_Config.cpuFreqPath, sizeof(g_Config.cpuFreqPath), "%s", o.second.c_str());
                                    if (isSel) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::Unindent();
                        }
                        RtlCheckbox(locale::T("  Show GPU core frequency"), &g_Config.showGpuCoreFreq);
                        if (g_Config.showGpuCoreFreq && g_gpuCount > 0) {
                            ImGui::Indent();
                            GpuInfo& gg = g_gpuList[g_Config.selectedGpu];
                            const char* gpPrev = locale::T("(select sensor)");
                            for (const auto& o : gg.coreClockOpts) {
                                if (strcmp(g_Config.gpuCoreFreqPath, o.second.c_str()) == 0) { gpPrev = o.first.c_str(); break; }
                            }
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::BeginCombo("##gpclkcombo", gpPrev)) {
                                for (const auto& o : gg.coreClockOpts) {
                                    bool isSel = (strcmp(g_Config.gpuCoreFreqPath, o.second.c_str()) == 0);
                                    if (ImGui::Selectable(o.first.c_str(), isSel)) snprintf(g_Config.gpuCoreFreqPath, sizeof(g_Config.gpuCoreFreqPath), "%s", o.second.c_str());
                                    if (isSel) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::Unindent();
                        }
                    }
                }
                else if (g_Config.settingsTab == SETTINGS_TAB_APPEARANCE) {
                    RtlSectionTitle(locale::T("POSITION"));
                    ImGui::Spacing();
                    int prevPos = g_Config.position;
                    RtlRadioButton(locale::T("Top Left"), &g_Config.position, POS_TOP_LEFT); ImGui::SameLine(0, 16);
                    RtlRadioButton(locale::T("Top Center"), &g_Config.position, POS_TOP_CENTER); ImGui::SameLine(0, 16);
                    RtlRadioButton(locale::T("Top Right"), &g_Config.position, POS_TOP_RIGHT);
                    RtlRadioButton(locale::T("Bottom Left"), &g_Config.position, POS_BOTTOM_LEFT); ImGui::SameLine(0, 16);
                    RtlRadioButton(locale::T("Bottom Center"), &g_Config.position, POS_BOTTOM_CENTER); ImGui::SameLine(0, 16);
                    RtlRadioButton(locale::T("Bottom Right"), &g_Config.position, POS_BOTTOM_RIGHT);
                    if (g_Config.position != prevPos) { g_Config.customX = -1.0f; g_Config.customY = -1.0f; }
                    RtlMutedText(locale::T("Hold CTRL to drag or right-click overlay"));

                    ImGui::Spacing(); ImGui::Spacing();
                    RtlSectionTitle(locale::T("LAYOUT"));
                    ImGui::Spacing();
                    RtlRadioButton(locale::T("  Vertical (default)"), &g_Config.layoutStyle, LAYOUT_VERTICAL);
                    RtlRadioButton(locale::T("  Horizontal compact"), &g_Config.layoutStyle, LAYOUT_HORIZONTAL);
                    RtlRadioButton(locale::T("  Steam-like bar"), &g_Config.layoutStyle, LAYOUT_STEAM);
                    ImGui::Spacing();
                    RtlSectionTitle(locale::T("Overlay size"));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderInt("##ovscale", &g_Config.overlayScale, 50, 200, "%d%%");

                    ImGui::Spacing(); ImGui::Spacing();
                    RtlCheckbox(locale::T("No background (text only)"), &g_Config.transparentBackground);
                    RtlSectionTitle(locale::T("Background opacity"));
                    ImGui::BeginDisabled(g_Config.transparentBackground);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderInt("##opac", &g_Config.opacity, 0, 100, "%d%%");
                    ImGui::EndDisabled();
                    RtlSectionTitle(locale::T("Text opacity"));
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderInt("##txtopac", &g_Config.textOpacity, 20, 100, "%d%%");
                }
                else if (g_Config.settingsTab == SETTINGS_TAB_TEMPERATURE) {
                    RtlSectionTitle(locale::T("TEMPERATURE"));
                    ImGui::Spacing();
                    int tempUnit = g_Config.useFahrenheit ? 1 : 0;
                    if (RtlRadioButton(locale::T("Celsius"), &tempUnit, 0)) g_Config.useFahrenheit = false;
                    ImGui::SameLine(0,24);
                    if (RtlRadioButton(locale::T("Fahrenheit"), &tempUnit, 1)) g_Config.useFahrenheit = true;
                }
                else if (g_Config.settingsTab == SETTINGS_TAB_HOTKEYS) {
                    RtlSectionTitle(locale::T("HOTKEYS"));
                    ImGui::Spacing();
                    DrawHotkeyRow("Toggle:", 1, g_Config.toggleKey);
                    DrawHotkeyRow("Exit:", 2, g_Config.exitKey);
                }
                else if (g_Config.settingsTab == SETTINGS_TAB_STARTUP) {
                    RtlSectionTitle(locale::T("STARTUP"));
                    ImGui::Spacing();
                    ImGui::TextWrapped("FPS Overlay now starts directly in overlay mode whenever you launch overlay.exe. Your saved display and sensor selections are loaded automatically.");
                }
                else if (g_Config.settingsTab == SETTINGS_TAB_LANGUAGE) {
                    RtlSectionTitle(locale::T("LANGUAGE"));
                    ImGui::Spacing();
                    struct LangOpt { const char* code; const char* labelKey; };
                    static const LangOpt kLangs[] = {
                        { "en-US", "English" }, { "zh-CN", "Simplified Chinese" }, { "ar", "Arabic" }, { "fa", "Persian" },
                        { "fr", "French" }, { "nl", "Dutch" }, { "de", "German" }, { "it", "Italian" },
                        { "es", "Spanish" }, { "pt-BR", "Portuguese" }, { "pt-PT", "Portuguese (Portugal)" },
                        { "ja", "Japanese" }, { "ko", "Korean" }, { "ru", "Russian" }, { "pl", "Polish" }, { "tr", "Turkish" },
                    };
                    int langIdx = 0;
                    for (int i = 0; i < (int)(sizeof(kLangs) / sizeof(kLangs[0])); ++i) {
                        if (_stricmp(g_Config.language, kLangs[i].code) == 0) { langIdx = i; break; }
                    }
                    for (int i = 0; i < (int)(sizeof(kLangs) / sizeof(kLangs[0])); ++i) {
                        if (RtlRadioButton(locale::T(kLangs[i].labelKey), &langIdx, i)) ApplyLanguage(kLangs[i].code);
                    }
                }
                else if (g_Config.settingsTab == SETTINGS_TAB_ABOUT) {
                    RtlSectionTitle(locale::T("DETECTED HARDWARE"));
                    ImGui::Spacing();
                    RtlLabeledValue("CPU:  ", g_cpuName);
                    RtlLabeledValue("GPU:  ", g_gpuName);
                }
                ImGui::EndChild();
            };

            if (!rtl) { drawSidebar(); ImGui::SameLine(0, gap); drawPage(pageW); }
            else      { drawPage(pageW); ImGui::SameLine(0, gap); drawSidebar(); }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.12f,.56f,.37f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.16f,.68f,.44f,1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.10f,.48f,.32f,1));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8);
            const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
            const char* startBtnLabel = lhwmBusy ? locale::T("Initializing LibreHardwareMonitor…") : locale::T("Start Overlay");
            ImGui::BeginDisabled(lhwmBusy);
            if (ImGui::Button(startBtnLabel, ImVec2(ImGui::GetContentRegionAvail().x, 42))) g_Pending = CMD_START_OVERLAY;
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            ImGui::End();
            Present(0.08f, 0.08f, 0.10f, 1);
        }

        // ══════════════════════════════════════════════════════════════
        // OVERLAY MODE
        // ══════════════════════════════════════════════════════════════
        else
        {
            if (GetAsyncKeyState(g_Config.toggleKey) & 1) g_OvlVisible = !g_OvlVisible;
            if (GetAsyncKeyState(g_Config.exitKey) & 1) { g_Running = false; break; }

            static bool wasVisible = true;
            if (g_OvlVisible != wasVisible) {
                ShowWindow(g_hwnd, g_OvlVisible ? SW_SHOWNA : SW_HIDE);
                wasVisible = g_OvlVisible;
            }
            if (!g_OvlVisible) { Sleep(50); continue; }

            HWND fg = GetForegroundWindow();
            DWORD currentPid = 0;
            if (fg && fg != g_hwnd) {
                GetWindowThreadProcessId(fg, &currentPid);
                g_targetPid.store(currentPid, std::memory_order_relaxed);
            }
            if (currentPid != g_lastTargetPid) {
                g_gameFps.store(0.0f, std::memory_order_relaxed);
                g_game1PercentLow.store(0.0f, std::memory_order_relaxed);
                g_lastTargetPid = currentPid;
                GetProcessName(currentPid, g_targetProcessName, sizeof(g_targetProcessName));
            }
            if (g_lastTargetPid != 0) {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_lastTargetPid);
                if (hProc) {
                    DWORD exitCode = 0;
                    if (GetExitCodeProcess(hProc, &exitCode) && exitCode != STILL_ACTIVE) {
                        g_gameFps.store(0.0f, std::memory_order_relaxed);
                        g_game1PercentLow.store(0.0f, std::memory_order_relaxed);
                        g_lastTargetPid = 0;
                    }
                    CloseHandle(hProc);
                } else {
                    g_gameFps.store(0.0f, std::memory_order_relaxed);
                    g_game1PercentLow.store(0.0f, std::memory_order_relaxed);
                    g_lastTargetPid = 0;
                }
            }

            static float cachedRamUsed = 0, cachedRamTotal = 1;
            auto now = Clock::now();

            if (!g_isDragging) {
                float cpuElapsed = std::chrono::duration<float>(now - lastCpuTime).count();
                if (cpuElapsed >= 1.0f) {
                    cpuUsage = GetCpuUsage();
                    if (g_lhwmAvailable && !g_lhwmCpuTempPath.empty()) g_cpuTemp = g_lhwmCpuTemp;
                    else if (g_cpuTempAvailable) g_cpuTemp = QueryCpuTemperature();
                    lastCpuTime = now;
                }
                float gpuElapsed = std::chrono::duration<float>(now - lastGpuTime).count();
                if (gpuElapsed >= 1.0f) {
                    if (g_lhwmAvailable) PollLHWMStats();
                    lastGpuTime = now;
                    static bool tooltipUpdated = false;
                    if (g_updateCheckDone && !tooltipUpdated) { UpdateTrayTooltip(); tooltipUpdated = true; }
                }
                MEMORYSTATUSEX mem = {}; mem.dwLength = sizeof(mem);
                GlobalMemoryStatusEx(&mem);
                cachedRamUsed  = (float)(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.f*1024.f*1024.f);
                cachedRamTotal = (float)(mem.ullTotalPhys)                    / (1024.f*1024.f*1024.f);

                static auto lastClkPoll = Clock::now();
                float clkDt = std::chrono::duration<float>(now - lastClkPoll).count();
                if (clkDt >= 1.0f) {
                    lastClkPoll = now;
                    PollClockSensors();
                }
            }

            float ramUsed = cachedRamUsed;
            float ramTotal = cachedRamTotal;
            float gameFps = g_gameFps.load(std::memory_order_relaxed);
            float game1PctLow = g_game1PercentLow.load(std::memory_order_relaxed);

            POINT cursorPt; GetCursorPos(&cursorPt);
            bool cursorOverOverlay = PtInRect(&g_overlayBounds, cursorPt);
            bool ctrlKeyDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool leftMouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

            static bool interactionActive = false;
            if (!leftMouseDown) g_isDragging = false;
            if (ctrlKeyDown && cursorOverOverlay) interactionActive = true;
            else if (!ctrlKeyDown) { interactionActive = false; g_isDragging = false; }
            else if (!cursorOverOverlay && !leftMouseDown) interactionActive = false;

            bool ctrlHeld = interactionActive;
            static bool wasCtrlHeld = false;
            if (ctrlHeld && !wasCtrlHeld) { SetClickThrough(false); wasCtrlHeld = true; }
            else if (!ctrlHeld && wasCtrlHeld && !leftMouseDown) { SetClickThrough(true); wasCtrlHeld = false; }

            static bool rightMouseWasDown = false;
            bool rightMouseDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            bool rightMouseJustPressed = rightMouseDown && !rightMouseWasDown;
            rightMouseWasDown = rightMouseDown;

            if (ctrlHeld && rightMouseJustPressed) {
                HMENU m = CreatePopupMenu();
                AppendMenuUtf8(m, MF_STRING, IDM_HIDE, locale::T("Hide Overlay"));
                AppendMenuUtf8(m, MF_STRING, IDM_RESET_POS, locale::T("Reset Position"));
                AppendMenu(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuUtf8(m, MF_STRING, IDM_SETTINGS, locale::T("Settings"));
                AppendMenuUtf8(m, MF_STRING, IDM_EXIT, locale::T("Exit"));
                SetForegroundWindow(g_hwnd);
                int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursorPt.x, cursorPt.y, 0, g_hwnd, nullptr);
                DestroyMenu(m);
                switch (cmd) {
                    case IDM_HIDE: g_OvlVisible = false; break;
                    case IDM_RESET_POS: g_Config.customX = -1.f; g_Config.customY = -1.f; g_overlayForceCornerSnap = true; SaveConfig(g_Config); break;
                    case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS; break;
                    case IDM_EXIT: g_Pending = CMD_EXIT; break;
                }
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            float margin = (g_Config.layoutStyle == LAYOUT_STEAM) ? 0.f : 16.f;
            RECT work{};
            SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
            const float wx = (float)work.left, wy = (float)work.top;
            const float sw = (float)(work.right - work.left), sh = (float)(work.bottom - work.top);
            ImVec2 pos, pivot = {0, 0};
            bool hasCustomPos = (g_Config.customX >= 0 && g_Config.customY >= 0);

            if (hasCustomPos) {
                pos = ImVec2(g_Config.customX, g_Config.customY);
            } else {
                switch (g_Config.position) {
                    default:
                    case POS_TOP_LEFT:      pos = ImVec2(wx + margin, wy + margin); pivot = {0, 0}; break;
                    case POS_TOP_CENTER:    pos = ImVec2(wx + sw * 0.5f, wy + margin); pivot = {0.5f, 0}; break;
                    case POS_TOP_RIGHT:     pos = ImVec2(wx + sw - margin, wy + margin); pivot = {1, 0}; break;
                    case POS_BOTTOM_LEFT:   pos = ImVec2(wx + margin, wy + sh - margin); pivot = {0, 1}; break;
                    case POS_BOTTOM_CENTER: pos = ImVec2(wx + sw * 0.5f, wy + sh - margin); pivot = {0.5f, 1}; break;
                    case POS_BOTTOM_RIGHT:  pos = ImVec2(wx + sw - margin, wy + sh - margin); pivot = {1, 1}; break;
                }
            }

            {
                bool forceSnap = g_overlayForceCornerSnap;
                if (forceSnap) g_overlayForceCornerSnap = false;
                if (!ctrlHeld || forceSnap) ImGui::SetNextWindowPos(pos, hasCustomPos ? ImGuiCond_Once : ImGuiCond_Always, pivot);
            }

            ImGui::SetNextWindowBgAlpha(OverlayBgAlpha(ctrlHeld));
            const float ovSc = g_Config.overlayScale / 100.f;

            ImGuiWindowFlags wf =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
            if (!ctrlHeld) wf |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

            if (g_Config.layoutStyle == LAYOUT_STEAM) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * ovSc, 6.f * ovSc));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f * ovSc, 2.f * ovSc));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
                const float steamBgA = g_Config.transparentBackground && !ctrlHeld ? 0.f : 1.f;
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, steamBgA));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.24f, 0.f));
            }

            ImGui::Begin("##ovl", nullptr, wf);
            {
                ImVec2 wPos = ImGui::GetWindowPos();
                ImVec2 wSize = ImGui::GetWindowSize();
                g_overlayBounds.left = (LONG)wPos.x; g_overlayBounds.top = (LONG)wPos.y;
                g_overlayBounds.right = (LONG)(wPos.x + wSize.x); g_overlayBounds.bottom = (LONG)(wPos.y + wSize.y);
            }

            if (ctrlHeld) {
                ImVec2 winPos = ImGui::GetWindowPos();
                g_Config.customX = winPos.x; g_Config.customY = winPos.y;
                if (leftMouseDown) g_isDragging = true;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wMin = ImGui::GetWindowPos();
                ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowSize().x, wMin.y + ImGui::GetWindowSize().y);
                dl->AddRect(wMin, wMax, IM_COL32(120, 220, 255, 220), 4.0f, 0, 1.5f);
            }

            // ═══════════════════════════════════════════════════════════
            // STEAM-LIKE BAR
            // ═══════════════════════════════════════════════════════════
            if (g_Config.layoutStyle == LAYOUT_STEAM) {
                const float ss = ovSc;
                const float hs = 5.f * ss;
                const float hsTight = 3.f * ss;
                const ImVec4 labFps(0.92f, 0.52f, 0.58f, 1.f);
                const ImVec4 labLow(0.95f, 0.68f, 0.42f, 1.f);
                const ImVec4 labCpu(0.94f, 0.88f, 0.58f, 1.f);
                const ImVec4 labGpu(0.52f, 0.90f, 0.70f, 1.f);
                const ImVec4 sepC(0.55f, 0.55f, 0.58f, 1.f);

                const bool showProcLine = g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0];
                const bool showTimeLine = g_Config.showTime;
                const bool showMetaLine = showProcLine || showTimeLine;
                const float lineH = ImGui::GetTextLineHeightWithSpacing();

                ImGui::SetWindowFontScale(1.0f * ss);
                ImVec2 steamRowStart = ImGui::GetCursorPos();
                if (ctrlHeld) {
                    float rowH = lineH * (showMetaLine ? 2.45f : 1.55f);
                    float rowW = ImGui::GetContentRegionAvail().x;
                    if (rowW < 32.f) rowW = ImGui::GetWindowWidth() - ImGui::GetCursorPos().x - ImGui::GetStyle().WindowPadding.x;
                    ImGui::InvisibleButton("##SteamBarDrag", ImVec2(rowW, rowH));
                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) ImGui::StartMouseMovingWindow(ImGui::GetCurrentWindow());
                    ImGui::SetCursorPos(steamRowStart);
                }

                bool needSep = false;

                if (g_Config.showFPS) {
                    ImGui::TextColored(OvColV(labFps), "%s", locale::T("FPS"));
                    ImGui::SameLine(0, hsTight);
                    if (g_etwAvailable && gameFps > 0)
                        ImGui::TextColored(OvColV(ColorByFps(gameFps)), "%.0f", gameFps);
                    else
                        ImGui::TextColored(OvCol(.50f, .50f, .55f, 1.f), "---");
                    needSep = true;
                }

                if (g_Config.show1PercentLow) {
                    if (needSep) { ImGui::SameLine(0, hs); ImGui::TextColored(OvColV(sepC), "|"); ImGui::SameLine(0, hs); }
                    ImGui::TextColored(OvColV(labLow), "1%% LOW");
                    ImGui::SameLine(0, hsTight);
                    if (g_etwAvailable && game1PctLow > 0)
                        ImGui::TextColored(OvColV(ColorByFps(game1PctLow)), "%.0f", game1PctLow);
                    else
                        ImGui::TextColored(OvCol(.50f, .50f, .55f, 1.f), "---");
                    needSep = true;
                }

                const bool wantCpuHzSt  = g_Config.showCpuFreq && g_Config.cpuFreqPath[0] && g_lhwmAvailable;
                const bool wantCpuPwrSt = g_Config.showCpuPower && !g_lhwmCpuPowerPath.empty() && g_lhwmAvailable;
                const bool wantCpuFanSt = g_Config.showCpuFan && !g_lhwmCpuFanPath.empty() && g_lhwmAvailable;
                if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzSt || wantCpuPwrSt || wantCpuFanSt) {
                    if (needSep) { ImGui::SameLine(0, hs); ImGui::TextColored(OvColV(sepC), "|"); ImGui::SameLine(0, hs); }
                    ImGui::TextColored(OvColV(labCpu), "%s", locale::T("CPU"));
                    ImGui::SameLine(0, hsTight);
                    bool anyCpu = false;
                    if (g_Config.showCpuUsage) {
                        ImGui::TextColored(OvColV(ColorByLoad(cpuUsage)), "%.0f%%", cpuUsage);
                        anyCpu = true;
                    }
                    if (g_Config.showCpuTemp && g_cpuTempAvailable && g_cpuTemp > 0) {
                        if (anyCpu) ImGui::SameLine(0, 4.f * ss);
                        float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                        ImGui::TextColored(OvTempCol(g_cpuTemp, 96.f), "%.0f\xC2\xB0%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                        anyCpu = true;
                    }
                    if (wantCpuHzSt) {
                        if (anyCpu) ImGui::SameLine(0, 6.f * ss);
                        DrawCleanFreqMHz(g_cpuClockMHz);
                        anyCpu = true;
                    }
                    if (wantCpuPwrSt && g_cpuPower > 0.f) {
                        if (anyCpu) ImGui::SameLine(0, 6.f * ss);
                        ImGui::TextColored(OvCol(.85f, .78f, .55f, 1), "%.0fW", g_cpuPower);
                    }
                    if (wantCpuFanSt) {
                        ImGui::SameLine(0, 6.f * ss);
                        ImGui::TextColored(OvCol(.60f, .80f, .90f, 1), "%.0frpm", g_cpuFanRpm);
                    }
                    needSep = true;
                }

                const bool wantGpuHzSt  = g_Config.showGpuCoreFreq && g_Config.gpuCoreFreqPath[0] && g_lhwmAvailable;
                const bool wantGpuPwrSt = g_Config.showGpuPower && !g_lhwmGpuPowerPath.empty() && g_lhwmAvailable;
                const bool wantGpuFanSt = g_Config.showGpuFan && !g_lhwmGpuFanPath.empty() && g_lhwmAvailable;
                if (g_Config.showGpuUsage || g_Config.showGpuTemp || wantGpuHzSt || wantGpuPwrSt || wantGpuFanSt) {
                    if (needSep) { ImGui::SameLine(0, hs); ImGui::TextColored(OvColV(sepC), "|"); ImGui::SameLine(0, hs); }
                    bool hasGpuData = g_lhwmAvailable && g_gpuCount > 0;
                    ImGui::TextColored(OvColV(labGpu), "%s", locale::T("GPU"));
                    ImGui::SameLine(0, hsTight);
                    if (hasGpuData) {
                        bool anyGpu = false;
                        if (g_Config.showGpuUsage) {
                            ImGui::TextColored(OvColV(ColorByLoad(g_gpuUsage)), "%.0f%%", g_gpuUsage);
                            anyGpu = true;
                        }
                        if (g_Config.showGpuTemp && g_gpuTemp > 0) {
                            if (anyGpu) ImGui::SameLine(0, 4.f * ss);
                            float dispTemp = ToDisplayTemp(g_gpuTemp, g_Config.useFahrenheit);
                            ImGui::TextColored(OvTempCol(g_gpuTemp, 86.f), "%.0f\xC2\xB0%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                            anyGpu = true;
                        }
                        if (wantGpuHzSt) {
                            if (anyGpu) ImGui::SameLine(0, 6.f * ss);
                            DrawCleanFreqMHz(g_gpuCoreClockMHz);
                            anyGpu = true;
                        }
                        if (wantGpuPwrSt && g_gpuPower > 0.f) {
                            if (anyGpu) ImGui::SameLine(0, 6.f * ss);
                            ImGui::TextColored(OvCol(.85f, .78f, .55f, 1), "%.0fW", g_gpuPower);
                        }
                        if (wantGpuFanSt) {
                            ImGui::SameLine(0, 6.f * ss);
                            ImGui::TextColored(OvCol(.60f, .80f, .90f, 1), "%.0frpm", g_gpuFanRpm);
                        }
                    } else {
                        ImGui::TextColored(OvCol(.50f, .50f, .55f, 1.f), "N/A");
                    }
                    needSep = true;
                }

                if (g_Config.showVRAM && g_vramTotal > 0.f) {
                    if (needSep) { ImGui::SameLine(0, hs); ImGui::TextColored(OvColV(sepC), "|"); ImGui::SameLine(0, hs); }
                    float vramPct = (g_vramUsed / g_vramTotal) * 100.0f;
                    ImGui::TextColored(OvColV(ColorByLoad(vramPct)), "VRAM %.1f/%.0fG", g_vramUsed, g_vramTotal);
                    needSep = true;
                }

                if (g_Config.showRAM) {
                    if (needSep) { ImGui::SameLine(0, hs); ImGui::TextColored(OvColV(sepC), "|"); ImGui::SameLine(0, hs); }
                    float pct = (ramUsed / ramTotal) * 100;
                    ImGui::TextColored(OvColV(ColorByLoad(pct)), "RAM %.1f/%.0fG", ramUsed, ramTotal);
                }

                if (showMetaLine) {
                    char timeBuf[32] = {};
                    if (showTimeLine) FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                    ImGui::SetWindowFontScale(0.78f * ss);
                    if (showProcLine) ImGui::TextColored(OvCol(.42f, .52f, .42f, 1.f), "%s", g_targetProcessName);
                    if (showTimeLine) {
                        if (showProcLine) ImGui::SameLine(0, 10.f * ss);
                        ImGui::TextColored(OvCol(.55f, .65f, .78f, 1.f), "%s", timeBuf);
                    }
                }
                ImGui::SetWindowFontScale(1.0f);
            }

            // ═══════════════════════════════════════════════════════════
            // HORIZONTAL COMPACT VIEW
            // ═══════════════════════════════════════════════════════════
            else if (g_Config.layoutStyle == LAYOUT_HORIZONTAL) {
                ImGui::SetWindowFontScale(ovSc);
                bool needSep = false;

                if (g_Config.showFPS) {
                    if (g_etwAvailable && gameFps > 0)
                        ImGui::TextColored(OvColV(ColorByFps(gameFps)), "FPS %.0f", gameFps);
                    else
                        ImGui::TextColored(OvCol(.50f,.50f,.55f,1), "FPS ---");
                    needSep = true;
                }

                if (g_Config.show1PercentLow) {
                    if (needSep) { ImGui::SameLine(); ImGui::TextColored(OvCol(.35f, .35f, .40f, 1), " | "); ImGui::SameLine(); }
                    if (g_etwAvailable && game1PctLow > 0)
                        ImGui::TextColored(OvColV(ColorByFps(game1PctLow)), "1%% LOW %.0f", game1PctLow);
                    else
                        ImGui::TextColored(OvCol(.50f,.50f,.55f,1), "1%% LOW ---");
                    needSep = true;
                }

                {
                    const bool wantCpuHzHz = g_Config.showCpuFreq && g_Config.cpuFreqPath[0] && g_lhwmAvailable;
                    const bool wantCpuPwrHz = g_Config.showCpuPower && !g_lhwmCpuPowerPath.empty() && g_lhwmAvailable;
                    const bool wantCpuFanHz = g_Config.showCpuFan && !g_lhwmCpuFanPath.empty() && g_lhwmAvailable;
                    if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzHz || wantCpuPwrHz || wantCpuFanHz) {
                        if (needSep) { ImGui::SameLine(); ImGui::TextColored(OvCol(.35f, .35f, .40f, 1), " | "); ImGui::SameLine(); }
                        if (g_Config.showCpuUsage)
                            ImGui::TextColored(OvColV(ColorByLoad(cpuUsage)), "CPU %.0f%%", cpuUsage);
                        else
                            ImGui::TextColored(OvCol(.78f, .78f, .82f, 1), "CPU");

                        if (g_Config.showCpuTemp && g_cpuTempAvailable && g_cpuTemp > 0) {
                            float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                            ImVec4 tc = OvTempCol(g_cpuTemp, 96.f);
                            ImGui::SameLine(0, 6);
                            ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                        }
                        if (wantCpuHzHz) {
                            ImGui::SameLine(0, 8);
                            DrawCleanFreqMHz(g_cpuClockMHz);
                        }
                        if (wantCpuPwrHz && g_cpuPower > 0.f) {
                            ImGui::SameLine(0, 6);
                            ImGui::TextColored(OvCol(.85f, .78f, .55f, 1), "%.0fW", g_cpuPower);
                        }
                        if (wantCpuFanHz) {
                            ImGui::SameLine(0, 6);
                            ImGui::TextColored(OvCol(.60f, .80f, .90f, 1), "%.0frpm", g_cpuFanRpm);
                        }
                        needSep = true;
                    }
                }

                {
                    const bool wantGpuHzHz = g_Config.showGpuCoreFreq && g_Config.gpuCoreFreqPath[0] && g_lhwmAvailable;
                    const bool wantGpuPwrHz = g_Config.showGpuPower && !g_lhwmGpuPowerPath.empty() && g_lhwmAvailable;
                    const bool wantGpuFanHz = g_Config.showGpuFan && !g_lhwmGpuFanPath.empty() && g_lhwmAvailable;
                    if (g_Config.showGpuUsage || g_Config.showGpuTemp || wantGpuHzHz || wantGpuPwrHz || wantGpuFanHz) {
                        if (needSep) { ImGui::SameLine(); ImGui::TextColored(OvCol(.35f, .35f, .40f, 1), " | "); ImGui::SameLine(); }
                        bool hasGpuData = g_lhwmAvailable && g_gpuCount > 0;
                        if (hasGpuData) {
                            if (g_Config.showGpuUsage)
                                ImGui::TextColored(OvColV(ColorByLoad(g_gpuUsage)), "GPU %.0f%%", g_gpuUsage);
                            else
                                ImGui::TextColored(OvCol(.78f, .78f, .82f, 1), "GPU");

                            if (g_Config.showGpuTemp && g_gpuTemp > 0) {
                                float dispTemp = ToDisplayTemp(g_gpuTemp, g_Config.useFahrenheit);
                                ImVec4 tc = OvTempCol(g_gpuTemp, 86.f);
                                ImGui::SameLine(0, 6);
                                ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                            }
                            if (wantGpuHzHz) {
                                ImGui::SameLine(0, 8);
                                DrawCleanFreqMHz(g_gpuCoreClockMHz);
                            }
                            if (wantGpuPwrHz && g_gpuPower > 0.f) {
                                ImGui::SameLine(0, 6);
                                ImGui::TextColored(OvCol(.85f, .78f, .55f, 1), "%.0fW", g_gpuPower);
                            }
                            if (wantGpuFanHz) {
                                ImGui::SameLine(0, 6);
                                ImGui::TextColored(OvCol(.60f, .80f, .90f, 1), "%.0frpm", g_gpuFanRpm);
                            }
                        } else {
                            ImGui::TextColored(OvCol(.50f, .50f, .55f, 1), "GPU N/A");
                        }
                        needSep = true;
                    }
                }

                if (g_Config.showVRAM && g_vramTotal > 0) {
                    if (needSep) { ImGui::SameLine(); ImGui::TextColored(OvCol(.35f,.35f,.40f,1), " | "); ImGui::SameLine(); }
                    float vramPct = (g_vramUsed / g_vramTotal) * 100.0f;
                    ImGui::TextColored(OvColV(ColorByLoad(vramPct)), "VRAM %.1f/%.0fG", g_vramUsed, g_vramTotal);
                    needSep = true;
                }

                if (g_Config.showRAM) {
                    if (needSep) { ImGui::SameLine(); ImGui::TextColored(OvCol(.35f,.35f,.40f,1), " | "); ImGui::SameLine(); }
                    float pct = (ramUsed / ramTotal) * 100;
                    ImGui::TextColored(OvColV(ColorByLoad(pct)), "RAM %.1f/%.0fG", ramUsed, ramTotal);
                }

                if ((g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0]) || g_Config.showTime) {
                    char timeBuf[32] = {};
                    if (g_Config.showTime) FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                    ImGui::SetWindowFontScale(0.78f * ovSc);
                    if (g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0])
                        ImGui::TextColored(OvCol(.42f,.52f,.42f,1), "%s", g_targetProcessName);
                    if (g_Config.showTime) {
                        if (g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0]) ImGui::SameLine(0, 10.f * ovSc);
                        ImGui::TextColored(OvCol(.55f,.65f,.78f,1), "%s", timeBuf);
                    }
                }
                ImGui::SetWindowFontScale(1.0f);
            }

            // ═══════════════════════════════════════════════════════════
            // VERTICAL VIEW (default)
            // ═══════════════════════════════════════════════════════════
            else {
                ImGui::SetWindowFontScale(ovSc);
                bool needSep = false;

                // FPS + 1% Low FPS
                if (g_Config.showFPS || g_Config.show1PercentLow) {
                    if (g_Config.showFPS) {
                        if (g_etwAvailable && gameFps > 0)
                            ImGui::TextColored(OvColV(ColorByFps(gameFps)), "FPS      %.0f", gameFps);
                        else
                            ImGui::TextColored(OvCol(.50f,.50f,.55f,1), "FPS      ---");
                    }
                    if (g_Config.show1PercentLow) {
                        if (g_etwAvailable && game1PctLow > 0)
                            ImGui::TextColored(OvColV(ColorByFps(game1PctLow)), "1%% LOW   %.0f", game1PctLow);
                        else
                            ImGui::TextColored(OvCol(.50f,.50f,.55f,1), "1%% LOW   ---");
                    }
                    if (g_Config.showProcessName) {
                        ImGui::SetWindowFontScale(0.82f * ovSc);
                        if (g_targetProcessName[0])
                            ImGui::TextColored(OvCol(.42f,.55f,.42f,1), "  %s", g_targetProcessName);
                        else
                            ImGui::TextColored(OvCol(.50f,.50f,.55f,1), "%s", locale::T("  (no process)"));
                        ImGui::SetWindowFontScale(ovSc);
                    }
                    if (g_Config.showTime) {
                        char timeBuf[32];
                        FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                        ImGui::SetWindowFontScale(0.82f * ovSc);
                        ImGui::TextColored(OvCol(.55f,.65f,.78f,1), "  %s", timeBuf);
                        ImGui::SetWindowFontScale(ovSc);
                    }
                    needSep = true;
                } else if (g_Config.showTime) {
                    char timeBuf[32];
                    FormatOverlayTime(g_Config, timeBuf, sizeof(timeBuf));
                    ImGui::TextColored(OvCol(.55f,.65f,.78f,1), "TIME     %s", timeBuf);
                    needSep = true;
                }

                // GPU (Usage, Temp, Frequency in MHz, VRAM — No Graph)
                {
                    const bool wantGpuHzV = g_Config.showGpuCoreFreq && g_Config.gpuCoreFreqPath[0] && g_lhwmAvailable;
                    const bool wantGpuPwrV = g_Config.showGpuPower && !g_lhwmGpuPowerPath.empty() && g_lhwmAvailable;
                    const bool wantGpuFanV = g_Config.showGpuFan && !g_lhwmGpuFanPath.empty() && g_lhwmAvailable;
                    const bool gpuVertBlock = g_Config.showGpuUsage || g_Config.showGpuTemp || wantGpuHzV ||
                                              (g_Config.showVRAM && g_lhwmAvailable && g_gpuCount > 0) ||
                                              wantGpuPwrV || wantGpuFanV;
                    if (gpuVertBlock) {
                        if (needSep) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                        bool hasGpuData = g_lhwmAvailable && g_gpuCount > 0;
                        if (hasGpuData) {
                            if (g_Config.showGpuUsage)
                                ImGui::TextColored(OvColV(ColorByLoad(g_gpuUsage)), "GPU      %.0f%%", g_gpuUsage);
                            if (g_Config.showGpuTemp && g_gpuTemp > 0) {
                                float dispTemp = ToDisplayTemp(g_gpuTemp, g_Config.useFahrenheit);
                                if (g_Config.showGpuUsage) ImGui::SameLine(0, 10.f * ovSc);
                                else { ImGui::TextColored(OvCol(.82f, .82f, .88f, 1), "GPU      "); ImGui::SameLine(0, 0); }
                                ImGui::TextColored(OvTempCol(g_gpuTemp, 86.f), "%.0f\xC2\xB0%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                            } else if (g_Config.showGpuTemp && !g_Config.showGpuUsage) {
                                ImGui::TextColored(OvCol(.50f, .50f, .55f, 1), "GPU      ---");
                            }

                            if (wantGpuHzV) {
                                ImGui::TextColored(OvCol(.55f, .68f, .82f, 1), "GPU CLK  ");
                                ImGui::SameLine(0, 0);
                                DrawCleanFreqMHz(g_gpuCoreClockMHz);
                            }
                            if (g_Config.showVRAM && g_vramTotal > 0) {
                                float vramPct = (g_vramUsed / g_vramTotal) * 100.0f;
                                ImGui::TextColored(OvColV(ColorByLoad(vramPct)), "VRAM     %.0f%%", vramPct);
                                ImGui::SameLine();
                                ImGui::TextColored(OvCol(.75f, .75f, .80f, 1), " %.1f / %.0f GB", g_vramUsed, g_vramTotal);
                            }
                            if (wantGpuPwrV && g_gpuPower > 0.f)
                                ImGui::TextColored(OvCol(.85f, .78f, .55f, 1), "GPU PWR  %.0f W", g_gpuPower);
                            if (wantGpuFanV)
                                ImGui::TextColored(OvCol(.60f, .80f, .90f, 1), "GPU FAN  %.0f RPM", g_gpuFanRpm);
                        } else {
                            ImGui::TextColored(OvCol(.50f, .50f, .55f, 1), "GPU      N/A");
                        }
                        ImGui::SetWindowFontScale(0.80f * ovSc);
                        ImGui::TextColored(OvCol(.42f, .42f, .48f, 1), "  %s", g_gpuName);
                        ImGui::SetWindowFontScale(ovSc);
                        needSep = true;
                    }
                }

                // CPU (Usage, Temp, Frequency in MHz — No Graph)
                {
                    const bool wantCpuHzV = g_Config.showCpuFreq && g_Config.cpuFreqPath[0] && g_lhwmAvailable;
                    const bool wantCpuPwrV = g_Config.showCpuPower && !g_lhwmCpuPowerPath.empty() && g_lhwmAvailable;
                    const bool wantCpuFanV = g_Config.showCpuFan && !g_lhwmCpuFanPath.empty() && g_lhwmAvailable;
                    if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzV || wantCpuPwrV || wantCpuFanV) {
                        if (needSep) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                        const bool hasCpuTempVal = g_cpuTempAvailable && g_cpuTemp > 0;
                        if (g_Config.showCpuUsage)
                            ImGui::TextColored(OvColV(ColorByLoad(cpuUsage)), "CPU      %.0f%%", cpuUsage);
                        if (g_Config.showCpuTemp && hasCpuTempVal) {
                            float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                            if (g_Config.showCpuUsage) ImGui::SameLine(0, 10.f * ovSc);
                            else { ImGui::TextColored(OvCol(.82f, .82f, .88f, 1), "CPU      "); ImGui::SameLine(0, 0); }
                            ImGui::TextColored(OvTempCol(g_cpuTemp, 96.f), "%.0f\xC2\xB0%s", dispTemp, g_Config.useFahrenheit ? "F" : "C");
                        } else if (g_Config.showCpuTemp && !g_Config.showCpuUsage) {
                            ImGui::TextColored(OvCol(.50f, .50f, .55f, 1), "CPU      ---");
                        }

                        if (wantCpuHzV) {
                            ImGui::TextColored(OvCol(.55f, .68f, .82f, 1), "CPU CLK  ");
                            ImGui::SameLine(0, 0);
                            DrawCleanFreqMHz(g_cpuClockMHz);
                        }
                        if (wantCpuPwrV && g_cpuPower > 0.f)
                            ImGui::TextColored(OvCol(.85f, .78f, .55f, 1), "CPU PWR  %.0f W", g_cpuPower);
                        if (wantCpuFanV)
                            ImGui::TextColored(OvCol(.60f, .80f, .90f, 1), "CPU FAN  %.0f RPM", g_cpuFanRpm);

                        ImGui::SetWindowFontScale(0.80f * ovSc);
                        ImGui::TextColored(OvCol(.42f, .42f, .48f, 1), "  %s", g_cpuName);
                        ImGui::SetWindowFontScale(ovSc);
                        needSep = true;
                    }
                }

                // RAM
                if (g_Config.showRAM) {
                    if (needSep) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                    float pct = (ramUsed / ramTotal) * 100;
                    ImGui::TextColored(OvColV(ColorByLoad(pct)), "RAM      %.0f%%", pct);
                    ImGui::SameLine();
                    ImGui::TextColored(OvCol(.75f,.75f,.80f,1), " %.1f / %.1f GB", ramUsed, ramTotal);
                }
                ImGui::SetWindowFontScale(1.0f);
            }

            if (ctrlHeld) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::SetWindowFontScale(0.85f * ovSc);
                ImGui::TextColored(OvCol(0.5f, 0.75f, 1.0f, 1.0f), "%s", locale::T("Drag to move | Right-click for menu"));
                ImGui::SetWindowFontScale(1.0f);
            }

            ImGui::End();

            if (g_Config.layoutStyle == LAYOUT_STEAM) {
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(4);
            }

            Present(0, 0, 0, 0);
        }
    }

    SaveConfig(g_Config);
    StopEtwSession();
    if (g_Mode == MODE_OVERLAY) RemoveTrayIcon();
    ReleaseHeaderIconTextures();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClass("FPSOverlay", g_hInstance);
    ShutdownWMI();
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg)
    {
    case WM_CLOSE:
        if (g_Mode == MODE_CONFIG) g_Running = false;
        return 0;
    case WM_DESTROY:
        return 0;
    case WM_GETMINMAXINFO:
        if (g_Mode == MODE_CONFIG && lParam) {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = kConfigDlgOuterW;
            mmi->ptMaxTrackSize.x = kConfigDlgOuterW;
            mmi->ptMinTrackSize.y = kConfigDlgMinOuterH;
        }
        return 0;
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            if (g_OvlVisible) AppendMenuUtf8(m, MF_STRING, IDM_HIDE, locale::T("Hide Overlay"));
            else AppendMenuUtf8(m, MF_STRING, IDM_SHOW, locale::T("Show Overlay"));
            AppendMenu(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuUtf8(m, MF_STRING, IDM_SETTINGS, locale::T("Settings"));
            AppendMenu(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuUtf8(m, MF_STRING, IDM_EXIT, locale::T("Exit"));
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(m);
            switch (cmd) {
                case IDM_SHOW: g_OvlVisible = true; break;
                case IDM_HIDE: g_OvlVisible = false; break;
                case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS; break;
                case IDM_EXIT: g_Pending = CMD_EXIT; break;
            }
        }
        return 0;
    case WM_CONTEXTMENU:
        if (g_Mode == MODE_OVERLAY) {
            bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            POINT pt; GetCursorPos(&pt);
            if (ctrlHeld && PtInRect(&g_overlayBounds, pt)) {
                HMENU m = CreatePopupMenu();
                AppendMenuUtf8(m, MF_STRING, IDM_HIDE, locale::T("Hide Overlay"));
                AppendMenuUtf8(m, MF_STRING, IDM_RESET_POS, locale::T("Reset Position"));
                AppendMenu(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuUtf8(m, MF_STRING, IDM_SETTINGS, locale::T("Settings"));
                AppendMenuUtf8(m, MF_STRING, IDM_EXIT, locale::T("Exit"));
                SetForegroundWindow(hWnd);
                int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(m);
                switch (cmd) {
                    case IDM_HIDE: g_OvlVisible = false; break;
                    case IDM_RESET_POS: g_Config.customX = -1.f; g_Config.customY = -1.f; g_overlayForceCornerSnap = true; SaveConfig(g_Config); break;
                    case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS; break;
                    case IDM_EXIT: g_Pending = CMD_EXIT; break;
                }
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
            case IDM_SHOW: g_OvlVisible = true; break;
            case IDM_HIDE: g_OvlVisible = false; break;
            case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS; break;
            case IDM_EXIT: g_Pending = CMD_EXIT; break;
        }
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
