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
#pragma comment(lib, "winhttp.lib")
#include "imgui.h"
#include "imgui_internal.h"
#include "lhwm-cpp-wrapper.h"
#include <tuple>
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// ═══════════════════════════════════════════════════════════════════════════
// Defines
// ═══════════════════════════════════════════════════════════════════════════
#define WM_TRAYICON   (WM_USER + 1)
#define IDM_SETTINGS  1001
#define IDM_EXIT      1002
#define IDM_SHOW      1003
#define IDM_HIDE      1004
#define IDM_UPDATE    1005
#define IDM_RESET_POS 1006

// Versão
#define APP_VERSION "v1.0"

// Janela de config: largura externa fixa (px), altura externa mínima para redimensionamento vertical
static const int kConfigDlgOuterW = 420;
static const int kConfigDlgMinOuterH = 520;

// ID do recurso do instalador PawnIO (executável embutido)
#define IDR_PAWNIO_SETUP 101

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

// Provedor Microsoft-Windows-DXGI  {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
static const GUID DXGI_PROVIDER =
{ 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };

// Provedor Microsoft-Windows-D3D9  {783ACA0A-790E-4D7F-8451-AA850511C6B9}
static const GUID D3D9_PROVIDER =
{ 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };

// Provedor Microsoft-Windows-DxgKrnl  {802EC45A-1E99-4B83-9920-87C98277BA9D}
// Captura apresentações no nível do kernel para TODAS as APIs gráficas: DX9/10/11/12, Vulkan, OpenGL
static const GUID DXGKRNL_PROVIDER =
{ 0x802EC45A, 0x1E99, 0x4B83, { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D } };

// Palavras-chave DxgKrnl para rastreamento de apresentação
static const ULONGLONG DXGKRNL_KEYWORD_PRESENT = 0x8000000;  // Palavra-chave Present
static const ULONGLONG DXGKRNL_KEYWORD_BASE = 0x1;        // Palavra-chave Base

// IDs de eventos DxgKrnl para rastreamento de apresentação
static const USHORT DXGKRNL_EVENT_PRESENT_INFO = 0x00B8;  // Present::Info (184)
static const USHORT DXGKRNL_EVENT_FLIP_INFO = 0x00A8;  // Flip::Info (168)
static const USHORT DXGKRNL_EVENT_BLIT_INFO = 0x00A6;  // Blit::Info (166)

static const char* ETW_SESSION_NAME = "FPSeco_ETW";

// ═══════════════════════════════════════════════════════════════════════════
// Configuração
// ═══════════════════════════════════════════════════════════════════════════
// Layout: 0 = pilha vertical, 1 = compacto horizontal, 2 = barra de desempenho estilo Steam
#define LAYOUT_VERTICAL   0
#define LAYOUT_HORIZONTAL 1
#define LAYOUT_STEAM      2

// Canto / borda do overlay (relativo à área de trabalho). INI legado 0..3 remapeado no carregamento via positionVer.
#define POS_TOP_LEFT       0
#define POS_TOP_CENTER     1
#define POS_TOP_RIGHT      2
#define POS_BOTTOM_LEFT    3
#define POS_BOTTOM_CENTER  4
#define POS_BOTTOM_RIGHT   5

#define FREQ_PATH_MAX   260
#define FREQ_SPARK_LEN  48

struct OverlayConfig {
    bool showFPS = true;
    bool showCpuUsage = true;
    bool showCpuTemp = true;
    bool showGpuUsage = true;
    bool showGpuTemp = true;
    bool showVRAM = true;     
    bool showRAM = true;
    bool showProcessName = true; 
    int  layoutStyle = LAYOUT_VERTICAL;
    bool useFahrenheit = false; 
    bool autoStart = false;   
    int  position = POS_TOP_LEFT; 
    int  opacity = 85;      
    int  toggleKey = VK_INSERT;
    int  exitKey = VK_END;
    float customX = -1.0f;  
    float customY = -1.0f;
    int  selectedGpu = 0;     
    int  overlayScale = 100; 
    bool showCpuFreq = false;
    bool showGpuCoreFreq = false;
    char cpuFreqPath[FREQ_PATH_MAX] = "";
    char gpuCoreFreqPath[FREQ_PATH_MAX] = "";
};

// ═══════════════════════════════════════════════════════════════════════════
// Lista de GPUs (para suporte a múltiplas GPUs via LHWM)
// ═══════════════════════════════════════════════════════════════════════════
#define MAX_GPUS 8

struct GpuInfo {
    char name[256];
    std::string tempPath;      // Caminho do sensor LHWM para temperatura
    std::string loadPath;      // Caminho do sensor LHWM para carga da GPU
    std::string vramUsedPath;  // Caminho do sensor LHWM para VRAM usada
    std::string vramTotalPath; // Caminho do sensor LHWM para VRAM total
    int         vramTotalPri = -1; // Quanto maior, melhor a correspondência (veja VramTotalSensorPriority)
    std::vector<std::pair<std::string, std::string>> coreClockOpts; // Nome de exibição, caminho
};
static GpuInfo g_gpuList[MAX_GPUS];
static int g_gpuCount = 0;

// Auxiliar para converter Celsius para Fahrenheit
inline float ToDisplayTemp(float celsius, bool useFahrenheit) {
    return useFahrenheit ? (celsius * 9.0f / 5.0f + 32.0f) : celsius;
}

// Limiares de temperatura (em Celsius) - ajuste para comparação de exibição F
inline float GetHighTempThreshold(bool useFahrenheit) { return useFahrenheit ? 185.0f : 85.0f; }
inline float GetMedTempThreshold(bool useFahrenheit) { return useFahrenheit ? 158.0f : 70.0f; }

// ═══════════════════════════════════════════════════════════════════════════
// Arquivo de configuração (INI) - salvo ao lado do overlay.exe
// ═══════════════════════════════════════════════════════════════════════════
static char g_configPath[MAX_PATH] = "";
// Escrito quando PawnIO_setup é bem-sucedido; sobrevive a reescrita/remoção do config.ini.
static char g_pawnioRebootStatePath[MAX_PATH] = "";

static void InitConfigPath()
{
    if (g_configPath[0] != '\0') return; // Já inicializado

    // Obtém o diretório onde o executável está localizado
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    // Remove o nome do executável para obter o diretório
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    // Anexa o nome do arquivo de configuração
    snprintf(g_configPath, MAX_PATH, "%sconfig.ini", exePath);
    snprintf(g_pawnioRebootStatePath, MAX_PATH, "%sfpseco-pawnio-reboot.state", exePath);
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

// Porta de reinicialização PawnIO — estado auxiliar (veja CommitPawnIORebootPending / CheckPawnIORebootGateOrExit).
static bool WritePawnIORebootPendingStateFile(const char* hex16)
{
    if (!hex16 || strlen(hex16) != 16) return false;
    InitConfigPath();
    char line[96];
    snprintf(line, sizeof(line), "FPSECO_PAWNIO_REBOOT 1 %s\n", hex16);
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH;
    HANDLE h = CreateFileA(g_pawnioRebootStatePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        flags, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
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
    HANDLE h = CreateFileA(g_pawnioRebootStatePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char buf[256] = {};
    DWORD rd = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd == 0) return false;
    buf[rd] = '\0';
    int ver = 0;
    char hexBuf[24] = {};
    if (sscanf_s(buf, "FPSECO_PAWNIO_REBOOT %d %23s", &ver, hexBuf, (unsigned)sizeof(hexBuf)) < 2 || ver != 1)
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

    // Verifica se o arquivo de configuração existe
    DWORD attrib = GetFileAttributesA(g_configPath);
    if (attrib == INVALID_FILE_ATTRIBUTES) {
        // Sem arquivo de configuração, usa padrões
        return;
    }

    // config de exibição
    cfg.showFPS = ReadIniInt("Display", "showFPS", 1) != 0;
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
    cfg.showVRAM = ReadIniInt("Display", "showVRAM", 1) != 0;
    cfg.showRAM = ReadIniInt("Display", "showRAM", 1) != 0;
    cfg.showProcessName = ReadIniInt("Display", "showProcessName", 1) != 0;

    cfg.showCpuFreq = ReadIniInt("Frequency", "showCpuFreq", 0) != 0;
    cfg.showGpuCoreFreq = ReadIniInt("Frequency", "showGpuCoreFreq", 0) != 0;
    ReadIniStr("Frequency", "cpuFreqPath", cfg.cpuFreqPath, sizeof(cfg.cpuFreqPath));
    ReadIniStr("Frequency", "gpuCoreFreqPath", cfg.gpuCoreFreqPath, sizeof(cfg.gpuCoreFreqPath));

    // Config de layout (migrar antigo horizontal=1 -> layoutStyle=1)
    {
        int ls = ReadIniInt("Layout", "layoutStyle", -1);
        if (ls < 0)
            ls = ReadIniInt("Layout", "horizontal", 0) ? LAYOUT_HORIZONTAL : LAYOUT_VERTICAL;
        if (ls < LAYOUT_VERTICAL || ls > LAYOUT_STEAM)
            ls = LAYOUT_VERTICAL;
        cfg.layoutStyle = ls;
    }
    cfg.useFahrenheit = ReadIniInt("Layout", "useFahrenheit", 0) != 0;
    cfg.autoStart = ReadIniInt("Layout", "autoStart", 0) != 0;
    {
        const int posVer = ReadIniInt("Layout", "positionVer", 0);
        int pos = ReadIniInt("Layout", "position", 0);
        if (posVer < 2) {
            // Legado: TL=0, TR=1, BL=2, BR=3  ->  nova grade POS_*
            static const int kLegacyToPos[] = { POS_TOP_LEFT, POS_TOP_RIGHT, POS_BOTTOM_LEFT, POS_BOTTOM_RIGHT };
            if (pos >= 0 && pos <= 3)
                pos = kLegacyToPos[pos];
            WriteIniInt("Layout", "positionVer", 2);
            WriteIniInt("Layout", "position", pos);
        }
        cfg.position = pos;
    }
    cfg.opacity = ReadIniInt("Layout", "opacity", 85);
    cfg.customX = ReadIniFloat("Layout", "customX", -1.0f);
    cfg.customY = ReadIniFloat("Layout", "customY", -1.0f);
    {
        int sc = ReadIniInt("Layout", "overlayScale", -1);
        if (sc < 0)
            sc = ReadIniInt("Layout", "steamBarScale", 100);
        cfg.overlayScale = sc;
    }

    // Teclas de atalho
    cfg.toggleKey = ReadIniInt("Hotkeys", "toggleKey", VK_INSERT);
    cfg.exitKey = ReadIniInt("Hotkeys", "exitKey", VK_END);

    // Seleção de GPU
    cfg.selectedGpu = ReadIniInt("GPU", "selectedGpu", 0);

    // Limita valores a intervalos válidos
    if (cfg.position < POS_TOP_LEFT || cfg.position > POS_BOTTOM_RIGHT) cfg.position = POS_TOP_LEFT;
    if (cfg.opacity < 30) cfg.opacity = 30;
    if (cfg.opacity > 100) cfg.opacity = 100;
    if (cfg.selectedGpu < 0) cfg.selectedGpu = 0;
    if (cfg.layoutStyle < LAYOUT_VERTICAL || cfg.layoutStyle > LAYOUT_STEAM)
        cfg.layoutStyle = LAYOUT_VERTICAL;
    if (cfg.overlayScale < 50) cfg.overlayScale = 50;
    if (cfg.overlayScale > 200) cfg.overlayScale = 200;
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

    // Config de exibição
    WriteIniInt("Display", "showFPS", cfg.showFPS ? 1 : 0);
    WriteIniInt("Display", "showCpuUsage", cfg.showCpuUsage ? 1 : 0);
    WriteIniInt("Display", "showCpuTemp", cfg.showCpuTemp ? 1 : 0);
    WriteIniInt("Display", "showGpuUsage", cfg.showGpuUsage ? 1 : 0);
    WriteIniInt("Display", "showGpuTemp", cfg.showGpuTemp ? 1 : 0);
    // Flags combinados legados (compilações mais antigas / INIs editados manualmente)
    WriteIniInt("Display", "showCPU", (cfg.showCpuUsage || cfg.showCpuTemp) ? 1 : 0);
    WriteIniInt("Display", "showGPU", (cfg.showGpuUsage || cfg.showGpuTemp) ? 1 : 0);
    WriteIniInt("Display", "showVRAM", cfg.showVRAM ? 1 : 0);
    WriteIniInt("Display", "showRAM", cfg.showRAM ? 1 : 0);
    WriteIniInt("Display", "showProcessName", cfg.showProcessName ? 1 : 0);

    WriteIniInt("Frequency", "showCpuFreq", cfg.showCpuFreq ? 1 : 0);
    WriteIniInt("Frequency", "showGpuCoreFreq", cfg.showGpuCoreFreq ? 1 : 0);
    WriteIniStr("Frequency", "cpuFreqPath", cfg.cpuFreqPath);
    WriteIniStr("Frequency", "gpuCoreFreqPath", cfg.gpuCoreFreqPath);

    // Config de layout
    WriteIniInt("Layout", "layoutStyle", cfg.layoutStyle);
    WriteIniInt("Layout", "horizontal", cfg.layoutStyle == LAYOUT_HORIZONTAL ? 1 : 0);
    WriteIniInt("Layout", "useFahrenheit", cfg.useFahrenheit ? 1 : 0);
    WriteIniInt("Layout", "autoStart", cfg.autoStart ? 1 : 0);
    WriteIniInt("Layout", "position", cfg.position);
    WriteIniInt("Layout", "positionVer", 2);
    WriteIniInt("Layout", "opacity", cfg.opacity);
    WriteIniFloat("Layout", "customX", cfg.customX);
    WriteIniFloat("Layout", "customY", cfg.customY);
    WriteIniInt("Layout", "overlayScale", cfg.overlayScale);

    // Teclas de atalho
    WriteIniInt("Hotkeys", "toggleKey", cfg.toggleKey);
    WriteIniInt("Hotkeys", "exitKey", cfg.exitKey);

    // Seleção de GPU
    WriteIniInt("GPU", "selectedGpu", cfg.selectedGpu);

    if (pawnioRb != 0) {
        WriteIniInt("App", "PawnIORequiresReboot", 1);
        if (pawnioHex[0] != '\0')
            WriteIniStr("App", "PawnIOInstallUtcHex", pawnioHex);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Estado da aplicação
// ═══════════════════════════════════════════════════════════════════════════
enum AppMode { MODE_CONFIG, MODE_OVERLAY };
enum PendingCmd { CMD_NONE, CMD_START_OVERLAY, CMD_SHOW_SETTINGS, CMD_EXIT };

static OverlayConfig g_Config;
static AppMode       g_Mode = MODE_CONFIG;
static PendingCmd    g_Pending = CMD_NONE;
static bool          g_Running = true;
static bool          g_OvlVisible = true;

static HINSTANCE      g_hInstance = nullptr;
static HWND           g_hwnd = nullptr;
static NOTIFYICONDATA g_nid = {};
static RECT           g_overlayBounds = { 0, 0, 0, 0 };  // Área do overlay ImGui para teste de hit
static bool           g_isDragging = false;            // Verdadeiro quando o usuário está arrastando o overlay
static bool           g_overlayForceCornerSnap = false; // Disparo único: ajustar ao canto predefinido (após Redefinir Posição)

// ── Informações de hardware ──
static char g_cpuName[256] = "Desconhecido";
static char g_gpuName[256] = "Desconhecido";

// ── Estado do verificador de atualizações ──
static std::atomic<bool> g_updateAvailable{ false };
static std::atomic<bool> g_updateCheckDone{ false };
static char g_latestVersion[32] = "";

// ═══════════════════════════════════════════════════════════════════════════
// Verificador de atualizações (consulta a API de releases do GitHub)
// ═══════════════════════════════════════════════════════════════════════════
static void CheckForUpdatesAsync()
{
    std::thread([]() {
        HINTERNET hSession = WinHttpOpen(L"FPS-Overlay/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { g_updateCheckDone = true; return; }

        HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com",
            INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            g_updateCheckDone = true;
            return;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
            L"/repos/zRafaX/FPSeco/releases/latest",
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            g_updateCheckDone = true;
            return;
        }

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            g_updateCheckDone = true;
            return;
        }

        // Lê a resposta
        std::string response;
        DWORD bytesRead = 0;
        char buffer[4096];
        while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        // Analisa "tag_name" da resposta JSON (pesquisa simples de string)
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

                    // Compara versões (comparação simples de string)
                    if (strcmp(g_latestVersion, APP_VERSION) != 0) {
                        g_updateAvailable = true;
                    }
                }
            }
        }
        g_updateCheckDone = true;
        }).detach();
}

// ── Estatísticas da GPU (do LHWM) ──
static float g_gpuUsage = 0.0f;
static float g_gpuTemp = 0.0f;
static float g_vramUsed = 0.0f;  // em GB
static float g_vramTotal = 0.0f;  // em GB

// ── Estado ETW ──
static TRACEHANDLE      g_etwSession = 0;
static TRACEHANDLE      g_etwTrace = 0;
static std::thread      g_etwThread;
static std::atomic<bool>  g_etwRunning{ false };
static std::atomic<float> g_gameFps{ 0.0f };
static std::atomic<DWORD> g_targetPid{ 0 };
static DWORD              g_lastTargetPid = 0;    // Para detectar alteração de PID
static bool               g_etwAvailable = false;
static bool               g_isAdmin = false;      // Executando como administrador?
static double              g_qpcFreq = 1.0;
static char               g_targetProcessName[768] = "";  // UTF-8: processo rastreado (exe + descrição)

// ── Temperatura da CPU (WMI) ──
static float g_cpuTemp = 0.0f;
static bool  g_cpuTempAvailable = false;

// ── Estado do LibreHardwareMonitor (LHWM) ──
static bool  g_lhwmAvailable = false;
static std::string g_lhwmCpuTempPath;      // ex.: "/amdcpu/0/temperature/3"
static std::string g_lhwmGpuTempPath;      // ex.: "/gpu-nvidia/0/temperature/0"
static std::string g_lhwmGpuLoadPath;      // ex.: "/gpu-nvidia/0/load/0"
static std::string g_lhwmGpuVramUsedPath;  // VRAM usada
static std::string g_lhwmGpuVramTotalPath; // VRAM total
static float g_lhwmCpuTemp = 0.0f;         // Temperatura da CPU do LHWM (usada diretamente)

// Opções de clock do núcleo da CPU / GPU e valores ativos (MHz) + histórico sparkline
static std::vector<std::pair<std::string, std::string>> g_cpuClockOpts;
static float g_cpuClockMHz = 0.f;
static float g_gpuCoreClockMHz = 0.f;
static float g_cpuSpark[FREQ_SPARK_LEN];
static int   g_cpuSparkN = 0;
static float g_gpuSpark[FREQ_SPARK_LEN];
static int   g_gpuSparkN = 0;

static bool FreqPathValid(const char* p, const std::vector<std::pair<std::string, std::string>>& opts)
{
    if (!p || !p[0]) return false;
    for (const auto& e : opts)
        if (e.second == p) return true;
    return false;
}

static void ValidateFrequencyPaths()
{
    if (!FreqPathValid(g_Config.cpuFreqPath, g_cpuClockOpts))
        g_Config.cpuFreqPath[0] = '\0';
    if (g_gpuCount > 0 && g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount) {
        GpuInfo& g = g_gpuList[g_Config.selectedGpu];
        if (!FreqPathValid(g_Config.gpuCoreFreqPath, g.coreClockOpts))
            g_Config.gpuCoreFreqPath[0] = '\0';
    }
    else {
        g_Config.gpuCoreFreqPath[0] = '\0';
    }
}

static void SparkPush(float* buf, int& n, int cap, float v)
{
    if (cap <= 0) return;
    if (n < cap)
        buf[n++] = v;
    else {
        memmove(buf, buf + 1, (size_t)(cap - 1) * sizeof(float));
        buf[cap - 1] = v;
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

        SparkPush(g_cpuSpark, g_cpuSparkN, FREQ_SPARK_LEN, g_cpuClockMHz > 0.f ? g_cpuClockMHz : 0.f);
        SparkPush(g_gpuSpark, g_gpuSparkN, FREQ_SPARK_LEN, g_gpuCoreClockMHz > 0.f ? g_gpuCoreClockMHz : 0.f);
    }
    catch (...) {
    }
}

static void DrawMiniSpark(const char* id, const float* hist, int n, float mhz, ImVec2 sz)
{
    if (n >= 2) {
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.33f, 0.82f, 0.52f, 1.f));
        ImGui::PlotLines(id, hist, n, 0, nullptr, FLT_MAX, FLT_MAX, sz);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (mhz > 0.f)
        ImGui::TextColored(ImVec4(.72f, .72f, .76f, 1), "%.0f MHz", mhz);
    else
        ImGui::TextColored(ImVec4(.45f, .45f, .50f, 1), "--- MHz");
}

// Spark inline + MHz para linhas horizontais / Steam: ImGui SameLine alinha widgets ao topo, então
// centralizamos verticalmente o PlotLines emoldurado e o bloco de texto um contra o outro.
static void InlineFreqSparkMHz(const char* plotId, const float* hist, int n, float mhz,
    ImVec2 plotInnerPx, float gapAfterPlot, const ImVec4& txtCol)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 2.f));
    const ImGuiStyle& st = ImGui::GetStyle();
    const float plotFrameH = plotInnerPx.y + st.FramePadding.y * 2.f;
    const float textH = ImGui::GetTextLineHeight();
    const float rowH = plotFrameH > textH ? plotFrameH : textH;
    const float rowTop = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(rowTop + (rowH - plotFrameH) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.33f, 0.82f, 0.52f, 1.f));
    ImGui::PlotLines(plotId, hist, n, 0, nullptr, FLT_MAX, FLT_MAX, plotInnerPx);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::SameLine(0, gapAfterPlot);
    ImGui::SetCursorPosY(rowTop + (rowH - textH) * 0.5f);
    ImGui::TextColored(txtCol, "%.0f MHz", mhz);
}

// ── DX11 ──
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_pRenderTargetView = nullptr;

// ── Estado do ouvinte de teclas de atalho ──
static int  g_listeningFor = 0;   // 0=nenhum, 1=alternância, 2=sair

// ═══════════════════════════════════════════════════════════════════════════
// Declarações antecipadas
// ═══════════════════════════════════════════════════════════════════════════
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
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND, UINT, WPARAM, LPARAM);

// ═══════════════════════════════════════════════════════════════════════════
// Utilidade: nome da tecla a partir do código VK
// ═══════════════════════════════════════════════════════════════════════════
static const char* GetKeyName(int vk)
{
    static char buf[64];
    UINT sc = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    LONG lp = sc << 16;

    // Flag de tecla estendida para teclas de navegação
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:
    case VK_LEFT:   case VK_RIGHT:  case VK_UP:   case VK_DOWN:
    case VK_NUMLOCK: case VK_SNAPSHOT: case VK_CANCEL:
        lp |= (1 << 24);
        break;
    }

    if (GetKeyNameTextA(lp, buf, sizeof(buf)) > 0)
        return buf;
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

// ═══════════════════════════════════════════════════════════════════════════
// Verificação de administrador
// ═══════════════════════════════════════════════════════════════════════════
static bool IsRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

// ═══════════════════════════════════════════════════════════════════════════
// Detecção de hardware
// ═══════════════════════════════════════════════════════════════════════════
static void QueryCpuName()
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD sz = sizeof(g_cpuName);
        RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
            reinterpret_cast<LPBYTE>(g_cpuName), &sz);
        RegCloseKey(hKey);

        // Remove espaços iniciais
        char* p = g_cpuName;
        while (*p == ' ') p++;
        if (p != g_cpuName) memmove(g_cpuName, p, strlen(p) + 1);
    }
}

static void QueryGpuName()
{
    if (!g_pd3dDevice) return;

    IDXGIDevice* dxgiDev = nullptr;
    g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice),
        reinterpret_cast<void**>(&dxgiDev));
    if (!dxgiDev) return;

    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
            g_gpuName, sizeof(g_gpuName), nullptr, nullptr);
        adapter->Release();
    }
    dxgiDev->Release();
}

// ═══════════════════════════════════════════════════════════════════════════
// Nome e descrição do processo a partir do PID (UTF-8 para ImGui / caminhos Unicode)
// ═══════════════════════════════════════════════════════════════════════════
static void WideToUtf8(const wchar_t* w, char* out, size_t outBytes)
{
    if (!out || outBytes == 0) return;
    out[0] = '\0';
    if (!w || !w[0]) return;
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, out, (int)outBytes, nullptr, nullptr);
    if (n <= 0)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outBytes, nullptr, nullptr);
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

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate = nullptr;
    UINT cbTranslate = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
        reinterpret_cast<LPVOID*>(&lpTranslate), &cbTranslate) ||
        !lpTranslate || cbTranslate < sizeof(LANGANDCODEPAGE))
        return;

    wchar_t subBlock[72];
    _snwprintf_s(subBlock, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\FileDescription",
        lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);

    wchar_t* description = nullptr;
    UINT descLen = 0;
    if (!VerQueryValueW(data.data(), subBlock, reinterpret_cast<LPVOID*>(&description), &descLen) ||
        !description || !description[0])
        return;

    WideToUtf8(description, outDesc, maxLen);
}

static void GetProcessName(DWORD pid, char* outName, size_t maxLen)
{
    outName[0] = '\0';
    if (pid == 0 || maxLen == 0) return;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc)
        hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
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

        if (descUtf8[0])
            snprintf(outName, maxLen, "%s (%s)", exeUtf8, descUtf8);
        else
            snprintf(outName, maxLen, "%s", exeUtf8);
    }
    CloseHandle(hProc);
}

// ═══════════════════════════════════════════════════════════════════════════
// Temperatura da CPU via WMI (funciona em alguns sistemas)
// ═══════════════════════════════════════════════════════════════════════════
static IWbemLocator* g_pWbemLocator = nullptr;
static IWbemServices* g_pWbemServices = nullptr;
static bool            g_wmiInitialized = false;

static bool InitWMI()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);
    // Ignora se já inicializado

    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, reinterpret_cast<void**>(&g_pWbemLocator));
    if (FAILED(hr)) return false;

    // Tenta o namespace WMI do OpenHardwareMonitor primeiro (mais confiável)
    hr = g_pWbemLocator->ConnectServer(
        _bstr_t(L"ROOT\\OpenHardwareMonitor"), nullptr, nullptr, nullptr,
        0, nullptr, nullptr, &g_pWbemServices);

    if (FAILED(hr)) {
        // Tenta o namespace WMI padrão (funciona em alguns sistemas)
        hr = g_pWbemLocator->ConnectServer(
            _bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
            0, nullptr, nullptr, &g_pWbemServices);
    }

    if (FAILED(hr)) {
        g_pWbemLocator->Release();
        g_pWbemLocator = nullptr;
        return false;
    }

    hr = CoSetProxyBlanket(g_pWbemServices, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
        nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE);

    g_wmiInitialized = true;
    return true;
}

static void ShutdownWMI()
{
    if (g_pWbemServices) { g_pWbemServices->Release(); g_pWbemServices = nullptr; }
    if (g_pWbemLocator) { g_pWbemLocator->Release();  g_pWbemLocator = nullptr; }
    g_wmiInitialized = false;
}

static float QueryCpuTemperature()
{
    if (!g_wmiInitialized || !g_pWbemServices) return 0.0f;

    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hr;

    // Tenta consulta de sensor do OpenHardwareMonitor
    hr = g_pWbemServices->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT Value FROM Sensor WHERE SensorType='Temperature' AND Name LIKE '%CPU%'"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &pEnumerator);

    if (FAILED(hr)) {
        // Tenta MSAcpi_ThermalZoneTemperature (embutido, mas menos confiável)
        hr = g_pWbemServices->ExecQuery(
            _bstr_t(L"WQL"),
            _bstr_t(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &pEnumerator);
    }

    if (FAILED(hr)) return 0.0f;

    float temp = 0.0f;
    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;

    if (pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && returned > 0) {
        VARIANT vtProp;
        VariantInit(&vtProp);

        // Tenta "Value" primeiro (OpenHardwareMonitor)
        hr = pObj->Get(L"Value", 0, &vtProp, nullptr, nullptr);
        if (SUCCEEDED(hr) && vtProp.vt == VT_R4) {
            temp = vtProp.fltVal;
        }
        else {
            // Tenta "CurrentTemperature" (MSAcpi - retorna em décimos de Kelvin)
            VariantClear(&vtProp);
            hr = pObj->Get(L"CurrentTemperature", 0, &vtProp, nullptr, nullptr);
            if (SUCCEEDED(hr) && (vtProp.vt == VT_I4 || vtProp.vt == VT_UI4)) {
                // Converte de décimos de Kelvin para Celsius
                temp = (vtProp.lVal / 10.0f) - 273.15f;
            }
        }
        VariantClear(&vtProp);
        pObj->Release();
    }

    pEnumerator->Release();
    return temp;
}

// ═══════════════════════════════════════════════════════════════════════════
// Instalação do driver PawnIO — necessário para o LibreHardwareMonitor
// ═══════════════════════════════════════════════════════════════════════════

static HKEY OpenPawnIOUninstallKeyRead()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
        return hKey;
    if (RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
        0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
        return hKey;
    return nullptr;
}

static bool RegQuerySzA(HKEY hKey, const char* valueName, char* buf, DWORD cap)
{
    if (!cap) return false;
    DWORD sz = cap;
    DWORD typ = 0;
    if (RegQueryValueExA(hKey, valueName, nullptr, &typ,
        reinterpret_cast<LPBYTE>(buf), &sz) != ERROR_SUCCESS)
        return false;
    if (typ != REG_SZ && typ != REG_EXPAND_SZ)
        return false;
    buf[cap - 1] = '\0';
    return buf[0] != '\0';
}

// Verifica se o driver PawnIO está instalado via registro
// LibreHardwareMonitor verifica: SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO
static bool IsPawnIOInstalled()
{
    HKEY hKey = OpenPawnIOUninstallKeyRead();
    if (!hKey)
        return false;
    char versionStr[64] = { 0 };
    bool ok = RegQuerySzA(hKey, "DisplayVersion", versionStr, sizeof(versionStr));
    RegCloseKey(hKey);
    return ok;
}

static bool GetFileVersionQuad(const char* path, DWORD* verMS, DWORD* verLS)
{
    if (!path || !path[0] || !verMS || !verLS)
        return false;
    DWORD dummy = 0;
    DWORD verSize = GetFileVersionInfoSizeA(path, &dummy);
    if (!verSize)
        return false;
    std::vector<BYTE> data(verSize);
    if (!GetFileVersionInfoA(path, 0, verSize, data.data()))
        return false;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<void**>(&ffi), &ffiLen) || !ffi ||
        ffiLen < sizeof(VS_FIXEDFILEINFO))
        return false;
    *verMS = ffi->dwFileVersionMS;
    *verLS = ffi->dwFileVersionLS;
    return (*verMS | *verLS) != 0;
}

// Grava os bytes do PawnIO_setup.exe embutido em um caminho absoluto (sobrescreve).
static bool WriteEmbeddedPawnIOSetupToPath(const char* destPath)
{
    HMODULE hModule = GetModuleHandle(nullptr);
    HRSRC hResource = FindResource(hModule, MAKEINTRESOURCE(IDR_PAWNIO_SETUP), RT_RCDATA);
    if (!hResource)
        return false;
    HGLOBAL hLoadedResource = LoadResource(hModule, hResource);
    if (!hLoadedResource)
        return false;
    LPVOID pResourceData = LockResource(hLoadedResource);
    DWORD dwResourceSize = SizeofResource(hModule, hResource);
    if (!pResourceData || dwResourceSize == 0)
        return false;
    HANDLE hFile = CreateFileA(destPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;
    DWORD bytesWritten = 0;
    BOOL writeResult = WriteFile(hFile, pResourceData, dwResourceSize, &bytesWritten, nullptr);
    CloseHandle(hFile);
    if (!writeResult || bytesWritten != dwResourceSize) {
        DeleteFileA(destPath);
        return false;
    }
    return true;
}

static bool GetBundledPawnIOSetupVersion(DWORD* verMS, DWORD* verLS)
{
    char tempPath[MAX_PATH];
    char tempFile[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) == 0)
        return false;
    snprintf(tempFile, MAX_PATH, "%sFPSeco_PawnIO_setup_%llu.exe", tempPath,
        (unsigned long long)GetTickCount64());
    if (!WriteEmbeddedPawnIOSetupToPath(tempFile))
        return false;
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
        if (q) {
            size_t n = (size_t)(q - p);
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = '\0';
        }
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
    if (*raw == '"') {
        StripPathFromDisplayIcon(raw, out, cap);
        return;
    }
    const char* sp = raw;
    const char* end = raw;
    while (*end && *end != ' ' && *end != '\t') ++end;
    size_t n = (size_t)(end - sp);
    if (n >= cap) n = cap - 1;
    memcpy(out, sp, n);
    out[n] = '\0';
}

static bool PathFileExistsA_(const char* p)
{
    return p && p[0] && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

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
    int v[4] = { 0, 0, 0, 0 };
    int n = 0;
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
    WORD maj = (WORD)(n >= 1 ? v[0] : 0);
    WORD minr = (WORD)(n >= 2 ? v[1] : 0);
    WORD pat = (WORD)(n >= 3 ? v[2] : 0);
    WORD bld = (WORD)(n >= 4 ? v[3] : 0);
    *verMS = ((DWORD)maj << 16) | (DWORD)minr;
    *verLS = ((DWORD)pat << 16) | (DWORD)bld;
    return true;
}

// Versão instalada: prefere a versão do arquivo a partir de caminhos relacionados à desinstalação, senão string DisplayVersion.
static bool GetInstalledPawnIOVersionQuad(DWORD* verMS, DWORD* verLS)
{
    if (!verMS || !verLS) return false;
    *verMS = *verLS = 0;
    HKEY hKey = OpenPawnIOUninstallKeyRead();
    if (!hKey)
        return false;

    char buf512[512];
    char path[MAX_PATH];

    auto tryPath = [&](const char* candidate) -> bool {
        if (!candidate || !candidate[0]) return false;
        if (!PathFileExistsA_(candidate)) return false;
        return GetFileVersionQuad(candidate, verMS, verLS);
        };

    bool got = false;
    if (RegQuerySzA(hKey, "DisplayIcon", buf512, sizeof(buf512))) {
        StripPathFromDisplayIcon(buf512, path, sizeof(path));
        got = tryPath(path);
    }
    if (!got) {
        DWORD typ = 0;
        DWORD sz = sizeof(buf512);
        if (RegQueryValueExA(hKey, "InstallLocation", nullptr, &typ,
            reinterpret_cast<LPBYTE>(buf512), &sz) == ERROR_SUCCESS &&
            (typ == REG_SZ || typ == REG_EXPAND_SZ) && buf512[0]) {
            buf512[sizeof(buf512) - 1] = '\0';
            if (typ == REG_EXPAND_SZ) {
                char expanded[512];
                if (ExpandEnvironmentStringsA(buf512, expanded, sizeof(expanded)) > 1)
                    snprintf(buf512, sizeof(buf512), "%s", expanded);
            }
            if (TryJoinExe(path, sizeof(path), buf512, "PawnIO.exe") && tryPath(path)) got = true;
            if (!got && TryJoinExe(path, sizeof(path), buf512, "PawnIO_setup.exe") && tryPath(path)) got = true;
        }
    }
    if (!got && RegQuerySzA(hKey, "UninstallString", buf512, sizeof(buf512))) {
        ExtractFirstQuotedPath(buf512, path, sizeof(path));
        if (!got && tryPath(path)) got = true;
    }
    if (!got)
        got = tryPath("C:\\Windows\\System32\\drivers\\PawnIO.sys");

    char disp[64] = { 0 };
    if (!got && RegQuerySzA(hKey, "DisplayVersion", disp, sizeof(disp)))
        got = ParseDisplayVersionToQuad(disp, verMS, verLS);

    RegCloseKey(hKey);
    return got && (*verMS != 0 || *verLS != 0);
}

static int CompareFileVersionQuad(DWORD aMS, DWORD aLS, DWORD bMS, DWORD bLS)
{
    if (aMS != bMS) return (aMS > bMS) ? 1 : -1;
    if (aLS != bLS) return (aLS > bLS) ? 1 : -1;
    return 0;
}

// Win32_OperatingSystem.LastBootUpTime como data e hora CIM -> FILETIME (UTC).
// O deslocamento +/-minutos é o deslocamento UTC do WMI; ignorá-lo fez a última inicialização parecer "mais recente" que o
// marcador de instalação e limpava PawnIORequiresReboot incorretamente em alguns locais.
static bool CimDateTimeStringToFileTimeUtc(const wchar_t* wsz, FILETIME* pft)
{
    if (!wsz || !pft) return false;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (swscanf_s(wsz, L"%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6)
        return false;

    int biasMin = 0;
    const wchar_t* dot = wcschr(wsz, L'.');
    if (dot) {
        const wchar_t* p = dot + 1;
        while (*p && iswdigit(*p)) ++p;
        if (*p == L'+' || *p == L'-')
            biasMin = _wtoi(p);
    }
    else {
        const wchar_t* q = wsz + 14;
        while (*q && *q != L'+' && *q != L'-') ++q;
        if (*q == L'+' || *q == L'-')
            biasMin = _wtoi(q);
    }

    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
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
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
        return false;

    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IEnumWbemClassObject* pEnum = nullptr;
    IWbemClassObject* pObj = nullptr;
    VARIANT vt;
    VariantInit(&vt);
    ULONG ret = 0;
    bool ok = false;

    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
        reinterpret_cast<void**>(&pLoc));
    if (FAILED(hr) || !pLoc) goto wmi_cleanup;

    hr = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &pSvc);
    if (FAILED(hr) || !pSvc) goto wmi_cleanup;

    hr = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) goto wmi_cleanup;

    hr = pSvc->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT LastBootUpTime FROM Win32_OperatingSystem"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    if (FAILED(hr) || !pEnum) goto wmi_cleanup;

    if (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) != S_OK || !pObj) goto wmi_cleanup;

    hr = pObj->Get(L"LastBootUpTime", 0, &vt, nullptr, nullptr);
    if (FAILED(hr)) goto wmi_cleanup;
    if (vt.vt == VT_BSTR && vt.bstrVal)
        ok = CimDateTimeStringToFileTimeUtc(vt.bstrVal, pft);

wmi_cleanup:
    VariantClear(&vt);
    if (pObj) pObj->Release();
    if (pEnum) pEnum->Release();
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    if (comNeedsUninit) CoUninitialize();
    return ok;
}

static bool CommitPawnIORebootPendingToIni()
{
    InitConfigPath();
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    char hex[20];
    snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)u.QuadPart);

    for (int attempt = 0; attempt < 12; attempt++) {
        if (!WritePawnIORebootPendingStateFile(hex)) {
            Sleep(30);
            continue;
        }
        WriteIniStr("App", "PawnIOInstallUtcHex", hex);
        WriteIniInt("App", "PawnIORequiresReboot", 1);

        char verifyFile[48] = {};
        const bool fileOk =
            ReadPawnIORebootPendingStateFile(verifyFile, sizeof(verifyFile)) && (_stricmp(verifyFile, hex) == 0);
        if (!fileOk) {
            Sleep(30);
            continue;
        }

        if (ReadIniInt("App", "PawnIORequiresReboot", 0) == 0) {
            WriteIniStr("App", "PawnIOInstallUtcHex", hex);
            WriteIniInt("App", "PawnIORequiresReboot", 1);
        }
        char verify[40] = {};
        ReadIniStr("App", "PawnIOInstallUtcHex", verify, sizeof(verify));
        if (_stricmp(verify, hex) != 0) {
            WriteIniStr("App", "PawnIOInstallUtcHex", hex);
            WriteIniInt("App", "PawnIORequiresReboot", 1);
            ReadIniStr("App", "PawnIOInstallUtcHex", verify, sizeof(verify));
        }
        if (ReadIniInt("App", "PawnIORequiresReboot", 0) != 0 && _stricmp(verify, hex) == 0)
            return true;

        // O arquivo auxiliar é autoritativo; o INI pode ser instável com algumas ferramentas.
        if (fileOk)
            return true;

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

// Aproximação da última inicialização no mesmo domínio UTC FILETIME que GetSystemTimeAsFileTime (marcador de instalação).
// Evita incompatibilidades de fuso horário WMI/CIM que mantinham a porta de reinicialização travada após um reinício real.
static bool ApproxLastBootFromUptimeFileTime(FILETIME* pft)
{
    if (!pft) return false;
    FILETIME nowFt = {};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    const ULONGLONG uptime100ns = GetTickCount64() * 10000ULL;
    if (uptime100ns == 0 || now.QuadPart <= uptime100ns)
        return false;
    ULARGE_INTEGER boot;
    boot.QuadPart = now.QuadPart - uptime100ns;
    pft->dwLowDateTime = boot.LowPart;
    pft->dwHighDateTime = boot.HighPart;
    return true;
}

// bootFt e markerFt devem ser comparáveis (mesma época). minSlack100ns evita empates limítrofes.
static bool LastBootUtcPlausiblyAfterPawnIOInstall(const FILETIME* bootFt, const FILETIME* markerFt,
    ULONGLONG minSlack100ns)
{
    ULARGE_INTEGER b, m;
    b.LowPart = bootFt->dwLowDateTime;
    b.HighPart = bootFt->dwHighDateTime;
    m.LowPart = markerFt->dwLowDateTime;
    m.HighPart = markerFt->dwHighDateTime;
    if (b.QuadPart <= m.QuadPart)
        return false;
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
    pft->dwLowDateTime = u.LowPart;
    pft->dwHighDateTime = u.HighPart;
    return true;
}

static bool AcquireShutdownPrivilege()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    TOKEN_PRIVILEGES tkp = {};
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(nullptr, "SeShutdownPrivilege", &tkp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }
    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, nullptr);
    CloseHandle(hToken);
    return ok != FALSE;
}

// Mostra aviso de reinicio obrigatorio.
// A janela sempre aparece na frente das outras.
static void ForceShowPawnIORestartRequiredDialogThenExit(const wchar_t* situationLead)
{
    const UINT kMb =
        MB_YESNO |
        MB_ICONWARNING |
        MB_DEFBUTTON2 |
        MB_TOPMOST |
        MB_SETFOREGROUND |
        MB_SYSTEMMODAL;

    wchar_t body[2048];

    _snwprintf_s(
        body,
        _TRUNCATE,

        L"%s\n\n"

        L"Importante:\n"
        L"Salve seu trabalho antes de reiniciar.\n"
        L"Arquivos nao salvos podem ser perdidos.\n\n"

        L"Um reinicio completo do Windows e necessario "
        L"antes de abrir o FPSeco.\n\n"

        L"SIM  - Reiniciar agora\n"
        L"NAO  - Reiniciar depois\n\n"

        L"O FPSeco sera fechado.",

        situationLead ? situationLead : L""
    );

    const int r = MessageBoxW(
        nullptr,
        body,
        L"FPSeco - Reinicio necessario",
        kMb
    );

    // Usuario escolheu reiniciar agora
    if (r == IDYES) {

        if (AcquireShutdownPrivilege()) {

            ExitWindowsEx(
                EWX_REBOOT | EWX_FORCEIFHUNG,

                SHTDN_REASON_MAJOR_APPLICATION |
                SHTDN_REASON_MINOR_INSTALLATION |
                SHTDN_REASON_FLAG_PLANNED
            );
        }

        // Falha ao reiniciar automaticamente
        MessageBoxW(
            nullptr,

            L"Nao foi possivel iniciar o reinicio automatico.\n\n"

            L"Reinicie manualmente:\n\n"

            L"Iniciar\n"
            L"-> Energia\n"
            L"-> Reiniciar\n\n"

            L"Depois abra o FPSeco novamente.",

            L"FPSeco",

            MB_OK |
            MB_ICONINFORMATION |
            MB_TOPMOST |
            MB_SETFOREGROUND |
            MB_SYSTEMMODAL
        );
    }

    ExitProcess(0);
}

// Bloqueia a inicializacao ate que o Windows seja reiniciado
// apos a ultima instalacao ou atualizacao do PawnIO.
static void CheckPawnIORebootGateOrExit()
{
    InitConfigPath();

    char hex[48] = {};
    ReadIniStr("App", "PawnIOInstallUtcHex", hex, sizeof(hex));

    char fileHex[48] = {};
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

    if (rb == 0)
        return;

    FILETIME marker = {};

    if (!ReadPawnIOInstallMarkerFileTime(&marker)) {
        ForceShowPawnIORestartRequiredDialogThenExit(
            L"O Overlay esta aguardando um reinicio do sistema apos a instalacao ou atualizacao do PawnIO.\n\n"
            L"Porem o marcador de reinicio no config.ini esta ausente ou invalido.\n\n"
            L"Se isso continuar apos reiniciar o Windows:\n\n"
            L"1. Remova PawnIORequiresReboot\n"
            L"2. Remova PawnIOInstallUtcHex\n"
            L"3. Apague fpseco-pawnio-reboot.state\n\n"
            L"Esses arquivos ficam ao lado do overlay.exe.");

        return;
    }

    FILETIME bootApprox = {};
    const bool haveBootApprox =
        ApproxLastBootFromUptimeFileTime(&bootApprox);

    if (haveBootApprox &&
        LastBootUtcPlausiblyAfterPawnIOInstall(
            &bootApprox,
            &marker,
            3ULL * 10000000ULL))
    {
        ClearPawnIORebootPendingAll();
        return;
    }

    FILETIME bootWmi = {};
    const bool haveBootWmi =
        WmiQueryLastBootUtcFileTime(&bootWmi);

    if (haveBootWmi &&
        LastBootUtcPlausiblyAfterPawnIOInstall(
            &bootWmi,
            &marker,
            45ULL * 10000000ULL))
    {
        ClearPawnIORebootPendingAll();
        return;
    }

    if (!haveBootApprox && !haveBootWmi) {
        ForceShowPawnIORestartRequiredDialogThenExit(
            L"O Overlay nao conseguiu verificar se o Windows foi reiniciado "
            L"apos a instalacao ou atualizacao do PawnIO.\n\n"
            L"Um reinicio completo do sistema ainda e necessario.");

        return;
    }

    ForceShowPawnIORestartRequiredDialogThenExit(
        L"Voce precisa reiniciar o Windows antes de usar o Overlay.\n\n"
        L"O PawnIO foi instalado ou atualizado anteriormente "
        L"e esta sessao ainda nao concluiu um reinicio completo.");
}

// Chamado apos o PawnIO_setup finalizar com sucesso.
static void RequireSystemRestartAfterPawnIOSetup()
{
    ForceShowPawnIORestartRequiredDialogThenExit(
        L"O PawnIO foi instalado ou atualizado com sucesso.");
}

// Extrai o PawnIO_setup.exe embutido e executa (-install). Sucesso apenas se o processo sair com código 0.
static bool ExtractAndRunPawnIOSetup()
{
    char tempPath[MAX_PATH];
    char tempFile[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) == 0)
        return false;
    snprintf(tempFile, MAX_PATH, "%sFPSeco_PawnIO_setup_run_%llu.exe", tempPath,
        (unsigned long long)GetTickCount64());
    if (!WriteEmbeddedPawnIOSetupToPath(tempFile))
        return false;

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas";
    sei.lpFile = tempFile;
    sei.lpParameters = "-install";
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExA(&sei)) {
        DeleteFileA(tempFile);
        return false;
    }

    if (!sei.hProcess) {
        DeleteFileA(tempFile);
        return false;
    }

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = (DWORD)-1;
    if (!GetExitCodeProcess(sei.hProcess, &exitCode)) {
        CloseHandle(sei.hProcess);
        DeleteFileA(tempFile);
        return false;
    }
    CloseHandle(sei.hProcess);
    DeleteFileA(tempFile);

    if (exitCode == STILL_ACTIVE)
        return false;
    return exitCode == 0;
}

// Verdadeiro se o PawnIO_setup.exe empacotado for mais recente que a compilação instalada (pular se não for possível ler a versão empacotada).
static bool IsPawnIOOutdatedVsBundled()
{
    DWORD bundledMS = 0, bundledLS = 0;
    if (!GetBundledPawnIOSetupVersion(&bundledMS, &bundledLS))
        return false;
    DWORD installedMS = 0, installedLS = 0;
    if (!GetInstalledPawnIOVersionQuad(&installedMS, &installedLS))
        return false;
    return CompareFileVersionQuad(installedMS, installedLS, bundledMS, bundledLS) < 0;
}

// Bloqueia ate que o PawnIO esteja instalado e atualizado,
// Fecha o aplicativo caso o PawnIO nao esteja instalado
// ou esteja desatualizado.
static void EnforcePawnIOOrExit()
{
    CheckPawnIORebootGateOrExit();

    for (;;) {

        // PawnIO nao instalado
        if (!IsPawnIOInstalled()) {

            int r = MessageBoxW(
                nullptr,

                L"O driver PawnIO e necessario para o FPSeco.\n\n"

                L"O LibreHardwareMonitor usa o PawnIO para ler "
                L"temperaturas da CPU e GPU.\n\n"

                L"O aplicativo nao pode continuar sem ele.\n\n"

                L"Clique em OK para instalar.\n"
                L"Clique em Cancelar para sair.",

                L"FPSeco - PawnIO necessario",

                MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST
            );

            if (r != IDOK)
                ExitProcess(1);

            // Executa instalador
            if (!ExtractAndRunPawnIOSetup()) {

                MessageBoxW(
                    nullptr,

                    L"A instalacao do PawnIO falhou.\n\n"

                    L"O instalador encontrou um erro.\n"
                    L"Uma versao antiga pode precisar ser removida primeiro.\n\n"

                    L"Para remover:\n\n"

                    L"Configuracoes do Windows\n"
                    L"-> Aplicativos\n"
                    L"-> Aplicativos instalados\n\n"

                    L"Depois tente novamente.",

                    L"FPSeco",

                    MB_OK | MB_ICONERROR | MB_TOPMOST
                );

                continue;
            }

            // Salva reboot pendente
            if (!CommitPawnIORebootPendingToIni()) {

                MessageBoxW(
                    nullptr,

                    L"O FPSeco nao conseguiu salvar "
                    L"o estado de reinicio necessario.\n\n"

                    L"Verifique se a pasta possui permissao "
                    L"de escrita e tente novamente.",

                    L"FPSeco",

                    MB_OK | MB_ICONERROR | MB_TOPMOST
                );

                ExitProcess(1);
            }

            RequireSystemRestartAfterPawnIOSetup();
        }

        // PawnIO desatualizado
        if (IsPawnIOOutdatedVsBundled()) {

            int r = MessageBoxW(
                nullptr,

                L"Seu PawnIO esta desatualizado.\n\n"

                L"Versoes antigas podem causar problemas "
                L"no LibreHardwareMonitor.\n\n"

                L"Temperaturas podem aparecer erradas "
                L"ou nao aparecer.\n\n"

                L"Voce precisa atualizar para continuar.\n\n"

                L"Clique em OK para atualizar.\n"
                L"Clique em Cancelar para sair.",

                L"FPSeco - Atualizacao necessaria",

                MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST
            );

            if (r != IDOK)
                ExitProcess(1);

            // Atualiza PawnIO
            if (!ExtractAndRunPawnIOSetup()) {

                MessageBoxW(
                    nullptr,

                    L"A atualizacao do PawnIO falhou.\n\n"

                    L"O instalador encontrou um erro.\n"
                    L"A versao antiga pode precisar ser removida primeiro.\n\n"

                    L"Para remover:\n\n"

                    L"Configuracoes do Windows\n"
                    L"-> Aplicativos\n"
                    L"-> Aplicativos instalados\n\n"

                    L"Depois tente novamente.",

                    L"FPSeco",

                    MB_OK | MB_ICONERROR | MB_TOPMOST
                );

                continue;
            }

            // Salva reboot pendente
            if (!CommitPawnIORebootPendingToIni()) {

                MessageBoxW(
                    nullptr,

                    L"O FPSeco nao conseguiu salvar "
                    L"o estado de reinicio necessario.\n\n"

                    L"Verifique se a pasta possui permissao "
                    L"de escrita e tente novamente.",

                    L"FPSeco",

                    MB_OK | MB_ICONERROR | MB_TOPMOST
                );

                ExitProcess(1);
            }

            RequireSystemRestartAfterPawnIOSetup();
        }

        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// LibreHardwareMonitor (LHWM) — monitoramento de hardware entre fornecedores
// ═══════════════════════════════════════════════════════════════════════════
// Verdadeiro para nós de hardware "GPU" do LHWM aos quais devemos vincular sensores (discretos + integrados).
// Nomes como "AMD Radeon(TM) Graphics" (ROG Ally / APUs) nunca correspondiam apenas a "Radeon RX" / GeForce,
// então g_gpuCount permanecia 0 e as estatísticas da GPU ficavam indisponíveis.
static bool IsLhwmGpuHardwareNode(const std::string& name)
{
    if (name.find("GeForce") != std::string::npos) return true;
    if (name.find("RTX") != std::string::npos) return true;
    if (name.find("GTX") != std::string::npos) return true;
    if (name.find("Radeon RX") != std::string::npos) return true;
    if (name.find("Radeon Pro") != std::string::npos) return true;
    if (name.find("NVIDIA") != std::string::npos) return true;
    if (name.find("Intel") != std::string::npos && name.find("Arc") != std::string::npos) return true;

    // Integrada AMD: "Radeon" presente, mas não linhas de produtos discretos RX / Pro
    if (name.find("Radeon") != std::string::npos &&
        name.find("Radeon RX") == std::string::npos && name.find("Radeon Pro") == std::string::npos) {
        if (name.find("Graphics") != std::string::npos) return true;
        if (name.find("780M") != std::string::npos || name.find("760M") != std::string::npos ||
            name.find("740M") != std::string::npos ||
            name.find("680M") != std::string::npos || name.find("660M") != std::string::npos)
            return true;
    }

    if (name.find("Intel") != std::string::npos && name.find("Graphics") != std::string::npos) {
        if (name.find("UHD") != std::string::npos || name.find("HD Graphics") != std::string::npos ||
            name.find("Iris") != std::string::npos)
            return true;
    }

    return false;
}

// Encontra uma GPU existente na lista pelo nome, ou retorna -1
static int FindGpuByName(const char* name) {
    for (int i = 0; i < g_gpuCount; i++) {
        if (strcmp(g_gpuList[i].name, name) == 0) return i;
    }
    return -1;
}

static bool IsGpuMemoryClockSensor(const std::string& name)
{
    std::string n;
    n.reserve(name.size());
    for (char c : name)
        n += (char)tolower((unsigned char)c);
    if (n.find("memory") != std::string::npos && n.find("clock") != std::string::npos)
        return true;
    if (n.find("hbm") != std::string::npos)
        return true;
    if (n.find("vram") != std::string::npos && n.find("clock") != std::string::npos)
        return true;
    return false;
}

// O LHWM expõe vários sensores "*Memory Total*" em APUs (ex.: ROG Ally). "Shared Memory Total"
// contém a substring "Memory Total" e pode sobrescrever "GPU Memory Total" (a última correspondência vence).
// Preferir a mesma linha que os usuários veem no LHM: GPU Memory Total, depois dedicated/D3D, não pool compartilhado.
static int VramTotalSensorPriority(const std::string& sensorName)
{
    std::string n;
    n.reserve(sensorName.size());
    for (char c : sensorName)
        n += (char)tolower((unsigned char)c);
    if (n.find("shared") != std::string::npos)
        return -1;
    if (n.find("gpu memory total") != std::string::npos)
        return 100;
    if (n.find("dedicated memory total") != std::string::npos || n.find("d3d dedicated") != std::string::npos)
        return 80;
    if (n.find("memory total") != std::string::npos)
        return 50;
    return -1;
}

// LHM AMD "Soc" (SoC) — não deve corresponder a "Socket" (substring "Soc" dentro de "Socket").
static bool IsAmdSocCpuTempSensorName(const std::string& sensorName)
{
    std::string n;
    n.reserve(sensorName.size());
    for (char c : sensorName)
        n += (char)tolower((unsigned char)c);
    if (n.find("socket") != std::string::npos)
        return false;
    if (n == "soc")
        return true;
    if (n.find("soc") != std::string::npos)
        return true;
    return false;
}

// Quando SoC / Package está ausente, prefira a mesma leitura do die que os usuários observam no LHM (Core Tctl/Tdie).
static int CpuTempFallbackPriority(const std::string& sensorName)
{
    std::string n;
    n.reserve(sensorName.size());
    for (char c : sensorName)
        n += (char)tolower((unsigned char)c);
    const bool hasCore = (n.find("core") != std::string::npos);
    const bool hasTctl = (n.find("tctl") != std::string::npos);
    const bool hasTdie = (n.find("tdie") != std::string::npos);
    const bool hasCcd = (n.find("ccd") != std::string::npos);
    if (hasCore && (hasTctl || hasTdie))
        return 100;
    if (hasCcd)
        return 60;
    if (hasTctl || hasTdie)
        return 80;
    return 10;
}

static bool InitLHWM()
{
    try {
        auto sensors = LHWM::GetHardwareSensorMap();
        if (sensors.empty()) return false;

        g_cpuClockOpts.clear();
        for (int gi = 0; gi < MAX_GPUS; gi++)
            g_gpuList[gi].coreClockOpts.clear();

        // A estrutura do mapa do wrapper lhwm-cpp-wrapper é:
        // Chave (map key) = Nome do hardware (ex., "AMD Ryzen 9 5900HS...")
        // Valor = vetor<tuple<nomeSensor, tipoSensor, caminhoSensor>>
        //   tupla[0] = Nome do sensor (ex., "CPU Core #1", "GPU Core")
        //   tupla[1] = Tipo do sensor (ex., "Temperature", "Load")
        //   tupla[2] = Caminho do sensor (ex., "/amdcpu/0/temperature/0")

        std::string cpuTempFallbackPath;
        int         cpuTempFallbackPri = -1;
        g_gpuCount = 0;  // Redefine contagem de GPUs

        for (const auto& [hardwareName, sensorList] : sensors) {
            // Verifica se é hardware de CPU ou GPU pelo nome
            bool isCpuHardware = (hardwareName.find("Ryzen") != std::string::npos ||
                hardwareName.find("Intel") != std::string::npos ||
                hardwareName.find("CPU") != std::string::npos ||
                hardwareName.find("Core") != std::string::npos);

            bool isGpuHardware = IsLhwmGpuHardwareNode(hardwareName);

            // Se for um nó de hardware GPU (discreto ou integrado), localiza ou cria entrada na lista
            int gpuIndex = -1;
            if (isGpuHardware && g_gpuCount < MAX_GPUS) {
                // Limpa o nome do hardware - LHWM pode retornar "Nome : /caminho", queremos apenas o nome
                std::string cleanName = hardwareName;
                size_t colonPos = cleanName.find(" : ");
                if (colonPos != std::string::npos) {
                    cleanName = cleanName.substr(0, colonPos);
                }

                gpuIndex = FindGpuByName(cleanName.c_str());
                if (gpuIndex < 0) {
                    // Nova GPU - adiciona à lista
                    gpuIndex = g_gpuCount;
                    snprintf(g_gpuList[gpuIndex].name, sizeof(g_gpuList[gpuIndex].name), "%s", cleanName.c_str());
                    g_gpuList[gpuIndex].tempPath.clear();
                    g_gpuList[gpuIndex].loadPath.clear();
                    g_gpuList[gpuIndex].vramUsedPath.clear();
                    g_gpuList[gpuIndex].vramTotalPath.clear();
                    g_gpuList[gpuIndex].vramTotalPri = -1;
                    g_gpuCount++;
                }
            }

            // Itera por todos os sensores para este hardware
            for (const auto& sensorInfo : sensorList) {
                const auto& [sensorName, sensorType, sensorPath] = sensorInfo;

                // Também detecta pelo padrão do caminho
                bool isCpuPath = (sensorPath.find("/amdcpu/") != std::string::npos ||
                    sensorPath.find("/intelcpu/") != std::string::npos);
                bool isGpuPath = (sensorPath.find("/gpu-nvidia/") != std::string::npos ||
                    sensorPath.find("/gpu-amd/") != std::string::npos ||
                    sensorPath.find("/gpu-intel/") != std::string::npos);

                // Sensores de temperatura da CPU
                if ((isCpuHardware || isCpuPath) && sensorType == "Temperature") {
                    // Ordem de prioridade para temperatura da CPU (correspondendo ao Gerenciador de Tarefas):
                    // 1. AMD SoC ("Soc") — não "Socket" (falso positivo na substring "Soc")
                    // 2. "Package" - temperatura do pacote Intel
                    // 3. Fallback: prefere Core (Tctl/Tdie), depois outros Tdie/Tctl, depois CCD, senão qualquer um
                    if (IsAmdSocCpuTempSensorName(sensorName)) {
                        g_lhwmCpuTempPath = sensorPath;  // Melhor escolha para AMD (quando presente)
                    }
                    else if (g_lhwmCpuTempPath.empty() &&
                        sensorName.find("Package") != std::string::npos) {
                        g_lhwmCpuTempPath = sensorPath;  // Melhor escolha para Intel
                    }
                    else if (g_lhwmCpuTempPath.empty()) {
                        int p = CpuTempFallbackPriority(sensorName);
                        if (p > cpuTempFallbackPri) {
                            cpuTempFallbackPath = sensorPath;
                            cpuTempFallbackPri = p;
                        }
                    }
                }

                // Sensores GPU - armazena na entrada da GPU
                if (gpuIndex >= 0) {
                    if (sensorType == "Temperature") {
                        // Prefere "GPU Core" exatamente, evita "Hot Spot" (corresponde ao Gerenciador de Tarefas)
                        bool isHotSpot = (sensorName.find("Hot Spot") != std::string::npos ||
                            sensorName.find("Hotspot") != std::string::npos);
                        bool isGpuCore = (sensorName == "GPU Core" ||
                            sensorName.find("GPU Core") != std::string::npos);

                        if (isGpuCore && !isHotSpot) {
                            g_gpuList[gpuIndex].tempPath = sensorPath;  // Melhor escolha
                        }
                        else if (g_gpuList[gpuIndex].tempPath.empty() && !isHotSpot) {
                            g_gpuList[gpuIndex].tempPath = sensorPath;  // Fallback (não hotspot)
                        }
                    }
                    else if (sensorType == "Load") {
                        if (sensorName.find("Core") != std::string::npos ||
                            sensorName.find("GPU") != std::string::npos ||
                            g_gpuList[gpuIndex].loadPath.empty()) {
                            g_gpuList[gpuIndex].loadPath = sensorPath;
                        }
                    }
                    else if (sensorType == "SmallData" || sensorType == "Data") {
                        if (sensorName.find("Memory Used") != std::string::npos ||
                            sensorName.find("GPU Memory Used") != std::string::npos) {
                            g_gpuList[gpuIndex].vramUsedPath = sensorPath;
                        }
                        else {
                            int totPri = VramTotalSensorPriority(sensorName);
                            if (totPri >= 0 && totPri > g_gpuList[gpuIndex].vramTotalPri) {
                                g_gpuList[gpuIndex].vramTotalPath = sensorPath;
                                g_gpuList[gpuIndex].vramTotalPri = totPri;
                            }
                        }
                    }
                    else if (sensorType == "Clock") {
                        if (IsGpuMemoryClockSensor(sensorName))
                            continue;
                        bool dup = false;
                        for (const auto& e : g_gpuList[gpuIndex].coreClockOpts)
                            if (e.second == sensorPath) { dup = true; break; }
                        if (!dup)
                            g_gpuList[gpuIndex].coreClockOpts.push_back({ sensorName, sensorPath });
                    }
                }

                if (sensorType == "Clock") {
                    if ((isCpuHardware || isCpuPath) && !isGpuPath) {
                        bool dup = false;
                        for (const auto& e : g_cpuClockOpts)
                            if (e.second == sensorPath) { dup = true; break; }
                        if (!dup)
                            g_cpuClockOpts.push_back({ sensorName, sensorPath });
                    }
                }
            }
        }

        // Usa temperatura da CPU fallback se necessário
        if (g_lhwmCpuTempPath.empty() && !cpuTempFallbackPath.empty()) {
            g_lhwmCpuTempPath = cpuTempFallbackPath;
        }

        // Limita GPU selecionada ao intervalo válido
        if (g_Config.selectedGpu >= g_gpuCount) {
            g_Config.selectedGpu = 0;
        }

        // Define caminhos ativos da GPU e nome
        if (g_gpuCount > 0) {
            int idx = g_Config.selectedGpu;
            g_lhwmGpuTempPath = g_gpuList[idx].tempPath;
            g_lhwmGpuLoadPath = g_gpuList[idx].loadPath;
            g_lhwmGpuVramUsedPath = g_gpuList[idx].vramUsedPath;
            g_lhwmGpuVramTotalPath = g_gpuList[idx].vramTotalPath;
            snprintf(g_gpuName, sizeof(g_gpuName), "%s", g_gpuList[idx].name);
        }

        ValidateFrequencyPaths();

        return !g_lhwmCpuTempPath.empty() || g_gpuCount > 0;
    }
    catch (...) {
        return false;
    }
}

static std::atomic<bool> g_lhwmInitFinished{ false };

static void LhwmBackgroundInitThread()
{
    bool ok = false;
    try {
        ok = InitLHWM();
    }
    catch (...) {
        ok = false;
    }
    g_lhwmAvailable = ok;
    g_lhwmInitFinished.store(true, std::memory_order_release);
}

static void PollLHWMStats()
{
    if (!g_lhwmAvailable) return;

    try {
        // Temperatura da CPU (armazenada em g_lhwmCpuTemp, usada diretamente em outro lugar)
        if (!g_lhwmCpuTempPath.empty()) {
            g_lhwmCpuTemp = LHWM::GetSensorValue(g_lhwmCpuTempPath);
        }

        // Estatísticas da GPU - grava diretamente nas variáveis unificadas
        if (!g_lhwmGpuTempPath.empty()) {
            g_gpuTemp = LHWM::GetSensorValue(g_lhwmGpuTempPath);
        }
        if (!g_lhwmGpuLoadPath.empty()) {
            g_gpuUsage = LHWM::GetSensorValue(g_lhwmGpuLoadPath);
        }
        if (!g_lhwmGpuVramUsedPath.empty()) {
            // O valor está em MB, converte para GB
            g_vramUsed = LHWM::GetSensorValue(g_lhwmGpuVramUsedPath) / 1024.0f;
        }
        if (!g_lhwmGpuVramTotalPath.empty()) {
            g_vramTotal = LHWM::GetSensorValue(g_lhwmGpuVramTotalPath) / 1024.0f;
        }
    }
    catch (...) {
        // Ignora silenciosamente erros de consulta
    }
}

// Muda para uma GPU diferente pelo índice
static void SelectGpu(int index)
{
    if (index < 0 || index >= g_gpuCount) return;

    g_Config.selectedGpu = index;

    // Atualiza caminhos de sensores ativos
    g_lhwmGpuTempPath = g_gpuList[index].tempPath;
    g_lhwmGpuLoadPath = g_gpuList[index].loadPath;
    g_lhwmGpuVramUsedPath = g_gpuList[index].vramUsedPath;
    g_lhwmGpuVramTotalPath = g_gpuList[index].vramTotalPath;

    snprintf(g_gpuName, sizeof(g_gpuName), "%s", g_gpuList[index].name);

    ValidateFrequencyPaths();
}

// ═══════════════════════════════════════════════════════════════════════════
// ETW — captura de FPS do jogo (conecta eventos DXGI Present em todo o sistema)
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

    // Verifica DXGI Present::Start (ID do Evento 42) - DirectX 10/11/12
    if (memcmp(&pEvent->EventHeader.ProviderId, &DXGI_PROVIDER, sizeof(GUID)) == 0) {
        if (pEvent->EventHeader.EventDescriptor.Id == 42) {
            isValidPresentEvent = true;
            isDxgiEvent = true;
        }
    }
    // Verifica eventos D3D9 Present (ID do Evento 1 = Present::Start)
    else if (memcmp(&pEvent->EventHeader.ProviderId, &D3D9_PROVIDER, sizeof(GUID)) == 0) {
        if (pEvent->EventHeader.EventDescriptor.Id == 1) {
            isValidPresentEvent = true;
            isD3D9Event = true;
        }
    }
    // Verifica eventos DxgKrnl - captura Vulkan, OpenGL e todas as outras APIs gráficas no nível do kernel
    else if (memcmp(&pEvent->EventHeader.ProviderId, &DXGKRNL_PROVIDER, sizeof(GUID)) == 0) {
        USHORT eventId = pEvent->EventHeader.EventDescriptor.Id;
        // Eventos Present::Info, Flip::Info ou Blit::Info indicam uma apresentação de quadro
        if (eventId == DXGKRNL_EVENT_PRESENT_INFO ||
            eventId == DXGKRNL_EVENT_FLIP_INFO ||
            eventId == DXGKRNL_EVENT_BLIT_INFO) {
            isValidPresentEvent = true;
            isDxgKrnlOnlyEvent = true;
        }
    }

    if (!isValidPresentEvent) return;

    double ts = (double)pEvent->EventHeader.TimeStamp.QuadPart / g_qpcFreq;

    // Acumulador simples de 1 segundo (tudo na thread ETW — sem necessidade de bloqueio)
    static DWORD s_lastPid = 0;
    static double s_startTs = 0;
    static int   s_dxgiCount = 0;      // Contagem de eventos DXGI (DirectX 10/11/12)
    static int   s_d3d9Count = 0;      // Contagem de eventos D3D9
    static int   s_dxgKrnlCount = 0;   // Contagem de eventos apenas DxgKrnl (Vulkan/OpenGL)

    if (pid != s_lastPid) {
        s_lastPid = pid;
        s_dxgiCount = 0;
        s_d3d9Count = 0;
        s_dxgKrnlCount = 0;
        s_startTs = ts;
        return;
    }

    // Conta eventos por fonte
    if (isDxgiEvent) s_dxgiCount++;
    if (isD3D9Event) s_d3d9Count++;
    if (isDxgKrnlOnlyEvent) s_dxgKrnlCount++;

    double elapsed = ts - s_startTs;
    if (elapsed >= 1.0) {
        // Prioriza DXGI/D3D9 (chamadas explícitas da API do jogo) sobre DxgKrnl (nível de kernel)
        // Isso filtra aplicativos de desktop como explorer.exe que só aparecem no DxgKrnl
        //
        // Prioridade:
        // 1. Eventos D3D9 = jogo DirectX 9
        // 2. Eventos DXGI = jogo DirectX 10/11/12  
        // 3. APENAS se nenhum evento DXGI/D3D9, usa DxgKrnl = jogo Vulkan/OpenGL
        //
        // Aplicativos de desktop (explorer, terminais, navegadores) passam pelo DWM que usa DxgKrnl,
        // mas eles não chamam DXGI/D3D9 Present diretamente, então são filtrados.

        int frameCount = 0;

        if (s_d3d9Count > 0) {
            // Jogo DirectX 9 - usa contagem D3D9
            frameCount = s_d3d9Count;
        }
        else if (s_dxgiCount > 0) {
            // Jogo DirectX 10/11/12 - usa contagem DXGI
            frameCount = s_dxgiCount;
        }
        else if (s_dxgKrnlCount > 0) {
            // Nenhum evento DXGI/D3D9, apenas DxgKrnl
            // Pode ser Vulkan/OpenGL OU um aplicativo de desktop através do DWM
            // 
            // Filtro: Só conta como jogo se FPS >= 20
            // Jogos reais renderizam a 20+ FPS, aplicativos de desktop tipicamente < 20 FPS
            float potentialFps = (float)s_dxgKrnlCount / (float)elapsed;
            if (potentialFps >= 20.0f) {
                frameCount = s_dxgKrnlCount;
            }
            // Se < 20 FPS, trata como aplicativo de desktop (frameCount permanece 0)
        }

        if (frameCount > 0) {
            g_gameFps.store((float)frameCount / (float)elapsed, std::memory_order_relaxed);
        }
        else {
            g_gameFps.store(0.0f, std::memory_order_relaxed);
        }

        s_dxgiCount = 0;
        s_d3d9Count = 0;
        s_dxgKrnlCount = 0;
        s_startTs = ts;
    }
}

static bool StartEtwSession()
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreq = (double)freq.QuadPart;

    // Buffer para propriedades + nome da sessão
    struct { EVENT_TRACE_PROPERTIES p; char name[256]; } buf;

    // Para qualquer sessão remanescente de uma falha anterior
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize = sizeof(buf);
    buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
    ControlTraceA(0, ETW_SESSION_NAME, &buf.p, EVENT_TRACE_CONTROL_STOP);

    // Prepara propriedades novas
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize = sizeof(buf);
    buf.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    buf.p.Wnode.ClientContext = 1;                        // Timestamps QPC
    buf.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    buf.p.LoggerNameOffset = offsetof(decltype(buf), name);

    ULONG rc = StartTraceA(&g_etwSession, ETW_SESSION_NAME, &buf.p);
    if (rc != ERROR_SUCCESS) return false;

    // Habilita provedor DXGI para eventos DirectX 10/11/12 Present
    rc = EnableTraceEx2(g_etwSession, &DXGI_PROVIDER,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        ZeroMemory(&buf, sizeof(buf));
        buf.p.Wnode.BufferSize = sizeof(buf);
        buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
        ControlTraceA(g_etwSession, nullptr, &buf.p, EVENT_TRACE_CONTROL_STOP);
        g_etwSession = 0;
        return false;
    }

    // Habilita provedor D3D9 para jogos DirectX 9
    rc = EnableTraceEx2(g_etwSession, &D3D9_PROVIDER,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
    // Provedor D3D9 é opcional - continua se falhar

    // Habilita provedor DxgKrnl para Vulkan, OpenGL e todas as outras APIs gráficas
    // A palavra-chave Present (0x8000000) captura eventos Present, Flip e Blit no nível do kernel
    rc = EnableTraceEx2(g_etwSession, &DXGKRNL_PROVIDER,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        DXGKRNL_KEYWORD_PRESENT | DXGKRNL_KEYWORD_BASE,
        0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        // DxgKrnl falhou - continua mesmo assim com apenas DXGI (DirectX ainda funcionará)
        // Isso pode falhar em versões mais antigas do Windows ou sem permissões adequadas
    }

    EVENT_TRACE_LOGFILEA logFile = {};
    logFile.LoggerName = const_cast<LPSTR>(ETW_SESSION_NAME);
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
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

    if (g_etwTrace != 0 && g_etwTrace != (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        CloseTrace(g_etwTrace);
        g_etwTrace = 0;
    }
    if (g_etwThread.joinable())
        g_etwThread.join();

    struct { EVENT_TRACE_PROPERTIES p; char name[256]; } buf;
    ZeroMemory(&buf, sizeof(buf));
    buf.p.Wnode.BufferSize = sizeof(buf);
    buf.p.LoggerNameOffset = offsetof(decltype(buf), name);
    ControlTraceA(g_etwSession, ETW_SESSION_NAME, &buf.p, EVENT_TRACE_CONTROL_STOP);
    g_etwSession = 0;

    g_gameFps.store(0.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
// Uso da CPU
// ═══════════════════════════════════════════════════════════════════════════
static float GetCpuUsage()
{
    static ULARGE_INTEGER sI = {}, sK = {}, sU = {};
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return 0;

    ULARGE_INTEGER i, k, u;
    i.LowPart = fi.dwLowDateTime; i.HighPart = fi.dwHighDateTime;
    k.LowPart = fk.dwLowDateTime; k.HighPart = fk.dwHighDateTime;
    u.LowPart = fu.dwLowDateTime; u.HighPart = fu.dwHighDateTime;

    ULONGLONG di = i.QuadPart - sI.QuadPart;
    ULONGLONG dk = k.QuadPart - sK.QuadPart;
    ULONGLONG du = u.QuadPart - sU.QuadPart;
    sI = i; sK = k; sU = u;

    ULONGLONG total = dk + du;
    return total ? (1.0f - (float)di / (float)total) * 100.0f : 0.0f;
}

// ═══════════════════════════════════════════════════════════════════════════
// Ícone da bandeja
// ═══════════════════════════════════════════════════════════════════════════
void AddTrayIcon()
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // Carrega ícone embutido (ID de recurso 1), fallback para padrão se não encontrado
    g_nid.hIcon = LoadIcon(g_hInstance, MAKEINTRESOURCE(1));
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpy(g_nid.szTip, "FPSeco");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() { Shell_NotifyIcon(NIM_DELETE, &g_nid); }

void UpdateTrayTooltip()
{
    if (g_updateAvailable) {
        snprintf(g_nid.szTip, sizeof(g_nid.szTip), "FPSeco - Update ! (%s)", g_latestVersion);
    }
    else {
        lstrcpy(g_nid.szTip, "FPSeco");
    }
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

// ═══════════════════════════════════════════════════════════════════════════
// Estilo ImGui
// ═══════════════════════════════════════════════════════════════════════════
void ApplyStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10; s.FrameRounding = 6; s.GrabRounding = 6;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(14, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(10, 8);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 1);
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.27f, 0.32f, 0.6f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.17f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.24f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.26f, 0.30f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.30f, 0.75f, 1.00f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.75f, 1.00f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.85f, 1.00f, 1);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.20f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.22f, 0.28f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.28f, 0.34f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.24f, 0.28f, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// DX11
// ═══════════════════════════════════════════════════════════════════════════
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &got, &g_pd3dDeviceContext)))
        return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
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

// ═══════════════════════════════════════════════════════════════════════════
// Auxiliares de backend / modo
// ═══════════════════════════════════════════════════════════════════════════
void ShutdownBackends()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    CleanupDeviceD3D();
}

void InitBackends()
{
    CreateDeviceD3D(g_hwnd);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
}

// Alterna o modo click-through na janela do overlay
static void SetClickThrough(bool enable)
{
    LONG_PTR style = GetWindowLongPtr(g_hwnd, GWL_EXSTYLE);
    if (enable)
        style |= WS_EX_TRANSPARENT;
    else
        style &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtr(g_hwnd, GWL_EXSTYLE, style);
}

void SwitchToOverlay()
{
    ShutdownBackends();
    DestroyWindow(g_hwnd);

    // Cobre apenas a área de trabalho do monitor (exclui a barra de tarefas) para que o overlay superior não a oculte.
    RECT wa;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = wa.right - wa.left;
    const int h = wa.bottom - wa.top;

    // Sempre inicia com click-through - alternamos quando CTRL é pressionado
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

    g_hwnd = CreateWindowEx(
        exStyle,
        "FPSeco", "FPSeco", WS_POPUP,
        wa.left, wa.top, w, h, nullptr, nullptr, g_hInstance, nullptr);

    SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS m = { -1 }; DwmExtendFrameIntoClientArea(g_hwnd, &m);

    InitBackends();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    AddTrayIcon();

    // Inicia ETW para FPS real do jogo
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
    g_hwnd = CreateWindowEx(0, "FPSeco", "FPSeco",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
        cx, cy, kConfigDlgOuterW, ch, nullptr, nullptr, g_hInstance, nullptr);

    InitBackends();
    ShowWindow(g_hwnd, SW_SHOW);
    g_Mode = MODE_CONFIG;
}

// ═══════════════════════════════════════════════════════════════════════════
// Auxiliares de renderização
// ═══════════════════════════════════════════════════════════════════════════
static void Present(float r, float g, float b, float a)
{
    ImGui::Render();
    const float c[4] = { r, g, b, a };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_pRenderTargetView, c);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Desabilita VSync durante o arrasto para resposta instantânea (reduz ~16ms de latência por quadro)
    g_pSwapChain->Present(g_isDragging ? 0 : 1, 0);
}

static ImVec4 ColorByLoad(float v, float warn = 70, float crit = 90)
{
    if (v > crit) return ImVec4(1, .3f, .3f, 1);
    if (v > warn) return ImVec4(1, .85f, .15f, 1);
    return ImVec4(.70f, .70f, .75f, 1);
}

// Tooltip com quebra de linha para caber em janelas de configuração estreitas
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

// ═══════════════════════════════════════════════════════════════════════════
// WinMain
// ═══════════════════════════════════════════════════════════════════════════
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    g_hInstance = hInst;

    // ── Carrega configuração salva ──
    LoadConfig(g_Config);

    // ── Verifica atualizações em segundo plano ──
    CheckForUpdatesAsync();

    // ── Consulta hardware ──
    QueryCpuName();

    // ── Registra classe da janela com ícone ──
    HICON hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));
    if (!hIcon) hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc); wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = hIcon;
    wc.hIconSm = hIcon;
    wc.lpszClassName = "FPSeco";
    RegisterClassEx(&wc);

    // ── Janela de configuração (largura fixa; redimensionável verticalmente via WM_GETMINMAXINFO) ──
    const int ch = 820;
    int cx = (GetSystemMetrics(SM_CXSCREEN) - kConfigDlgOuterW) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - ch) / 2;
    g_hwnd = CreateWindowEx(0, wc.lpszClassName, "FPSeco",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
        cx, cy, kConfigDlgOuterW, ch, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    // ── Verifica privilégios de administrador (o aplicativo deve sempre ser executado como admin via manifest) ──
    g_isAdmin = IsRunningAsAdmin();

    if (!CreateDeviceD3D(g_hwnd)) {
        MessageBox(g_hwnd, "Falha na inicialização do DirectX 11.", "FPSeco", MB_OK | MB_ICONERROR);
        CleanupDeviceD3D(); return 1;
    }

    // Obtém nome da GPU do adaptador DXGI (fallback se LHWM não fornecer)
    QueryGpuName();

    // PawnIO é necessário: instala/atualiza (ou sai) antes do LHWM
    EnforcePawnIOOrExit();

    // A inicialização do LibreHardwareMonitor pode levar vários segundos — executa em uma thread separada
    // para que a UI de config apareça imediatamente. Consulta caminhos quando g_lhwmInitFinished for definido.
    g_lhwmAvailable = false;
    g_lhwmInitFinished.store(false, std::memory_order_relaxed);
    std::thread(LhwmBackgroundInitThread).detach();

    // Tenta WMI para temperatura da CPU como fallback (LHWM pode habilitar temp da CPU quando a inicialização terminar)
    g_cpuTempAvailable = InitWMI();
    if (g_cpuTempAvailable) {
        float testTemp = QueryCpuTemperature();
        g_cpuTempAvailable = (testTemp > 0.0f && testTemp < 150.0f);
    }

    // Exibe janela de configuração (a menos que início automático esteja habilitado)
    if (!g_Config.autoStart) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }

    // ── Contexto ImGui (vive por todo o aplicativo) ──
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;

    ImFontGlyphRangesBuilder glyphBuilder;
    glyphBuilder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    glyphBuilder.AddChar((ImWchar)0x2122); // MARCA REGISTRADA
    glyphBuilder.AddChar((ImWchar)0x00A9); // DIREITOS AUTORAIS
    glyphBuilder.AddChar((ImWchar)0x00AE); // MARCA REGISTRADA
    static ImVector<ImWchar> s_imguiGlyphRanges;
    s_imguiGlyphRanges.clear();
    glyphBuilder.BuildRanges(&s_imguiGlyphRanges);
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, nullptr,
        s_imguiGlyphRanges.Data);
    if (!font) { io.Fonts->Clear(); ImFontConfig fc; fc.SizePixels = 16; io.Fonts->AddFontDefault(&fc); }

    ApplyStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // ── Temporização ──
    using Clock = std::chrono::high_resolution_clock;
    auto lastCpuTime = Clock::now();
    auto lastGpuTime = lastCpuTime;
    float cpuUsage = 0;
    GetCpuUsage(); // inicializa

    // ── Início automático do overlay se habilitado ──
    if (g_Config.autoStart) {
        g_Pending = CMD_START_OVERLAY;
    }

    // ── Loop principal ──
    MSG msg = {};
    while (g_Running)
    {
        if (g_Pending == CMD_START_OVERLAY) { g_Pending = CMD_NONE; SwitchToOverlay(); }
        if (g_Pending == CMD_SHOW_SETTINGS) { g_Pending = CMD_NONE; SwitchToConfig(); }
        if (g_Pending == CMD_EXIT) { g_Running = false; break; }

        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_Running = false;
        }
        if (!g_Running) break;

        static bool s_appliedLhwmCpuTempLift = false;
        if (g_lhwmInitFinished.load(std::memory_order_acquire) && !s_appliedLhwmCpuTempLift) {
            s_appliedLhwmCpuTempLift = true;
            if (g_lhwmAvailable && !g_lhwmCpuTempPath.empty())
                g_cpuTempAvailable = true;
        }

        // ══════════════════════════════════════════════════════════════
        // MODO CONFIGURAÇÃO
        // ══════════════════════════════════════════════════════════════
        if (g_Mode == MODE_CONFIG)
        {
            // ── Ouvinte de teclas de atalho (executa mesmo durante a renderização da configuração) ──
            if (g_listeningFor != 0) {
                // Verifica se ESC foi pressionado para cancelar
                if (GetAsyncKeyState(VK_ESCAPE) & 1) {
                    g_listeningFor = 0;
                }
                else {
                    for (int vk = 1; vk < 256; vk++) {
                        // Pula botões do mouse e teclas apenas modificadoras que não queremos
                        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                        if (vk == VK_ESCAPE) continue;  // tratado acima
                        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) continue;
                        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) continue;
                        if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) continue;  // Teclas Alt

                        if (GetAsyncKeyState(vk) & 1) {
                            if (g_listeningFor == 1) g_Config.toggleKey = vk;
                            if (g_listeningFor == 2) g_Config.exitKey = vk;
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

            // ── Título ──
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextColored(ImVec4(.35f, .78f, 1, 1), "FPSeco");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine(); ImGui::TextColored(ImVec4(.45f, .45f, .5f, 1), " %s", APP_VERSION);

            // Exibe notificação de atualização disponível
            if (g_updateAvailable) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.2f, .9f, .4f, 1), " -");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.2f, .9f, .4f, 1));
                if (ImGui::SmallButton("Update")) {
                    ShellExecuteA(nullptr, "open",
                        "https://github.com/zRafaX/FPSeco/releases/latest",
                        nullptr, nullptr, SW_SHOWNORMAL);
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    TooltipWrappedFmt("Clique para baixar %s", g_latestVersion);
            }

            // Texto do desenvolvedor
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.45f, .45f, .5f, 1), " Desenvolvido por zrafax");

            ImGui::Spacing(); ImGui::Separator();

            // ── EXIBIÇÃO ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "EXIBIÇÃO");
            ImGui::Spacing();
            ImGui::Checkbox("  Contador de FPS (jogo)", &g_Config.showFPS);
            if (!g_isAdmin) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.9f, .4f, .2f, 1), "(precisa de admin!)");
            }
            ImGui::Checkbox("  Uso da CPU", &g_Config.showCpuUsage);
            ImGui::Checkbox("  Temp da CPU", &g_Config.showCpuTemp);
            ImGui::Checkbox("  Uso da GPU", &g_Config.showGpuUsage);
            ImGui::Checkbox("  Temp da GPU", &g_Config.showGpuTemp);
            {
                const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
                const bool lhwmBad = !g_lhwmAvailable || g_gpuCount == 0;
                if (lhwmBusy || lhwmBad) {
                    ImGui::SameLine();
                    ImGui::TextColored(lhwmBusy ? ImVec4(.55f, .55f, .58f, 1) : ImVec4(.9f, .4f, .2f, 1),
                        lhwmBusy ? "(carregando…)" : "(indisponível)");
                }
            }
            ImGui::Checkbox("  Uso de VRAM da GPU", &g_Config.showVRAM);
            if (!g_lhwmInitFinished.load(std::memory_order_acquire)) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.55f, .55f, .58f, 1), "(carregando…)");
            }
            else if (!g_lhwmAvailable || g_gpuCount == 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.9f, .4f, .2f, 1), "(indisponível)");
            }
            ImGui::Checkbox("  Uso de RAM", &g_Config.showRAM);
            ImGui::Checkbox("  Mostrar nome do processo", &g_Config.showProcessName);
            if (ImGui::IsItemHovered())
                TooltipWrapped("Rótulo do processo/jogo rastreado no overlay (todos os layouts).");

            // ── SELEÇÃO DE GPU ──
            if (g_gpuCount > 0) {
                ImGui::Spacing(); ImGui::Spacing();
                ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "SELEÇÃO DE GPU");
                ImGui::Spacing();

                // Constrói string de visualização da combo
                const char* previewName = (g_Config.selectedGpu >= 0 && g_Config.selectedGpu < g_gpuCount)
                    ? g_gpuList[g_Config.selectedGpu].name
                    : "Selecionar GPU...";

                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##gpuselect", previewName)) {
                    for (int i = 0; i < g_gpuCount; i++) {
                        bool isSelected = (g_Config.selectedGpu == i);
                        if (ImGui::Selectable(g_gpuList[i].name, isSelected)) {
                            SelectGpu(i);
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                if (g_gpuCount > 1) {
                    ImGui::TextColored(ImVec4(.45f, .45f, .50f, 1), "Múltiplas GPUs detectadas - selecione qual monitorar");
                }
            }

            // ── FREQUÊNCIA (sparklines; sensores de clock do LHWM) ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "FREQUÊNCIA");
            ImGui::Spacing();
            if (!g_lhwmInitFinished.load(std::memory_order_acquire)) {
                ImGui::TextColored(ImVec4(.55f, .55f, .58f, 1), "Inicializando LibreHardwareMonitor…");
            }
            else if (!g_lhwmAvailable) {
                ImGui::TextColored(ImVec4(.55f, .55f, .58f, 1), "Requer LibreHardwareMonitor.");
            }
            else {
                ImGui::Checkbox("  Mostrar frequência da CPU", &g_Config.showCpuFreq);
                if (g_Config.showCpuFreq) {
                    ImGui::Indent();
                    const char* cpuPrev = "(selecionar sensor)";
                    for (const auto& o : g_cpuClockOpts) {
                        if (strcmp(g_Config.cpuFreqPath, o.second.c_str()) == 0) {
                            cpuPrev = o.first.c_str();
                            break;
                        }
                    }
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##cpuclkcombo", cpuPrev)) {
                        for (const auto& o : g_cpuClockOpts) {
                            bool isSel = (strcmp(g_Config.cpuFreqPath, o.second.c_str()) == 0);
                            if (ImGui::Selectable(o.first.c_str(), isSel))
                                snprintf(g_Config.cpuFreqPath, sizeof(g_Config.cpuFreqPath), "%s", o.second.c_str());
                            if (isSel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (g_cpuClockOpts.empty())
                        ImGui::TextColored(ImVec4(.85f, .45f, .35f, 1), "  Nenhum sensor de clock da CPU encontrado.");
                    ImGui::Unindent();
                }

                ImGui::Checkbox("  Mostrar frequência do núcleo da GPU", &g_Config.showGpuCoreFreq);
                if (g_Config.showGpuCoreFreq && g_gpuCount > 0) {
                    ImGui::Indent();
                    GpuInfo& gg = g_gpuList[g_Config.selectedGpu];
                    const char* gpPrev = "(selecionar sensor)";
                    for (const auto& o : gg.coreClockOpts) {
                        if (strcmp(g_Config.gpuCoreFreqPath, o.second.c_str()) == 0) {
                            gpPrev = o.first.c_str();
                            break;
                        }
                    }
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##gpclkcombo", gpPrev)) {
                        for (const auto& o : gg.coreClockOpts) {
                            bool isSel = (strcmp(g_Config.gpuCoreFreqPath, o.second.c_str()) == 0);
                            if (ImGui::Selectable(o.first.c_str(), isSel))
                                snprintf(g_Config.gpuCoreFreqPath, sizeof(g_Config.gpuCoreFreqPath), "%s", o.second.c_str());
                            if (isSel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (gg.coreClockOpts.empty())
                        ImGui::TextColored(ImVec4(.85f, .45f, .35f, 1), "  Nenhum sensor de clock do núcleo para esta GPU.");
                    ImGui::Unindent();
                }
            }

            // ── POSIÇÃO ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "POSIÇÃO");
            ImGui::Spacing();
            int prevPos = g_Config.position;
            ImGui::RadioButton("Superior Esquerdo", &g_Config.position, POS_TOP_LEFT);
            ImGui::SameLine(0, 16);
            ImGui::RadioButton("Superior Centro", &g_Config.position, POS_TOP_CENTER);
            ImGui::SameLine(0, 16);
            ImGui::RadioButton("Superior Direito", &g_Config.position, POS_TOP_RIGHT);
            ImGui::RadioButton("Inferior Esquerdo", &g_Config.position, POS_BOTTOM_LEFT);
            ImGui::SameLine(0, 16);
            ImGui::RadioButton("Inferior Centro", &g_Config.position, POS_BOTTOM_CENTER);
            ImGui::SameLine(0, 16);
            ImGui::RadioButton("Inferior Direito", &g_Config.position, POS_BOTTOM_RIGHT);
            // Redefine posição personalizada quando o canto predefinido é alterado
            if (g_Config.position != prevPos) {
                g_Config.customX = -1.0f;
                g_Config.customY = -1.0f;
            }
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.45f, .45f, .50f, 1), "Segure CTRL para arrastar ou clique com botão direito no overlay");

            // ── LAYOUT ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "LAYOUT");
            ImGui::Spacing();
            ImGui::RadioButton("  Vertical (padrão)", &g_Config.layoutStyle, LAYOUT_VERTICAL);
            ImGui::RadioButton("  Compacto horizontal", &g_Config.layoutStyle, LAYOUT_HORIZONTAL);
            ImGui::RadioButton("  Barra estilo Steam", &g_Config.layoutStyle, LAYOUT_STEAM);
            if (ImGui::IsItemHovered())
                TooltipWrapped(
                    "Barra preta com rótulos FPS / CPU / GPU estilo Steam.\n"
                    "Mesmas estatísticas do compacto horizontal (temps, detalhes VRAM/RAM, nome do processo).\n"
                    "Com tamanho 100%, o texto corresponde à escala vertical/horizontal.");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "Tamanho do overlay");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##ovscale", &g_Config.overlayScale, 50, 200, "%d%%");
            if (ImGui::IsItemHovered())
                TooltipWrapped(
                    "Escala de texto e espaçamento para layouts verticais, horizontais e estilo Steam.\n"
                    "Segure CTRL no overlay e arraste para mover.");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "Opacidade do overlay");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##opac", &g_Config.opacity, 30, 100, "%d%%");
            if (ImGui::IsItemHovered())
                TooltipWrapped("Transparência do fundo para todos os layouts (padrão 85%).");

            // ── UNIDADE DE TEMPERATURA ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "TEMPERATURA");
            ImGui::Spacing();
            int tempUnit = g_Config.useFahrenheit ? 1 : 0;
            if (ImGui::RadioButton("Celsius", &tempUnit, 0)) g_Config.useFahrenheit = false;
            ImGui::SameLine(0, 24);
            if (ImGui::RadioButton("Fahrenheit", &tempUnit, 1)) g_Config.useFahrenheit = true;

            // ── TECLAS DE ATALHO ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "TECLAS DE ATALHO");
            ImGui::Spacing();

            // Tecla de alternância
            ImGui::Text("Alternar:");
            ImGui::SameLine(90);
            if (g_listeningFor == 1) {
                ImGui::TextColored(ImVec4(1, .8f, .2f, 1), "Pressione qualquer tecla...  ");
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancelar##1")) g_listeningFor = 0;
            }
            else {
                ImGui::Text("%-12s", GetKeyName(g_Config.toggleKey));
                ImGui::SameLine();
                if (ImGui::SmallButton("Alterar##1")) g_listeningFor = 1;
            }

            // Tecla de saída
            ImGui::Text("Sair:");
            ImGui::SameLine(90);
            if (g_listeningFor == 2) {
                ImGui::TextColored(ImVec4(1, .8f, .2f, 1), "Pressione qualquer tecla...  ");
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancelar##2")) g_listeningFor = 0;
            }
            else {
                ImGui::Text("%-12s", GetKeyName(g_Config.exitKey));
                ImGui::SameLine();
                if (ImGui::SmallButton("Alterar##2")) g_listeningFor = 2;
            }

            // ── INICIALIZAÇÃO ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "INICIALIZAÇÃO");
            ImGui::Spacing();
            ImGui::Checkbox("  Iniciar overlay imediatamente", &g_Config.autoStart);
            if (ImGui::IsItemHovered())
                TooltipWrapped("Pular esta janela e iniciar o overlay diretamente na próxima vez");

            // ── HARDWARE ──
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(.55f, .70f, .95f, 1), "PC BATATA DETECTADO");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "CPU:  %s", g_cpuName);
            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "GPU:  %s", g_gpuName);

            // ── BOTÃO INICIAR ──
            ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.12f, .56f, .37f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.16f, .68f, .44f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.10f, .48f, .32f, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8);
            const bool lhwmBusy = !g_lhwmInitFinished.load(std::memory_order_acquire);
            const char* startBtnLabel = lhwmBusy ? "Inicializando LibreHardwareMonitor…" : "Iniciar Overlay";
            ImGui::BeginDisabled(lhwmBusy);
            if (ImGui::Button(startBtnLabel, ImVec2(ImGui::GetContentRegionAvail().x, 42)))
                g_Pending = CMD_START_OVERLAY;
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            ImGui::End();
            Present(0.08f, 0.08f, 0.10f, 1);
        }

        // ══════════════════════════════════════════════════════════════
        // MODO OVERLAY
        // ══════════════════════════════════════════════════════════════
        else
        {
            // ── Teclas de atalho (configuráveis pelo usuário) ──
            if (GetAsyncKeyState(g_Config.toggleKey) & 1)
                g_OvlVisible = !g_OvlVisible;
            if (GetAsyncKeyState(g_Config.exitKey) & 1)
            {
                g_Running = false; break;
            }

            // ── Mostra/Oculta janela com base no flag de visibilidade ──
            static bool wasVisible = true;
            if (g_OvlVisible != wasVisible) {
                ShowWindow(g_hwnd, g_OvlVisible ? SW_SHOWNA : SW_HIDE);
                wasVisible = g_OvlVisible;
            }

            if (!g_OvlVisible) { Sleep(50); continue; }

            // ── Atualiza PID alvo (processo da janela em primeiro plano) ──
            HWND fg = GetForegroundWindow();
            DWORD currentPid = 0;
            if (fg && fg != g_hwnd) {
                GetWindowThreadProcessId(fg, &currentPid);
                g_targetPid.store(currentPid, std::memory_order_relaxed);
            }

            // ── Redefine FPS quando o aplicativo alvo muda ou fecha ──
            if (currentPid != g_lastTargetPid) {
                g_gameFps.store(0.0f, std::memory_order_relaxed);
                g_lastTargetPid = currentPid;
                // Atualiza nome do processo
                GetProcessName(currentPid, g_targetProcessName, sizeof(g_targetProcessName));
            }
            // Também verifica se o processo ainda está ativo
            if (g_lastTargetPid != 0) {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_lastTargetPid);
                if (hProc) {
                    DWORD exitCode = 0;
                    if (GetExitCodeProcess(hProc, &exitCode) && exitCode != STILL_ACTIVE) {
                        g_gameFps.store(0.0f, std::memory_order_relaxed);
                        g_lastTargetPid = 0;
                    }
                    CloseHandle(hProc);
                }
                else {
                    // Processo não existe mais
                    g_gameFps.store(0.0f, std::memory_order_relaxed);
                    g_lastTargetPid = 0;
                }
            }

            // ── Métricas periódicas (uma vez/seg) ──
            // Pula TODA leitura de estatísticas durante o arrasto para máxima suavidade
            static float cachedRamUsed = 0, cachedRamTotal = 1;
            auto now = Clock::now();

            if (!g_isDragging) {
                float cpuElapsed = std::chrono::duration<float>(now - lastCpuTime).count();
                if (cpuElapsed >= 1.0f) {
                    cpuUsage = GetCpuUsage();
                    // Consulta temp da CPU - prefere LHWM sobre WMI
                    if (g_lhwmAvailable && !g_lhwmCpuTempPath.empty()) {
                        g_cpuTemp = g_lhwmCpuTemp;
                    }
                    else if (g_cpuTempAvailable) {
                        g_cpuTemp = QueryCpuTemperature();
                    }
                    lastCpuTime = now;
                }

                float gpuElapsed = std::chrono::duration<float>(now - lastGpuTime).count();
                if (gpuElapsed >= 1.0f) {
                    // Consulta LHWM primeiro (cobre AMD, Intel, NVIDIA)
                    if (g_lhwmAvailable) {
                        PollLHWMStats();
                    }
                    lastGpuTime = now;

                    // Atualiza tooltip da bandeja se a verificação de atualização terminou
                    static bool tooltipUpdated = false;
                    if (g_updateCheckDone && !tooltipUpdated) {
                        UpdateTrayTooltip();
                        tooltipUpdated = true;
                    }
                }

                // ── RAM ──
                MEMORYSTATUSEX mem = {}; mem.dwLength = sizeof(mem);
                GlobalMemoryStatusEx(&mem);
                cachedRamUsed = (float)(mem.ullTotalPhys - mem.ullAvailPhys) / (1024.f * 1024.f * 1024.f);
                cachedRamTotal = (float)(mem.ullTotalPhys) / (1024.f * 1024.f * 1024.f);

                static auto lastClkPoll = Clock::now();
                float clkDt = std::chrono::duration<float>(now - lastClkPoll).count();
                if (clkDt >= 0.12f) {
                    lastClkPoll = now;
                    PollClockSensors();
                }
            }

            // Usa valores em cache (atualizados quando não está arrastando)
            float ramUsed = cachedRamUsed;
            float ramTotal = cachedRamTotal;

            // ── FPS do jogo (do ETW) ──
            float gameFps = g_gameFps.load(std::memory_order_relaxed);

            // ── Manipula tecla CTRL para arrastar / menu do botão direito ──
            // Responde ao CTRL apenas quando o cursor está sobre o overlay
            POINT cursorPt; GetCursorPos(&cursorPt);
            bool cursorOverOverlay = PtInRect(&g_overlayBounds, cursorPt);
            bool ctrlKeyDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool leftMouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

            // Rastreia estado de interação e arrasto separadamente
            // - interactionActive: controla click-through e flags da janela
            // - g_isDragging: controla VSync (definido pela detecção de arrasto do ImGui para precisão)
            static bool interactionActive = false;

            // Redefine estado de arrasto quando o mouse é liberado
            if (!leftMouseDown) {
                g_isDragging = false;
            }

            // Entra no modo de interação: Ctrl pressionado E cursor sobre o overlay
            if (ctrlKeyDown && cursorOverOverlay) {
                interactionActive = true;
            }
            // Sai do modo de interação: Ctrl liberado, OU (cursor saiu do overlay E não está interagindo ativamente)
            else if (!ctrlKeyDown) {
                interactionActive = false;
                g_isDragging = false;
            }
            else if (!cursorOverOverlay && !leftMouseDown) {
                // Só sai se o cursor saiu E não está segurando o botão do mouse
                interactionActive = false;
            }
            // Enquanto o mouse está pressionado, permanece no modo de interação independentemente da posição do cursor

            bool ctrlHeld = interactionActive;

            // Gerencia estado de click-through
            // IMPORTANTE: Não reabilite click-through enquanto o botão do mouse está pressionado
            // (isso interromperia uma operação de arrasto em andamento)
            static bool wasCtrlHeld = false;
            if (ctrlHeld && !wasCtrlHeld) {
                // Entrando no modo de interação - desabilita click-through
                SetClickThrough(false);
                wasCtrlHeld = true;
            }
            else if (!ctrlHeld && wasCtrlHeld && !leftMouseDown) {
                // Saindo do modo de interação - reabilita click-through apenas se o mouse foi liberado
                SetClickThrough(true);
                wasCtrlHeld = false;
            }

            // Menu de contexto do botão direito (quando CTRL está pressionado E o botão direito foi clicado NO modo de interação)
            // Rastreia estado do botão direito do mouse para detectar cliques novos (não cliques de outro lugar)
            static bool rightMouseWasDown = false;
            bool rightMouseDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            bool rightMouseJustPressed = rightMouseDown && !rightMouseWasDown;
            rightMouseWasDown = rightMouseDown;

            // Só mostra o menu se o botão direito foi clicado enquanto já estava no modo de interação
            if (ctrlHeld && rightMouseJustPressed) {
                HMENU m = CreatePopupMenu();
                AppendMenu(m, MF_STRING, IDM_HIDE, "Ocultar");
                AppendMenu(m, MF_STRING, IDM_RESET_POS, "Redefinir");
                AppendMenu(m, MF_SEPARATOR, 0, nullptr);
                AppendMenu(m, MF_STRING, IDM_SETTINGS, "Configurar");
                AppendMenu(m, MF_STRING, IDM_EXIT, "Sair");
                SetForegroundWindow(g_hwnd);
                int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                    cursorPt.x, cursorPt.y, 0, g_hwnd, nullptr);
                DestroyMenu(m);
                switch (cmd) {
                case IDM_HIDE:       g_OvlVisible = false;           break;
                case IDM_RESET_POS:
                    g_Config.customX = -1.f;
                    g_Config.customY = -1.f;
                    g_overlayForceCornerSnap = true;
                    SaveConfig(g_Config);
                    break;
                case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS;  break;
                case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
                }
            }

            // ── Quadro ImGui ──
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Posição: usa personalizada se definida, caso contrário usa canto predefinido
            // IMPORTANTE: Não define posição durante o modo de interação - deixa o ImGui lidar com o arrasto
            float margin = (g_Config.layoutStyle == LAYOUT_STEAM) ? 0.f : 16.f;
            RECT work{};
            SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
            const float wx = (float)work.left;
            const float wy = (float)work.top;
            const float sw = (float)(work.right - work.left);
            const float sh = (float)(work.bottom - work.top);
            ImVec2 pos, pivot = { 0, 0 };
            bool hasCustomPos = (g_Config.customX >= 0 && g_Config.customY >= 0);

            if (hasCustomPos) {
                // O usuário arrastou o overlay - usa a posição dele
                pos = ImVec2(g_Config.customX, g_Config.customY);
            }
            else {
                // Usa canto predefinido (coordenadas dentro da área de trabalho do monitor principal)
                switch (g_Config.position) {
                default:
                case POS_TOP_LEFT:
                    pos = ImVec2(wx + margin, wy + margin);
                    pivot = { 0, 0 };
                    break;
                case POS_TOP_CENTER:
                    pos = ImVec2(wx + sw * 0.5f, wy + margin);
                    pivot = { 0.5f, 0 };
                    break;
                case POS_TOP_RIGHT:
                    pos = ImVec2(wx + sw - margin, wy + margin);
                    pivot = { 1, 0 };
                    break;
                case POS_BOTTOM_LEFT:
                    pos = ImVec2(wx + margin, wy + sh - margin);
                    pivot = { 0, 1 };
                    break;
                case POS_BOTTOM_CENTER:
                    pos = ImVec2(wx + sw * 0.5f, wy + sh - margin);
                    pivot = { 0.5f, 1 };
                    break;
                case POS_BOTTOM_RIGHT:
                    pos = ImVec2(wx + sw - margin, wy + sh - margin);
                    pivot = { 1, 1 };
                    break;
                }
            }

            // Só define posição quando NÃO está no modo de interação para evitar conflito com o arrasto do ImGui
            // - Quando não está interagindo: define posição (canto predefinido ou posição personalizada salva)
            // - Quando está interagindo (ctrlHeld): deixa o ImGui gerenciar a posição livremente para arrasto suave
            // - g_overlayForceCornerSnap: após o menu "Redefinir Posição", ajusta ao canto mesmo se CTRL ainda estiver pressionado
            {
                bool forceSnap = g_overlayForceCornerSnap;
                if (forceSnap)
                    g_overlayForceCornerSnap = false;
                if (!ctrlHeld || forceSnap) {
                    ImGui::SetNextWindowPos(pos, hasCustomPos ? ImGuiCond_Once : ImGuiCond_Always, pivot);
                }
            }

            const float opacityPct = (float)g_Config.opacity;
            const float overlayBgAlpha = ctrlHeld ? 1.0f : (opacityPct / 100.f);
            ImGui::SetNextWindowBgAlpha(overlayBgAlpha);

            const float ovSc = g_Config.overlayScale / 100.f;

            // Flags da janela - permite arrastar quando CTRL está pressionado
            ImGuiWindowFlags wf =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

            if (!ctrlHeld) {
                wf |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
            }

            if (g_Config.layoutStyle == LAYOUT_STEAM) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * ovSc, 6.f * ovSc));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f * ovSc, 2.f * ovSc));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.24f, 0.f));
            }

            ImGui::Begin("##ovl", nullptr, wf);

            // Atualiza limites do overlay para teste de hit (usado para verificação do menu de contexto)
            {
                ImVec2 wPos = ImGui::GetWindowPos();
                ImVec2 wSize = ImGui::GetWindowSize();
                g_overlayBounds.left = (LONG)wPos.x;
                g_overlayBounds.top = (LONG)wPos.y;
                g_overlayBounds.right = (LONG)(wPos.x + wSize.x);
                g_overlayBounds.bottom = (LONG)(wPos.y + wSize.y);
            }

            // Salva posição quando arrastado e atualiza estado de arrasto
            if (ctrlHeld) {
                ImVec2 winPos = ImGui::GetWindowPos();
                g_Config.customX = winPos.x;
                g_Config.customY = winPos.y;

                // Desabilita VSync assim que o usuário clica no modo de interação
                // (não espera pelo limiar de arrasto - isso evita atraso inicial)
                if (leftMouseDown) {
                    g_isDragging = true;
                }
            }

            // ── Desenha borda brilhante quando CTRL está pressionado ──
            if (ctrlHeld) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 wMin = ImGui::GetWindowPos();
                ImVec2 wMax = ImVec2(wMin.x + ImGui::GetWindowSize().x, wMin.y + ImGui::GetWindowSize().y);

                // Efeito de brilho animado (pulsante)
                float t = (float)fmod(ImGui::GetTime() * 2.0, 3.14159 * 2.0);
                float glow = 0.6f + 0.4f * sinf(t);

                // Desenha múltiplas bordas para efeito de brilho (externa para interna)
                ImU32 glowColor1 = IM_COL32(80, 180, 255, (int)(40 * glow));
                ImU32 glowColor2 = IM_COL32(80, 180, 255, (int)(80 * glow));
                ImU32 glowColor3 = IM_COL32(100, 200, 255, (int)(160 * glow));
                ImU32 coreColor = IM_COL32(120, 220, 255, (int)(255 * glow));

                dl->AddRect(ImVec2(wMin.x - 4, wMin.y - 4), ImVec2(wMax.x + 4, wMax.y + 4), glowColor1, 8.0f, 0, 3.0f);
                dl->AddRect(ImVec2(wMin.x - 2, wMin.y - 2), ImVec2(wMax.x + 2, wMax.y + 2), glowColor2, 6.0f, 0, 2.0f);
                dl->AddRect(ImVec2(wMin.x - 1, wMin.y - 1), ImVec2(wMax.x + 1, wMax.y + 1), glowColor3, 4.0f, 0, 1.5f);
                dl->AddRect(wMin, wMax, coreColor, 4.0f, 0, 1.0f);
            }

            // ═══════════════════════════════════════════════════════════
            // BARRA ESTILO STEAM (mesmas estatísticas do horizontal: FPS, CPU+temp, GPU+temp, VRAM/RAM, linha do processo)
            // ═══════════════════════════════════════════════════════════
            if (g_Config.layoutStyle == LAYOUT_STEAM) {
                const float ss = ovSc;
                const float hs = 4.f * ss;
                const float hsTight = 3.f * ss;

                const ImVec4 labFps(0.92f, 0.52f, 0.58f, 1.f);  // rosa-avermelhado suave (rótulo estilo Steam)
                const ImVec4 labCpu(0.94f, 0.88f, 0.58f, 1.f);  // amarelo pálido
                const ImVec4 labGpu(0.52f, 0.90f, 0.70f, 1.f);  // verde menta
                const ImVec4 sepC(0.55f, 0.55f, 0.58f, 1.f);

                const bool showProcLine = g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0];
                const float lineH = ImGui::GetTextLineHeightWithSpacing();

                ImGui::SetWindowFontScale(1.0f * ss);

                // Alvo de hit de linha inteira para que CTRL+arrastar funcione em rótulos/valores (não apenas padding)
                ImVec2 steamRowStart = ImGui::GetCursorPos();
                if (ctrlHeld) {
                    float rowH = lineH * (showProcLine ? 2.45f : 1.55f);
                    float rowW = ImGui::GetContentRegionAvail().x;
                    if (rowW < 32.f)
                        rowW = ImGui::GetWindowWidth() - ImGui::GetCursorPos().x - ImGui::GetStyle().WindowPadding.x;
                    ImGui::InvisibleButton("##SteamBarDrag", ImVec2(rowW, rowH));
                    ImGuiWindow* wbar = ImGui::GetCurrentWindow();
                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
                        ImGui::StartMouseMovingWindow(wbar);
                    ImGui::SetCursorPos(steamRowStart);
                }

                bool needSep = false;

                auto FpsTierCol = [](float fps) -> ImVec4 {
                    if (fps >= 60.f) return ImVec4(.18f, .94f, .45f, 1.f);
                    if (fps >= 30.f) return ImVec4(1.f, .85f, .15f, 1.f);
                    return ImVec4(1.f, .25f, .25f, 1.f);
                    };

                if (g_Config.showFPS) {
                    ImGui::TextColored(labFps, "FPS");
                    ImGui::SameLine(0, hsTight);
                    if (g_etwAvailable && gameFps > 0)
                        ImGui::TextColored(FpsTierCol(gameFps), "%.0f", gameFps);
                    else
                        ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1.f), "---");
                    needSep = true;
                }

                const bool wantCpuHzSt = g_Config.showCpuFreq && g_Config.cpuFreqPath[0] && g_lhwmAvailable;
                if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzSt) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    ImGui::TextColored(labCpu, "CPU");
                    ImGui::SameLine(0, hsTight);
                    bool anyCpuSteam = false;
                    if (g_Config.showCpuUsage) {
                        ImGui::TextColored(ColorByLoad(cpuUsage), "%.0f%%", cpuUsage);
                        anyCpuSteam = true;
                    }
                    if (g_Config.showCpuTemp) {
                        if (g_cpuTempAvailable && g_cpuTemp > 0) {
                            if (anyCpuSteam) ImGui::SameLine(0, 2.f * ss);
                            float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                            ImVec4 tc = g_cpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                : g_cpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                : ImVec4(.70f, .70f, .75f, 1);
                            ImGui::TextColored(tc, "%s%.0f\xC2\xB0%s", anyCpuSteam ? " " : "", dispTemp,
                                g_Config.useFahrenheit ? "F" : "C");
                            anyCpuSteam = true;
                        }
                        else if (!g_Config.showCpuUsage) {
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1.f), "---");
                            anyCpuSteam = true;
                        }
                    }
                    if (wantCpuHzSt) {
                        if (anyCpuSteam) {
                            ImGui::SameLine(0, hsTight);
                            ImGui::TextColored(sepC, "|");
                            ImGui::SameLine(0, hsTight);
                        }
                        InlineFreqSparkMHz("##st_cpu", g_cpuSpark, g_cpuSparkN, g_cpuClockMHz,
                            ImVec2(38.f * ss, 11.f * ss), 5.f * ss, ImVec4(.75f, .75f, .78f, 1.f));
                    }
                    needSep = true;
                }

                const bool wantGpuHzSt = g_Config.showGpuCoreFreq && g_Config.gpuCoreFreqPath[0] && g_lhwmAvailable;
                if (g_Config.showGpuUsage || g_Config.showGpuTemp || wantGpuHzSt) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    float dispGpuLoad = g_gpuUsage;
                    float dispGpuTemp = g_gpuTemp;
                    bool hasGpuData = g_lhwmAvailable && g_gpuCount > 0;
                    ImGui::TextColored(labGpu, "GPU");
                    ImGui::SameLine(0, hsTight);
                    if (hasGpuData) {
                        bool anyGpuSteam = false;
                        if (g_Config.showGpuUsage) {
                            ImGui::TextColored(ColorByLoad(dispGpuLoad), "%.0f%%", dispGpuLoad);
                            anyGpuSteam = true;
                        }
                        if (g_Config.showGpuTemp) {
                            if (dispGpuTemp > 0) {
                                if (anyGpuSteam) ImGui::SameLine(0, 2.f * ss);
                                float dispTemp = ToDisplayTemp(dispGpuTemp, g_Config.useFahrenheit);
                                ImVec4 tc = dispGpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                    : dispGpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                    : ImVec4(.70f, .70f, .75f, 1);
                                ImGui::TextColored(tc, "%s%.0f\xC2\xB0%s", anyGpuSteam ? " " : "", dispTemp,
                                    g_Config.useFahrenheit ? "F" : "C");
                                anyGpuSteam = true;
                            }
                            else if (!g_Config.showGpuUsage) {
                                ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1.f), "---");
                                anyGpuSteam = true;
                            }
                        }
                        if (wantGpuHzSt) {
                            if (anyGpuSteam) {
                                ImGui::SameLine(0, hsTight);
                                ImGui::TextColored(sepC, "|");
                                ImGui::SameLine(0, hsTight);
                            }
                            InlineFreqSparkMHz("##st_gpu", g_gpuSpark, g_gpuSparkN, g_gpuCoreClockMHz,
                                ImVec2(38.f * ss, 11.f * ss), 5.f * ss, ImVec4(.75f, .75f, .78f, 1.f));
                        }
                    }
                    else {
                        ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1.f), "N/D");
                    }
                    needSep = true;
                }

                if (g_Config.showVRAM) {
                    float dispVramUsed = g_vramUsed;
                    float dispVramTotal = g_vramTotal;
                    if (dispVramTotal > 0.f) {
                        if (needSep) {
                            ImGui::SameLine(0, hs);
                            ImGui::TextColored(sepC, "|");
                            ImGui::SameLine(0, hs);
                        }
                        float vramPct = (dispVramUsed / dispVramTotal) * 100.0f;
                        ImGui::TextColored(ColorByLoad(vramPct), "VRAM %.0f%% %.1f/%.0fG", vramPct, dispVramUsed, dispVramTotal);
                        needSep = true;
                    }
                }

                if (g_Config.showRAM) {
                    if (needSep) {
                        ImGui::SameLine(0, hs);
                        ImGui::TextColored(sepC, "|");
                        ImGui::SameLine(0, hs);
                    }
                    float pct = (ramUsed / ramTotal) * 100;
                    ImGui::TextColored(ColorByLoad(pct), "RAM %.0f%% %.1f/%.0fG", pct, ramUsed, ramTotal);
                }

                if (showProcLine) {
                    ImGui::SetWindowFontScale(0.78f * ss);
                    ImGui::TextColored(ImVec4(.42f, .52f, .42f, 1.f), "%s", g_targetProcessName);
                }

                ImGui::SetWindowFontScale(1.0f);
            }
            // ═══════════════════════════════════════════════════════════
            // VISUALIZAÇÃO HORIZONTAL COMPACTA
            // ═══════════════════════════════════════════════════════════
            else if (g_Config.layoutStyle == LAYOUT_HORIZONTAL) {
                ImGui::SetWindowFontScale(ovSc);
                bool needSep = false;

                // FPS
                if (g_Config.showFPS) {
                    if (g_etwAvailable && gameFps > 0) {
                        ImVec4 col = gameFps >= 60 ? ImVec4(.18f, .94f, .45f, 1)
                            : gameFps >= 30 ? ImVec4(1, .85f, .15f, 1)
                            : ImVec4(1, .25f, .25f, 1);
                        ImGui::TextColored(col, "FPS %.0f", gameFps);
                    }
                    else {
                        ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "FPS ---");
                    }
                    needSep = true;
                }

                // CPU
                {
                    const bool wantCpuHzHz =
                        g_Config.showCpuFreq && g_Config.cpuFreqPath[0] && g_lhwmAvailable;
                    if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzHz) {
                        if (needSep) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | ");
                            ImGui::SameLine();
                        }
                        const bool hasCpuTempVal = g_cpuTempAvailable && g_cpuTemp > 0;
                        if (g_Config.showCpuUsage)
                            ImGui::TextColored(ColorByLoad(cpuUsage), "CPU %.0f%%", cpuUsage);
                        else if (g_Config.showCpuTemp || wantCpuHzHz)
                            ImGui::TextColored(ImVec4(.78f, .78f, .82f, 1), "CPU");

                        if (g_Config.showCpuTemp && hasCpuTempVal) {
                            float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                            ImVec4 tc = g_cpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                : g_cpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                : ImVec4(.70f, .70f, .75f, 1);
                            if (g_Config.showCpuUsage) ImGui::SameLine(0, 2);
                            else ImGui::SameLine(0, 4);
                            ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp,
                                g_Config.useFahrenheit ? "F" : "C");
                        }
                        else if (g_Config.showCpuTemp && !g_Config.showCpuUsage) {
                            ImGui::SameLine(0, 2);
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "---");
                        }

                        if (wantCpuHzHz) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | ");
                            ImGui::SameLine();
                            InlineFreqSparkMHz("##hz_cpu", g_cpuSpark, g_cpuSparkN, g_cpuClockMHz,
                                ImVec2(52.f * ovSc, 12.f * ovSc), 6.f * ovSc,
                                ImVec4(.62f, .62f, .68f, 1.f));
                        }
                        needSep = true;
                    }
                }

                // Estatísticas da GPU via LHWM
                {
                    const bool wantGpuHzHz =
                        g_Config.showGpuCoreFreq && g_Config.gpuCoreFreqPath[0] && g_lhwmAvailable;
                    if (g_Config.showGpuUsage || g_Config.showGpuTemp || wantGpuHzHz) {
                        if (needSep) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | ");
                            ImGui::SameLine();
                        }

                        float dispGpuLoad = g_gpuUsage;
                        float dispGpuTemp = g_gpuTemp;
                        bool hasGpuData = g_lhwmAvailable && g_gpuCount > 0;

                        if (hasGpuData) {
                            if (g_Config.showGpuUsage)
                                ImGui::TextColored(ColorByLoad(dispGpuLoad), "GPU %.0f%%", dispGpuLoad);
                            else if (g_Config.showGpuTemp || wantGpuHzHz)
                                ImGui::TextColored(ImVec4(.78f, .78f, .82f, 1), "GPU");

                            if (g_Config.showGpuTemp && dispGpuTemp > 0) {
                                float dispTemp = ToDisplayTemp(dispGpuTemp, g_Config.useFahrenheit);
                                ImVec4 tc = dispGpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                    : dispGpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                    : ImVec4(.70f, .70f, .75f, 1);
                                if (g_Config.showGpuUsage) ImGui::SameLine(0, 2);
                                else ImGui::SameLine(0, 4);
                                ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp,
                                    g_Config.useFahrenheit ? "F" : "C");
                            }
                            else if (g_Config.showGpuTemp && !g_Config.showGpuUsage) {
                                ImGui::SameLine(0, 2);
                                ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "---");
                            }

                            if (wantGpuHzHz) {
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | ");
                                ImGui::SameLine();
                                InlineFreqSparkMHz("##hz_gpu", g_gpuSpark, g_gpuSparkN, g_gpuCoreClockMHz,
                                    ImVec2(52.f * ovSc, 12.f * ovSc), 6.f * ovSc,
                                    ImVec4(.62f, .62f, .68f, 1.f));
                            }
                        }
                        else {
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "GPU N/D");
                        }
                        needSep = true;
                    }
                }

                // VRAM
                if (g_Config.showVRAM) {
                    float dispVramUsed = g_vramUsed;
                    float dispVramTotal = g_vramTotal;
                    if (dispVramTotal > 0) {
                        if (needSep) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | "); ImGui::SameLine(); }
                        float vramPct = (dispVramUsed / dispVramTotal) * 100.0f;
                        ImGui::TextColored(ColorByLoad(vramPct), "VRAM %.0f%% %.1f/%.0fG", vramPct, dispVramUsed, dispVramTotal);
                        needSep = true;
                    }
                }

                // RAM
                if (g_Config.showRAM) {
                    if (needSep) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.35f, .35f, .40f, 1), " | "); ImGui::SameLine(); }
                    float pct = (ramUsed / ramTotal) * 100;
                    ImGui::TextColored(ColorByLoad(pct), "RAM %.0f%% %.1f/%.0fG", pct, ramUsed, ramTotal);
                }

                // Nome do processo na segunda linha (compacto)
                if (g_Config.showFPS && g_Config.showProcessName && g_targetProcessName[0]) {
                    ImGui::SetWindowFontScale(0.78f * ovSc);
                    ImGui::TextColored(ImVec4(.42f, .52f, .42f, 1), "%s", g_targetProcessName);
                    ImGui::SetWindowFontScale(ovSc);
                }
                ImGui::SetWindowFontScale(1.0f);
            }
            // ═══════════════════════════════════════════════════════════
            // VISUALIZAÇÃO VERTICAL (padrão)
            // ═══════════════════════════════════════════════════════════
            else {
                ImGui::SetWindowFontScale(ovSc);
                bool needSep = false;

                // FPS
                if (g_Config.showFPS) {
                    if (g_etwAvailable && gameFps > 0) {
                        ImVec4 col = gameFps >= 60 ? ImVec4(.18f, .94f, .45f, 1)
                            : gameFps >= 30 ? ImVec4(1, .85f, .15f, 1)
                            : ImVec4(1, .25f, .25f, 1);
                        ImGui::TextColored(col, "FPS  %.0f", gameFps);
                    }
                    else {
                        ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "FPS  ---");
                    }
                    // Mostra nome do processo rastreado
                    if (g_Config.showProcessName) {
                        if (g_targetProcessName[0]) {
                            ImGui::SetWindowFontScale(0.82f * ovSc);
                            ImGui::TextColored(ImVec4(.42f, .55f, .42f, 1), "  %s", g_targetProcessName);
                            ImGui::SetWindowFontScale(ovSc);
                        }
                        else {
                            ImGui::SetWindowFontScale(0.82f * ovSc);
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "  (nenhum processo)");
                            ImGui::SetWindowFontScale(ovSc);
                        }
                    }
                    needSep = true;
                }

                // CPU
                {
                    const bool wantCpuHzV =
                        g_Config.showCpuFreq && g_Config.cpuFreqPath[0] && g_lhwmAvailable;
                    if (g_Config.showCpuUsage || g_Config.showCpuTemp || wantCpuHzV) {
                        if (needSep) {
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                        }
                        const bool hasCpuTempVal = g_cpuTempAvailable && g_cpuTemp > 0;
                        if (g_Config.showCpuUsage)
                            ImGui::TextColored(ColorByLoad(cpuUsage), "CPU  %.0f%%", cpuUsage);
                        if (g_Config.showCpuTemp && hasCpuTempVal) {
                            float dispTemp = ToDisplayTemp(g_cpuTemp, g_Config.useFahrenheit);
                            ImVec4 tc = g_cpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                : g_cpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                : ImVec4(.70f, .70f, .75f, 1);
                            if (g_Config.showCpuUsage) ImGui::SameLine();
                            else ImGui::TextColored(ImVec4(.82f, .82f, .88f, 1), "CPU  ");
                            if (!g_Config.showCpuUsage) ImGui::SameLine(0, 0);
                            ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp,
                                g_Config.useFahrenheit ? "F" : "C");
                        }
                        else if (g_Config.showCpuTemp && !g_Config.showCpuUsage) {
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "CPU  ---");
                        }

                        ImGui::SetWindowFontScale(0.82f * ovSc);
                        ImGui::TextColored(ImVec4(.42f, .42f, .48f, 1), "  %s", g_cpuName);
                        ImGui::SetWindowFontScale(ovSc);
                        if (wantCpuHzV) {
                            ImGui::Dummy(ImVec2(0, 3.f * ovSc));
                            ImGui::TextColored(ImVec4(.48f, .58f, .65f, 1), "CPU MHz");
                            ImGui::SameLine();
                            DrawMiniSpark("##vsp_cpu", g_cpuSpark, g_cpuSparkN, g_cpuClockMHz,
                                ImVec2(130.f * ovSc, 24.f * ovSc));
                        }
                        needSep = true;
                    }
                }

                // Estatísticas da GPU via LHWM
                {
                    const bool wantGpuHzV =
                        g_Config.showGpuCoreFreq && g_Config.gpuCoreFreqPath[0] && g_lhwmAvailable;
                    const bool gpuVertBlock = g_Config.showGpuUsage || g_Config.showGpuTemp || wantGpuHzV ||
                        (g_Config.showVRAM && g_lhwmAvailable && g_gpuCount > 0);
                    if (gpuVertBlock) {
                        if (needSep) {
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                        }

                        float dispGpuLoad = g_gpuUsage;
                        float dispGpuTemp = g_gpuTemp;
                        float dispVramUsed = g_vramUsed;
                        float dispVramTotal = g_vramTotal;
                        bool hasGpuData = g_lhwmAvailable && g_gpuCount > 0;

                        if (hasGpuData) {
                            if (g_Config.showGpuUsage)
                                ImGui::TextColored(ColorByLoad(dispGpuLoad), "GPU  %.0f%%", dispGpuLoad);
                            if (g_Config.showGpuTemp && dispGpuTemp > 0) {
                                float dispTemp = ToDisplayTemp(dispGpuTemp, g_Config.useFahrenheit);
                                ImVec4 tc = dispGpuTemp > 85 ? ImVec4(1, .3f, .3f, 1)
                                    : dispGpuTemp > 70 ? ImVec4(1, .85f, .15f, 1)
                                    : ImVec4(.70f, .70f, .75f, 1);
                                if (g_Config.showGpuUsage) ImGui::SameLine();
                                else ImGui::TextColored(ImVec4(.82f, .82f, .88f, 1), "GPU  ");
                                if (!g_Config.showGpuUsage) ImGui::SameLine(0, 0);
                                ImGui::TextColored(tc, "%.0f\xC2\xB0%s", dispTemp,
                                    g_Config.useFahrenheit ? "F" : "C");
                            }
                            else if (g_Config.showGpuTemp && !g_Config.showGpuUsage) {
                                ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "GPU  ---");
                            }
                            // Uso de VRAM
                            if (g_Config.showVRAM && dispVramTotal > 0) {
                                float vramPct = (dispVramUsed / dispVramTotal) * 100.0f;
                                ImGui::TextColored(ColorByLoad(vramPct), "VRAM %.0f%%", vramPct);
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(.70f, .70f, .75f, 1), " %.1f / %.0f GB", dispVramUsed,
                                    dispVramTotal);
                            }
                        }
                        else {
                            ImGui::TextColored(ImVec4(.50f, .50f, .55f, 1), "GPU  N/D");
                        }
                        ImGui::SetWindowFontScale(0.82f * ovSc);
                        ImGui::TextColored(ImVec4(.42f, .42f, .48f, 1), "  %s", g_gpuName);
                        ImGui::SetWindowFontScale(ovSc);
                        if (wantGpuHzV && hasGpuData) {
                            ImGui::Dummy(ImVec2(0, 3.f * ovSc));
                            ImGui::TextColored(ImVec4(.48f, .58f, .65f, 1), "GPU MHz");
                            ImGui::SameLine();
                            DrawMiniSpark("##vsp_gpu", g_gpuSpark, g_gpuSparkN, g_gpuCoreClockMHz,
                                ImVec2(130.f * ovSc, 24.f * ovSc));
                        }
                        needSep = true;
                    }
                }

                // RAM
                if (g_Config.showRAM) {
                    if (needSep) { ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing(); }
                    float pct = (ramUsed / ramTotal) * 100;
                    ImGui::TextColored(ColorByLoad(pct), "RAM  %.0f%%", pct);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(.70f, .70f, .75f, 1), " %.1f / %.1f GB", ramUsed, ramTotal);
                }
                ImGui::SetWindowFontScale(1.0f);
            }

            // ── Mostra texto auxiliar quando CTRL está pressionado ──
            if (ctrlHeld) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                float helpSc = 0.85f * ovSc;
                ImGui::SetWindowFontScale(helpSc);
                ImGui::TextColored(ImVec4(0.5f, 0.75f, 1.0f, 1.0f), "Arraste para mover | Clique direito para menu");
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

    // ═══ Limpeza ═══
    SaveConfig(g_Config);  // Salva Config antes de sair
    StopEtwSession();
    if (g_Mode == MODE_OVERLAY) RemoveTrayIcon();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClass("FPSeco", g_hInstance);
    ShutdownWMI();

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Procedimento da janela
// ═══════════════════════════════════════════════════════════════════════════
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
        // ─────────────────────────────────────────────────────────────────────
        // Desenha uma borda preta sólida (substitui a borda branca padrão)
        // ─────────────────────────────────────────────────────────────────────
    case WM_NCPAINT:
    {
        RECT rc;
        GetWindowRect(hWnd, &rc);
        HDC hdc = GetWindowDC(hWnd);
        if (hdc) {
            HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));      // borda preta, 2px
            HBRUSH hBrush = (HBRUSH)GetStockObject(NULL_BRUSH);   // sem preenchimento
            SelectObject(hdc, hPen);
            SelectObject(hdc, hBrush);
            Rectangle(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top);
            DeleteObject(hPen);
            ReleaseDC(hWnd, hdc);
        }
        // Deixa o Windows desenhar o restante (barra de título, ícones, etc.)
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_CLOSE:
        if (g_Mode == MODE_CONFIG) g_Running = false;
        return 0;
    case WM_DESTROY:
        return 0;
        // Tamanho da Janela (Inicial)    
    case WM_GETMINMAXINFO:
        if (g_Mode == MODE_CONFIG && lParam) {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = 500;   // largura mínima
            mmi->ptMaxTrackSize.x = 900;   // largura máxima
            mmi->ptMinTrackSize.y = kConfigDlgMinOuterH;
        }
        return 0;
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            if (g_updateAvailable) {
                char updateText[64];
                snprintf(updateText, sizeof(updateText), "Baixar Update (%s)", g_latestVersion);
                AppendMenu(m, MF_STRING, IDM_UPDATE, updateText);
                AppendMenu(m, MF_SEPARATOR, 0, nullptr);
            }
            if (g_OvlVisible)
                AppendMenu(m, MF_STRING, IDM_HIDE, "Ocultar Overlay");
            else
                AppendMenu(m, MF_STRING, IDM_SHOW, "Mostrar Overlay");
            AppendMenu(m, MF_SEPARATOR, 0, nullptr);
            AppendMenu(m, MF_STRING, IDM_SETTINGS, "Config");
            AppendMenu(m, MF_SEPARATOR, 0, nullptr);
            AppendMenu(m, MF_STRING, IDM_EXIT, "Sair");
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(m);
            switch (cmd) {
            case IDM_UPDATE:
                ShellExecuteA(nullptr, "open",
                    "https://github.com/zRafaX/FPSeco/releases/latest",
                    nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case IDM_SHOW:     g_OvlVisible = true;            break;
            case IDM_HIDE:     g_OvlVisible = false;           break;
            case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS;  break;
            case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
            }
        }
        return 0;
    case WM_CONTEXTMENU:
        if (g_Mode == MODE_OVERLAY) {
            bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            POINT pt; GetCursorPos(&pt);
            if (ctrlHeld && PtInRect(&g_overlayBounds, pt)) {
                HMENU m = CreatePopupMenu();
                AppendMenu(m, MF_STRING, IDM_HIDE, "Ocultar Overlay");
                AppendMenu(m, MF_STRING, IDM_RESET_POS, "Redefinir Posição");
                AppendMenu(m, MF_SEPARATOR, 0, nullptr);
                AppendMenu(m, MF_STRING, IDM_SETTINGS, "Config");
                AppendMenu(m, MF_STRING, IDM_EXIT, "Sair");
                SetForegroundWindow(hWnd);
                int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                    pt.x, pt.y, 0, hWnd, nullptr);
                DestroyMenu(m);
                switch (cmd) {
                case IDM_HIDE:       g_OvlVisible = false;           break;
                case IDM_RESET_POS:
                    g_Config.customX = -1.f;
                    g_Config.customY = -1.f;
                    g_overlayForceCornerSnap = true;
                    SaveConfig(g_Config);
                    break;
                case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS;  break;
                case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
                }
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SHOW:     g_OvlVisible = true;            break;
        case IDM_HIDE:     g_OvlVisible = false;           break;
        case IDM_SETTINGS: g_Pending = CMD_SHOW_SETTINGS;  break;
        case IDM_EXIT:     g_Pending = CMD_EXIT;           break;
        }
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}