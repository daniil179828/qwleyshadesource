 
 
 
 
 
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include "UiControlClient.h"

#include <tlhelp32.h>
#include <cwchar>
#include <dwmapi.h>
#include <wincodec.h>
#include <objbase.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <cfloat>
#include <cmath>
#include <cstdlib>

#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#define PIPE_NAME       L"\\\\.\\pipe\\qwleyshade"
#define FLAG_COLOR_OK   (1u << 0)
#define FLAG_DEPTH_OK   (1u << 1)
#define FLAG_FPS_OK     (1u << 2)
#define FLAG_VIEWPORT_OK (1u << 4)
#define FLAG_FULL_FRAME  (1u << 5)
#define PAYLOAD_SIZE    0x40u

extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#pragma pack(push, 1)
struct PipePayloadRaw40
{
    uint64_t depthHandle;
    uint64_t colorHandle;
    uint32_t depthWidth;
    uint32_t depthHeight;
    uint32_t colorWidth;
    uint32_t colorHeight;
    uint32_t depthFormat;
    uint32_t colorFormat;
    uint32_t frameCount;
    uint32_t flags;
    uint32_t vpX;
    uint32_t vpY;
    uint32_t vpWidth;
    uint32_t vpHeight;
};
#pragma pack(pop)
static_assert(sizeof(PipePayloadRaw40) == PAYLOAD_SIZE, "PipePayloadRaw40 must be 0x40 bytes");

struct PipePayload
{
    uint64_t depthHandle = 0;
    uint64_t colorHandle = 0;
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    uint32_t colorWidth = 0;
    uint32_t colorHeight = 0;
    uint32_t depthFormat = 0;
    uint32_t colorFormat = 0;
    uint32_t frameCount = 0;
    uint32_t flags = 0;
    uint32_t vpX = 0;
    uint32_t vpY = 0;
    uint32_t vpWidth = 0;
    uint32_t vpHeight = 0;
};

template <typename T>
static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static DXGI_FORMAT DepthSRVFormat(uint32_t fmt)
{
    DXGI_FORMAT f = (DXGI_FORMAT)fmt;
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return DXGI_FORMAT_R32_FLOAT;
    }
}

static DXGI_FORMAT ColorSRVFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:   return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return f;
    }
}

static PipePayload NormalizePayload(const uint8_t raw[PAYLOAD_SIZE])
{
    const PipePayloadRaw40* r = reinterpret_cast<const PipePayloadRaw40*>(raw);
    PipePayload p;
    p.depthHandle = r->depthHandle;
    p.colorHandle = r->colorHandle;
    p.depthWidth = r->depthWidth ? r->depthWidth : r->colorWidth;
    p.depthHeight = r->depthHeight ? r->depthHeight : r->colorHeight;
    p.colorWidth = r->colorWidth;
    p.colorHeight = r->colorHeight;
    p.depthFormat = r->depthFormat;
    p.colorFormat = r->colorFormat;
    p.frameCount = r->frameCount;
    p.flags = r->flags;
    p.vpX = r->vpX;
    p.vpY = r->vpY;
    p.vpWidth = r->vpWidth;
    p.vpHeight = r->vpHeight;

    if ((p.flags & FLAG_VIEWPORT_OK) && p.vpWidth && p.vpHeight)
    {
        if (!p.colorWidth)  p.colorWidth = p.vpWidth;
        if (!p.colorHeight) p.colorHeight = p.vpHeight;
        if (!p.depthWidth)  p.depthWidth = p.vpWidth;
        if (!p.depthHeight) p.depthHeight = p.vpHeight;
    }
    return p;
}

static HWND   g_hwnd = nullptr;
static HWND   g_target = nullptr;
static DWORD  g_targetPid = 0;
static qwui::Client g_nativeUi;
static std::vector<DWORD> g_targetPids;
static DWORD  g_targetPidTick = 0;
static std::string g_targetExeName = "";
static bool   g_console = false;
static bool   g_desktop = false;
static bool   g_running = true;
static bool   g_menu = false;
static bool   g_reshadeInput = false;
static bool   g_transparent = false;
static bool   g_overlayEnabled = true;
static bool   g_overlayHiddenForForeground = false;
static bool   g_resumeArmed = false;
static DWORD  g_resumeStartTick = 0;
static bool   g_presentOccluded = false;
static DWORD  g_nextPresentProbeTick = 0;

static bool   g_externalOverlayForeground = false;
static bool   g_gameForegroundSession = false;
static bool   g_graphicsRecovering = false;
static DWORD  g_graphicsRetryTick = 0;
static uint64_t g_graphicsGeneration = 0;
 
static uint64_t g_constantBufferEpoch = 0;
static HANDLE g_frameWakeEvent = nullptr;  
static bool   g_vsync = true;
static bool   g_tearingSupported = false;
static UINT   g_maxOverlayFps = 60;
static bool   g_overlayFpsExplicit = false;
static bool   g_uiLayoutReset = true;
static bool   g_showKeyHelp = true;
static bool   g_highResolutionTimer = false;

static bool   g_schedulingBoostRequested = false;
static bool   g_nvidiaBackgroundCapRequested = false;
static bool   g_reshadeCompatibility = true;
static bool   g_separateScenePasses = false;
static int    g_adapterIndex = -1;
static bool   g_disableDepthHeuristic = false;
static std::atomic<bool> g_statsVisible{ false };
enum class PerfTier { Auto, Low, Balanced, High };
static PerfTier g_requestedPerfTier = PerfTier::Auto;
static const char* g_activePerfProfile = "Auto";
static bool   g_perfTierFromCmdline = false;
static bool   g_gpuSoftware = false;
static UINT   g_displayRefreshHz = 60;
static SIZE_T g_gpuDedicatedMB = 0;
static double g_overlayFrameCostMs = 0.0;

struct pdh_process_stats_t
{
    float roblox_cpu = 0.f;
    float overlay_cpu = 0.f;
    float roblox_vram_mb = 0.f;
    float roblox_shared_mb = 0.f;
    float roblox_gpu_pct = 0.f;
    bool  valid = false;
    bool cpu_valid = false, overlay_cpu_valid = false;
    bool gpu_valid = false, dedicated_valid = false, shared_valid = false;
};

static pdh_process_stats_t g_process_stats;
static std::mutex          g_process_stats_mutex;
static std::thread         g_pdh_thread;
static std::atomic<bool>   g_pdh_stop{ false };
static HANDLE              g_single_instance_mutex = nullptr;
static std::atomic<DWORD>  g_statsTargetPid{ 0 };
static std::mutex          g_pdhWaitMutex;
static std::condition_variable g_pdhWaitCv;

static UINT  g_menuActiveFps = 60;
static UINT  g_menuIdleFps = 20;
static UINT  g_counterUiFps = 8;
static std::atomic<DWORD> g_statsSampleIntervalMs{ 150 };
static DWORD g_vramQueryIntervalMs = 1500;



static bool  g_renderUsePointSampler = false;

static bool  g_skipRedundantDepthDraw = false;

static DWORD g_autoDepthRebuildIntervalMs = 0;
static DWORD g_lastAutoDepthRebuildMs = 0;

static DWORD g_cursorPollIntervalMs = 0;
static DWORD g_lastCursorPollMs = 0;

static DWORD g_pidScanIntervalMs = 6000;


static int   g_resourceUpdateStrideFrames = 0;
static int   g_resourceUpdateFrameCounter = 0;

static UINT  g_samplerAnisotropy = 1;

static UINT   g_width = 1280, g_height = 720;
static DWORD  g_lastFollow = 0;
static RECT   g_lastRect = { 0,0,0,0 };

static bool  g_depthReversed = true;
static bool  g_depthUpsideDown = false;
static bool  g_depthLogarithmic = false;
static float g_depthFarPlane = 1000.0f;
static bool  g_depthPreview = false;
static bool  g_useViewportRemap = true;
static bool  g_depthPassEnabled = true;
static bool  g_fastDepthFinal = true;

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11Texture2D* g_localDepth = nullptr;
static ID3D11DepthStencilView* g_localDSV = nullptr;
static ID3D11ShaderResourceView* g_localDepthSRV = nullptr;
static ID3D11DepthStencilState* g_dsWrite = nullptr;
static ID3D11DepthStencilState* g_dsOff = nullptr;
static ID3D11BlendState* g_blendColor = nullptr;
static ID3D11BlendState* g_blendDepthOnly = nullptr;
static ID3D11RasterizerState* g_sceneRaster = nullptr;
static ID3D11SamplerState* g_sampLinear = nullptr;
static ID3D11SamplerState* g_sampPoint = nullptr;
static ID3D11Buffer* g_cb = nullptr;
static ID3D11Buffer* g_cbColor = nullptr;
static ID3D11Buffer* g_cbCombined = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_psDepth = nullptr;
static ID3D11PixelShader* g_psColorDepth = nullptr;
static ID3D11PixelShader* g_psColor = nullptr;
static ID3D11PixelShader* g_psDepthView = nullptr;

static HWND g_uiHwnd = nullptr;
static HWND g_fpsHwnd = nullptr;
static ID3D11Texture2D* g_uiRT = nullptr;
static ID3D11RenderTargetView* g_uiRTV = nullptr;
static ID3D11ShaderResourceView* g_uiRTSRV = nullptr;
static ID3D11Texture2D* g_uiRT2 = nullptr;
static ID3D11RenderTargetView* g_uiRTV2 = nullptr;

static constexpr int kUiReadbackSlots = 3;
struct UiReadbackSlot
{
    ID3D11Texture2D* texture = nullptr;
    bool pending = false;
    uint64_t serial = 0;
    int x = 0, y = 0;
};
static UiReadbackSlot g_uiReadbacks[kUiReadbackSlots];
static uint64_t g_uiReadbackSerial = 0;
static void ReleaseUiReadbacks()
{
    for (auto& slot : g_uiReadbacks)
    {
        SafeRelease(slot.texture);
        slot.pending = false;
    }
}
static ID3D11PixelShader* g_psPremult = nullptr;
static HBITMAP g_uiBitmap = nullptr;
static HDC g_uiDc = nullptr;
static HGDIOBJ g_uiOriginalBitmap = nullptr;
static void* g_uiBits = nullptr;
static UINT g_uiW = 0, g_uiH = 0;
static UINT g_uiRegionW = 0, g_uiRegionH = 0;
static UINT g_uiBitmapW = 0, g_uiBitmapH = 0;
static ID3D11Buffer* g_uiCB = nullptr;
static HBITMAP g_fpsBitmap = nullptr;
static HDC g_fpsDc = nullptr;
static HGDIOBJ g_fpsOriginalBitmap = nullptr;
static void* g_fpsBits = nullptr;
static HFONT g_fpsFont = nullptr;
static std::wstring g_uiFontPath;
static bool g_privateUiFontLoaded = false;

static ID3D11Texture2D* g_depthTex = nullptr;
static ID3D11ShaderResourceView* g_depthSRV = nullptr;
static ID3D11Texture2D* g_colorTex = nullptr;
static ID3D11Texture2D* g_colorSampleTex = nullptr;
static bool g_colorNeedsResolve = false;
static DXGI_FORMAT g_colorResolveFormat = DXGI_FORMAT_UNKNOWN;
static ID3D11ShaderResourceView* g_colorSRV = nullptr;
static uint64_t g_lastDepthHandle = 0, g_lastColorHandle = 0;
static uint32_t g_lastDepthW = 0, g_lastDepthH = 0, g_lastDepthFmt = 0;
static uint32_t g_lastColorW = 0, g_lastColorH = 0, g_lastColorFmt = 0;
static HRESULT  g_lastDepthOpenHR = S_OK, g_lastColorOpenHR = S_OK;
static HRESULT  g_lastDepthSRVHR = S_OK, g_lastColorSRVHR = S_OK;
static UINT     g_depthTexW = 0, g_depthTexH = 0;
static UINT     g_colorTexW = 0, g_colorTexH = 0;

static int      g_depthHeuristicDraws = 12;
static bool     g_showFPS = true;
static float    g_fpsColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static int      g_fpsOffsetX = 10;
static int      g_fpsOffsetY = 10;
static int  g_minimizeKey = VK_F1;
static int  g_depthPreviewKey = VK_F3;
static int  g_menuKey = VK_TAB;
static bool g_menuNeedsShift = true;
static bool g_menuNeedsCtrl = false;
static bool g_menuNeedsAlt = false;

static const DWORD kAutoGraceMs = 1500;
static const int   kAutoMaxAttempts = 6;
static bool g_autoArmed = false;
static int  g_autoAttemptCount = 0;
static DWORD g_autoLastAttemptTick = 0;
static DWORD g_autoWatchStartTick = 0;

static int   g_depthRecreateCount = 0;

static uint64_t g_framesPresented = 0;
static DWORD    g_firstPresentTick = 0;

static bool     g_autoDepthRebuildEnabled = true;

static int  g_waitKeyTarget = 0;
static DWORD g_waitKeyStart = 0;
static float g_fps = 0.0f;
static bool  g_sourceFpsAvailable = false;
static std::atomic<uint64_t> g_sourcePacketSerial{ 0 };
static std::string g_gpuName = "Unknown GPU";
static IDXGIAdapter3* g_adapter3 = nullptr;
static UINT64 g_vramUsageBytes = 0;
static UINT64 g_vramBudgetBytes = 0;
static DWORD g_lastVramQueryTick = 0;
static bool g_vramStatsAvailable = false;
static constexpr int kPerfHistorySize = 180;
static float g_overlayFrameHistory[kPerfHistorySize] = {};
static int g_perfHistoryOffset = 0;
static int g_perfHistoryCount = 0;
static DWORD g_resyncDueTick = 0;
static bool  g_resyncing = false;

static std::thread       g_pipeThread;
static std::mutex        g_pipeMutex;
static std::condition_variable g_pipeCv;
static std::atomic<bool> g_pipeStop{ false };
static std::atomic<bool>  g_pipeConnected{ false };

static std::atomic<bool>  g_pipeEverConnected{ false };
static std::atomic<bool>  g_hasPayload{ false };
static std::atomic<DWORD> g_lastPayloadTick{ 0 };
static PipePayload        g_payload;

static char g_iniPath[MAX_PATH] = {};


struct OverlayAppearance {
    ImVec4 background = ImVec4(20.0f / 255, 20.0f / 255, 20.0f / 255, 1);
    ImVec4 accent = ImVec4(38.0f / 255, 31.0f / 255, 24.0f / 255, 1);
    float opacity = 0.96f;
    float backgroundDim = 0.35f;
    bool showControls = true;
    std::string backgroundImage;
};
static OverlayAppearance g_appearance;
static ID3D11ShaderResourceView* g_backgroundSrv = nullptr;
static UINT g_backgroundWidth = 0, g_backgroundHeight = 0;
static int g_mainTab = 0;

static char g_reshadeIniPath[MAX_PATH] = {};
static char g_imguiIniPath[MAX_PATH] = {};

static void SetMenu(bool v);
static void ForceDepthRebuildTick(const char* reason);
static void RequestOverlayResync(const char* reason, DWORD delayMs);
static void ApplyWindowMode();
static void FocusTarget();
static void SaveSettings();
static void WriteReShadeConfig();

static void Log(const char* fmt, ...)
{
    char b[2048];
    va_list va; va_start(va, fmt); vsnprintf(b, sizeof(b), fmt, va); va_end(va);
    OutputDebugStringA(b); OutputDebugStringA("\n");
    if (g_console) { printf("%s\n", b); fflush(stdout); }
}
static bool HasArg(const char* a) { const char* c = GetCommandLineA(); return c && strstr(c, a); }

static std::string TrimStr(const std::string& s);

static const char* const kDefaultTargetExes[] = {
    "RobloxPlayerBeta.exe",
    "RobloxPlayerLauncher.exe",
    "Voidstrap.exe",
    "voistrap.exe",
    "voistrap.bin.exe"
};
static const size_t kDefaultTargetExeCount =
sizeof(kDefaultTargetExes) / sizeof(kDefaultTargetExes[0]);

static std::vector<std::string> g_targetExes;
static std::string g_targetExeDisplay = "";

static std::string LowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static std::vector<std::string> ArgValues(const char* name)
{
    std::vector<std::string> out;
    const char* cmd = GetCommandLineA();
    if (!cmd) return out;

    char key[64] = {};
    snprintf(key, sizeof(key), "/%s:", name);

    const char* p = cmd;
    while ((p = strstr(p, key)) != nullptr)
    {
        p += strlen(key);
        char buf[1024] = {};
        size_t n = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '"' && n + 1 < sizeof(buf))
            buf[n++] = *p++;
        buf[n] = 0;

        size_t start = 0;
        while (start <= strlen(buf))
        {
            const size_t len = strlen(buf);
            size_t sep = len;
            for (size_t i = start; i < len; ++i)
                if (buf[i] == ',' || buf[i] == ';') { sep = i; break; }
            std::string item = LowerAscii(TrimStr(std::string(buf + start, sep - start)));
            if (!item.empty()) out.push_back(item);
            if (sep >= len) break;
            start = sep + 1;
        }
    }
    return out;
}

static void AddTargetExe(const std::string& raw)
{
    std::string v = LowerAscii(TrimStr(raw));
    if (v.empty()) return;
    const size_t slash = v.find_last_of("/\\");
    if (slash != std::string::npos) v = v.substr(slash + 1);
    if (v.empty()) return;
    for (const auto& e : g_targetExes)
        if (e == v) return;
    g_targetExes.push_back(v);
}

static void InitTargetList()
{
    g_targetExes.clear();
    for (size_t i = 0; i < kDefaultTargetExeCount; ++i)
        AddTargetExe(kDefaultTargetExes[i]);
    for (const auto& v : ArgValues("target"))
        AddTargetExe(v);

    g_targetExeDisplay.clear();
    for (size_t i = 0; i < g_targetExes.size(); ++i)
    {
        if (i) g_targetExeDisplay += ", ";
        g_targetExeDisplay += g_targetExes[i];
    }
}

static bool QueryProcessExeName(DWORD pid, char* out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!pid || !out || cap < 2) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &size) != FALSE;
    CloseHandle(h);
    if (!ok) return false;

    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (WideCharToMultiByte(CP_UTF8, 0, base, -1, out, (int)cap, nullptr, nullptr) <= 0)
    {
        out[0] = 0;
        return false;
    }
    return out[0] != 0;
}

static bool IsProcessAlive(DWORD pid)
{
    if (!pid) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

static bool IsTargetProcess(DWORD pid, char* exeOut = nullptr, size_t exeCap = 0)
{
    char buf[MAX_PATH] = {};
    if (!QueryProcessExeName(pid, buf, sizeof(buf))) return false;

    std::string name = LowerAscii(buf);
    for (const auto& e : g_targetExes)
        if (name == e)
        {
            if (exeOut && exeCap) strncpy_s(exeOut, exeCap, buf, _TRUNCATE);
            return true;
        }
    return false;
}

struct PowerThrottleState
{
    DWORD version = 1;
    DWORD controlMask = 0;
    DWORD stateMask = 0;
};

using SetPowerInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using GetPowerInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using MmcssRegisterFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
using MmcssPriorityFn = BOOL(WINAPI*)(HANDLE, int);
using MmcssRevertFn = BOOL(WINAPI*)(HANDLE);

static SetPowerInformationFn g_setProcessPower = nullptr;
static GetPowerInformationFn g_getProcessPower = nullptr;
static SetPowerInformationFn g_setThreadPower = nullptr;
static GetPowerInformationFn g_getThreadPower = nullptr;
static HMODULE g_avrtModule = nullptr;
static HANDLE g_mmcssTask = nullptr;
static MmcssRevertFn g_revertMmcss = nullptr;
static PowerThrottleState g_oldProcessPower, g_oldThreadPower;
static bool g_processPowerChanged = false, g_threadPowerChanged = false;
static DWORD g_oldOverlayPriority = 0;
static int g_oldRenderPriority = THREAD_PRIORITY_ERROR_RETURN;
static HANDLE g_boostedGame = nullptr;
static DWORD g_boostedGamePid = 0;
static DWORD g_gameBoostAttemptPid = 0;
static DWORD g_oldGamePriority = 0;
static bool g_gamePriorityChanged = false, g_gamePowerChanged = false;
static PowerThrottleState g_oldGamePower;
static std::thread g_driverSettingsThread;
static bool g_driverSettingsStarted = false;

static bool DisablePowerThrottling(HANDLE handle, bool thread, PowerThrottleState& previous)
{
    auto get = thread ? g_getThreadPower : g_getProcessPower;
    auto set = thread ? g_setThreadPower : g_setProcessPower;
    const int infoClass = thread ? 3 : 4;
    previous = {};
    if (!get || !set || !get(handle, infoClass, &previous, sizeof(previous))) return false;
    PowerThrottleState state = previous;
    state.version = 1;
    state.controlMask |= 1u;
    state.stateMask &= ~1u;
    return set(handle, infoClass, &state, sizeof(state)) != FALSE;
}

static void RestorePowerThrottling(HANDLE handle, bool thread, const PowerThrottleState& previous)
{
    auto get = thread ? g_getThreadPower : g_getProcessPower;
    auto set = thread ? g_setThreadPower : g_setProcessPower;
    const int infoClass = thread ? 3 : 4;
    PowerThrottleState current;
    if (!get || !set || !get(handle, infoClass, &current, sizeof(current))) return;
    if ((current.controlMask & 1u) && !(current.stateMask & 1u))
    {
        current.controlMask = (current.controlMask & ~1u) | (previous.controlMask & 1u);
        current.stateMask = (current.stateMask & ~1u) | (previous.stateMask & 1u);
        set(handle, infoClass, &current, sizeof(current));
    }
}

static void InitializeSchedulingBoost()
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel)
    {
        g_setProcessPower = reinterpret_cast<SetPowerInformationFn>(GetProcAddress(kernel, "SetProcessInformation"));
        g_getProcessPower = reinterpret_cast<GetPowerInformationFn>(GetProcAddress(kernel, "GetProcessInformation"));
        g_setThreadPower = reinterpret_cast<SetPowerInformationFn>(GetProcAddress(kernel, "SetThreadInformation"));
        g_getThreadPower = reinterpret_cast<GetPowerInformationFn>(GetProcAddress(kernel, "GetThreadInformation"));
    }
    g_oldOverlayPriority = GetPriorityClass(GetCurrentProcess());
    g_oldRenderPriority = GetThreadPriority(GetCurrentThread());
    if (g_schedulingBoostRequested)
    {
        g_processPowerChanged = DisablePowerThrottling(GetCurrentProcess(), false, g_oldProcessPower);
        g_threadPowerChanged = DisablePowerThrottling(GetCurrentThread(), true, g_oldThreadPower);
    }

    if (!g_schedulingBoostRequested)
    {
        Log("Scheduling: normal CPU priority; optional boost disabled");
        return;
    }
    bool aboveNormal = false;
    if (g_oldOverlayPriority == NORMAL_PRIORITY_CLASS || g_oldOverlayPriority == BELOW_NORMAL_PRIORITY_CLASS ||
        g_oldOverlayPriority == IDLE_PRIORITY_CLASS)
        aboveNormal = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != FALSE;
    else aboveNormal = (g_oldOverlayPriority == ABOVE_NORMAL_PRIORITY_CLASS || g_oldOverlayPriority == HIGH_PRIORITY_CLASS);

    if (g_oldRenderPriority != THREAD_PRIORITY_ERROR_RETURN && g_oldRenderPriority < THREAD_PRIORITY_HIGHEST)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    g_avrtModule = LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    bool mmcssHigh = false;
    if (g_avrtModule)
    {
        auto registerTask = reinterpret_cast<MmcssRegisterFn>(GetProcAddress(g_avrtModule, "AvSetMmThreadCharacteristicsW"));
        auto priority = reinterpret_cast<MmcssPriorityFn>(GetProcAddress(g_avrtModule, "AvSetMmThreadPriority"));
        g_revertMmcss = reinterpret_cast<MmcssRevertFn>(GetProcAddress(g_avrtModule, "AvRevertMmThreadCharacteristics"));
        if (registerTask && priority && g_revertMmcss)
        {
            DWORD taskIndex = 0;

            g_mmcssTask = registerTask(L"Games", &taskIndex);

            if (g_mmcssTask) mmcssHigh = priority(g_mmcssTask, 2) != FALSE;
        }
    }
    Log("Scheduling boost: power throttling off process=%d thread=%d, above-normal=%d, MMCSS=%s critical=%d, time-critical=%d",
        int(g_processPowerChanged), int(g_threadPowerChanged), int(aboveNormal),
        g_mmcssTask ? (mmcssHigh ? "Games/high" : "Games/default") : "unavailable", 0, 0);
}

#pragma pack(push, 4)
struct NvSettingValue
{
    union
    {
        uint32_t dword;
        struct { uint32_t length; uint8_t data[4096]; } binary;
        uint16_t text[2048];
    };
};
struct NvSettingV1
{
    uint32_t version;
    uint16_t name[2048];
    uint32_t id;
    uint32_t type;
    uint32_t location;
    uint32_t currentPredefined;
    uint32_t predefinedValid;
    NvSettingValue predefinedValue;
    NvSettingValue currentValue;
};
struct NvProfileV1
{
    uint32_t version;
    uint16_t name[2048];
    uint32_t gpuSupport;
    uint32_t predefined;
    uint32_t applications;
    uint32_t settings;
};
struct NvApplicationV1
{
    uint32_t version;
    uint32_t predefined;
    uint16_t name[2048];
    uint16_t friendlyName[2048];
    uint16_t launcher[2048];
};
#pragma pack(pop)
static_assert(sizeof(NvSettingValue) == 4100, "NVAPI setting value ABI");
static_assert(sizeof(NvSettingV1) == 12320, "NVAPI setting ABI");
static_assert(sizeof(NvProfileV1) == 4116, "NVAPI profile ABI");
static_assert(sizeof(NvApplicationV1) == 12296, "NVAPI application ABI");
static_assert(sizeof(wchar_t) == sizeof(uint16_t), "Windows wchar_t ABI required");

static void NvCopyString(uint16_t* target, size_t capacity, const wchar_t* source)
{
    size_t i = 0;
    for (; i + 1 < capacity && source && source[i]; ++i) target[i] = static_cast<uint16_t>(source[i]);
    target[i] = 0;
}

static void DisableNvidiaBackgroundCap(std::wstring executable)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#ifdef _WIN64
    const wchar_t* dllName = L"nvapi64.dll";
#else
    const wchar_t* dllName = L"nvapi.dll";
#endif
    HMODULE module = LoadLibraryExW(dllName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { Log("NVIDIA: driver API unavailable (%lu).", GetLastError()); return; }
    using QueryFn = void* (__cdecl*)(uint32_t);
    using SimpleFn = int(__cdecl*)();
    using CreateSessionFn = int(__cdecl*)(void**);
    using SessionFn = int(__cdecl*)(void*);
    using FindApplicationFn = int(__cdecl*)(void*, const uint16_t*, void**, NvApplicationV1*);
    using CreateProfileFn = int(__cdecl*)(void*, NvProfileV1*, void**);
    using FindProfileFn = int(__cdecl*)(void*, const uint16_t*, void**);
    using GetProfileFn = int(__cdecl*)(void*, void*, NvProfileV1*);
    using CreateApplicationFn = int(__cdecl*)(void*, void*, NvApplicationV1*);
    using SetSettingFn = int(__cdecl*)(void*, void*, NvSettingV1*);
    using GetSettingFn = int(__cdecl*)(void*, void*, uint32_t, NvSettingV1*);
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(module, "nvapi_QueryInterface"));
    auto initialize = query ? reinterpret_cast<SimpleFn>(query(0x0150E828)) : nullptr;
    auto unload = query ? reinterpret_cast<SimpleFn>(query(0xD22BDD7E)) : nullptr;
    auto createSession = query ? reinterpret_cast<CreateSessionFn>(query(0x0694D52E)) : nullptr;
    auto destroySession = query ? reinterpret_cast<SessionFn>(query(0xDAD9CFF8)) : nullptr;
    auto load = query ? reinterpret_cast<SessionFn>(query(0x375DBD6B)) : nullptr;
    auto save = query ? reinterpret_cast<SessionFn>(query(0xFCBC7E14)) : nullptr;
    auto findApplication = query ? reinterpret_cast<FindApplicationFn>(query(0xEEE566B2)) : nullptr;
    auto createProfile = query ? reinterpret_cast<CreateProfileFn>(query(0xCC176068)) : nullptr;
    auto findProfile = query ? reinterpret_cast<FindProfileFn>(query(0x7E4A9A0B)) : nullptr;
    auto getProfile = query ? reinterpret_cast<GetProfileFn>(query(0x61CD6FD6)) : nullptr;
    auto createApplication = query ? reinterpret_cast<CreateApplicationFn>(query(0x4347A9DE)) : nullptr;
    auto setSetting = query ? reinterpret_cast<SetSettingFn>(query(0x577DD202)) : nullptr;
    auto getSetting = query ? reinterpret_cast<GetSettingFn>(query(0x73BF8338)) : nullptr;
    if (!initialize || !unload || !createSession || !destroySession || !load || !save || !findApplication ||
        !createProfile || !findProfile || !getProfile || !createApplication || !setSetting || !getSetting)
    {
        Log("NVIDIA: required driver interfaces unavailable."); FreeLibrary(module); return;
    }
    int status = initialize();
    if (status != 0) { Log("NVIDIA: initialize failed (%d).", status); FreeLibrary(module); return; }
    void* session = nullptr;
    status = createSession(&session);
    if (status == 0 && session)
    {
        const auto apply = [&]() -> bool {
            int result = load(session);
            if (result != 0) { Log("NVIDIA: load settings failed (%d).", result); return false; }
            NvApplicationV1 application{};
            application.version = uint32_t(sizeof(application)) | (1u << 16);
            NvCopyString(application.name, 2048, executable.c_str());
            NvCopyString(application.friendlyName, 2048, L"Roblox");
            void* profile = nullptr;
            bool applicationAdded = false;
            result = findApplication(session, application.name, &profile, &application);
            if (result == -166)
            {
                NvProfileV1 info{};
                info.version = uint32_t(sizeof(info)) | (1u << 16);
                NvCopyString(info.name, 2048, L"eurotrucks2 - Roblox background FPS");
                result = findProfile(session, info.name, &profile);
                if (result == -163)
                {
                    info.gpuSupport = 1;
                    result = createProfile(session, &info, &profile);
                }
                else if (result == 0)
                {
                    result = getProfile(session, profile, &info);
                    if (result == 0 && info.applications != 0)
                    {
                        Log("NVIDIA: dedicated profile name is already in use; leaving it unchanged."); return false;
                    }
                }
                if (result != 0 || !profile) { Log("NVIDIA: create/find profile failed (%d).", result); return false; }
                application = {};
                application.version = uint32_t(sizeof(application)) | (1u << 16);
                NvCopyString(application.name, 2048, executable.c_str());
                NvCopyString(application.friendlyName, 2048, L"Roblox");
                result = createApplication(session, profile, &application);
                if (result != 0) { Log("NVIDIA: add application failed (%d).", result); return false; }
                applicationAdded = true;
            }
            else if (result != 0 || !profile)
            {
                Log("NVIDIA: find application failed (%d).", result); return false;
            }
            NvProfileV1 info{};
            info.version = uint32_t(sizeof(info)) | (1u << 16);
            result = getProfile(session, profile, &info);
            if (result != 0) { Log("NVIDIA: get profile failed (%d).", result); return false; }
            if (info.applications != 1)
            {
                Log("NVIDIA: profile is shared by %u applications; leaving it unchanged.", info.applications); return false;
            }
            NvSettingV1 setting{};
            setting.version = uint32_t(sizeof(setting)) | (1u << 16);
            constexpr uint32_t backgroundLimitId = 0x10835005;
            result = getSetting(session, profile, backgroundLimitId, &setting);
            if (result == 0 && setting.type == 0 && setting.currentValue.dword == 0 &&
                setting.location == 0 && !applicationAdded)
            {
                Log("NVIDIA: background frame rate cap already off for %ls.", executable.c_str()); return true;
            }
            if (result != 0 && result != -160)
            {
                Log("NVIDIA: background setting query failed (%d).", result); return false;
            }
            if (result == 0 && setting.type != 0)
            {
                Log("NVIDIA: unexpected background setting type; leaving it unchanged."); return false;
            }
            setting = {};
            setting.version = uint32_t(sizeof(setting)) | (1u << 16);
            setting.id = backgroundLimitId;
            setting.type = 0;
            setting.currentValue.dword = 0;
            result = setSetting(session, profile, &setting);
            if (result != 0) { Log("NVIDIA: set background cap failed (%d).", result); return false; }
            result = save(session);
            if (result != 0) { Log("NVIDIA: save settings failed (%d).", result); return false; }
            setting = {};
            setting.version = uint32_t(sizeof(setting)) | (1u << 16);
            result = getSetting(session, profile, backgroundLimitId, &setting);
            if (result != 0 || setting.type != 0 || setting.currentValue.dword != 0)
            {
                Log("NVIDIA: background setting verification failed (%d).", result); return false;
            }
            Log("NVIDIA: turned the background frame rate cap off for %ls.", executable.c_str());
            return true;
            };
        apply();
        destroySession(session);
    }
    else Log("NVIDIA: create session failed (%d).", status);
    unload();
    FreeLibrary(module);
}

static void RestoreGameSchedulingBoost()
{
    if (g_boostedGame)
    {
        if (WaitForSingleObject(g_boostedGame, 0) == WAIT_TIMEOUT)
        {
            if (g_gamePowerChanged) RestorePowerThrottling(g_boostedGame, false, g_oldGamePower);
            if (g_gamePriorityChanged && GetPriorityClass(g_boostedGame) == ABOVE_NORMAL_PRIORITY_CLASS)
                SetPriorityClass(g_boostedGame, g_oldGamePriority);
        }
        CloseHandle(g_boostedGame);
        g_boostedGame = nullptr;
    }
    g_boostedGamePid = 0;
    g_gamePowerChanged = false;
    g_gamePriorityChanged = false;
}

static void GameSchedulingBoostTick()
{
    if (!g_schedulingBoostRequested && !g_nvidiaBackgroundCapRequested) return;
    const DWORD pid = g_targetPid;
    if (g_boostedGame && (pid != g_boostedGamePid || WaitForSingleObject(g_boostedGame, 0) != WAIT_TIMEOUT))
    {
        RestoreGameSchedulingBoost(); g_gameBoostAttemptPid = 0;
    }
    if (!pid) { g_gameBoostAttemptPid = 0; return; }
    if (g_boostedGame || g_gameBoostAttemptPid == pid) return;
    g_gameBoostAttemptPid = pid;
    HANDLE queryProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!queryProcess) { Log("Game scheduling boost: OpenProcess failed (%lu)", GetLastError()); return; }
    wchar_t path[MAX_PATH] = {}; DWORD length = MAX_PATH;
    const bool queried = QueryFullProcessImageNameW(queryProcess, 0, path, &length) != FALSE;
    const bool alive = WaitForSingleObject(queryProcess, 0) == WAIT_TIMEOUT;
    CloseHandle(queryProcess);
    const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
    if (!queried || !alive || _wcsicmp(name, L"RobloxPlayerBeta.exe") != 0) return;

    if (g_nvidiaBackgroundCapRequested && !g_driverSettingsStarted)
    {
        try
        {
            g_driverSettingsThread = std::thread(DisableNvidiaBackgroundCap, std::wstring(name));
            g_driverSettingsStarted = true;
        }
        catch (...) { Log("NVIDIA: could not start driver settings worker."); }
    }
    if (!g_schedulingBoostRequested) return;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) { Log("Game scheduling boost: OpenProcess failed (%lu)", GetLastError()); return; }
    length = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, path, &length)) { CloseHandle(process); return; }
    name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
    if (_wcsicmp(name, L"RobloxPlayerBeta.exe") != 0 || WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
    {
        CloseHandle(process); return;
    }
    g_boostedGame = process; g_boostedGamePid = pid;
    g_oldGamePriority = GetPriorityClass(process);
    g_gamePowerChanged = DisablePowerThrottling(process, false, g_oldGamePower);

    if (g_oldGamePriority == NORMAL_PRIORITY_CLASS || g_oldGamePriority == BELOW_NORMAL_PRIORITY_CLASS ||
        g_oldGamePriority == IDLE_PRIORITY_CLASS)
        g_gamePriorityChanged = SetPriorityClass(process, HIGH_PRIORITY_CLASS) != FALSE;
    Log("Game scheduling boost: pid=%lu, power throttling off=%d, high-priority=%d",
        pid, int(g_gamePowerChanged), int(GetPriorityClass(process) == HIGH_PRIORITY_CLASS));

}

static void StopSchedulingBoost()
{
    if (g_driverSettingsThread.joinable()) g_driverSettingsThread.join();
    RestoreGameSchedulingBoost();
    if (g_mmcssTask && g_revertMmcss) g_revertMmcss(g_mmcssTask);
    g_mmcssTask = nullptr;
    if (g_threadPowerChanged) RestorePowerThrottling(GetCurrentThread(), true, g_oldThreadPower);
    if (g_processPowerChanged) RestorePowerThrottling(GetCurrentProcess(), false, g_oldProcessPower);
    if (g_oldRenderPriority != THREAD_PRIORITY_ERROR_RETURN)
        SetThreadPriority(GetCurrentThread(), g_oldRenderPriority);
    if (g_oldOverlayPriority) SetPriorityClass(GetCurrentProcess(), g_oldOverlayPriority);
    if (g_avrtModule) { FreeLibrary(g_avrtModule); g_avrtModule = nullptr; }
}

static void RefreshTargetPids()
{
    const DWORD now = GetTickCount();

    const DWORD selectedPid = g_targetPid ? g_targetPid :
        (g_targetPids.empty() ? 0 : g_targetPids.front());
    if (g_statsTargetPid.load(std::memory_order_relaxed) != selectedPid)
        g_statsTargetPid.store(selectedPid, std::memory_order_release);

    if (now - g_targetPidTick < 6000) return;
    g_targetPidTick = now;

    g_targetPids.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            char exe[MAX_PATH] = {};
            if (WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exe,
                (int)sizeof(exe), nullptr, nullptr) <= 0)
                continue;
            const std::string name = LowerAscii(exe);
            for (const auto& target : g_targetExes)
            {
                if (name == target)
                {
                    g_targetPids.push_back(pe.th32ProcessID);
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    g_statsTargetPid.store(g_targetPid ? g_targetPid :
        (g_targetPids.empty() ? 0 : g_targetPids.front()), std::memory_order_release);
}

static UINT QueryDisplayRefreshHz()
{
    UINT hz = 60;
    HMONITOR monitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoA(monitor, &mi))
    {
        DEVMODEA dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 1000)
            hz = dm.dmDisplayFrequency;
    }
    return hz;
}

static const char* PerfTierName(PerfTier t)
{
    switch (t)
    {
    case PerfTier::Low:      return "low";
    case PerfTier::Balanced: return "balanced";
    case PerfTier::High:     return "high";
    default:                 break;
    }
    return "auto";
}

static void ApplyPerfProfile()
{
    PerfTier tier = g_requestedPerfTier;
    if (tier == PerfTier::Auto)
    {


        if (g_gpuSoftware || g_gpuDedicatedMB < 4096)
            tier = PerfTier::Low;
        else if (g_gpuDedicatedMB < 8192)
            tier = PerfTier::Balanced;
        else
            tier = PerfTier::High;
    }
    g_displayRefreshHz = QueryDisplayRefreshHz();

    g_depthHeuristicDraws = 12;

    g_fastDepthFinal = !g_reshadeCompatibility || (g_requestedPerfTier == PerfTier::Low);





    switch (tier)
    {
    case PerfTier::Low:
        g_activePerfProfile = (g_requestedPerfTier == PerfTier::Auto) ? "Auto (Low / Stable)" : "Low / Stable";


        g_depthHeuristicDraws = 1;
        g_fastDepthFinal = true;
        g_skipRedundantDepthDraw = true;
        g_renderUsePointSampler = true;
        g_samplerAnisotropy = 1;


        g_statsSampleIntervalMs = 4000;
        g_vramQueryIntervalMs = 10000;
        g_pidScanIntervalMs = 8000;
        g_autoDepthRebuildIntervalMs = 3000;
        g_cursorPollIntervalMs = 50;
        g_resourceUpdateStrideFrames = 2;


        g_menuActiveFps = 30;
        g_menuIdleFps = 8;
        g_counterUiFps = 4;



        break;

    case PerfTier::Balanced:
        g_activePerfProfile = (g_requestedPerfTier == PerfTier::Auto) ? "Auto (Balanced)" : "Balanced";


        g_depthHeuristicDraws = 4;
        g_fastDepthFinal = false;
        g_skipRedundantDepthDraw = true;
        g_renderUsePointSampler = false;
        g_samplerAnisotropy = 2;


        g_statsSampleIntervalMs = 1500;
        g_vramQueryIntervalMs = 3000;
        g_pidScanIntervalMs = 6000;
        g_autoDepthRebuildIntervalMs = 1000;
        g_cursorPollIntervalMs = 16;
        g_resourceUpdateStrideFrames = 1;


        g_menuActiveFps = std::min<UINT>(60, g_displayRefreshHz);
        g_menuIdleFps = 15;
        g_counterUiFps = 8;
        break;

    case PerfTier::High:
        g_activePerfProfile = (g_requestedPerfTier == PerfTier::Auto) ? "Auto (High / Smooth)" : "High / Smooth";


        g_depthHeuristicDraws = 12;
        g_fastDepthFinal = false;
        g_skipRedundantDepthDraw = false;
        g_renderUsePointSampler = false;
        g_samplerAnisotropy = 4;


        g_statsSampleIntervalMs = 500;
        g_vramQueryIntervalMs = 750;
        g_pidScanIntervalMs = 4000;
        g_autoDepthRebuildIntervalMs = 0;
        g_cursorPollIntervalMs = 0;
        g_resourceUpdateStrideFrames = 0;


        g_menuActiveFps = std::min<UINT>(240, g_displayRefreshHz);
        g_menuIdleFps = 30;
        g_counterUiFps = 20;
        break;

    default:
        g_activePerfProfile = "Auto";
        break;
    }



    if (!g_overlayFpsExplicit)
        g_maxOverlayFps = std::clamp<UINT>(g_displayRefreshHz, 30u, 360u);

    if (g_reshadeCompatibility && !g_disableDepthHeuristic) g_depthHeuristicDraws = 12;
    if (g_disableDepthHeuristic || !g_reshadeCompatibility) g_depthHeuristicDraws = 0;
     
     
    if (HasArg("/lean-render"))
    {
        g_depthHeuristicDraws = 0;
        g_fastDepthFinal = true;
        g_skipRedundantDepthDraw = true;
    }
    if (g_reshadeIniPath[0]) WriteReShadeConfig();
    g_overlayFrameCostMs = 0.0;
    Log("[Performance] profile=%s (tier=%d, requested=%d), overlayMaxFps=%u, depthHeuristic=%d, fastDepth=%d, VRAM=%zu MB",
        g_activePerfProfile, (int)tier, (int)g_requestedPerfTier,
        g_maxOverlayFps, g_depthHeuristicDraws, (int)g_fastDepthFinal, g_gpuDedicatedMB);
}

static void QueryGpuNameFromDevice()
{
    if (!g_dev) return;
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) || !dxgiDevice)
        return;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter)
    {
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(adapter->GetDesc(&desc)))
        {
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            if (name[0]) g_gpuName = name;
            g_gpuDedicatedMB = desc.DedicatedVideoMemory / (1024u * 1024u);
            g_displayRefreshHz = QueryDisplayRefreshHz();
            g_gpuSoftware = (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C);
            ApplyPerfProfile();
        }
        if (!g_adapter3)
            adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_adapter3);
        adapter->Release();
    }
    dxgiDevice->Release();
}

static void UpdateSourceFPS()
{
    using Clock = std::chrono::steady_clock;
    static bool s_started = false;
    static uint64_t s_windowStartPacket = 0;
    static auto s_windowStartTime = Clock::now();
    static float s_smoothedFPS = 0.0f;

    const DWORD packetTick = g_lastPayloadTick.load(std::memory_order_acquire);
    const DWORD nowTick = GetTickCount();
    const bool validSource = g_pipeConnected.load(std::memory_order_acquire) &&
        g_hasPayload.load(std::memory_order_acquire) && packetTick &&
        nowTick - packetTick <= 750;
    if (!validSource)
    {
        s_started = false;
        s_smoothedFPS = 0.0f;
        g_fps = 0.0f;
        g_sourceFpsAvailable = false;
        return;
    }

    const uint64_t packetSerial = g_sourcePacketSerial.load(std::memory_order_acquire);
    const auto now = Clock::now();
    if (!s_started)
    {
        s_started = true;
        s_windowStartPacket = packetSerial;
        s_windowStartTime = now;
        g_sourceFpsAvailable = false;
        return;
    }

    const float elapsed = std::chrono::duration<float>(now - s_windowStartTime).count();
    if (elapsed < 0.075f) return;

    const uint64_t frameDelta = packetSerial - s_windowStartPacket;
    s_windowStartPacket = packetSerial;
    s_windowStartTime = now;

    if (frameDelta < 10000ull && elapsed < 2.0f)
    {
        const float measured = (float)frameDelta / elapsed;
        if (s_smoothedFPS <= 0.0f) s_smoothedFPS = measured;

        else s_smoothedFPS = s_smoothedFPS * 0.5f + measured * 0.5f;
        g_fps = s_smoothedFPS;
        g_sourceFpsAvailable = true;
    }
    else
    {
        s_smoothedFPS = 0.0f;
        g_sourceFpsAvailable = false;
    }
}

static void RecordPerformanceStats(double renderCostMs)
{
    if (renderCostMs < 0.0 || renderCostMs >= 1000.0) return;

    const DWORD now = GetTickCount();
    static DWORD lastSampleTick = 0;
    static double accumulatedMs = 0.0;
    static int accumulatedFrames = 0;
    accumulatedMs += renderCostMs;
    ++accumulatedFrames;

    if (now - lastSampleTick < g_statsSampleIntervalMs.load(std::memory_order_relaxed)) return;
    lastSampleTick = now;
    const double sampleMs = accumulatedMs / (double)std::max(1, accumulatedFrames);
    accumulatedMs = 0.0;
    accumulatedFrames = 0;

    if (g_overlayFrameCostMs <= 0.0) g_overlayFrameCostMs = sampleMs;
    else g_overlayFrameCostMs = g_overlayFrameCostMs * 0.82 + sampleMs * 0.18;

    g_overlayFrameHistory[g_perfHistoryOffset] = (float)sampleMs;
    g_perfHistoryOffset = (g_perfHistoryOffset + 1) % kPerfHistorySize;
    g_perfHistoryCount = std::min(g_perfHistoryCount + 1, kPerfHistorySize);
}

static const char* VkKeyName(int vk)
{
    static char name[64];
    name[0] = 0;
    UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(scan << 16);
    switch (vk)
    {
    case VK_INSERT: case VK_DELETE: case VK_END:
    case VK_PRIOR:  case VK_NEXT:  case VK_HOME:
    case VK_LEFT:   case VK_RIGHT: case VK_UP: case VK_DOWN:
        lp |= (1 << 24);
        break;
    default: break;
    }
    if (GetKeyNameTextA(lp, name, sizeof(name)) > 0)
        return name;
    snprintf(name, sizeof(name), "VK_%02X", vk & 0xff);
    return name;
}

struct IniSection
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> kv;
};

static std::string TrimStr(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static int MenuKeyTokenToVk(const std::string& raw)
{
    const std::string key = LowerAscii(TrimStr(raw));
    if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
        return 'A' + (key[0] - 'a');
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
        return '0' + (key[0] - '0');
    if (key.size() >= 2 && key[0] == 'f')
    {
        const int number = atoi(key.c_str() + 1);
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    if (key == "tab") return VK_TAB;
    if (key == "insert" || key == "ins") return VK_INSERT;
    if (key == "delete" || key == "del") return VK_DELETE;
    if (key == "home") return VK_HOME;
    if (key == "end") return VK_END;
    if (key == "pageup" || key == "pgup") return VK_PRIOR;
    if (key == "pagedown" || key == "pgdn") return VK_NEXT;
    if (key == "space") return VK_SPACE;
    if (key == "enter" || key == "return") return VK_RETURN;
    if (key == "escape" || key == "esc") return VK_ESCAPE;
    if (key == "backspace") return VK_BACK;
    if (key == "up") return VK_UP;
    if (key == "down") return VK_DOWN;
    if (key == "left") return VK_LEFT;
    if (key == "right") return VK_RIGHT;
    return 0;
}

static bool ParseMenuBinding(const char* text)
{
    if (!text || !text[0]) return false;
    bool shift = false, ctrl = false, alt = false;
    int keyVk = 0;
    std::string binding = text;
    size_t start = 0;
    while (start <= binding.size())
    {
        const size_t plus = binding.find('+', start);
        const std::string token = LowerAscii(TrimStr(binding.substr(start,
            plus == std::string::npos ? std::string::npos : plus - start)));
        if (token == "shift") shift = true;
        else if (token == "ctrl" || token == "control") ctrl = true;
        else if (token == "alt") alt = true;
        else
        {
            const int parsed = MenuKeyTokenToVk(token);
            if (!parsed || keyVk) return false;
            keyVk = parsed;
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (!keyVk) return false;
    g_menuKey = keyVk;
    g_menuNeedsShift = shift;
    g_menuNeedsCtrl = ctrl;
    g_menuNeedsAlt = alt;
    return true;
}

static std::string MenuBindingText()
{
    std::string out;
    if (g_menuNeedsCtrl) out += "CTRL+";
    if (g_menuNeedsShift) out += "SHIFT+";
    if (g_menuNeedsAlt) out += "ALT+";

    if (g_menuKey >= 'A' && g_menuKey <= 'Z')
        out.push_back((char)g_menuKey);
    else if (g_menuKey >= '0' && g_menuKey <= '9')
        out.push_back((char)g_menuKey);
    else if (g_menuKey >= VK_F1 && g_menuKey <= VK_F24)
        out += "F" + std::to_string(g_menuKey - VK_F1 + 1);
    else
    {
        switch (g_menuKey)
        {
        case VK_TAB: out += "TAB"; break;
        case VK_INSERT: out += "INSERT"; break;
        case VK_DELETE: out += "DELETE"; break;
        case VK_HOME: out += "HOME"; break;
        case VK_END: out += "END"; break;
        case VK_PRIOR: out += "PAGEUP"; break;
        case VK_NEXT: out += "PAGEDOWN"; break;
        case VK_SPACE: out += "SPACE"; break;
        case VK_RETURN: out += "ENTER"; break;
        case VK_ESCAPE: out += "ESCAPE"; break;
        case VK_BACK: out += "BACKSPACE"; break;
        case VK_UP: out += "UP"; break;
        case VK_DOWN: out += "DOWN"; break;
        case VK_LEFT: out += "LEFT"; break;
        case VK_RIGHT: out += "RIGHT"; break;
        default: out += "TAB"; break;
        }
    }
    return out;
}

static std::vector<IniSection> IniRead(FILE* f)
{
    std::vector<IniSection> secs;
    if (!f) return secs;
    char line[4096];
    IniSection* cur = nullptr;
    while (fgets(line, sizeof(line), f))
    {
        std::string s = TrimStr(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']')
        {
            secs.push_back(IniSection{ s.substr(1, s.size() - 2), {} });
            cur = &secs.back();
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        if (!cur) { secs.push_back(IniSection{ "", {} }); cur = &secs.back(); }
        cur->kv.emplace_back(TrimStr(s.substr(0, eq)), TrimStr(s.substr(eq + 1)));
    }
    fclose(f);
    return secs;
}

static std::vector<IniSection> IniLoad(const char* path)
{
    return IniRead(fopen(path, "rb"));
}
static std::vector<IniSection> IniLoadWide(const std::wstring& path)
{
    return IniRead(_wfopen(path.c_str(), L"rb"));
}
static std::wstring OverlaySiblingPath(const char* filename)
{
    wchar_t executable[32768] = {};
    DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    if (!count || count >= 32768) return std::wstring();
    std::wstring path(executable, count);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    path.resize(slash + 1);
    path.append(filename, filename + strlen(filename));
    return path;
}

static IniSection& IniSec(std::vector<IniSection>& secs, const char* name)
{
    for (auto& s : secs)
        if (_stricmp(s.name.c_str(), name) == 0)
            return s;
    secs.push_back(IniSection{ name, {} });
    return secs.back();
}

static const char* IniGet(std::vector<IniSection>& secs, const char* sec, const char* key, const char* def = "")
{
    for (auto& s : secs)
        if (_stricmp(s.name.c_str(), sec) == 0)
            for (auto& p : s.kv)
                if (_stricmp(p.first.c_str(), key) == 0)
                    return p.second.c_str();
    return def;
}

static void IniSet(std::vector<IniSection>& secs, const char* sec, const char* key, const std::string& val)
{
    IniSection& s = IniSec(secs, sec);
    for (auto& p : s.kv)
        if (_stricmp(p.first.c_str(), key) == 0) { p.second = val; return; }
    s.kv.emplace_back(key, val);
}

static bool IniSave(const char* path, const std::vector<IniSection>& secs)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    for (const auto& s : secs)
    {
        if (!s.name.empty()) fprintf(f, "[%s]\n", s.name.c_str());
        for (const auto& p : s.kv)
            fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}

static void MergeDefine(std::vector<std::string>& defs, const std::string& key, const std::string& value)
{
    for (auto& d : defs)
    {
        size_t eq = d.find('=');
        std::string k = (eq == std::string::npos) ? d : d.substr(0, eq);
        if (_stricmp(TrimStr(k).c_str(), key.c_str()) == 0)
        {
            d = key + "=" + value;
            return;
        }
    }
    defs.push_back(key + "=" + value);
}
static void WriteReShadeConfig()
{
    if (!g_reshadeIniPath[0]) return;

     
     
    const bool compatibility = g_reshadeCompatibility && !g_disableDepthHeuristic;
    if (!compatibility)
        Log("[ReShade] fast scene path enabled; keeping depth heuristics disabled");

    std::vector<IniSection> ini = IniLoad(g_reshadeIniPath);



    static bool loggedIniKeys = false;
    if (!loggedIniKeys) { Log("[Keybinds] QWLEY_OVERLAY_INI_KEYS_V1"); loggedIniKeys = true; }
    if (!IniGet(ini, "INPUT", "KeyOverlay", "")[0])
        IniSet(ini, "INPUT", "KeyOverlay", "36,0,0,0");

    IniSet(ini, "DEPTH", "DepthCopyBeforeClears", "0");
    IniSet(ini, "DEPTH", "DepthCopyAtClearIndex", "0");
    IniSet(ini, "DEPTH", "DisableINTZ", "0");
    IniSet(ini, "DEPTH", "UseAspectRatioHeuristics", compatibility ? "1" : "0");
    IniSet(ini, "DEPTH", "DrawStatsHeuristic", compatibility ? "2" : "0");
    std::string cur = IniGet(ini, "GENERAL", "PreprocessorDefinitions", "");
    std::vector<std::string> defs;
    {
        size_t start = 0;
        while (start <= cur.size())
        {
            size_t c = cur.find(',', start);
            std::string item = TrimStr(cur.substr(start, c == std::string::npos ? std::string::npos : c - start));
            if (!item.empty()) defs.push_back(item);
            if (c == std::string::npos) break;
            start = c + 1;
        }
    }

    char farbuf[64];
    snprintf(farbuf, sizeof(farbuf), "%.1f", g_depthFarPlane);
    MergeDefine(defs, "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", farbuf);
    MergeDefine(defs, "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", g_depthUpsideDown ? "1" : "0");
    MergeDefine(defs, "RESHADE_DEPTH_INPUT_IS_REVERSED", g_depthReversed ? "1" : "0");
    MergeDefine(defs, "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", g_depthLogarithmic ? "1" : "0");

    std::string joined;
    for (size_t i = 0; i < defs.size(); ++i)
    {
        if (i) joined += ",";
        joined += defs[i];
    }
    IniSet(ini, "GENERAL", "PreprocessorDefinitions", joined);


    IniSet(ini, "GENERAL", "PerformanceMode", "1");

    IniSet(ini, "DEPTH", "DepthCopyBeforeClears", "0");
    IniSet(ini, "DEPTH", "DepthCopyAtClearIndex", "0");

    IniSet(ini, "GENERAL", "NoReloadOnAssetsChange", "1");

    if (IniSave(g_reshadeIniPath, ini))
        Log("[ReShade] config written (%s), reversed=%d upsideDown=%d log=%d",
            g_reshadeIniPath, (int)g_depthReversed, (int)g_depthUpsideDown, (int)g_depthLogarithmic);
    else
        Log("[ReShade] cannot write %s", g_reshadeIniPath);
}
static ImVec4 ParseAppearanceColor(const char* value, const ImVec4& fallback)
{
    if (!value || strlen(value) != 7 || value[0] != '#') return fallback;
    for (int i = 1; i < 7; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') ||
            (value[i] >= 'A' && value[i] <= 'F'))) return fallback;
    unsigned color = 0;
    if (sscanf(value + 1, "%x", &color) != 1) return fallback;
    return ImVec4(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, 1);
}

static void LoadOverlayBackground()
{
    SafeRelease(g_backgroundSrv);
    g_backgroundWidth = g_backgroundHeight = 0;
    if (!g_dev || g_appearance.backgroundImage.empty()) return;

    if (g_appearance.backgroundImage.find_first_of("/\\:") != std::string::npos ||
        g_appearance.backgroundImage.find("..") != std::string::npos) {
        Log("[Appearance] Background must be a local filename beside the overlay."); return;
    }
    const std::wstring path = OverlaySiblingPath(g_appearance.backgroundImage.c_str());
    if (path.empty()) return;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory, reinterpret_cast<void**>(&factory));
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (SUCCEEDED(hr) && (!width || !height || width > 4096 || height > 4096)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data = {}; data.pSysMem = pixels.data(); data.SysMemPitch = width * 4;
            hr = g_dev->CreateTexture2D(&desc, &data, &texture);
            if (SUCCEEDED(hr)) hr = g_dev->CreateShaderResourceView(texture, nullptr, &g_backgroundSrv);
            if (SUCCEEDED(hr)) { g_backgroundWidth = width; g_backgroundHeight = height; }
        }
    }
    if (FAILED(hr)) Log("[Appearance] Cannot decode background (%08lx); using solid color.", static_cast<unsigned long>(hr));
    SafeRelease(texture); SafeRelease(converter); SafeRelease(frame); SafeRelease(decoder); SafeRelease(factory);
    if (SUCCEEDED(co)) CoUninitialize();
}

static OverlayAppearance ReadOverlayAppearance(std::vector<IniSection>& ini)
{
    OverlayAppearance a;
    a.background = ParseAppearanceColor(IniGet(ini, "Appearance", "BackgroundColor", "#141414"), a.background);
    a.accent = ParseAppearanceColor(IniGet(ini, "Appearance", "AccentColor", "#261f18"), a.accent);
    float opacity = (float)atof(IniGet(ini, "Appearance", "Opacity", "0.96"));
    float darkness = (float)atof(IniGet(ini, "Appearance", "BackgroundDim", "0.35"));
    a.opacity = std::isfinite(opacity) ? std::max(0.20f, std::min(1.0f, opacity)) : 0.96f;
    a.backgroundDim = std::isfinite(darkness) ? std::max(0.0f, std::min(0.90f, darkness)) : 0.35f;
    a.showControls = atoi(IniGet(ini, "Appearance", "ShowControls", "1")) != 0;
    a.backgroundImage = IniGet(ini, "Appearance", "BackgroundImage", "");
    return a;
}
static uint64_t AppearanceFileStamp(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER value = {};
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return value.QuadPart;
}
static void PollOverlayAppearance()
{
    static bool loggedCapability = false;
    if (!loggedCapability) { Log("[Appearance] QWLEY_OVERLAY_LIVE_APPEARANCE_V1"); loggedCapability = true; }

    static DWORD lastPoll = 0;
    DWORD tick = GetTickCount();
    if (lastPoll && tick - lastPoll < 250) return;
    lastPoll = tick;
    static uint64_t lastPreviewStamp = 0, lastSelectedStamp = UINT64_MAX, lastImageStamp = 0;
    static DWORD ownerPid = 0;
    static bool usingPreview = false;
    static std::vector<IniSection> previewIni;
    const std::wstring savedPath = OverlaySiblingPath("overlay_settings.ini");
    const std::wstring previewPath = OverlaySiblingPath(".qwley_overlay_preview.ini");
    const uint64_t previewStamp = AppearanceFileStamp(previewPath);
    if (previewStamp != lastPreviewStamp) {
        previewIni = previewStamp ? IniLoadWide(previewPath) : std::vector<IniSection>();
        ownerPid = (DWORD)strtoul(IniGet(previewIni, "Preview", "OwnerPid", "0"), nullptr, 10);
        lastPreviewStamp = previewStamp;
    }

    const bool activePreview = previewStamp && ownerPid && IsProcessAlive(ownerPid);
    const uint64_t selectedStamp = activePreview ? previewStamp : AppearanceFileStamp(savedPath);
    const uint64_t currentImageStamp = g_appearance.backgroundImage.empty() ? 0 :
        AppearanceFileStamp(OverlaySiblingPath(g_appearance.backgroundImage.c_str()));
    if (activePreview == usingPreview && selectedStamp == lastSelectedStamp && currentImageStamp == lastImageStamp) return;
    std::vector<IniSection> ini = activePreview ? previewIni : IniLoadWide(savedPath);
    const OverlayAppearance next = ReadOverlayAppearance(ini);
    const uint64_t nextImageStamp = next.backgroundImage.empty() ? 0 :
        AppearanceFileStamp(OverlaySiblingPath(next.backgroundImage.c_str()));
    const bool reloadImage = next.backgroundImage != g_appearance.backgroundImage || nextImageStamp != lastImageStamp;
    g_appearance = next;
    if (reloadImage) LoadOverlayBackground();
    if (ImGui::GetCurrentContext()) {
        ImVec4* colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(g_appearance.background.x, g_appearance.background.y,
            g_appearance.background.z, g_appearance.opacity);
        colors[ImGuiCol_ButtonActive] = g_appearance.accent;
        colors[ImGuiCol_HeaderActive] = g_appearance.accent;
    }
    usingPreview = activePreview;
    lastSelectedStamp = selectedStamp;
    lastImageStamp = nextImageStamp;
}

static void LoadSettings()
{
    if (!g_iniPath[0]) return;

    std::vector<IniSection> ini = IniLoadWide(OverlaySiblingPath("overlay_settings.ini"));
    if (ini.empty()) return;
    g_appearance = ReadOverlayAppearance(ini);
    const int savedLayoutVersion = atoi(IniGet(ini, "UI", "LayoutVersion", "0"));
    g_uiLayoutReset = savedLayoutVersion < 6;
    const bool resetLegacyFpsColor = savedLayoutVersion < 4;
    g_showKeyHelp = atoi(IniGet(ini, "UI", "HasOpenedMenu", "0")) == 0;

    int v = atoi(IniGet(ini, "Keybinds", "Minimize", "0"));
    if (v > 0 && v < 256) g_minimizeKey = v;
    v = atoi(IniGet(ini, "Keybinds", "DepthPreview", "0"));
    if (v > 0 && v < 256) g_depthPreviewKey = v;
    ParseMenuBinding(IniGet(ini, "Keybinds", "Menu", "SHIFT+TAB"));

    g_showFPS = atoi(IniGet(ini, "Style", "ShowFPS", "1")) != 0;
    const char* fpsX = IniGet(ini, "Style", "FpsPosX", "");
    const char* fpsY = IniGet(ini, "Style", "FpsPosY", "");
    if (fpsX[0]) g_fpsOffsetX = atoi(fpsX);
    if (fpsY[0]) g_fpsOffsetY = atoi(fpsY);
    if (!resetLegacyFpsColor)
    {
        float r = 0, g = 0, b = 0, a = 0;
        if (sscanf(IniGet(ini, "Style", "FpsColor", ""), "%f,%f,%f,%f", &r, &g, &b, &a) == 4)
        {
            g_fpsColor[0] = std::max(0.0f, std::min(1.0f, r));
            g_fpsColor[1] = std::max(0.0f, std::min(1.0f, g));
            g_fpsColor[2] = std::max(0.0f, std::min(1.0f, b));
            g_fpsColor[3] = std::max(0.0f, std::min(1.0f, a));
        }
    }
    else
    {
        g_fpsColor[0] = 1.0f;
        g_fpsColor[1] = 1.0f;
        g_fpsColor[2] = 1.0f;
        g_fpsColor[3] = 1.0f;
    }

    g_depthReversed = atoi(IniGet(ini, "Depth", "Reversed", "1")) != 0;
    g_depthUpsideDown = atoi(IniGet(ini, "Depth", "UpsideDown", "0")) != 0;
    g_depthLogarithmic = atoi(IniGet(ini, "Depth", "Logarithmic", "0")) != 0;
    g_useViewportRemap = atoi(IniGet(ini, "Depth", "ViewportRemap", "1")) != 0;
    {
        float fp = (float)atof(IniGet(ini, "Depth", "FarPlane", "1000.0"));
        if (fp > 1.0f) g_depthFarPlane = fp;
    }

    if (!g_perfTierFromCmdline)
    {
        const char* prof = IniGet(ini, "Performance", "Profile", "");
        if (_stricmp(prof, "low") == 0)                             g_requestedPerfTier = PerfTier::Low;
        else if (_stricmp(prof, "balanced") == 0 ||
            _stricmp(prof, "medium") == 0)                     g_requestedPerfTier = PerfTier::Balanced;
        else if (_stricmp(prof, "high") == 0)                       g_requestedPerfTier = PerfTier::High;
        else if (_stricmp(prof, "auto") == 0)                       g_requestedPerfTier = PerfTier::Auto;
    }

}

static void SaveSettings()
{
    if (!g_iniPath[0]) return;
    std::vector<IniSection> ini = IniLoad(g_iniPath);
    IniSet(ini, "Keybinds", "Minimize", std::to_string(g_minimizeKey));
    IniSet(ini, "Keybinds", "DepthPreview", std::to_string(g_depthPreviewKey));
    IniSet(ini, "Keybinds", "Menu", MenuBindingText());
    IniSet(ini, "UI", "LayoutVersion", g_uiLayoutReset ? "0" : "6");
    IniSet(ini, "UI", "HasOpenedMenu", g_showKeyHelp ? "0" : "1");
    IniSet(ini, "Performance", "Profile", PerfTierName(g_requestedPerfTier));
    IniSet(ini, "Performance", "OverlayLimit", "0");
    IniSet(ini, "Style", "ShowFPS", g_showFPS ? "1" : "0");
    IniSet(ini, "Style", "FpsPosX", std::to_string(g_fpsOffsetX));
    IniSet(ini, "Style", "FpsPosY", std::to_string(g_fpsOffsetY));
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%.6f,%.6f,%.6f,%.6f",
            g_fpsColor[0], g_fpsColor[1], g_fpsColor[2], g_fpsColor[3]);
        IniSet(ini, "Style", "FpsColor", buf);
    }
    IniSet(ini, "Depth", "Reversed", g_depthReversed ? "1" : "0");
    IniSet(ini, "Depth", "UpsideDown", g_depthUpsideDown ? "1" : "0");
    IniSet(ini, "Depth", "Logarithmic", g_depthLogarithmic ? "1" : "0");
    IniSet(ini, "Depth", "ViewportRemap", g_useViewportRemap ? "1" : "0");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f", g_depthFarPlane);
        IniSet(ini, "Depth", "FarPlane", buf);
    }
    IniSave(g_iniPath, ini);

    WriteReShadeConfig();
}

static bool CaptureKeyIfWaiting()
{
    if (!g_waitKeyTarget) return false;
    if (GetTickCount() - g_waitKeyStart < 250) return true;

    for (int vk = 8; vk < 256; ++vk)
    {
        if (!(GetAsyncKeyState(vk) & 1)) continue;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
            vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
            continue;
        if (vk == VK_ESCAPE) { g_waitKeyTarget = 0; return true; }
        if (g_waitKeyTarget == 1) g_minimizeKey = vk;
        if (g_waitKeyTarget == 2) g_depthPreviewKey = vk;
        g_waitKeyTarget = 0;
        SaveSettings();
        return true;
    }
    return true;
}

static float pdh_sum_by_pid(PDH_HCOUNTER counter, DWORD pid, bool busiestEngine = false)
{
    if (!counter || !pid) return NAN;
    static thread_local std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> storage;
    DWORD bytes = 0, count = 0;
    const DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, nullptr);
    if (status != PDH_MORE_DATA || !bytes) return NAN;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
    for (int retry = 0; retry < 3; ++retry)
    {
        storage.resize((bytes + sizeof(*items) - 1) / sizeof(*items));
        items = storage.data();
        DWORD capacity = static_cast<DWORD>(storage.size() * sizeof(*items));
        status = PdhGetFormattedCounterArrayW(counter, format, &capacity, &count, items);
        bytes = capacity;
        if (status != PDH_MORE_DATA) break;
    }
    if (status != ERROR_SUCCESS) return NAN;
    wchar_t prefix[32] = {};
    swprintf_s(prefix, L"pid_%lu_", pid);
    const size_t prefixLen = wcslen(prefix);
    double value = 0.0;
    bool valid = false;
    for (DWORD i = 0; i < count; ++i)
    {
        const auto& item = items[i];
        if (!item.szName || wcsncmp(item.szName, prefix, prefixLen) != 0) continue;
        if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        const double v = item.FmtValue.doubleValue;
        if (!std::isfinite(v) || v < 0.0) continue;
        value = busiestEngine ? std::max(value, v) : value + v;
        valid = true;
    }
    return valid ? static_cast<float>(value) : NAN;
}

static void PdhThread()
{
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER dedicated = nullptr, shared = nullptr, gpu = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS)
    {

        if (PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &dedicated) != ERROR_SUCCESS)
            dedicated = nullptr;
        if (PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &shared) != ERROR_SUCCESS)
            shared = nullptr;
        if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu) != ERROR_SUCCESS)
            gpu = nullptr;
        if (dedicated || shared || gpu) PdhCollectQueryData(query);
    }
    const double cores = static_cast<double>(std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    HANDLE target = nullptr;
    DWORD targetPid = 0;
    uint64_t previousTarget = 0, previousOverlay = 0;
    bool targetPrimed = false, overlayPrimed = false;
    auto lastSample = std::chrono::steady_clock::now();
    auto nextTargetProbe = std::chrono::steady_clock::time_point::min();

    const auto cpuTime = [](HANDLE process, uint64_t& ticks) -> bool {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
        ticks = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
            ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
        return true;
        };
    while (!g_pdh_stop.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> waitLock(g_pdhWaitMutex);
        if (g_pdhWaitCv.wait_for(waitLock, std::chrono::milliseconds(
            g_statsVisible.load(std::memory_order_relaxed)
            ? std::max<DWORD>(500, g_statsSampleIntervalMs.load(std::memory_order_relaxed))
            : 1000),
            [] { return g_pdh_stop.load(std::memory_order_acquire); })) break;
        waitLock.unlock();

        if (!g_statsVisible.load(std::memory_order_acquire))
        {
            targetPrimed = overlayPrimed = false;
            lastSample = std::chrono::steady_clock::now();
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastSample).count();
        lastSample = now;
        DWORD pid = g_statsTargetPid.load(std::memory_order_acquire);
        if (target && WaitForSingleObject(target, 0) != WAIT_TIMEOUT)
        {
            CloseHandle(target); target = nullptr; targetPrimed = false;
        }
        if (pid != targetPid)
        {
            if (target) CloseHandle(target);
            target = nullptr; targetPrimed = false; targetPid = pid;
            nextTargetProbe = std::chrono::steady_clock::time_point::min();
        }
        if (pid && !target && now >= nextTargetProbe)
        {
            target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            nextTargetProbe = now + std::chrono::seconds(2);
        }
        pdh_process_stats_t stats;
        stats.valid = target && WaitForSingleObject(target, 0) == WAIT_TIMEOUT;
        uint64_t ticks = 0;
        if (stats.valid && cpuTime(target, ticks))
        {
            stats.cpu_valid = targetPrimed && ticks >= previousTarget && elapsed > 0.0;
            if (stats.cpu_valid) stats.roblox_cpu = static_cast<float>(
                std::clamp((ticks - previousTarget) / (elapsed * 100000.0 * cores), 0.0, 100.0));
            previousTarget = ticks; targetPrimed = true;
        }
        else targetPrimed = false;
        if (cpuTime(GetCurrentProcess(), ticks))
        {
            stats.overlay_cpu_valid = overlayPrimed && ticks >= previousOverlay && elapsed > 0.0;
            if (stats.overlay_cpu_valid) stats.overlay_cpu = static_cast<float>(
                std::clamp((ticks - previousOverlay) / (elapsed * 100000.0 * cores), 0.0, 100.0));
            previousOverlay = ticks; overlayPrimed = true;
        }
        else overlayPrimed = false;
        if (stats.valid && query && (dedicated || shared || gpu) &&
            PdhCollectQueryData(query) == ERROR_SUCCESS)
        {
            const float d = pdh_sum_by_pid(dedicated, pid);
            const float m = pdh_sum_by_pid(shared, pid);
            const float u = pdh_sum_by_pid(gpu, pid, true);
            stats.dedicated_valid = std::isfinite(d);
            stats.shared_valid = std::isfinite(m);
            stats.gpu_valid = std::isfinite(u);
            if (stats.dedicated_valid) stats.roblox_vram_mb = d / (1024.f * 1024.f);
            if (stats.shared_valid) stats.roblox_shared_mb = m / (1024.f * 1024.f);
            if (stats.gpu_valid) stats.roblox_gpu_pct = std::clamp(u, 0.f, 100.f);
        }
        std::lock_guard<std::mutex> lk(g_process_stats_mutex);
        g_process_stats = stats;
    }
    if (target) CloseHandle(target);
    if (query) PdhCloseQuery(query);
}

 
 
static void NotifyFrameWake()
{
    if (g_frameWakeEvent) SetEvent(g_frameWakeEvent);
}

static void WaitForRenderWork(std::chrono::steady_clock::time_point deadline,
    bool wakeForPacket = true)
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (deadline <= now) return;
    const auto remaining = deadline - now;
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (milliseconds < remaining) milliseconds += std::chrono::milliseconds(1);
    const DWORD timeout = (DWORD)std::min<long long>(milliseconds.count(), 1000);
    HANDLE event = g_frameWakeEvent;
    const DWORD count = wakeForPacket && event ? 1u : 0u;
    const DWORD result = MsgWaitForMultipleObjectsEx(count, count ? &event : nullptr,
        timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED) Sleep(std::min<DWORD>(timeout, 4));
}

 
static bool SameInputResources(const PipePayload& a, const PipePayload& b)
{
    return a.depthHandle == b.depthHandle && a.colorHandle == b.colorHandle &&
        a.depthWidth == b.depthWidth && a.depthHeight == b.depthHeight &&
        a.colorWidth == b.colorWidth && a.colorHeight == b.colorHeight &&
        a.depthFormat == b.depthFormat && a.colorFormat == b.colorFormat &&
        a.flags == b.flags && a.vpX == b.vpX && a.vpY == b.vpY &&
        a.vpWidth == b.vpWidth && a.vpHeight == b.vpHeight;
}

static bool ReadExact(HANDLE h, void* out, DWORD sz)
{
    BYTE* p = (BYTE*)out; DWORD total = 0;
    while (total < sz && !g_pipeStop.load())
    {
        DWORD br = 0;
        if (!ReadFile(h, p + total, sz - total, &br, nullptr) || br == 0) return false;
        total += br;
    }
    return total == sz;
}

static void PipeThread()
{
    Log("[Pipe] waiting for %ls", PIPE_NAME);
    while (!g_pipeStop.load())
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        while (h == INVALID_HANDLE_VALUE && !g_pipeStop.load())
        {
            h = CreateFileW(PIPE_NAME, GENERIC_READ, 0, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(PIPE_NAME, 500);
                else Sleep(300);
            }
        }
        if (g_pipeStop.load()) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); break; }
        g_pipeConnected.store(true, std::memory_order_release);
        g_pipeEverConnected.store(true, std::memory_order_release);
        g_pipeCv.notify_all();
        NotifyFrameWake();
        Log("[Pipe] connected");
        while (!g_pipeStop.load())
        {
            uint8_t raw[PAYLOAD_SIZE] = {};
            if (!ReadExact(h, raw, sizeof(raw))) break;
            PipePayload p = NormalizePayload(raw);
            {
                std::lock_guard<std::mutex> lk(g_pipeMutex);
                g_payload = p;
                g_lastPayloadTick.store(GetTickCount(), std::memory_order_release);
                g_hasPayload.store(true, std::memory_order_release);
                g_sourcePacketSerial.fetch_add(1, std::memory_order_release);
            }
            g_pipeCv.notify_all();
            NotifyFrameWake();
        }
        CloseHandle(h);
        {
            std::lock_guard<std::mutex> lk(g_pipeMutex);
            g_payload = PipePayload{};
            g_lastPayloadTick.store(0, std::memory_order_release);
            g_hasPayload.store(false, std::memory_order_release);
            g_pipeConnected.store(false, std::memory_order_release);
            g_sourcePacketSerial.fetch_add(1, std::memory_order_release);
        }
        g_pipeCv.notify_one();
        NotifyFrameWake();
        Log("[Pipe] disconnected");
        Sleep(300);
    }
}

struct RemapCB
{
    float uvScale[2];
    float uvBias[2];
    float misc[4];
};
static_assert(sizeof(RemapCB) == 32, "cb size");

struct CombinedRemapCB
{
    float colorUV[4];
    float depthUV[4];
    float misc[4];
};
static_assert(sizeof(CombinedRemapCB) == 48, "combined cb size");

template<class T> struct ConstantUploadCache
{
    T value{};
    ID3D11Buffer* buffer = nullptr;  
    uint64_t epoch = 0;
    bool valid = false;
};

template<class T> static bool UploadCachedConstants(ID3D11Buffer* buffer,
    const T& value, ConstantUploadCache<T>& cache)
{
    if (!buffer || !g_ctx) return false;
    if (cache.valid && cache.buffer == buffer && cache.epoch == g_constantBufferEpoch &&
        memcmp(&cache.value, &value, sizeof(T)) == 0) return true;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    memcpy(mapped.pData, &value, sizeof(T));
    g_ctx->Unmap(buffer, 0);
    cache.value = value;
    cache.buffer = buffer;
    cache.epoch = g_constantBufferEpoch;
    cache.valid = true;
    return true;
}

static const char* VS_SRC = R"HLSL(
struct O { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };
O main(uint id:SV_VertexID){
    O o;
    float2 uv = float2((id<<1)&2, id&2);
    o.uv = uv;
    o.p  = float4(uv*float2(2,-2)+float2(-1,1),0,1);
    return o;
}
)HLSL";

static const char* PS_COLOR = R"HLSL(
cbuffer P : register(b0) { float4 gUV; float4 gMisc; };
Texture2D<float4> T:register(t0);
SamplerState S:register(s0);
float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Target {
    float2 c = uv * gUV.xy + gUV.zw;
    float4 col = T.SampleLevel(S, c, 0);
    return float4(col.rgb, 1);
}
)HLSL";

static const char* PS_COLOR_DEPTH = R"HLSL(
cbuffer P : register(b0) {
    float4 gColorUV;
    float4 gDepthUV;
    float4 gMisc;
};
Texture2D<float4> ColorT : register(t0);
Texture2D<float> DepthT : register(t1);
SamplerState LinearS : register(s0);
SamplerState PointS : register(s1);
struct Out {
    float4 color : SV_Target;
    float depth : SV_Depth;
};
Out main(float4 p:SV_POSITION, float2 uv:TEXCOORD0) {
    Out o;
    const float2 colorUV = uv * gColorUV.xy + gColorUV.zw;
    const float2 depthUV = uv * gDepthUV.xy + gDepthUV.zw;
    o.color = float4(ColorT.SampleLevel(LinearS, colorUV, 0).rgb, 1.0);
    o.depth = saturate(DepthT.SampleLevel(PointS, depthUV, 0).r);
    return o;
}
)HLSL";

static const char* PS_DEPTH = R"HLSL(
cbuffer P : register(b0) { float4 gUV; float4 gMisc; };
Texture2D<float> T:register(t0);
SamplerState S:register(s0);
float main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Depth {
    float2 c = uv * gUV.xy + gUV.zw;
    float d = T.SampleLevel(S, c, 0).r;
    if (gMisc.x > 0.5) d = 1.0 - d;
    return saturate(d);
}
)HLSL";

static const char* PS_DEPTH_VIEW = R"HLSL(
cbuffer P : register(b0) { float4 gUV; float4 gMisc; };
Texture2D<float> T:register(t0);
SamplerState S:register(s0);
float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Target {
    float2 c = uv * gUV.xy + gUV.zw;
    float d = T.SampleLevel(S, c, 0).r;
    if (gMisc.x > 0.5) d = 1.0 - d;

    float f = max(gMisc.y, 1.0);
    float lin = 1.0 / (d * (1.0 - 1.0 / f) + (1.0 / f));
    lin = saturate(lin / f);
    return float4(lin, lin, lin, 1);
}
)HLSL";

static const char* PS_PREMULT = R"HLSL(
cbuffer P : register(b0) { float4 gUV; float4 gMisc; };
Texture2D<float4> T : register(t0);
SamplerState S : register(s0);
float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_Target {
    float2 c = uv * gUV.xy + gUV.zw;
    float4 s = T.SampleLevel(S, c, 0);
    return float4(s.rgb * s.a, s.a);
}
)HLSL";

static bool Compile(const char* src, const char* target, ID3DBlob** out)
{
    ID3DBlob* e = nullptr;

    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
        "main", target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_SKIP_VALIDATION,
        0, out, &e);
    if (FAILED(hr))
    {
        Log("[Shader] %s failed: %s", target, e ? (char*)e->GetBufferPointer() : "?");
        if (e) e->Release();
        return false;
    }
    if (e) e->Release();
    return true;
}

static bool CreateLocalDepth(UINT w, UINT h)
{
    SafeRelease(g_localDepthSRV);
    SafeRelease(g_localDSV);
    SafeRelease(g_localDepth);

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_localDepth)))
    {

        d.Format = DXGI_FORMAT_D32_FLOAT;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_localDepth)))
            return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_D32_FLOAT;
    v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(g_dev->CreateDepthStencilView(g_localDepth, &v, &g_localDSV)))
        return false;

    if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = DXGI_FORMAT_R32_FLOAT;
        s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        s.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(g_localDepth, &s, &g_localDepthSRV);
    }
    return true;
}

static ID3D11RenderTargetView* CreateBackBufferRTV()
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
        return nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    g_dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    return rtv;
}

static bool Resize(UINT w, UINT h)
{
    if (!w || !h) return false;
    if (w == g_width && h == g_height && g_rtv) return true;

    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_localDepthSRV); SafeRelease(g_localDSV); SafeRelease(g_localDepth);

    if (w != g_width || h != g_height)
    {
        SafeRelease(g_rtv);

        const UINT resizeFlags = g_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        HRESULT hr = g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, resizeFlags);
        if (FAILED(hr))
        {
            Log("[D3D] ResizeBuffers(%ux%u) failed 0x%08lX; retrying next frame",
                w, h, (unsigned long)hr);
            if (!g_rtv) g_rtv = CreateBackBufferRTV();
            CreateLocalDepth(g_width, g_height);
            return false;
        }
        g_width = w; g_height = h;
    }

    if (!g_rtv) g_rtv = CreateBackBufferRTV();
    if (g_depthPassEnabled)
        CreateLocalDepth(g_width, g_height);
    Log("[D3D] resized to %ux%u", w, h);
    return true;
}

static void EnsureBackBufferMatchesWindow()
{
    if (!g_hwnd || !g_swap || !g_ctx) return;

    RECT c = {};
    if (!GetClientRect(g_hwnd, &c)) return;
    const UINT cw = (UINT)std::max<LONG>(1, c.right - c.left);
    const UINT ch = (UINT)std::max<LONG>(1, c.bottom - c.top);

    if (cw == g_width && ch == g_height && g_rtv) return;

    Log("[Geometry] client %ux%u vs back buffer %ux%u -> correcting",
        cw, ch, g_width, g_height);
    Resize(cw, ch);
}

static bool OpenSharedTex(uint64_t handleValue, ID3D11Texture2D** out, HRESULT* outHr)
{
    *out = nullptr;
    HANDLE h = (HANDLE)(uintptr_t)handleValue;
    HRESULT hr = g_dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)out);
    if (FAILED(hr))
    {

        SafeRelease(*out);
        ID3D11Device1* device1 = nullptr;
        if (SUCCEEDED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1)))
        {
            const HRESULT ntHr = device1->OpenSharedResource1(h,
                __uuidof(ID3D11Texture2D), (void**)out);
            if (SUCCEEDED(ntHr)) hr = ntHr;
            device1->Release();
        }
    }
    if (outHr) *outHr = hr;
    return SUCCEEDED(hr) && *out;
}

static void ReleaseDepthInput()
{
    if (!g_depthSRV && !g_depthTex && !g_lastDepthHandle) return;
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    if (g_ctx) g_ctx->PSSetShaderResources(0, 2, nullSRVs);
    SafeRelease(g_depthSRV);
    SafeRelease(g_depthTex);
    g_depthTexW = g_depthTexH = 0;
    g_lastDepthHandle = 0;
    g_lastDepthW = g_lastDepthH = g_lastDepthFmt = 0;
}

static void ReleaseColorInput()
{
    if (!g_colorSRV && !g_colorTex && !g_colorSampleTex && !g_lastColorHandle) return;
    ID3D11ShaderResourceView* nullSRV = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSRV);
    SafeRelease(g_colorSRV);
    SafeRelease(g_colorSampleTex);
    SafeRelease(g_colorTex);
    g_colorNeedsResolve = false;
    g_colorResolveFormat = DXGI_FORMAT_UNKNOWN;
    g_colorTexW = g_colorTexH = 0;
    g_lastColorHandle = 0;
    g_lastColorW = g_lastColorH = g_lastColorFmt = 0;
}

static bool IsDepthFresh(const PipePayload& p)
{
    static uint32_t lastFrame = 0;
    static DWORD lastAdvanceTick = 0;
    const DWORD now = GetTickCount();
    const DWORD packetTick = g_lastPayloadTick.load(std::memory_order_acquire);

    if (!g_pipeConnected.load(std::memory_order_acquire) ||
        !g_hasPayload.load(std::memory_order_acquire) ||
        !packetTick || now - packetTick > 750 ||
        !(p.flags & FLAG_DEPTH_OK) || !p.depthHandle)
    {
        lastAdvanceTick = 0;
        lastFrame = p.frameCount;
        return false;
    }
    if (p.flags & FLAG_FPS_OK)
    {
        if (!lastAdvanceTick || p.frameCount != lastFrame)
        {
            lastFrame = p.frameCount;
            lastAdvanceTick = now;
        }
        else if (now - lastAdvanceTick > 750)
        {
            return false;
        }
    }
    return true;
}

static HRESULT CreateColorInputView()
{
    SafeRelease(g_colorSRV);
    SafeRelease(g_colorSampleTex);
    g_colorNeedsResolve = false;
    g_colorResolveFormat = DXGI_FORMAT_UNKNOWN;
    if (!g_colorTex) return E_POINTER;
    D3D11_TEXTURE2D_DESC td = {};
    g_colorTex->GetDesc(&td);
    g_colorTexW = td.Width; g_colorTexH = td.Height;
    if (!td.Width || !td.Height || td.ArraySize != 1) return E_INVALIDARG;
    D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = ColorSRVFormat(td.Format);
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    HRESULT hr = E_FAIL;
    if (td.SampleDesc.Count == 1 && (td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    {
        hr = g_dev->CreateShaderResourceView(g_colorTex, &view, &g_colorSRV);
        if (SUCCEEDED(hr)) return hr;
        SafeRelease(g_colorSRV);
    }
    D3D11_TEXTURE2D_DESC copy = td;
    copy.Usage = D3D11_USAGE_DEFAULT;
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.CPUAccessFlags = 0;
    copy.MiscFlags = 0;
    copy.SampleDesc.Count = 1; copy.SampleDesc.Quality = 0;
    if (td.SampleDesc.Count > 1)
    {
        UINT support = 0;
        hr = g_dev->CheckFormatSupport(view.Format, &support);
        if (FAILED(hr)) return hr;
        if (!(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) return E_NOTIMPL;
        copy.Format = view.Format;
        copy.MipLevels = 1;
        g_colorNeedsResolve = true;
        g_colorResolveFormat = view.Format;
    }
    hr = g_dev->CreateTexture2D(&copy, nullptr, &g_colorSampleTex);
    if (FAILED(hr)) return hr;
    hr = g_dev->CreateShaderResourceView(g_colorSampleTex, &view, &g_colorSRV);
    if (FAILED(hr)) SafeRelease(g_colorSampleTex);
    Log("[Color] sampleable GPU fallback: samples=%u bind=0x%X format=%u hr=0x%08lX",
        td.SampleDesc.Count, td.BindFlags, td.Format, (unsigned long)hr);
    return hr;
}

static void RefreshColorInputCopy()
{
    if (!g_colorSampleTex || !g_colorTex || !g_colorSRV) return;
    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    g_ctx->PSSetShaderResources(0, 2, none);
    if (g_colorNeedsResolve)
        g_ctx->ResolveSubresource(g_colorSampleTex, 0, g_colorTex, 0, g_colorResolveFormat);
    else
        g_ctx->CopyResource(g_colorSampleTex, g_colorTex);
}

static void UpdateResources(const PipePayload& p)
{
    static DWORD lastColorAttempt = 0, lastDepthAttempt = 0;
    const DWORD now = GetTickCount();
    constexpr DWORD retryMs = 500;

    const bool colorValid = p.colorHandle != 0;
    if (colorValid)
    {
        bool ch = p.colorHandle != g_lastColorHandle ||
            p.colorWidth != g_lastColorW ||
            p.colorHeight != g_lastColorH ||
            p.colorFormat != g_lastColorFmt;
        if (ch || (!g_colorSRV && now - lastColorAttempt >= retryMs))
        {
            lastColorAttempt = now;
            ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
            g_ctx->PSSetShaderResources(0, 2, none);
            SafeRelease(g_colorSRV); SafeRelease(g_colorSampleTex); SafeRelease(g_colorTex);
            g_colorTexW = g_colorTexH = 0;
            if (OpenSharedTex(p.colorHandle, &g_colorTex, &g_lastColorOpenHR))
            {
                D3D11_TEXTURE2D_DESC td = {};
                g_colorTex->GetDesc(&td);
                g_colorTexW = td.Width; g_colorTexH = td.Height;
                g_lastColorSRVHR = CreateColorInputView();
                Log("[Color] tex %ux%u fmt=%u bind=0x%X samples=%u srvHR=0x%08lX",
                    td.Width, td.Height, td.Format, td.BindFlags, td.SampleDesc.Count,
                    (unsigned long)g_lastColorSRVHR);
            }
            else
            {
                g_lastColorSRVHR = E_PENDING;
                Log("[Color] Open failed 0x%08lX; check producer handle and SAME GPU adapter",
                    (unsigned long)g_lastColorOpenHR);
            }
            g_lastColorHandle = p.colorHandle;
            g_lastColorW = p.colorWidth; g_lastColorH = p.colorHeight;
            g_lastColorFmt = p.colorFormat;
        }
    }
    else
    {
        ReleaseColorInput();
        g_lastColorOpenHR = g_lastColorSRVHR = E_PENDING;
    }

    const bool depthValid = (g_depthPassEnabled || g_depthPreview) && IsDepthFresh(p) &&
        (p.depthWidth || p.colorWidth) && (p.depthHeight || p.colorHeight);
    if (depthValid)
    {
        uint32_t dw = p.depthWidth ? p.depthWidth : p.colorWidth;
        uint32_t dh = p.depthHeight ? p.depthHeight : p.colorHeight;
        bool ch = p.depthHandle != g_lastDepthHandle ||
            dw != g_lastDepthW || dh != g_lastDepthH ||
            p.depthFormat != g_lastDepthFmt;
        if (ch || (!g_depthSRV && now - lastDepthAttempt >= retryMs))
        {
            lastDepthAttempt = now;
            SafeRelease(g_depthSRV); SafeRelease(g_depthTex);
            g_depthTexW = g_depthTexH = 0;
            if (OpenSharedTex(p.depthHandle, &g_depthTex, &g_lastDepthOpenHR))
            {
                D3D11_TEXTURE2D_DESC td = {};
                g_depthTex->GetDesc(&td);
                g_depthTexW = td.Width; g_depthTexH = td.Height;
                D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
                s.Format = DepthSRVFormat(p.depthFormat ? p.depthFormat : td.Format);
                s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                s.Texture2D.MipLevels = 1;
                g_lastDepthSRVHR = g_dev->CreateShaderResourceView(g_depthTex, &s, &g_depthSRV);
                Log("tex %ux%u payloadFmt=%u texFmt=%u srvFmt=%u srvHR=0x%08lX",
                    td.Width, td.Height, p.depthFormat, td.Format, s.Format,
                    (unsigned long)g_lastDepthSRVHR);
            }
            else Log(" Open failed 0x%08lX", (unsigned long)g_lastDepthOpenHR);
            g_lastDepthHandle = p.depthHandle;
            g_lastDepthW = dw; g_lastDepthH = dh;
            g_lastDepthFmt = p.depthFormat;
        }
    }
    else
    {
        ReleaseDepthInput();
    }
}

static void SetRemap(UINT texW, UINT texH, const PipePayload& p, bool invert,
    ID3D11Buffer* buffer)
{
    RemapCB cb = {};
    cb.uvScale[0] = 1.0f; cb.uvScale[1] = 1.0f;
    cb.uvBias[0] = 0.0f; cb.uvBias[1] = 0.0f;

    if (g_useViewportRemap && !(p.flags & FLAG_FULL_FRAME) && (p.flags & FLAG_VIEWPORT_OK) &&
        p.vpWidth && p.vpHeight && texW && texH)
    {
        cb.uvScale[0] = (float)p.vpWidth / (float)texW;
        cb.uvScale[1] = (float)p.vpHeight / (float)texH;
        cb.uvBias[0] = (float)p.vpX / (float)texW;
        cb.uvBias[1] = (float)p.vpY / (float)texH;
    }

    if (invert && g_depthUpsideDown)
    {
        cb.uvBias[1] += cb.uvScale[1];
        cb.uvScale[1] = -cb.uvScale[1];
    }

    cb.misc[0] = 0.0f;
    cb.misc[1] = g_depthFarPlane;
    cb.misc[2] = 0.0f;
    cb.misc[3] = 0.0f;

    static ConstantUploadCache<RemapCB> depthCache, colorCache;
    auto& cache = buffer == g_cbColor ? colorCache : depthCache;
    UploadCachedConstants(buffer, cb, cache);
    g_ctx->PSSetConstantBuffers(0, 1, &buffer);
}

static void SetCombinedRemap(const PipePayload& p)
{
    CombinedRemapCB cb = {};
    cb.colorUV[0] = cb.colorUV[1] = 1.0f;
    cb.depthUV[0] = cb.depthUV[1] = 1.0f;

    if (g_useViewportRemap && !(p.flags & FLAG_FULL_FRAME) && (p.flags & FLAG_VIEWPORT_OK) &&
        p.vpWidth && p.vpHeight)
    {
        if (g_colorTexW && g_colorTexH)
        {
            cb.colorUV[0] = (float)p.vpWidth / (float)g_colorTexW;
            cb.colorUV[1] = (float)p.vpHeight / (float)g_colorTexH;
            cb.colorUV[2] = (float)p.vpX / (float)g_colorTexW;
            cb.colorUV[3] = (float)p.vpY / (float)g_colorTexH;
        }
        if (g_depthTexW && g_depthTexH)
        {
            cb.depthUV[0] = (float)p.vpWidth / (float)g_depthTexW;
            cb.depthUV[1] = (float)p.vpHeight / (float)g_depthTexH;
            cb.depthUV[2] = (float)p.vpX / (float)g_depthTexW;
            cb.depthUV[3] = (float)p.vpY / (float)g_depthTexH;
        }
    }
    if (g_depthUpsideDown)
    {
        cb.depthUV[3] += cb.depthUV[1];
        cb.depthUV[1] = -cb.depthUV[1];
    }

    cb.misc[0] = g_depthReversed ? 0.0f : 1.0f;
    cb.misc[1] = g_depthFarPlane;
    cb.misc[2] = 0.0f;
    cb.misc[3] = 0.0f;

    static ConstantUploadCache<CombinedRemapCB> cache;
    UploadCachedConstants(g_cbCombined, cb, cache);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cbCombined);
}

static void DrawTri()
{
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    g_ctx->Draw(3, 0);
}

static void ViewportFull()
{
    D3D11_VIEWPORT v = {};
    v.Width = (float)g_width;
    v.Height = (float)g_height;
    v.MinDepth = 0; v.MaxDepth = 1;
    g_ctx->RSSetViewports(1, &v);
}

static void UpdateGpuMemoryStats()
{
    const DWORD now = GetTickCount();
    if (!g_adapter3 || now - g_lastVramQueryTick < g_vramQueryIntervalMs) return;
    g_lastVramQueryTick = now;
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0,
        DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
    {
        g_vramUsageBytes = info.CurrentUsage;
        g_vramBudgetBytes = info.Budget;
        g_vramStatsAvailable = true;
    }
    else g_vramStatsAvailable = false;
}

static void HistoryStats(const float* values, float& minValue,
    float& average, float& maxValue)
{
    minValue = 0.0f; average = 0.0f; maxValue = 0.0f;
    if (g_perfHistoryCount <= 0) return;
    minValue = FLT_MAX;
    int validCount = 0;
    for (int i = 0; i < g_perfHistoryCount; ++i)
    {
        const float value = values[i];
        if (value <= 0.0f) continue;
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
        average += value;
        ++validCount;
    }
    if (minValue == FLT_MAX || validCount == 0) { minValue = 0.0f; return; }
    average /= (float)validCount;
}

static void UiSection(const char* title, const char* description = nullptr)
{
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.76f, 0.79f, 0.84f, 1.0f), "%s", title);
    if (description)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", description);
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

static void DrawGpuStatsTab()
{
    UiSection("Performance profile");
    const char* modes[] = { "Auto", "Low / Stable", "Balanced", "High / Smooth" };
    int mode = (int)g_requestedPerfTier;
    ImGui::PushItemWidth(-1);
    if (ImGui::Combo("##profile", &mode, modes, 4))
    {
        g_requestedPerfTier = (PerfTier)mode;
        ApplyPerfProfile();
        SaveSettings();
    }
    ImGui::PopItemWidth();
    ImGui::Text("Active: %s", g_activePerfProfile);
    ImGui::TextDisabled("Overlay cap: %u FPS%s", g_maxOverlayFps,
        g_vsync ? "  |  VSync" : (g_tearingSupported ? "  |  VRR/tearing" : "  |  VSync fallback"));

    UiSection("Process stats");
    pdh_process_stats_t stats;
    {
        std::lock_guard<std::mutex> lk(g_process_stats_mutex);
        stats = g_process_stats;
    }
    if (!stats.valid)
    {
        ImGui::TextDisabled("GPU  Roblox");
        ImGui::TextDisabled("Waiting for Roblox process...");
    }
    else
    {
        if (stats.gpu_valid) ImGui::Text("GPU  Roblox %.1f%% (busiest engine)", stats.roblox_gpu_pct);
        else ImGui::TextDisabled("GPU  Roblox --");
        char targetCpu[32] = "--", overlayCpu[32] = "--";
        if (stats.cpu_valid) snprintf(targetCpu, sizeof(targetCpu), "%.1f%%", stats.roblox_cpu);
        if (stats.overlay_cpu_valid) snprintf(overlayCpu, sizeof(overlayCpu), "%.1f%%", stats.overlay_cpu);
        ImGui::Text("CPU  Roblox %s   Overlay %s  (of all cores)", targetCpu, overlayCpu);
        char dedicatedMb[32] = "--", sharedMb[32] = "--";
        if (stats.dedicated_valid) snprintf(dedicatedMb, sizeof(dedicatedMb), "%.0f", stats.roblox_vram_mb);
        if (stats.shared_valid) snprintf(sharedMb, sizeof(sharedMb), "%.0f", stats.roblox_shared_mb);
        ImGui::Text("VRAM Roblox  %s MB  (+%s MB shared)", dedicatedMb, sharedMb);
    }

    UiSection("Adapter & memory");
    ImGui::TextWrapped("%s", g_gpuName.c_str());
    ImGui::Text("Dedicated VRAM: %zu MB", g_gpuDedicatedMB);



    UpdateGpuMemoryStats();
    if (g_vramStatsAvailable && g_vramBudgetBytes)
    {
        const double mb = 1024.0 * 1024.0;
        const float ratio = (float)std::min(1.0, (double)g_vramUsageBytes / g_vramBudgetBytes);
        char memory[96];
        snprintf(memory, sizeof(memory), "%.0f / %.0f MB", g_vramUsageBytes / mb, g_vramBudgetBytes / mb);
        ImGui::ProgressBar(ratio, ImVec2(-1, 24), memory);
    }
    else ImGui::TextDisabled("VRAM data unavailable");

    UiSection("Frame pacing");
    if (g_sourceFpsAvailable) ImGui::Text("%.0f FPS", g_fps);
    else ImGui::TextDisabled("-- FPS");
    ImGui::Text("Overlay frame: %.2f ms", g_overlayFrameCostMs);

    float minimum, average, maximum;
    HistoryStats(g_overlayFrameHistory, minimum, average, maximum);
    const int count = std::max(1, g_perfHistoryCount);
    const int offset = g_perfHistoryCount == kPerfHistorySize ? g_perfHistoryOffset : 0;
    ImGui::PlotLines("##cpu_history", g_overlayFrameHistory, count, offset, nullptr,
        0.0f, std::max(8.0f, maximum * 1.15f), ImVec2(-1, 76));
    ImGui::Text("Min %.2f  /  Avg %.2f  /  Max %.2f ms", minimum, average, maximum);

    const float budget = 1000.0f / (float)std::max<UINT>(30, g_maxOverlayFps);
    char timing[64];
    snprintf(timing, sizeof(timing), "%.2f / %.2f ms", g_overlayFrameCostMs, budget);
    ImGui::ProgressBar(std::clamp((float)(g_overlayFrameCostMs / budget), 0.0f, 1.0f),
        ImVec2(-1, 24), timing);
}

static const char* OverlayStatus(const PipePayload& p)
{
    static char text[192];
    if (!g_pipeConnected.load())   return "Searching pipe";
    if (!g_hasPayload.load())      return "Pipe connected, waiting payload";
    bool colorReady = g_colorSRV && p.colorHandle;
    bool depthReady = g_depthSRV && p.depthHandle && (p.flags & FLAG_DEPTH_OK);
    if (colorReady && depthReady)  return "Connected: color + depth";
    if (colorReady)                return "Connected: color only";
    if (depthReady)                return "Connected: depth only";
    if (p.colorHandle || p.depthHandle)
    {
        snprintf(text, sizeof(text),
            "Connected: opening C=0x%08lX D=0x%08lX",
            (unsigned long)g_lastColorOpenHR, (unsigned long)g_lastDepthOpenHR);
        return text;
    }
    return "Connected: no handles";
}

static void SetSceneState(bool writeColor)
{
    g_ctx->OMSetBlendState(writeColor ? g_blendColor : g_blendDepthOnly, nullptr, 0xffffffff);
    g_ctx->RSSetState(g_sceneRaster);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->HSSetShader(nullptr, nullptr, 0);
    g_ctx->DSSetShader(nullptr, nullptr, 0);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->SetPredication(nullptr, FALSE);
    ViewportFull();
}

static void Render(const PipePayload& p)
{



    static PipePayload lastResourcePayload{};
    static bool resourcePayloadValid = false;
    static bool lastDepthEnabled = false, lastPreviewEnabled = false;
    static bool lastDepthFresh = false;
    static uint64_t resourceEpoch = 0;
    const bool depthFresh = IsDepthFresh(p);
    const bool resourcesChanged = !resourcePayloadValid ||
        resourceEpoch != g_constantBufferEpoch ||
        !SameInputResources(p, lastResourcePayload) ||
        lastDepthEnabled != g_depthPassEnabled || lastPreviewEnabled != g_depthPreview ||
        lastDepthFresh != depthFresh;
    const bool periodicUpdate = (g_resourceUpdateStrideFrames == 0) ||
        ((++g_resourceUpdateFrameCounter) % (g_resourceUpdateStrideFrames + 1) == 0);
     
    if (resourcesChanged || periodicUpdate)
    {
        UpdateResources(p);
        lastResourcePayload = p;
        resourcePayloadValid = true;
        resourceEpoch = g_constantBufferEpoch;
        lastDepthEnabled = g_depthPassEnabled;
        lastPreviewEnabled = g_depthPreview;
        lastDepthFresh = depthFresh;
    }
    RefreshColorInputCopy();
    if (g_depthPassEnabled && !g_localDSV)
        CreateLocalDepth(g_width, g_height);
    SetSceneState(true);
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_depthPassEnabled ? g_localDSV : nullptr);
    const float clear[4] = { 0, 0, 0, g_transparent ? 0.0f : 1.0f };
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    if (g_depthPassEnabled && g_localDSV)
        g_ctx->ClearDepthStencilView(g_localDSV, D3D11_CLEAR_DEPTH,
            g_depthReversed ? 0.0f : 1.0f, 0);

}

static void RenderDepthFinalPass(const PipePayload& p)
{
    if (!g_rtv) return;
    const bool hasDepth = g_depthPassEnabled && g_localDSV && g_depthSRV;
    const bool preview = g_depthPreview && g_depthSRV;
    const bool fused = hasDepth && g_colorSRV && g_psColorDepth &&
        !preview && !g_separateScenePasses;
    SetSceneState(false);
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_depthPassEnabled ? g_localDSV : nullptr);

    if (hasDepth)
    {
        g_ctx->PSSetShader(g_psDepth, nullptr, 0);
        SetRemap(g_depthTexW, g_depthTexH, p, true, g_cb);
        g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
        g_ctx->PSSetSamplers(0, 1, &g_sampPoint);

        if (g_depthHeuristicDraws > 0)
        {
            g_ctx->OMSetDepthStencilState(g_dsWrite, 0);
            D3D11_VIEWPORT tiny = {};
            tiny.Width = tiny.Height = tiny.MaxDepth = 1.0f;
            g_ctx->RSSetViewports(1, &tiny);
            for (int i = 0; i < g_depthHeuristicDraws; ++i) DrawTri();
            ViewportFull();
        }
        g_ctx->OMSetDepthStencilState(g_dsWrite, 0);
        if (fused)
        {
            SetSceneState(true);
            g_ctx->PSSetShader(g_psColorDepth, nullptr, 0);
            SetCombinedRemap(p);
            ID3D11ShaderResourceView* views[2] = { g_colorSRV, g_depthSRV };
            ID3D11SamplerState* samplers[2] = { g_sampLinear, g_sampPoint };
            g_ctx->PSSetShaderResources(0, 2, views);
            g_ctx->PSSetSamplers(0, 2, samplers);
        }
        DrawTri();

        if (!g_fastDepthFinal && !g_skipRedundantDepthDraw) DrawTri();
    }
    if (!fused && (preview || g_colorSRV))
    {

        SetSceneState(true);
        g_ctx->OMSetDepthStencilState(g_dsOff, 0);
        if (preview)
        {
            g_ctx->PSSetShader(g_psDepthView, nullptr, 0);
            SetRemap(g_depthTexW, g_depthTexH, p, true, g_cb);
            g_ctx->PSSetShaderResources(0, 1, &g_depthSRV);
            g_ctx->PSSetSamplers(0, 1, &g_sampLinear);
        }
        else
        {
            g_ctx->PSSetShader(g_psColor, nullptr, 0);
            SetRemap(g_colorTexW, g_colorTexH, p, false, g_cbColor);
            g_ctx->PSSetShaderResources(0, 1, &g_colorSRV);

            ID3D11SamplerState* colorSamp = g_renderUsePointSampler ? g_sampPoint : g_sampLinear;
            g_ctx->PSSetSamplers(0, 1, &colorSamp);
        }
        DrawTri();
    }
    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    g_ctx->PSSetShaderResources(0, 2, none);

    g_ctx->OMSetBlendState(g_blendColor, nullptr, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_depthPassEnabled ? g_dsWrite : g_dsOff, 0);
    g_ctx->OMSetRenderTargets(1, &g_rtv, g_depthPassEnabled ? g_localDSV : nullptr);
}

static RECT WindowClientRectOnScreen(HWND w)
{
    RECT out = { 0,0,0,0 };
    RECT client = {};
    if (!w || !GetClientRect(w, &client)) return out;
    POINT tl = { client.left, client.top };
    POINT br = { client.right, client.bottom };
    if (!ClientToScreen(w, &tl) || !ClientToScreen(w, &br)) return out;
    out.left = tl.x; out.top = tl.y;
    out.right = br.x; out.bottom = br.y;
    return out;
}

struct TargetScan
{
    HWND  best = nullptr;
    LONG  bestArea = 0;
    DWORD bestPid = 0;
};

static BOOL CALLBACK EnumTargetWindow(HWND w, LPARAM lp)
{
    TargetScan* scan = (TargetScan*)lp;

    if (!IsWindowVisible(w)) return TRUE;
    if (GetWindowTextLengthA(w) <= 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid || !IsTargetProcess(pid)) return TRUE;

    RECT c = WindowClientRectOnScreen(w);
    const LONG cw = c.right - c.left;
    const LONG ch = c.bottom - c.top;
    if (cw < 128 || ch < 128) return TRUE;
    const LONG area = cw * ch;

    if (area > scan->bestArea || !scan->best)
    {
        scan->best = w;
        scan->bestPid = pid;
        scan->bestArea = area;
    }
    return TRUE;
}

static HWND FindTarget()
{
    TargetScan scan;
    EnumWindows(EnumTargetWindow, (LPARAM)&scan);
    if (scan.best)
    {
        g_targetPid = scan.bestPid;
        return scan.best;
    }
    static const char* const kLegacyTitles[] = { "Roblox", "Voidstrap", "Voistrap" };
    for (const char* title : kLegacyTitles)
    {
        HWND h = FindWindowA(nullptr, title);
        if (h) { GetWindowThreadProcessId(h, &g_targetPid); return h; }
    }
    return nullptr;
}

static RECT TargetRect()
{
    RECT r = { 100,100,100 + (LONG)g_width,100 + (LONG)g_height };
    if (!g_desktop && g_target && IsWindow(g_target))
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(g_target, &pid);
        if (pid) g_targetPid = pid;
        RECT s = WindowClientRectOnScreen(g_target);
        if (s.right > s.left && s.bottom > s.top)
        {
            r.left = s.left; r.top = s.top;
            r.right = s.right; r.bottom = s.bottom;
        }
    }
    else if (g_desktop)
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &r, 0);
    return r;
}

static void FocusTarget()
{
    if (g_externalOverlayForeground || g_graphicsRecovering) return;
    if (g_target && IsWindow(g_target))
        SetForegroundWindow(g_target);
}

static bool IsCompanionOverlayWindow(HWND window, DWORD pid)
{


    static DWORD cachedPid = 0, checkedTick = 0;
    static bool cachedResult = false;
    const DWORD now = GetTickCount();
    if (pid != cachedPid || now - checkedTick >= 500)
    {
        cachedPid = pid; checkedTick = now;
        char exe[MAX_PATH] = {};
        cachedResult = QueryProcessExeName(pid, exe, sizeof(exe)) &&
            (_stricmp(exe, "NVIDIA Overlay.exe") == 0 ||
                _stricmp(exe, "NVIDIA Share.exe") == 0);
    }
    if (cachedResult) return true;
    const LONG_PTR ex = GetWindowLongPtrA(window, GWL_EXSTYLE);
    if (!(ex & WS_EX_TOPMOST) || !(ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)))
        return false;
    char exe[MAX_PATH] = {};
    return QueryProcessExeName(pid, exe, sizeof(exe)) &&
        (_stricmp(exe, "Discord.exe") == 0 ||
         _stricmp(exe, "DiscordCanary.exe") == 0 ||
         _stricmp(exe, "DiscordPTB.exe") == 0);
}

static bool ForegroundIsTargetOrOverlay()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return g_gameForegroundSession;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    bool targetOrOwn = fg == g_hwnd || fgPid == GetCurrentProcessId();
    if (g_target && IsWindow(g_target))
    {
        DWORD targetPid = 0;
        GetWindowThreadProcessId(g_target, &targetPid);
        if (targetPid) g_targetPid = targetPid;
        targetOrOwn = targetOrOwn || fg == g_target || IsChild(g_target, fg) ||
            GetAncestor(fg, GA_ROOT) == g_target;
    }
    targetOrOwn = targetOrOwn || (g_targetPid && fgPid == g_targetPid);
    if (!targetOrOwn)
    {
        RefreshTargetPids();
        for (DWORD pid : g_targetPids)
            if (pid && fgPid == pid) { targetOrOwn = true; break; }
    }
    if (targetOrOwn)
    {
        g_externalOverlayForeground = false;
        g_gameForegroundSession = true;
        return true;
    }
    const bool targetVisible = g_target && IsWindow(g_target) &&
        IsWindowVisible(g_target) && !IsIconic(g_target);
    g_externalOverlayForeground = g_gameForegroundSession && targetVisible &&
        IsCompanionOverlayWindow(fg, fgPid);
    if (g_externalOverlayForeground) return true;
    g_gameForegroundSession = false;
    return false;
}



 
 
static bool IsWindowAbove(HWND upper, HWND lower)
{
    if (!upper || !lower || upper == lower) return false;
    for (HWND w = GetWindow(lower, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
        if (w == upper) return true;
    return false;
}

static void RefreshOverlayZOrder()
{
    if (!g_hwnd || !IsWindowVisible(g_hwnd)) return;
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (!(GetWindowLongPtrA(g_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);

     
     
     
    for (HWND layer : { g_uiHwnd, g_fpsHwnd })
        if (layer && IsWindowVisible(layer) &&
            !(GetWindowLongPtrA(layer, GWL_EXSTYLE) & WS_EX_TOPMOST))
            SetWindowPos(layer, HWND_TOPMOST, 0, 0, 0, 0, flags);

     
    HWND upper = nullptr;
    for (HWND layer : { g_uiHwnd, g_fpsHwnd, g_hwnd })
    {
        if (!layer || !IsWindowVisible(layer)) continue;
        if (upper && !IsWindowAbove(upper, layer))
            SetWindowPos(layer, upper, 0, 0, 0, 0, flags);
        upper = layer;
    }
    if (g_externalOverlayForeground)
    {
        HWND companion = GetForegroundWindow();
        if (companion && companion != g_hwnd &&
            (GetWindowLongPtrA(companion, GWL_EXSTYLE) & WS_EX_TOPMOST))
        {
            HWND previous = companion;
            for (HWND layer : { g_uiHwnd, g_fpsHwnd, g_hwnd })
            {
                if (!layer || !IsWindowVisible(layer)) continue;
                if (!IsWindowAbove(previous, layer))
                    SetWindowPos(layer, previous, 0, 0, 0, 0, flags);
                previous = layer;
            }
        }
    }
}

static void HideMenuLayer()
{
    if (g_uiHwnd && IsWindowVisible(g_uiHwnd))
        ShowWindow(g_uiHwnd, SW_HIDE);
     
    for (auto& slot : g_uiReadbacks) slot.pending = false;
}

static void HideFpsLayer()
{
    if (g_fpsHwnd && IsWindowVisible(g_fpsHwnd))
        ShowWindow(g_fpsHwnd, SW_HIDE);
}

static void HideUiLayer()
{
    HideMenuLayer();
    HideFpsLayer();
}

static void ResetOverlayResume()
{
    g_resumeArmed = false;
    g_resumeStartTick = 0;
}

static bool ResumeForegroundReady(DWORD now)
{
    if (!g_resumeArmed)
    {
        g_resumeArmed = true;
        g_resumeStartTick = now;
        return false;
    }
     
    return now - g_resumeStartTick >= 100;
}

static void InvalidateOverlayFrame()
{
    for (auto& slot : g_uiReadbacks) slot.pending = false;
    g_presentOccluded = false;
    g_nextPresentProbeTick = 0;
    ++g_graphicsGeneration;  
}

static void UpdateOverlayInputStyle(HWND window, DWORD desired)
{
    if (!window) return;
    const LONG_PTR current = GetWindowLongPtrA(window, GWL_EXSTYLE);
     
    const LONG_PTR next = (current & ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) |
        (desired & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE));
    if (current == next) return;
    SetWindowLongPtrA(window, GWL_EXSTYLE, next);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static DWORD ExStyle();

static void UpdateForegroundVisibility()
{
    if (g_desktop || !g_hwnd) return;

    const bool wasExternalOverlay = g_externalOverlayForeground;
    const bool foregroundAllowed = ForegroundIsTargetOrOverlay();
    const bool targetVisible = g_target && IsWindow(g_target) &&
        IsWindowVisible(g_target) && !IsIconic(g_target);
    const bool shouldShow = g_overlayEnabled && !g_resyncing && !g_graphicsRecovering &&
        targetVisible && foregroundAllowed;
    if (wasExternalOverlay != g_externalOverlayForeground)
    {
        UpdateOverlayInputStyle(g_hwnd, ExStyle());
        if (shouldShow) RefreshOverlayZOrder();
    }
    if (!shouldShow)
    {
        ResetOverlayResume();
         
         
         
        if (IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
        HideUiLayer();
        g_overlayHiddenForForeground = true;
        return;
    }
    if (g_overlayHiddenForForeground || !IsWindowVisible(g_hwnd) || IsIconic(g_hwnd))
    {
        g_overlayHiddenForForeground = true;
        if (!ResumeForegroundReady(GetTickCount())) return;
        RECT r = TargetRect();
        if (r.right <= r.left || r.bottom <= r.top) return;
         
        SetWindowPos(g_hwnd, nullptr, r.left, r.top,
            r.right - r.left, r.bottom - r.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        HideUiLayer();
        InvalidateOverlayFrame();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        g_overlayHiddenForForeground = false;
        g_lastRect = r;
        ResetOverlayResume();
        RefreshOverlayZOrder();
        Log("[Window] restored; forced fresh scene/UI frame");
    }
}

static DWORD ExStyle()
{

    DWORD e = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    if (g_externalOverlayForeground || g_graphicsRecovering || (!g_menu && !g_reshadeInput))
        e |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    return e;
}

static DWORD UiExStyle()
{

    return WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
        WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
}

static void ApplyWindowMode()
{
    if (!g_hwnd) return;
    if (!g_overlayEnabled || g_graphicsRecovering)
    {
        ShowWindow(g_hwnd, SW_HIDE);
        HideUiLayer();
        ResetOverlayResume();
        g_overlayHiddenForForeground = true;
        return;
    }
    UpdateOverlayInputStyle(g_hwnd, ExStyle());
    UpdateOverlayInputStyle(g_uiHwnd, UiExStyle());
    UpdateOverlayInputStyle(g_fpsHwnd, UiExStyle());

     
    if (g_transparent)
        SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    else
        SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    if (g_desktop)
    {
        if (!IsWindowVisible(g_hwnd) || IsIconic(g_hwnd))
        {
            HideUiLayer();
            InvalidateOverlayFrame();
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        }
        g_overlayHiddenForForeground = false;
    }
    else UpdateForegroundVisibility();
     
     
    if (!g_menu && !g_showKeyHelp) HideMenuLayer();
    if (!g_showFPS || g_reshadeInput) HideFpsLayer();

    RefreshOverlayZOrder();
    if (!g_menu && !g_reshadeInput) FocusTarget();
}

static void SetMenu(bool v)
{

    if (v && !g_overlayEnabled) return;
    if (v && g_showKeyHelp)
    {
        g_showKeyHelp = false;
        SaveSettings();
    }
    g_menu = v;
    if (v) g_reshadeInput = false;
    if (!v) HideMenuLayer();
    ApplyWindowMode();

    if (v && !g_externalOverlayForeground && !g_graphicsRecovering) { if (g_hwnd) { SetForegroundWindow(g_hwnd); SetFocus(g_hwnd); } }
    else FocusTarget();
}

static LRESULT CALLBACK UiWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{

    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_DESTROY) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_menu && ImGui_ImplWin32_WndProcHandler(h, m, w, l))
        return TRUE;
    if (m == WM_SIZE && w != SIZE_MINIMIZED && !g_graphicsRecovering && g_swap && g_ctx)
    {
        const UINT width = LOWORD(l);
        const UINT height = HIWORD(l);
        if (width && height) Resize(width, height);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_DESTROY) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static bool InitWindow()
{
    if (!g_desktop)
    {
        g_target = FindTarget();
        if (!g_target)
        {
            Log("[Window] Roblox process/window not found; closing overlay");
            return false;
        }
        char exe[MAX_PATH] = {};
        QueryProcessExeName(g_targetPid, exe, sizeof(exe));
        g_targetExeName = exe;
        Log("[Window] target: %s (pid %lu)", exe[0] ? exe : "?", g_targetPid);
    }
    RECT r = TargetRect();
    g_width = std::max<UINT>(1, r.right - r.left);
    g_height = std::max<UINT>(1, r.bottom - r.top);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "FixedHookStatusPipeOverlay";
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(ExStyle(), wc.lpszClassName, "FeatureModule Overlay",
        WS_POPUP, r.left, r.top, g_width, g_height,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;

    WNDCLASSEXA wcu = {};
    wcu.cbSize = sizeof(wcu);
    wcu.lpfnWndProc = UiWndProc;
    wcu.hInstance = wc.hInstance;
    wcu.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcu.lpszClassName = "FixedHookStatusPipeOverlayUi";
    RegisterClassExA(&wcu);

    g_uiHwnd = CreateWindowExA(UiExStyle(), wcu.lpszClassName, "FeatureModule Overlay UI",
        WS_POPUP, r.left, r.top, g_width, g_height,
        g_hwnd, nullptr, wcu.hInstance, nullptr);
    if (!g_uiHwnd) return false;

    g_fpsHwnd = CreateWindowExA(UiExStyle(), wcu.lpszClassName, "FeatureModule FPS",
        WS_POPUP, r.left + g_fpsOffsetX, r.top + g_fpsOffsetY, 72, 30,
        g_hwnd, nullptr, wcu.hInstance, nullptr);
    if (!g_fpsHwnd) return false;

    MARGINS ma = { -1,-1,-1,-1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &ma);

    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
    ApplyWindowMode();
    ShowWindow(g_hwnd, SW_SHOWNA);
    return true;
}

static ImFont* LoadOverlayTtf(ImFontAtlas* atlas, const std::wstring& path, float pixels, const ImFontConfig& config)
{
    if (path.empty()) return nullptr;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return nullptr;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return nullptr; }
    const long bytes = ftell(file);
    if (bytes < 12 || bytes > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file); return nullptr;
    }
    void* data = ImGui::MemAlloc(static_cast<size_t>(bytes));
    if (!data) { fclose(file); return nullptr; }
    const size_t readBytes = fread(data, 1, static_cast<size_t>(bytes), file);
    fclose(file);
    if (readBytes != static_cast<size_t>(bytes)) { ImGui::MemFree(data); return nullptr; }
    const auto* signature = static_cast<const unsigned char*>(data);
    const bool trueType = signature[0] == 0 && signature[1] == 1 && signature[2] == 0 && signature[3] == 0;
    const bool openType = memcmp(data, "OTTO", 4) == 0;
    if (!trueType && !openType) { ImGui::MemFree(data); return nullptr; }
    ImFontConfig ownedConfig = config;
    ownedConfig.FontDataOwnedByAtlas = true;
    return atlas->AddFontFromMemoryTTF(data, static_cast<int>(bytes), pixels, &ownedConfig,
        atlas->GetGlyphRangesCyrillic());
}

static void InitializeOverlayFont(ImGuiIO& io, ImFontConfig& config)
{
    std::vector<wchar_t> executable(32768);
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length&& length < executable.size())
    {
        std::wstring path(executable.data(), length);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            g_uiFontPath = path.substr(0, slash + 1) + L"NunitoSans-Regular.ttf";
    }
    ImFont* font = LoadOverlayTtf(io.Fonts, g_uiFontPath, 16.0f, config);
    if (font)
        g_privateUiFontLoaded = AddFontResourceExW(g_uiFontPath.c_str(), FR_PRIVATE, nullptr) > 0;
    else
    {
        wchar_t windows[MAX_PATH] = {};
        const UINT windowsLength = GetWindowsDirectoryW(windows, MAX_PATH);
        if (windowsLength && windowsLength < MAX_PATH)
            font = LoadOverlayTtf(io.Fonts, std::wstring(windows, windowsLength) + L"\\Fonts\\segoeui.ttf", 16.0f, config);
        Log("[Font] NunitoSans-Regular.ttf unavailable; using fallback.");
    }
    if (font) io.FontDefault = font;
    else
    {
        config.SizePixels = 16.0f;
        io.FontDefault = io.Fonts->AddFontDefault(&config);
    }
}

static void ReleaseOverlayFontResource()
{
    if (g_privateUiFontLoaded)
    {
        RemoveFontResourceExW(g_uiFontPath.c_str(), FR_PRIVATE, nullptr);
        g_privateUiFontLoaded = false;
    }
}

static bool InitD3D(bool reuseUiContext = false)
{
    ++g_constantBufferEpoch;  
    DXGI_SWAP_CHAIN_DESC sd = {};

    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_width;
    sd.BufferDesc.Height = g_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    const bool flipModel = HasArg("/flip-model");
    sd.SwapEffect = flipModel ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;

     
     
     
    g_tearingSupported = false;
    IDXGIFactory5* factory5 = nullptr;
    if (flipModel && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory5), (void**)&factory5)) && factory5)
    {
        BOOL allowTearing = FALSE;
        g_tearingSupported = SUCCEEDED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))) && allowTearing;
        factory5->Release();
    }
    sd.Flags = g_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    IDXGIAdapter* selectedAdapter = nullptr;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
    {
        for (UINT index = 0; ; ++index)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            if (!adapter) break;
            DXGI_ADAPTER_DESC1 description = {};
            if (SUCCEEDED(adapter->GetDesc1(&description)))
                Log("[Adapter %u] %ls vendor=0x%X LUID=%08lX:%08lX", index,
                    description.Description, description.VendorId,
                    (unsigned long)description.AdapterLuid.HighPart,
                    (unsigned long)description.AdapterLuid.LowPart);
            if ((int)index == g_adapterIndex) selectedAdapter = adapter;
            else adapter->Release();
        }
        factory->Release();
    }
    if (g_adapterIndex >= 0 && !selectedAdapter)
    {
        Log("[D3D] requested adapter index %d is unavailable", g_adapterIndex);
        return false;
    }
    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    const auto createDevice = [&](D3D_DRIVER_TYPE driver) -> HRESULT {

        const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
        HRESULT result = D3D11CreateDeviceAndSwapChain(selectedAdapter,
            selectedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : driver, nullptr,
            deviceFlags, lv, 2, D3D11_SDK_VERSION,
            &sd, &g_swap, &g_dev, &fl, &g_ctx);
        if (result == E_INVALIDARG)
        {
            SafeRelease(g_swap); SafeRelease(g_ctx); SafeRelease(g_dev);
            result = D3D11CreateDeviceAndSwapChain(selectedAdapter,
                selectedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : driver, nullptr,
                deviceFlags, lv + 1, 1, D3D11_SDK_VERSION,
                &sd, &g_swap, &g_dev, &fl, &g_ctx);
        }
        return result;
        };
    HRESULT hr = createDevice(D3D_DRIVER_TYPE_HARDWARE);
    if (FAILED(hr) && !selectedAdapter)
    {
        SafeRelease(g_swap); SafeRelease(g_ctx); SafeRelease(g_dev);
        hr = createDevice(D3D_DRIVER_TYPE_WARP);
    }
    SafeRelease(selectedAdapter);
    if (FAILED(hr)) return false;
    Log("[D3D] presentation: %s", flipModel ? "flip (opt-in)" : "DWM compatibility (blt)");

     
     
    IDXGIFactory* swapFactory = nullptr;
    if (g_hwnd && SUCCEEDED(g_swap->GetParent(__uuidof(IDXGIFactory), (void**)&swapFactory)) && swapFactory)
    {
        swapFactory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        swapFactory->Release();
    }

    g_rtv = CreateBackBufferRTV();
    if (!g_rtv) return false;
    ID3DBlob* b = nullptr;
    if (Compile(VS_SRC, "vs_5_0", &b)) { g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs); b->Release(); }
    if (Compile(PS_COLOR, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psColor); b->Release(); }
    if (Compile(PS_DEPTH, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psDepth); b->Release(); }
    if (Compile(PS_COLOR_DEPTH, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psColorDepth); b->Release(); }
    if (Compile(PS_DEPTH_VIEW, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psDepthView); b->Release(); }
    if (Compile(PS_PREMULT, "ps_5_0", &b)) { g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_psPremult); b->Release(); }

    D3D11_BUFFER_DESC uiCbd = {};
    uiCbd.ByteWidth = sizeof(RemapCB);
    uiCbd.Usage = D3D11_USAGE_DYNAMIC;
    uiCbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    uiCbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&uiCbd, nullptr, &g_uiCB))) return false;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(RemapCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb)) ||
        FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cbColor)))
        return false;
    cbd.ByteWidth = sizeof(CombinedRemapCB);
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cbCombined)))
        return false;

    IDXGIDevice1* dxgiDevice1 = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice1)))
    {

        const int gpuPriority = g_schedulingBoostRequested ? 7 : 0;
        const HRESULT gpuPriorityResult = dxgiDevice1->SetGPUThreadPriority(gpuPriority);
        const HRESULT frameLatencyResult = dxgiDevice1->SetMaximumFrameLatency(1);
        Log("GPU scheduling: priority=%d result=0x%08lX, frame latency=1 result=0x%08lX",
            gpuPriority, static_cast<unsigned long>(gpuPriorityResult), static_cast<unsigned long>(frameLatencyResult));
        dxgiDevice1->Release();
    }

    if (!g_vs || !g_psColor || !g_psDepth || !g_psDepthView || !g_psPremult)
    {
        Log("[D3D] required shader failed; refusing an invisible/black scene");
        return false;
    }
    if (!g_psColorDepth) Log("[D3D] fused shader unavailable; using separate color/depth passes");
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_dev->CreateBlendState(&blend, &g_blendColor))) return false;
    blend.RenderTarget[0].RenderTargetWriteMask = 0;
    if (FAILED(g_dev->CreateBlendState(&blend, &g_blendDepthOnly))) return false;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = FALSE;
    if (FAILED(g_dev->CreateRasterizerState(&raster, &g_sceneRaster))) return false;

    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(g_dev->CreateDepthStencilState(&ds, &g_dsWrite))) return false;
    ds = {}; ds.DepthEnable = FALSE;
    if (FAILED(g_dev->CreateDepthStencilState(&ds, &g_dsOff))) return false;

    D3D11_SAMPLER_DESC sp = {};
    sp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sp.AddressU = sp.AddressV = sp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(g_dev->CreateSamplerState(&sp, &g_sampLinear))) return false;
    sp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(g_dev->CreateSamplerState(&sp, &g_sampPoint))) return false;

    QueryGpuNameFromDevice();
    if (reuseUiContext)
    {

        LoadOverlayBackground();
        return ImGui_ImplDX11_Init(g_dev, g_ctx);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = g_imguiIniPath[0] ? g_imguiIniPath : "imgui.ini";
    io.LogFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImFontConfig fontConfig;

    fontConfig.OversampleH = 1;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;
    fontConfig.RasterizerMultiply = 1.20f;
    InitializeOverlayFont(io, fontConfig);
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.Alpha = 1.0f;
        st.WindowRounding = 18.0f;
        st.ChildRounding = 14.0f;
        st.FrameRounding = 8.0f;
        st.GrabRounding = 6.0f;
        st.PopupRounding = 7.0f;
        st.TabRounding = 6.0f;
        st.ScrollbarRounding = 7.0f;
        st.WindowBorderSize = 1.0f;
        st.FrameBorderSize = 0.0f;
        st.WindowPadding = ImVec2(12, 10);
        st.FramePadding = ImVec2(8, 5);
        st.ItemSpacing = ImVec2(7, 6);
        st.ItemInnerSpacing = ImVec2(8, 6);
        st.ScrollbarSize = 11.0f;
        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(g_appearance.background.x, g_appearance.background.y,
            g_appearance.background.z, g_appearance.opacity);
        c[ImGuiCol_ChildBg] = ImVec4(26.0f / 255, 26.0f / 255, 28.0f / 255, 0.86f);
        c[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.058f, 0.064f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
        c[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(192.0f / 255, 192.0f / 255, 200.0f / 255, 1);
        c[ImGuiCol_FrameBg] = ImVec4(0.105f, 0.112f, 0.122f, 0.96f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.155f, 0.164f, 0.178f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.195f, 0.205f, 0.222f, 1.0f);
        c[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.058f, 0.063f, 0.97f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.079f, 0.086f, 0.98f);
        c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
        c[ImGuiCol_MenuBarBg] = c[ImGuiCol_ChildBg];
        c[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.93f, 0.89f, 1);
        c[ImGuiCol_SliderGrab] = ImVec4(0.56f, 0.60f, 0.68f, 1.0f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.72f, 0.76f, 0.84f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.055f);
        c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.10f);
        c[ImGuiCol_ButtonActive] = g_appearance.accent;
        c[ImGuiCol_Header] = c[ImGuiCol_Button];
        c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
        c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
        c[ImGuiCol_Tab] = ImVec4(0.090f, 0.095f, 0.104f, 0.98f);
        c[ImGuiCol_TabHovered] = ImVec4(0.175f, 0.185f, 0.202f, 1.0f);
        c[ImGuiCol_TabActive] = ImVec4(0.145f, 0.154f, 0.168f, 1.0f);
        c[ImGuiCol_Separator] = c[ImGuiCol_Border];
        c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SliderGrab];
        c[ImGuiCol_SeparatorActive] = c[ImGuiCol_SliderGrabActive];
        c[ImGuiCol_PlotLines] = ImVec4(0.68f, 0.72f, 0.80f, 1.0f);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.62f, 0.67f, 0.75f, 1.0f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.045f, 0.048f, 0.052f, 0.70f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.26f, 0.29f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.36f, 0.40f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.43f, 0.45f, 0.50f, 1.0f);
        c[ImGuiCol_ResizeGrip] = ImVec4(0.45f, 0.48f, 0.54f, 0.45f);
        c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_SliderGrab];
        c[ImGuiCol_ResizeGripActive] = c[ImGuiCol_SliderGrabActive];
        c[ImGuiCol_NavHighlight] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    LoadOverlayBackground();
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_dev, g_ctx))
        return false;
    return true;
}

static void ReleaseGraphicsResources()
{
    if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
        ImGui_ImplDX11_Shutdown();
    if (g_ctx) { g_ctx->ClearState(); g_ctx->Flush(); }
    SafeRelease(g_colorSRV); SafeRelease(g_colorSampleTex); SafeRelease(g_colorTex);
    SafeRelease(g_depthSRV); SafeRelease(g_depthTex);
    SafeRelease(g_rtv); SafeRelease(g_localDepthSRV);
    SafeRelease(g_localDSV); SafeRelease(g_localDepth);
    SafeRelease(g_dsWrite); SafeRelease(g_dsOff);
    SafeRelease(g_blendColor); SafeRelease(g_blendDepthOnly); SafeRelease(g_sceneRaster);
    SafeRelease(g_sampLinear); SafeRelease(g_sampPoint);
    SafeRelease(g_uiRTV); SafeRelease(g_uiRTSRV); SafeRelease(g_uiRT);
    SafeRelease(g_uiRTV2); SafeRelease(g_uiRT2); ReleaseUiReadbacks();
    SafeRelease(g_uiCB); SafeRelease(g_psPremult);
    SafeRelease(g_cbCombined); SafeRelease(g_cbColor); SafeRelease(g_cb);
    SafeRelease(g_vs); SafeRelease(g_psColor); SafeRelease(g_psDepth);
    SafeRelease(g_psColorDepth); SafeRelease(g_psDepthView);
    SafeRelease(g_adapter3); SafeRelease(g_backgroundSrv);
    SafeRelease(g_swap); SafeRelease(g_ctx); SafeRelease(g_dev);
    g_lastColorHandle = g_lastDepthHandle = 0;
    g_lastColorW = g_lastColorH = g_lastColorFmt = 0;
    g_lastDepthW = g_lastDepthH = g_lastDepthFmt = 0;
    g_colorTexW = g_colorTexH = g_depthTexW = g_depthTexH = 0;
    g_colorNeedsResolve = false; g_colorResolveFormat = DXGI_FORMAT_UNKNOWN;
    g_uiW = g_uiH = g_uiRegionW = g_uiRegionH = 0;
    g_uiReadbackSerial = 0;
    g_backgroundWidth = g_backgroundHeight = 0;
    g_autoArmed = false; g_framesPresented = 0; g_firstPresentTick = 0;
}

static void RequestGraphicsRecovery(HRESULT reason)
{
    if (g_graphicsRecovering) return;
    Log("[D3D] recovering device (0x%08lX), preserving overlay state", (unsigned long)reason);
    g_graphicsRecovering = true;
    g_graphicsRetryTick = 0;
    ShowWindow(g_hwnd, SW_HIDE);
    HideUiLayer();
}

static bool ProcessGraphicsRecovery()
{
    if (!g_graphicsRecovering) return true;
    const DWORD now = GetTickCount();
    if (g_graphicsRetryTick && (LONG)(now - g_graphicsRetryTick) < 0) return false;
    ReleaseGraphicsResources();
    RECT client = {};
    if (GetClientRect(g_hwnd, &client))
    {
        g_width = (UINT)std::max<LONG>(1, client.right - client.left);
        g_height = (UINT)std::max<LONG>(1, client.bottom - client.top);
    }
    if (!InitD3D(true))
    {
        ReleaseGraphicsResources();
        g_graphicsRetryTick = GetTickCount() + 1000;
        Log("[D3D] recreate failed; retrying in 1 second");
        return false;
    }
    g_graphicsRecovering = false;
    g_graphicsRetryTick = 0;
    ++g_graphicsGeneration;
    g_overlayHiddenForForeground = true;
    g_lastRect = { 0,0,0,0 };

    if (g_desktop && g_overlayEnabled) ApplyWindowMode();
    else UpdateForegroundVisibility();
    Log("[D3D] device restored, generation=%llu", (unsigned long long)g_graphicsGeneration);
    return true;
}

static bool CreateUiRtv(ID3D11Texture2D* tex, ID3D11RenderTargetView** out)
{
    D3D11_RENDER_TARGET_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    return SUCCEEDED(g_dev->CreateRenderTargetView(tex, &v, out));
}

static bool EnsureUiLayerSize(UINT w, UINT h)
{
    if (!w || !h) return false;
    if (g_uiRT && g_uiRTV && g_uiRTSRV && g_uiW == w && g_uiH == h) return true;

    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_uiRTV); SafeRelease(g_uiRTSRV); SafeRelease(g_uiRT);

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_uiRT)))
        return false;

    g_uiRTV = nullptr;
    if (!CreateUiRtv(g_uiRT, &g_uiRTV))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
    s.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    s.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateShaderResourceView(g_uiRT, &s, &g_uiRTSRV)))
        return false;

    g_uiW = w; g_uiH = h;
    Log("[UI] layer render target %ux%u", w, h);
    return true;
}

static bool EnsureUiRegion(UINT w, UINT h)
{
    if (!w || !h) return false;
    if (g_uiRT2 && g_uiRTV2 && g_uiReadbacks[0].texture &&
        g_uiReadbacks[1].texture && g_uiReadbacks[2].texture &&
        g_uiRegionW == w && g_uiRegionH == h)
        return true;

    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_uiRTV2); SafeRelease(g_uiRT2);
    ReleaseUiReadbacks();

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_uiRT2)))
        return false;

    g_uiRTV2 = nullptr;
    if (!CreateUiRtv(g_uiRT2, &g_uiRTV2))
        return false;

    D3D11_TEXTURE2D_DESC sd = d;
    sd.BindFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& slot : g_uiReadbacks)
    {
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &slot.texture)))
        {
            ReleaseUiReadbacks();
            return false;
        }
    }

    g_uiRegionW = w; g_uiRegionH = h;
    Log("[UI] region %ux%u", w, h);
    return true;
}

static bool EnsureUiBitmap(UINT w, UINT h)
{
    if (g_uiBitmap && g_uiBitmapW == w && g_uiBitmapH == h)
        return true;

    if (g_uiBitmap)
    {
        if (g_uiDc && g_uiOriginalBitmap) SelectObject(g_uiDc, g_uiOriginalBitmap);
        DeleteObject(g_uiBitmap);
        g_uiBitmap = nullptr;
    }
    g_uiBits = nullptr;

    if (!g_uiDc)
        g_uiDc = CreateCompatibleDC(nullptr);
    if (!g_uiDc) return false;

    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = (LONG)w;
    bh.bV5Height = -(LONG)h;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    g_uiBitmap = CreateDIBSection(g_uiDc, (BITMAPINFO*)&bh, DIB_RGB_COLORS,
        &bits, nullptr, 0);
    if (!g_uiBitmap || !bits)
    {
        if (g_uiBitmap) { DeleteObject(g_uiBitmap); g_uiBitmap = nullptr; }
        return false;
    }
    g_uiBits = bits;
    g_uiBitmapW = w;
    g_uiBitmapH = h;
    HGDIOBJ previousBitmap = SelectObject(g_uiDc, g_uiBitmap);
    if (!g_uiOriginalBitmap) g_uiOriginalBitmap = previousBitmap;
    return true;
}

static bool EnsureFpsSurface(UINT w, UINT h)
{
    if (g_fpsBitmap && g_fpsBits) return true;
    if (!g_fpsDc) g_fpsDc = CreateCompatibleDC(nullptr);
    if (!g_fpsDc) return false;

    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = (LONG)w;
    bh.bV5Height = -(LONG)h;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;

    g_fpsBitmap = CreateDIBSection(g_fpsDc, (BITMAPINFO*)&bh,
        DIB_RGB_COLORS, &g_fpsBits, nullptr, 0);
    if (!g_fpsBitmap || !g_fpsBits) return false;
    g_fpsOriginalBitmap = SelectObject(g_fpsDc, g_fpsBitmap);
    g_fpsFont = CreateFontW(-15, 0, 0, 0, g_privateUiFontLoaded ? FW_NORMAL : FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        g_privateUiFontLoaded ? L"Nunito Sans" : L"Segoe UI");
    return true;
}

static void RenderFpsLayer()
{
    constexpr int width = 72;
    constexpr int height = 30;
    constexpr int radius = 7;
    if (!g_fpsHwnd || !g_showFPS || !g_overlayEnabled ||
        g_overlayHiddenForForeground || g_reshadeInput || g_resyncing)
    {
        HideFpsLayer();
        return;
    }
    if (!EnsureFpsSurface(width, height)) return;

    char text[32];
    if (g_sourceFpsAvailable) snprintf(text, sizeof(text), "%.0f FPS", g_fps);
    else snprintf(text, sizeof(text), "-- FPS");

    const COLORREF textColor = RGB(
        static_cast<BYTE>(std::clamp(static_cast<int>(lroundf(g_fpsColor[0] * 255.0f)), 0, 255)),
        static_cast<BYTE>(std::clamp(static_cast<int>(lroundf(g_fpsColor[1] * 255.0f)), 0, 255)),
        static_cast<BYTE>(std::clamp(static_cast<int>(lroundf(g_fpsColor[2] * 255.0f)), 0, 255)));
    static HBITMAP renderedBitmap = nullptr;
    static char renderedText[32] = {};
    static COLORREF renderedTextColor = 0;
    const bool redraw = renderedBitmap != g_fpsBitmap || strcmp(renderedText, text) != 0 ||
        renderedTextColor != textColor;
    if (redraw)
    {
        RECT rc = { 0, 0, width, height };
        memset(g_fpsBits, 0, width * height * 4);
        HBRUSH background = CreateSolidBrush(RGB(17, 18, 20));
        FillRect(g_fpsDc, &rc, background);
        DeleteObject(background);

        HGDIOBJ oldFont = nullptr;
        if (g_fpsFont) oldFont = SelectObject(g_fpsDc, g_fpsFont);
        SetBkMode(g_fpsDc, TRANSPARENT);
        SetTextColor(g_fpsDc, textColor);
        DrawTextA(g_fpsDc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (oldFont) SelectObject(g_fpsDc, oldFont);

        DWORD* pixels = reinterpret_cast<DWORD*>(g_fpsBits);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int cx = x < radius ? radius - x : (x >= width - radius ? x - (width - radius - 1) : 0);
                const int cy = y < radius ? radius - y : (y >= height - radius ? y - (height - radius - 1) : 0);
                DWORD& pixel = pixels[y * width + x];
                if (cx && cy && cx * cx + cy * cy > radius * radius)
                {
                    pixel = 0;
                    continue;
                }

                const BYTE b = static_cast<BYTE>(pixel & 0xff);
                const BYTE g = static_cast<BYTE>((pixel >> 8) & 0xff);
                const BYTE r = static_cast<BYTE>((pixel >> 16) & 0xff);
                const bool textPixel = r > 32 || g > 32 || b > 32;
                const BYTE alpha = textPixel ? 255 : 210;
                const BYTE outR = static_cast<BYTE>((r * alpha + 127) / 255);
                const BYTE outG = static_cast<BYTE>((g * alpha + 127) / 255);
                const BYTE outB = static_cast<BYTE>((b * alpha + 127) / 255);
                pixel = (static_cast<DWORD>(alpha) << 24) | (static_cast<DWORD>(outR) << 16) |
                    (static_cast<DWORD>(outG) << 8) | outB;
            }
        }
        renderedBitmap = g_fpsBitmap;
        strcpy_s(renderedText, text);
        renderedTextColor = textColor;
    }

    RECT overlayRect = {};
    GetWindowRect(g_hwnd, &overlayRect);

    static bool dragging = false;
    static POINT dragOffset = {};
    POINT cursor = {};
    GetCursorPos(&cursor);
    const bool leftDown = !g_externalOverlayForeground && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const int currentLeft = overlayRect.left + g_fpsOffsetX;
    const int currentTop = overlayRect.top + g_fpsOffsetY;
    const bool hovered = cursor.x >= currentLeft && cursor.x < currentLeft + width &&
        cursor.y >= currentTop && cursor.y < currentTop + height;

    if (g_menu && leftDown && !dragging && hovered)
    {
        dragging = true;
        dragOffset.x = cursor.x - currentLeft;
        dragOffset.y = cursor.y - currentTop;
    }
    if (dragging)
    {
        if (g_menu && leftDown)
        {
            g_fpsOffsetX = std::clamp(
                (int)(cursor.x - overlayRect.left - dragOffset.x),
                0, std::max(0, (int)g_width - width));
            g_fpsOffsetY = std::clamp(
                (int)(cursor.y - overlayRect.top - dragOffset.y),
                0, std::max(0, (int)g_height - height));
        }
        else
        {
            dragging = false;
            SaveSettings();
        }
    }

    g_fpsOffsetX = std::clamp(g_fpsOffsetX, 0, std::max(0, (int)g_width - width));
    g_fpsOffsetY = std::clamp(g_fpsOffsetY, 0, std::max(0, (int)g_height - height));
    POINT dst = {
        overlayRect.left + g_fpsOffsetX,
        overlayRect.top + g_fpsOffsetY
    };
    static POINT renderedPosition = { LONG_MIN, LONG_MIN };
    const bool visible = IsWindowVisible(g_fpsHwnd) != FALSE;
    const bool positionChanged = renderedPosition.x != dst.x || renderedPosition.y != dst.y;
    if (!redraw && visible && !positionChanged) return;

    POINT src = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screenDc = GetDC(nullptr);
    const BOOL updated = UpdateLayeredWindow(g_fpsHwnd, screenDc, &dst, &size, g_fpsDc,
        &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screenDc);
    if (updated)
    {
        renderedPosition = dst;
    }
    if (updated && !visible)
    {
        SetWindowPos(g_fpsHwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RefreshOverlayZOrder();
    }
}

static void SetUiRemap(UINT fullW, UINT fullH, UINT rx, UINT ry, UINT rw, UINT rh)
{
    if (!g_uiCB) return;
    RemapCB cb = {};
    cb.uvScale[0] = fullW ? (float)rw / (float)fullW : 1.0f;
    cb.uvScale[1] = fullH ? (float)rh / (float)fullH : 1.0f;
    cb.uvBias[0] = fullW ? (float)rx / (float)fullW : 0.0f;
    cb.uvBias[1] = fullH ? (float)ry / (float)fullH : 0.0f;

    static ConstantUploadCache<RemapCB> cache;
    UploadCachedConstants(g_uiCB, cb, cache);
    g_ctx->PSSetConstantBuffers(0, 1, &g_uiCB);
}

struct UiRegionBounds { int x, y, w, h; };
static bool ComputeUiRegion(float minX, float minY, float maxX, float maxY,
    UINT screenW, UINT screenH, UiRegionBounds& out)
{
    if (!screenW || !screenH || !std::isfinite(minX) || !std::isfinite(minY) ||
        !std::isfinite(maxX) || !std::isfinite(maxY)) return false;

    const float w = (float)screenW, h = (float)screenH;
    const int x = (int)floorf(std::clamp(minX, 0.0f, w));
    const int y = (int)floorf(std::clamp(minY, 0.0f, h));
    const int right = (int)ceilf(std::clamp(maxX, 0.0f, w));
    const int bottom = (int)ceilf(std::clamp(maxY, 0.0f, h));
    if (right <= x || bottom <= y) return false;
    out = { x, y, std::min((right - x + 63) / 64 * 64, (int)screenW - x),
        std::min((bottom - y + 63) / 64 * 64, (int)screenH - y) };
    return true;
}

static void StabilizeUiRegion(UiRegionBounds& region, UINT screenW, UINT screenH,
    UINT allocatedW, UINT allocatedH)
{
    region.w = std::min(static_cast<int>(screenW), std::max(region.w, static_cast<int>(allocatedW)));
    region.h = std::min(static_cast<int>(screenH), std::max(region.h, static_cast<int>(allocatedH)));
    region.x = std::clamp(region.x, 0, static_cast<int>(screenW) - region.w);
    region.y = std::clamp(region.y, 0, static_cast<int>(screenH) - region.h);
}


static void DrawLauncherBackground()
{
    const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float radius = ImGui::GetStyle().WindowRounding;
    const ImVec4 panelColor(g_appearance.background.x, g_appearance.background.y,
        g_appearance.background.z, g_appearance.opacity);
    draw->AddRectFilled(pos, end, ImGui::GetColorU32(panelColor), radius);
    if (g_backgroundSrv && g_backgroundWidth && g_backgroundHeight) {
        const float sourceAspect = (float)g_backgroundWidth / g_backgroundHeight;
        const float targetAspect = size.x / std::max(1.0f, size.y);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (sourceAspect > targetAspect) { float visible = targetAspect / sourceAspect; uv0.x = (1 - visible) * 0.5f; uv1.x = 1 - uv0.x; }
        else { float visible = sourceAspect / targetAspect; uv0.y = (1 - visible) * 0.5f; uv1.y = 1 - uv0.y; }
        draw->AddImageRounded((ImTextureID)(intptr_t)g_backgroundSrv, pos, end, uv0, uv1,
            ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), radius);
        draw->AddRectFilled(pos, end, ImGui::GetColorU32(ImVec4(10.0f / 255, 10.0f / 255, 10.0f / 255, g_appearance.backgroundDim)), radius);
    }
    draw->AddRect(pos, end, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)), radius);
}
static void DrawLauncherHeader()
{
    ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const bool controls = g_appearance.showControls;
    const float left = controls ? 38.0f : 12.0f;
    const float right = 12.0f;

    ImGui::SetCursorScreenPos(ImVec2(pos.x + left, pos.y + 5));
    ImGui::InvisibleButton("##launcher_drag", ImVec2(std::max(40.0f, size.x - left - right), 24));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        ImGui::SetWindowPos(ImVec2(std::max(0.0f, std::min((float)g_width - size.x, pos.x + delta.x)),
            std::max(0.0f, std::min((float)g_height - size.y, pos.y + delta.y))));
        pos = ImGui::GetWindowPos();
    }
    if (controls) {
        const ImVec2 button(pos.x + 12.0f, pos.y + 5.0f);
        ImGui::SetCursorScreenPos(button);
        ImGui::InvisibleButton("##overlay_close", ImVec2(18, 24));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) g_running = false;
        const ImVec4 red = hovered ? ImVec4(0.88f, 0.33f, 0.30f, 1) : ImVec4(1, 95.0f / 255, 87.0f / 255, 1);
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(button.x + 9, button.y + 12), 6,
            ImGui::GetColorU32(red), 20);
        if (hovered) ImGui::SetTooltip("Exit");
    }
    ImGui::SetCursorPos(ImVec2(12, 34));
}

static void DrawLauncherNavigation()
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + 164, pos.y + 34), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.04f)), 17);
    draw->AddRect(pos, ImVec2(pos.x + 164, pos.y + 34), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.08f)), 17);
    for (int tab = 0;tab < 2;++tab) {
        const ImVec2 button(pos.x + 3 + tab * 80, pos.y + 3);
        ImGui::SetCursorScreenPos(button);
        ImGui::InvisibleButton(tab == 0 ? "##home_tab" : "##gpu_tab", ImVec2(78, 28));
        if (ImGui::IsItemClicked()) g_mainTab = tab;
        const bool active = g_mainTab == tab, hovered = ImGui::IsItemHovered();
        if (active || hovered) draw->AddRectFilled(button, ImVec2(button.x + 78, button.y + 28),
            ImGui::GetColorU32(active ? g_appearance.accent : ImVec4(1, 1, 1, 0.08f)), 14);
        const char* label = tab == 0 ? "Home" : "GPU";
        const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(13.0f, FLT_MAX, 0, label);
        draw->AddText(ImGui::GetFont(), 13.0f, ImVec2(button.x + (78 - textSize.x) * 0.5f, button.y + (28 - textSize.y) * 0.5f),
            ImGui::GetColorU32(active ? ImVec4(1, 1, 1, 1) : ImVec4(192.0f / 255, 192.0f / 255, 200.0f / 255, 1)), label);
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + 44));
}

static void LauncherSection(const char* label)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), 13.0f, pos,
        ImGui::GetColorU32(ImVec4(192.0f / 255, 192.0f / 255, 200.0f / 255, 1)), label);
    ImGui::Dummy(ImVec2(0, 13));
    ImGui::Separator();
}
static bool LauncherToggle(const char* label, bool* value)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = std::max(60.0f, ImGui::GetContentRegionAvail().x);
    ImGui::InvisibleButton(label, ImVec2(width, 24));
    const bool changed = ImGui::IsItemClicked();
    if (changed) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddText(ImVec2(pos.x, pos.y + 4), ImGui::GetColorU32(ImGuiCol_Text), label);
    const ImVec2 a(pos.x + width - 38, pos.y + 2), b(pos.x + width, pos.y + 22);
    draw->AddRectFilled(a, b, ImGui::GetColorU32(*value ? g_appearance.accent : ImVec4(1, 1, 1, 0.08f)), 10);
    draw->AddRect(a, b, ImGui::GetColorU32(ImVec4(1, 1, 1, hovered ? 0.25f : 0.12f)), 10);
    draw->AddCircleFilled(ImVec2(a.x + (*value ? 28.0f : 10.0f), a.y + 10), 7,
        ImGui::GetColorU32(*value ? ImVec4(1, 1, 1, 1) : ImVec4(0.64f, 0.64f, 0.67f, 1)), 20);
    return changed;
}
static bool LauncherExitButton()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 100.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.90f, 0.92f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.80f, 0.83f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(20.0f / 255, 20.0f / 255, 20.0f / 255, 1));
    const bool clicked = ImGui::Button("Exit", ImVec2(-1, 32));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return clicked;
}

static void DrawPhotoModeControl()
{
    const bool usable = g_nativeUi.ReadyForRequest() || g_nativeUi.Desired();
    bool hidden = g_nativeUi.Desired();
    ImGui::BeginDisabled(!usable);
    if (LauncherToggle(u8"Фото режим", &hidden))
    {
        g_nativeUi.Set(hidden);
        g_nativeUi.Tick(g_targetPid);
        InvalidateOverlayFrame();
    }
    ImGui::EndDisabled();
}

static void RenderUiLayer(const PipePayload& p)
{
    const bool drawUi = g_menu || g_showKeyHelp;

    if (!g_uiHwnd || !g_overlayEnabled || g_overlayHiddenForForeground ||
        g_reshadeInput || g_resyncing || !drawUi)
    {
        HideMenuLayer();
        return;
    }

    static bool lastMenu = false;
    static bool lastShowKeyHelp = false;
    static bool lastShowFPS = false;
    static UINT lastWidth = 0, lastHeight = 0;
    const bool uiModeChanged = lastMenu != g_menu || lastShowKeyHelp != g_showKeyHelp ||
        lastShowFPS != g_showFPS || lastWidth != g_width || lastHeight != g_height;
    lastShowKeyHelp = g_showKeyHelp;
    if (uiModeChanged)
        for (auto& slot : g_uiReadbacks) slot.pending = false;
    lastMenu = g_menu; lastShowFPS = g_showFPS;
    lastWidth = g_width; lastHeight = g_height;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_externalOverlayForeground)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        for (bool& down : io.MouseDown) down = false;
    }

    if (g_menu)
    {
        const float maxW = std::max(240.0f, (float)g_width - 16.0f);
        const float maxH = std::max(180.0f, (float)g_height - 16.0f);
        const ImVec2 initial(std::min(400.0f, maxW), std::min(430.0f, maxH));
        const ImVec2 initialPos(
            std::max(0.0f, ((float)g_width - initial.x) * 0.5f),
            std::max(0.0f, ((float)g_height - initial.y) * 0.5f));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(300.0f, maxW), std::min(240.0f, maxH)), ImVec2(maxW, maxH));
        ImGui::SetNextWindowPos(initialPos,
            g_uiLayoutReset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(initial,
            g_uiLayoutReset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
        if (ImGui::Begin("qwleyshade###qwleyshade_window", nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            DrawLauncherBackground();
            DrawLauncherHeader();
            DrawLauncherNavigation();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

            if (ImGui::BeginChild("##overlay_content", ImVec2(0, 0), true)) {
                if (g_mainTab == 0) {
                    LauncherSection("SYSTEM");
                    ImGui::TextUnformatted("v1.3.4");
                    ImGui::TextWrapped("%s", OverlayStatus(p));
                    if (g_sourceFpsAvailable)
                        ImGui::TextColored(ImVec4(g_fpsColor[0], g_fpsColor[1], g_fpsColor[2], 1), "%.0f FPS", g_fps);
                    else ImGui::TextDisabled("-- FPS");
                    LauncherSection("OPTIONS");
                    DrawPhotoModeControl();
                    if (LauncherToggle("Show FPS", &g_showFPS)) SaveSettings();
                    const float swatchX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight();
                    ImGui::TextUnformatted("FPS color");
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(swatchX);
                    ImGui::ColorEdit3("##fps_color", g_fpsColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                    if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
                    char depthToggleLabel[128];
                    snprintf(depthToggleLabel, sizeof(depthToggleLabel),
                        "Depth preview (%s)", VkKeyName(g_depthPreviewKey));
                    if (LauncherToggle(depthToggleLabel, &g_depthPreview)) SaveSettings();
                    char depthKeyLabel[128];
                    if (g_waitKeyTarget == 2) snprintf(depthKeyLabel, sizeof(depthKeyLabel), "Depth preview key: [press a key...]");
                    else snprintf(depthKeyLabel, sizeof(depthKeyLabel), "Depth preview key: [%s]", VkKeyName(g_depthPreviewKey));
                    if (ImGui::Button(depthKeyLabel, ImVec2(-1, 30))) { g_waitKeyTarget = 2; g_waitKeyStart = GetTickCount(); }
                    if (g_waitKeyTarget == 2) ImGui::TextDisabled("Escape cancels");
                    LauncherSection("CONTROL");
                    char overlayKeyLabel[128];
                    if (g_waitKeyTarget == 1) snprintf(overlayKeyLabel, sizeof(overlayKeyLabel), "Hide overlay: [press a key...]");
                    else snprintf(overlayKeyLabel, sizeof(overlayKeyLabel), "Hide overlay: [%s]", VkKeyName(g_minimizeKey));
                    if (ImGui::Button(overlayKeyLabel, ImVec2(-1, 30))) { g_waitKeyTarget = 1; g_waitKeyStart = GetTickCount(); }
                    if (g_waitKeyTarget == 1) ImGui::TextDisabled("Escape cancels");
                    if (LauncherExitButton()) g_running = false;
                }
                else DrawGpuStatsTab();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        ImGui::End();
        ImGui::PopStyleVar();
        if (g_uiLayoutReset) { g_uiLayoutReset = false; SaveSettings(); }
        if (!g_menu && !g_reshadeInput) SetMenu(false);

    }

    if (g_showKeyHelp && !g_menu)
    {
        ImGui::SetNextWindowPos(
            ImVec2((float)g_width * 0.5f, (float)g_height * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(295.0f, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.24f, 0.25f, 0.26f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.24f, 0.25f, 0.26f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.18f, 0.18f, 0.90f));

        if (ImGui::Begin("Keybind Help", nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            std::string bindText = MenuBindingText();
            ImGui::TextColored(ImVec4(0.92f, 0.90f, 0.35f, 1.0f),
                "Press %s to open settings", bindText.c_str());
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.78f, 0.78f, 1.0f));
            ImGui::TextWrapped(
                "This popup will disappear after you open the settings menu for the first time.");
            ImGui::PopStyleColor();
        }
        ImGui::End();

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
    }

    ImGui::Render();
    if (!g_running || !g_overlayEnabled || (!g_menu && !g_showKeyHelp)) {
        HideMenuLayer();
        for (auto& slot : g_uiReadbacks) slot.pending = false;
        return;
    }

    if (g_reshadeInput) { HideMenuLayer(); return; }

    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount <= 0) { HideMenuLayer(); return; }

    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    for (int n = 0; n < dd->CmdListsCount; ++n)
    {
        const ImDrawList* cl = dd->CmdLists[n];
        float listMinX = FLT_MAX, listMinY = FLT_MAX;
        float listMaxX = -FLT_MAX, listMaxY = -FLT_MAX;
        for (int i = 0; i < cl->VtxBuffer.Size; ++i)
        {
            const ImVec2& vertex = cl->VtxBuffer[i].pos;
            listMinX = std::min(listMinX, vertex.x); listMinY = std::min(listMinY, vertex.y);
            listMaxX = std::max(listMaxX, vertex.x); listMaxY = std::max(listMaxY, vertex.y);
        }
        for (int i = 0; i < cl->CmdBuffer.Size; ++i)
        {
            const ImDrawCmd& cmd = cl->CmdBuffer[i];
            if (!cmd.ElemCount) continue;
            const float x0 = std::max(listMinX, cmd.ClipRect.x);
            const float y0 = std::max(listMinY, cmd.ClipRect.y);
            const float x1 = std::min(listMaxX, cmd.ClipRect.z);
            const float y1 = std::min(listMaxY, cmd.ClipRect.w);
            if (x0 >= x1 || y0 >= y1) continue;
            minX = std::min(minX, x0); minY = std::min(minY, y0);
            maxX = std::max(maxX, x1); maxY = std::max(maxY, y1);
        }
    }
    if (minX > maxX || minY > maxY) return;

    UiRegionBounds region = {};
    if (!ComputeUiRegion(minX, minY, maxX, maxY, g_width, g_height, region))
    {
        HideMenuLayer(); return;
    }
    StabilizeUiRegion(region, g_width, g_height, g_uiRegionW, g_uiRegionH);
    int rx = region.x, ry = region.y, rw = region.w, rh = region.h;
    if (!EnsureUiLayerSize((UINT)rw, (UINT)rh) ||
        !EnsureUiRegion((UINT)rw, (UINT)rh) || !EnsureUiBitmap((UINT)rw, (UINT)rh)) return;

    RECT parentRect = {};
    GetWindowRect(g_hwnd, &parentRect);
    POINT destination = { parentRect.left + rx, parentRect.top + ry };
    const bool layerVisible = IsWindowVisible(g_uiHwnd) != FALSE;

    UiReadbackSlot* freeSlot = nullptr;
    for (auto& slot : g_uiReadbacks)
        if (!slot.pending) { freeSlot = &slot; break; }
    if (freeSlot)
    {
        const ImVec2 savedPos = dd->DisplayPos;
        const ImVec2 savedSize = dd->DisplaySize;
        dd->DisplayPos = ImVec2((float)rx, (float)ry);
        dd->DisplaySize = ImVec2((float)rw, (float)rh);
        ID3D11RenderTargetView* rtv = g_uiRTV;
        const float clear[4] = { 0, 0, 0, 0 };
        g_ctx->ClearRenderTargetView(rtv, clear);
        g_ctx->OMSetDepthStencilState(g_dsOff, 0);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT v = {};
        v.Width = (float)rw; v.Height = (float)rh; v.MaxDepth = 1.0f;
        g_ctx->RSSetViewports(1, &v);
        ImGui_ImplDX11_RenderDrawData(dd);
        dd->DisplayPos = savedPos;
        dd->DisplaySize = savedSize;

        rtv = g_uiRTV2;

        g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        g_ctx->RSSetState(nullptr);
        g_ctx->OMSetDepthStencilState(g_dsOff, 0);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_psPremult, nullptr, 0);
        SetUiRemap((UINT)rw, (UINT)rh, 0, 0, (UINT)rw, (UINT)rh);
        ID3D11ShaderResourceView* srv = g_uiRTSRV;
        g_ctx->PSSetShaderResources(0, 1, &srv);
        g_ctx->PSSetSamplers(0, 1, &g_sampLinear);
        D3D11_VIEWPORT rv = {};
        rv.Width = (float)rw; rv.Height = (float)rh; rv.MaxDepth = 1.0f;
        g_ctx->RSSetViewports(1, &rv);
        DrawTri();
        ID3D11ShaderResourceView* nullSRV = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &nullSRV);

        g_ctx->CopyResource(freeSlot->texture, g_uiRT2);
        freeSlot->pending = true;
        freeSlot->serial = ++g_uiReadbackSerial;
        freeSlot->x = rx; freeSlot->y = ry;
         
         
        g_ctx->Flush();
    }
    UiReadbackSlot* displayed = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    uint64_t upperSerial = UINT64_MAX;
    for (int attempt = 0; attempt < kUiReadbackSlots; ++attempt)
    {
        UiReadbackSlot* candidate = nullptr;
        for (auto& slot : g_uiReadbacks)
        {
            if (slot.pending && slot.serial < upperSerial &&
                (!candidate || slot.serial > candidate->serial))
                candidate = &slot;
        }
        if (!candidate) break;
        upperSerial = candidate->serial;
        const HRESULT hr = g_ctx->Map(candidate->texture, 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(hr))
        {
            displayed = candidate;
            break;
        }
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            continue;
        }
        candidate->pending = false;
    }
    if (!displayed) return;

    const size_t rowBytes = (size_t)rw * 4;
    const BYTE* src = (const BYTE*)mapped.pData;
    BYTE* dst = (BYTE*)g_uiBits;
    if (mapped.RowPitch == rowBytes)
        memcpy(dst, src, rowBytes * (size_t)rh);
    else
        for (int y = 0; y < rh; ++y)
            memcpy(dst + (size_t)y * rowBytes, src + (size_t)y * mapped.RowPitch, rowBytes);
    g_ctx->Unmap(displayed->texture, 0);

    const uint64_t displayedSerial = displayed->serial;
    for (auto& slot : g_uiReadbacks)
        if (slot.pending && slot.serial <= displayedSerial)
            slot.pending = false;

    rx = displayed->x;
    ry = displayed->y;
    destination = { parentRect.left + rx, parentRect.top + ry };

    POINT srcPos = { 0, 0 };
    SIZE winSize = { (LONG)rw, (LONG)rh };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, static_cast<BYTE>(g_menu ? g_appearance.opacity * 255.0f + 0.5f : 255.0f), AC_SRC_ALPHA };

    if (UpdateLayeredWindow(g_uiHwnd, nullptr, &destination, &winSize, g_uiDc,
        &srcPos, 0, &bf, ULW_ALPHA) && !layerVisible)
    {
        SetWindowPos(g_uiHwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RefreshOverlayZOrder();
    }

    g_ctx->OMSetRenderTargets(1, &g_rtv, g_localDSV);
}

static void Follow()
{
    if (g_desktop) return;
    const DWORD now = GetTickCount();

    if (now - g_lastFollow < 33) return;
    g_lastFollow = now;

    static DWORD lastProcessProbe = 0;
    static bool targetProcessDead = false;

    if (now - lastProcessProbe >= 1000)
    {
        lastProcessProbe = now;
        targetProcessDead = g_targetPid != 0 && !IsProcessAlive(g_targetPid);
    }
    if (!g_target || !IsWindow(g_target) || targetProcessDead)
    {
        static DWORD lastSearch = 0;
        if (now - lastSearch < 250) return;
        lastSearch = now;

        const DWORD previousPid = g_targetPid;
        HWND found = FindTarget();
        if (!found)
        {
            if (previousPid && IsProcessAlive(previousPid)) return;
            Log("[Window] Roblox process exited; closing overlay");
            g_running = false;
            if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
            return;
        }

        g_target = found;
        targetProcessDead = false;
        char exe[MAX_PATH] = {};
        QueryProcessExeName(g_targetPid, exe, sizeof(exe));
        g_targetExeName = exe;
        g_lastRect = { 0,0,0,0 };
        Log("[Window] reattached to %s (pid %lu)", exe[0] ? exe : "?", g_targetPid);
    }

    UpdateForegroundVisibility();
    RefreshTargetPids();
    if (!g_overlayEnabled || g_overlayHiddenForForeground || g_resyncing || g_graphicsRecovering) return;

    RECT r = TargetRect();
    if (memcmp(&r, &g_lastRect, sizeof(r)))
    {
        g_lastRect = r;
        SetWindowPos(g_hwnd, nullptr,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (g_fpsHwnd && IsWindowVisible(g_fpsHwnd))
            SetWindowPos(g_fpsHwnd, nullptr,
                r.left + g_fpsOffsetX, r.top + g_fpsOffsetY, 0, 0,
                SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    static DWORD lastZOrderTick = 0;
    if (now - lastZOrderTick >= 250)
    {
        lastZOrderTick = now;
        RefreshOverlayZOrder();
    }
}

static void RequestOverlayResync(const char* reason, DWORD delayMs)
{
    Log("[Resync] requested: %s", reason ? reason : "manual");
    g_menu = false;
    g_reshadeInput = false;
    g_resyncing = true;
    ApplyWindowMode();
    if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE);
    HideUiLayer();
    g_lastRect = { 0,0,0,0 };
    g_resyncDueTick = GetTickCount() + delayMs;
}

static void ForceDepthRebuildTick(const char* reason)
{
    if (!g_ctx || !g_dev || g_graphicsRecovering) return;

    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    SafeRelease(g_localDepthSRV);
    SafeRelease(g_localDSV);
    SafeRelease(g_localDepth);

    const bool ok = g_depthPassEnabled ? CreateLocalDepth(g_width, g_height) : false;

    g_ctx->Flush();

    ++g_depthRecreateCount;
    Log("[AutoColorDepth] depth rebuild #%d (%s) %ux%u ok=%d",
        g_depthRecreateCount, reason ? reason : "auto",
        g_width, g_height, ok ? 1 : 0);
}

static void AutoColorDepthTick()
{
    if (!g_overlayEnabled) { g_autoArmed = false; return; }

    if (!g_autoDepthRebuildEnabled) return;
    if (g_framesPresented < 30 || !g_firstPresentTick) return;

    const DWORD now = GetTickCount();
    const bool pipeLive = g_pipeConnected.load(std::memory_order_acquire) &&
        g_hasPayload.load(std::memory_order_acquire) &&
        g_lastPayloadTick.load(std::memory_order_acquire) &&
        now - g_lastPayloadTick.load(std::memory_order_acquire) <= 750;

    if (!pipeLive)
    {
        g_autoArmed = false;
        return;
    }

    if (!g_autoArmed)
    {
        g_autoArmed = true;
        g_autoAttemptCount = 0;
        g_autoLastAttemptTick = 0;
        g_autoWatchStartTick = now;
        return;
    }

    PipePayload p;
    { std::lock_guard<std::mutex> lk(g_pipeMutex); p = g_payload; }
    const bool colorReady = g_colorSRV && p.colorHandle;
    const bool depthReady = g_depthSRV && p.depthHandle && (p.flags & FLAG_DEPTH_OK);
    if (depthReady || !g_depthPassEnabled)
    {
        if (g_autoAttemptCount > 0)
            Log("[AutoColorDepth] depth live after %d rebuild(s)",
                g_autoAttemptCount);
        g_autoAttemptCount = 0;
        return;
    }

    if (now - g_autoWatchStartTick < kAutoGraceMs)
        return;

    static const DWORD kAutoRetryGapMs[] = { 800, 1500, 2500, 4000 };
    const int gapCount = (int)(sizeof(kAutoRetryGapMs) / sizeof(kAutoRetryGapMs[0]));
    const DWORD retryGap = kAutoRetryGapMs[std::min(g_autoAttemptCount, gapCount - 1)];
    if (g_autoLastAttemptTick && now - g_autoLastAttemptTick < retryGap)
        return;

    Log("[AutoColorDepth] status incomplete (C=%d D=%d), rebuilding depth (attempt %d/%d)",
        colorReady ? 1 : 0, depthReady ? 1 : 0,
        g_autoAttemptCount + 1, kAutoMaxAttempts);

    ForceDepthRebuildTick("auto color+depth");

    g_autoLastAttemptTick = now;
    ++g_autoAttemptCount;
}

static void ProcessOverlayResync()
{
    if (!g_resyncDueTick || g_graphicsRecovering) return;
    DWORD now = GetTickCount();
    if ((LONG)(now - g_resyncDueTick) < 0) return;
    g_resyncDueTick = 0;

    if (!g_desktop) g_target = FindTarget();
    RECT r = TargetRect();
    int nw = std::max<int>(1, (int)(r.right - r.left));
    int nh = std::max<int>(1, (int)(r.bottom - r.top));
    SetWindowPos(g_hwnd, nullptr, r.left, r.top, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Resize((UINT)nw, (UINT)nh);
    g_lastRect = r;

    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    g_ctx->PSSetShaderResources(0, 2, none);
    ReleaseColorInput();
    ReleaseDepthInput();
    g_lastColorHandle = g_lastDepthHandle = 0;
    g_lastColorW = g_lastColorH = g_lastDepthW = g_lastDepthH = 0;
    g_colorTexW = g_colorTexH = g_depthTexW = g_depthTexH = 0;

    g_resyncing = false;
    g_overlayHiddenForForeground = true;
    ResetOverlayResume();
    InvalidateOverlayFrame();
    ApplyWindowMode();
    Log("[Resync] applied: %dx%d at %ld,%ld", nw, nh, r.left, r.top);
}

static void Hotkeys()
{
    if (!g_externalOverlayForeground && CaptureKeyIfWaiting())
        return;

    static bool pMenu = false, pF8 = false, pF1 = false, pF11 = false, pF3 = false;
    static bool previousNativeUiKey = false;

    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool menuKeyDown = (GetAsyncKeyState(g_menuKey) & 0x8000) != 0;
    const bool menuCombo = menuKeyDown &&
        (g_menuNeedsShift == shift) &&
        (g_menuNeedsCtrl == ctrl) &&
        (g_menuNeedsAlt == alt);
    const bool bareKeys = !shift && !ctrl && !alt && !g_externalOverlayForeground;
    const bool rawF8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool rawMinKey = (GetAsyncKeyState(g_minimizeKey) & 0x8000) != 0;
    const bool rawF11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool f8 = bareKeys && rawF8;
    const bool minKeyDown = bareKeys && rawMinKey;
    const bool f11 = bareKeys && rawF11;
    const bool nativeKey = bareKeys && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (nativeKey && !previousNativeUiKey && !g_graphicsRecovering && ForegroundIsTargetOrOverlay())
    {
        g_nativeUi.Set(!g_nativeUi.Desired());
        g_nativeUi.Tick(g_targetPid);
        InvalidateOverlayFrame();
    }
    previousNativeUiKey = nativeKey;

    if (menuCombo && !pMenu && !g_externalOverlayForeground && !g_graphicsRecovering && !g_reshadeInput && g_overlayEnabled)
        SetMenu(!g_menu);

    if (f8 && !pF8)
    {
        g_reshadeInput = !g_reshadeInput;
        if (g_reshadeInput) g_menu = false;
        ApplyWindowMode();

        if (g_reshadeInput)
        {
            HideUiLayer();
            if (g_hwnd)
            {
                SetForegroundWindow(g_hwnd);
                SetFocus(g_hwnd);
            }
        }
    }

    if (minKeyDown && !pF1)
    {
        g_overlayEnabled = !g_overlayEnabled;
        g_menu = false;
        g_reshadeInput = false;
        g_resyncing = false;
        if (!g_overlayEnabled)
        {
            ShowWindow(g_hwnd, SW_HIDE);
            HideUiLayer();
            ResetOverlayResume();
            g_overlayHiddenForForeground = true;
            FocusTarget();
            Log("[Overlay] disabled");
        }
        else
        {
            g_overlayHiddenForForeground = true;
            ResetOverlayResume();
            g_lastRect = { 0,0,0,0 };
            ApplyWindowMode();
            Log("[Overlay] enabled");
        }
    }

    if (f11 && !pF11)
    {

        ForceDepthRebuildTick("manual F11");
    }

    const bool rawF3 = (GetAsyncKeyState(g_depthPreviewKey) & 0x8000) != 0;
    if (rawF3 && !pF3 && bareKeys)
        g_depthPreview = !g_depthPreview;
    pF3 = rawF3;

    if ((GetAsyncKeyState(VK_END) & 1) && !shift && !ctrl && !alt)
        g_running = false;



    pMenu = menuCombo; pF8 = rawF8; pF1 = rawMinKey; pF11 = rawF11;
}

static std::chrono::steady_clock::time_point AdvanceFrameDeadline(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::duration interval,
    std::chrono::steady_clock::time_point now)
{
    if (interval <= std::chrono::microseconds::zero())
        interval = std::chrono::microseconds(4166);
    deadline += interval;
    if (deadline <= now)
    {

        if (now - deadline > interval * 64)
            deadline = now;
        else
            while (deadline <= now)
                deadline += interval;
    }
    return deadline;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setDpiContext = user32 ? reinterpret_cast<SetDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
    if (!setDpiContext || !setDpiContext((HANDLE)-4))
        SetProcessDPIAware();

    g_single_instance_mutex = CreateMutexW(nullptr, TRUE, L"qwleyshade_overlay_instance");
    if (!g_single_instance_mutex)
    {
        MessageBoxA(nullptr, "Cannot create overlay instance guard.", "qwleyshade", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxA(nullptr, "Overlay already running.", "qwleyshade",
            MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        CloseHandle(g_single_instance_mutex);
        g_single_instance_mutex = nullptr;
        return 0;
    }

    g_console = HasArg("/console");
    g_desktop = HasArg("/desktop");
    g_menu = HasArg("/menu");
    g_transparent = HasArg("/transparent");


    g_schedulingBoostRequested = !HasArg("/no-scheduling-boost");
    g_nvidiaBackgroundCapRequested = !HasArg("/no-nvidia-unlock");



    g_reshadeCompatibility = !HasArg("/fast-scene");
    if (HasArg("/reshade-compat")) g_reshadeCompatibility = true;
    if (HasArg("/no-reshade-compat")) g_reshadeCompatibility = false;

    g_separateScenePasses = HasArg("/separate-passes");
    for (const auto& value : ArgValues("adapter"))
    {
        char* tail = nullptr;
        const long index = strtol(value.c_str(), &tail, 10);
        if (tail && *tail == '\0' && index >= 0 && index <= 63) g_adapterIndex = (int)index;
    }
    g_disableDepthHeuristic = HasArg("/no-depth-heuristic");
    if (g_disableDepthHeuristic) g_depthHeuristicDraws = 0;
    if (HasArg("/no-auto-depth-rebuild")) g_autoDepthRebuildEnabled = false;


     
    g_vsync = !HasArg("/no-vsync");
    if (HasArg("/vsync")) g_vsync = true;
    if (HasArg("/no-depth")) g_depthPassEnabled = false;
    if (HasArg("/fast-depth-final")) g_fastDepthFinal = true;
    for (const auto& value : ArgValues("profile"))
    {
        if (value == "low") g_requestedPerfTier = PerfTier::Low;
        else if (value == "balanced" || value == "medium") g_requestedPerfTier = PerfTier::Balanced;
        else if (value == "high") g_requestedPerfTier = PerfTier::High;
        else if (value == "auto") g_requestedPerfTier = PerfTier::Auto;
        else continue;
        g_perfTierFromCmdline = true;
    }
    for (const auto& value : ArgValues("overlay-fps"))
    {
        const int fps = atoi(value.c_str());

        if (fps >= 1 && fps <= 1000)
        {
            g_maxOverlayFps = (UINT)fps;
            g_overlayFpsExplicit = true;
        }
    }

    g_highResolutionTimer = timeBeginPeriod(1) == TIMERR_NOERROR;

    InitTargetList();

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash)
    {
        *(slash + 1) = '\0';
        snprintf(g_iniPath, MAX_PATH, "%soverlay_settings.ini", exe);
        snprintf(g_reshadeIniPath, MAX_PATH, "%sReShade.ini", exe);
        snprintf(g_imguiIniPath, MAX_PATH, "%soverlay_windows.ini", exe);
    }

    LoadSettings();

    SaveSettings();

    if (g_console)
    {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    InitializeSchedulingBoost();
    Log("FeatureModule Overlay starting (menu=%s, minimizeKey=%d=%s, reversedDepth=%d)",
        MenuBindingText().c_str(), g_minimizeKey, VkKeyName(g_minimizeKey), (int)g_depthReversed);
    Log("[Target] watching: %s", g_targetExeDisplay.c_str());

    if (!InitWindow() || !InitD3D())
    {
        Log("[Init] window/device initialization failed");
        StopSchedulingBoost();
        ReleaseOverlayFontResource();
        if (g_highResolutionTimer) { timeEndPeriod(1); g_highResolutionTimer = false; }
        if (g_single_instance_mutex)
        {
            ReleaseMutex(g_single_instance_mutex);
            CloseHandle(g_single_instance_mutex); g_single_instance_mutex = nullptr;
        }
        if (g_console) FreeConsole();
        return 1;
    }

    g_frameWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_frameWakeEvent)
        Log("[Wait] frame event unavailable; message-aware timed waits remain active");
    GameSchedulingBoostTick();
    g_pipeThread = std::thread(PipeThread);
    g_pdh_thread = std::thread(PdhThread);

    bool pipeWasSeen = false;
    const auto connectionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (g_running && !g_pipeStop.load(std::memory_order_acquire) &&
        !(pipeWasSeen = g_pipeEverConnected.load(std::memory_order_acquire)) &&
        std::chrono::steady_clock::now() < connectionDeadline)
    {
        MSG pending = {};
        while (PeekMessageA(&pending, nullptr, 0, 0, PM_REMOVE))
        {
            if (pending.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&pending);
            DispatchMessageA(&pending);
        }
        if (g_running) WaitForRenderWork(std::min(connectionDeadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
    }
    if (g_running && !pipeWasSeen && !g_pipeStop.load(std::memory_order_acquire))
    {
        ShowWindow(g_hwnd, SW_HIDE);
        HideUiLayer();
        MessageBoxA(nullptr, "pls repeat the injection.", "didn't inject",
            MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        g_running = false;
    }

    MSG msg = {};
    while (g_running)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;
        UpdateForegroundVisibility();
        g_nativeUi.Tick(g_targetPid);
        Hotkeys();
        if (!g_running) break;
        static DWORD lastDeviceProbe = 0;
        const DWORD deviceTick = GetTickCount();
        if (!g_graphicsRecovering && g_dev && deviceTick - lastDeviceProbe >= 500)
        {
            lastDeviceProbe = deviceTick;
            const HRESULT reason = g_dev->GetDeviceRemovedReason();
            if (FAILED(reason)) RequestGraphicsRecovery(reason);
        }
        if (!ProcessGraphicsRecovery())
        {
            Follow();
            Sleep(8);
            continue;
        }
        PollOverlayAppearance();
        g_statsVisible.store(g_menu && g_overlayEnabled &&
            !g_overlayHiddenForForeground && !g_resyncing, std::memory_order_release);
        AutoColorDepthTick();
        Follow();
        GameSchedulingBoostTick();
        RefreshTargetPids();
        ProcessOverlayResync();
        if (g_overlayEnabled && !g_overlayHiddenForForeground && !g_resyncing)
        {
            if (g_presentOccluded)
            {
                const DWORD nowTick = GetTickCount();
                if ((LONG)(nowTick - g_nextPresentProbeTick) < 0)
                {
                    Sleep(4);
                    continue;
                }
                const HRESULT probe = g_swap->Present(0, DXGI_PRESENT_TEST);
                g_nextPresentProbeTick = nowTick + 50;
                if (probe == DXGI_ERROR_DEVICE_REMOVED || probe == DXGI_ERROR_DEVICE_RESET)
                {
                    RequestGraphicsRecovery(probe);
                    continue;
                }
                if (probe != S_OK)
                {
                    Sleep(4);
                    continue;
                }
                InvalidateOverlayFrame();
            }
            using FrameClock = std::chrono::steady_clock;
            static uint64_t lastRenderedPacket = UINT64_MAX;
            static auto nextFrameTime = FrameClock::now();
            static auto nextUiTime = FrameClock::now();

            PipePayload fp;
            uint64_t packet = 0;
            {
                 
                std::lock_guard<std::mutex> lk(g_pipeMutex);
                fp = g_payload;
                packet = g_sourcePacketSerial.load(std::memory_order_acquire);
            }
            const auto now = FrameClock::now();

            const auto interval = std::chrono::nanoseconds(
                1000000000ll / (long long)std::max<UINT>(1, g_maxOverlayFps));
            const bool menuUi = g_menu || g_reshadeInput;
            const bool wantsUi = menuUi || g_showKeyHelp || g_showFPS;

            static POINT lastCursor = {};
            static DWORD lastInteractionTick = 0;
            static bool wasInteractive = false;
            const DWORD uiTick = GetTickCount();


            const bool doCursorPoll = (g_cursorPollIntervalMs == 0) ||
                (uiTick - g_lastCursorPollMs >= g_cursorPollIntervalMs);
            if (doCursorPoll)
            {
                g_lastCursorPollMs = uiTick;
                POINT cursor = {};
                GetCursorPos(&cursor);
                const bool pointerMoved = cursor.x != lastCursor.x || cursor.y != lastCursor.y;
                const bool pointerDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                    (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
                if (pointerMoved || pointerDown) lastInteractionTick = uiTick;
                lastCursor = cursor;
            }
            const bool interactiveUi = menuUi && uiTick - lastInteractionTick < 400;
            if (interactiveUi && !wasInteractive) nextUiTime = now;
            wasInteractive = interactiveUi;

            const UINT activeUiFps = std::min(g_menuActiveFps,
                std::max<UINT>(30, g_displayRefreshHz));
            const UINT uiFps = menuUi
                ? (interactiveUi ? activeUiFps : g_menuIdleFps)
                : g_counterUiFps;
            const auto uiInterval = std::chrono::nanoseconds(
                1000000000ll / (long long)std::max<UINT>(1, uiFps));
            static bool scheduledMenu = false;
            static bool scheduledReShade = false;
            static bool scheduledShowKeyHelp = false;
            static bool scheduledShowFPS = false;
            const bool frameDue = now >= nextFrameTime;
            const bool uiNeedsFrame = wantsUi && now >= nextUiTime;
            const bool uiModeChanged = scheduledMenu != g_menu ||
                scheduledReShade != g_reshadeInput ||
                scheduledShowKeyHelp != g_showKeyHelp ||
                scheduledShowFPS != g_showFPS;
            static uint64_t renderedGeneration = 0;
            const bool graphicsChanged = renderedGeneration != g_graphicsGeneration;
            const bool sourceChanged = graphicsChanged || packet != lastRenderedPacket;

            const bool sceneDue = frameDue && (sourceChanged || g_reshadeInput);
            if (graphicsChanged || sceneDue || uiNeedsFrame || uiModeChanged)
            {
                UpdateSourceFPS();

                EnsureBackBufferMatchesWindow();
                const auto renderBegin = FrameClock::now();
                const bool presentScene = graphicsChanged || sceneDue;
                if (presentScene) Render(fp);

                if (graphicsChanged || uiNeedsFrame || uiModeChanged)
                {
                    if (g_menu || g_showKeyHelp) RenderUiLayer(fp);
                    else HideMenuLayer();

                    if (g_showFPS) RenderFpsLayer();
                    else HideFpsLayer();
                }

                if (presentScene)
                {
                    RenderDepthFinalPass(fp);

                    const bool useTearing = !g_vsync && g_tearingSupported;
                    const UINT presentFlags = useTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
                    const HRESULT presentHr = g_swap->Present(g_vsync ? 1 : 0, presentFlags);
                    if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET)
                    {
                        Log("[D3D] Present=0x%08lX reason=0x%08lX; scheduling recovery",
                            (unsigned long)presentHr, (unsigned long)g_dev->GetDeviceRemovedReason());
                        RequestGraphicsRecovery(presentHr);
                    }
                    else if (presentHr == DXGI_STATUS_OCCLUDED)
                    {
                        g_presentOccluded = true;
                        g_nextPresentProbeTick = GetTickCount() + 50;
                    }
                    else if (presentHr == S_OK)
                    {
                        if (!g_framesPresented) g_firstPresentTick = GetTickCount();
                        ++g_framesPresented;
                        lastRenderedPacket = packet;
                        renderedGeneration = g_graphicsGeneration;
                    }
                    else
                    {

                        Sleep(0);
                    }
                }
                else
                {


                }
                const double renderCostMs = std::chrono::duration<double, std::milli>(
                    FrameClock::now() - renderBegin).count();
                if (presentScene) RecordPerformanceStats(renderCostMs);
                const auto done = FrameClock::now();
                if (presentScene)

                    nextFrameTime = std::max(renderBegin + interval, done);
                if (uiNeedsFrame || uiModeChanged)
                    nextUiTime = interactiveUi
                    ? std::max(renderBegin + uiInterval, done)
                    : AdvanceFrameDeadline(nextUiTime, uiInterval, done);
                scheduledMenu = g_menu;
                scheduledReShade = g_reshadeInput;
                scheduledShowKeyHelp = g_showKeyHelp;
                scheduledShowFPS = g_showFPS;
            }
            else
            {

                auto wakeAt = frameDue ? now + std::chrono::milliseconds(20) : nextFrameTime;
                if (wantsUi && nextUiTime < wakeAt)
                    wakeAt = nextUiTime;
                if (wakeAt <= now)
                    wakeAt = now + std::chrono::milliseconds(2);
                const auto hardCap = now + std::chrono::milliseconds(menuUi ? 4 : 20);
                if (wakeAt > hardCap)
                    wakeAt = hardCap;
                 
                 
                 
                if (!(frameDue &&
                    g_sourcePacketSerial.load(std::memory_order_acquire) != packet))
                    WaitForRenderWork(wakeAt, frameDue);
            }
        }
        else
        {

            Sleep(4);
        }
    }

    g_nativeUi.Close();  
    g_pipeStop = true;
    g_pipeCv.notify_all();
    NotifyFrameWake();
    if (g_pipeThread.joinable())
    {
        CancelSynchronousIo((HANDLE)g_pipeThread.native_handle());
        g_pipeThread.join();
    }
    if (g_frameWakeEvent)
    {
        CloseHandle(g_frameWakeEvent);
        g_frameWakeEvent = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_pdhWaitMutex);
        g_pdh_stop.store(true, std::memory_order_release);
    }
    g_pdhWaitCv.notify_all();
    if (g_pdh_thread.joinable())
        g_pdh_thread.join();
    StopSchedulingBoost();
    if (g_single_instance_mutex)
    {
        ReleaseMutex(g_single_instance_mutex);
        CloseHandle(g_single_instance_mutex);
        g_single_instance_mutex = nullptr;
    }

    if (g_imguiIniPath[0])
        ImGui::SaveIniSettingsToDisk(g_imguiIniPath);
    if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    SafeRelease(g_colorSRV); SafeRelease(g_colorSampleTex); SafeRelease(g_colorTex);
    SafeRelease(g_depthSRV);  SafeRelease(g_depthTex);
    SafeRelease(g_rtv);
    SafeRelease(g_localDepthSRV);
    SafeRelease(g_localDSV);  SafeRelease(g_localDepth);
    SafeRelease(g_dsWrite); SafeRelease(g_dsOff);
    SafeRelease(g_blendColor); SafeRelease(g_blendDepthOnly); SafeRelease(g_sceneRaster);
    SafeRelease(g_sampLinear); SafeRelease(g_sampPoint);
    SafeRelease(g_uiRTV); SafeRelease(g_uiRTSRV); SafeRelease(g_uiRT);
    SafeRelease(g_uiRTV2); SafeRelease(g_uiRT2);
    ReleaseUiReadbacks();
    SafeRelease(g_uiCB);
    SafeRelease(g_psPremult);
    if (g_uiBitmap)
    {
        if (g_uiDc && g_uiOriginalBitmap) SelectObject(g_uiDc, g_uiOriginalBitmap);
        DeleteObject(g_uiBitmap);
        g_uiBitmap = nullptr;
    }
    if (g_uiDc) { DeleteDC(g_uiDc); g_uiDc = nullptr; }
    g_uiBits = nullptr;
    if (g_fpsFont) { DeleteObject(g_fpsFont); g_fpsFont = nullptr; }
    if (g_fpsBitmap)
    {
        if (g_fpsDc && g_fpsOriginalBitmap)
            SelectObject(g_fpsDc, g_fpsOriginalBitmap);
        DeleteObject(g_fpsBitmap);
        g_fpsBitmap = nullptr;
    }
    if (g_fpsDc) { DeleteDC(g_fpsDc); g_fpsDc = nullptr; }
    g_fpsBits = nullptr;
    ReleaseOverlayFontResource();
    SafeRelease(g_cbCombined);
    SafeRelease(g_cbColor);
    SafeRelease(g_cb);
    SafeRelease(g_vs);
    SafeRelease(g_psColor);   SafeRelease(g_psDepth);  SafeRelease(g_psColorDepth);
    SafeRelease(g_psDepthView);
    SafeRelease(g_adapter3);
    SafeRelease(g_backgroundSrv);
    SafeRelease(g_swap);      SafeRelease(g_ctx);       SafeRelease(g_dev);

    if (g_highResolutionTimer) timeEndPeriod(1);
    if (g_console) FreeConsole();
    return 0;
}
