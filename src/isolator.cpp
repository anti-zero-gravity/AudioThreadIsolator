#undef UNICODE
#undef _UNICODE

#include "isolator.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <map>

void LogDebug(const char* msg);

namespace ati {

static std::string ToLowerA(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return str;
}

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* pfnNtQueryInformationThread)(
    HANDLE ThreadHandle,
    ULONG ThreadInformationClass,
    PVOID ThreadInformation,
    ULONG ThreadInformationLength,
    PULONG ReturnLength
);
static pfnNtQueryInformationThread s_pfnNtQueryInformationThread = nullptr;

static void* QueryThreadStartAddress(HANDLE hThread) {
    if (!s_pfnNtQueryInformationThread) {
        HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
        if (hNtdll) {
            s_pfnNtQueryInformationThread = reinterpret_cast<pfnNtQueryInformationThread>(
                GetProcAddress(hNtdll, "NtQueryInformationThread")
            );
        }
    }
    if (!s_pfnNtQueryInformationThread) return nullptr;

    void* startAddr = nullptr;
    ULONG retLen = 0;
    NTSTATUS status = s_pfnNtQueryInformationThread(hThread, 9 /* ThreadQuerySetWin32StartAddress */, &startAddr, sizeof(startAddr), &retLen);
    if (status == 0) return startAddr;
    return nullptr;
}

struct THREAD_BASIC_INFO_RAW {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    DWORD_PTR UniqueProcessId;
    DWORD_PTR UniqueThreadId;
    DWORD_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
};

static DWORD_PTR QueryThreadAffinityMask(HANDLE hThread) {
    if (!s_pfnNtQueryInformationThread) {
        HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
        if (hNtdll) {
            s_pfnNtQueryInformationThread = reinterpret_cast<pfnNtQueryInformationThread>(
                GetProcAddress(hNtdll, "NtQueryInformationThread")
            );
        }
    }
    if (!s_pfnNtQueryInformationThread) return 0;

    THREAD_BASIC_INFO_RAW tbi = { 0 };
    ULONG retLen = 0;
    NTSTATUS status = s_pfnNtQueryInformationThread(hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
    if (status == 0) {
        return tbi.AffinityMask;
    }
    return 0;
}

static std::string QueryFmodOrUnityThreadName(HANDLE hProcess, HANDLE hThread) {
    if (!s_pfnNtQueryInformationThread) {
        HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
        if (hNtdll) {
            s_pfnNtQueryInformationThread = reinterpret_cast<pfnNtQueryInformationThread>(
                GetProcAddress(hNtdll, "NtQueryInformationThread")
            );
        }
    }
    if (!s_pfnNtQueryInformationThread) return "";

    THREAD_BASIC_INFO_RAW tbi = { 0 };
    ULONG retLen = 0;
    NTSTATUS status = s_pfnNtQueryInformationThread(hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
    if (status != 0 || !tbi.TebBaseAddress) {
        return "";
    }

    // 64bit Windows: TEB + 0x08 is StackBase
    DWORD_PTR stackBase = 0;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 8), &stackBase, sizeof(stackBase), &bytesRead) || bytesRead != sizeof(stackBase)) {
        return "";
    }
    if (stackBase < 0x10000) return "";

    // Read 512 bytes near bottom of stack: [stackBase - 0x200, stackBase)
    const DWORD scanSize = 0x200;
    BYTE stackBuf[scanSize] = { 0 };
    LPCVOID scanAddr = reinterpret_cast<LPCVOID>(stackBase - scanSize);
    if (!ReadProcessMemory(hProcess, scanAddr, stackBuf, scanSize, &bytesRead) || bytesRead < 64) {
        return "";
    }

    size_t u64Count = bytesRead / sizeof(DWORD_PTR);
    const DWORD_PTR* ptrs = reinterpret_cast<const DWORD_PTR*>(stackBuf);

    for (size_t i = 0; i < u64Count; ++i) {
        DWORD_PTR p = ptrs[i];
        if (p < 0x10000 || p > 0x7FFFFFFFFFFFULL) continue;

        char objBuf[64] = { 0 };
        if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(p), objBuf, sizeof(objBuf), &bytesRead) && bytesRead >= 16) {
            for (size_t off = 0; off + 4 <= sizeof(objBuf); ++off) {
                if (memcmp(&objBuf[off], "FMOD", 4) == 0) {
                    const char* strStart = &objBuf[off];
                    size_t len = strnlen(strStart, sizeof(objBuf) - off);
                    return std::string(strStart, len);
                }
            }

            const DWORD_PTR* objPtrs = reinterpret_cast<const DWORD_PTR*>(objBuf);
            DWORD_PTR argPtr = objPtrs[1];
            if (argPtr >= 0x10000 && argPtr <= 0x7FFFFFFFFFFFULL) {
                char argBuf[64] = { 0 };
                if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(argPtr), argBuf, sizeof(argBuf), &bytesRead) && bytesRead >= 16) {
                    for (size_t off = 0; off + 4 <= sizeof(argBuf); ++off) {
                        if (memcmp(&argBuf[off], "FMOD", 4) == 0) {
                            const char* strStart = &argBuf[off];
                            size_t len = strnlen(strStart, sizeof(argBuf) - off);
                            return std::string(strStart, len);
                        }
                    }
                }
            }
        }
    }
    return "";
}

static int GetAudioThreadRank(const std::string& threadName) {
    if (threadName.empty()) return 0;
    std::string lower = ToLowerA(threadName);

    // 除外対象 (ユーティリティ、メインループ、I/O ファイルスレッド、監視系)
    if (lower.find("utility") != std::string::npos ||
        lower.find("main") != std::string::npos ||
        lower.find("watchdog") != std::string::npos ||
        lower.find("file thread") != std::string::npos) {
        return 0;
    }

    // 最優先帯: Feeder, WASAPI, ASIO, Playback Thread (DAC 出力最前線)
    if (lower.find("feeder") != std::string::npos) return 100;
    if (lower.find("wasapi") != std::string::npos) return 95; // wasapi_render_thread, WASAPI Exclusive Worker, ao/wasapi, WASAPI (Godot)
    if (lower == "ao") return 92;
    if (lower.find("asio") != std::string::npos) return 90;
    if (lower.find("playback") != std::string::npos && lower.find("decod") == std::string::npos) return 85; // Fb2k Playback Thread
    if (lower.find("audiooutputdevice") != std::string::npos) return 80;

    // 一般オーディオスレッド名
    if (lower.find("audiothread") != std::string::npos || lower.find("craudio") != std::string::npos) return 65;
    if (lower.find("mixer") != std::string::npos) return 50;
    if (lower.find("audio") != std::string::npos || lower.find("playback") != std::string::npos || lower.find("sound") != std::string::npos) return 40;
    if (lower.find("decod") != std::string::npos) return 35;

    // ゲーム系 (FMOD stream 等)
    if (lower.find("stream") != std::string::npos) return 20;
    if (lower.find("fmod") != std::string::npos) return 10;

    return 0;
}

typedef DWORD(WINAPI* pfnGetMappedFileNameA)(HANDLE, LPVOID, LPSTR, DWORD);
static pfnGetMappedFileNameA s_pfnGetMappedFileNameA = nullptr;

static void EnsurePsapiLoaded() {
    if (!s_pfnGetMappedFileNameA) {
        HMODULE hPsapi = LoadLibraryA("psapi.dll");
        if (hPsapi) {
            s_pfnGetMappedFileNameA = reinterpret_cast<pfnGetMappedFileNameA>(
                GetProcAddress(hPsapi, "GetMappedFileNameA")
            );
        }
    }
}

// コールスタック参照シグネチャ走査 (Godot WASAPI 等)
static std::string QueryCallstackAudioSignature(HANDLE hProcess, HANDLE hThread) {
    if (!s_pfnNtQueryInformationThread) return "";

    // スタック上のコード参照シグネチャ走査 (Godot WASAPI 等)
    THREAD_BASIC_INFO_RAW tbi = { 0 };
    ULONG retLen = 0;
    NTSTATUS status = s_pfnNtQueryInformationThread(hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
    if (status != 0 || !tbi.TebBaseAddress) {
        return "";
    }

    // 64bit Windows: TEB + 0x08 is StackBase
    DWORD_PTR stackBase = 0;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 8), &stackBase, sizeof(stackBase), &bytesRead) || bytesRead != sizeof(stackBase)) {
        return "";
    }
    if (stackBase < 0x10000) return "";

    // StackBase 手前 4096 バイトを走査
    const DWORD scanSize = 4096;
    BYTE stackBuf[scanSize] = { 0 };
    LPCVOID scanAddr = reinterpret_cast<LPCVOID>(stackBase - scanSize);
    if (!ReadProcessMemory(hProcess, scanAddr, stackBuf, scanSize, &bytesRead) || bytesRead < 64) {
        return "";
    }

    size_t u64Count = bytesRead / sizeof(DWORD_PTR);
    const DWORD_PTR* ptrs = reinterpret_cast<const DWORD_PTR*>(stackBuf);

    for (size_t i = 0; i < u64Count; ++i) {
        DWORD_PTR p = ptrs[i];
        if (p < 0x100000 || p > 0x7FFFFFFFFFFFULL) continue;

        MEMORY_BASIC_INFORMATION mbi = { 0 };
        if (VirtualQueryEx(hProcess, reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) {
            if (mbi.Protect == PAGE_EXECUTE_READ || mbi.Protect == PAGE_EXECUTE_READWRITE) {
                BYTE codeBuf[1024] = { 0 };
                DWORD_PTR codeStart = (p >= 256) ? (p - 256) : p;
                SIZE_T cRead = 0;
                if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(codeStart), codeBuf, sizeof(codeBuf), &cRead) && cRead >= 128) {
                    // x64 相対参照 [rip + disp32] の走査
                    for (size_t c = 0; c + 7 <= cRead; ++c) {
                        if (codeBuf[c] == 0x48 && (codeBuf[c + 1] == 0x8d || codeBuf[c + 1] == 0x8b)) {
                            BYTE modrm = codeBuf[c + 2];
                            if ((modrm & 0xC7) == 0x05) { // [rip + disp32]
                                INT32 disp = *reinterpret_cast<const INT32*>(&codeBuf[c + 3]);
                                DWORD_PTR targetAddr = (codeStart + c + 7) + disp;
                                if (targetAddr >= 0x10000 && targetAddr <= 0x7FFFFFFFFFFFULL) {
                                    char strBuf[64] = { 0 };
                                    SIZE_T sRead = 0;
                                    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(targetAddr), strBuf, sizeof(strBuf) - 1, &sRead) && sRead >= 8) {
                                        std::string lowerStr = ToLowerA(strBuf);
                                        if (lowerStr.find("audio_driver_wasapi") != std::string::npos ||
                                            lowerStr.find("audiodriverwasapi") != std::string::npos) {
                                            return "WASAPI (Godot AudioDriver)";
                                        }
                                        if (lowerStr.find("audioserver") != std::string::npos) {
                                            return "AudioServer (Godot)";
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // 直近コードバッファ内の ASCII 文字列もチェック
                    std::string lowerCode(reinterpret_cast<const char*>(codeBuf), cRead);
                    lowerCode = ToLowerA(lowerCode);
                    if (lowerCode.find("audio_driver_wasapi") != std::string::npos ||
                        lowerCode.find("audiodriverwasapi") != std::string::npos) {
                        return "WASAPI (Godot AudioDriver)";
                    }
                }
            }
        }
    }
    return "";
}

// オーディオスレッドの優先度に応じた直下（1段階下）の優先度を取得
static int GetOneStepLowerPriority(int audioPriority) {
    if (audioPriority >= THREAD_PRIORITY_TIME_CRITICAL) {
        return THREAD_PRIORITY_HIGHEST; // +15 -> +2
    } else if (audioPriority >= THREAD_PRIORITY_HIGHEST) {
        return THREAD_PRIORITY_ABOVE_NORMAL; // +2 -> +1
    } else if (audioPriority >= THREAD_PRIORITY_ABOVE_NORMAL) {
        return THREAD_PRIORITY_NORMAL; // +1 -> 0
    } else if (audioPriority >= THREAD_PRIORITY_NORMAL) {
        return THREAD_PRIORITY_BELOW_NORMAL; // 0 -> -1
    } else if (audioPriority >= THREAD_PRIORITY_BELOW_NORMAL) {
        return THREAD_PRIORITY_LOWEST; // -1 -> -2
    } else {
        return THREAD_PRIORITY_IDLE; // -2 以下はすべて最低の -15 (IDLE)
    }
}

// Chromium 判定: プロセスの exe ファイルをディスクから読み、"chromeos" ASCII 文字列を検索
bool ThreadIsolator::DetectChromiumExe(HANDLE hProcess) {
    char exePath[MAX_PATH] = { 0 };
    DWORD pathLen = MAX_PATH;
    if (!QueryFullProcessImageNameA(hProcess, 0, exePath, &pathLen) || pathLen == 0) {
        return false;
    }

    FILE* fp = fopen(exePath, "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    if (fileSize <= 0 || fileSize > 10 * 1024 * 1024) {
        fclose(fp);
        return false;
    }
    fseek(fp, 0, SEEK_SET);

    std::vector<BYTE> buf(static_cast<size_t>(fileSize));
    size_t readBytes = fread(buf.data(), 1, static_cast<size_t>(fileSize), fp);
    fclose(fp);
    if (readBytes < 8) return false;

    const char* needle = "chromeos";
    size_t needleLen = 8;
    for (size_t i = 0; i + needleLen <= readBytes; ++i) {
        if (memcmp(&buf[i], needle, needleLen) == 0) {
            char dLog[256];
            snprintf(dLog, sizeof(dLog), "DetectChromiumExe: FOUND 'chromeos' in '%s'", exePath);
            LogDebug(dLog);
            return true;
        }
    }
    return false;
}

// プロセスのコマンドラインを PEB 経由で取得 (ANSI 変換)
std::string ThreadIsolator::QueryProcessCommandLine(HANDLE hProcess) {
    typedef LONG NTSTATUS;
    typedef NTSTATUS(NTAPI* pfnNtQueryInformationProcess)(
        HANDLE, ULONG, PVOID, ULONG, PULONG);

    static pfnNtQueryInformationProcess s_pfnNtQIP = nullptr;
    if (!s_pfnNtQIP) {
        HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
        if (hNtdll) {
            s_pfnNtQIP = reinterpret_cast<pfnNtQueryInformationProcess>(
                GetProcAddress(hNtdll, "NtQueryInformationProcess"));
        }
    }
    if (!s_pfnNtQIP) return "";

    struct PROCESS_BASIC_INFO {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID Reserved3;
    };
    PROCESS_BASIC_INFO pbi = { 0 };
    ULONG retLen = 0;
    NTSTATUS status = s_pfnNtQIP(hProcess, 0, &pbi, sizeof(pbi), &retLen);
    if (status != 0 || !pbi.PebBaseAddress) return "";

    PVOID pProcessParams = nullptr;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess,
        reinterpret_cast<LPCVOID>(reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + 0x20),
        &pProcessParams, sizeof(pProcessParams), &bytesRead) || !pProcessParams) {
        return "";
    }

    struct UNICODE_STRING_RAW {
        USHORT Length;
        USHORT MaximumLength;
        DWORD  padding;
        PVOID  Buffer;
    };
    UNICODE_STRING_RAW cmdLineUs = { 0 };
    if (!ReadProcessMemory(hProcess,
        reinterpret_cast<LPCVOID>(reinterpret_cast<uintptr_t>(pProcessParams) + 0x70),
        &cmdLineUs, sizeof(cmdLineUs), &bytesRead) || !cmdLineUs.Buffer || cmdLineUs.Length == 0) {
        return "";
    }

    USHORT wcharCount = cmdLineUs.Length / sizeof(WCHAR);
    if (wcharCount > 16384) wcharCount = 16384;
    std::vector<WCHAR> wBuf(wcharCount + 1, 0);
    if (!ReadProcessMemory(hProcess, cmdLineUs.Buffer, wBuf.data(),
        static_cast<SIZE_T>(wcharCount) * sizeof(WCHAR), &bytesRead)) {
        return "";
    }
    wBuf[wcharCount] = L'\0';

    int aBufLen = WideCharToMultiByte(CP_ACP, 0, wBuf.data(), wcharCount, nullptr, 0, nullptr, nullptr);
    if (aBufLen <= 0) return "";
    std::string result(static_cast<size_t>(aBufLen), '\0');
    WideCharToMultiByte(CP_ACP, 0, wBuf.data(), wcharCount, &result[0], aBufLen, nullptr, nullptr);
    return result;
}

ThreadIsolator::ThreadIsolator()

    : m_pfnGetThreadDescription(nullptr) {
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        m_pfnGetThreadDescription = reinterpret_cast<PFN_GetThreadDescription>(
            GetProcAddress(hKernel32, "GetThreadDescription")
        );
    }
}

ThreadIsolator::~ThreadIsolator() {
    ResumeAllSuspendedThreads();
}

void ThreadIsolator::Initialize(const GlobalConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_appliedThreads.clear();
    m_prevThreadCpuTimes.clear();
    m_samplingStates.clear();
}

void ThreadIsolator::UpdateConfig(const GlobalConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_appliedThreads.clear();
    m_prevThreadCpuTimes.clear();
    m_samplingStates.clear();
}

GlobalConfig ThreadIsolator::GetConfig() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void ThreadIsolator::AddRule(const ProcessRule& rule) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.rules.push_back(rule);
    m_appliedThreads.clear();
}

void ThreadIsolator::RemoveRule(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_config.rules.size()) {
        auto& rule = m_config.rules[index];
        if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
            HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
            if (hThread) {
                ResumeThread(hThread);
                CloseHandle(hThread);
            }
        }
        m_config.rules.erase(m_config.rules.begin() + index);
    }
}

void ThreadIsolator::UpdateRule(size_t index, const ProcessRule& rule) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_config.rules.size()) {
        m_config.rules[index] = rule;
        m_appliedThreads.clear();
        m_prevThreadCpuTimes.clear();
    }
}

std::vector<ProcessRule> ThreadIsolator::GetRulesSnapshot() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.rules;
}

void ThreadIsolator::ToggleProcessHeuristics(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_config.rules.size()) {
        m_config.rules[index].enableHeuristics = !m_config.rules[index].enableHeuristics;
        if (!m_config.rules[index].enableHeuristics) {
            if (!m_config.rules[index].isAudioIsolated) {
                m_config.rules[index].detectedThreadName = "Standby";
            }
        }
    }
}

void ThreadIsolator::ToggleProcessBypass(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_config.rules.size()) {
        auto& rule = m_config.rules[index];
        rule.isBypassed = !rule.isBypassed;
        if (rule.isBypassed) {
            if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
                HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
                if (hThread) {
                    ResumeThread(hThread);
                    CloseHandle(hThread);
                }
                rule.isAudioThreadSuspended = false;
            }
            rule.isAudioIsolated = false;
            rule.hasIntruderThreads = false;
            if (rule.isRunning) {
                rule.detectedThreadName = "Bypassed";
                if (rule.activePid != 0) {
                    m_trackedAudioThreads.erase(rule.activePid);
                    m_prevThreadCpuTimes.erase(rule.activePid);
                    m_samplingStates.erase(rule.activePid);
                }
            } else {
                rule.detectedThreadName = "";
            }
        } else {
            if (rule.isRunning) {
                rule.detectedThreadName = "Scanning...";
            }
        }
    }
}

bool ThreadIsolator::ToggleSuspendAudioThread(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_config.rules.size()) return false;
    auto& rule = m_config.rules[index];
    if (!rule.isRunning || rule.activeAudioTid == 0) return false;

    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
    if (!hThread) {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "Failed to OpenThread(TID=%lu) for suspend/resume: err=%lu", rule.activeAudioTid, GetLastError());
        LogDebug(errBuf);
        return false;
    }

    if (!rule.isAudioThreadSuspended) {
        DWORD prevCount = SuspendThread(hThread);
        rule.isAudioThreadSuspended = true;
        char logBuf[128];
        snprintf(logBuf, sizeof(logBuf), "Suspended Audio Thread: TID=%lu (prevCount=%lu)", rule.activeAudioTid, prevCount);
        LogDebug(logBuf);
    } else {
        DWORD prevCount = ResumeThread(hThread);
        rule.isAudioThreadSuspended = false;
        char logBuf[128];
        snprintf(logBuf, sizeof(logBuf), "Resumed Audio Thread: TID=%lu (prevCount=%lu)", rule.activeAudioTid, prevCount);
        LogDebug(logBuf);
    }
    CloseHandle(hThread);
    return true;
}

void ThreadIsolator::ResumeAllSuspendedThreads() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& rule : m_config.rules) {
        if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
            HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
            if (hThread) {
                ResumeThread(hThread);
                CloseHandle(hThread);
            }
            rule.isAudioThreadSuspended = false;
        }
    }
}

bool CpuTopology::CheckIfAllECoresCpu() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char nameBuf[256] = { 0 };
        DWORD nameLen = sizeof(nameBuf);
        DWORD type = 0;
        if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(nameBuf), &nameLen) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            std::string name(nameBuf);
            try {
                if (std::regex_search(name, std::regex(R"(\b(N100|N95|N97|N200|N50|N300|N305|N150|N250)\b)", std::regex_constants::icase)) ||
                    std::regex_search(name, std::regex(R"(Processor\s+N\d+)", std::regex_constants::icase)) ||
                    std::regex_search(name, std::regex(R"(i3-N\d+)", std::regex_constants::icase))) {
                    return true;
                }
            } catch (...) {
            }
        } else {
            RegCloseKey(hKey);
        }
    }
    return false;
}

int CpuTopology::GetSystemCoreCount() {
    typedef DWORD (WINAPI *PFN_GetActiveProcessorCount)(WORD);
    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    if (hK32) {
        auto pfn = reinterpret_cast<PFN_GetActiveProcessorCount>(GetProcAddress(hK32, "GetActiveProcessorCount"));
        if (pfn) {
            DWORD count = pfn(0xFFFF /* ALL_PROCESSOR_GROUPS */);
            if (count > 0) return static_cast<int>(count);
        }
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<int>(si.dwNumberOfProcessors);
}

std::vector<CoreInfo> CpuTopology::GetCpuCores() {
    std::vector<CoreInfo> list;
    int totalLogical = GetSystemCoreCount();
    if (totalLogical <= 0) totalLogical = 1;

    bool isAllECores = CheckIfAllECoresCpu();

    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        std::string prefix = isAllECores ? "E" : "#";
        for (int i = 0; i < totalLogical; i++) {
            list.push_back({ i, 0, prefix + std::to_string(i) });
        }
        return list;
    }

    std::vector<BYTE> buffer(length);
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX current = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
        DWORD offset = 0;

        std::map<int, BYTE> effMap;
        int maxBitFound = -1;

        while (offset < length) {
            if (current->Relationship == RelationProcessorCore) {
                BYTE effClass = current->Processor.EfficiencyClass;
                KAFFINITY mask = current->Processor.GroupMask[0].Mask;
                for (int bit = 0; bit < 64; bit++) {
                    if ((mask & (1ULL << bit)) != 0) {
                        effMap[bit] = effClass;
                        if (bit > maxBitFound) maxBitFound = bit;
                    }
                }
            }
            offset += current->Size;
            current = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(reinterpret_cast<BYTE*>(current) + current->Size);
        }

        int coreCount = (std::max)(totalLogical, maxBitFound + 1);

        std::set<BYTE> distinctClasses;
        BYTE maxEff = 0;
        for (const auto& kv : effMap) {
            distinctClasses.insert(kv.second);
            if (kv.second > maxEff) maxEff = kv.second;
        }

        bool isHybrid = distinctClasses.size() > 1;

        for (int i = 0; i < coreCount; i++) {
            BYTE eff = 0;
            auto it = effMap.find(i);
            if (it != effMap.end()) eff = it->second;

            std::string label;
            if (isHybrid) {
                // ハイブリッドCPU (12th/13th/14th Gen, Core Ultra等): P0, P1... / E16, E17...
                label = (eff == maxEff) ? ("P" + std::to_string(i)) : ("E" + std::to_string(i));
            } else if (isAllECores) {
                // 純EコアCPU (Intel N100, N95, N200, N300, N305等): E0, E1, E2, E3...
                label = "E" + std::to_string(i);
            } else {
                // 通常の均一CPU (Xeon, Ryzen, 従来型Core等): #0, #1, #2...
                label = "#" + std::to_string(i);
            }

            list.push_back({ i, eff, label });
        }
    }

    if (list.empty()) {
        std::string prefix = isAllECores ? "E" : "#";
        for (int i = 0; i < totalLogical; i++) {
            list.push_back({ i, 0, prefix + std::to_string(i) });
        }
    }

    return list;
}

int ThreadIsolator::GetSystemCoreCount() {
    return CpuTopology::GetSystemCoreCount();
}

DWORD_PTR ThreadIsolator::GetFullCoreMask(int coreCount) {
    if (coreCount >= 64) return ~0ULL;
    return (1ULL << coreCount) - 1ULL;
}

DWORD_PTR ThreadIsolator::MakeCoreMask(int coreIndex) {
    if (coreIndex < 0 || coreIndex >= 64) return 0;
    return (1ULL << coreIndex);
}

DWORD_PTR ThreadIsolator::MakeDefaultNormalMask(int coreCount, int isolatedCore) {
    DWORD_PTR full = GetFullCoreMask(coreCount);
    DWORD_PTR iso = MakeCoreMask(isolatedCore);
    DWORD_PTR normal = full & ~iso;
    return normal ? normal : full;
}

std::string ThreadIsolator::QueryThreadNameA(HANDLE hThread) {
    if (!m_pfnGetThreadDescription) return "";
    
    PWSTR pDesc = nullptr;
    HRESULT hr = m_pfnGetThreadDescription(hThread, &pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        int len = WideCharToMultiByte(CP_ACP, 0, pDesc, -1, nullptr, 0, nullptr, nullptr);
        std::string result;
        if (len > 0) {
            result.resize(len - 1);
            WideCharToMultiByte(CP_ACP, 0, pDesc, -1, &result[0], len, nullptr, nullptr);
        }
        LocalFree(pDesc);
        return result;
    }
    return "";
}

bool ThreadIsolator::IsNamedAudioThread(const std::string& threadName) {
    return GetAudioThreadRank(threadName) > 0;
}

bool ThreadIsolator::ScanAndIsolate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_config.rules.empty()) return false;

    bool stateChanged = false;
    int coreCount = GetSystemCoreCount();
    std::string heuristicsStatus = "";

    // 1. 実行中の全プロセスを取得 (ANSI)
    HANDLE hSnapProc = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapProc == INVALID_HANDLE_VALUE) return false;

    std::unordered_map<std::string, std::vector<DWORD>> runningProcesses;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (Process32First(hSnapProc, &pe)) {
        do {
            std::string procNameLower = ToLowerA(pe.szExeFile);
            runningProcesses[procNameLower].push_back(pe.th32ProcessID);
        } while (Process32Next(hSnapProc, &pe));
    }
    CloseHandle(hSnapProc);

    // スレッド一覧を 1 回だけ取得して PID 別に分類 (高速・低負荷化)
    std::unordered_map<DWORD, std::vector<THREADENTRY32>> processThreads;
    HANDLE hSnapThread = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapThread != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (Thread32First(hSnapThread, &te)) {
            do {
                processThreads[te.th32OwnerProcessID].push_back(te);
            } while (Thread32Next(hSnapThread, &te));
        }
        CloseHandle(hSnapThread);
    }

    // 終了したプロセスのトラッキング情報をクリーンアップ
    std::unordered_set<DWORD> allActivePids;
    for (const auto& kv : runningProcesses) {
        for (DWORD p : kv.second) allActivePids.insert(p);
    }
    for (auto itTrack = m_trackedAudioThreads.begin(); itTrack != m_trackedAudioThreads.end(); ) {
        if (allActivePids.find(itTrack->first) == allActivePids.end()) {
            itTrack = m_trackedAudioThreads.erase(itTrack);
        } else {
            ++itTrack;
        }
    }
    for (auto itCpu = m_prevThreadCpuTimes.begin(); itCpu != m_prevThreadCpuTimes.end(); ) {
        if (allActivePids.find(itCpu->first) == allActivePids.end()) {
            itCpu = m_prevThreadCpuTimes.erase(itCpu);
        } else {
            ++itCpu;
        }
    }

    // 2. 各登録ルールのプロセスを検査・アフィニティ制御
    for (auto& rule : m_config.rules) {
        std::string targetLower = ToLowerA(rule.processName);
        auto it = runningProcesses.find(targetLower);

        if (it == runningProcesses.end() || it->second.empty()) {
            if (rule.isRunning) {
                if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
                    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
                    if (hThread) {
                        ResumeThread(hThread);
                        CloseHandle(hThread);
                    }
                    rule.isAudioThreadSuspended = false;
                }
                if (rule.activePid != 0) {
                    m_samplingStates.erase(rule.activePid);
                }
                rule.isRunning = false;
                rule.activePid = 0;
                rule.activeAudioTid = 0;
                rule.detectedThreadName = "";
                rule.isAudioIsolated = false;
                stateChanged = true;
            }
            continue;
        }

        rule.isRunning = true;

        // 監視除外・ペンディング (チェックON時) はスレッド走査・アフィニティ制御・優先度制御を完全スキップ
        if (rule.isBypassed) {
            if (rule.detectedThreadName != "Bypassed") {
                rule.detectedThreadName = "Bypassed";
                stateChanged = true;
            }
            rule.isAudioIsolated = false;
            rule.hasIntruderThreads = false;
            for (DWORD pid : it->second) {
                m_trackedAudioThreads.erase(pid);
                m_prevThreadCpuTimes.erase(pid);
                m_samplingStates.erase(pid);
            }
            continue;
        }

        // Chromium 系ブラウザ自動判定 (初回のみ exe ファイルを走査)
        if (rule.isChromium == -1) {
            HANDLE hFirstProc = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, it->second[0]);
            if (hFirstProc) {
                rule.isChromium = DetectChromiumExe(hFirstProc) ? 1 : 0;
                CloseHandle(hFirstProc);
                char cLog[128];
                snprintf(cLog, sizeof(cLog), "Chromium detect: '%s' -> isChromium=%d",
                         rule.processName.c_str(), rule.isChromium);
                LogDebug(cLog);
            } else {
                rule.isChromium = 0;
            }
        }

        int totalThreadCount = 0;
        bool audioDetectedInAny = false;
        std::string primaryAudioThreadName = "";
        DWORD primaryAudioPid = 0;
        DWORD primaryAudioTid = 0;

        // コアマスクの計算 (複数コア指定に対応)
        DWORD_PTR audioMask = rule.audioAffinityMask ? rule.audioAffinityMask : MakeCoreMask(rule.audioCore);
        DWORD_PTR baseNormal = (rule.normalAffinityMask != 0)
            ? rule.normalAffinityMask
            : (m_config.normalAffinityMask ? m_config.normalAffinityMask : GetFullCoreMask(coreCount));
        // 通常スレッドのアフィニティマスクからオーディオコア群を物理的に完全除外
        DWORD_PTR normalMask = baseNormal & ~audioMask;
        if (normalMask == 0) {
            normalMask = MakeDefaultNormalMask(coreCount, rule.audioCore);
        }
        DWORD_PTR parentMask = audioMask | baseNormal;

        // ── Chromium 専用パス: Audio Service のみスレッド走査、他は AffinityMask のみ ──
        if (rule.isChromium == 1) {
            DWORD audioServicePid = 0;

            // Audio Service PID を特定 (コマンドライン走査)
            for (DWORD pid : it->second) {
                HANDLE hProc = OpenProcess(
                    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_SET_INFORMATION,
                    FALSE, pid);
                if (!hProc) continue;

                std::string cmdLine = QueryProcessCommandLine(hProc);
                if (cmdLine.find("audio.mojom.AudioService") != std::string::npos) {
                    audioServicePid = pid;
                    rule.audioServicePid = pid;
                    CloseHandle(hProc);
                    char asLog[128];
                    snprintf(asLog, sizeof(asLog), "Chromium AudioService found: PID=%lu", pid);
                    LogDebug(asLog);
                    break;
                }

                // Audio Service 以外: プロセス単位で audioCore を除外するマスクのみ適用
                DWORD_PTR currentProcMask = 0, sysMask = 0;
                if (GetProcessAffinityMask(hProc, &currentProcMask, &sysMask)) {
                    if (currentProcMask != normalMask) {
                        SetProcessAffinityMask(hProc, normalMask);
                    }
                }
                CloseHandle(hProc);
            }

            if (audioServicePid == 0) {
                // Audio Service 未検出: Standby 表示
                if (rule.detectedThreadName != "Standby") {
                    rule.detectedThreadName = "Standby";
                    stateChanged = true;
                }
                rule.isAudioIsolated = false;
                continue;
            }

            // Audio Service プロセスのスレッドを走査 (軽量: 通常3スレッド程度)
            auto ptIt = processThreads.find(audioServicePid);
            if (ptIt == processThreads.end() || ptIt->second.empty()) continue;

            HANDLE hAsProc = OpenProcess(
                PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                FALSE, audioServicePid);
            if (!hAsProc) continue;

            // 親プロセスのアフィニティを拡張
            DWORD_PTR currentProcMask = 0, sysMask = 0;
            if (GetProcessAffinityMask(hAsProc, &currentProcMask, &sysMask)) {
                if ((currentProcMask & parentMask) != parentMask) {
                    SetProcessAffinityMask(hAsProc, parentMask);
                }
            }

            totalThreadCount = static_cast<int>(ptIt->second.size());

            // CyclesDelta サンプリングでオーディオスレッドを特定
            DWORD bestTid = 0;
            ULONGLONG bestDelta = 0;
            auto& sampleState = m_samplingStates[audioServicePid];
            FILETIME cr, ex, kr, ur;

            // 永続トラッキングから事前解決
            {
                auto trackIt = m_trackedAudioThreads.find(audioServicePid);
                if (trackIt != m_trackedAudioThreads.end() && !trackIt->second.empty()) {
                    bestTid = trackIt->second.begin()->first;
                    primaryAudioThreadName = trackIt->second.begin()->second;
                }
            }

            struct AsThreadInfo { DWORD tid; HANDLE hThread; };
            std::vector<AsThreadInfo> asThreads;

            for (const auto& te : ptIt->second) {
                HANDLE hThread = OpenThread(
                    THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                    FALSE, te.th32ThreadID);
                if (!hThread) continue;
                asThreads.push_back({ te.th32ThreadID, hThread });

                // スレッド名チェック (wasapi_render_thread 等)
                std::string tName = QueryThreadNameA(hThread);
                int rank = GetAudioThreadRank(tName);
                if (rank > 0 && bestTid == 0) {
                    bestTid = te.th32ThreadID;
                    primaryAudioThreadName = tName;
                    m_trackedAudioThreads[audioServicePid].clear();
                    m_trackedAudioThreads[audioServicePid][bestTid] = primaryAudioThreadName;
                }

                // CPU 時間サンプリング
                if (GetThreadTimes(hThread, &cr, &ex, &kr, &ur)) {
                    ULONGLONG totalTime = ((static_cast<ULONGLONG>(kr.dwHighDateTime) << 32) | kr.dwLowDateTime)
                                        + ((static_cast<ULONGLONG>(ur.dwHighDateTime) << 32) | ur.dwLowDateTime);
                    auto prevIt = sampleState.lastCpuTime.find(te.th32ThreadID);
                    if (prevIt != sampleState.lastCpuTime.end()) {
                        ULONGLONG d = (totalTime >= prevIt->second) ? (totalTime - prevIt->second) : 0;
                        if (d > bestDelta) {
                            bestDelta = d;
                            if (bestTid == 0) {
                                bestTid = te.th32ThreadID;
                            }
                        }
                    }
                    sampleState.lastCpuTime[te.th32ThreadID] = totalTime;
                }
            }

            // 名前で見つからず CyclesDelta 最大で選定
            if (bestTid != 0 && primaryAudioThreadName.empty()) {
                primaryAudioThreadName = "Audio (TID: " + std::to_string(bestTid) + ")";
                m_trackedAudioThreads[audioServicePid].clear();
                m_trackedAudioThreads[audioServicePid][bestTid] = primaryAudioThreadName;
            }

            // アフィニティ・優先度の適用
            for (const auto& at : asThreads) {
                if (at.tid == bestTid) {
                    SetThreadAffinityMask(at.hThread, audioMask);
                    SetThreadPriority(at.hThread, rule.audioPriority);
                    SetThreadIdealProcessor(at.hThread, static_cast<DWORD>(rule.audioCore));
                    audioDetectedInAny = true;
                    primaryAudioPid = audioServicePid;
                    primaryAudioTid = bestTid;
                } else {
                    SetThreadAffinityMask(at.hThread, normalMask);
                }
                CloseHandle(at.hThread);
            }

            CloseHandle(hAsProc);

            // 残りの全子プロセスにも normalMask を適用 (Audio Service 以外)
            for (DWORD pid : it->second) {
                if (pid == audioServicePid) continue;
                HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
                if (!hProc) continue;
                DWORD_PTR cpMask = 0, smask = 0;
                if (GetProcessAffinityMask(hProc, &cpMask, &smask)) {
                    if (cpMask != normalMask) {
                        SetProcessAffinityMask(hProc, normalMask);
                    }
                }
                CloseHandle(hProc);
            }

            // 状態更新
            if (audioDetectedInAny) {
                if (!rule.isAudioIsolated) { rule.isAudioIsolated = true; stateChanged = true; }
                if (rule.activePid != primaryAudioPid) { rule.activePid = primaryAudioPid; stateChanged = true; }
                if (rule.activeAudioTid != primaryAudioTid) { rule.activeAudioTid = primaryAudioTid; stateChanged = true; }
                if (rule.detectedThreadName != primaryAudioThreadName) { rule.detectedThreadName = primaryAudioThreadName; stateChanged = true; }
            } else {
                if (rule.isAudioIsolated) { rule.isAudioIsolated = false; stateChanged = true; }
                if (rule.detectedThreadName != "Scanning...") { rule.detectedThreadName = "Scanning..."; stateChanged = true; }
            }
            if (rule.currentThreadCount != totalThreadCount) { rule.currentThreadCount = totalThreadCount; stateChanged = true; }
            continue;
        }

        // ── 既存パス（Firefox, foobar2000, ゲーム等）──

        // 同名プロセスの全 PID (マルチプロセス) を走査
        for (DWORD pid : it->second) {
            auto ptIt = processThreads.find(pid);
            if (ptIt == processThreads.end() || ptIt->second.empty()) continue;

            HANDLE hProcess = OpenProcess(
                PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, 
                FALSE, 
                pid
            );
            if (!hProcess) {
                char errBuf[128];
                snprintf(errBuf, sizeof(errBuf), "OpenProcess FAILED: pid=%lu, err=%lu", pid, GetLastError());
                LogDebug(errBuf);
                continue;
            }

            // アイドル優先度クラス (IDLE_PRIORITY_CLASS) のプロセスはスキップ (休眠タブ走査負荷の削減: ブラウザ系のみ)
            if (rule.processPriorityClass == 0) {
                std::string pName = ToLowerA(rule.processName);
                bool isBrowser = (pName.find("chrome") != std::string::npos ||
                                  pName.find("vivaldi") != std::string::npos ||
                                  pName.find("edge") != std::string::npos ||
                                  pName.find("brave") != std::string::npos ||
                                  pName.find("opera") != std::string::npos);
                if (isBrowser) {
                    DWORD pClass = GetPriorityClass(hProcess);
                    if (pClass == IDLE_PRIORITY_CLASS) {
                        CloseHandle(hProcess);
                        continue;
                    }
                }
            }

            if (rule.processPriorityClass != 0) {
                SetPriorityClass(hProcess, rule.processPriorityClass);
            }

            // 親プロセスの Affinity Mask を拡張
            DWORD_PTR currentProcMask = 0, sysMask = 0;
            if (GetProcessAffinityMask(hProcess, &currentProcMask, &sysMask)) {
                if ((currentProcMask & parentMask) != parentMask) {
                    SetProcessAffinityMask(hProcess, parentMask);
                }
            }

            std::unordered_set<DWORD> aliveTids;

            struct LocalThreadInfo {
                DWORD tid;
                LONG basePri;
                int priority;
                std::string threadName;
                HANDLE hThread;
                DWORD_PTR currentAffinity;
            };
            std::vector<LocalThreadInfo> threadInfos;

            DWORD identifiedAudioTid = 0;
            std::string identifiedAudioLabel = "";

            int bestRank = 0;
            DWORD bestRankTid = 0;
            std::string bestRankLabel = "";

            DWORD mainThreadTid = ptIt->second.empty() ? 0 : ptIt->second[0].th32ThreadID;

            // 永続トラッキングから事前解決 (ループ前にオーディオTIDを確定させ、重い走査をスキップ)
            {
                auto trackPidIt = m_trackedAudioThreads.find(pid);
                if (trackPidIt != m_trackedAudioThreads.end() && !trackPidIt->second.empty()) {
                    auto firstTracked = trackPidIt->second.begin();
                    identifiedAudioTid = firstTracked->first;
                    identifiedAudioLabel = firstTracked->second;
                }
            }

            // 1. 各スレッドの基本情報を収集 & 永続トラッキング・名前一致を先行判定
            for (const auto& te : ptIt->second) {
                totalThreadCount++;
                aliveTids.insert(te.th32ThreadID);

                HANDLE hThread = OpenThread(
                    THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                    FALSE,
                    te.th32ThreadID
                );
                if (!hThread) continue;

                std::string threadName = QueryThreadNameA(hThread);
                if (threadName.empty() && identifiedAudioTid == 0) {
                    threadName = QueryFmodOrUnityThreadName(hProcess, hThread);
                }
                if (threadName.empty() && identifiedAudioTid == 0) {
                    threadName = QueryCallstackAudioSignature(hProcess, hThread);
                }
                int priority = GetThreadPriority(hThread);
                DWORD_PTR currentAff = QueryThreadAffinityMask(hThread);

                threadInfos.push_back({ te.th32ThreadID, te.tpBasePri, priority, threadName, hThread, currentAff });

                // (A) 永続トラッキングチェック
                auto trackPidIt = m_trackedAudioThreads.find(pid);
                if (trackPidIt != m_trackedAudioThreads.end()) {
                    auto tIt = trackPidIt->second.find(te.th32ThreadID);
                    if (tIt != trackPidIt->second.end()) {
                        identifiedAudioTid = te.th32ThreadID;
                        identifiedAudioLabel = tIt->second;
                        if (!threadName.empty() && identifiedAudioLabel != threadName) {
                            identifiedAudioLabel = threadName;
                            trackPidIt->second[te.th32ThreadID] = identifiedAudioLabel;
                        }
                    }
                }

                // (B) スレッド名ランク判定
                int rank = GetAudioThreadRank(threadName);
                if (rank > bestRank) {
                    bestRank = rank;
                    bestRankTid = te.th32ThreadID;
                    bestRankLabel = threadName;
                }
            }

            // (B-2) 明確なオーディオスレッド名の一致判定 (未特定時、またはより高ランクな Feeder 等を発見した場合に昇格)
            if (bestRankTid != 0) {
                int currentTrackedRank = identifiedAudioTid != 0 ? GetAudioThreadRank(identifiedAudioLabel) : 0;
                if (identifiedAudioTid == 0 || bestRank > currentTrackedRank) {
                    identifiedAudioTid = bestRankTid;
                    identifiedAudioLabel = bestRankLabel;
                    m_trackedAudioThreads[pid].clear();
                    m_trackedAudioThreads[pid][bestRankTid] = bestRankLabel;
                    char bLog[128];
                    snprintf(bLog, sizeof(bLog), "Selected audio thread by rank (%d): PID=%lu, TID=%lu, Name='%s'",
                             bestRank, pid, identifiedAudioTid, identifiedAudioLabel.c_str());
                    LogDebug(bLog);
                }
            }

            // 2. 未特定の場合：無名スレッド判定 (既存隔離の引き継ぎ / 10秒間サンプリング・最上delta除外・2〜10位候補検査)
            if (identifiedAudioTid == 0) {
                // (C-0) 既存隔離スレッドの引き継ぎ (Adopt)
                for (const auto& ti : threadInfos) {
                    if (ti.tid == mainThreadTid) continue; // メインスレッドは除外
                    if (ti.threadName.empty() && ti.priority == rule.audioPriority) {
                        DWORD_PTR currentAff = QueryThreadAffinityMask(ti.hThread);
                        if (currentAff == audioMask) {
                            identifiedAudioTid = ti.tid;
                            identifiedAudioLabel = "Audio (TID: " + std::to_string(identifiedAudioTid) + ")";
                            m_trackedAudioThreads[pid][identifiedAudioTid] = identifiedAudioLabel;
                            char aLog[128];
                            snprintf(aLog, sizeof(aLog), "Adopted existing isolated audio thread: PID=%lu, TID=%lu", pid, identifiedAudioTid);
                            LogDebug(aLog);
                            break;
                        }
                    }
                }
            }

            // (C) 10秒間サンプリング・最上 delta 除外・2〜10位候補検査エンジン
            if (identifiedAudioTid == 0) {
                if (m_config.enableHeuristics && rule.enableHeuristics) {
                    auto& sampleState = m_samplingStates[pid];
                    FILETIME cr, ex, kr, ur;

                    for (const auto& ti : threadInfos) {
                        if (GetThreadTimes(ti.hThread, &cr, &ex, &kr, &ur)) {
                            ULONGLONG totalTime = ((static_cast<ULONGLONG>(kr.dwHighDateTime) << 32) | kr.dwLowDateTime)
                                                + ((static_cast<ULONGLONG>(ur.dwHighDateTime) << 32) | ur.dwLowDateTime);
                            auto prevIt = sampleState.lastCpuTime.find(ti.tid);
                            if (prevIt != sampleState.lastCpuTime.end()) {
                                ULONGLONG d = (totalTime >= prevIt->second) ? (totalTime - prevIt->second) : 0;
                                sampleState.accumulatedDelta[ti.tid] += d;
                            }
                            sampleState.lastCpuTime[ti.tid] = totalTime;
                        }
                    }

                    sampleState.sampleTurns++;

                    if (sampleState.sampleTurns < 20) {
                        // 10秒未満: サンプリング中ステータス表示 (500ms * 20 = 10.0s)
                        float sec = sampleState.sampleTurns * 0.5f;
                        char sBuf[64];
                        snprintf(sBuf, sizeof(sBuf), "Heuristics: Sampling (%.1f/10.0s)...", sec);
                        heuristicsStatus = sBuf;
                    } else {
                        // 10秒経過: ランキング作成・最上delta除外・2〜10位候補検査
                        std::vector<std::pair<DWORD, ULONGLONG>> ranking;
                        for (const auto& kv : sampleState.accumulatedDelta) {
                            ranking.push_back({ kv.first, kv.second });
                        }
                        std::sort(ranking.begin(), ranking.end(), [](const auto& a, const auto& b) {
                            return a.second > b.second;
                        });

                        if (ranking.size() >= 2) {
                            // 第1位 (最上 delta スレッド: メイン描画ループ等) は除外！
                            // 第2位 〜 第10位 (最大 9 本) を候補群とする
                            size_t maxCandidates = std::min<size_t>(ranking.size(), 10);
                            DWORD bestTid = 0;
                            ULONGLONG bestDelta = 0;

                            for (size_t r = 1; r < maxCandidates; ++r) {
                                DWORD candTid = ranking[r].first;
                                ULONGLONG candDelta = ranking[r].second;
                                if (candDelta == 0) continue;

                                for (const auto& ti : threadInfos) {
                                    if (ti.tid == candTid) {
                                        EnsurePsapiLoaded();
                                        void* sAddr = QueryThreadStartAddress(ti.hThread);
                                        std::string modName = "";
                                        if (sAddr && s_pfnGetMappedFileNameA) {
                                            char mPath[MAX_PATH] = { 0 };
                                            MEMORY_BASIC_INFORMATION mbi = { 0 };
                                            if (VirtualQueryEx(hProcess, sAddr, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
                                                if (s_pfnGetMappedFileNameA(hProcess, mbi.AllocationBase, mPath, sizeof(mPath)) > 0) {
                                                    modName = ToLowerA(mPath);
                                                }
                                            }
                                        }

                                        // オーディオ関連キーワード (dsound, winmm, pxtone, audioses, audio, sound)
                                        if (modName.find("dsound.dll") != std::string::npos ||
                                            modName.find("winmm.dll") != std::string::npos ||
                                            modName.find("pxtone") != std::string::npos ||
                                            modName.find("audioses") != std::string::npos ||
                                            modName.find("audio") != std::string::npos ||
                                            modName.find("sound") != std::string::npos) {
                                            bestTid = candTid;
                                            bestDelta = candDelta;
                                            break;
                                        }

                                        // または適正ポーリング帯 (20ターン累計で 100,000 〜 5,000,000 ticks)
                                        if (candDelta >= 100000 && candDelta <= 5000000) {
                                            if (bestTid == 0) {
                                                bestTid = candTid;
                                                bestDelta = candDelta;
                                            }
                                        }
                                        break;
                                    }
                                }
                                if (bestTid != 0 && bestTid == candTid) {
                                    break;
                                }
                            }

                            if (bestTid == 0 && ranking.size() >= 2) {
                                bestTid = ranking[1].first;
                                bestDelta = ranking[1].second;
                            }

                            if (bestTid != 0) {
                                identifiedAudioTid = bestTid;
                                identifiedAudioLabel = "Audio (TID: " + std::to_string(identifiedAudioTid) + ")";
                                m_trackedAudioThreads[pid][identifiedAudioTid] = identifiedAudioLabel;
                                char hLog[128];
                                snprintf(hLog, sizeof(hLog), "Heuristics 10s Sampling MATCH: PID=%lu, TID=%lu (accDelta=%llu)",
                                         pid, identifiedAudioTid, (unsigned long long)bestDelta);
                                LogDebug(hLog);
                                heuristicsStatus = "";
                            }
                        }
                    }
                }
            }

            // 3. アフィニティおよび優先度の適用
            std::vector<DWORD> intruderTids;
            DWORD_PTR effectiveNormal = normalMask;

            // まず非オーディオスレッド (通常スレッド群) を通常コア群へ先行退避し、真の侵入者を物理検出
            for (const auto& ti : threadInfos) {
                if (ti.tid == identifiedAudioTid) continue;

                // スキャン時点 (退避前) の物理アフィニティを検査:
                // オーディオコア (audioMask) を実行対象に含んでおり、かつ
                // (A) オーディオコア単独に自己バインドしている、または
                // (B) 以前のサイクルで通常コア群へ退避させたにもかかわらず自らオーディオコアへ再バインドして居座るスレッド
                if (identifiedAudioTid != 0 && (ti.currentAffinity & audioMask) != 0) {
                    bool isSpecificToAudio = ((ti.currentAffinity & ~audioMask) == 0);
                    bool wasPreviouslyEvacuated = (m_appliedThreads.find(ti.tid) != m_appliedThreads.end());
                    char dbgBuf[256];
                    snprintf(dbgBuf, sizeof(dbgBuf), "IntruderCheck: PID=%lu, TID=%lu, aff=0x%llX, audioMask=0x%llX, specific=%d, evac=%d",
                             pid, ti.tid, (unsigned long long)ti.currentAffinity, (unsigned long long)audioMask, isSpecificToAudio, wasPreviouslyEvacuated);
                    LogDebug(dbgBuf);
                    if (isSpecificToAudio || wasPreviouslyEvacuated) {
                        intruderTids.push_back(ti.tid);
                    }
                }

                // 通常スレッドの Ideal Processor を通常コアに設定
                DWORD normalIdeal = 0;
                for (int c = 0; c < coreCount; ++c) {
                    if ((normalMask & (1ULL << c)) != 0) {
                        normalIdeal = static_cast<DWORD>(c);
                        if ((ti.tid % coreCount) <= static_cast<DWORD>(c)) break;
                    }
                }
                SetThreadIdealProcessor(ti.hThread, normalIdeal);

                // 通常コア群 (effectiveNormal) へ退避
                DWORD_PTR prevMask = SetThreadAffinityMask(ti.hThread, effectiveNormal);
                if (prevMask != 0) {
                    m_appliedThreads[ti.tid] = effectiveNormal;
                    if (prevMask != effectiveNormal) {
                        stateChanged = true;
                    }
                }
            }

            bool hasIntruders = !intruderTids.empty();
            rule.hasIntruderThreads = hasIntruders;
            char hLog[128];
            snprintf(hLog, sizeof(hLog), "IntrudersSummary: PID=%lu, count=%zu, hasIntruders=%d", pid, intruderTids.size(), hasIntruders);
            LogDebug(hLog);

            // オーディオスレッドの適用優先度 (targetAudioPrio) の決定
            int targetAudioPrio = rule.audioPriority;
            if (hasIntruders) {
                // 侵入者が存在する場合：オーディオ優先度が -15 (IDLE) なら -2 (LOWEST) に 1段階昇格！
                if (targetAudioPrio <= THREAD_PRIORITY_IDLE) {
                    targetAudioPrio = THREAD_PRIORITY_LOWEST;
                }
            }

            // 侵入者スレッドの適用優先度 (オーディオスレッドの 1段階下)
            int intruderPrio = GetOneStepLowerPriority(targetAudioPrio);

            // オーディオスレッドへの適用
            if (identifiedAudioTid != 0) {
                for (const auto& ti : threadInfos) {
                    if (ti.tid == identifiedAudioTid) {
                        audioDetectedInAny = true;
                        if (primaryAudioThreadName.empty()) {
                            primaryAudioThreadName = !ti.threadName.empty() ? ti.threadName : identifiedAudioLabel;
                            primaryAudioPid = pid;
                            primaryAudioTid = ti.tid;
                        }

                        // オーディオスレッド優先度設定 (昇格後の targetAudioPrio を適用)
                        if (ti.priority != targetAudioPrio) {
                            SetThreadPriority(ti.hThread, targetAudioPrio);
                        }

                        // Ideal Processor をオーディオコアに固定
                        SetThreadIdealProcessor(ti.hThread, static_cast<DWORD>(rule.audioCore));

                        // アフィニティを audioMask に設定
                        DWORD_PTR prevMask = SetThreadAffinityMask(ti.hThread, audioMask);
                        if (prevMask != 0 && prevMask != audioMask) {
                            m_appliedThreads[ti.tid] = audioMask;
                            rule.applyCount++;
                            stateChanged = true;
                        }
                        break;
                    }
                }
            }

            // 真の侵入者スレッドにのみ降格優先度 (intruderPrio) を適用
            // ※通常コア群へ正常退避できた通常スレッドの優先度には一切手を触れない！
            for (DWORD intTid : intruderTids) {
                for (const auto& ti : threadInfos) {
                    if (ti.tid == intTid) {
                        if (ti.priority != intruderPrio) {
                            SetThreadPriority(ti.hThread, intruderPrio);
                            char iLog[128];
                            snprintf(iLog, sizeof(iLog), "Adjusted true intruder thread priority to %d (audio is %d): PID=%lu, TID=%lu",
                                     intruderPrio, targetAudioPrio, pid, ti.tid);
                            LogDebug(iLog);
                        }
                        break;
                    }
                }
            }

            // ハンドル解放
            for (const auto& ti : threadInfos) {
                CloseHandle(ti.hThread);
            }


            // 消滅したスレッドのトラッキング情報および CPU 時間記録をクリーンアップ
            auto trackCleanIt = m_trackedAudioThreads.find(pid);
            if (trackCleanIt != m_trackedAudioThreads.end()) {
                for (auto tIt = trackCleanIt->second.begin(); tIt != trackCleanIt->second.end(); ) {
                    if (aliveTids.find(tIt->first) == aliveTids.end()) {
                        tIt = trackCleanIt->second.erase(tIt);
                    } else {
                        ++tIt;
                    }
                }
            }
            auto cpuCleanIt = m_prevThreadCpuTimes.find(pid);
            if (cpuCleanIt != m_prevThreadCpuTimes.end()) {
                for (auto itCpu = cpuCleanIt->second.begin(); itCpu != cpuCleanIt->second.end(); ) {
                    if (aliveTids.find(itCpu->first) == aliveTids.end()) {
                        itCpu = cpuCleanIt->second.erase(itCpu);
                    } else {
                        ++itCpu;
                    }
                }
            }

            CloseHandle(hProcess);
        }

        if (audioDetectedInAny) {
            if (!rule.isAudioIsolated) {
                rule.isAudioIsolated = true;
                stateChanged = true;
            }
            if (rule.activePid != primaryAudioPid) {
                rule.activePid = primaryAudioPid;
                stateChanged = true;
            }
            if (rule.activeAudioTid != primaryAudioTid) {
                if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
                    HANDLE hOld = OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
                    if (hOld) {
                        ResumeThread(hOld);
                        CloseHandle(hOld);
                    }
                    rule.isAudioThreadSuspended = false;
                }
                rule.activeAudioTid = primaryAudioTid;
                stateChanged = true;
            }
            if (rule.detectedThreadName != primaryAudioThreadName) {
                rule.detectedThreadName = primaryAudioThreadName;
                stateChanged = true;
            }
        } else {
            if (rule.isAudioIsolated) {
                rule.isAudioIsolated = false;
                stateChanged = true;
            }
            if (rule.activeAudioTid != 0) {
                if (rule.isAudioThreadSuspended) {
                    HANDLE hOld = OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
                    if (hOld) {
                        ResumeThread(hOld);
                        CloseHandle(hOld);
                    }
                    rule.isAudioThreadSuspended = false;
                }
                rule.activeAudioTid = 0;
                stateChanged = true;
            }
            std::string notDetectedName = (m_config.enableHeuristics && rule.enableHeuristics) ? "Scanning..." : "Standby";
            if (rule.detectedThreadName != notDetectedName) {
                rule.detectedThreadName = notDetectedName;
                stateChanged = true;
            }
        }

        if (rule.currentThreadCount != totalThreadCount) {
            rule.currentThreadCount = totalThreadCount;
            stateChanged = true;
        }

        if (stateChanged) {
            char rLog[256];
            snprintf(rLog, sizeof(rLog), "Rule State Changed: Proc=%s, PID=%lu, AudioThread='%s', Threads=%d",
                     rule.processName.c_str(), rule.activePid, rule.detectedThreadName.c_str(), rule.currentThreadCount);
            LogDebug(rLog);
        }
    }

    m_heuristicsStatusText = heuristicsStatus;

    return stateChanged;
}

std::string ThreadIsolator::GetHeuristicsStatusText() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_heuristicsStatusText;
}

} // namespace ati
