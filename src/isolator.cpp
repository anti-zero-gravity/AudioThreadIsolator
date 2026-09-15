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

    // 最優先帯: Feeder, WASAPI, Playback Thread, ASIO (DAC 出力最前線)
    if (lower.find("feeder") != std::string::npos) return 100;
    if (lower.find("wasapi_render_thread") != std::string::npos) return 95;
    if (lower.find("playback") != std::string::npos && lower.find("decod") == std::string::npos) return 90;
    if (lower.find("ao/wasapi") != std::string::npos || lower == "ao") return 88;
    if (lower.find("wasapi") != std::string::npos) return 85;
    if (lower.find("asio") != std::string::npos) return 82;
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

static bool IsAllowedGameAudioModule(HANDLE hProcess, void* startAddr, const std::string& processName) {
    if (!startAddr) return false;
    if (!s_pfnGetMappedFileNameA) {
        HMODULE hPsapi = LoadLibraryA("psapi.dll");
        if (hPsapi) {
            s_pfnGetMappedFileNameA = reinterpret_cast<pfnGetMappedFileNameA>(
                GetProcAddress(hPsapi, "GetMappedFileNameA")
            );
        }
    }
    if (!s_pfnGetMappedFileNameA) return false;

    MEMORY_BASIC_INFORMATION mbi = { 0 };
    if (VirtualQueryEx(hProcess, startAddr, &mbi, sizeof(mbi))) {
        if (mbi.AllocationBase) {
            char modPath[MAX_PATH] = { 0 };
            if (s_pfnGetMappedFileNameA(hProcess, mbi.AllocationBase, modPath, sizeof(modPath)) > 0) {
                std::string p = ToLowerA(modPath);
                std::string pName = ToLowerA(processName);
                // ゲーム本体 exe または UnityPlayer.dll のみをホワイトリスト許可 (外部DLL・入力DLL・ドライバを除外)
                if (p.find(pName) != std::string::npos || p.find("unityplayer.dll") != std::string::npos) {
                    return true;
                }
            }
        }
    }
    return false;
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
}

void ThreadIsolator::Initialize(const GlobalConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_appliedThreads.clear();
    m_prevThreadCpuTimes.clear();
}

void ThreadIsolator::UpdateConfig(const GlobalConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_appliedThreads.clear();
    m_prevThreadCpuTimes.clear();
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
            rule.isAudioIsolated = false;
            rule.hasIntruderThreads = false;
            if (rule.isRunning) {
                rule.detectedThreadName = "Bypassed";
                if (rule.activePid != 0) {
                    m_trackedAudioThreads.erase(rule.activePid);
                    m_prevThreadCpuTimes.erase(rule.activePid);
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
                rule.isRunning = false;
                rule.activePid = 0;
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
            }
            continue;
        }

        int totalThreadCount = 0;
        bool audioDetectedInAny = false;
        std::string primaryAudioThreadName = "";
        DWORD primaryAudioPid = 0;

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
                if (threadName.empty()) {
                    threadName = QueryFmodOrUnityThreadName(hProcess, hThread);
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

            // 2. 未特定の場合：無名スレッド判定 (既存隔離の引き継ぎ / MMCSS リアルタイム帯 / 非MMCSS 適正ポーリングサンプリング)
            if (identifiedAudioTid == 0) {
                // (C-0) 既存隔離スレッドの引き継ぎ (Adopt)
                for (const auto& ti : threadInfos) {
                    if (ti.threadName.empty() && ti.priority == rule.audioPriority) {
                        DWORD_PTR currentAff = QueryThreadAffinityMask(ti.hThread);
                        if (currentAff == audioMask) {
                            void* sAddr = QueryThreadStartAddress(ti.hThread);
                            if (IsAllowedGameAudioModule(hProcess, sAddr, rule.processName)) {
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
            }

            if (identifiedAudioTid == 0) {
                std::vector<DWORD> rtCandidates;       // BasePri >= 15 (MMCSS / ASIO 帯)
                std::vector<DWORD> highestCandidates;  // BasePri == 10 または 降格済み (Unity / 非MMCSS 帯)

                for (const auto& ti : threadInfos) {
                    if (ti.threadName.empty()) {
                        void* sAddr = QueryThreadStartAddress(ti.hThread);
                        // ホワイトリスト判定: ゲーム本体 exe または UnityPlayer.dll のみ許可
                        if (!IsAllowedGameAudioModule(hProcess, sAddr, rule.processName)) {
                            continue;
                        }

                        if (ti.basePri >= 15) {
                            rtCandidates.push_back(ti.tid);
                        } else if (ti.basePri == 10 || (rule.audioPriority != 0 && ti.priority == rule.audioPriority)) {
                            highestCandidates.push_back(ti.tid);
                        }
                    }
                }

                // (C) MMCSS / リアルタイム帯: 候補が「ちょうど 1 本」の場合に特定
                if (rtCandidates.size() == 1) {
                    identifiedAudioTid = rtCandidates[0];
                    identifiedAudioLabel = "MMCSS (TID: " + std::to_string(identifiedAudioTid) + ")";
                    m_trackedAudioThreads[pid][identifiedAudioTid] = identifiedAudioLabel;
                }
                // (D) 非MMCSS (Unity 等): ヒューリスティック監視が ON かつ個別有効の場合
                else if (m_config.enableHeuristics && rule.enableHeuristics && rtCandidates.empty() && !highestCandidates.empty()) {
                    FILETIME cr, ex, kr, ur;
                    auto& cpuMap = m_prevThreadCpuTimes[pid];
                    DWORD bestTid = 0;
                    ULONGLONG maxDelta = 0;
                    ULONGLONG maxTotal = 0;
                    bool anyDeltaActive = false;

                    for (const auto& ti : threadInfos) {
                        if (std::find(highestCandidates.begin(), highestCandidates.end(), ti.tid) != highestCandidates.end()) {
                            if (GetThreadTimes(ti.hThread, &cr, &ex, &kr, &ur)) {
                                ULONGLONG totalTime = ((static_cast<ULONGLONG>(kr.dwHighDateTime) << 32) | kr.dwLowDateTime)
                                                    + ((static_cast<ULONGLONG>(ur.dwHighDateTime) << 32) | ur.dwLowDateTime);
                                auto prevIt = cpuMap.find(ti.tid);
                                if (prevIt != cpuMap.end()) {
                                    ULONGLONG delta = (totalTime >= prevIt->second) ? (totalTime - prevIt->second) : 0;

                                    if (delta > 0) {
                                        anyDeltaActive = true;
                                    }

                                    // 過大デルタ除外: 25ms (250,000 ticks) 超の爆裂スレッド (メインスレッドや描画等) は除外
                                    // 適正ポーリング帯: 10ms〜15.4ms オーダー (5,000 ticks 〜 250,000 ticks) の定常スレッドを選定
                                    if (delta >= 5000 && delta <= 250000) {
                                        // 適正範囲内で最も安定して CPU 累計時間・デルタを記録しているスレッドを選定
                                        if (totalTime > maxTotal || (totalTime == maxTotal && delta > maxDelta)) {
                                            maxDelta = delta;
                                            maxTotal = totalTime;
                                            bestTid = ti.tid;
                                        }
                                    }
                                }
                                cpuMap[ti.tid] = totalTime;
                            }
                        }
                    }

                    if (bestTid != 0) {
                        // Turn 3 (特定・隔離開始): 表示消去
                        identifiedAudioTid = bestTid;
                        identifiedAudioLabel = "Audio (TID: " + std::to_string(identifiedAudioTid) + ")";
                        m_trackedAudioThreads[pid][identifiedAudioTid] = identifiedAudioLabel;
                        char hLog[128];
                        snprintf(hLog, sizeof(hLog), "Heuristics MATCH (polling): PID=%lu, TID=%lu (delta=%llu us, total=%llu us)",
                                 pid, identifiedAudioTid, (unsigned long long)maxDelta, (unsigned long long)maxTotal);
                        LogDebug(hLog);
                    } else if (anyDeltaActive) {
                        // Turn 2: 候補グループ内から delta 数で特定中 (英語)
                        heuristicsStatus = "Heuristics: Identifying audio thread via delta...";
                    } else {
                        // Turn 1: オーディオ関連スレッドの稼働を待機中 (英語)
                        heuristicsStatus = "Heuristics: Waiting for audio thread activity...";
                    }
                }
            }

            // 3. アフィニティおよび優先度の適用
            bool foundIntruders = false;
            int lowerPrio = GetOneStepLowerPriority(rule.audioPriority);

            // 先行判定: オーディオ確定時、audioMask を持つ非オーディオスレッドの有無
            if (identifiedAudioTid != 0) {
                for (const auto& ti : threadInfos) {
                    if (ti.tid != identifiedAudioTid && (ti.currentAffinity & audioMask) != 0) {
                        foundIntruders = true;
                        break;
                    }
                }
            }
            rule.hasIntruderThreads = foundIntruders;

            for (const auto& ti : threadInfos) {
                bool isAudio = (ti.tid == identifiedAudioTid);
                std::string effectiveThreadName = !ti.threadName.empty() ? ti.threadName : (isAudio ? identifiedAudioLabel : "");
                // オーディオスレッド確定前であっても全スレッドをオーディオコアから通常コア群へ先行退避
                DWORD_PTR effectiveNormal = normalMask;
                DWORD_PTR targetMask = isAudio ? audioMask : effectiveNormal;

                if (isAudio) {
                    audioDetectedInAny = true;
                    if (primaryAudioThreadName.empty()) {
                        primaryAudioThreadName = effectiveThreadName;
                        primaryAudioPid = pid;
                    }
                    std::string effLower = ToLowerA(effectiveThreadName);
                    if (effLower.find("wasapi") != std::string::npos ||
                        effLower.find("playback") != std::string::npos ||
                        effLower.find("asio") != std::string::npos) {
                        primaryAudioThreadName = effectiveThreadName;
                        primaryAudioPid = pid;
                    }

                    char aLog[256];
                    snprintf(aLog, sizeof(aLog), "Detected Audio Thread: PID=%lu, TID=%lu, Name='%s', Pri=%d, BasePri=%ld, TargetCore=#%d",
                             pid, ti.tid, effectiveThreadName.c_str(), ti.priority, ti.basePri, rule.audioCore);
                    LogDebug(aLog);

                    // オーディオスレッド優先度設定 (明示的に指定された優先度を適用)
                    int targetPrio = rule.audioPriority;
                    if (ti.priority != targetPrio) {
                        SetThreadPriority(ti.hThread, targetPrio);
                    }

                    // オーディオスレッドの Ideal Processor を指定コアに固定
                    SetThreadIdealProcessor(ti.hThread, static_cast<DWORD>(rule.audioCore));
                } else {
                    // 通常スレッドの Ideal Processor をオーディオコア以外の通常コアに明示退避
                    DWORD normalIdeal = 0;
                    for (int c = 0; c < coreCount; ++c) {
                        if ((normalMask & (1ULL << c)) != 0) {
                            normalIdeal = static_cast<DWORD>(c);
                            if ((ti.tid % coreCount) <= static_cast<DWORD>(c)) {
                                break;
                            }
                        }
                    }
                    SetThreadIdealProcessor(ti.hThread, normalIdeal);

                    // オーディオ専有コアへの侵入・同居スレッドの優先度連動降格 (オーディオ優先度の直下階層へ設定)
                    if (identifiedAudioTid != 0 && (ti.currentAffinity & audioMask) != 0) {
                        foundIntruders = true;
                        rule.hasIntruderThreads = true;
                        if (ti.priority != lowerPrio) {
                            SetThreadPriority(ti.hThread, lowerPrio);
                            char iLog[128];
                            snprintf(iLog, sizeof(iLog), "Adjusted intruder thread priority to %d (audio was %d): PID=%lu, TID=%lu",
                                     lowerPrio, rule.audioPriority, pid, ti.tid);
                            LogDebug(iLog);
                        }
                    }
                }

                // アフィニティ適用
                DWORD_PTR prevMask = SetThreadAffinityMask(ti.hThread, targetMask);
                if (prevMask != 0 && prevMask != targetMask) {
                    m_appliedThreads[ti.tid] = targetMask;
                    if (isAudio) {
                        rule.applyCount++; // オーディオスレッドの隔離変更のみカウント
                    } else if (identifiedAudioTid != 0 && (prevMask & audioMask) != 0) {
                        // アフィニティ変更前が audioMask だった場合も侵入者として連動優先度を適用
                        foundIntruders = true;
                        rule.hasIntruderThreads = true;
                        if (ti.priority != lowerPrio) {
                            SetThreadPriority(ti.hThread, lowerPrio);
                        }
                    }
                    stateChanged = true;
                }

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
            if (rule.detectedThreadName != primaryAudioThreadName) {
                rule.detectedThreadName = primaryAudioThreadName;
                stateChanged = true;
            }
        } else {
            if (rule.isAudioIsolated) {
                rule.isAudioIsolated = false;
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
