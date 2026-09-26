#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <string>
#include <vector>
#include <sstream>
#include <cmath>
#include <unordered_set>
#include <algorithm>
#include <tlhelp32.h>

#include "resource.h"
#include "isolator.h"
#include "process_picker.h"
#include "priority_matrix_picker.h"
#include "priority_matrix_data.h"
#include "dpi_utils.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

using ati::GetDpiScaleForWindow;
using ati::ScaleI;
using ati::GetEffectiveFont;
namespace Layout = ati::Layout;

#define WM_TRAYICON_MSG (WM_APP + 1)
#define TIMER_POLLING_ID 1001

static HINSTANCE g_hInstance = nullptr;
static HWND g_hMainDlg = nullptr;
static NOTIFYICONDATAA g_nid = { 0 };
static ati::ThreadIsolator g_isolator;
static std::string g_iniPath;
static bool g_startInTray = false;
static bool g_alwaysOnTop = false;
static bool s_pulseTick = false;
static HANDLE g_hSingleInstanceMutex = nullptr;

// ListView 列幅定義 (DPI 基準値: scale=1.0 時のピクセル幅)
// Col: 0:!  1:ProcessName  2:Thread Priority  3:Process Priority  4:||  5:PID/TID  6:TIDs  7:Chgs  8:Audio  9:Excluded
// 10列最適化値 (16, 137, 135, 110, 20, 139, 35, 67, 44, 59) ➔ 合計 762px (scale=1.0 基準値)
static const int kColDefaultWidths[] = { 16, 137, 135, 110, 20, 139, 35, 67, 44, 59 };
static const int kColCount = sizeof(kColDefaultWidths) / sizeof(kColDefaultWidths[0]);
static std::vector<int> g_colWidths = { 16, 137, 135, 110, 20, 139, 35, 67, 44, 59 };
static int g_windowHeight = 0;

#ifdef _DEBUG
void LogDebug(const char* msg) {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecA(exePath);
    PathAppendA(exePath, "debug.log");
    FILE* fp = fopen(exePath, "a");
    if (fp) {
        fprintf(fp, "%s\n", msg);
        fclose(fp);
    }
}
#else
void LogDebug(const char*) {}
#endif

// --- 高DPI (Per-Monitor V2) 初期化・スケーリングヘルパー ---
static void InitializeHighDpi() {
    // 1. Windows 10 1703+ : SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
        PFN_SetProcessDpiAwarenessContext pfnSetContext = 
            (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pfnSetContext) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((DPI_AWARENESS_CONTEXT)-4)
            if (pfnSetContext((DPI_AWARENESS_CONTEXT)-4)) {
                return;
            }
        }
    }

    // 2. Windows 8.1 / 10 初期 : SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)
    HMODULE hShcore = LoadLibraryA("shcore.dll");
    if (hShcore) {
        typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
        PFN_SetProcessDpiAwareness pfnSetAwareness = 
            (PFN_SetProcessDpiAwareness)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (pfnSetAwareness) {
            // PROCESS_PER_MONITOR_DPI_AWARE = 2
            if (SUCCEEDED(pfnSetAwareness(2))) {
                FreeLibrary(hShcore);
                return;
            }
        }
        FreeLibrary(hShcore);
    }

    // 3. Windows Vista+ レガシーフォールバック
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_SetProcessDPIAware)();
        PFN_SetProcessDPIAware pfnSetAware = 
            (PFN_SetProcessDPIAware)GetProcAddress(hUser32, "SetProcessDPIAware");
        if (pfnSetAware) {
            pfnSetAware();
        }
    }
}

// --- スタートアップ (Runキー) 管理 ---
static const char* STARTUP_REG_KEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char* STARTUP_VALUE_NAME = "AudioThreadIsolator";

static bool IsStartupEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD size = 0;
        LONG res = RegQueryValueExA(hKey, STARTUP_VALUE_NAME, nullptr, &type, nullptr, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS && type == REG_SZ);
    }
    return false;
}

static void SetStartupEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            char exePath[MAX_PATH] = { 0 };
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            std::string cmd = "\"" + std::string(exePath) + "\" --tray";
            RegSetValueExA(
                hKey, STARTUP_VALUE_NAME, 0, REG_SZ, 
                reinterpret_cast<const BYTE*>(cmd.c_str()), 
                static_cast<DWORD>(cmd.length() + 1)
            );
        } else {
            RegDeleteValueA(hKey, STARTUP_VALUE_NAME);
        }
        RegCloseKey(hKey);
    }
}

// --- INI パス取得 ---
static std::string GetIniFilePath() {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecA(exePath);
    PathAppendA(exePath, "ATI.ini");
    return std::string(exePath);
}

// --- 優先度定義ヘルパー (ANSI) ---
struct PriorityOption {
    int value;
    const char* label;
};

static const PriorityOption PRIO_OPTIONS[] = {
    { THREAD_PRIORITY_IDLE,          "Idle (-15)" },
    { THREAD_PRIORITY_LOWEST,        "Lowest (-2)" },
    { THREAD_PRIORITY_BELOW_NORMAL,  "Below Normal (-1)" },
    { THREAD_PRIORITY_NORMAL,        "Normal (0)" },
    { THREAD_PRIORITY_ABOVE_NORMAL,  "Above Normal (+1)" },
    { THREAD_PRIORITY_HIGHEST,       "Highest (+2)" },
    { THREAD_PRIORITY_TIME_CRITICAL, "Time Critical (+15)" }
};

static const int PRIO_OPTIONS_COUNT = sizeof(PRIO_OPTIONS) / sizeof(PRIO_OPTIONS[0]);

static std::string GetPriorityString(int prio) {
    for (int i = 0; i < PRIO_OPTIONS_COUNT; ++i) {
        if (PRIO_OPTIONS[i].value == prio) return PRIO_OPTIONS[i].label;
    }
    return std::to_string(prio);
}

static std::string GetProcessPriorityString(DWORD pClass) {
    switch (pClass) {
    case REALTIME_PRIORITY_CLASS: return "Realtime";
    case HIGH_PRIORITY_CLASS: return "High";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "Above Normal";
    case NORMAL_PRIORITY_CLASS: return "Normal";
    case BELOW_NORMAL_PRIORITY_CLASS: return "Below Normal";
    case IDLE_PRIORITY_CLASS: return "Idle";
    case 0: default: return "(Default)";
    }
}

// --- コア番号・マスク変換ヘルパー (ANSI) ---
static std::string FormatMaskToCoreList(DWORD_PTR mask) {
    if (mask == 0) return "0";
    std::string res;
    for (int c = 0; c < 64; ++c) {
        if ((mask & (1ULL << c)) != 0) {
            if (!res.empty()) res += ", ";
            res += std::to_string(c);
        }
    }
    return res.empty() ? "0" : res;
}

static DWORD_PTR ParseCoreListToMask(const std::string& str, int& outFirstCore) {
    DWORD_PTR mask = 0;
    outFirstCore = -1;
    if (str.empty()) return 0;

    // 0x で始まる 16進マスク表記にも対応
    if (str.length() > 2 && (str.substr(0, 2) == "0x" || str.substr(0, 2) == "0X")) {
        mask = _strtoui64(str.c_str(), nullptr, 16);
        for (int c = 0; c < 64; ++c) {
            if ((mask & (1ULL << c)) != 0) {
                outFirstCore = c;
                break;
            }
        }
        if (outFirstCore < 0) outFirstCore = 0;
        return mask;
    }

    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && isspace(static_cast<unsigned char>(token.front()))) token.erase(token.begin());
        while (!token.empty() && isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
        if (token.empty()) continue;

        int coreIdx = atoi(token.c_str());
        if (coreIdx >= 0 && coreIdx < 64) {
            mask |= (1ULL << coreIdx);
            if (outFirstCore < 0) {
                outFirstCore = coreIdx;
            }
        }
    }
    if (outFirstCore < 0) outFirstCore = 0;
    return mask;
}

static std::string FormatMaskToCoreLabels(DWORD_PTR mask, const std::vector<ati::CoreInfo>& cores) {
    if (mask == 0) return "-";
    std::vector<ati::CoreInfo> matched;
    for (const auto& c : cores) {
        if ((mask & (1ULL << c.logicalIndex)) != 0) {
            matched.push_back(c);
        }
    }
    if (matched.empty()) return "-";

    // プレフィックスの共通性を確認 (例: "#1", "#2" -> prefix="#", nums=[1, 2])
    std::string firstPrefix;
    std::string firstLabel = matched[0].label;
    size_t numStart = firstLabel.find_first_of("0123456789");
    if (numStart != std::string::npos) {
        firstPrefix = firstLabel.substr(0, numStart);
    }

    bool allSamePrefix = true;
    for (const auto& c : matched) {
        size_t ns = c.label.find_first_of("0123456789");
        std::string p = (ns != std::string::npos) ? c.label.substr(0, ns) : "";
        if (p != firstPrefix) {
            allSamePrefix = false;
            break;
        }
    }

    if (allSamePrefix && !firstPrefix.empty()) {
        std::string res = firstPrefix;
        for (size_t i = 0; i < matched.size(); ++i) {
            if (i > 0) res += ",";
            size_t ns = matched[i].label.find_first_of("0123456789");
            res += matched[i].label.substr(ns);
        }
        return res;
    }

    std::string res;
    for (size_t i = 0; i < matched.size(); ++i) {
        if (i > 0) res += ",";
        res += matched[i].label;
    }
    return res;
}

static std::string FormatAudioMaskDisplay(DWORD_PTR mask, const std::vector<ati::CoreInfo>& cores) {
    if (mask == 0) return "-";
    int bits = 0;
    for (int c = 0; c < 64; ++c) {
        if ((mask & (1ULL << c)) != 0) bits++;
    }
    if (bits > 4) {
        char hexBuf[32] = { 0 };
        snprintf(hexBuf, sizeof(hexBuf), "0x%llX", static_cast<unsigned long long>(mask));
        return hexBuf;
    }
    return FormatMaskToCoreLabels(mask, cores);
}

static std::string FormatOtherThanLabels(bool isRunning, bool isBypassed, bool isAudioIsolated, DWORD_PTR audioMask, DWORD_PTR normalMask, int coreCount, const std::vector<ati::CoreInfo>& cores) {
    if (!isRunning || isBypassed) {
        return "-";
    }
    DWORD_PTR fullMask = ati::ThreadIsolator::GetFullCoreMask(coreCount);
    DWORD_PTR effectiveNormal = normalMask ? normalMask : fullMask;
    if (isAudioIsolated) {
        effectiveNormal &= ~audioMask;
    }
    DWORD_PTR excludedMask = fullMask & ~effectiveNormal;
    if (excludedMask == 0) {
        return "All";
    }
    int exclBits = 0;
    for (int c = 0; c < 64; ++c) {
        if ((excludedMask & (1ULL << c)) != 0) exclBits++;
    }
    // 除外コア数が 4 個以下の場合は除外コアを列挙 (例: !#1, !#1,5)
    if (exclBits <= 4) {
        std::string exclStr = FormatMaskToCoreLabels(excludedMask, cores);
        if (exclStr == "-" || exclStr.empty()) return "All";
        return "!" + exclStr;
    }
    // 除外コアが多数 (5個以上) の場合は、通常コア有効マスクを HEX 表記 (例: 0x1D)
    char hexBuf[32] = { 0 };
    snprintf(hexBuf, sizeof(hexBuf), "0x%llX", static_cast<unsigned long long>(effectiveNormal));
    return hexBuf;
}

// --- 設定の読み書き (ANSI) ---
static void LoadConfig(ati::GlobalConfig& config, const std::string& iniPath) {
    int coreCount = ati::ThreadIsolator::GetSystemCoreCount();
    int defaultCore = coreCount > 1 ? 1 : 0;

    char defaultAudioBuf[128] = { 0 };
    GetPrivateProfileStringA(
        "Global", "DefaultAudioCore", std::to_string(defaultCore).c_str(), defaultAudioBuf, sizeof(defaultAudioBuf), iniPath.c_str()
    );
    int firstCore = defaultCore;
    config.defaultAudioAffinityMask = ParseCoreListToMask(defaultAudioBuf, firstCore);
    if (config.defaultAudioAffinityMask == 0) {
        config.defaultAudioAffinityMask = ati::ThreadIsolator::MakeCoreMask(defaultCore);
        firstCore = defaultCore;
    }
    config.defaultAudioCore = firstCore;

    config.defaultAudioPriority = GetPrivateProfileIntA(
        "Global", "DefaultAudioPriority", THREAD_PRIORITY_IDLE, iniPath.c_str()
    );

    char normalMaskBuf[64] = { 0 };
    GetPrivateProfileStringA(
        "Global", "NormalCores", "0", normalMaskBuf, 64, iniPath.c_str()
    );
    config.normalAffinityMask = _strtoui64(normalMaskBuf, nullptr, 16);
    if (config.normalAffinityMask == 0 || (config.normalAffinityMask & config.defaultAudioAffinityMask) != 0) {
        DWORD_PTR full = ati::ThreadIsolator::GetFullCoreMask(coreCount);
        config.normalAffinityMask = full & ~config.defaultAudioAffinityMask;
        if (config.normalAffinityMask == 0) config.normalAffinityMask = full;
    }

    config.pollingIntervalMs = GetPrivateProfileIntA(
        "Global", "PollingIntervalMs", 500, iniPath.c_str()
    );
    config.pollingIntervalMs = ati::ThreadIsolator::NormalizePollingInterval(config.pollingIntervalMs);

    config.boostPollingIntervalMs = GetPrivateProfileIntA(
        "Global", "BoostPollingIntervalMs", 250, iniPath.c_str()
    );
    if (config.boostPollingIntervalMs <= 0) config.boostPollingIntervalMs = 250;

    char deltaBuf[64] = { 0 };
    GetPrivateProfileStringA("Global", "DefaultCyclesDelta", "", deltaBuf, sizeof(deltaBuf), iniPath.c_str());
    config.rawDefaultCyclesDeltaStr = deltaBuf;
    if (deltaBuf[0] != '\0') {
        double dVal = atof(deltaBuf);
        dVal = std::ceil(dVal * 10.0) / 10.0;
        config.defaultCyclesDelta = (dVal > 0.0) ? dVal : 5.0;
    } else {
        config.defaultCyclesDelta = 5.0;
    }

    g_alwaysOnTop = (GetPrivateProfileIntA("Global", "AlwaysOnTop", 0, iniPath.c_str()) != 0);
    config.enableHeuristics = true;

    // ColumnWidths (または ColumnWidth) の読み込み
    char colBuf[256] = { 0 };
    GetPrivateProfileStringA("Global", "ColumnWidths", "", colBuf, sizeof(colBuf), iniPath.c_str());
    if (colBuf[0] == '\0') {
        GetPrivateProfileStringA("Global", "ColumnWidth", "", colBuf, sizeof(colBuf), iniPath.c_str());
    }
    config.columnWidths.clear();
    if (colBuf[0] != '\0') {
        std::string s(colBuf);
        // 全角カンマ '，' を半角カンマ ',' に変換
        for (size_t i = 0; i + 1 < s.size(); ) {
            if (static_cast<unsigned char>(s[i]) == 0x81 && static_cast<unsigned char>(s[i+1]) == 0x43) { // Shift-JIS 全角カンマ
                s.replace(i, 2, ",");
            } else if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xEF && static_cast<unsigned char>(s[i+1]) == 0xBC && static_cast<unsigned char>(s[i+2]) == 0x8C) { // UTF-8 全角カンマ
                s.replace(i, 3, ",");
            } else {
                ++i;
            }
        }
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            while (!item.empty() && isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
            while (!item.empty() && isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
            if (!item.empty()) {
                config.columnWidths.push_back(atoi(item.c_str()));
            }
        }
    }
    if (config.columnWidths.size() != kColCount) {
        config.columnWidths.assign(kColDefaultWidths, kColDefaultWidths + kColCount);
    }
    g_colWidths = config.columnWidths;

    // WindowHeight の読み込み
    config.windowHeight = GetPrivateProfileIntA("Global", "WindowHeight", 0, iniPath.c_str());
    g_windowHeight = config.windowHeight;

    config.rules.clear();

    char sectionBuf[4096] = { 0 };
    DWORD chars = GetPrivateProfileSectionA(
        "Processes", sectionBuf, 4096, iniPath.c_str()
    );
    if (chars > 0) {
        char* p = sectionBuf;
        while (*p) {
            std::string line(p);
            size_t eqPos = line.find('=');
            if (eqPos != std::string::npos) {
                std::string procName = line.substr(0, eqPos);
                std::string valStr = line.substr(eqPos + 1);

                while (!procName.empty() && isspace(static_cast<unsigned char>(procName.front()))) procName.erase(procName.begin());
                while (!procName.empty() && isspace(static_cast<unsigned char>(procName.back()))) procName.pop_back();

                ati::ProcessRule rule;
                rule.processName = procName;
                rule.audioCore = config.defaultAudioCore;
                rule.audioAffinityMask = config.defaultAudioAffinityMask;
                rule.audioPriority = config.defaultAudioPriority;
                rule.targetPriority = 0;
                rule.processPriorityClass = 0;
                rule.applyCount = 0;
                rule.currentThreadCount = 0;
                rule.detectedThreadName = "";
                rule.activePid = 0;
                rule.isRunning = false;

                std::stringstream ss(valStr);
                std::string rawToken;
                std::vector<std::pair<std::string, std::string>> kvPairs;
                while (std::getline(ss, rawToken, ',')) {
                    while (!rawToken.empty() && isspace(static_cast<unsigned char>(rawToken.front()))) rawToken.erase(rawToken.begin());
                    while (!rawToken.empty() && isspace(static_cast<unsigned char>(rawToken.back()))) rawToken.pop_back();
                    if (rawToken.empty()) continue;

                    size_t colon = rawToken.find(':');
                    if (colon != std::string::npos) {
                        std::string k = rawToken.substr(0, colon);
                        std::string v = rawToken.substr(colon + 1);
                        while (!k.empty() && isspace(static_cast<unsigned char>(k.front()))) k.erase(k.begin());
                        while (!k.empty() && isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
                        while (!v.empty() && isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
                        while (!v.empty() && isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
                        kvPairs.push_back({k, v});
                    } else {
                        // コロンを含まないトークンは直前のキーの値の続き (例: AudioCore:10, 11 や AudioTid:22220, 24500)
                        if (!kvPairs.empty()) {
                            if (!kvPairs.back().second.empty()) {
                                kvPairs.back().second += ", ";
                            }
                            kvPairs.back().second += rawToken;
                        } else {
                            kvPairs.push_back({rawToken, ""});
                        }
                    }
                }

                for (const auto& kv : kvPairs) {
                    const std::string& k = kv.first;
                    const std::string& v = kv.second;

                    if (k == "IgnoreSig" || k == "IgnoreSigRank") {
                        rule.ignoreSig = true;
                        if (!v.empty()) {
                            int cnt = atoi(v.c_str());
                            rule.ignoreSigCount = (cnt > 0) ? cnt : 1;
                        } else {
                            rule.ignoreSigCount = 1;
                        }
                    } else if (k == "AudioCore" || k == "AudioCores" || k == "AudioCoreMask") {
                        int fc = 0;
                        DWORD_PTR m = ParseCoreListToMask(v, fc);
                        rule.audioCores.clear();
                        std::stringstream ss(v);
                        std::string ctok;
                        while (std::getline(ss, ctok, ',')) {
                            while (!ctok.empty() && isspace(static_cast<unsigned char>(ctok.front()))) ctok.erase(ctok.begin());
                            while (!ctok.empty() && isspace(static_cast<unsigned char>(ctok.back()))) ctok.pop_back();
                            if (!ctok.empty()) {
                                int cVal = atoi(ctok.c_str());
                                if (cVal >= 0 && cVal < 64) rule.audioCores.push_back(cVal);
                            }
                        }
                        if (m != 0) {
                            rule.audioAffinityMask = m;
                            rule.audioCore = fc;
                        } else {
                            rule.audioCore = atoi(v.c_str());
                            rule.audioAffinityMask = ati::ThreadIsolator::MakeCoreMask(rule.audioCore);
                            rule.audioCores.push_back(rule.audioCore);
                        }
                    } else if (k == "AudioPriority" || k == "Priority") {
                        rule.audioPriorities.clear();
                        std::stringstream ss(v);
                        std::string ptok;
                        while (std::getline(ss, ptok, ',')) {
                            while (!ptok.empty() && isspace(static_cast<unsigned char>(ptok.front()))) ptok.erase(ptok.begin());
                            while (!ptok.empty() && isspace(static_cast<unsigned char>(ptok.back()))) ptok.pop_back();
                            if (!ptok.empty()) {
                                rule.audioPriorities.push_back(atoi(ptok.c_str()));
                            }
                        }
                        if (!rule.audioPriorities.empty()) {
                            rule.audioPriority = rule.audioPriorities[0];
                        } else {
                            rule.audioPriority = atoi(v.c_str());
                            rule.audioPriorities.push_back(rule.audioPriority);
                        }
                    } else if (k == "NormalCores" || k == "OtherThanAudio" || k == "NormalMask") {
                            if (v.length() > 2 && (v.substr(0, 2) == "0x" || v.substr(0, 2) == "0X")) {
                                rule.normalAffinityMask = _strtoui64(v.c_str(), nullptr, 16);
                            } else {
                                int fc = 0;
                                rule.normalAffinityMask = ParseCoreListToMask(v, fc);
                            }
                        } else if (k == "TargetPriority") {
                            rule.targetPriority = static_cast<DWORD>(atoi(v.c_str()));
                        } else if (k == "Bypass" || k == "Ignore") {
                            rule.isBypassed = (atoi(v.c_str()) != 0);
                        } else if (k == "AppType" || k == "Normchrom") {
                            rule.appType = atoi(v.c_str());
                        } else if (k == "Delta" || k == "CyclesDelta") {
                            double dVal = atof(v.c_str());
                            dVal = std::ceil(dVal * 10.0) / 10.0;
                            rule.cyclesDelta = (dVal > 0.0) ? dVal : 0.0;
                        } else if (k == "AudioPid") {
                            rule.lastAudioPid = static_cast<DWORD>(atoi(v.c_str()));
                        } else if (k == "AudioTid" || k == "AudioTids") {
                            rule.lastAudioTids.clear();
                            std::stringstream ss(v);
                            std::string ttok;
                            while (std::getline(ss, ttok, ',')) {
                                while (!ttok.empty() && isspace(static_cast<unsigned char>(ttok.front()))) ttok.erase(ttok.begin());
                                while (!ttok.empty() && isspace(static_cast<unsigned char>(ttok.back()))) ttok.pop_back();
                                if (!ttok.empty()) {
                                    DWORD tVal = static_cast<DWORD>(atoi(ttok.c_str()));
                                    if (tVal > 0) rule.lastAudioTids.push_back(tVal);
                                }
                            }
                            rule.lastAudioTid = rule.lastAudioTids.empty() ? 0 : rule.lastAudioTids[0];
                        } else if (k == "Decline") {
                            rule.isDeclineBoost = (atoi(v.c_str()) != 0);
                        } else if (k == "ProcessPriority" || k == "ProcessPriorityClass") {
                            if (v.length() > 2 && (v.substr(0, 2) == "0x" || v.substr(0, 2) == "0X")) {
                                rule.processPriorityClass = static_cast<DWORD>(_strtoui64(v.c_str(), nullptr, 16));
                            } else if (v == "Realtime") {
                                rule.processPriorityClass = REALTIME_PRIORITY_CLASS;
                            } else if (v == "High") {
                                rule.processPriorityClass = HIGH_PRIORITY_CLASS;
                            } else if (v == "Above Normal" || v == "AboveNormal") {
                                rule.processPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
                            } else if (v == "Normal") {
                                rule.processPriorityClass = NORMAL_PRIORITY_CLASS;
                            } else if (v == "Below Normal" || v == "BelowNormal") {
                                rule.processPriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
                            } else if (v == "Idle") {
                                rule.processPriorityClass = IDLE_PRIORITY_CLASS;
                            } else {
                                rule.processPriorityClass = static_cast<DWORD>(atoi(v.c_str()));
                            }
                        }
                    }

                if (rule.isBypassed) {
                    rule.detectedThreadName = "Bypassed";
                }

                config.rules.push_back(rule);
            }
            p += strlen(p) + 1;
        }
    }
}

static void SaveConfig(const ati::GlobalConfig& config, const std::string& iniPath) {
    DWORD_PTR defaultAudioMask = config.defaultAudioAffinityMask 
        ? config.defaultAudioAffinityMask 
        : ati::ThreadIsolator::MakeCoreMask(config.defaultAudioCore);
    WritePrivateProfileStringA(
        "Global", "DefaultAudioCore", 
        FormatMaskToCoreList(defaultAudioMask).c_str(), iniPath.c_str()
    );

    WritePrivateProfileStringA(
        "Global", "DefaultAudioPriority", 
        std::to_string(config.defaultAudioPriority).c_str(), iniPath.c_str()
    );

    char hexBuf[32] = { 0 };
    snprintf(hexBuf, sizeof(hexBuf), "0x%llX", static_cast<unsigned long long>(config.normalAffinityMask));
    WritePrivateProfileStringA(
        "Global", "NormalCores", hexBuf, iniPath.c_str()
    );

    WritePrivateProfileStringA(
        "Global", "PollingIntervalMs", 
        std::to_string(config.pollingIntervalMs).c_str(), iniPath.c_str()
    );

    WritePrivateProfileStringA(
        "Global", "BoostPollingIntervalMs", 
        std::to_string(config.boostPollingIntervalMs > 0 ? config.boostPollingIntervalMs : 250).c_str(), iniPath.c_str()
    );

    WritePrivateProfileStringA(
        "Global", "DefaultCyclesDelta", 
        config.rawDefaultCyclesDeltaStr.c_str(), iniPath.c_str()
    );

    WritePrivateProfileStringA(
        "Global", "AlwaysOnTop", 
        g_alwaysOnTop ? "1" : "0", iniPath.c_str()
    );

    std::string colStr;
    const std::vector<int>& widths = !config.columnWidths.empty() ? config.columnWidths : g_colWidths;
    for (size_t i = 0; i < widths.size(); ++i) {
        if (i > 0) colStr += ", ";
        colStr += std::to_string(widths[i]);
    }
    WritePrivateProfileStringA("Global", "ColumnWidths", colStr.c_str(), iniPath.c_str());

    int wH = config.windowHeight > 0 ? config.windowHeight : g_windowHeight;
    if (wH > 0) {
        WritePrivateProfileStringA("Global", "WindowHeight", std::to_string(wH).c_str(), iniPath.c_str());
    }

    WritePrivateProfileSectionA("Processes", "\0", iniPath.c_str());
    for (const auto& rule : config.rules) {
        DWORD_PTR rMask = rule.audioAffinityMask 
            ? rule.audioAffinityMask 
            : ati::ThreadIsolator::MakeCoreMask(rule.audioCore);
        std::string val;
        if (rule.appType != 0) {
            val += "AppType:" + std::to_string(rule.appType);
        }
        if (rule.cyclesDelta > 0.0) {
            if (!val.empty()) val += ", ";
            char dBuf[32];
            if (std::floor(rule.cyclesDelta) == rule.cyclesDelta) {
                snprintf(dBuf, sizeof(dBuf), "%.0f", rule.cyclesDelta);
            } else {
                snprintf(dBuf, sizeof(dBuf), "%.1f", rule.cyclesDelta);
            }
            val += "Delta:" + std::string(dBuf);
        }
        if (rule.isDeclineBoost) {
            if (!val.empty()) val += ", ";
            val += "Decline:1";
        }
        if (rule.ignoreSig) {
            if (!val.empty()) val += ", ";
            if (rule.ignoreSigCount > 1) {
                val += "IgnoreSig:" + std::to_string(rule.ignoreSigCount);
            } else {
                val += "IgnoreSig";
            }
        }
        if (!val.empty()) val += ", ";
        if (rule.audioCores.size() > 1) {
            std::string coreListStr;
            for (size_t ci = 0; ci < rule.audioCores.size(); ++ci) {
                if (ci > 0) coreListStr += ", ";
                coreListStr += std::to_string(rule.audioCores[ci]);
            }
            val += "AudioCore:" + coreListStr;
        } else {
            val += "AudioCore:" + FormatMaskToCoreList(rMask);
        }
        if (rule.audioPriorities.size() > 1) {
            std::string prioListStr;
            for (size_t pi = 0; pi < rule.audioPriorities.size(); ++pi) {
                if (pi > 0) prioListStr += ", ";
                prioListStr += std::to_string(rule.audioPriorities[pi]);
            }
            val += ", AudioPriority:" + prioListStr;
        } else {
            val += ", AudioPriority:" + std::to_string(rule.audioPriority);
        }
        if (rule.processPriorityClass != 0) {
            val += ", ProcessPriority:" + GetProcessPriorityString(rule.processPriorityClass);
        }
        if (rule.normalAffinityMask != 0) {
            char nHex[32] = { 0 };
            snprintf(nHex, sizeof(nHex), "0x%llX", static_cast<unsigned long long>(rule.normalAffinityMask));
            val += ", NormalCores:" + std::string(nHex);
        }
        if (rule.targetPriority != 0) {
            val += ", TargetPriority:" + std::to_string(rule.targetPriority);
        }
        // 正常隔離中かつ稼働中のプロセスのみ、次回照合用として AudioPid / AudioTid を記録 (未起動やBypass時は消去)
        if (rule.isAudioIsolated && rule.isRunning && !rule.isBypassed && rule.activePid != 0 && (!rule.activeAudioTids.empty() || rule.activeAudioTid != 0)) {
            val += ", AudioPid:" + std::to_string(rule.activePid);
            val += ", AudioTid:";
            if (!rule.activeAudioTids.empty()) {
                for (size_t i = 0; i < rule.activeAudioTids.size(); ++i) {
                    if (i > 0) val += ", ";
                    val += std::to_string(rule.activeAudioTids[i]);
                }
            } else {
                val += std::to_string(rule.activeAudioTid);
            }
        }
        if (rule.isBypassed) {
            val += ", Bypass:1";
        }
        WritePrivateProfileStringA(
            "Processes", rule.processName.c_str(), val.c_str(), iniPath.c_str()
        );
    }
}

// --- ListView ヘルパー (メイン監視画面: 固定9列) ---
static std::string StripExeExtension(const std::string& name) {
    if (name.length() >= 4) {
        std::string ext = name.substr(name.length() - 4);
        if (_stricmp(ext.c_str(), ".exe") == 0) {
            return name.substr(0, name.length() - 4);
        }
    }
    return name;
}

static std::string GetCoreLabel(int coreIdx, const std::vector<ati::CoreInfo>& cores) {
    for (const auto& c : cores) {
        if (c.logicalIndex == coreIdx) return c.label;
    }
    return "#" + std::to_string(coreIdx);
}

struct DisplayRow {
    size_t ruleIndex = 0;
    size_t subIndex = 0;
    DWORD tid = 0;
};
static std::vector<DisplayRow> g_displayRows;

static void RefreshListView(HWND hList, const std::vector<ati::ProcessRule>& rules) {
    g_displayRows.clear();
    ListView_DeleteAllItems(hList);

    auto cores = ati::CpuTopology::GetCpuCores();

    for (size_t i = 0; i < rules.size(); ++i) {
        const auto& r = rules[i];
        size_t rowCount = (r.ignoreSig && r.ignoreSigCount > 1) ? static_cast<size_t>(r.ignoreSigCount) : 1;

        for (size_t s = 0; s < rowCount; ++s) {
            DWORD tid = 0;
            if (s < r.activeAudioTids.size()) {
                tid = r.activeAudioTids[s];
            } else if (s == 0) {
                tid = r.activeAudioTid;
            }

            DisplayRow dr;
            dr.ruleIndex = i;
            dr.subIndex = s;
            dr.tid = tid;
            int itemIdx = static_cast<int>(g_displayRows.size());
            g_displayRows.push_back(dr);

            LVITEMA lvi = { 0 };
            lvi.mask = LVIF_TEXT;
            lvi.iItem = itemIdx;
            lvi.iSubItem = 0;
            lvi.pszText = const_cast<LPSTR>("");
            ListView_InsertItem(hList, &lvi);

            // 1: Process Name (.exe を除いて表示)
            std::string displayName = StripExeExtension(r.processName);
            ListView_SetItemText(hList, itemIdx, 1, const_cast<LPSTR>(displayName.c_str()));

            // 2: Thread Priority
            int rowPrio = (s < r.audioPriorities.size()) ? r.audioPriorities[s] : r.audioPriority;
            std::string prioStr = GetPriorityString(rowPrio);
            ListView_SetItemText(hList, itemIdx, 2, const_cast<LPSTR>(prioStr.c_str()));

            // 3: Process Priority
            std::string procPrioStr = GetProcessPriorityString(r.processPriorityClass);
            ListView_SetItemText(hList, itemIdx, 3, const_cast<LPSTR>(procPrioStr.c_str()));

            // 4: Pause / Verify (カスタムドロー描画、テキストは空)
            ListView_SetItemText(hList, itemIdx, 4, const_cast<LPSTR>(""));

            // 5: Audio Core PID/TID
            std::string thName;
            if (r.isBypassed) {
                thName = "Bypassed";
            } else if (!r.isRunning) {
                thName = "Not running";
            } else if (r.ignoreSig && r.ignoreSigCount > 1) {
                size_t confirmedCount = r.activeAudioTids.size();
                if (s < confirmedCount && tid != 0) {
                    thName = "PID " + std::to_string(r.activePid) + " / TID " + std::to_string(tid);
                } else if (r.multiSearchSleeping) {
                    thName = "sleeping...";
                } else {
                    int intervalMs = (g_isolator.GetConfig().pollingIntervalMs > 0) ? g_isolator.GetConfig().pollingIntervalMs : 1000;
                    int turnsPerSec = 1000 / intervalMs;
                    if (turnsPerSec < 1) turnsPerSec = 1;
                    int sec = (r.multiSearchTurns + turnsPerSec - 1) / turnsPerSec;
                    if (sec < 1) sec = 1;
                    if (sec > 10) sec = 10;
                    if (s == confirmedCount) {
                        thName = "Standby " + std::to_string(sec);
                    } else {
                        thName = "Standby 10";
                    }
                }
            } else if (r.isAudioIsolated && tid != 0) {
                thName = "PID " + std::to_string(r.activePid) + " / TID " + std::to_string(tid);
            } else if (r.detectedThreadName.empty()) {
                thName = r.isRunning ? "Scanning..." : "Not running";
            } else {
                thName = r.detectedThreadName;
            }
            ListView_SetItemText(hList, itemIdx, 5, const_cast<LPSTR>(thName.c_str()));

            // 6: TIDs
            std::string threadsStr = r.isRunning ? std::to_string(r.currentThreadCount) : "-";
            ListView_SetItemText(hList, itemIdx, 6, const_cast<LPSTR>(threadsStr.c_str()));

            // 7: Chgs
            std::string countStr = std::to_string(r.applyCount);
            ListView_SetItemText(hList, itemIdx, 7, const_cast<LPSTR>(countStr.c_str()));

            // 8: Audio
            int assignedCore = r.audioCore;
            if (s < r.audioCores.size()) {
                assignedCore = r.audioCores[s];
            } else if (r.audioAffinityMask != 0) {
                int bitIdx = -1;
                int foundCount = 0;
                for (int c = 0; c < 64; ++c) {
                    if ((r.audioAffinityMask & (1ULL << c)) != 0) {
                        if (foundCount == static_cast<int>(s)) {
                            bitIdx = c;
                            break;
                        }
                        foundCount++;
                    }
                }
                if (bitIdx >= 0) assignedCore = bitIdx;
            }
            std::string forAudioStr = r.isRunning ? GetCoreLabel(assignedCore, cores) : "-";
            ListView_SetItemText(hList, itemIdx, 8, const_cast<LPSTR>(forAudioStr.c_str()));

            // 9: Excluded (最小TID以外は "same")
            if (s > 0) {
                ListView_SetItemText(hList, itemIdx, 9, const_cast<LPSTR>("same"));
            } else {
                int coreCount = ati::CpuTopology::GetSystemCoreCount();
                DWORD_PTR normalMask = (r.normalAffinityMask != 0) ? r.normalAffinityMask : g_isolator.GetConfig().normalAffinityMask;
                DWORD_PTR mask = r.audioAffinityMask ? r.audioAffinityMask : ati::ThreadIsolator::MakeCoreMask(r.audioCore);
                std::string otherThanStr = FormatOtherThanLabels(r.isRunning, r.isBypassed, r.isAudioIsolated, mask, normalMask, coreCount, cores);
                ListView_SetItemText(hList, itemIdx, 9, const_cast<LPSTR>(otherThanStr.c_str()));
            }
        }
    }
}

static void SetSubItemTextIfChanged(HWND hList, int item, int subItem, const std::string& newText) {
    char curBuf[256] = { 0 };
    ListView_GetItemText(hList, item, subItem, curBuf, sizeof(curBuf));
    if (newText != curBuf) {
        ListView_SetItemText(hList, item, subItem, const_cast<LPSTR>(newText.c_str()));
    }
}

static void UpdateListViewDynamic(HWND hList, const std::vector<ati::ProcessRule>& rules) {
    int count = ListView_GetItemCount(hList);
    size_t expectedRows = 0;
    for (const auto& r : rules) {
        expectedRows += (r.ignoreSig && r.ignoreSigCount > 1) ? static_cast<size_t>(r.ignoreSigCount) : 1;
    }
    if (count != static_cast<int>(expectedRows) || g_displayRows.size() != expectedRows) {
        RefreshListView(hList, rules);
        return;
    }

    auto cores = ati::CpuTopology::GetCpuCores();

    for (size_t rowIdx = 0; rowIdx < g_displayRows.size(); ++rowIdx) {
        auto& dr = g_displayRows[rowIdx];
        if (dr.ruleIndex >= rules.size()) continue;
        const auto& r = rules[dr.ruleIndex];

        // 最新の TID を更新
        if (dr.subIndex < r.activeAudioTids.size()) {
            dr.tid = r.activeAudioTids[dr.subIndex];
        } else if (dr.subIndex == 0) {
            dr.tid = r.activeAudioTid;
        } else {
            dr.tid = 0;
        }

        int item = static_cast<int>(rowIdx);

        // 0: Indicator (カスタムドロー描画、テキストは空)
        SetSubItemTextIfChanged(hList, item, 0, "");

        // 1: Process Name (.exe を除いて表示)
        SetSubItemTextIfChanged(hList, item, 1, StripExeExtension(r.processName));

        // 2: Thread Priority
        int rowPrio = (dr.subIndex < r.audioPriorities.size()) ? r.audioPriorities[dr.subIndex] : r.audioPriority;
        std::string prioStr = GetPriorityString(rowPrio);
        SetSubItemTextIfChanged(hList, item, 2, prioStr);

        // 3: Process Priority
        std::string procPrioStr = GetProcessPriorityString(r.processPriorityClass);
        SetSubItemTextIfChanged(hList, item, 3, procPrioStr);

        // 4: Pause / Verify (カスタムドロー描画、テキストは空)
        SetSubItemTextIfChanged(hList, item, 4, "");

        // 5: Audio Core PID/TID
        std::string thName;
        if (r.isBypassed) {
            thName = "Bypassed";
        } else if (!r.isRunning) {
            thName = "Not running";
        } else if (r.ignoreSig && r.ignoreSigCount > 1) {
            size_t confirmedCount = r.activeAudioTids.size();
            if (dr.subIndex < confirmedCount && dr.tid != 0) {
                thName = "PID " + std::to_string(r.activePid) + " / TID " + std::to_string(dr.tid);
            } else if (r.multiSearchSleeping) {
                thName = "sleeping...";
            } else {
                int intervalMs = (g_isolator.GetConfig().pollingIntervalMs > 0) ? g_isolator.GetConfig().pollingIntervalMs : 1000;
                int turnsPerSec = 1000 / intervalMs;
                if (turnsPerSec < 1) turnsPerSec = 1;
                int sec = (r.multiSearchTurns + turnsPerSec - 1) / turnsPerSec;
                if (sec < 1) sec = 1;
                if (sec > 10) sec = 10;
                if (dr.subIndex == confirmedCount) {
                    thName = "Standby " + std::to_string(sec);
                } else {
                    thName = "Standby 10";
                }
            }
        } else if (r.isAudioIsolated && dr.tid != 0) {
            thName = "PID " + std::to_string(r.activePid) + " / TID " + std::to_string(dr.tid);
        } else if (r.detectedThreadName.empty()) {
            thName = r.isRunning ? "Scanning..." : "Not running";
        } else {
            thName = r.detectedThreadName;
        }
        SetSubItemTextIfChanged(hList, item, 5, thName);

        // 6: TIDs
        std::string threadsStr = r.isRunning ? std::to_string(r.currentThreadCount) : "-";
        SetSubItemTextIfChanged(hList, item, 6, threadsStr);

        // 7: Chgs
        std::string countStr = std::to_string(r.applyCount);
        SetSubItemTextIfChanged(hList, item, 7, countStr);

        // 8: Audio
        int assignedCore = r.audioCore;
        if (dr.subIndex < r.audioCores.size()) {
            assignedCore = r.audioCores[dr.subIndex];
        } else if (r.audioAffinityMask != 0) {
            int bitIdx = -1;
            int foundCount = 0;
            for (int c = 0; c < 64; ++c) {
                if ((r.audioAffinityMask & (1ULL << c)) != 0) {
                    if (foundCount == static_cast<int>(dr.subIndex)) {
                        bitIdx = c;
                        break;
                    }
                    foundCount++;
                }
            }
            if (bitIdx >= 0) assignedCore = bitIdx;
        }
        std::string forAudioStr = r.isRunning ? GetCoreLabel(assignedCore, cores) : "-";
        SetSubItemTextIfChanged(hList, item, 8, forAudioStr);

        // 9: Excluded (最小TID以外は "same")
        if (dr.subIndex > 0) {
            SetSubItemTextIfChanged(hList, item, 9, "same");
        } else {
            int coreCount = ati::CpuTopology::GetSystemCoreCount();
            DWORD_PTR normalMask = (r.normalAffinityMask != 0) ? r.normalAffinityMask : g_isolator.GetConfig().normalAffinityMask;
            DWORD_PTR mask = r.audioAffinityMask ? r.audioAffinityMask : ati::ThreadIsolator::MakeCoreMask(r.audioCore);
            std::string otherThanStr = FormatOtherThanLabels(r.isRunning, r.isBypassed, r.isAudioIsolated, mask, normalMask, coreCount, cores);
            SetSubItemTextIfChanged(hList, item, 9, otherThanStr);
        }
    }
}

// --- システムトレイ制御 (ANSI) ---
static void InitTrayIcon(HWND hWnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON_MSG;
    g_nid.hIcon = LoadIconA(g_hInstance, MAKEINTRESOURCEA(IDI_APP_ICON));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    strcpy_s(g_nid.szTip, "Audio Thread Isolator (ATI)");

    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon() {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

// --- affinity 表用サブクラスプロシージャ (横スクロール許可、縦スクロールのみ抑止、マウスホイール横スクロール連動) ---
static LRESULT CALLBACK AffinityTableSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_VSCROLL) {
        return 0; // 縦スクロールのみ抑止
    }
    if (msg == WM_MOUSEWHEEL) {
        // マウスホイールの回転を横スクロール (WM_HSCROLL) に変換してコア一覧を快適にスクロール
        short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        UINT code = (zDelta < 0) ? SB_LINERIGHT : SB_LINELEFT;
        SendMessageA(hWnd, WM_HSCROLL, MAKEWPARAM(code, 0), 0);
        return 0;
    }
    WNDPROC pfnOld = (WNDPROC)GetPropA(hWnd, "ATI_OrigTableProc");
    if (msg == WM_NCDESTROY) {
        RemovePropA(hWnd, "ATI_OrigTableProc");
        if (pfnOld) {
            SetWindowLongPtrA(hWnd, GWLP_WNDPROC, (LONG_PTR)pfnOld);
            return CallWindowProcA(pfnOld, hWnd, msg, wParam, lParam);
        }
    }
    if (pfnOld) {
        LRESULT res = CallWindowProcA(pfnOld, hWnd, msg, wParam, lParam);
        if (msg == WM_PAINT || msg == WM_SIZE || msg == WM_NCCALCSIZE) {
            ShowScrollBar(hWnd, SB_VERT, FALSE);
        }
        return res;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static void SubclassAffinityTable(HWND hList) {
    if (!hList) return;
    WNDPROC pfnOld = (WNDPROC)SetWindowLongPtrA(hList, GWLP_WNDPROC, (LONG_PTR)AffinityTableSubclassProc);
    if (pfnOld) {
        SetPropA(hList, "ATI_OrigTableProc", (HANDLE)pfnOld);
    }
}

// ── アフィニティテーブル初期化共通ヘルパー (候補 M5) ──
static void InitAffinityListView(HWND hDlg, int listCtrlId, float scale,
                                  const std::vector<ati::CoreInfo>& cores,
                                  const char* row0Label, const char* row1Label) {
    HWND hList = GetDlgItem(hDlg, listCtrlId);
    if (!hList) return;

    HFONT hFont = GetEffectiveFont(hDlg);
    SubclassAffinityTable(hList);
    SendMessageA(hList, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    HWND hHeader = ListView_GetHeader(hList);
    if (hHeader) SendMessageA(hHeader, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNA lvc = { 0 };
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;

    // 0: Item
    lvc.fmt = LVCFMT_LEFT;
    lvc.cx = ScaleI(Layout::kAffinityItemColW, scale);
    lvc.iSubItem = 0;
    lvc.pszText = const_cast<LPSTR>("Item");
    ListView_InsertColumn(hList, 0, &lvc);

    // 1 .. coreCount: コアラベル (#0, #1, P0, E0 等)
    for (size_t c = 0; c < cores.size(); ++c) {
        lvc.fmt = LVCFMT_CENTER;
        lvc.cx = ScaleI(Layout::kAffinityCoreColW, scale);
        lvc.iSubItem = static_cast<int>(1 + c);
        lvc.pszText = const_cast<LPSTR>(cores[c].label.c_str());
        ListView_InsertColumn(hList, static_cast<int>(1 + c), &lvc);
    }

    // 行 0:
    LVITEMA lvi = { 0 };
    lvi.mask = LVIF_TEXT;
    lvi.iItem = 0;
    lvi.pszText = const_cast<LPSTR>(row0Label);
    ListView_InsertItem(hList, &lvi);

    // 行 1:
    lvi.iItem = 1;
    lvi.pszText = const_cast<LPSTR>(row1Label);
    ListView_InsertItem(hList, &lvi);
}

// ── アフィニティテーブル チェックボックスカスタムドロー共通ハンドラ (候補 M6) ──
template <typename CheckFn>
static BOOL HandleAffinityCustomDraw(HWND hDlg, int listCtrlId, LPARAM lParam,
                                     const std::vector<ati::CoreInfo>& cores,
                                     CheckFn isCheckedFn) {
    LPNMLVCUSTOMDRAW plvcd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
    switch (plvcd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
        return TRUE;
    case CDDS_ITEMPREPAINT:
        SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
        return TRUE;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        int item = static_cast<int>(plvcd->nmcd.dwItemSpec);
        int subItem = plvcd->iSubItem;
        int coreCount = static_cast<int>(cores.size());

        HWND hList = GetDlgItem(hDlg, listCtrlId);
        bool isSelected = (ListView_GetItemState(hList, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        COLORREF clrBk = isSelected ? RGB(225, 238, 254) : RGB(255, 255, 255);
        plvcd->clrTextBk = clrBk;
        plvcd->clrText = RGB(0, 0, 0);

        if (subItem >= 1 && subItem <= coreCount) {
            HDC hdc = plvcd->nmcd.hdc;
            RECT rcSub;
            ListView_GetSubItemRect(hList, item, subItem, LVIR_BOUNDS, &rcSub);

            HBRUSH hBr = CreateSolidBrush(clrBk);
            FillRect(hdc, &rcSub, hBr);
            DeleteObject(hBr);

            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(235, 235, 235));
            HPEN hOldPen = static_cast<HPEN>(SelectObject(hdc, hPen));
            MoveToEx(hdc, rcSub.right - 1, rcSub.top, nullptr);
            LineTo(hdc, rcSub.right - 1, rcSub.bottom);
            MoveToEx(hdc, rcSub.left, rcSub.bottom - 1, nullptr);
            LineTo(hdc, rcSub.right, rcSub.bottom - 1);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);

            int logicalIdx = cores[subItem - 1].logicalIndex;
            bool isChecked = isCheckedFn(item, logicalIdx);

            float scale = GetDpiScaleForWindow(hDlg);
            int boxSize = ScaleI(Layout::kCheckboxSize, scale);
            if (boxSize < Layout::kCheckboxSize) boxSize = Layout::kCheckboxSize;
            RECT rcBox;
            rcBox.left = rcSub.left + (rcSub.right - rcSub.left - boxSize) / 2;
            rcBox.top = rcSub.top + (rcSub.bottom - rcSub.top - boxSize) / 2;
            rcBox.right = rcBox.left + boxSize;
            rcBox.bottom = rcBox.top + boxSize;

            UINT uState = DFCS_BUTTONCHECK | (isChecked ? DFCS_CHECKED : 0) | DFCS_FLAT;
            DrawFrameControl(hdc, &rcBox, DFC_BUTTON, uState);

            SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
            return TRUE;
        }

        SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
        return TRUE;
    }
    default:
        break;
    }
    return FALSE;
}

// ── 子ダイアログ内全幅コントロールのリサイズ共通ヘルパー (候補 M7) ──
static inline void ResizeDialogControlWidth(HWND hDlg, int ctrlId, int cx, int margin) {
    HWND hCtrl = GetDlgItem(hDlg, ctrlId);
    if (hCtrl) {
        RECT rc;
        GetWindowRect(hCtrl, &rc);
        MapWindowPoints(HWND_DESKTOP, hDlg, reinterpret_cast<LPPOINT>(&rc), 2);
        int w = cx - margin * 2;
        int h = rc.bottom - rc.top;
        SetWindowPos(hCtrl, nullptr, margin, rc.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// --- CPU Sets 構造体・API 型定義 (Windows 10/11) ---
#pragma pack(push, 8)
typedef struct _ATI_SYSTEM_CPU_SET_INFORMATION {
    DWORD Size;
    DWORD Type; // 0 = CpuSetInformation
    union {
        struct {
            DWORD Id;
            WORD  Group;
            BYTE  LogicalProcessorIndex;
            BYTE  CoreIndex;
            BYTE  LastLevelCacheIndex;
            BYTE  EfficiencyClass;
            BYTE  AllFlags;
            BYTE  Reserved;
            BYTE  SchedulingClass;
            DWORD64 AllocationTag;
        } CpuSet;
    };
} ATI_SYSTEM_CPU_SET_INFORMATION;
#pragma pack(pop)

typedef BOOL (WINAPI *PFN_GetSystemCpuSetInformation)(
    void* Information,
    ULONG BufferLength,
    PULONG ReturnedLength,
    HANDLE Process,
    ULONG Flags
);

typedef BOOL (WINAPI *PFN_SetProcessDefaultCpuSets)(
    HANDLE Process,
    const ULONG *CpuSetIds,
    ULONG CpuSetIdCount
);

// 揮発性メモリフラグ (起動時のみ保持、終了で消滅)
static BOOL s_bUnlistedMaskApplied = FALSE;

// 特権昇格 (SeDebugPrivilege)
static void EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp = { 0 };
        LUID luid = { 0 };
        if (LookupPrivilegeValueA(nullptr, "SeDebugPrivilege", &luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        }
        CloseHandle(hToken);
    }
}

// リスト外プロセスへの CPU Sets 適用処理
static bool ApplyCpuSetsToUnlistedProcesses(HWND hParent, DWORD_PTR targetMask) {
    EnableDebugPrivilege();

    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    if (!hK32) return false;
    auto pfnGetSystemCpuSetInformation = (PFN_GetSystemCpuSetInformation)GetProcAddress(hK32, "GetSystemCpuSetInformation");
    auto pfnSetProcessDefaultCpuSets = (PFN_SetProcessDefaultCpuSets)GetProcAddress(hK32, "SetProcessDefaultCpuSets");
    if (!pfnGetSystemCpuSetInformation || !pfnSetProcessDefaultCpuSets) {
        MessageBoxA(hParent, "CPU Sets API is not supported on this Windows version.", "ATI System CPU Sets", MB_OK | MB_ICONWARNING);
        return false;
    }

    ULONG bufSize = 0;
    pfnGetSystemCpuSetInformation(nullptr, 0, &bufSize, GetCurrentProcess(), 0);
    if (bufSize == 0) {
        MessageBoxA(hParent, "Failed to query system CPU Sets information.", "ATI System CPU Sets", MB_OK | MB_ICONERROR);
        return false;
    }

    std::vector<BYTE> buf(bufSize);
    if (!pfnGetSystemCpuSetInformation(buf.data(), bufSize, &bufSize, GetCurrentProcess(), 0)) {
        MessageBoxA(hParent, "Failed to retrieve system CPU Sets.", "ATI System CPU Sets", MB_OK | MB_ICONERROR);
        return false;
    }

    std::vector<ULONG> targetCpuSetIds;
    ULONG offset = 0;
    while (offset + sizeof(DWORD) * 2 <= bufSize) {
        auto* pInfo = reinterpret_cast<const ATI_SYSTEM_CPU_SET_INFORMATION*>(buf.data() + offset);
        if (pInfo->Size == 0) break;
        if (pInfo->Type == 0) { // CpuSetInformation
            BYTE logicalIdx = pInfo->CpuSet.LogicalProcessorIndex;
            if (logicalIdx < 64 && ((targetMask & (1ULL << logicalIdx)) != 0)) {
                targetCpuSetIds.push_back(pInfo->CpuSet.Id);
            }
        }
        offset += pInfo->Size;
    }

    if (targetCpuSetIds.empty()) {
        MessageBoxA(hParent, "No CPU Set IDs matched the target mask.", "ATI System CPU Sets", MB_OK | MB_ICONWARNING);
        return false;
    }

    // ホワイトリスト構築
    std::unordered_set<std::string> whitelist;
    whitelist.insert("ati.exe");
    whitelist.insert("system");
    whitelist.insert("system idle process");
    for (const auto& r : g_isolator.GetConfig().rules) {
        std::string lowerName = r.processName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        whitelist.insert(lowerName);
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        MessageBoxA(hParent, "Failed to create process snapshot.", "ATI System CPU Sets", MB_OK | MB_ICONERROR);
        return false;
    }

    DWORD myPid = GetCurrentProcessId();
    int successCount = 0;
    int skippedCount = 0;
    int deniedCount = 0;

    PROCESSENTRY32 pe = { sizeof(pe) };
    if (Process32First(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4 || pe.th32ProcessID == myPid) {
                skippedCount++;
                continue;
            }
            std::string exeLower = pe.szExeFile;
            std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::tolower);
            if (whitelist.count(exeLower) > 0) {
                skippedCount++;
                continue;
            }

            HANDLE hProcess = OpenProcess(PROCESS_SET_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hProcess) {
                hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pe.th32ProcessID);
            }
            if (!hProcess) {
                deniedCount++;
                continue;
            }

            if (pfnSetProcessDefaultCpuSets(hProcess, targetCpuSetIds.data(), static_cast<ULONG>(targetCpuSetIds.size()))) {
                successCount++;
            } else {
                deniedCount++;
            }
            CloseHandle(hProcess);
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);

    char msg[256];
    snprintf(msg, sizeof(msg),
        "CPU Sets applied successfully!\n\n"
        "- Target Mask: 0x%llX (%zu cores)\n"
        "- Applied Processes: %d\n"
        "- Whitelisted / Protected (Skipped): %d",
        static_cast<unsigned long long>(targetMask),
        targetCpuSetIds.size(),
        successCount,
        skippedCount + deniedCount);

    MessageBoxA(hParent, msg, "ATI System CPU Sets", MB_OK | MB_ICONINFORMATION);
    return true;
}

// リスト外プロセスへの CPU Sets 解除処理
static bool ReleaseCpuSetsFromUnlistedProcesses(HWND hParent) {
    EnableDebugPrivilege();

    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    if (!hK32) return false;
    auto pfnSetProcessDefaultCpuSets = (PFN_SetProcessDefaultCpuSets)GetProcAddress(hK32, "SetProcessDefaultCpuSets");
    if (!pfnSetProcessDefaultCpuSets) return false;

    std::unordered_set<std::string> whitelist;
    whitelist.insert("ati.exe");
    whitelist.insert("system");
    whitelist.insert("system idle process");
    for (const auto& r : g_isolator.GetConfig().rules) {
        std::string lowerName = r.processName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        whitelist.insert(lowerName);
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    DWORD myPid = GetCurrentProcessId();
    int releasedCount = 0;

    PROCESSENTRY32 pe = { sizeof(pe) };
    if (Process32First(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4 || pe.th32ProcessID == myPid) continue;
            std::string exeLower = pe.szExeFile;
            std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::tolower);
            if (whitelist.count(exeLower) > 0) continue;

            HANDLE hProcess = OpenProcess(PROCESS_SET_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hProcess) {
                hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pe.th32ProcessID);
            }
            if (!hProcess) continue;

            if (pfnSetProcessDefaultCpuSets(hProcess, nullptr, 0)) {
                releasedCount++;
            }
            CloseHandle(hProcess);
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);

    char msg[256];
    snprintf(msg, sizeof(msg),
        "CPU Sets restrictions released successfully!\n\n"
        "- Restored Processes: %d\n"
        "All general processes restored to default all-core scheduling.",
        releasedCount);

    MessageBoxA(hParent, msg, "ATI System CPU Sets", MB_OK | MB_ICONINFORMATION);
    return true;
}

// Priority ComboBox オーナードロー補助 (右詰め描画)
static BOOL HandlePriorityComboMeasureItem(HWND hDlg, LPARAM lParam, UINT ctlId) {
    LPMEASUREITEMSTRUCT pmis = reinterpret_cast<LPMEASUREITEMSTRUCT>(lParam);
    if (pmis && pmis->CtlID == ctlId) {
        float scale = GetDpiScaleForWindow(hDlg);
        pmis->itemHeight = static_cast<UINT>(18 * scale);
        return TRUE;
    }
    return FALSE;
}

static BOOL HandlePriorityComboDrawItem(HWND hDlg, LPARAM lParam, UINT ctlId) {
    LPDRAWITEMSTRUCT pdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
    if (pdis && pdis->CtlID == ctlId) {
        if (pdis->itemID == static_cast<UINT>(-1)) {
            return TRUE;
        }
        HDC hdc = pdis->hDC;
        RECT rc = pdis->rcItem;
        bool isSelected = (pdis->itemState & ODS_SELECTED) != 0;
        bool isDisabled = (pdis->itemState & ODS_DISABLED) != 0;
        bool isComboEdit = (pdis->itemState & ODS_COMBOBOXEDIT) != 0;

        COLORREF bkCol = isDisabled ? GetSysColor(COLOR_BTNFACE) : (isSelected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW));
        COLORREF textCol = isDisabled ? GetSysColor(COLOR_GRAYTEXT) : (isSelected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT));

        HBRUSH hBr = CreateSolidBrush(bkCol);
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);

        char text[64] = { 0 };
        SendMessageA(pdis->hwndItem, CB_GETLBTEXT, pdis->itemID, reinterpret_cast<LPARAM>(text));

        HFONT hFont = reinterpret_cast<HFONT>(SendMessageA(pdis->hwndItem, WM_GETFONT, 0, 0));
        if (!hFont) hFont = GetEffectiveFont(hDlg);
        HGDIOBJ oldFont = SelectObject(hdc, hFont);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textCol);

        float scale = GetDpiScaleForWindow(hDlg);
        int extraLeft = ScaleI(2, scale); // 200%環境で4px相当 (6pxから2px右へ戻す)
        RECT rcText = rc;
        if (isComboEdit) {
            rcText.right -= 6;
        } else {
            // ドロップダウンリスト内: 矢印ボタン幅分オフセット + 200%環境で4px相当を左へ微調整
            int arrowW = GetSystemMetrics(SM_CXVSCROLL);
            rcText.right -= (arrowW + 6 + extraLeft);
        }
        DrawTextA(hdc, text, -1, &rcText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldFont);

        if (pdis->itemState & ODS_FOCUS) {
            DrawFocusRect(hdc, &rc);
        }
        return TRUE;
    }
    return FALSE;
}

// --- 全体設定ダイアログ (ANSI - 表形式 + 2行説明 + 押しやすいApply + リサイズ対応) ---
static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static ati::GlobalConfig s_tmpConfig;

    switch (msg) {
    case WM_MEASUREITEM: {
        if (HandlePriorityComboMeasureItem(hDlg, lParam, IDC_COMBO_AUDIO_PRIORITY)) {
            return TRUE;
        }
        break;
    }

    case WM_INITDIALOG: {
        s_tmpConfig = g_isolator.GetConfig();
        if (s_tmpConfig.defaultAudioAffinityMask == 0) {
            s_tmpConfig.defaultAudioAffinityMask = (1ULL << s_tmpConfig.defaultAudioCore);
        }

        auto cores = ati::CpuTopology::GetCpuCores();
        int coreCount = static_cast<int>(cores.size());
        float scale = GetDpiScaleForWindow(hDlg);

        // メインウィンドウからディスプレイ中央方向に少しずれた場所へ表示
        if (g_hMainDlg && IsWindow(g_hMainDlg)) {
            RECT rcMain = { 0 };
            GetWindowRect(g_hMainDlg, &rcMain);

            HMONITOR hMon = MonitorFromWindow(g_hMainDlg, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoA(hMon, &mi);

            int screenCenterX = (mi.rcWork.left + mi.rcWork.right) / 2;
            int screenCenterY = (mi.rcWork.top + mi.rcWork.bottom) / 2;
            int mainCenterX = (rcMain.left + rcMain.right) / 2;
            int mainCenterY = (rcMain.top + rcMain.bottom) / 2;

            int stepX = ScaleI(Layout::kPickerOffset, scale);
            int stepY = ScaleI(Layout::kPickerOffset, scale);
            int offsetX = (screenCenterX >= mainCenterX) ? stepX : -stepX;
            int offsetY = (screenCenterY >= mainCenterY) ? stepY : -stepY;

            RECT rcDlg = { 0 };
            GetWindowRect(hDlg, &rcDlg);
            int dlgW = rcDlg.right - rcDlg.left;
            int dlgH = rcDlg.bottom - rcDlg.top;

            int newX = rcMain.left + offsetX;
            int newY = rcMain.top + offsetY;

            if (newX + dlgW > mi.rcWork.right - 10) newX = mi.rcWork.right - dlgW - 10;
            if (newX < mi.rcWork.left + 10) newX = mi.rcWork.left + 10;
            if (newY + dlgH > mi.rcWork.bottom - 10) newY = mi.rcWork.bottom - dlgH - 10;
            if (newY < mi.rcWork.top + 10) newY = mi.rcWork.top + 10;

            SetWindowPos(hDlg, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        InitAffinityListView(hDlg, IDC_LIST_SETTINGS_CORES, scale, cores,
                             "Default Audio Core", "Excluded");
        HWND hList = GetDlgItem(hDlg, IDC_LIST_SETTINGS_CORES);
        if (hList) {
            ShowScrollBar(hList, SB_VERT, FALSE);
            ListView_Scroll(hList, 0, -9999);
        }

        // 入力ボックスの初期値設定 (0x HEX 表示)
        char aHex[32] = { 0 };
        snprintf(aHex, sizeof(aHex), "0x%llX", static_cast<unsigned long long>(s_tmpConfig.defaultAudioAffinityMask));
        SetDlgItemTextA(hDlg, IDC_EDIT_AUDIO_CORE, aHex);

        char hexBuf[32] = { 0 };
        snprintf(hexBuf, sizeof(hexBuf), "0x%llX", static_cast<unsigned long long>(s_tmpConfig.normalAffinityMask));
        SetDlgItemTextA(hDlg, IDC_EDIT_NORMAL_CORES, hexBuf);

        HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_AUDIO_PRIORITY);
        if (hCombo) {
            SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
            int selectedIdx = 0;
            for (int i = 0; i < PRIO_OPTIONS_COUNT; ++i) {
                int itemIdx = static_cast<int>(SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)PRIO_OPTIONS[i].label));
                SendMessageA(hCombo, CB_SETITEMDATA, itemIdx, PRIO_OPTIONS[i].value);
                if (PRIO_OPTIONS[i].value == s_tmpConfig.defaultAudioPriority) {
                    selectedIdx = itemIdx;
                }
            }
            SendMessageA(hCombo, CB_SETCURSEL, selectedIdx, 0);
        }

        SetDlgItemInt(hDlg, IDC_EDIT_POLLING_MS, s_tmpConfig.pollingIntervalMs, FALSE);

        // リリースボタンの活性・非活性状態を反映
        HWND hBtnRel = GetDlgItem(hDlg, IDC_BTN_RELEASE_MASK);
        if (hBtnRel) {
            EnableWindow(hBtnRel, s_bUnlistedMaskApplied);
        }

        // 初回レイアウトを強制適用
        RECT rcInit;
        GetClientRect(hDlg, &rcInit);
        SendMessageA(hDlg, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rcInit.right, rcInit.bottom));
        return TRUE;
    }

    case WM_DRAWITEM: {
        if (HandlePriorityComboDrawItem(hDlg, lParam, IDC_COMBO_AUDIO_PRIORITY)) {
            return TRUE;
        }
        LPDRAWITEMSTRUCT pdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
        if (pdis && pdis->CtlID == IDC_BTN_RELEASE_MASK) {
            HDC hdc = pdis->hDC;
            RECT rc = pdis->rcItem;
            bool isPressed = (pdis->itemState & ODS_SELECTED) != 0;
            bool isDisabled = (pdis->itemState & ODS_DISABLED) != 0;

            HBRUSH hBr = CreateSolidBrush(isDisabled ? RGB(242, 242, 242) : (isPressed ? RGB(220, 230, 224) : RGB(244, 247, 245)));
            FillRect(hdc, &rc, hBr);
            DeleteObject(hBr);

            COLORREF borderColor = isDisabled ? RGB(208, 208, 208) : (isPressed ? RGB(75, 110, 88) : RGB(95, 135, 110));
            HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
            HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
            HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 4, 4);
            SelectObject(hdc, hOldBr);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);

            SetBkMode(hdc, TRANSPARENT);
            COLORREF textColor = isDisabled ? RGB(160, 160, 160) : RGB(60, 95, 75); // 落ち着いた鈍い緑
            SetTextColor(hdc, textColor);

            HFONT hBaseFont = reinterpret_cast<HFONT>(SendMessageA(pdis->hwndItem, WM_GETFONT, 0, 0));
            if (!hBaseFont) hBaseFont = GetEffectiveFont(hDlg);

            LOGFONTA lf = { 0 };
            GetObjectA(hBaseFont, sizeof(lf), &lf);
            lf.lfWeight = FW_BOLD; // 太字で視認性を強化
            HFONT hBoldFont = CreateFontIndirectA(&lf);
            HFONT hOldFont = (HFONT)SelectObject(hdc, hBoldFont);

            char text[64] = { 0 };
            GetWindowTextA(pdis->hwndItem, text, sizeof(text));
            DrawTextA(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, hOldFont);
            DeleteObject(hBoldFont);
            return TRUE;
        }
        break;
    }

    case WM_GETMINMAXINFO: {
        LPMINMAXINFO lpMMI = reinterpret_cast<LPMINMAXINFO>(lParam);
        float scale = GetDpiScaleForWindow(hDlg);
        lpMMI->ptMinTrackSize.x = static_cast<LONG>(ScaleI(Layout::kPickerMinW, scale));
        lpMMI->ptMinTrackSize.y = static_cast<LONG>(ScaleI(Layout::kPickerMinH, scale));
        return 0;
    }

    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) return TRUE;
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        if (cx <= 0 || cy <= 0) return TRUE;

        float scale = GetDpiScaleForWindow(hDlg);
        int margin = ScaleI(Layout::kDialogMargin, scale);
        int spacing = ScaleI(Layout::kCtrlSpacing, scale);

        // コア表リストビューの幅追従
        ResizeDialogControlWidth(hDlg, IDC_LIST_SETTINGS_CORES, cx, margin);

        // Apply ボタンの右寄せ追従
        auto MoveRight = [&](int ctrlId) {
            HWND hCtrl = GetDlgItem(hDlg, ctrlId);
            if (hCtrl) {
                RECT rc;
                GetWindowRect(hCtrl, &rc);
                MapWindowPoints(HWND_DESKTOP, hDlg, reinterpret_cast<LPPOINT>(&rc), 2);
                int w = rc.right - rc.left;
                int h = rc.bottom - rc.top;
                int newX = cx - margin - w;
                SetWindowPos(hCtrl, nullptr, newX, rc.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        };

        MoveRight(IDC_BTN_APPLY_EXCLUDE_CORE);
        MoveRight(IDC_BTN_APPLY_NORMAL_MASK);

        // 説明テキストの幅追従（入力欄の右 〜 Apply ボタンの左）
        auto AdjustDescWidth = [&](int descId, int btnId) {
            HWND hDesc = GetDlgItem(hDlg, descId);
            HWND hBtn = GetDlgItem(hDlg, btnId);
            if (hDesc && hBtn) {
                RECT rcDesc, rcBtn;
                GetWindowRect(hDesc, &rcDesc);
                GetWindowRect(hBtn, &rcBtn);
                MapWindowPoints(HWND_DESKTOP, hDlg, reinterpret_cast<LPPOINT>(&rcDesc), 2);
                MapWindowPoints(HWND_DESKTOP, hDlg, reinterpret_cast<LPPOINT>(&rcBtn), 2);
                int descW = rcBtn.left - spacing - rcDesc.left;
                if (descW > 0) {
                    SetWindowPos(hDesc, nullptr, rcDesc.left, rcDesc.top, descW, rcDesc.bottom - rcDesc.top, SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        };

        AdjustDescWidth(IDC_STATIC_EXCLUDE_DESC, IDC_BTN_APPLY_EXCLUDE_CORE);
        AdjustDescWidth(IDC_STATIC_EXCLUDE_DESC2, IDC_BTN_APPLY_EXCLUDE_CORE);
        AdjustDescWidth(IDC_STATIC_NORMAL_DESC, IDC_BTN_APPLY_NORMAL_MASK);
        AdjustDescWidth(IDC_STATIC_NORMAL_DESC2, IDC_BTN_APPLY_NORMAL_MASK);

        // 最下部ボタン列:
        // 左下: Release Mask
        HWND hBtnRelease = GetDlgItem(hDlg, IDC_BTN_RELEASE_MASK);
        if (hBtnRelease) {
            RECT rcRel;
            GetWindowRect(hBtnRelease, &rcRel);
            int relW = rcRel.right - rcRel.left;
            int relH = rcRel.bottom - rcRel.top;
            int relY = cy - margin - relH;
            SetWindowPos(hBtnRelease, nullptr, margin, relY, relW, relH, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // 右下: Cancel / OK
        HWND hBtnCancel = GetDlgItem(hDlg, IDC_BTN_SETTINGS_CANCEL);
        HWND hBtnOk = GetDlgItem(hDlg, IDC_BTN_SETTINGS_OK);
        if (hBtnCancel && hBtnOk) {
            RECT rcCancel, rcOk;
            GetWindowRect(hBtnCancel, &rcCancel);
            GetWindowRect(hBtnOk, &rcOk);
            int btnW = rcCancel.right - rcCancel.left;
            int btnH = rcCancel.bottom - rcCancel.top;
            int btnY = cy - margin - btnH;
            int cancelX = cx - margin - btnW;
            int okX = cancelX - spacing - btnW;
            SetWindowPos(hBtnCancel, nullptr, cancelX, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(hBtnOk, nullptr, okX, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        InvalidateRect(hDlg, nullptr, TRUE);
        return TRUE;
    }

    case WM_NOTIFY: {
        LPNMHDR pnm = reinterpret_cast<LPNMHDR>(lParam);
        if (pnm && pnm->idFrom == IDC_LIST_SETTINGS_CORES) {
            auto cores = ati::CpuTopology::GetCpuCores();
            int coreCount = static_cast<int>(cores.size());

            if (pnm->code == NM_CUSTOMDRAW) {
                return HandleAffinityCustomDraw(hDlg, IDC_LIST_SETTINGS_CORES, lParam, cores,
                    [](int item, int logicalIdx) {
                        if (item == 0) {
                            return (s_tmpConfig.defaultAudioAffinityMask & (1ULL << logicalIdx)) != 0;
                        } else if (item == 1) {
                            return (s_tmpConfig.normalAffinityMask & (1ULL << logicalIdx)) != 0;
                        }
                        return false;
                    });
            } else if (pnm->code == NM_CLICK) {
                LPNMITEMACTIVATE pia = reinterpret_cast<LPNMITEMACTIVATE>(lParam);
                if (pia && pia->iItem >= 0 && pia->iSubItem >= 1 && pia->iSubItem <= coreCount) {
                    int logicalIdx = cores[pia->iSubItem - 1].logicalIndex;
                    DWORD_PTR bit = (1ULL << logicalIdx);
                    if (pia->iItem == 0) {
                        // 行 0: Default Audio Core
                        if (s_tmpConfig.defaultAudioAffinityMask & bit) {
                            // チェックを外す: 最低 1 コア保護 (最後の 1 個なら変更不可)
                            if ((s_tmpConfig.defaultAudioAffinityMask & ~bit) != 0) {
                                s_tmpConfig.defaultAudioAffinityMask &= ~bit;
                            }
                        } else {
                            // チェックを入れる: 相互排他インターロック
                            // Other than から外すが、Other than の最後の 1 個なら変更不可
                            if ((s_tmpConfig.normalAffinityMask & bit) != 0 && (s_tmpConfig.normalAffinityMask & ~bit) == 0) {
                                // Other than が空になってしまうため変更不可
                            } else {
                                s_tmpConfig.defaultAudioAffinityMask |= bit;
                                s_tmpConfig.normalAffinityMask &= ~bit; // 相互排他
                            }
                        }
                        int firstCore = 0;
                        for (int c = 0; c < 64; ++c) {
                            if ((s_tmpConfig.defaultAudioAffinityMask & (1ULL << c)) != 0) {
                                firstCore = c;
                                break;
                            }
                        }
                        s_tmpConfig.defaultAudioCore = firstCore;
                    } else if (pia->iItem == 1) {
                        // 行 1: Other than Audio
                        if (s_tmpConfig.normalAffinityMask & bit) {
                            // チェックを外す: 最低 1 コア保護 (最後の 1 個なら変更不可)
                            if ((s_tmpConfig.normalAffinityMask & ~bit) != 0) {
                                s_tmpConfig.normalAffinityMask &= ~bit;
                            }
                        } else {
                            // チェックを入れる: 相互排他インターロック
                            // Audio Core から外すが、Audio Core の最後の 1 個なら変更不可
                            if ((s_tmpConfig.defaultAudioAffinityMask & bit) != 0 && (s_tmpConfig.defaultAudioAffinityMask & ~bit) == 0) {
                                // Audio Core が空になってしまうため変更不可
                            } else {
                                s_tmpConfig.normalAffinityMask |= bit;
                                s_tmpConfig.defaultAudioAffinityMask &= ~bit; // 相互排他
                                int firstCore = 0;
                                for (int c = 0; c < 64; ++c) {
                                    if ((s_tmpConfig.defaultAudioAffinityMask & (1ULL << c)) != 0) {
                                        firstCore = c;
                                        break;
                                    }
                                }
                                s_tmpConfig.defaultAudioCore = firstCore;
                            }
                        }
                    }
                    char aHex[32] = { 0 };
                    snprintf(aHex, sizeof(aHex), "0x%llX", static_cast<unsigned long long>(s_tmpConfig.defaultAudioAffinityMask));
                    SetDlgItemTextA(hDlg, IDC_EDIT_AUDIO_CORE, aHex);
                    char hexBuf[32];
                    snprintf(hexBuf, sizeof(hexBuf), "0x%llX", static_cast<unsigned long long>(s_tmpConfig.normalAffinityMask));
                    SetDlgItemTextA(hDlg, IDC_EDIT_NORMAL_CORES, hexBuf);

                    HWND hList = GetDlgItem(hDlg, IDC_LIST_SETTINGS_CORES);
                    InvalidateRect(hList, nullptr, FALSE);
                    return TRUE;
                }
            }
        }
        break;
    }

    case WM_COMMAND: {
        WORD cmdId = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (code == EN_CHANGE) {
            if (cmdId == IDC_EDIT_AUDIO_CORE) {
                char buf[128] = { 0 };
                GetDlgItemTextA(hDlg, IDC_EDIT_AUDIO_CORE, buf, sizeof(buf));
                std::string s(buf);
                DWORD_PTR m = 0;
                int fc = 0;
                if (s.length() > 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
                    m = _strtoui64(s.c_str(), nullptr, 16);
                    for (int c = 0; c < 64; ++c) {
                        if ((m & (1ULL << c)) != 0) { fc = c; break; }
                    }
                } else {
                    m = ParseCoreListToMask(s, fc);
                }
                if (m != 0) {
                    s_tmpConfig.defaultAudioAffinityMask = m;
                    s_tmpConfig.defaultAudioCore = fc;
                    HWND hList = GetDlgItem(hDlg, IDC_LIST_SETTINGS_CORES);
                    if (hList) InvalidateRect(hList, nullptr, FALSE);
                }
            } else if (cmdId == IDC_EDIT_NORMAL_CORES) {
                char hexBuf[32] = { 0 };
                GetDlgItemTextA(hDlg, IDC_EDIT_NORMAL_CORES, hexBuf, sizeof(hexBuf));
                DWORD_PTR mask = _strtoui64(hexBuf, nullptr, 16);
                s_tmpConfig.normalAffinityMask = mask;
                HWND hList = GetDlgItem(hDlg, IDC_LIST_SETTINGS_CORES);
                if (hList) InvalidateRect(hList, nullptr, FALSE);
            }
        }

        if (cmdId == IDC_BTN_APPLY_EXCLUDE_CORE) {
            int coreCount = ati::CpuTopology::GetSystemCoreCount();
            DWORD_PTR fullMask = ati::ThreadIsolator::GetFullCoreMask(coreCount);
            DWORD_PTR excludeMask = fullMask & ~s_tmpConfig.defaultAudioAffinityMask;
            if (excludeMask == 0) {
                MessageBoxA(hDlg, "Invalid target mask (cannot exclude all cores).", "ATI System CPU Sets", MB_OK | MB_ICONWARNING);
            } else {
                if (ApplyCpuSetsToUnlistedProcesses(hDlg, excludeMask)) {
                    s_bUnlistedMaskApplied = TRUE;
                    HWND hBtnRel = GetDlgItem(hDlg, IDC_BTN_RELEASE_MASK);
                    if (hBtnRel) {
                        EnableWindow(hBtnRel, TRUE);
                        InvalidateRect(hBtnRel, nullptr, TRUE);
                    }
                }
            }
            return TRUE;
        } else if (cmdId == IDC_BTN_APPLY_NORMAL_MASK) {
            if (s_tmpConfig.normalAffinityMask == 0) {
                MessageBoxA(hDlg, "Invalid normal affinity mask.", "ATI System CPU Sets", MB_OK | MB_ICONWARNING);
            } else {
                if (ApplyCpuSetsToUnlistedProcesses(hDlg, s_tmpConfig.normalAffinityMask)) {
                    s_bUnlistedMaskApplied = TRUE;
                    HWND hBtnRel = GetDlgItem(hDlg, IDC_BTN_RELEASE_MASK);
                    if (hBtnRel) {
                        EnableWindow(hBtnRel, TRUE);
                        InvalidateRect(hBtnRel, nullptr, TRUE);
                    }
                }
            }
            return TRUE;
        } else if (cmdId == IDC_BTN_RELEASE_MASK) {
            if (ReleaseCpuSetsFromUnlistedProcesses(hDlg)) {
                s_bUnlistedMaskApplied = FALSE;
                HWND hBtnRel = GetDlgItem(hDlg, IDC_BTN_RELEASE_MASK);
                if (hBtnRel) {
                    EnableWindow(hBtnRel, FALSE);
                    InvalidateRect(hBtnRel, nullptr, TRUE);
                }
            }
            return TRUE;
        }

        if (cmdId == IDC_BTN_SETTINGS_OK) {
            HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_AUDIO_PRIORITY);
            if (hCombo) {
                int sel = static_cast<int>(SendMessageA(hCombo, CB_GETCURSEL, 0, 0));
                if (sel != CB_ERR) {
                    s_tmpConfig.defaultAudioPriority = static_cast<int>(SendMessageA(hCombo, CB_GETITEMDATA, sel, 0));
                }
            }

            char coreBuf[128] = { 0 };
            GetDlgItemTextA(hDlg, IDC_EDIT_AUDIO_CORE, coreBuf, sizeof(coreBuf));
            std::string s(coreBuf);
            DWORD_PTR m = 0;
            int fc = 0;
            if (s.length() > 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
                m = _strtoui64(s.c_str(), nullptr, 16);
                for (int c = 0; c < 64; ++c) {
                    if ((m & (1ULL << c)) != 0) { fc = c; break; }
                }
            } else {
                m = ParseCoreListToMask(s, fc);
            }
            if (m != 0) {
                s_tmpConfig.defaultAudioAffinityMask = m;
                s_tmpConfig.defaultAudioCore = fc;
            }

            char hexBuf[32] = { 0 };
            GetDlgItemTextA(hDlg, IDC_EDIT_NORMAL_CORES, hexBuf, 32);
            s_tmpConfig.normalAffinityMask = _strtoui64(hexBuf, nullptr, 16);

            BOOL ok = FALSE;
            int poll = GetDlgItemInt(hDlg, IDC_EDIT_POLLING_MS, &ok, FALSE);
            if (ok) s_tmpConfig.pollingIntervalMs = ati::ThreadIsolator::NormalizePollingInterval(poll);

            g_isolator.UpdateConfig(s_tmpConfig);
            SaveConfig(s_tmpConfig, g_iniPath);

            if (s_tmpConfig.normalAffinityMask != 0) {
                SetProcessAffinityMask(GetCurrentProcess(), s_tmpConfig.normalAffinityMask);
            }

            SetTimer(g_hMainDlg, TIMER_POLLING_ID, s_tmpConfig.pollingIntervalMs, nullptr);
            HWND hList = GetDlgItem(g_hMainDlg, IDC_LIST_PROCESSES);
            if (hList) {
                RefreshListView(hList, s_tmpConfig.rules);
                InvalidateRect(hList, nullptr, FALSE);
            }

            EndDialog(hDlg, IDOK);
            return TRUE;
        } else if (cmdId == IDC_BTN_SETTINGS_CANCEL || cmdId == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

// --- 個別ルール編集ダイアログ (ANSI - 表形式 + 右詰め入力欄) ---
static size_t s_editRuleIndex = 0;
static ati::ProcessRule s_editRule;

static void SyncAudioCoresFromMask(std::vector<int>& cores, DWORD_PTR mask) {
    std::vector<int> updated;
    // 既存の順序を尊重して mask に含まれるものを残す
    for (int c : cores) {
        if ((mask & (1ULL << c)) != 0) {
            if (std::find(updated.begin(), updated.end(), c) == updated.end()) {
                updated.push_back(c);
            }
        }
    }
    // mask に新しく追加されたビットを追加
    for (int c = 0; c < 64; ++c) {
        if ((mask & (1ULL << c)) != 0) {
            if (std::find(updated.begin(), updated.end(), c) == updated.end()) {
                updated.push_back(c);
            }
        }
    }
    cores = updated;
}

static INT_PTR CALLBACK RuleEditDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MEASUREITEM: {
        if (HandlePriorityComboMeasureItem(hDlg, lParam, IDC_COMBO_RULE_PRIO)) {
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        if (HandlePriorityComboDrawItem(hDlg, lParam, IDC_COMBO_RULE_PRIO)) {
            return TRUE;
        }
        break;
    }
    case WM_INITDIALOG: {
        SetDlgItemTextA(hDlg, IDC_STATIC_PROCESS_NAME, StripExeExtension(s_editRule.processName).c_str());

        // メイン画面の当該プロセス行を隠さない直上に重ねて配置
        HWND hMainList = g_hMainDlg ? GetDlgItem(g_hMainDlg, IDC_LIST_PROCESSES) : nullptr;
        if (hMainList && s_editRuleIndex < static_cast<size_t>(ListView_GetItemCount(hMainList))) {
            RECT rcItem = { 0 };
            ListView_GetItemRect(hMainList, static_cast<int>(s_editRuleIndex), &rcItem, LVIR_BOUNDS);
            MapWindowPoints(hMainList, HWND_DESKTOP, reinterpret_cast<LPPOINT>(&rcItem), 2);

            RECT rcDlg = { 0 };
            GetWindowRect(hDlg, &rcDlg);
            int dlgW = rcDlg.right - rcDlg.left;
            int dlgH = rcDlg.bottom - rcDlg.top;

            float scale = GetDpiScaleForWindow(hDlg);
            HMONITOR hMon = MonitorFromWindow(hDlg, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoA(hMon, &mi);

            int newX = rcItem.left + ScaleI(Layout::kMainMargin, scale);
            if (newX + dlgW > mi.rcWork.right) newX = mi.rcWork.right - dlgW - 10;
            if (newX < mi.rcWork.left) newX = mi.rcWork.left + 10;

            // 当該行の上端の直上 (rcItem.top - dlgH - 4) に配置
            int newY = rcItem.top - dlgH - ScaleI(Layout::kPerAppGap, scale);
            // 画面上部からはみ出る場合は直下 (rcItem.bottom + 4) に配置
            if (newY < mi.rcWork.top + 10) {
                newY = rcItem.bottom + ScaleI(Layout::kPerAppGap, scale);
            }
            SetWindowPos(hDlg, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        auto cores = ati::CpuTopology::GetCpuCores();
        int coreCount = static_cast<int>(cores.size());
        float scale = GetDpiScaleForWindow(hDlg);


        InitAffinityListView(hDlg, IDC_LIST_RULE_CORES, scale, cores,
                             "Audio Core", "Excluded");
        HWND hList = GetDlgItem(hDlg, IDC_LIST_RULE_CORES);
        if (hList) {
            ShowScrollBar(hList, SB_VERT, FALSE);
            ListView_Scroll(hList, 0, -9999);
        }

        if (s_editRule.audioAffinityMask == 0) {
            s_editRule.audioAffinityMask = (1ULL << s_editRule.audioCore);
        }
        SyncAudioCoresFromMask(s_editRule.audioCores, s_editRule.audioAffinityMask);
        if (!s_editRule.audioCores.empty()) {
            s_editRule.audioCore = s_editRule.audioCores[0];
        }
        char aHex[32] = { 0 };
        snprintf(aHex, sizeof(aHex), "0x%llX", static_cast<unsigned long long>(s_editRule.audioAffinityMask));
        SetDlgItemTextA(hDlg, IDC_EDIT_RULE_CORE, aHex);

        DWORD_PTR initialNormal = (s_editRule.normalAffinityMask != 0) 
            ? s_editRule.normalAffinityMask 
            : g_isolator.GetConfig().normalAffinityMask;
        if (s_editRule.normalAffinityMask == 0) {
            s_editRule.normalAffinityMask = initialNormal;
        }
        char nHex[32] = { 0 };
        snprintf(nHex, sizeof(nHex), "0x%llX", static_cast<unsigned long long>(s_editRule.normalAffinityMask));
        SetDlgItemTextA(hDlg, IDC_EDIT_RULE_NORMAL_CORES, nHex);

        HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_RULE_PRIO);
        if (hCombo) {
            SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
            int selectedIdx = 0;
            for (int i = 0; i < PRIO_OPTIONS_COUNT; ++i) {
                int itemIdx = static_cast<int>(SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)PRIO_OPTIONS[i].label));
                SendMessageA(hCombo, CB_SETITEMDATA, itemIdx, PRIO_OPTIONS[i].value);
                if (PRIO_OPTIONS[i].value == s_editRule.audioPriority) {
                    selectedIdx = itemIdx;
                }
            }
            SendMessageA(hCombo, CB_SETCURSEL, selectedIdx, 0);
        }

        // 注釈テキスト (Segoe UI 8pt 小さめフォントを適用)
        HWND hNote = GetDlgItem(hDlg, IDC_STATIC_HALF_ISOLATED_NOTE);
        if (hNote) {
            HFONT hBaseFont = GetEffectiveFont(hDlg);
            LOGFONTA lf = { 0 };
            GetObjectA(hBaseFont, sizeof(lf), &lf);
            lf.lfHeight = static_cast<LONG>(-11 * scale); // 8pt 相当
            lf.lfWeight = FW_NORMAL;
            HFONT hNoteFont = CreateFontIndirectA(&lf);
            SendMessageA(hNote, WM_SETFONT, (WPARAM)hNoteFont, TRUE);
            SetWindowLongPtrA(hNote, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(hNoteFont));
        }

        // 開いた初回起動時から Cancel ボタン右端と整列させるため初期レイアウトを強制適用
        RECT rcInit;
        GetClientRect(hDlg, &rcInit);
        SendMessageA(hDlg, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rcInit.right, rcInit.bottom));
        return TRUE;
    }

    case WM_DESTROY: {
        HWND hNote = GetDlgItem(hDlg, IDC_STATIC_HALF_ISOLATED_NOTE);
        if (hNote) {
            HFONT hNoteFont = reinterpret_cast<HFONT>(GetWindowLongPtrA(hNote, GWLP_USERDATA));
            if (hNoteFont) DeleteObject(hNoteFont);
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        LPMINMAXINFO lpMMI = reinterpret_cast<LPMINMAXINFO>(lParam);
        float scale = GetDpiScaleForWindow(hDlg);
        lpMMI->ptMinTrackSize.x = static_cast<LONG>(ScaleI(Layout::kPerAppMinW, scale));
        lpMMI->ptMinTrackSize.y = static_cast<LONG>(ScaleI(Layout::kPerAppMinH, scale));
        return 0;
    }

    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) return TRUE;
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        if (cx <= 0 || cy <= 0) return TRUE;

        float scale = GetDpiScaleForWindow(hDlg);
        int margin = ScaleI(Layout::kDialogMargin, scale);
        int spacing = ScaleI(Layout::kCtrlSpacing, scale);

        ResizeDialogControlWidth(hDlg, IDC_LIST_RULE_CORES, cx, margin);
        ResizeDialogControlWidth(hDlg, IDC_STATIC_HALF_ISOLATED_NOTE, cx, margin);

        HWND hBtnCancel = GetDlgItem(hDlg, IDC_BTN_RULE_CANCEL);
        HWND hBtnOk = GetDlgItem(hDlg, IDC_BTN_RULE_OK);
        if (hBtnCancel && hBtnOk) {
            RECT rcCancel, rcOk;
            GetWindowRect(hBtnCancel, &rcCancel);
            GetWindowRect(hBtnOk, &rcOk);
            int btnW = rcCancel.right - rcCancel.left;
            int btnH = rcCancel.bottom - rcCancel.top;
            int btnY = cy - margin - btnH;
            int cancelX = cx - margin - btnW;
            int okX = cancelX - spacing - btnW;
            SetWindowPos(hBtnCancel, nullptr, cancelX, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(hBtnOk, nullptr, okX, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        InvalidateRect(hDlg, nullptr, TRUE);
        return TRUE;
    }

    case WM_NOTIFY: {
        LPNMHDR pnm = reinterpret_cast<LPNMHDR>(lParam);
        if (pnm && pnm->idFrom == IDC_LIST_RULE_CORES) {
            auto cores = ati::CpuTopology::GetCpuCores();
            int coreCount = static_cast<int>(cores.size());

            if (pnm->code == NM_CUSTOMDRAW) {
                return HandleAffinityCustomDraw(hDlg, IDC_LIST_RULE_CORES, lParam, cores,
                    [](int item, int logicalIdx) {
                        if (item == 0) {
                            return (s_editRule.audioAffinityMask & (1ULL << logicalIdx)) != 0;
                        } else if (item == 1) {
                            return (s_editRule.normalAffinityMask & (1ULL << logicalIdx)) != 0;
                        }
                        return false;
                    });
            } else if (pnm->code == NM_CLICK) {
                LPNMITEMACTIVATE pia = reinterpret_cast<LPNMITEMACTIVATE>(lParam);
                if (pia && pia->iItem >= 0 && pia->iSubItem >= 1 && pia->iSubItem <= coreCount) {
                    int logicalIdx = cores[pia->iSubItem - 1].logicalIndex;
                    DWORD_PTR bit = (1ULL << logicalIdx);
                    if (pia->iItem == 0) {
                        // 行 0: Audio Core
                        if (s_editRule.audioAffinityMask & bit) {
                            // チェックを外す: 最低 1 コア保護 (最後の 1 個なら変更不可)
                            if ((s_editRule.audioAffinityMask & ~bit) != 0) {
                                s_editRule.audioAffinityMask &= ~bit;
                            }
                        } else {
                            // チェックを入れる: 相互排他インターロック
                            // Other than から外すが、Other than の最後の 1 個なら変更不可
                            if ((s_editRule.normalAffinityMask & bit) != 0 && (s_editRule.normalAffinityMask & ~bit) == 0) {
                                // Other than が空になってしまうため変更不可
                            } else {
                                s_editRule.audioAffinityMask |= bit;
                                s_editRule.normalAffinityMask &= ~bit; // 相互排他
                            }
                        }
                        SyncAudioCoresFromMask(s_editRule.audioCores, s_editRule.audioAffinityMask);
                        int firstCore = 0;
                        if (!s_editRule.audioCores.empty()) {
                            firstCore = s_editRule.audioCores[0];
                        }
                        s_editRule.audioCore = firstCore;
                    } else if (pia->iItem == 1) {
                        // 行 1: Other than Audio
                        if (s_editRule.normalAffinityMask & bit) {
                            // チェックを外す: 最低 1 コア保護 (最後の 1 個なら変更不可)
                            if ((s_editRule.normalAffinityMask & ~bit) != 0) {
                                s_editRule.normalAffinityMask &= ~bit;
                            }
                        } else {
                            // チェックを入れる: 相互排他インターロック
                            // Audio Core から外すが、Audio Core の最後の 1 個なら変更不可
                            if ((s_editRule.audioAffinityMask & bit) != 0 && (s_editRule.audioAffinityMask & ~bit) == 0) {
                                // Audio Core が空になってしまうため変更不可
                            } else {
                                s_editRule.normalAffinityMask |= bit;
                                s_editRule.audioAffinityMask &= ~bit; // 相互排他
                                SyncAudioCoresFromMask(s_editRule.audioCores, s_editRule.audioAffinityMask);
                                int firstCore = 0;
                                if (!s_editRule.audioCores.empty()) {
                                    firstCore = s_editRule.audioCores[0];
                                }
                                s_editRule.audioCore = firstCore;
                            }
                        }
                    }
                    char aHex[32] = { 0 };
                    snprintf(aHex, sizeof(aHex), "0x%llX", static_cast<unsigned long long>(s_editRule.audioAffinityMask));
                    SetDlgItemTextA(hDlg, IDC_EDIT_RULE_CORE, aHex);
                    char hexBuf[32] = { 0 };
                    snprintf(hexBuf, sizeof(hexBuf), "0x%llX", static_cast<unsigned long long>(s_editRule.normalAffinityMask));
                    SetDlgItemTextA(hDlg, IDC_EDIT_RULE_NORMAL_CORES, hexBuf);

                    HWND hList = GetDlgItem(hDlg, IDC_LIST_RULE_CORES);
                    InvalidateRect(hList, nullptr, FALSE);
                    return TRUE;
                }
            }
        }
        break;
    }

    case WM_COMMAND: {
        WORD cmdId = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (code == EN_CHANGE && cmdId == IDC_EDIT_RULE_CORE) {
            char buf[128] = { 0 };
            GetDlgItemTextA(hDlg, IDC_EDIT_RULE_CORE, buf, sizeof(buf));
            std::string s(buf);
            DWORD_PTR m = 0;
            int fc = 0;
            if (s.length() > 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
                m = _strtoui64(s.c_str(), nullptr, 16);
                for (int c = 0; c < 64; ++c) {
                    if ((m & (1ULL << c)) != 0) { fc = c; break; }
                }
            } else {
                m = ParseCoreListToMask(s, fc);
            }
            if (m != 0) {
                s_editRule.audioAffinityMask = m;
                s_editRule.audioCore = fc;
                SyncAudioCoresFromMask(s_editRule.audioCores, s_editRule.audioAffinityMask);
                HWND hList = GetDlgItem(hDlg, IDC_LIST_RULE_CORES);
                if (hList) InvalidateRect(hList, nullptr, FALSE);
            }
        }

        if (code == EN_CHANGE && cmdId == IDC_EDIT_RULE_NORMAL_CORES) {
            char buf[128] = { 0 };
            GetDlgItemTextA(hDlg, IDC_EDIT_RULE_NORMAL_CORES, buf, sizeof(buf));
            std::string s(buf);
            if (s.length() > 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
                s_editRule.normalAffinityMask = _strtoui64(s.c_str(), nullptr, 16);
            } else {
                int fc = 0;
                s_editRule.normalAffinityMask = ParseCoreListToMask(s, fc);
            }
            HWND hList = GetDlgItem(hDlg, IDC_LIST_RULE_CORES);
            if (hList) InvalidateRect(hList, nullptr, FALSE);
        }

        if (cmdId == IDC_BTN_RULE_OK) {
            HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_RULE_PRIO);
            if (hCombo) {
                int sel = static_cast<int>(SendMessageA(hCombo, CB_GETCURSEL, 0, 0));
                if (sel != CB_ERR) {
                    s_editRule.audioPriority = static_cast<int>(SendMessageA(hCombo, CB_GETITEMDATA, sel, 0));
                    if (!s_editRule.audioPriorities.empty()) {
                        s_editRule.audioPriorities[0] = s_editRule.audioPriority;
                    }
                    s_editRule.wasHalfAutoPromoted = true; // ユーザー明示設定を保護
                }
            }

            char buf[128] = { 0 };
            GetDlgItemTextA(hDlg, IDC_EDIT_RULE_CORE, buf, sizeof(buf));
            std::string s(buf);
            DWORD_PTR m = 0;
            int fc = 0;
            if (s.length() > 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
                m = _strtoui64(s.c_str(), nullptr, 16);
                for (int c = 0; c < 64; ++c) {
                    if ((m & (1ULL << c)) != 0) { fc = c; break; }
                }
            } else {
                m = ParseCoreListToMask(s, fc);
            }
            if (m != 0) {
                s_editRule.audioAffinityMask = m;
                s_editRule.audioCore = fc;
                SyncAudioCoresFromMask(s_editRule.audioCores, s_editRule.audioAffinityMask);
                if (!s_editRule.audioCores.empty()) {
                    s_editRule.audioCore = s_editRule.audioCores[0];
                }
            }

            char nBuf[128] = { 0 };
            GetDlgItemTextA(hDlg, IDC_EDIT_RULE_NORMAL_CORES, nBuf, sizeof(nBuf));
            std::string ns(nBuf);
            if (ns.length() > 2 && (ns.substr(0, 2) == "0x" || ns.substr(0, 2) == "0X")) {
                s_editRule.normalAffinityMask = _strtoui64(ns.c_str(), nullptr, 16);
            } else {
                int nfc = 0;
                s_editRule.normalAffinityMask = ParseCoreListToMask(ns, nfc);
            }

            g_isolator.UpdateRule(s_editRuleIndex, s_editRule);
            auto updated = g_isolator.GetConfig();
            SaveConfig(updated, g_iniPath);

            HWND hList = GetDlgItem(g_hMainDlg, IDC_LIST_PROCESSES);
            if (hList) {
                RefreshListView(hList, updated.rules);
                InvalidateRect(hList, nullptr, FALSE);
            }

            EndDialog(hDlg, IDOK);
            return TRUE;
        } else if (cmdId == IDC_BTN_RULE_CANCEL || cmdId == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static void OpenRuleEditDialog(HWND hParent, size_t index) {
    auto rules = g_isolator.GetRulesSnapshot();
    if (index >= rules.size()) return;

    s_editRuleIndex = index;
    s_editRule = rules[index];
    if (s_editRule.audioAffinityMask != 0) {
        SyncAudioCoresFromMask(s_editRule.audioCores, s_editRule.audioAffinityMask);
        if (!s_editRule.audioCores.empty()) {
            s_editRule.audioCore = s_editRule.audioCores[0];
        }
    }

    DialogBoxA(g_hInstance, MAKEINTRESOURCEA(IDD_RULE_EDIT_DIALOG), hParent, RuleEditDlgProc);
}

// ヘッダーサブクラス化プロシージャ (Col 0 の "!" を鮮やかな赤文字かつ太字で中央描画)
static WNDPROC s_pfnOriginalHeaderProc = nullptr;

// ソート確認ダイアログプロシージャ (タイトル「SORT?」、本文・アイコンなし、OK/Cancelボタンのみ、親ウィンドウ真上にセンタリング)
static INT_PTR CALLBACK SortConfirmDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND hParent = reinterpret_cast<HWND>(lParam);
        if (!hParent) hParent = GetParent(hDlg);
        if (hParent) {
            RECT rcParent, rcDlg;
            GetWindowRect(hParent, &rcParent);
            GetWindowRect(hDlg, &rcDlg);
            int dlgW = rcDlg.right - rcDlg.left;
            int dlgH = rcDlg.bottom - rcDlg.top;
            int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
            int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;
            SetWindowPos(hDlg, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK || id == IDCANCEL) {
            EndDialog(hDlg, id);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE: {
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    }
    return FALSE;
}

// プロセス名昇順ソート確認＆実行ヘルパー (Process Name カラム見出しクリック時専用)
static void SortProcessesByName(HWND hDlg) {
    static bool s_inSortPrompt = false;
    if (s_inSortPrompt) return;
    s_inSortPrompt = true;

    INT_PTR ret = DialogBoxParamA(
        g_hInstance, 
        MAKEINTRESOURCEA(IDD_SORT_CONFIRM), 
        hDlg, 
        SortConfirmDlgProc, 
        reinterpret_cast<LPARAM>(hDlg)
    );

    if (ret == IDOK) {
        g_isolator.SortRulesByName();
        auto updated = g_isolator.GetConfig();
        // INI保存は即時実行せず終了処理時のみとする
        HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
        if (hList) {
            RefreshListView(hList, updated.rules);
        }
    }

    s_inSortPrompt = false;
}

static POINT s_ptHeaderDown = { 0, 0 };

static LRESULT CALLBACK CustomHeaderProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_LBUTTONDOWN) {
        s_ptHeaderDown.x = static_cast<short>(LOWORD(lParam));
        s_ptHeaderDown.y = static_cast<short>(HIWORD(lParam));
    }

    LRESULT lRes = CallWindowProcA(s_pfnOriginalHeaderProc, hWnd, msg, wParam, lParam);

    if (msg == WM_LBUTTONUP) {
        POINT ptUp;
        ptUp.x = static_cast<short>(LOWORD(lParam));
        ptUp.y = static_cast<short>(HIWORD(lParam));
        int dx = abs(ptUp.x - s_ptHeaderDown.x);
        int dy = abs(ptUp.y - s_ptHeaderDown.y);

        // ドラッグ幅変更中等の移動を排除（3px以内の純粋なクリックのみ）
        if (dx <= 3 && dy <= 3) {
            // クリックを離した瞬間に最新のカラム矩形を取得
            RECT rcCol1 = { 0 };
            if (SendMessageA(hWnd, HDM_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&rcCol1))) {
                int colW = rcCol1.right - rcCol1.left;
                if (colW > 0) {
                    int margin = static_cast<int>(colW * 0.10f); // 左右10%（境界ドラッグマージン）
                    int minX = rcCol1.left + margin;
                    int maxX = rcCol1.right - margin;
                    // 幅の中央80%範囲内をクリックした場合のみソート発火
                    if (ptUp.x >= minX && ptUp.x <= maxX) {
                        HWND hList = GetParent(hWnd);
                        HWND hDlg = hList ? GetParent(hList) : nullptr;
                        if (hDlg) {
                            SortProcessesByName(hDlg);
                        }
                    }
                }
            }
        }
    }

    if (msg == WM_PAINT) {
        RECT rcItem;
        if (SendMessageA(hWnd, HDM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rcItem))) {
            HDC hdc = GetDC(hWnd);
            if (hdc) {
                // ヘッダー背景をクリアして標準の黒文字残余を消去
                RECT rcFill = rcItem;
                InflateRect(&rcFill, -1, -1);
                FillRect(hdc, &rcFill, GetSysColorBrush(COLOR_BTNFACE));

                HFONT hFont = GetEffectiveFont(hWnd);
                LOGFONTA lf = { 0 };
                GetObjectA(hFont, sizeof(lf), &lf);
                lf.lfWeight = FW_BOLD; // 太字
                HFONT hBoldFont = CreateFontIndirectA(&lf);

                HGDIOBJ oldFont = SelectObject(hdc, hBoldFont);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(220, 50, 50)); // 鮮やかな赤

                RECT rcText = rcItem;
                DrawTextA(hdc, "!", 1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdc, oldFont);
                DeleteObject(hBoldFont);
                ReleaseDC(hWnd, hdc);
            }
        }
        RECT rcItem4; // Col 4: || (Pause)
        if (SendMessageA(hWnd, HDM_GETITEMRECT, 4, reinterpret_cast<LPARAM>(&rcItem4))) {
            HDC hdc = GetDC(hWnd);
            if (hdc) {
                RECT rcFill = rcItem4;
                InflateRect(&rcFill, -1, -1);
                FillRect(hdc, &rcFill, GetSysColorBrush(COLOR_BTNFACE));

                float scale = GetDpiScaleForWindow(hWnd);
                int barW = (std::max)(1, ScaleI(Layout::kBarW, scale));
                int barH = (std::max)(2, ScaleI(Layout::kBarH, scale));
                int gap  = (std::max)(1, ScaleI(Layout::kBarGap, scale));
                int totalW = barW * 2 + gap;
                int startX = rcItem4.left + ((rcItem4.right - rcItem4.left) - totalW) / 2;
                int startY = rcItem4.top + ((rcItem4.bottom - rcItem4.top) - barH) / 2;

                RECT rcBar1 = { startX, startY, startX + barW, startY + barH };
                RECT rcBar2 = { startX + barW + gap, startY, startX + totalW, startY + barH };
                HBRUSH hIconBr = CreateSolidBrush(RGB(60, 60, 60));
                FillRect(hdc, &rcBar1, hIconBr);
                FillRect(hdc, &rcBar2, hIconBr);
                DeleteObject(hIconBr);

                ReleaseDC(hWnd, hdc);
            }
        }
    }
    return lRes;
}

// リストビューサブクラス化プロシージャ (Priority 列の直接クリック検知・2D Matrix Picker ポップアップ)
static WNDPROC s_pfnOriginalListProc = nullptr;

static LRESULT CALLBACK CustomListProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_STYLECHANGING) {
        if (wParam == GWL_STYLE) {
            STYLESTRUCT* pSS = reinterpret_cast<STYLESTRUCT*>(lParam);
            pSS->styleNew &= ~WS_HSCROLL;
        }
    }
    if (msg == WM_NCCALCSIZE) {
        LONG_PTR style = GetWindowLongPtrA(hWnd, GWL_STYLE);
        if (style & WS_HSCROLL) {
            SetWindowLongPtrA(hWnd, GWL_STYLE, style & ~WS_HSCROLL);
        }
    }
    if (msg == WM_ERASEBKGND) {
        // リサイズ時や露出時にアイテム未存在の余白領域を白(COLOR_WINDOW)で確実に消去し、
        // リサイズ前の古い凡例等の残像・透け込みを防止する
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        FillRect(hdc, &rcClient, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    if (msg == WM_LBUTTONDOWN) {
        LVHITTESTINFO lvhti = { 0 };
        lvhti.pt.x = static_cast<short>(LOWORD(lParam));
        lvhti.pt.y = static_cast<short>(HIWORD(lParam));
        ListView_SubItemHitTest(hWnd, &lvhti);

        if (lvhti.iItem >= 0 && (lvhti.iSubItem == 2 || lvhti.iSubItem == 3)) {
            // Col 2: Thread Priority または Col 3: Process Priority のクリックで新UI (2D Priority Matrix Picker) をポップアップ
            HWND hDlg = GetParent(hWnd);
            auto rules = g_isolator.GetRulesSnapshot();
            size_t rowIdx = static_cast<size_t>(lvhti.iItem);
            if (rowIdx < g_displayRows.size()) {
                size_t ruleIdx = g_displayRows[rowIdx].ruleIndex;
                size_t subIdx = g_displayRows[rowIdx].subIndex;
                if (ruleIdx < rules.size()) {
                    const auto& r = rules[ruleIdx];
                    int currentPrio = (subIdx < r.audioPriorities.size()) ? r.audioPriorities[subIdx] : r.audioPriority;
                    int initialRank = ati::FindRankByPrio(r.processPriorityClass, currentPrio);
                    if (initialRank <= 0) initialRank = 25; // デフォルト (Normal/Normal)

                    ati::ShowPriorityMatrixPicker(hDlg, initialRank, [hDlg, hWnd, ruleIdx, subIdx](int selectedRank) {
                        const auto* entry = ati::GetMatrixEntryByRank(selectedRank);
                        if (!entry) return;

                        auto config = g_isolator.GetConfig();
                        if (ruleIdx < config.rules.size()) {
                            auto& targetRule = config.rules[ruleIdx];
                            targetRule.processPriorityClass = entry->processPriorityClass;

                            size_t requiredSlots = (targetRule.ignoreSig && targetRule.ignoreSigCount > 1)
                                ? static_cast<size_t>(targetRule.ignoreSigCount) : 1;
                            if (targetRule.audioPriorities.size() < requiredSlots) {
                                targetRule.audioPriorities.resize(requiredSlots, targetRule.audioPriority);
                            }
                            if (subIdx < targetRule.audioPriorities.size()) {
                                targetRule.audioPriorities[subIdx] = entry->threadPriority;
                            }
                            if (subIdx == 0) {
                                targetRule.audioPriority = entry->threadPriority;
                            }
                            g_isolator.UpdateConfig(config);

                            // 稼働中プロセスが存在する場合、即時 SetPriorityClass を発行
                            if (targetRule.activePid != 0 && entry->processPriorityClass != 0) {
                                HANDLE hP = OpenProcess(PROCESS_SET_INFORMATION, FALSE, targetRule.activePid);
                                if (hP) {
                                    if (!SetPriorityClass(hP, entry->processPriorityClass) && entry->processPriorityClass == REALTIME_PRIORITY_CLASS) {
                                        SetPriorityClass(hP, HIGH_PRIORITY_CLASS);
                                    }
                                    CloseHandle(hP);
                                }
                            }

                            // 稼働中スレッド・プロセスへ即時反映
                            g_isolator.ScanAndIsolate();

                            // INI 保存
                            SaveConfig(config, g_iniPath);

                            // リストビュー表示を即時更新
                            auto updated = g_isolator.GetRulesSnapshot();
                            UpdateListViewDynamic(hWnd, updated);
                            InvalidateRect(hWnd, nullptr, FALSE);
                        }
                    });
                    return 0; // デフォルトの行選択を抑止しピッカー展開に専念
                }
            }
        }
    } else if (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL) {
        if (ati::IsPriorityMatrixPickerOpen()) {
            ati::ClosePriorityMatrixPicker();
        }
    }
    LRESULT lRes = CallWindowProcA(s_pfnOriginalListProc, hWnd, msg, wParam, lParam);
    if (msg == WM_NCCALCSIZE || msg == WM_SIZE || msg == WM_WINDOWPOSCHANGED || msg == WM_PAINT) {
        LONG_PTR style = GetWindowLongPtrA(hWnd, GWL_STYLE);
        if (style & WS_HSCROLL) {
            SetWindowLongPtrA(hWnd, GWL_STYLE, style & ~WS_HSCROLL);
        }
        ShowScrollBar(hWnd, SB_HORZ, FALSE);
    }
    return lRes;
}

// --- 管理者権限判定ヘルパー ---
static bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

// --- Realtime 優先度用特権有効化ヘルパー ---
static bool EnableRealtimePrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }
    TOKEN_PRIVILEGES tp = { 0 };
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueA(nullptr, "SeIncreaseBasePriorityPrivilege", &tp.Privileges[0].Luid)) {
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    DWORD err = GetLastError();
    CloseHandle(hToken);
    return (err == ERROR_SUCCESS);
}

// --- アプリケーション再起動ヘルパー ---
static void RestartApplication(HWND hDlg) {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    KillTimer(hDlg, TIMER_POLLING_ID);
    RemoveTrayIcon();
    g_isolator.ResumeAllSuspendedThreads();
    bool wasInTray = !IsWindowVisible(hDlg) || IsIconic(hDlg);
    const char* params = wasInTray ? "--tray" : nullptr;
    if (g_hSingleInstanceMutex) {
        CloseHandle(g_hSingleInstanceMutex);
        g_hSingleInstanceMutex = nullptr;
    }
    ShellExecuteA(nullptr, "open", exePath, params, nullptr, wasInTray ? SW_HIDE : SW_SHOW);
    DestroyWindow(hDlg);
}

// --- 管理者として再起動ヘルパー (UAC 昇格) ---
static void RestartAsAdmin(HWND hDlg) {
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    bool wasInTray = !IsWindowVisible(hDlg) || IsIconic(hDlg);
    const char* params = wasInTray ? "--tray" : nullptr;

    HINSTANCE hInst = ShellExecuteA(hDlg, "runas", exePath, params, nullptr, wasInTray ? SW_HIDE : SW_SHOW);
    if (reinterpret_cast<INT_PTR>(hInst) > 32) {
        KillTimer(hDlg, TIMER_POLLING_ID);
        RemoveTrayIcon();
        g_isolator.ResumeAllSuspendedThreads();
        if (g_hSingleInstanceMutex) {
            CloseHandle(g_hSingleInstanceMutex);
            g_hSingleInstanceMutex = nullptr;
        }
        DestroyWindow(hDlg);
    }
}

static bool s_inSizeMove = false;
static HFONT s_hMainFont = nullptr;
static WNDPROC s_pfnOriginalCheckboxProc = nullptr;
static bool s_bCheckboxHot = false;

static LRESULT CALLBACK CustomCheckboxProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        // 背景塗りつぶし (親ダイアログ標準背景色)
        FillRect(hdc, &rcClient, GetSysColorBrush(COLOR_BTNFACE));

        float scale = GetDpiScaleForWindow(hWnd);
        UINT dpi = static_cast<UINT>(96 * scale);
        if (dpi == 0) dpi = 96;

        bool isChecked = (SendMessageA(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
        LRESULT bState = SendMessageA(hWnd, BM_GETSTATE, 0, 0);
        bool isPushed = (bState & BST_PUSHED) != 0;

        // UxTheme による正確な DPI アイコン描画
        bool themeDrawn = false;
        HMODULE hUxTheme = GetModuleHandleA("uxtheme.dll");
        if (!hUxTheme) hUxTheme = LoadLibraryA("uxtheme.dll");
        if (hUxTheme) {
            typedef HTHEME (WINAPI *PFN_OpenThemeDataForDpi)(HWND, LPCWSTR, UINT);
            typedef HTHEME (WINAPI *PFN_OpenThemeData)(HWND, LPCWSTR);
            typedef HRESULT (WINAPI *PFN_DrawThemeBackground)(HTHEME, HDC, int, int, const RECT*, const RECT*);
            typedef HRESULT (WINAPI *PFN_GetThemePartSize)(HTHEME, HDC, int, int, const RECT*, THEMESIZE, SIZE*);
            typedef HRESULT (WINAPI *PFN_CloseThemeData)(HTHEME);

            PFN_OpenThemeDataForDpi pfnOpenDpi = (PFN_OpenThemeDataForDpi)GetProcAddress(hUxTheme, "OpenThemeDataForDpi");
            PFN_OpenThemeData pfnOpen = (PFN_OpenThemeData)GetProcAddress(hUxTheme, "OpenThemeData");
            PFN_DrawThemeBackground pfnDrawBg = (PFN_DrawThemeBackground)GetProcAddress(hUxTheme, "DrawThemeBackground");
            PFN_GetThemePartSize pfnGetSize = (PFN_GetThemePartSize)GetProcAddress(hUxTheme, "GetThemePartSize");
            PFN_CloseThemeData pfnCloseTheme = (PFN_CloseThemeData)GetProcAddress(hUxTheme, "CloseThemeData");

            if (pfnDrawBg && pfnCloseTheme) {
                HTHEME hTheme = nullptr;
                if (pfnOpenDpi) {
                    hTheme = pfnOpenDpi(hWnd, L"BUTTON", dpi);
                } else if (pfnOpen) {
                    hTheme = pfnOpen(hWnd, L"BUTTON");
                }

                if (hTheme) {
                    int stateId = isChecked 
                        ? (isPushed ? CBS_CHECKEDPRESSED : (s_bCheckboxHot ? CBS_CHECKEDHOT : CBS_CHECKEDNORMAL))
                        : (isPushed ? CBS_UNCHECKEDPRESSED : (s_bCheckboxHot ? CBS_UNCHECKEDHOT : CBS_UNCHECKEDNORMAL));

                    SIZE sz = { static_cast<LONG>(13 * scale), static_cast<LONG>(13 * scale) };
                    if (pfnGetSize) {
                        SIZE szTheme = { 0, 0 };
                        if (SUCCEEDED(pfnGetSize(hTheme, hdc, BP_CHECKBOX, stateId, nullptr, TS_TRUE, &szTheme)) && szTheme.cx > 0 && szTheme.cy > 0) {
                            sz = szTheme;
                        }
                    }

                    int cbX = 0;
                    int cbY = (rcClient.bottom - rcClient.top - sz.cy) / 2;
                    RECT rcBox = { cbX, cbY, cbX + sz.cx, cbY + sz.cy };

                    if (SUCCEEDED(pfnDrawBg(hTheme, hdc, BP_CHECKBOX, stateId, &rcBox, nullptr))) {
                        themeDrawn = true;
                    }
                    pfnCloseTheme(hTheme);

                    // テキスト描画
                    char text[64] = { 0 };
                    GetWindowTextA(hWnd, text, sizeof(text));
                    if (text[0] != '\0') {
                        HFONT hFont = reinterpret_cast<HFONT>(SendMessageA(hWnd, WM_GETFONT, 0, 0));
                        if (!hFont) hFont = s_hMainFont;
                        HGDIOBJ oldFont = SelectObject(hdc, hFont);
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

                        RECT rcText = rcClient;
                        rcText.left = cbX + sz.cx + ScaleI(Layout::kBtnSpacing, scale);
                        DrawTextA(hdc, text, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                        SelectObject(hdc, oldFont);
                    }
                }
            }
        }

        // フォールバック: DrawFrameControl
        if (!themeDrawn) {
            int cbSize = ScaleI(Layout::kCheckboxSize, scale);
            int cbX = 0;
            int cbY = (rcClient.bottom - rcClient.top - cbSize) / 2;
            RECT rcBox = { cbX, cbY, cbX + cbSize, cbY + cbSize };
            UINT uState = DFCS_BUTTONCHECK | (isChecked ? DFCS_CHECKED : 0) | (isPushed ? DFCS_PUSHED : 0);
            DrawFrameControl(hdc, &rcBox, DFC_BUTTON, uState);

            char text[64] = { 0 };
            GetWindowTextA(hWnd, text, sizeof(text));
            if (text[0] != '\0') {
                HFONT hFont = reinterpret_cast<HFONT>(SendMessageA(hWnd, WM_GETFONT, 0, 0));
                if (!hFont) hFont = s_hMainFont;
                HGDIOBJ oldFont = SelectObject(hdc, hFont);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

                RECT rcText = rcClient;
                rcText.left = cbX + cbSize + ScaleI(Layout::kBtnSpacing, scale);
                DrawTextA(hdc, text, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                SelectObject(hdc, oldFont);
            }
        }

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!s_bCheckboxHot) {
            s_bCheckboxHot = true;
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }

    case WM_MOUSELEAVE: {
        s_bCheckboxHot = false;
        InvalidateRect(hWnd, nullptr, FALSE);
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    default:
        break;
    }
    return CallWindowProcA(s_pfnOriginalCheckboxProc, hWnd, msg, wParam, lParam);
}

static void UpdateDialogFonts(HWND hDlg, float scale) {
    int fontHeight = -MulDiv(Layout::kFontPt, ScaleI(96, scale), 72);
    HFONT hNewFont = CreateFontA(
        fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
    );
    if (!hNewFont) return;

    SendMessageA(hDlg, WM_SETFONT, reinterpret_cast<WPARAM>(hNewFont), TRUE);

    // 子コントロール巡回適用
    EnumChildWindows(hDlg, [](HWND hWndChild, LPARAM lParam) -> BOOL {
        SendMessageA(hWndChild, WM_SETFONT, static_cast<WPARAM>(lParam), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(hNewFont));

    HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
    if (hList) {
        HWND hHeader = ListView_GetHeader(hList);
        if (hHeader) {
            SendMessageA(hHeader, WM_SETFONT, reinterpret_cast<WPARAM>(hNewFont), TRUE);
        }
    }

    HWND hChk = GetDlgItem(hDlg, IDC_CHK_ALWAYS_ON_TOP);
    if (hChk) {
        InvalidateRect(hChk, nullptr, TRUE);
    }

    if (s_hMainFont) {
        DeleteObject(s_hMainFont);
    }
    s_hMainFont = hNewFont;
}

static void UpdateListViewColumnsDpi(HWND hDlg, float scale) {
    HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
    if (!hList) return;

    const std::vector<int>& widths = (!g_colWidths.empty() && g_colWidths.size() == kColCount) 
        ? g_colWidths 
        : std::vector<int>(kColDefaultWidths, kColDefaultWidths + kColCount);

    for (int i = 0; i < kColCount; ++i) {
        ListView_SetColumnWidth(hList, i, ScaleI(widths[i], scale));
    }

    ShowScrollBar(hList, SB_HORZ, FALSE);
}

static void AdjustWindowSizeToContent(HWND hDlg, float scale) {
    HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
    if (!hList) return;

    HWND hHeader = ListView_GetHeader(hList);

    // 1. 各カラムの幅の合計 (Col 0〜8 全列)
    const std::vector<int>& widths = (!g_colWidths.empty() && g_colWidths.size() == kColCount) 
        ? g_colWidths 
        : std::vector<int>(kColDefaultWidths, kColDefaultWidths + kColCount);

    int totalBaseColW = 0;
    for (int i = 0; i < kColCount; ++i) totalBaseColW += ScaleI(widths[i], scale);

    // 2. 登録行数と 1 行あたりの高さ
    int itemCount = ListView_GetItemCount(hList);
    int itemH = ScaleI(18, scale);
    if (itemCount > 0) {
        RECT rcItem = { 0 };
        if (ListView_GetItemRect(hList, 0, &rcItem, LVIR_BOUNDS)) {
            int h = rcItem.bottom - rcItem.top;
            if (h > 0) itemH = h;
        }
    }

    int headerH = ScaleI(20, scale);
    if (hHeader) {
        RECT rcHdr = { 0 };
        GetWindowRect(hHeader, &rcHdr);
        int h = rcHdr.bottom - rcHdr.top;
        if (h > 0) headerH = h;
    }

    int margin = ScaleI(10, scale);
    int rightColW = ScaleI(115, scale);
    int bottomLegendH = ScaleI(18, scale); // ボタン高さ分 (20*scale) 縮小し、旧ステータス余白を排除 (38 -> 18)
    int sbW = GetSystemMetrics(SM_CXVSCROLL); // 垂直スクロールバー幅

    // 登録行数がすべてスクロールバーなしで収まるために必要な ListView 高さ
    int rows = (itemCount > 0) ? itemCount : 5;
    int reqListH = headerH + rows * itemH + ScaleI(4, scale);

    // 現在のウィンドウ矩形とクライアント矩形から外枠（ボーダー・タイトルバー）の差分を取得
    RECT rcDlg, rcCl;
    GetWindowRect(hDlg, &rcDlg);
    GetClientRect(hDlg, &rcCl);
    int borderW = (rcDlg.right - rcDlg.left) - (rcCl.right - rcCl.left);
    int borderH = (rcDlg.bottom - rcDlg.top) - (rcCl.bottom - rcCl.top);

    int curClientW = rcCl.right - rcCl.left;
    int curClientH = rcCl.bottom - rcCl.top;

    int newClientH = margin + reqListH + bottomLegendH + margin;

    // INI に WindowHeight が指定されている場合は優先復元
    if (g_windowHeight > 0) {
        int customWndH = ScaleI(g_windowHeight, scale);
        int customClientH = customWndH - borderH;
        if (customClientH > 0) {
            newClientH = customClientH;
        }
    }

    // ListView に割り当てられる高さ listH から、表示可能行数 n を算出
    int listH = newClientH - margin - margin - bottomLegendH;
    int effectiveListH = listH - headerH - ScaleI(4, scale);
    int n = (itemH > 0 && effectiveListH > 0) ? (effectiveListH / itemH) : 5;

    // 登録アプリ数 > n の場合にのみ縦スクロールバーが出るので、その分ウィンドウ幅を増やす
    int vScrollW = (itemCount > n) ? sbW : 0;

    // ウィンドウ幅は余白を内容に縮めてジャストフィット (+ 縦スクロールバー分)
    int newClientW = margin + totalBaseColW + ScaleI(4, scale) + vScrollW + rightColW + margin;

    if (curClientW != newClientW || curClientH != newClientH) {
        int newWndW = newClientW + borderW;
        int newWndH = newClientH + borderH;
        SetWindowPos(hDlg, nullptr, 0, 0, newWndW, newWndH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void RelayoutMainDialog(HWND hDlg, float scale, int cx, int cy) {
    if (cx <= 0 || cy <= 0) return;

    int margin = ScaleI(Layout::kMainMargin, scale);
    int rightColW = ScaleI(Layout::kRightColW, scale);
    int bottomLegendH = ScaleI(Layout::kBottomLegendH, scale); // ボタン高さ分 (20*scale) 縮小 (38 -> 18)

    // 1. ListView のリサイズ
    HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
    if (hList) {
        int listX = margin;
        int listY = margin;
        int listW = cx - margin - listX - rightColW;
        int listH = cy - margin - listY - bottomLegendH;
        if (listW < 100) listW = 100;
        if (listH < 100) listH = 100;
        SetWindowPos(hList, nullptr, listX, listY, listW, listH, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 2. 右パネル有効領域の定義とコントロールの水平センタリング配置
    int panelLeft = cx - margin - rightColW;
    int panelW = rightColW;

    int btnW = ScaleI(Layout::kBtnW, scale);
    int btnH = ScaleI(Layout::kBtnH, scale);
    int chkW = ScaleI(Layout::kChkW, scale);
    int chkH = ScaleI(Layout::kChkH, scale);

    int btnX = panelLeft + (panelW - btnW) / 2;
    int chkX = btnX;

    // Y 座標を上から整然と配置
    int curY = margin;
    HWND hChkTop = GetDlgItem(hDlg, IDC_CHK_ALWAYS_ON_TOP);
    if (hChkTop) {
        SetWindowPos(hChkTop, nullptr, chkX, curY, chkW, chkH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    curY += chkH + ScaleI(Layout::kChkTopPad, scale);

    auto PlaceBtnAt = [&](int ctrlId, int y) {
        HWND hBtn = GetDlgItem(hDlg, ctrlId);
        if (hBtn) {
            SetWindowPos(hBtn, nullptr, btnX, y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    // ToTray ボタンは廃止 (非表示化)
    HWND hBtnHide = GetDlgItem(hDlg, IDC_BTN_HIDE);
    if (hBtnHide) {
        ShowWindow(hBtnHide, SW_HIDE);
    }

    // 上部ボタングループ: EDIT -> ADD -> REMOVE
    int btnSpacing = ScaleI(Layout::kBtnSpacing, scale);
    int yEdit = curY;
    int yAdd = yEdit + btnH + btnSpacing;
    int yRemove = yAdd + btnH + btnSpacing;

    PlaceBtnAt(IDC_BTN_EDIT, yEdit);
    PlaceBtnAt(IDC_BTN_ADD, yAdd);
    PlaceBtnAt(IDC_BTN_REMOVE, yRemove);

    // 下部ボタングループ: EXIT は常にクライアント下端から margin 内側
    int exitY = cy - margin - btnH;
    if (exitY < margin) exitY = margin;

    // SETTINGS ボタンの配置:
    // 高さに余裕がある場合は従来の枠4（空きスペース）を保持し、
    // ウィンドウ高さを縮めた際は EXIT と衝突しないよう空きスペースを自動圧縮して上に追従
    int defaultSettingsY = yRemove + btnH + btnSpacing + btnH + btnSpacing; // 従来の空き枠込み
    int maxSettingsY = exitY - btnSpacing - btnH; // EXIT 直上
    int minSettingsY = yRemove + btnH + btnSpacing; // REMOVE 直下

    int settingsY = defaultSettingsY;
    if (settingsY > maxSettingsY) {
        settingsY = maxSettingsY;
    }
    if (settingsY < minSettingsY) {
        settingsY = minSettingsY;
    }
    PlaceBtnAt(IDC_BTN_SETTINGS, settingsY);

    // Exit ボタンは最下部に配置し、高さを縮めた場合でも画面外へ押し出されず最前面表示
    HWND hBtnExit = GetDlgItem(hDlg, IDC_BTN_EXIT);
    if (hBtnExit) {
        SetWindowPos(hBtnExit, HWND_TOP, btnX, exitY, btnW, btnH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        BringWindowToTop(hBtnExit);
        InvalidateRect(hBtnExit, nullptr, TRUE);
    }
}

// --- メインダイアログ (ANSI) ---
static INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        LogDebug("WM_INITDIALOG: start");
        g_hMainDlg = hDlg;
        SetWindowTextA(hDlg, "Audio Thread Isolator v1.0.4");

        HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
        if (!hList) LogDebug("WM_INITDIALOG: hList is NULL!");
        else {
            LogDebug("WM_INITDIALOG: hList found");
            if (!s_pfnOriginalListProc) {
                s_pfnOriginalListProc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrA(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CustomListProc))
                );
            }
        }

        float scale = GetDpiScaleForWindow(hDlg);
        UpdateDialogFonts(hDlg, scale);

        HWND hHeader = ListView_GetHeader(hList);
        if (hHeader && !s_pfnOriginalHeaderProc) {
            s_pfnOriginalHeaderProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(hHeader, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CustomHeaderProc))
            );
        }

        ListView_SetExtendedListViewStyle(
            hList, 
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER
        );
        ShowScrollBar(hList, SB_HORZ, FALSE);
        EnableScrollBar(hList, SB_HORZ, ESB_DISABLE_BOTH);

        g_iniPath = GetIniFilePath();
        LogDebug(("WM_INITDIALOG: iniPath=" + g_iniPath).c_str());

        ati::GlobalConfig config;
        LoadConfig(config, g_iniPath);
        LogDebug("WM_INITDIALOG: LoadConfig done");

        // カラム挿入: 固定 9 列監視専用テーブル
        LVCOLUMNA lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;

        int colIdx = 0;

        // カラムヘッダー定義 (列名・書式)
        static const struct { const char* text; int fmt; } kColDefs[kColCount] = {
            { "",                   LVCFMT_CENTER }, // 0: !
            { "Process Name",       LVCFMT_LEFT   }, // 1: Process Name
            { "Thread Priority",    LVCFMT_CENTER }, // 2: Thread Priority
            { "Process Priority",   LVCFMT_CENTER }, // 3: Process Priority
            { "",                   LVCFMT_CENTER }, // 4: || (Pause)
            { "Audio Core PID/TID", LVCFMT_CENTER }, // 5: Audio Core PID/TID
            { "TIDs",               LVCFMT_CENTER }, // 6: TIDs
            { "Chgs",               LVCFMT_CENTER }, // 7: Chgs
            { "Audio",              LVCFMT_CENTER }, // 8: Audio
            { "Excluded",           LVCFMT_CENTER }, // 9: Excluded
        };

        const std::vector<int>& widths = (!g_colWidths.empty() && g_colWidths.size() == kColCount)
            ? g_colWidths
            : std::vector<int>(kColDefaultWidths, kColDefaultWidths + kColCount);

        for (int i = 0; i < kColCount; ++i) {
            lvc.fmt = kColDefs[i].fmt;
            lvc.cx = ScaleI(widths[i], scale);
            lvc.iSubItem = colIdx;
            lvc.pszText = const_cast<LPSTR>(kColDefs[i].text);
            ListView_InsertColumn(hList, colIdx++, &lvc);
        }

        LogDebug(("WM_INITDIALOG: 10 columns inserted, scale=" + std::to_string(scale)).c_str());

        // Always on Top の初期状態適用および DPI 追従描画サブクラス化
        HWND hChkTop = GetDlgItem(hDlg, IDC_CHK_ALWAYS_ON_TOP);
        if (hChkTop) {
            if (!s_pfnOriginalCheckboxProc) {
                s_pfnOriginalCheckboxProc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrA(hChkTop, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CustomCheckboxProc))
                );
            }
            SendMessageA(hChkTop, BM_SETCHECK, g_alwaysOnTop ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (g_alwaysOnTop) {
            SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        // ATI 自身の自己隔離 (通常コア群への配置)
        if (config.normalAffinityMask != 0) {
            SetProcessAffinityMask(GetCurrentProcess(), config.normalAffinityMask);
        }
        LogDebug("WM_INITDIALOG: Self isolation applied");

        g_isolator.Initialize(config);
        LogDebug("WM_INITDIALOG: isolator Initialize done");

        RefreshListView(hList, config.rules);
        LogDebug("WM_INITDIALOG: RefreshListView done");

        HICON hAppIcon = LoadIconA(g_hInstance, MAKEINTRESOURCEA(IDI_APP_ICON));
        if (hAppIcon) {
            SendMessageA(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
            SendMessageA(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));
        }

        InitTrayIcon(hDlg);
        LogDebug("WM_INITDIALOG: InitTrayIcon done");

        SetTimer(hDlg, TIMER_POLLING_ID, config.pollingIntervalMs, nullptr);
        LogDebug("WM_INITDIALOG: SetTimer done");

        // 初期レイアウト計算を強制適用 (全カラム・全行が収まるようウィンドウ寸法を最適化)
        AdjustWindowSizeToContent(hDlg, scale);

        RECT rcCl;
        GetClientRect(hDlg, &rcCl);
        RelayoutMainDialog(hDlg, scale, rcCl.right - rcCl.left, rcCl.bottom - rcCl.top);
        UpdateListViewColumnsDpi(hDlg, scale);

        return TRUE;
    }

    case WM_GETMINMAXINFO: {
        LPMINMAXINFO lpMMI = reinterpret_cast<LPMINMAXINFO>(lParam);
        float scale = GetDpiScaleForWindow(hDlg);
        lpMMI->ptMinTrackSize.x = static_cast<LONG>(514 * scale);
        lpMMI->ptMinTrackSize.y = static_cast<LONG>(180 * scale);
        return 0;
    }

    case WM_DPICHANGED: {
        RECT* prcNew = reinterpret_cast<RECT*>(lParam);
        if (prcNew) {
            SetWindowPos(hDlg, nullptr, prcNew->left, prcNew->top,
                         prcNew->right - prcNew->left, prcNew->bottom - prcNew->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        float newScale = GetDpiScaleForWindow(hDlg);
        UpdateDialogFonts(hDlg, newScale);
        AdjustWindowSizeToContent(hDlg, newScale);
        RECT rcCl;
        GetClientRect(hDlg, &rcCl);
        RelayoutMainDialog(hDlg, newScale, rcCl.right - rcCl.left, rcCl.bottom - rcCl.top);
        UpdateListViewColumnsDpi(hDlg, newScale);
        InvalidateRect(hDlg, nullptr, TRUE);
        return 0;
    }

    case WM_DISPLAYCHANGE: {
        float newScale = GetDpiScaleForWindow(hDlg);
        UpdateDialogFonts(hDlg, newScale);
        AdjustWindowSizeToContent(hDlg, newScale);
        RECT rcCl;
        GetClientRect(hDlg, &rcCl);
        RelayoutMainDialog(hDlg, newScale, rcCl.right - rcCl.left, rcCl.bottom - rcCl.top);
        UpdateListViewColumnsDpi(hDlg, newScale);
        InvalidateRect(hDlg, nullptr, TRUE);
        return 0;
    }

    case WM_SIZE: {
        if (ati::IsPriorityMatrixPickerOpen()) {
            ati::ClosePriorityMatrixPicker();
        }
        if (wParam == SIZE_MINIMIZED) return TRUE;
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        if (cx <= 0 || cy <= 0) return TRUE;

        float scale = GetDpiScaleForWindow(hDlg);
        RelayoutMainDialog(hDlg, scale, cx, cy);
        UpdateListViewColumnsDpi(hDlg, scale);

        // ウィンドウリサイズ時、親ダイアログの余白・凡例領域を背景消去付きで無効化（旧位置の残像を防止）
        InvalidateRect(hDlg, nullptr, TRUE);
        return TRUE;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        float scale = GetDpiScaleForWindow(hDlg);

        HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
        if (hList) {
            RECT rcList;
            GetWindowRect(hList, &rcList);
            MapWindowPoints(HWND_DESKTOP, hDlg, reinterpret_cast<LPPOINT>(&rcList), 2);

            int startY = rcList.bottom + ScaleI(Layout::kBtnSpacing, scale);
            int startX = rcList.left + ScaleI(Layout::kBtnSpacing, scale);
            int dotSize = ScaleI(Layout::kDotSize, scale);

            HFONT hFont = GetEffectiveFont(hDlg);
            HGDIOBJ oldFont = SelectObject(hdc, hFont);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(60, 60, 60));

            // 凡例領域の背景を先に塗りつぶし（ListView下端からダイアログ底まで、右パネルボタン領域を侵食しないよう rcList.right までに限定）
            RECT rcClient;
            GetClientRect(hDlg, &rcClient);
            int legendRight = rcList.right;
            if (legendRight <= 0) legendRight = rcClient.right - ScaleI(Layout::kRightColW, scale);
            RECT rcLegendArea = { 0, rcList.bottom, legendRight, rcClient.bottom };
            FillRect(hdc, &rcLegendArea, GetSysColorBrush(COLOR_BTNFACE));

            // 1行目: カラー凡例 4種 (左寄せ配置: 1: Half-Isolated, 2: Fully Isolated, 3: Standby / Scanning, 4: Sleeping (Restart))
            std::string txt1 = " Half-Isolated (Audio<-OtherThread)";
            std::string txt2 = " Fully Isolated";
            std::string txt3 = " Standby / Scanning";
            std::string txt4 = " Sleeping (Restart)";
            SIZE sz1, sz2, sz3, sz4;
            GetTextExtentPoint32A(hdc, txt1.c_str(), static_cast<int>(txt1.length()), &sz1);
            GetTextExtentPoint32A(hdc, txt2.c_str(), static_cast<int>(txt2.length()), &sz2);
            GetTextExtentPoint32A(hdc, txt3.c_str(), static_cast<int>(txt3.length()), &sz3);
            GetTextExtentPoint32A(hdc, txt4.c_str(), static_cast<int>(txt4.length()), &sz4);
            int spacing = ScaleI(Layout::kLegendSpacing, scale);
            int legendStartX = rcList.left + ScaleI(Layout::kBtnSpacing, scale);

            int dotY = startY + ScaleI(3, scale);

            bool anyRunning = false;
            auto rules = g_isolator.GetRulesSnapshot();
            for (const auto& r : rules) {
                if (r.isRunning) {
                    anyRunning = true;
                    break;
                }
            }

            // 1. 黄色/青緑ドット + テキスト (Half-Isolated: リストプロセスが1つでも起動していれば無条件でパカパカ点滅、全停止時は鈍い黄色で静止)
            COLORREF colHalf = (anyRunning && s_pulseTick) ? RGB(124, 224, 173) : RGB(216, 178, 76);
            HBRUSH brHalf = CreateSolidBrush(colHalf);
            HPEN penHalf = CreatePen(PS_SOLID, 1, colHalf);
            HGDIOBJ oldBrush = SelectObject(hdc, brHalf);
            HGDIOBJ oldPen = SelectObject(hdc, penHalf);
            Ellipse(hdc, legendStartX, dotY, legendStartX + dotSize, dotY + dotSize);
            TextOutA(hdc, legendStartX + dotSize + 2, startY, txt1.c_str(), static_cast<int>(txt1.length()));

            // 2. 青緑ドット + テキスト (Fully Isolated: 消灯色固定)
            int x2 = legendStartX + dotSize + 2 + sz1.cx + spacing;
            HBRUSH brGreen = CreateSolidBrush(RGB(111, 201, 155));
            HPEN penGreen = CreatePen(PS_SOLID, 1, RGB(111, 201, 155));
            SelectObject(hdc, brGreen);
            SelectObject(hdc, penGreen);
            Ellipse(hdc, x2, dotY, x2 + dotSize, dotY + dotSize);
            TextOutA(hdc, x2 + dotSize + 2, startY, txt2.c_str(), static_cast<int>(txt2.length()));

            // 3. コーラルピンクドット + テキスト (Standby / Scanning: 消灯色固定)
            int x3 = x2 + dotSize + 2 + sz2.cx + spacing;
            HBRUSH brPink = CreateSolidBrush(RGB(201, 139, 135));
            HPEN penPink = CreatePen(PS_SOLID, 1, RGB(201, 139, 135));
            SelectObject(hdc, brPink);
            SelectObject(hdc, penPink);
            Ellipse(hdc, x3, dotY, x3 + dotSize, dotY + dotSize);
            TextOutA(hdc, x3 + dotSize + 2, startY, txt3.c_str(), static_cast<int>(txt3.length()));

            // 4. 薄紫/ピンクドット + テキスト (Sleeping: 再生＆リスタートのナッジ)
            int x4 = x3 + dotSize + 2 + sz3.cx + spacing;
            COLORREF colSleep = s_pulseTick ? RGB(255, 80, 120) : RGB(190, 160, 235);
            HBRUSH brSleep = CreateSolidBrush(colSleep);
            HPEN penSleep = CreatePen(PS_SOLID, 1, colSleep);
            SelectObject(hdc, brSleep);
            SelectObject(hdc, penSleep);
            Ellipse(hdc, x4, dotY, x4 + dotSize, dotY + dotSize);
            TextOutA(hdc, x4 + dotSize + 2, startY, txt4.c_str(), static_cast<int>(txt4.length()));

            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(penSleep);
            DeleteObject(brSleep);
            DeleteObject(penPink);
            DeleteObject(brPink);
            DeleteObject(penGreen);
            DeleteObject(brGreen);
            DeleteObject(penHalf);
            DeleteObject(brHalf);

            SelectObject(hdc, oldFont);
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ENTERSIZEMOVE: {
        s_inSizeMove = true;
        return TRUE;
    }

    case WM_EXITSIZEMOVE: {
        s_inSizeMove = false;
        // ドラッグ移動・リサイズ終了時に最新の画面状態を描画
        auto rules = g_isolator.GetRulesSnapshot();
        HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
        if (hList) {
            UpdateListViewDynamic(hList, rules);
            InvalidateRect(hList, nullptr, FALSE);
        }
        InvalidateRect(hDlg, nullptr, FALSE);
        return TRUE;
    }

    case WM_TIMER: {
        if (wParam == TIMER_POLLING_ID) {
            s_pulseTick = !s_pulseTick;

            // バックグラウンド監視・TID捕捉・アフィニティ制御はドラッグ中も休まず実行
            bool changed = g_isolator.ScanAndIsolate();

            // 起動時の初期照合クリーンアップ完了時に1回だけINIを一括更新
            if (g_isolator.CheckAndClearInitialNeedSave()) {
                SaveConfig(g_isolator.GetConfig(), g_iniPath);
            }

            // タイマー設定 (通常周期、または高速検証要求時は 250ms)
            static UINT s_currentInterval = 0;
            auto cfg = g_isolator.GetConfig();
            UINT desiredInterval = g_isolator.GetDesiredPollingIntervalMs();
            if (s_currentInterval != desiredInterval) {
                SetTimer(hDlg, TIMER_POLLING_ID, desiredInterval, nullptr);
                s_currentInterval = desiredInterval;
            }

            // ドラッグ移動中（s_inSizeMove == true）は DWM との衝突を防ぐため再描画を一時スキップ
            if (!s_inSizeMove) {
                auto rules = g_isolator.GetRulesSnapshot();
                bool anyActiveDot = false;
                for (const auto& r : rules) {
                    if (r.isRunning && !r.isBypassed) {
                        anyActiveDot = true;
                        break;
                    }
                }

                HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
                if (hList) {
                    if (changed) {
                        UpdateListViewDynamic(hList, rules);
                    }
                    // 画面上に実際に ● (通常監視中の稼働プロセス) が存在する場合のみ点滅再描画
                    if (anyActiveDot) {
                        RECT rcClient;
                        GetClientRect(hList, &rcClient);
                        int col0W = ListView_GetColumnWidth(hList, 0);
                        RECT rcCol0 = { 0, 0, col0W, rcClient.bottom };
                        InvalidateRect(hList, &rcCol0, FALSE);
                    }

                    // 全対象 Not running / Bypassed かつ変化なしの時は、下部凡例領域の不要な再描画を行わず CPU/GPU 負荷とチラつきを抑止
                    if (changed || anyActiveDot) {
                        RECT rcList;
                        GetWindowRect(hList, &rcList);
                        MapWindowPoints(HWND_DESKTOP, hDlg, reinterpret_cast<LPPOINT>(&rcList), 2);
                        float scale = GetDpiScaleForWindow(hDlg);
                        RECT rcStatus = { rcList.left, rcList.bottom, rcList.right, rcList.bottom + ScaleI(Layout::kBtnH, scale) };
                        InvalidateRect(hDlg, &rcStatus, FALSE);
                    }
                }
            }
        }
        return TRUE;
    }


    case WM_SHOWWINDOW: {
        char buf[64];
        snprintf(buf, sizeof(buf), "WM_SHOWWINDOW: show=%d, status=%lu", (int)wParam, (unsigned long)lParam);
        LogDebug(buf);
        break;
    }

    case WM_COMMAND: {
        WORD cmdId = LOWORD(wParam);
        char buf[64];
        snprintf(buf, sizeof(buf), "WM_COMMAND: cmdId=%d", cmdId);
        LogDebug(buf);

        if (cmdId == IDC_CHK_ALWAYS_ON_TOP) {
            HWND hChkTop = GetDlgItem(hDlg, IDC_CHK_ALWAYS_ON_TOP);
            if (hChkTop) {
                g_alwaysOnTop = (SendMessageA(hChkTop, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetWindowPos(
                    hDlg,
                    g_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE
                );
                SaveConfig(g_isolator.GetConfig(), g_iniPath);
                LogDebug(("MainDlg: AlwaysOnTop toggled to " + std::to_string(g_alwaysOnTop ? 1 : 0)).c_str());
            }
            return TRUE;
        }

        else if (cmdId == IDC_BTN_HIDE || cmdId == IDCANCEL) {
            LogDebug("MainDlg: Hiding window (IDC_BTN_HIDE or IDCANCEL)");
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        }
        else if (cmdId == IDOK) {
            // Prevent accidental dialog closing on Enter key
            return TRUE;
        }
        else if (cmdId == IDC_BTN_SETTINGS) {
            DialogBoxA(g_hInstance, MAKEINTRESOURCEA(IDD_SETTINGS_DIALOG), hDlg, SettingsDlgProc);
            return TRUE;
        }
        else if (cmdId == IDC_BTN_ADD) {
            std::vector<ati::PickedProcess> selected;
            if (ati::ShowProcessPickerDialog(hDlg, g_hInstance, selected)) {
                auto config = g_isolator.GetConfig();
                for (const auto& sp : selected) {
                    bool exists = false;
                    for (const auto& r : config.rules) {
                        if (_stricmp(r.processName.c_str(), sp.exeFileName.c_str()) == 0) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        ati::ProcessRule newRule;
                        newRule.processName = sp.exeFileName;
                        newRule.audioCore = config.defaultAudioCore;
                        newRule.audioAffinityMask = config.defaultAudioAffinityMask;
                        newRule.audioPriority = config.defaultAudioPriority;
                        newRule.currentThreadCount = 0;
                        newRule.targetPriority = 0;
                        newRule.processPriorityClass = 0;
                        newRule.applyCount = 0;
                        newRule.detectedThreadName = "";
                        newRule.activePid = 0;
                        newRule.isRunning = false;

                        g_isolator.AddRule(newRule);
                    }
                }
                auto updated = g_isolator.GetConfig();
                SaveConfig(updated, g_iniPath);
                HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
                RefreshListView(hList, updated.rules);
                AdjustWindowSizeToContent(hDlg, GetDpiScaleForWindow(hDlg));
            }
            return TRUE;
        }
        else if (cmdId == IDC_BTN_REMOVE) {
            HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel >= 0 && static_cast<size_t>(sel) < g_displayRows.size()) {
                size_t ruleIdx = g_displayRows[sel].ruleIndex;
                g_isolator.RemoveRule(ruleIdx);
                auto updated = g_isolator.GetConfig();
                SaveConfig(updated, g_iniPath);
                RefreshListView(hList, updated.rules);
                AdjustWindowSizeToContent(hDlg, GetDpiScaleForWindow(hDlg));
            }
            return TRUE;
        }
        else if (cmdId == IDC_BTN_EDIT) {
            HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel >= 0 && static_cast<size_t>(sel) < g_displayRows.size()) {
                size_t ruleIdx = g_displayRows[sel].ruleIndex;
                OpenRuleEditDialog(hDlg, ruleIdx);
            }
            return TRUE;
        }
        else if (cmdId == IDC_BTN_EXIT || cmdId == ID_TRAY_EXIT) {
            KillTimer(hDlg, TIMER_POLLING_ID);
            RemoveTrayIcon();
            g_isolator.ResumeAllSuspendedThreads();
            DestroyWindow(hDlg);
            return TRUE;
        }
        else if (cmdId == ID_TRAY_OPEN) {
            ShowWindow(hDlg, SW_SHOW);
            SetForegroundWindow(hDlg);
            return TRUE;
        }
        else if (cmdId == ID_TRAY_STARTUP) {
            SetStartupEnabled(!IsStartupEnabled());
            return TRUE;
        }
        else if (cmdId == ID_TRAY_RESTART) {
            RestartApplication(hDlg);
            return TRUE;
        }
        else if (cmdId == ID_TRAY_RESTART_ADMIN) {
            RestartAsAdmin(hDlg);
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY: {
        LPNMHDR pnm = reinterpret_cast<LPNMHDR>(lParam);

        if (pnm && pnm->idFrom == IDC_LIST_PROCESSES) {
            if (pnm->code == NM_DBLCLK) {
                if (ati::IsPriorityMatrixPickerOpen()) {
                    ati::ClosePriorityMatrixPicker();
                }
                LPNMITEMACTIVATE pia = reinterpret_cast<LPNMITEMACTIVATE>(lParam);
                if (pia && pia->iItem >= 0 && pia->iSubItem != 2 && pia->iSubItem != 3 && pia->iSubItem != 4 && pia->iSubItem != 5) {
                    size_t rowIdx = static_cast<size_t>(pia->iItem);
                    if (rowIdx < g_displayRows.size()) {
                        OpenRuleEditDialog(hDlg, g_displayRows[rowIdx].ruleIndex);
                        return TRUE;
                    }
                }
            } else if (pnm->code == NM_CLICK) {
                HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
                LVHITTESTINFO lvhti = { 0 };
                GetCursorPos(&lvhti.pt);
                ScreenToClient(hList, &lvhti.pt);
                ListView_SubItemHitTest(hList, &lvhti);

                if (lvhti.iItem >= 0 && lvhti.iSubItem == 0) {
                    if (ati::IsPriorityMatrixPickerOpen()) {
                        ati::ClosePriorityMatrixPicker();
                    }
                    size_t rowIdx = static_cast<size_t>(lvhti.iItem);
                    if (rowIdx < g_displayRows.size()) {
                        size_t ruleIdx = g_displayRows[rowIdx].ruleIndex;
                        auto rules = g_isolator.GetRulesSnapshot();
                        if (ruleIdx < rules.size()) {
                            const auto& targetRule = rules[ruleIdx];

                            // sleeping... 状態のナッジ点滅 ● クリック時: bypass 解除時と同様に探索を即時リスタート
                            const auto& dr = g_displayRows[rowIdx];
                            if (targetRule.isRunning && !targetRule.isBypassed) {
                                if (targetRule.ignoreSig && targetRule.ignoreSigCount > 1) {
                                    bool isSlotConfirmed = (dr.subIndex < targetRule.activeAudioTids.size());
                                    if (!isSlotConfirmed && targetRule.multiSearchSleeping) {
                                        g_isolator.ResumeMultiSearch(ruleIdx);
                                        g_isolator.ScanAndIsolate();
                                        auto updatedRules = g_isolator.GetRulesSnapshot();
                                        UpdateListViewDynamic(hList, updatedRules);
                                        InvalidateRect(hList, nullptr, FALSE);
                                        return TRUE;
                                    }
                                } else if (targetRule.detectedThreadName == "sleeping...") {
                                    g_isolator.RestartSearch(ruleIdx);
                                    g_isolator.ScanAndIsolate();
                                    auto updatedRules = g_isolator.GetRulesSnapshot();
                                    UpdateListViewDynamic(hList, updatedRules);
                                    InvalidateRect(hList, nullptr, FALSE);
                                    return TRUE;
                                }
                            }

                            // 全状態でクリック可: 未起動時は事前除外トグル、起動中は Bypass 切替
                            // ● クリック時: 内部状態を全破棄して即時 Bypass → ☑ 表示
                            // ☑ 解除時: ゼロからリスタート（ScanAndIsolate 即時実行）
                            {
                                bool wasRunning = targetRule.isRunning;
                                g_isolator.ToggleProcessBypass(ruleIdx);
                                SaveConfig(g_isolator.GetConfig(), g_iniPath);

                                // 起動中の Bypass 解除時は直ちにアイソレーターを即時実行
                                if (wasRunning) {
                                    g_isolator.ScanAndIsolate();
                                }

                                auto updatedRules = g_isolator.GetRulesSnapshot();
                                UpdateListViewDynamic(hList, updatedRules);
                                InvalidateRect(hList, nullptr, FALSE);
                            }
                            return TRUE;
                        }
                    }
                } else if (lvhti.iItem >= 0 && lvhti.iSubItem == 4) {
                    // Col 4: Pause / Verify (一時停止・検証チェックボックス - 個別TID対応)
                    if (ati::IsPriorityMatrixPickerOpen()) {
                        ati::ClosePriorityMatrixPicker();
                    }
                    size_t rowIdx = static_cast<size_t>(lvhti.iItem);
                    if (rowIdx < g_displayRows.size()) {
                        const auto& dr = g_displayRows[rowIdx];
                        auto rules = g_isolator.GetRulesSnapshot();
                        if (dr.ruleIndex < rules.size()) {
                            const auto& targetRule = rules[dr.ruleIndex];
                            if (targetRule.isRunning && dr.tid != 0) {
                                g_isolator.ToggleSuspendAudioThread(dr.ruleIndex, dr.tid);
                                auto updatedRules = g_isolator.GetRulesSnapshot();
                                UpdateListViewDynamic(hList, updatedRules);
                                InvalidateRect(hList, nullptr, FALSE);
                                return TRUE;
                            }
                        }
                    }
                } else if (lvhti.iItem >= 0 && lvhti.iSubItem == 5) {
                    // Col 5 (Audio Core PID/TID 列):
                    // - sleeping... 状態時: bypass 解除時と同様に探索を即時リスタート
                    // - Standby (稼働) 状態時: Bypass へ切り替え、! カラムに ☑ を表示
                    if (ati::IsPriorityMatrixPickerOpen()) {
                        ati::ClosePriorityMatrixPicker();
                    }
                    size_t rowIdx = static_cast<size_t>(lvhti.iItem);
                    if (rowIdx < g_displayRows.size()) {
                        size_t ruleIdx = g_displayRows[rowIdx].ruleIndex;
                        auto rules = g_isolator.GetRulesSnapshot();
                        if (ruleIdx < rules.size()) {
                            const auto& targetRule = rules[ruleIdx];
                            bool isHandled = false;
                            if (targetRule.ignoreSig && targetRule.ignoreSigCount > 1) {
                                const auto& dr = g_displayRows[rowIdx];
                                bool isSlotConfirmed = (dr.subIndex < targetRule.activeAudioTids.size());
                                if (!isSlotConfirmed && targetRule.multiSearchSleeping) {
                                    g_isolator.ResumeMultiSearch(ruleIdx);
                                    g_isolator.ScanAndIsolate();
                                    isHandled = true;
                                }
                            }
                            if (!isHandled) {
                                if (targetRule.detectedThreadName == "sleeping...") {
                                    g_isolator.RestartSearch(ruleIdx);
                                    g_isolator.ScanAndIsolate();
                                } else {
                                    bool wasRunning = targetRule.isRunning;
                                    g_isolator.ToggleProcessBypass(ruleIdx);
                                    SaveConfig(g_isolator.GetConfig(), g_iniPath);
                                    if (wasRunning && targetRule.isBypassed) {
                                        // Bypass 解除時は直ちにスキャン実行
                                        g_isolator.ScanAndIsolate();
                                    }
                                }
                            }
                            auto updatedRules = g_isolator.GetRulesSnapshot();
                            UpdateListViewDynamic(hList, updatedRules);
                            InvalidateRect(hList, nullptr, FALSE);
                            return TRUE;
                        }
                    }
                } else {
                    if (ati::IsPriorityMatrixPickerOpen()) {
                        ati::ClosePriorityMatrixPicker();
                    }
                }
            } else if (pnm->code == NM_CUSTOMDRAW) {
                LPNMLVCUSTOMDRAW plvcd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
                switch (plvcd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                case CDDS_ITEMPREPAINT: {
                    int row = static_cast<int>(plvcd->nmcd.dwItemSpec);
                    COLORREF rowBkCol = ((row % 2) == 1) ? RGB(233, 231, 227) : RGB(248, 248, 248);
                    plvcd->clrTextBk = rowBkCol;
                    SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
                    return TRUE;
                }
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                    int row = static_cast<int>(plvcd->nmcd.dwItemSpec);
                    COLORREF rowBkCol = ((row % 2) == 1) ? RGB(233, 231, 227) : RGB(248, 248, 248);
                    HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
                    UINT state = ListView_GetItemState(hList, row, LVIS_SELECTED);

                    plvcd->clrTextBk = (state & LVIS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHT) : rowBkCol;
                    plvcd->clrText = (state & LVIS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(0, 0, 0);

                    if (plvcd->iSubItem == 0) {
                        auto rules = g_isolator.GetRulesSnapshot();
                        if (row >= 0 && row < static_cast<int>(g_displayRows.size())) {
                            const auto& dr = g_displayRows[row];
                            if (dr.ruleIndex < rules.size()) {
                                const auto& r = rules[dr.ruleIndex];
                                RECT rcSub;
                                ListView_GetSubItemRect(hList, row, 0, LVIR_BOUNDS, &rcSub);
                                int col0W = ListView_GetColumnWidth(hList, 0);
                                rcSub.right = rcSub.left + col0W;

                                HDC hdc = plvcd->nmcd.hdc;

                                // 背景のクリア
                                HBRUSH hBkBrush = (state & LVIS_SELECTED) 
                                    ? GetSysColorBrush(COLOR_HIGHLIGHT) 
                                    : CreateSolidBrush(rowBkCol);
                                FillRect(hdc, &rcSub, hBkBrush);
                                if (!(state & LVIS_SELECTED)) DeleteObject(hBkBrush);

                                int cellW = rcSub.right - rcSub.left;
                                int cellH = rcSub.bottom - rcSub.top;
                                float scale = GetDpiScaleForWindow(hDlg);

                                if (r.isRunning && !r.isBypassed) {
                                    // 起動中（稼働中）: 丸印インジケーターを描画
                                    int dotSize = ScaleI(10, scale);
                                    if (dotSize > cellH - 4) dotSize = cellH - 4;
                                    if (dotSize < 6) dotSize = 6;

                                    int dotX = rcSub.left + (cellW - dotSize) / 2;
                                    int dotY = rcSub.top + (cellH - dotSize) / 2;

                                    COLORREF col;
                                    if (r.ignoreSig && r.ignoreSigCount > 1) {
                                        bool isSlotConfirmed = (dr.subIndex < r.activeAudioTids.size());
                                        if (isSlotConfirmed) {
                                            if (r.hasIntruderThreads) {
                                                col = s_pulseTick ? RGB(124, 224, 173) : RGB(216, 178, 76);
                                            } else {
                                                col = s_pulseTick ? RGB(124, 224, 173) : RGB(111, 201, 155);
                                            }
                                        } else if (r.multiSearchSleeping) {
                                            col = s_pulseTick ? RGB(255, 80, 120) : RGB(190, 160, 235);
                                        } else {
                                            col = s_pulseTick ? RGB(224, 155, 151) : RGB(201, 139, 135);
                                        }
                                    } else {
                                        if (r.isAudioIsolated) {
                                            if (r.hasIntruderThreads) {
                                                col = s_pulseTick ? RGB(124, 224, 173) : RGB(216, 178, 76);
                                            } else {
                                                col = s_pulseTick ? RGB(124, 224, 173) : RGB(111, 201, 155);
                                            }
                                        } else if (r.detectedThreadName == "sleeping...") {
                                            col = s_pulseTick ? RGB(255, 80, 120) : RGB(190, 160, 235);
                                        } else {
                                            col = s_pulseTick ? RGB(224, 155, 151) : RGB(201, 139, 135);
                                        }
                                    }

                                    HBRUSH hBr = CreateSolidBrush(col);
                                    HPEN hPen = CreatePen(PS_SOLID, 1, col);
                                    HGDIOBJ oldBrush = SelectObject(hdc, hBr);
                                    HGDIOBJ oldPen = SelectObject(hdc, hPen);
                                    Ellipse(hdc, dotX, dotY, dotX + dotSize, dotY + dotSize);
                                    SelectObject(hdc, oldPen);
                                    SelectObject(hdc, oldBrush);
                                    DeleteObject(hPen);
                                    DeleteObject(hBr);
                                } else if (dr.subIndex == 0) {
                                    // 未起動時 (親行のみチェックボックス描画)
                                    int cbSize = ScaleI(Layout::kCheckboxSize, scale);
                                    int cbX = rcSub.left + (cellW - cbSize) / 2;
                                    int cbY = rcSub.top + (cellH - cbSize) / 2;
                                    RECT rcCb = { cbX, cbY, cbX + cbSize, cbY + cbSize };
                                    UINT uState = DFCS_BUTTONCHECK | (r.isBypassed ? DFCS_CHECKED : 0);
                                    DrawFrameControl(hdc, &rcCb, DFC_BUTTON, uState);
                                }

                                SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                                return TRUE;
                            }
                        }
                    } else if (plvcd->iSubItem == 1) {
                        // Col 1: Process Name (2行目以降は GDI ペンによる「└」L 字ツリー線 + プロセス名を描画)
                        auto rules = g_isolator.GetRulesSnapshot();
                        if (row >= 0 && row < static_cast<int>(g_displayRows.size())) {
                            const auto& dr = g_displayRows[row];
                            if (dr.ruleIndex < rules.size()) {
                                const auto& r = rules[dr.ruleIndex];
                                RECT rcSub;
                                ListView_GetSubItemRect(hList, row, 1, LVIR_BOUNDS, &rcSub);
                                HDC hdc = plvcd->nmcd.hdc;
                                float scale = GetDpiScaleForWindow(hDlg);

                                // 背景塗りつぶし
                                HBRUSH hBkBrush = (state & LVIS_SELECTED) 
                                    ? GetSysColorBrush(COLOR_HIGHLIGHT) 
                                    : CreateSolidBrush(rowBkCol);
                                FillRect(hdc, &rcSub, hBkBrush);
                                if (!(state & LVIS_SELECTED)) DeleteObject(hBkBrush);

                                std::string procName = StripExeExtension(r.processName);
                                HFONT hFont = GetEffectiveFont(hList);
                                HGDIOBJ oldFont = SelectObject(hdc, hFont);
                                SetBkMode(hdc, TRANSPARENT);
                                COLORREF textCol = (state & LVIS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
                                SetTextColor(hdc, textCol);

                                int cellH = rcSub.bottom - rcSub.top;
                                int startX = rcSub.left + ScaleI(6, scale);

                                if (dr.subIndex > 0) {
                                    // 2行目以降: GDI ペンによる「└」L 字ツリー線描画 (高 DPI・多言語完全両立)
                                    int penW = (std::max)(1, ScaleI(1, scale));
                                    COLORREF lineColor = (state & LVIS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(140, 140, 140);
                                    HPEN hPen = CreatePen(PS_SOLID, penW, lineColor);
                                    HGDIOBJ oldPen = SelectObject(hdc, hPen);

                                    int treeX = rcSub.left + ScaleI(8, scale);
                                    int treeMidY = rcSub.top + cellH / 2;
                                    int armW = ScaleI(8, scale);

                                    // 縦線: セル上端 (rcSub.top) から中央 (treeMidY) まで
                                    MoveToEx(hdc, treeX, rcSub.top, nullptr);
                                    LineTo(hdc, treeX, treeMidY + penW);

                                    // 横線: 縦線交点 (treeX) から右へ armW まで
                                    MoveToEx(hdc, treeX, treeMidY, nullptr);
                                    LineTo(hdc, treeX + armW, treeMidY);

                                    SelectObject(hdc, oldPen);
                                    DeleteObject(hPen);

                                    startX = treeX + armW + ScaleI(4, scale);
                                }

                                RECT rcText = rcSub;
                                rcText.left = startX;
                                rcText.right = rcSub.right - ScaleI(4, scale);
                                DrawTextA(hdc, procName.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                                SelectObject(hdc, oldFont);
                                SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                                return TRUE;
                            }
                        }
                    } else if (plvcd->iSubItem == 2 || plvcd->iSubItem == 3) {
                        // Col 2: Thread Priority / Col 3: Process Priority (右詰め + 半角SP2個 + 4px[200%時]の余白)
                        auto rules = g_isolator.GetRulesSnapshot();
                        if (row >= 0 && row < static_cast<int>(g_displayRows.size())) {
                            const auto& dr = g_displayRows[row];
                            if (dr.ruleIndex < rules.size()) {
                                RECT rcSub;
                                ListView_GetSubItemRect(hList, row, plvcd->iSubItem, LVIR_BOUNDS, &rcSub);
                                HDC hdc = plvcd->nmcd.hdc;
                                float scale = GetDpiScaleForWindow(hDlg);

                                HBRUSH hBkBrush = (state & LVIS_SELECTED) 
                                    ? GetSysColorBrush(COLOR_HIGHLIGHT) 
                                    : CreateSolidBrush(rowBkCol);
                                FillRect(hdc, &rcSub, hBkBrush);
                                if (!(state & LVIS_SELECTED)) DeleteObject(hBkBrush);

                                char txtBuf[64] = { 0 };
                                ListView_GetItemText(hList, row, plvcd->iSubItem, txtBuf, sizeof(txtBuf));
                                std::string text(txtBuf);

                                if (!text.empty()) {
                                    HFONT hFont = GetEffectiveFont(hList);
                                    HGDIOBJ oldFont = SelectObject(hdc, hFont);
                                    SetBkMode(hdc, TRANSPARENT);

                                    COLORREF textCol = (state & LVIS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
                                    SetTextColor(hdc, textCol);

                                    SIZE szSp;
                                    GetTextExtentPoint32A(hdc, "  ", 2, &szSp);
                                    int extraPx = ScaleI(2, scale); // 200%環境で4px
                                    int rightMargin = szSp.cx + extraPx;

                                    RECT rcText = rcSub;
                                    rcText.right -= rightMargin;
                                    DrawTextA(hdc, text.c_str(), -1, &rcText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                                    SelectObject(hdc, oldFont);
                                }

                                SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                                return TRUE;
                            }
                        }
                    } else if (plvcd->iSubItem == 4) {
                        // Col 4: Pause / Verify (一時停止・検証チェックボックス - 個別TID対応)
                        auto rules = g_isolator.GetRulesSnapshot();
                        if (row >= 0 && row < static_cast<int>(g_displayRows.size())) {
                            const auto& dr = g_displayRows[row];
                            if (dr.ruleIndex < rules.size()) {
                                const auto& r = rules[dr.ruleIndex];
                                bool isSusp = g_isolator.IsAudioThreadSuspended(dr.ruleIndex, dr.tid);

                                RECT rcSub;
                                ListView_GetSubItemRect(hList, row, 4, LVIR_BOUNDS, &rcSub);
                                HDC hdc = plvcd->nmcd.hdc;
                                float scale = GetDpiScaleForWindow(hDlg);
                                int cellW = rcSub.right - rcSub.left;
                                int cellH = rcSub.bottom - rcSub.top;

                                // 背景塗りつぶし (一時停止中は注意を促す薄赤ハイライト、通常時は行背景色)
                                COLORREF cellBkCol = isSusp ? RGB(255, 230, 230) : rowBkCol;
                                HBRUSH hBkBrush = (state & LVIS_SELECTED) 
                                    ? GetSysColorBrush(COLOR_HIGHLIGHT) 
                                    : CreateSolidBrush(cellBkCol);
                                FillRect(hdc, &rcSub, hBkBrush);
                                if (!(state & LVIS_SELECTED)) DeleteObject(hBkBrush);

                                // セル中央にチェックボックス (☐ または ☑) を描画
                                int cbSize = ScaleI(Layout::kCheckboxSize, scale);
                                int cbX = rcSub.left + (cellW - cbSize) / 2;
                                int cbY = rcSub.top + (cellH - cbSize) / 2;
                                RECT rcCb = { cbX, cbY, cbX + cbSize, cbY + cbSize };
                                UINT uState = DFCS_BUTTONCHECK;
                                if (!r.isRunning || dr.tid == 0) {
                                    uState |= DFCS_INACTIVE;
                                } else if (isSusp) {
                                    uState |= DFCS_CHECKED;
                                }
                                DrawFrameControl(hdc, &rcCb, DFC_BUTTON, uState);

                                SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                                return TRUE;
                            }
                        }
                    } else if (plvcd->iSubItem == 9) {
                        // Col 9 (Excluded 列): 稼働中かつ監視通常の場合、退避済みであるため先頭の '!' を赤太字で描画
                        auto rules = g_isolator.GetRulesSnapshot();
                        if (row >= 0 && row < static_cast<int>(g_displayRows.size())) {
                            const auto& dr = g_displayRows[row];
                            if (dr.ruleIndex < rules.size()) {
                                const auto& r = rules[dr.ruleIndex];
                                if (r.isRunning && !r.isBypassed) {
                                    RECT rcSub;
                                    ListView_GetSubItemRect(hList, row, 9, LVIR_BOUNDS, &rcSub);
                                    HDC hdc = plvcd->nmcd.hdc;

                                    HRGN hClip = CreateRectRgn(rcSub.left, rcSub.top, rcSub.right, rcSub.bottom);
                                    SelectClipRgn(hdc, hClip);

                                    HBRUSH hBkBrush = (state & LVIS_SELECTED) 
                                        ? GetSysColorBrush(COLOR_HIGHLIGHT) 
                                        : CreateSolidBrush(rowBkCol);
                                    FillRect(hdc, &rcSub, hBkBrush);
                                    if (!(state & LVIS_SELECTED)) DeleteObject(hBkBrush);

                                    char txtBuf[64] = { 0 };
                                    ListView_GetItemText(hList, row, 9, txtBuf, sizeof(txtBuf));
                                    std::string text(txtBuf);

                                    if (!text.empty()) {
                                        HFONT hFont = GetEffectiveFont(hList);
                                        HGDIOBJ oldFont = SelectObject(hdc, hFont);
                                        SetBkMode(hdc, TRANSPARENT);

                                        SIZE szTotal;
                                        GetTextExtentPoint32A(hdc, text.c_str(), static_cast<int>(text.length()), &szTotal);
                                        int cellW = rcSub.right - rcSub.left;
                                        int startX = (szTotal.cx < cellW) ? (rcSub.left + (cellW - szTotal.cx) / 2) : (rcSub.left + 4);
                                        int startY = rcSub.top + (rcSub.bottom - rcSub.top - szTotal.cy) / 2;

                                        if (text[0] == '!') {
                                            LOGFONTA lf = { 0 };
                                            GetObjectA(hFont, sizeof(lf), &lf);
                                            lf.lfWeight = FW_BOLD;
                                            HFONT hBold = CreateFontIndirectA(&lf);
                                            SelectObject(hdc, hBold);
                                            SetTextColor(hdc, RGB(220, 50, 50)); // 鮮やかな赤
                                            TextOutA(hdc, startX, startY, "!", 1);

                                            SIZE szEx;
                                            GetTextExtentPoint32A(hdc, "!", 1, &szEx);
                                            SelectObject(hdc, hFont);
                                            DeleteObject(hBold);

                                            COLORREF textCol = (state & LVIS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
                                            SetTextColor(hdc, textCol);
                                            TextOutA(hdc, startX + szEx.cx, startY, text.c_str() + 1, static_cast<int>(text.length() - 1));
                                        } else {
                                            COLORREF textCol = (state & LVIS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
                                            SetTextColor(hdc, textCol);
                                            TextOutA(hdc, startX, startY, text.c_str(), static_cast<int>(text.length()));
                                        }

                                        SelectObject(hdc, oldFont);
                                    }

                                    SelectClipRgn(hdc, NULL);
                                    DeleteObject(hClip);

                                    SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                                    return TRUE;
                                }
                            }
                        }
                    }
                    SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
                    return TRUE;
                }
                }
            }
        }
        break;
    }

    case WM_TRAYICON_MSG: {
        if (lParam == WM_LBUTTONUP) {
            if (IsIconic(hDlg)) {
                ShowWindow(hDlg, SW_RESTORE);
            } else if (!IsWindowVisible(hDlg)) {
                ShowWindow(hDlg, SW_SHOW);
            }
            SetForegroundWindow(hDlg);
        } else if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = LoadMenuA(g_hInstance, MAKEINTRESOURCEA(IDR_TRAY_MENU));
            if (hMenu) {
                HMENU hSub = GetSubMenu(hMenu, 0);
                CheckMenuItem(
                    hSub, 
                    ID_TRAY_STARTUP, 
                    IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED
                );
                if (IsRunningAsAdmin()) {
                    RemoveMenu(hSub, ID_TRAY_RESTART_ADMIN, MF_BYCOMMAND);
                }
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hDlg);
                TrackPopupMenu(hSub, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hDlg, nullptr);
                DestroyMenu(hMenu);
            }
        }
        return TRUE;
    }

    case WM_CLOSE:
        ShowWindow(hDlg, SW_HIDE);
        return TRUE;

    case WM_DESTROY: {
        KillTimer(hDlg, TIMER_POLLING_ID);
        RemoveTrayIcon();
        g_isolator.ResumeAllSuspendedThreads();

        // 終了時に現在のカラム幅およびウィンドウ高さを論理ピクセル換算で退避
        HWND hList = GetDlgItem(hDlg, IDC_LIST_PROCESSES);
        float scale = GetDpiScaleForWindow(hDlg);
        if (scale <= 0.0f) scale = 1.0f;
        if (hList) {
            g_colWidths.clear();
            for (int i = 0; i < kColCount; ++i) {
                int colW = ListView_GetColumnWidth(hList, i);
                int baseW = static_cast<int>(std::round(colW / scale));
                if (baseW < 10) baseW = kColDefaultWidths[i];
                g_colWidths.push_back(baseW);
            }
        }
        RECT rcDlg;
        if (GetWindowRect(hDlg, &rcDlg)) {
            int curH = rcDlg.bottom - rcDlg.top;
            g_windowHeight = static_cast<int>(std::round(curH / scale));
        }

        ati::GlobalConfig curCfg = g_isolator.GetConfig();
        curCfg.columnWidths = g_colWidths;
        curCfg.windowHeight = g_windowHeight;
        SaveConfig(curCfg, g_iniPath);

        if (s_hMainFont) {
            DeleteObject(s_hMainFont);
            s_hMainFont = nullptr;
        }
        ati::ShutdownGdiPlus();
        PostQuitMessage(0);
        return TRUE;
    }
    }

    return FALSE;
}

// --- エントリポイント (純粋 ANSI WinMain / モードレス常駐メッセージループ) ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    InitializeHighDpi();
    LogDebug("WinMain: start (High-DPI PerMonitorV2 enabled)");

    if (IsRunningAsAdmin()) {
        if (EnableRealtimePrivilege()) {
            LogDebug("WinMain: Running as Admin, SeIncreaseBasePriorityPrivilege enabled");
        } else {
            LogDebug("WinMain: Running as Admin, failed to enable SeIncreaseBasePriorityPrivilege");
        }
    }

    // 単一インスタンス制御 (多重起動の物理的防止)
    g_hSingleInstanceMutex = CreateMutexA(nullptr, FALSE, "Global\\AudioThreadIsolator_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LogDebug("WinMain: Another instance is already running. Activating existing window and exiting.");
        HWND hExisting = FindWindowA(nullptr, "Audio Thread Isolator v1.0.4");
        if (!hExisting) hExisting = FindWindowA(nullptr, "Audio Thread Isolator v1.0.3");
        if (!hExisting) hExisting = FindWindowA(nullptr, "Audio Thread Isolator");
        if (hExisting) {
            ShowWindow(hExisting, SW_SHOWNORMAL);
            SetForegroundWindow(hExisting);
        }
        if (g_hSingleInstanceMutex) {
            CloseHandle(g_hSingleInstanceMutex);
            g_hSingleInstanceMutex = nullptr;
        }
        return 0;
    }

    if (lpCmdLine) {
        std::string cmdLog = "WinMain: lpCmdLine='" + std::string(lpCmdLine) + "'";
        LogDebug(cmdLog.c_str());
    } else {
        LogDebug("WinMain: lpCmdLine is NULL");
    }
    g_hInstance = hInstance;

    if (lpCmdLine && strstr(lpCmdLine, "--tray") != nullptr) {
        g_startInTray = true;
        LogDebug("WinMain: g_startInTray = true");
    } else {
        LogDebug("WinMain: g_startInTray = false");
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);
    LogDebug("WinMain: InitCommonControlsEx done");

    HWND hDlg = CreateDialogA(hInstance, MAKEINTRESOURCEA(IDD_MAIN_DIALOG), nullptr, MainDlgProc);
    if (!hDlg) {
        DWORD err = GetLastError();
        char buf[256];
        snprintf(buf, sizeof(buf), "CreateDialogA failed! GetLastError = %lu", err);
        LogDebug(buf);
        MessageBoxA(nullptr, buf, "ATI Error", MB_ICONERROR);
        return 1;
    }
    char dlgLog[128];
    snprintf(dlgLog, sizeof(dlgLog), "WinMain: CreateDialogA succeeded, hDlg=%p", (void*)hDlg);
    LogDebug(dlgLog);

    if (!g_startInTray) {
        ShowWindow(hDlg, SW_SHOWNORMAL);
        UpdateWindow(hDlg);
        SetForegroundWindow(hDlg);
        SetWindowPos(hDlg, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

        RECT rc;
        GetWindowRect(hDlg, &rc);
        char geomLog[256];
        snprintf(geomLog, sizeof(geomLog), "WinMain: hDlg=%p, Rect=(%ld,%ld-%ld,%ld), Size=%ldx%ld, IsVisible=%d",
                 (void*)hDlg, rc.left, rc.top, rc.right, rc.bottom, rc.right - rc.left, rc.bottom - rc.top, IsWindowVisible(hDlg));
        LogDebug(geomLog);

        HWND hParent = GetParent(hDlg);
        HWND hOwner = GetWindow(hDlg, GW_OWNER);
        char relLog[256];
        snprintf(relLog, sizeof(relLog), "WinMain: Parent=%p, Owner=%p", (void*)hParent, (void*)hOwner);
        LogDebug(relLog);
    } else {
        ShowWindow(hDlg, SW_HIDE);
        LogDebug("WinMain: startInTray, hidden to tray");
    }

    LogDebug("WinMain: entering message loop");
    MSG msg;
    bool loggedDestroy = false;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (!loggedDestroy && !IsWindow(hDlg)) {
            LogDebug("WinMain: hDlg has become INVALID (destroyed)!");
            loggedDestroy = true;
        }
        HWND hActive = GetActiveWindow();
        if (hActive && (hActive == hDlg || IsChild(hDlg, hActive))) {
            if (msg.message == WM_SYSKEYDOWN && (msg.wParam == 'D' || msg.wParam == 'd')) {
                SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_BTN_REMOVE, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(hDlg, IDC_BTN_REMOVE)));
                continue;
            }
        }
        if (hActive && IsDialogMessageA(hActive, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    LogDebug("WinMain: message loop exited");

    if (g_hSingleInstanceMutex) {
        CloseHandle(g_hSingleInstanceMutex);
        g_hSingleInstanceMutex = nullptr;
    }

    return static_cast<int>(msg.wParam);
}
