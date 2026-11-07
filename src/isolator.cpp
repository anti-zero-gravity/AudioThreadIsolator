#undef UNICODE
#undef _UNICODE

#include "isolator.h"
#include "priority_matrix_data.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <regex>
#include <set>
#include <unordered_map>

void LogDebug(const char *msg);

namespace ati {

static std::string ToLowerA(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return str;
}

static ULONG64 CalculateDeltaThreshold(double ruleDelta, double globalDelta,
                                       int intervalMs) {
  double d = (ruleDelta > 0.0) ? ruleDelta : globalDelta;
  if (d <= 0.0)
    d = 3.0;
  d = std::ceil(d * 10.0) / 10.0;
  ULONG64 base = static_cast<ULONG64>(d * 1000000.0);
  if (intervalMs <= 0)
    return base;
  return (base * static_cast<ULONG64>(intervalMs)) / 1000ULL;
}

// 候補 6+: Dead-PID クリーンアップ共通テンプレート
template <typename MapType>
static void EraseDeadPidKeys(MapType &map,
                             const std::unordered_set<DWORD> &alivePids) {
  for (auto it = map.begin(); it != map.end();) {
    if (alivePids.find(it->first) == alivePids.end()) {
      it = map.erase(it);
    } else {
      ++it;
    }
  }
}

template <typename SetType>
static void EraseDeadPidSet(SetType &set,
                            const std::unordered_set<DWORD> &alivePids) {
  for (auto it = set.begin(); it != set.end();) {
    if (alivePids.find(*it) == alivePids.end()) {
      it = set.erase(it);
    } else {
      ++it;
    }
  }
}

static void ApplyProcessHighQoS(HANDLE hProcess) {
  struct PROCESS_POWER_THROTTLING_STATE_LOCAL {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
  };
  static auto pfnSetProcInfo =
      reinterpret_cast<BOOL(WINAPI *)(HANDLE, int, PVOID, DWORD)>(
          GetProcAddress(GetModuleHandleA("kernel32.dll"),
                         "SetProcessInformation"));
  if (pfnSetProcInfo && hProcess) {
    PROCESS_POWER_THROTTLING_STATE_LOCAL state = {};
    state.Version = 1;
    state.ControlMask =
        1 | 4;           // PROCESS_POWER_THROTTLING_EXECUTION_SPEED (1) |
                         // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION (4)
    state.StateMask = 0; // 0 = HighQoS (EcoQoS OFF)
    pfnSetProcInfo(hProcess, 4, &state, sizeof(state));
  }
}

static bool SetProcessAffinityByPid(DWORD pid, DWORD_PTR mask) {
  if (pid <= 4 || mask == 0)
    return false;
  HANDLE hProcess = OpenProcess(
      PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!hProcess) {
    hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
  }
  if (!hProcess)
    return false;

  DWORD_PTR curMask = 0, sysMask = 0;
  if (GetProcessAffinityMask(hProcess, &curMask, &sysMask)) {
    if (curMask == mask) {
      // 既に目的のマスクと完全一致しているため、IPI 割り込みを伴う再設定をスキップ
      CloseHandle(hProcess);
      return false;
    }
  }

  BOOL ok = SetProcessAffinityMask(hProcess, mask);
  CloseHandle(hProcess);
  return ok != FALSE;
}

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI *pfnNtQueryInformationThread)(
    HANDLE ThreadHandle, ULONG ThreadInformationClass, PVOID ThreadInformation,
    ULONG ThreadInformationLength, PULONG ReturnLength);
static pfnNtQueryInformationThread s_pfnNtQueryInformationThread = nullptr;

typedef NTSTATUS(NTAPI *pfnNtSetInformationThread)(
    HANDLE ThreadHandle, ULONG ThreadInformationClass, PVOID ThreadInformation,
    ULONG ThreadInformationLength);
static pfnNtSetInformationThread s_pfnNtSetInformationThread = nullptr;

typedef NTSTATUS(NTAPI *pfnNtGetNextThread)(HANDLE ProcessHandle,
                                            HANDLE ThreadHandle,
                                            ACCESS_MASK DesiredAccess,
                                            ULONG HandleAttributes, ULONG Flags,
                                            PHANDLE NewThreadHandle);
static pfnNtGetNextThread s_pfnNtGetNextThread = nullptr;

typedef NTSTATUS(NTAPI *pfnNtQuerySystemInformation)(
    ULONG SystemInformationClass, PVOID SystemInformation,
    ULONG SystemInformationLength, PULONG ReturnLength);
static pfnNtQuerySystemInformation s_pfnNtQuerySystemInformation = nullptr;

typedef LONG(NTAPI *pfnNtSuspendProcess)(HANDLE ProcessHandle);
typedef LONG(NTAPI *pfnNtResumeProcess)(HANDLE ProcessHandle);
static pfnNtSuspendProcess s_pfnNtSuspendProcess = nullptr;
static pfnNtResumeProcess s_pfnNtResumeProcess = nullptr;

static LONG NtSuspendProcessHelper(HANDLE hProcess) {
  if (!s_pfnNtSuspendProcess) {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll)
      s_pfnNtSuspendProcess =
          (pfnNtSuspendProcess)GetProcAddress(hNtdll, "NtSuspendProcess");
  }
  return s_pfnNtSuspendProcess ? s_pfnNtSuspendProcess(hProcess) : -1;
}

static LONG NtResumeProcessHelper(HANDLE hProcess) {
  if (!s_pfnNtResumeProcess) {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll)
      s_pfnNtResumeProcess =
          (pfnNtResumeProcess)GetProcAddress(hNtdll, "NtResumeProcess");
  }
  return s_pfnNtResumeProcess ? s_pfnNtResumeProcess(hProcess) : -1;
}

typedef struct _UNICODE_STRING_RAW {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR Buffer;
} UNICODE_STRING_RAW;

typedef struct _CLIENT_ID_RAW {
  HANDLE UniqueProcess;
  HANDLE UniqueThread;
} CLIENT_ID_RAW;

typedef struct _SYSTEM_THREAD_INFORMATION_RAW {
  LARGE_INTEGER KernelTime;
  LARGE_INTEGER UserTime;
  LARGE_INTEGER CreateTime;
  ULONG WaitTime;
  PVOID StartAddress;
  CLIENT_ID_RAW ClientId;
  LONG Priority;
  LONG BasePriority;
  ULONG ContextSwitches;
  ULONG ThreadState; // 2: Running, 5: Waiting
  ULONG WaitReason;  // 4: DelayExecution, 6: UserRequest
} SYSTEM_THREAD_INFORMATION_RAW;

typedef struct _SYSTEM_PROCESS_INFORMATION_RAW {
  ULONG NextEntryOffset;
  ULONG NumberOfThreads;
  LARGE_INTEGER WorkingSetPrivateSize;
  ULONG HardFaultCount;
  ULONG NumberOfThreadsHighWatermark;
  ULONGLONG CycleTime;
  LARGE_INTEGER CreateTime;
  LARGE_INTEGER UserTime;
  LARGE_INTEGER KernelTime;
  UNICODE_STRING_RAW ImageName;
  LONG BasePriority;
  HANDLE UniqueProcessId;
  HANDLE InheritedFromUniqueProcessId;
  ULONG HandleCount;
  ULONG SessionId;
  ULONG_PTR UniqueProcessKey;
  ULONG_PTR PeakVirtualSize;
  ULONG_PTR VirtualSize;
  ULONG PageFaultCount;
  ULONG_PTR PeakWorkingSetSize;
  ULONG_PTR WorkingSetSize;
  ULONG_PTR QuotaPeakPagedPoolUsage;
  ULONG_PTR QuotaPagedPoolUsage;
  ULONG_PTR QuotaPeakNonPagedPoolUsage;
  ULONG_PTR QuotaNonPagedPoolUsage;
  ULONG_PTR PagefileUsage;
  ULONG_PTR PeakPagefileUsage;
  ULONG_PTR PrivatePageCount;
  LARGE_INTEGER ReadOperationCount;
  LARGE_INTEGER WriteOperationCount;
  LARGE_INTEGER OtherOperationCount;
  LARGE_INTEGER ReadTransferCount;
  LARGE_INTEGER WriteTransferCount;
  LARGE_INTEGER OtherTransferCount;
  SYSTEM_THREAD_INFORMATION_RAW Threads[1];
} SYSTEM_PROCESS_INFORMATION_RAW;

static void EnsureNtLoaded() {
  HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
  if (hNtdll) {
    if (!s_pfnNtQueryInformationThread) {
      s_pfnNtQueryInformationThread =
          reinterpret_cast<pfnNtQueryInformationThread>(
              GetProcAddress(hNtdll, "NtQueryInformationThread"));
    }
    if (!s_pfnNtSetInformationThread) {
      s_pfnNtSetInformationThread =
          reinterpret_cast<pfnNtSetInformationThread>(
              GetProcAddress(hNtdll, "NtSetInformationThread"));
    }
    if (!s_pfnNtGetNextThread) {
      s_pfnNtGetNextThread = reinterpret_cast<pfnNtGetNextThread>(
          GetProcAddress(hNtdll, "NtGetNextThread"));
    }
    if (!s_pfnNtQuerySystemInformation) {
      s_pfnNtQuerySystemInformation =
          reinterpret_cast<pfnNtQuerySystemInformation>(
              GetProcAddress(hNtdll, "NtQuerySystemInformation"));
    }
  }
}

template <typename Fn>
static void WalkSystemProcessThreads(DWORD targetPid, Fn &&onThread) {
  EnsureNtLoaded();
  if (!s_pfnNtQuerySystemInformation)
    return;

  ULONG bufferSize = 512 * 1024;
  std::vector<BYTE> buffer(bufferSize);
  ULONG returnLength = 0;

  NTSTATUS status =
      s_pfnNtQuerySystemInformation(5 /* SystemProcessInformation */,
                                    buffer.data(), bufferSize, &returnLength);
  while (status == 0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */) {
    bufferSize = returnLength + (64 * 1024);
    buffer.resize(bufferSize);
    status = s_pfnNtQuerySystemInformation(5, buffer.data(), bufferSize,
                                           &returnLength);
  }
  if (status != 0)
    return;

  BYTE *pCurrent = buffer.data();
  while (pCurrent) {
    auto *pProc = reinterpret_cast<SYSTEM_PROCESS_INFORMATION_RAW *>(pCurrent);
    DWORD pid =
        static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(pProc->UniqueProcessId));
    if (pid == targetPid) {
      for (ULONG i = 0; i < pProc->NumberOfThreads; ++i) {
        const auto &th = pProc->Threads[i];
        DWORD tid = static_cast<DWORD>(
            reinterpret_cast<ULONG_PTR>(th.ClientId.UniqueThread));
        onThread(tid, th);
      }
      break;
    }
    if (pProc->NextEntryOffset == 0)
      break;
    pCurrent += pProc->NextEntryOffset;
  }
}

static std::unordered_map<DWORD, ULONG>
QueryProcessThreadsWaitReason(DWORD targetPid) {
  std::unordered_map<DWORD, ULONG> result;
  WalkSystemProcessThreads(
      targetPid, [&](DWORD tid, const SYSTEM_THREAD_INFORMATION_RAW &th) {
        result[tid] = th.WaitReason;
      });
  return result;
}

static std::unordered_map<DWORD, ULONG> QueryProcessThreadsContextSwitches(
    DWORD targetPid, std::vector<THREADENTRY32> *outEntries = nullptr) {
  std::unordered_map<DWORD, ULONG> result;
  WalkSystemProcessThreads(
      targetPid, [&](DWORD tid, const SYSTEM_THREAD_INFORMATION_RAW &th) {
        result[tid] = th.ContextSwitches;
        if (outEntries && tid != 0) {
          THREADENTRY32 te = {0};
          te.dwSize = sizeof(te);
          te.th32OwnerProcessID = targetPid;
          te.th32ThreadID = tid;
          te.tpBasePri = th.BasePriority;
          outEntries->push_back(te);
        }
      });
  return result;
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
  EnsureNtLoaded();
  if (!s_pfnNtQueryInformationThread)
    return 0;

  THREAD_BASIC_INFO_RAW tbi = {0};
  ULONG retLen = 0;
  NTSTATUS status = s_pfnNtQueryInformationThread(
      hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
  if (status == 0) {
    return tbi.AffinityMask;
  }
  return 0;
}

static int QueryThreadDynamicPriority(HANDLE hThread) {
  EnsureNtLoaded();
  if (!s_pfnNtQueryInformationThread)
    return 0;

  THREAD_BASIC_INFO_RAW tbi = {0};
  ULONG retLen = 0;
  NTSTATUS status = s_pfnNtQueryInformationThread(
      hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
  if (status == 0 && tbi.Priority > 0) {
    return static_cast<int>(tbi.Priority);
  }
  return 0;
}

static bool ApplyAbsoluteThreadPriority(HANDLE hThread, int absPrio) {
  EnsureNtLoaded();
  if (!s_pfnNtSetInformationThread || absPrio < 1 || absPrio > 31)
    return false;
  LONG kprio = static_cast<LONG>(absPrio);
  if (absPrio < 16) {
    s_pfnNtSetInformationThread(hThread, 25 /* ThreadActualBasePriority */,
                                &kprio, sizeof(kprio));
  }
  NTSTATUS st = s_pfnNtSetInformationThread(
      hThread, 2 /* ThreadPriority */, &kprio, sizeof(kprio));
  return (st == 0);
}

static PVOID QueryThreadTebBaseAddress(HANDLE hThread) {
  EnsureNtLoaded();
  if (!s_pfnNtQueryInformationThread)
    return nullptr;

  THREAD_BASIC_INFO_RAW tbi = {0};
  ULONG retLen = 0;
  NTSTATUS status = s_pfnNtQueryInformationThread(
      hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
  if (status == 0) {
    return tbi.TebBaseAddress;
  }
  return nullptr;
}

typedef DWORD(WINAPI *pfnGetMappedFileNameA)(HANDLE, LPVOID, LPSTR, DWORD);
static pfnGetMappedFileNameA s_pfnGetMappedFileNameA = nullptr;

static void EnsurePsapiLoaded() {
  if (!s_pfnGetMappedFileNameA) {
    HMODULE hPsapi = LoadLibraryA("psapi.dll");
    if (hPsapi) {
      s_pfnGetMappedFileNameA = reinterpret_cast<pfnGetMappedFileNameA>(
          GetProcAddress(hPsapi, "GetMappedFileNameA"));
    }
  }
}

static std::unordered_set<std::string> s_registeredAsioDlls;
static bool s_asioDriversLoaded = false;

static void EnsureAsioDriversLoaded() {
  if (s_asioDriversLoaded)
    return;
  s_asioDriversLoaded = true;
  const LPCSTR subkeys[] = {"SOFTWARE\\ASIO", "SOFTWARE\\WOW6432Node\\ASIO"};
  for (const auto &subKeyPath : subkeys) {
    HKEY hAsio = NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subKeyPath, 0, KEY_READ, &hAsio) ==
        ERROR_SUCCESS) {
      DWORD index = 0;
      char childName[256];
      DWORD childLen = sizeof(childName);
      while (RegEnumKeyExA(hAsio, index++, childName, &childLen, NULL, NULL,
                           NULL, NULL) == ERROR_SUCCESS) {
        childLen = sizeof(childName);
        HKEY hDriver = NULL;
        if (RegOpenKeyExA(hAsio, childName, 0, KEY_READ, &hDriver) ==
            ERROR_SUCCESS) {
          char clsidStr[128] = {0};
          DWORD clsidLen = sizeof(clsidStr);
          if (RegQueryValueExA(hDriver, "CLSID", NULL, NULL, (LPBYTE)clsidStr,
                               &clsidLen) == ERROR_SUCCESS) {
            std::string clsidPaths[] = {
                std::string("SOFTWARE\\Classes\\CLSID\\") + clsidStr +
                    "\\InprocServer32",
                std::string("SOFTWARE\\Classes\\WOW6432Node\\CLSID\\") +
                    clsidStr + "\\InprocServer32",
                std::string("SOFTWARE\\WOW6432Node\\Classes\\CLSID\\") +
                    clsidStr + "\\InprocServer32"};
            for (const auto &cPath : clsidPaths) {
              HKEY hInproc = NULL;
              if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, cPath.c_str(), 0, KEY_READ,
                                &hInproc) == ERROR_SUCCESS) {
                char dllPath[MAX_PATH] = {0};
                DWORD dllLen = sizeof(dllPath);
                if (RegQueryValueExA(hInproc, NULL, NULL, NULL, (LPBYTE)dllPath,
                                     &dllLen) == ERROR_SUCCESS) {
                  std::string lowerPath = ToLowerA(dllPath);
                  size_t slash = lowerPath.find_last_of("\\/");
                  std::string fileName = (slash != std::string::npos)
                                             ? lowerPath.substr(slash + 1)
                                             : lowerPath;
                  if (!fileName.empty()) {
                    s_registeredAsioDlls.insert(fileName);
                  }
                }
                CloseHandle(hInproc);
              }
            }
          }
          CloseHandle(hDriver);
        }
      }
      CloseHandle(hAsio);
    }
  }
}

static bool IsRegisteredAsioDll(const std::string &modPath) {
  EnsureAsioDriversLoaded();
  if (modPath.find("asio") != std::string::npos)
    return true;
  size_t slash = modPath.find_last_of("\\/");
  std::string fileName =
      (slash != std::string::npos) ? modPath.substr(slash + 1) : modPath;
  return s_registeredAsioDlls.find(fileName) != s_registeredAsioDlls.end();
}

// コールスタック上のモジュールおよび参照シグネチャ走査 (WASAPI / ASIO /
// DirectSound / Godot / Roon 等)
static bool CollectThreadStackPointers(HANDLE hProcess, HANDLE hThread,
                                       std::vector<DWORD_PTR> &ptrs) {
  ptrs.clear();
  EnsureNtLoaded();
  if (!s_pfnNtQueryInformationThread)
    return false;

  EnsurePsapiLoaded();

  THREAD_BASIC_INFO_RAW tbi = {0};
  ULONG retLen = 0;
  NTSTATUS status = s_pfnNtQueryInformationThread(
      hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
  if (status != 0 || !tbi.TebBaseAddress) {
    return false;
  }

  BOOL isWow64 = FALSE;
  IsWow64Process(hProcess, &isWow64);

  DWORD_PTR stackBase = 0;
  DWORD_PTR stackLimit = 0;
  SIZE_T bytesRead = 0;

  if (isWow64) {
    // WOW64 (32bit on 64bit Windows): TEB32 is TEB64 + 0x2000, StackBase is
    // TEB32 + 4
    uintptr_t teb32 = reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 0x2000;
    uint32_t base32 = 0;
    uint32_t limit32 = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(teb32 + 4),
                          &base32, sizeof(base32), &bytesRead) &&
        bytesRead == sizeof(base32)) {
      stackBase = base32;
    }
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(teb32 + 8),
                          &limit32, sizeof(limit32), &bytesRead) &&
        bytesRead == sizeof(limit32)) {
      stackLimit = limit32;
    }
  } else {
    // 64bit Windows: TEB + 0x08 is StackBase, TEB + 0x10 is StackLimit
    if (ReadProcessMemory(
            hProcess,
            reinterpret_cast<LPCVOID>(
                reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 8),
            &stackBase, sizeof(stackBase), &bytesRead) &&
        bytesRead == sizeof(stackBase)) {
      ReadProcessMemory(
          hProcess,
          reinterpret_cast<LPCVOID>(
              reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 16),
          &stackLimit, sizeof(stackLimit), &bytesRead);
    }
  }

  if (stackBase < 0x1000)
    return false;

  // スタック領域を走査 (直近のアクティブなフレーム群を含む最大 32KB を走査)
  DWORD scanSize = 32768;
  if (stackLimit > 0 && stackBase > stackLimit) {
    DWORD_PTR actualStackSpan = stackBase - stackLimit;
    if (actualStackSpan < scanSize) {
      scanSize = static_cast<DWORD>(actualStackSpan);
    }
  }
  if (scanSize < 4096)
    scanSize = 4096;

  std::vector<BYTE> stackBuf(scanSize, 0);
  LPCVOID scanAddr = reinterpret_cast<LPCVOID>(stackBase - scanSize);
  if (!ReadProcessMemory(hProcess, scanAddr, stackBuf.data(), scanSize,
                         &bytesRead) ||
      bytesRead < 64) {
    return false;
  }

  if (isWow64) {
    size_t count32 = bytesRead / sizeof(uint32_t);
    const uint32_t *p32 = reinterpret_cast<const uint32_t *>(stackBuf.data());
    for (size_t i = 0; i < count32; ++i) {
      if (p32[i] >= 0x10000 && p32[i] <= 0xFFFFFFFF) {
        ptrs.push_back(p32[i]);
      }
    }
  } else {
    size_t count64 = bytesRead / sizeof(DWORD_PTR);
    const DWORD_PTR *p64 = reinterpret_cast<const DWORD_PTR *>(stackBuf.data());
    for (size_t i = 0; i < count64; ++i) {
      if (p64[i] >= 0x10000 && p64[i] <= 0x7FFFFFFFFFFFULL) {
        ptrs.push_back(p64[i]);
      }
    }
  }
  return !ptrs.empty();
}

static std::string QueryCallstackAudioSignature(HANDLE hProcess, HANDLE hThread,
                                                int appType = 1) {
  std::vector<DWORD_PTR> ptrs;
  if (!CollectThreadStackPointers(hProcess, hThread, ptrs)) {
    return "";
  }

  // モジュール名解決キャッシュ (同一 AllocationBase に対する API
  // 呼び出しの重複を防ぐ)
  std::unordered_map<DWORD_PTR, std::string> modNameCache;
  bool hasDiscordVoice = false;
  bool hasAvrt = false;
  bool hasRoonSocket = false;
  bool hasVmWs2 = false;
  bool hasVmGui = false;

  for (size_t i = 0; i < ptrs.size(); ++i) {
    DWORD_PTR p = ptrs[i];

    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQueryEx(hProcess, reinterpret_cast<LPCVOID>(p), &mbi,
                       sizeof(mbi))) {
      if (mbi.Protect == PAGE_EXECUTE_READ ||
          mbi.Protect == PAGE_EXECUTE_READWRITE) {
        DWORD_PTR allocBase = reinterpret_cast<DWORD_PTR>(mbi.AllocationBase);
        if (allocBase != 0) {
          std::string modName;
          auto cIt = modNameCache.find(allocBase);
          if (cIt != modNameCache.end()) {
            modName = cIt->second;
          } else {
            if (s_pfnGetMappedFileNameA) {
              char mPath[MAX_PATH] = {0};
              if (s_pfnGetMappedFileNameA(hProcess, mbi.AllocationBase, mPath,
                                          sizeof(mPath)) > 0) {
                modName = ToLowerA(mPath);
              }
            }
            modNameCache[allocBase] = modName;
          }

          if (!modName.empty()) {
            // (0) Voicemeeter 専用シグネチャ (AppType: 5 限定)
            if (appType == 5) {
              if (modName.find("ws2_32") != std::string::npos) {
                hasVmWs2 = true;
              }
              if (modName.find("user32") != std::string::npos ||
                  modName.find("gdi32") != std::string::npos ||
                  modName.find("gdiplus") != std::string::npos ||
                  modName.find("win32u") != std::string::npos) {
                hasVmGui = true;
              }
              continue;
            }
            // (1) InstantAud.ax (CyberLink PowerDVD オーディオ出力)
            if (modName.find("instantaud") != std::string::npos) {
              return "InstantAud.ax (CyberLink)";
            }
            // (2) AUDIOSES.DLL (WASAPI 最前線)
            if (modName.find("audioses") != std::string::npos) {
              return "WASAPI (audioses.dll)";
            }
            // (3) ASIO ドライバ DLL (汎用 asio または レジストリ登録 DLL)
            if (IsRegisteredAsioDll(modName)) {
              return "ASIO Driver";
            }
            // (4) DirectSound
            if (modName.find("dsound") != std::string::npos) {
              return "DirectSound (dsound.dll)";
            }
            // (5) XAudio2
            if (modName.find("xaudio2") != std::string::npos) {
              return "XAudio2";
            }
            // (6) pxtone (洞窟物語・オルガーニャ等の音源エンジン)
            if (modName.find("pxtone") != std::string::npos) {
              return "pxtone (Organya)";
            }
            // (7) Discord Voice (discord_voice.node + avrt.dll MMCSS)
            if (modName.find("discord_voice") != std::string::npos) {
              hasDiscordVoice = true;
              if (hasAvrt) {
                return "Discord Voice";
              }
            }
            if (modName.find("avrt") != std::string::npos) {
              hasAvrt = true;
              if (hasDiscordVoice) {
                return "Discord Voice";
              }
            }
            // (8)-(10) Roon 専用シグネチャ (AppType: 4
            // 限定、一般アプリでは非使用)
            if (appType == 4) {
              if (modName.find("roonmedia") != std::string::npos) {
                return "Roon Media (roonmedia.dll)";
              }
              if (modName.find("raatmanager") != std::string::npos) {
                return "Roon RAAT (raatmanager.dll)";
              }
              if (modName.find("mswsock") != std::string::npos ||
                  modName.find("ws2_32") != std::string::npos) {
                hasRoonSocket = true;
              }
            }
          }
        }
      }
    }
  }
  if (appType == 4 && hasRoonSocket) {
    return "Roon Socket (mswsock.dll)";
  }
  if (appType == 5) {
    std::string vmSig;
    if (hasVmWs2)
      vmSig += "[WS2]";
    if (hasVmGui)
      vmSig += "[GUI]";
    return vmSig;
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

// Chromium 判定: プロセスの exe ファイルをディスクから読み、"chromeos" ASCII
// 文字列を検索
bool ThreadIsolator::DetectChromiumExe(HANDLE hProcess) {
  char exePath[MAX_PATH] = {0};
  DWORD pathLen = MAX_PATH;
  if (!QueryFullProcessImageNameA(hProcess, 0, exePath, &pathLen) ||
      pathLen == 0) {
    return false;
  }

  FILE *fp = fopen(exePath, "rb");
  if (!fp)
    return false;

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
  if (readBytes < 8)
    return false;

  const char *needle = "chromeos";
  size_t needleLen = 8;
  for (size_t i = 0; i + needleLen <= readBytes; ++i) {
    if (memcmp(&buf[i], needle, needleLen) == 0) {
      char dLog[256];
      snprintf(dLog, sizeof(dLog),
               "DetectChromiumExe: FOUND 'chromeos' in '%s'", exePath);
      LogDebug(dLog);
      return true;
    }
  }
  return false;
}

// プロセスのコマンドラインを PEB 経由で取得 (ANSI 変換)
std::string ThreadIsolator::QueryProcessCommandLine(HANDLE hProcess) {
  typedef LONG NTSTATUS;
  typedef NTSTATUS(NTAPI * pfnNtQueryInformationProcess)(HANDLE, ULONG, PVOID,
                                                         ULONG, PULONG);

  static pfnNtQueryInformationProcess s_pfnNtQIP = nullptr;
  if (!s_pfnNtQIP) {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll) {
      s_pfnNtQIP = reinterpret_cast<pfnNtQueryInformationProcess>(
          GetProcAddress(hNtdll, "NtQueryInformationProcess"));
    }
  }
  if (!s_pfnNtQIP)
    return "";

  struct PROCESS_BASIC_INFO {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
  };
  PROCESS_BASIC_INFO pbi = {0};
  ULONG retLen = 0;
  NTSTATUS status = s_pfnNtQIP(hProcess, 0, &pbi, sizeof(pbi), &retLen);
  if (status != 0 || !pbi.PebBaseAddress)
    return "";

  PVOID pProcessParams = nullptr;
  SIZE_T bytesRead = 0;
  if (!ReadProcessMemory(
          hProcess,
          reinterpret_cast<LPCVOID>(
              reinterpret_cast<uintptr_t>(pbi.PebBaseAddress) + 0x20),
          &pProcessParams, sizeof(pProcessParams), &bytesRead) ||
      !pProcessParams) {
    return "";
  }

  struct UNICODE_STRING_RAW {
    USHORT Length;
    USHORT MaximumLength;
    DWORD padding;
    PVOID Buffer;
  };
  UNICODE_STRING_RAW cmdLineUs = {0};
  if (!ReadProcessMemory(
          hProcess,
          reinterpret_cast<LPCVOID>(
              reinterpret_cast<uintptr_t>(pProcessParams) + 0x70),
          &cmdLineUs, sizeof(cmdLineUs), &bytesRead) ||
      !cmdLineUs.Buffer || cmdLineUs.Length == 0) {
    return "";
  }

  USHORT wcharCount = cmdLineUs.Length / sizeof(WCHAR);
  if (wcharCount > 16384)
    wcharCount = 16384;
  std::vector<WCHAR> wBuf(wcharCount + 1, 0);
  if (!ReadProcessMemory(hProcess, cmdLineUs.Buffer, wBuf.data(),
                         static_cast<SIZE_T>(wcharCount) * sizeof(WCHAR),
                         &bytesRead)) {
    return "";
  }
  wBuf[wcharCount] = L'\0';

  int aBufLen = WideCharToMultiByte(CP_ACP, 0, wBuf.data(), wcharCount, nullptr,
                                    0, nullptr, nullptr);
  if (aBufLen <= 0)
    return "";
  std::string result(static_cast<size_t>(aBufLen), '\0');
  WideCharToMultiByte(CP_ACP, 0, wBuf.data(), wcharCount, &result[0], aBufLen,
                      nullptr, nullptr);
  return result;
}

ThreadIsolator::ThreadIsolator() {}

ThreadIsolator::~ThreadIsolator() { ResumeAllSuspendedThreads(); }

void ThreadIsolator::Initialize(const GlobalConfig &config) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config = config;
  m_appliedThreads.clear();
  m_appliedIdealCores.clear();
  m_prevThreadCpuTimes.clear();
  m_samplingStates.clear();
  m_chromiumThreadTracks.clear();
  m_chromiumAudioStates.clear();
  m_chromiumEvictedPids.clear();
  m_nonChromiumMaskedPids.clear();
  m_highQosAppliedPids.clear();
  m_audioDgAppliedTids.clear();
  m_audioDgLastPrio.clear();
  m_cachedProcessTids.clear();
  m_lastProcessThreadCount.clear();
}

void ThreadIsolator::UpdateConfig(const GlobalConfig &config) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config = config;
  for (auto &r : m_config.rules) {
    WakeUpSteadyRule(r, 2);
  }
  m_appliedThreads.clear();
  m_appliedIdealCores.clear();
  m_prevThreadCpuTimes.clear();
  m_samplingStates.clear();
  m_nonChromiumMaskedPids.clear();
  m_cachedProcessTids.clear();
  m_lastProcessThreadCount.clear();
  // 注意: 設定更新（優先度変更やコア変更等）時に Chromium
  // の追従情報や確定状態を破棄してはならない
  // 破棄すると定常確定スレッドが見失われ、長時間のサンプリングに逆戻りしてしまうため維持する
}

GlobalConfig ThreadIsolator::GetConfig() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_config;
}

void ThreadIsolator::AddRule(const ProcessRule &rule) {
  std::lock_guard<std::mutex> lock(m_mutex);
  ProcessRule norm = rule;
  NormalizeSpecialRule(norm);
  WakeUpSteadyRule(norm, 2);
  m_config.rules.push_back(norm);
  if (m_config.unlistedMaskMode != 0 || m_hasUnlistedMaskInOs) {
    RestoreAffinityForProcessNamesLocked({ToLowerA(norm.processName)});
  }
  m_appliedThreads.clear();
  m_appliedIdealCores.clear();
  m_cachedProcessTids.clear();
  m_lastProcessThreadCount.clear();
}

void ThreadIsolator::RemoveRule(size_t index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index < m_config.rules.size()) {
    auto &rule = m_config.rules[index];
    ResumeAudioThreadIfSuspended(rule);
    if (IsAudioDgProcess(rule.processName)) {
      RestoreAudioDgProcessLocked(rule.activePid);
    }
    if (rule.activePid != 0) {
      m_cachedProcessTids.erase(rule.activePid);
      m_lastProcessThreadCount.erase(rule.activePid);
      m_highQosAppliedPids.erase(rule.activePid);
    }
    if (rule.audioServicePid != 0) {
      m_highQosAppliedPids.erase(rule.audioServicePid);
    }
    m_config.rules.erase(m_config.rules.begin() + index);
  }
}

void ThreadIsolator::UpdateRule(size_t index, const ProcessRule &rule) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index < m_config.rules.size()) {
    ProcessRule oldRule = m_config.rules[index];
    ProcessRule norm = rule;
    NormalizeSpecialRule(norm);

    DWORD targetPid =
        (norm.audioServicePid != 0) ? norm.audioServicePid : norm.activePid;

    bool affinityConfigChanged =
        (oldRule.audioCore != norm.audioCore ||
         oldRule.audioAffinityMask != norm.audioAffinityMask ||
         oldRule.normalAffinityMask != norm.normalAffinityMask ||
         oldRule.audioCores != norm.audioCores ||
         oldRule.audioSlotMasks != norm.audioSlotMasks ||
         oldRule.isBypassed != norm.isBypassed ||
         oldRule.slotBypassed != norm.slotBypassed);

    bool procPrioChanged =
        norm.processPriorityModified &&
        (!oldRule.processPriorityModified ||
         oldRule.processPriorityClass != norm.processPriorityClass);

    if (procPrioChanged) {
      if (targetPid != 0) {
        HANDLE hProc = OpenProcess(
            PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
            targetPid);
        if (hProc) {
          bool setOk = (SetPriorityClass(hProc, norm.processPriorityClass) != 0);
          if (!setOk && norm.processPriorityClass == REALTIME_PRIORITY_CLASS) {
            setOk = (SetPriorityClass(hProc, HIGH_PRIORITY_CLASS) != 0);
          }
          DWORD actualCls = GetPriorityClass(hProc);
          if (actualCls != 0) {
            norm.actualProcessPriorityClass = actualCls;
          }
          CloseHandle(hProc);

          // Col 3 変更直後: Fixed 指定スロットには無条件で NtSetInformationThread を再適用
          if (IsAudioDgProcess(norm.processName)) {
            int p0 = (!norm.audioPriorities.empty()) ? norm.audioPriorities[0]
                                                     : norm.audioPriority;
            if (IsFixedThreadPriority(p0)) {
              int absPrio = DecodeFixedThreadPriority(p0);
              auto itDg = m_audioDgAppliedTids.find(targetPid);
              if (itDg != m_audioDgAppliedTids.end()) {
                for (DWORD tid : itDg->second) {
                  HANDLE hTh = OpenThread(
                      THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE,
                      tid);
                  if (hTh) {
                    ApplyAbsoluteThreadPriority(hTh, absPrio);
                    CloseHandle(hTh);
                  }
                }
              }
            }
          } else {
            for (size_t s = 0; s < norm.activeAudioTids.size(); ++s) {
              DWORD aTid = norm.activeAudioTids[s];
              if (aTid == 0)
                continue;
              int sPrio = (s < norm.audioPriorities.size())
                              ? norm.audioPriorities[s]
                              : norm.audioPriority;
              if (IsFixedThreadPriority(sPrio)) {
                HANDLE hTh = OpenThread(
                    THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE,
                    aTid);
                if (hTh) {
                  ApplyAbsoluteThreadPriority(
                      hTh, DecodeFixedThreadPriority(sPrio));
                  CloseHandle(hTh);
                }
              }
            }
          }
        }
      }
      ResetDynamicPriorityFetch(norm, -1);
    } else if (oldRule.processPriorityModified !=
                   norm.processPriorityModified ||
               oldRule.audioPriority != norm.audioPriority ||
               oldRule.audioPriorities != norm.audioPriorities ||
               oldRule.normalPrioForceApplied !=
                   norm.normalPrioForceApplied ||
               oldRule.resetPriorityToZeroRequested !=
                   norm.resetPriorityToZeroRequested) {
      ResetDynamicPriorityFetch(norm, -1);
    }

    WakeUpSteadyRule(norm, 2);
    m_config.rules[index] = norm;
    if (oldRule.isBypassed && !norm.isBypassed && targetPid != 0) {
      m_highQosAppliedPids.erase(targetPid);
    }
    if (affinityConfigChanged) {
      m_appliedThreads.clear();
      m_appliedIdealCores.clear();
      m_prevThreadCpuTimes.clear();
      m_nonChromiumMaskedPids.clear();
      m_cachedProcessTids.clear();
      m_lastProcessThreadCount.clear();
    }
  }
}

std::vector<ProcessRule> ThreadIsolator::GetRulesSnapshot() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_config.rules;
}

void ThreadIsolator::SortRulesByName() {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::sort(m_config.rules.begin(), m_config.rules.end(),
            [](const ProcessRule &a, const ProcessRule &b) {
              return _stricmp(a.processName.c_str(), b.processName.c_str()) < 0;
            });
}

void ThreadIsolator::ToggleProcessBypass(size_t index, size_t slot) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index < m_config.rules.size()) {
    auto &rule = m_config.rules[index];
    if (IsMultiSlotRule(rule)) {
      size_t slotCount = GetRuleSlotCount(rule);
      if (rule.slotBypassed.size() < slotCount) {
        rule.slotBypassed.resize(slotCount, false);
      }
      if (slot < slotCount) {
        rule.slotBypassed[slot] = !rule.slotBypassed[slot];
        if (slot < rule.normalPrioForceApplied.size()) {
          rule.normalPrioForceApplied[slot] = false;
        }
        if (slot < rule.actualAudioBasePriorities.size()) {
          rule.actualAudioBasePriorities[slot] = 0;
        }
        if (slot < rule.actualThreadRelativePrios.size()) {
          rule.actualThreadRelativePrios[slot] = 0;
        }
        ResetDynamicPriorityFetch(rule, static_cast<int>(slot));
        WakeUpSteadyRule(rule, 2);
        m_appliedThreads.clear();
        m_appliedIdealCores.clear();
        m_cachedProcessTids.clear();
        m_lastProcessThreadCount.clear();
        if (rule.slotBypassed[slot]) {
          if (slot < rule.activeAudioTids.size() &&
              rule.activeAudioTids[slot] != 0) {
            DWORD tid = rule.activeAudioTids[slot];
            if (rule.suspendedAudioTids.count(tid) > 0) {
              HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
              if (hThread) {
                while (ResumeThread(hThread) > 1) {
                }
                CloseHandle(hThread);
              }
              rule.suspendedAudioTids.erase(tid);
              rule.isAudioThreadSuspended = !rule.suspendedAudioTids.empty();
            }
          }
        } else {
          if (rule.isRunning) {
            bool needSearch = (slot >= rule.activeAudioTids.size() ||
                               rule.activeAudioTids[slot] == 0);
            if (needSearch) {
              int intervalMs =
                  NormalizePollingInterval(m_config.pollingIntervalMs);
              int turnsPerSec = 1000 / intervalMs;
              if (turnsPerSec < 1)
                turnsPerSec = 1;
              rule.multiSearchSleeping = false;
              rule.multiSearchTurns = 10 * turnsPerSec;
            }
          }
        }
      }
      rule.isBypassed = false;
      return;
    }

    rule.isBypassed = !rule.isBypassed;
    rule.processPriorityModified = false;
    WakeUpSteadyRule(rule, 2);
    m_appliedThreads.clear();
    m_appliedIdealCores.clear();
    m_cachedProcessTids.clear();
    m_lastProcessThreadCount.clear();
    for (size_t s = 0; s < rule.normalPrioForceApplied.size(); ++s) {
      rule.normalPrioForceApplied[s] = false;
    }
    for (size_t s = 0; s < rule.actualAudioBasePriorities.size(); ++s) {
      rule.actualAudioBasePriorities[s] = 0;
    }
    for (size_t s = 0; s < rule.actualThreadRelativePrios.size(); ++s) {
      rule.actualThreadRelativePrios[s] = 0;
    }
    ResetDynamicPriorityFetch(rule, -1);
    if (rule.audioServicePid != 0) {
      ClearChromiumTrackingState(rule.audioServicePid);
      rule.audioServicePid = 0;
    }
    if (rule.activePid != 0) {
      m_chromiumMaskedPids.erase(rule.activePid);
      m_chromiumScannedPids.erase(rule.activePid);
      m_highQosAppliedPids.erase(rule.activePid);
    }
    rule.chromiumScanAttempted = false;
    rule.wasHalfAutoPromoted = false;
    if (rule.isBypassed) {
      ResumeAudioThreadIfSuspended(rule);
      if (IsAudioDgProcess(rule.processName)) {
        RestoreAudioDgProcessLocked(rule.activePid);
      }
      rule.isAudioIsolated = false;
      rule.hasIntruderThreads = false;
      rule.steadyWaitTurns = 0;
      rule.postIsolationGraceTurns = 0;
      rule.activeAudioTid = 0;
      rule.activeAudioTids.clear();
      rule.detectedThreadName = "Bypassed";
      if (rule.isRunning) {
        if (rule.activePid != 0) {
          m_trackedAudioThreads.erase(rule.activePid);
          m_prevThreadCpuTimes.erase(rule.activePid);
          m_samplingStates.erase(rule.activePid);
        }
      }
    } else {
      if (rule.isRunning) {
        int intervalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
        int turnsPerSec = 1000 / intervalMs;
        if (turnsPerSec < 1)
          turnsPerSec = 1;
        rule.detectedThreadName = "Searching...";
        rule.searchPhase = 1;
        rule.searchTurns = 2 * turnsPerSec;
        rule.multiSearchTurns = 10 * turnsPerSec;
        rule.multiSearchSleeping = false;
        rule.lastIdentifiedCount = 0;
      } else {
        rule.detectedThreadName = "";
      }
    }
  }
}

void ThreadIsolator::RestartSearch(size_t index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index >= m_config.rules.size())
    return;
  auto &rule = m_config.rules[index];
  int intervalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
  int turnsPerSec = 1000 / intervalMs;
  if (turnsPerSec < 1)
    turnsPerSec = 1;

  rule.searchPhase = 1;
  rule.searchTurns = 2 * turnsPerSec;
  rule.multiSearchTurns = 10 * turnsPerSec;
  rule.multiSearchSleeping = false;
  rule.lastIdentifiedCount = 0;
  rule.steadyWaitTurns = 0;
  rule.postIsolationGraceTurns = 0;
  if (rule.audioServicePid != 0) {
    rule.detectedThreadName =
        "PID " + std::to_string(rule.audioServicePid) + " / Searching...";
    rule.activePid = rule.audioServicePid;
  } else {
    rule.detectedThreadName = "Searching...";
  }
  rule.isAudioIsolated = false;
  rule.hasIntruderThreads = false;
  rule.activeAudioTid = 0;
  rule.activeAudioTids.clear();
  rule.chromiumScanAttempted = false;
  if (rule.audioServicePid != 0) {
    m_chromiumAudioStates.erase(rule.audioServicePid);
    m_chromiumThreadTracks.erase(rule.audioServicePid);
  }
  m_samplingStates.erase(rule.activePid);
  m_trackedAudioThreads.erase(rule.activePid);
  m_audioDgAppliedTids.erase(rule.activePid);
  m_audioDgLastPrio.erase(rule.activePid);
  m_cachedProcessTids.erase(rule.activePid);
  m_lastProcessThreadCount.erase(rule.activePid);
}

void ThreadIsolator::ResumeMultiSearch(size_t index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index >= m_config.rules.size())
    return;
  auto &rule = m_config.rules[index];
  int intervalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
  int turnsPerSec = 1000 / intervalMs;
  if (turnsPerSec < 1)
    turnsPerSec = 1;
  rule.multiSearchSleeping = false;
  rule.multiSearchTurns = 10 * turnsPerSec;
  WakeUpSteadyRule(rule, 2);
}

bool ThreadIsolator::CheckAndClearInitialNeedSave() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_initialNeedSave) {
    m_initialNeedSave = false;
    return true;
  }
  return false;
}

int ThreadIsolator::NormalizePollingInterval(int ms) {
  if (ms <= 0)
    return 100;
  return ((ms + 99) / 100) * 100;
}

bool ThreadIsolator::ToggleSuspendAudioThread(size_t index, DWORD tid) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index >= m_config.rules.size())
    return false;
  auto &rule = m_config.rules[index];
  if (!rule.isRunning)
    return false;

  // audiodg.exe 専用: プロセス全体を NtSuspendProcess / NtResumeProcess
  // で一時停止・再開
  if (IsAudioDgProcess(rule.processName)) {
    if (rule.activePid == 0)
      return false;
    HANDLE hProc = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, rule.activePid);
    if (!hProc) {
      char errBuf[128];
      snprintf(
          errBuf, sizeof(errBuf),
          "Failed to OpenProcess(PID=%lu) for audiodg suspend/resume: err=%lu",
          rule.activePid, GetLastError());
      LogDebug(errBuf);
      return false;
    }
    if (!rule.isAudioThreadSuspended) {
      if (NtSuspendProcessHelper(hProc) >= 0) {
        rule.isAudioThreadSuspended = true;
        LogDebug("Suspended audiodg.exe Process (NtSuspendProcess)");
      }
    } else {
      if (NtResumeProcessHelper(hProc) >= 0) {
        rule.isAudioThreadSuspended = false;
        LogDebug("Resumed audiodg.exe Process (NtResumeProcess)");
      }
    }
    CloseHandle(hProc);
    return true;
  }

  DWORD targetTid = (tid != 0) ? tid : rule.activeAudioTid;
  if (targetTid == 0)
    return false;

  HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, targetTid);
  if (!hThread) {
    char errBuf[128];
    snprintf(errBuf, sizeof(errBuf),
             "Failed to OpenThread(TID=%lu) for suspend/resume: err=%lu",
             targetTid, GetLastError());
    LogDebug(errBuf);
    return false;
  }

  bool isSuspended = (rule.suspendedAudioTids.count(targetTid) > 0);

  if (!isSuspended) {
    DWORD prevCount = SuspendThread(hThread);
    rule.suspendedAudioTids.insert(targetTid);
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf),
             "Suspended Audio Thread: TID=%lu (prevCount=%lu)", targetTid,
             prevCount);
    LogDebug(logBuf);
  } else {
    DWORD prevCount = ResumeThread(hThread);
    while (prevCount > 1 && prevCount != static_cast<DWORD>(-1)) {
      prevCount = ResumeThread(hThread);
    }
    rule.suspendedAudioTids.erase(targetTid);
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf),
             "Resumed Audio Thread: TID=%lu (prevCount=%lu)", targetTid,
             prevCount);
    LogDebug(logBuf);
  }
  rule.isAudioThreadSuspended = !rule.suspendedAudioTids.empty();
  CloseHandle(hThread);
  return true;
}

bool ThreadIsolator::IsAudioThreadSuspended(size_t index, DWORD tid) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index >= m_config.rules.size())
    return false;
  const auto &rule = m_config.rules[index];
  if (IsAudioDgProcess(rule.processName)) {
    return rule.isAudioThreadSuspended;
  }
  if (tid == 0)
    return false;
  return rule.suspendedAudioTids.count(tid) > 0;
}

void ThreadIsolator::ResumeAllSuspendedThreads() {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto &rule : m_config.rules) {
    ResumeAudioThreadIfSuspended(rule);
    if (IsAudioDgProcess(rule.processName) && !rule.isBypassed) {
      RestoreAudioDgProcessLocked(rule.activePid);
    }
  }
}

bool CpuTopology::CheckIfAllECoresCpu() {
  HKEY hKey = nullptr;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                    KEY_READ, &hKey) == ERROR_SUCCESS) {
    char nameBuf[256] = {0};
    DWORD nameLen = sizeof(nameBuf);
    DWORD type = 0;
    if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, &type,
                         reinterpret_cast<LPBYTE>(nameBuf),
                         &nameLen) == ERROR_SUCCESS) {
      RegCloseKey(hKey);
      std::string name(nameBuf);
      try {
        if (std::regex_search(
                name,
                std::regex(R"(\b(N100|N95|N97|N200|N50|N300|N305|N150|N250)\b)",
                           std::regex_constants::icase)) ||
            std::regex_search(name, std::regex(R"(Processor\s+N\d+)",
                                               std::regex_constants::icase)) ||
            std::regex_search(
                name, std::regex(R"(i3-N\d+)", std::regex_constants::icase))) {
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
  typedef DWORD(WINAPI * PFN_GetActiveProcessorCount)(WORD);
  HMODULE hK32 = GetModuleHandleA("kernel32.dll");
  if (hK32) {
    auto pfn = reinterpret_cast<PFN_GetActiveProcessorCount>(
        GetProcAddress(hK32, "GetActiveProcessorCount"));
    if (pfn) {
      DWORD count = pfn(0xFFFF /* ALL_PROCESSOR_GROUPS */);
      if (count > 0)
        return static_cast<int>(count);
    }
  }
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return static_cast<int>(si.dwNumberOfProcessors);
}

std::vector<CoreInfo> CpuTopology::GetCpuCores() {
  std::vector<CoreInfo> list;
  int totalLogical = GetSystemCoreCount();
  if (totalLogical <= 0)
    totalLogical = 1;

  bool isAllECores = CheckIfAllECoresCpu();

  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (length == 0) {
    std::string prefix = isAllECores ? "E" : "#";
    for (int i = 0; i < totalLogical; i++) {
      list.push_back({i, 0, prefix + std::to_string(i)});
    }
    return list;
  }

  std::vector<BYTE> buffer(length);
  if (GetLogicalProcessorInformationEx(
          RelationProcessorCore,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
              buffer.data()),
          &length)) {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX current =
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buffer.data());
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
            if (bit > maxBitFound)
              maxBitFound = bit;
          }
        }
      }
      offset += current->Size;
      current = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
          reinterpret_cast<BYTE *>(current) + current->Size);
    }

    int coreCount = (std::max)(totalLogical, maxBitFound + 1);

    std::set<BYTE> distinctClasses;
    BYTE maxEff = 0;
    for (const auto &kv : effMap) {
      distinctClasses.insert(kv.second);
      if (kv.second > maxEff)
        maxEff = kv.second;
    }

    bool isHybrid = distinctClasses.size() > 1;

    for (int i = 0; i < coreCount; i++) {
      BYTE eff = 0;
      auto it = effMap.find(i);
      if (it != effMap.end())
        eff = it->second;

      std::string label;
      if (isHybrid) {
        // ハイブリッドCPU (12th/13th/14th Gen, Core Ultra等): P0, P1... / E16,
        // E17...
        label = (eff == maxEff) ? ("P" + std::to_string(i))
                                : ("E" + std::to_string(i));
      } else if (isAllECores) {
        // 純EコアCPU (Intel N100, N95, N200, N300, N305等): E0, E1, E2, E3...
        label = "E" + std::to_string(i);
      } else {
        // 通常の均一CPU (Xeon, Ryzen, 従来型Core等): #0, #1, #2...
        label = "#" + std::to_string(i);
      }

      list.push_back({i, eff, label});
    }
  }

  if (list.empty()) {
    std::string prefix = isAllECores ? "E" : "#";
    for (int i = 0; i < totalLogical; i++) {
      list.push_back({i, 0, prefix + std::to_string(i)});
    }
  }

  return list;
}

int ThreadIsolator::GetSystemCoreCount() {
  return CpuTopology::GetSystemCoreCount();
}

DWORD_PTR ThreadIsolator::GetFullCoreMask(int coreCount) {
  if (coreCount >= 64)
    return ~0ULL;
  return (1ULL << coreCount) - 1ULL;
}

DWORD_PTR ThreadIsolator::MakeCoreMask(int coreIndex) {
  if (coreIndex < 0 || coreIndex >= 64)
    return 0;
  return (1ULL << coreIndex);
}

DWORD_PTR ThreadIsolator::MakeDefaultNormalMask(int coreCount,
                                                int isolatedCore) {
  DWORD_PTR full = GetFullCoreMask(coreCount);
  DWORD_PTR iso = MakeCoreMask(isolatedCore);
  DWORD_PTR normal = full & ~iso;
  return normal ? normal : full;
}

// ── リファクタリング: 共通ヘルパーメソッド実装 ──

// 候補 2: Chromium 系 PID 追跡状態の一括クリア
void ThreadIsolator::ClearChromiumTrackingState(DWORD pid) {
  m_chromiumMaskedPids.erase(pid);
  m_chromiumEvictedPids.erase(pid);
  m_chromiumThreadTracks.erase(pid);
  m_chromiumAudioStates.erase(pid);
  m_chromiumScannedPids.erase(pid);
  m_highQosAppliedPids.erase(pid);
}

// audiodg.exe (AppType: 0) 終了・削除・Bypass 時の標準状態復元 (全コア 0xFFF /
// Normal / 全TID +2)
void ThreadIsolator::RestoreAudioDgProcessLocked(DWORD targetPid) {
  EnableDebugPrivilegeOnce();
  std::vector<DWORD> pids;
  if (targetPid != 0) {
    pids.push_back(targetPid);
  }
  for (const auto &kv : m_audioDgAppliedTids) {
    if (kv.first != 0 &&
        std::find(pids.begin(), pids.end(), kv.first) == pids.end()) {
      pids.push_back(kv.first);
    }
  }
  if (pids.empty()) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32 pe = {sizeof(pe)};
      if (Process32First(hSnap, &pe)) {
        do {
          if (IsAudioDgProcess(pe.szExeFile)) {
            pids.push_back(pe.th32ProcessID);
          }
        } while (Process32Next(hSnap, &pe));
      }
      CloseHandle(hSnap);
    }
  }

  DWORD_PTR fullMask = GetFullCoreMask(CpuTopology::GetSystemCoreCount());
  EnsureNtLoaded();

  for (DWORD pid : pids) {
    if (pid <= 4)
      continue;
    HANDLE hProc = OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) {
      hProc = OpenProcess(PROCESS_SET_INFORMATION |
                              PROCESS_QUERY_LIMITED_INFORMATION,
                          FALSE, pid);
    }
    if (!hProc) {
      hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    }
    if (hProc) {
      if (fullMask != 0) {
        SetProcessAffinityMask(hProc, fullMask);
      }
      SetPriorityClass(hProc, NORMAL_PRIORITY_CLASS);
      CloseHandle(hProc);
    }

    std::vector<THREADENTRY32> threads;
    if (s_pfnNtGetNextThread) {
      HANDLE hQProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
      if (hQProc) {
        HANDLE hCur = NULL, hNext = NULL;
        while (s_pfnNtGetNextThread(hQProc, hCur, THREAD_QUERY_INFORMATION, 0,
                                    0, &hNext) == 0) {
          if (hCur)
            CloseHandle(hCur);
          hCur = hNext;
          DWORD exitCode = 0;
          if (GetExitCodeThread(hCur, &exitCode) && exitCode == STILL_ACTIVE) {
            DWORD tid = GetThreadId(hCur);
            if (tid != 0) {
              THREADENTRY32 te = {0};
              te.dwSize = sizeof(te);
              te.th32OwnerProcessID = pid;
              te.th32ThreadID = tid;
              threads.push_back(te);
            }
          }
        }
        if (hCur)
          CloseHandle(hCur);
        CloseHandle(hQProc);
      }
    }
    if (threads.empty()) {
      QueryProcessThreadsContextSwitches(pid, &threads);
    }

    for (const auto &te : threads) {
      DWORD tid = te.th32ThreadID;
      HANDLE hTh = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                              FALSE, tid);
      if (!hTh) {
        hTh = OpenThread(THREAD_SET_INFORMATION |
                             THREAD_QUERY_LIMITED_INFORMATION,
                         FALSE, tid);
      }
      if (!hTh) {
        hTh = OpenThread(THREAD_SET_INFORMATION, FALSE, tid);
      }
      if (hTh) {
        SetThreadPriority(hTh, THREAD_PRIORITY_HIGHEST); // +2
        CloseHandle(hTh);
      }
    }
  }

  m_audioDgAppliedTids.clear();
  m_audioDgLastPrio.clear();
}

// 候補 3: Suspend 中オーディオスレッドの安全な Resume
void ThreadIsolator::ResumeAudioThreadIfSuspended(ProcessRule &rule) {
  if (IsAudioDgProcess(rule.processName)) {
    if (rule.isAudioThreadSuspended && rule.activePid != 0) {
      HANDLE hProc = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, rule.activePid);
      if (hProc) {
        NtResumeProcessHelper(hProc);
        CloseHandle(hProc);
        LogDebug("Resumed audiodg.exe Process (NtResumeProcess)");
      }
      rule.isAudioThreadSuspended = false;
    }
    return;
  }
  for (DWORD sTid : rule.suspendedAudioTids) {
    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, sTid);
    if (hThread) {
      DWORD prevCount = ResumeThread(hThread);
      while (prevCount > 1 && prevCount != static_cast<DWORD>(-1)) {
        prevCount = ResumeThread(hThread);
      }
      CloseHandle(hThread);
    }
  }
  rule.suspendedAudioTids.clear();
  rule.isAudioThreadSuspended = false;
}

// 候補 4: 表示用スレッドカウントの算出・更新
bool ThreadIsolator::UpdateDisplayThreadCount(ProcessRule &rule,
                                              int totalThreadCount) {
  int displayThreadCount = 0;
  if (rule.appType == 2 || IsAudioDgProcess(rule.processName)) {
    // Chromium 系ブラウザ (AppType: 2) および audiodg.exe (AppType: 0):
    // 確定後もプロセス全体の総スレッド数を常時反映
    displayThreadCount = totalThreadCount;
  } else if (rule.isAudioIsolated) {
    displayThreadCount = 1;
  } else if (rule.detectedThreadName == "sleeping..." ||
             rule.searchPhase == 4) {
    displayThreadCount = 0;
  } else {
    displayThreadCount = totalThreadCount;
  }
  if (rule.currentThreadCount != displayThreadCount) {
    rule.currentThreadCount = displayThreadCount;
    return true;
  }
  return false;
}

// 候補 5: ルール状態変更ログ出力
void ThreadIsolator::LogRuleStateChange(const ProcessRule &rule) {
  char rLog[256];
  snprintf(rLog, sizeof(rLog),
           "Rule State Changed: Proc=%s, PID=%lu, AudioThread='%s', Threads=%d",
           rule.processName.c_str(), rule.activePid,
           rule.detectedThreadName.c_str(), rule.currentThreadCount);
  LogDebug(rLog);
}

// 定常状態 (AppType 0, 2 以外) の 3 秒周期監視ゲート判定
bool ThreadIsolator::IsRuleInSteadyState(const ProcessRule &rule) const {
  if (rule.appType == 0 || rule.appType == 2)
    return false;
  if (!rule.isRunning || rule.isBypassed)
    return false;
  if (rule.postIsolationGraceTurns > 0)
    return false;
  for (bool req : rule.resetPriorityToZeroRequested) {
    if (req)
      return false;
  }
  if (!rule.isAudioIsolated) {
    // RAATServer.exe は Delta 不要の 1 ショット特定 (WaitReason == 4) のため、
    // 未特定待機中も共通 3 秒ゲートでカーネル API 呼び出しを間引く
    return IsRaatServerProcess(rule.processName);
  }
  if (IsMultiSlotRule(rule)) {
    size_t targetCount = GetRuleSlotCount(rule);
    size_t satisfied = 0;
    for (size_t s = 0; s < targetCount; ++s) {
      bool bypassed = (s < rule.slotBypassed.size() && rule.slotBypassed[s]);
      bool hasTid =
          (s < rule.activeAudioTids.size() && rule.activeAudioTids[s] != 0);
      if (bypassed || hasTid) {
        satisfied++;
      }
    }
    if (satisfied < targetCount && !rule.multiSearchSleeping) {
      return false;
    }
    return true;
  }
  return rule.activeAudioTid != 0;
}

void ThreadIsolator::WakeUpSteadyRule(ProcessRule &rule, int graceTurns) {
  rule.steadyWaitTurns = 0;
  if (graceTurns > rule.postIsolationGraceTurns) {
    rule.postIsolationGraceTurns = graceTurns;
  }
}

void ThreadIsolator::ResetDynamicPriorityFetch(ProcessRule &rule,
                                               int targetSlot) {
  if (targetSlot < 0) {
    for (size_t s = 0; s < rule.prioPollTurns.size(); ++s) {
      rule.prioPollTurns[s] = 0;
    }
    for (size_t s = 0; s < rule.prioRefetchTicks.size(); ++s) {
      rule.prioRefetchTicks[s] = 0;
    }
  } else {
    size_t slot = static_cast<size_t>(targetSlot);
    if (rule.prioPollTurns.size() <= slot) {
      rule.prioPollTurns.resize(slot + 1, 0);
    }
    if (rule.prioRefetchTicks.size() <= slot) {
      rule.prioRefetchTicks.resize(slot + 1, 0);
    }
    rule.prioPollTurns[slot] = 0;
    rule.prioRefetchTicks[slot] = 0;
  }
}

bool ThreadIsolator::ShouldFetchDynamicPriority(ProcessRule &rule,
                                                size_t slot) {
  if (rule.prioPollTurns.size() <= slot) {
    rule.prioPollTurns.resize(slot + 1, 0);
  }
  if (rule.prioRefetchTicks.size() <= slot) {
    rule.prioRefetchTicks.resize(slot + 1, 0);
  }
  int stage = rule.prioPollTurns[slot];
  ULONGLONG now = GetTickCount64();
  if (stage <= 0) {
    rule.prioPollTurns[slot] = 1;
    rule.prioRefetchTicks[slot] = now + 3000ULL;
    return true;
  }
  if (stage == 1 && now >= rule.prioRefetchTicks[slot]) {
    rule.prioPollTurns[slot] = 2;
    return true;
  }
  return false;
}

// 共通ヘルパー: オーディオスレッドへの優先度・Ideal
// Processor・アフィニティマスク適用 (Type 1 & Type 2 共通)
bool ThreadIsolator::ApplyAudioThreadSettings(HANDLE hThread, DWORD tid,
                                              int targetAudioPrio,
                                              int audioCore,
                                              DWORD_PTR audioMask,
                                              ProcessRule &rule) {
  bool changed = false;

  // 1. スレッド優先度設定 (現在の優先度と異なる場合のみ更新)
  // 新仕様 (Plan 107): targetAudioPrio == 0 (Normal) の場合は PID 優先度のままとみなし、
  // TID に一切アタッチしない（GetThreadPriority / SetThreadPriority をスキップ）。
  // Fixed (101..131) の場合は NtSetInformationThread で絶対優先度 (KPRIORITY 1..31) を適用。
  if (targetAudioPrio != 0) {
    if (IsFixedThreadPriority(targetAudioPrio)) {
      int absPrio = DecodeFixedThreadPriority(targetAudioPrio);
      int curDyn = QueryThreadDynamicPriority(hThread);
      if (curDyn != absPrio) {
        if (ApplyAbsoluteThreadPriority(hThread, absPrio)) {
          char pLog[128];
          snprintf(pLog, sizeof(pLog),
                   "ApplyAudioThreadSettings: Fixed KPRIORITY TID=%lu, %d -> %d (Success)",
                   tid, curDyn, absPrio);
          LogDebug(pLog);
        }
      }
    } else {
      int curPrio = GetThreadPriority(hThread);
      if (curPrio != targetAudioPrio) {
        if (SetThreadPriority(hThread, targetAudioPrio)) {
          char pLog[128];
          snprintf(pLog, sizeof(pLog),
                   "ApplyAudioThreadSettings: Priority TID=%lu, %d -> %d (Success)",
                   tid, curPrio, targetAudioPrio);
          LogDebug(pLog);
        } else {
          char pLog[128];
          snprintf(pLog, sizeof(pLog),
                   "ApplyAudioThreadSettings: SetThreadPriority FAILED: TID=%lu, "
                   "err=%lu",
                   tid, GetLastError());
          LogDebug(pLog);
        }
      }
    }
  }

  // 2. アフィニティおよび Ideal Processor を設定 (未適用またはマスク変更時のみ発行)
  DWORD_PTR curAff = QueryThreadAffinityMask(hThread);
  bool needApplyMask = (curAff != audioMask ||
                        m_appliedThreads.find(tid) == m_appliedThreads.end() ||
                        m_appliedThreads[tid] != audioMask);
  if (needApplyMask) {
    DWORD_PTR prevMask = SetThreadAffinityMask(hThread, audioMask);
    if (prevMask != 0) {
      m_appliedThreads[tid] = audioMask;
      if (prevMask != audioMask) {
        rule.applyCount++;
        changed = true;
      }
    }
    SetThreadIdealProcessor(hThread, static_cast<DWORD>(audioCore));
    m_appliedIdealCores[tid] = audioCore;
  }

  return changed;
}

// 共通ヘルパー: 通常スレッドへのアフィニティマスク退避適用 (Type 1 & Type 2
// 共通)
bool ThreadIsolator::EvacuateNormalThreadSettings(HANDLE hThread, DWORD tid,
                                                  DWORD_PTR normalMask,
                                                  int idealCore) {
  bool changed = false;
  DWORD_PTR curAff = QueryThreadAffinityMask(hThread);
  bool needApplyMask = (curAff != normalMask ||
                        m_appliedThreads.find(tid) == m_appliedThreads.end() ||
                        m_appliedThreads[tid] != normalMask);
  if (needApplyMask) {
    DWORD_PTR prevMask = SetThreadAffinityMask(hThread, normalMask);
    if (prevMask != 0) {
      m_appliedThreads[tid] = normalMask;
      if (prevMask != normalMask) {
        changed = true;
      }
    }
  }

  if (idealCore < 0 && normalMask != 0) {
    auto it = m_appliedIdealCores.find(tid);
    if (it != m_appliedIdealCores.end() && it->second >= 0 &&
        it->second < static_cast<int>(sizeof(DWORD_PTR) * 8) &&
        (normalMask & (static_cast<DWORD_PTR>(1) << it->second)) != 0) {
      idealCore = it->second;
    } else {
      std::vector<int> cores;
      for (int c = 0; c < static_cast<int>(sizeof(DWORD_PTR) * 8); ++c) {
        if ((normalMask & (static_cast<DWORD_PTR>(1) << c)) != 0) {
          cores.push_back(c);
        }
      }
      if (!cores.empty()) {
        idealCore = cores[m_idealRoundRobinCounter++ % cores.size()];
      }
    }
  }

  if (idealCore >= 0) {
    bool needApplyIdeal =
        (needApplyMask ||
         m_appliedIdealCores.find(tid) == m_appliedIdealCores.end() ||
         m_appliedIdealCores[tid] != idealCore);
    if (needApplyIdeal) {
      SetThreadIdealProcessor(hThread, static_cast<DWORD>(idealCore));
      m_appliedIdealCores[tid] = idealCore;
    }
  }

  return changed;
}

// 候補 6: Phase 1 / Phase 3 スレッド探索・確定共通ルーチン
bool ThreadIsolator::TryIdentifyAudioThread(
    ProcessRule &rule, ProcessSamplingState &sampleState,
    const std::vector<LocalThreadInfo> &threadInfos, HANDLE hProcess, DWORD pid,
    DWORD_PTR audioMask, int intervalMs, const char *phaseName,
    DWORD &identifiedAudioTid, std::string &identifiedAudioLabel,
    bool &stateChanged) {
  DWORD candTid = 0;
  ULONG64 maxDelta = 0;

  DWORD prevActiveTid = rule.activeAudioTid;
  std::vector<DWORD> prevActiveTids = rule.activeAudioTids;

  if (IsRoonApplianceProcess(rule.processName)) {
    // === AppType: 4 (RoonAppliance 単一スロットモード):
    // roonmedia.dll 保持スレッド 負荷1位 (RAAT PCM) を選出 ===
    double effectiveRuleDelta =
        (rule.cyclesDelta > 0.0) ? rule.cyclesDelta : 0.3;
    ULONG64 deltaThreshold = CalculateDeltaThreshold(
        effectiveRuleDelta, m_config.defaultCyclesDelta, intervalMs);

    std::unordered_map<DWORD, ULONG64> threadDeltas;
    std::unordered_set<DWORD> curAliveSet;
    for (const auto &ti : threadInfos) {
      curAliveSet.insert(ti.tid);
      ULONG64 curCycle = 0;
      if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
        auto prevIt = sampleState.lastCycles.find(ti.tid);
        ULONG64 d = 0;
        if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
          d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
        }
        sampleState.lastCycles[ti.tid] = curCycle;
        sampleState.latestDelta[ti.tid] = d;
        threadDeltas[ti.tid] = d;

        if (rule.appType != -1 && d >= deltaThreshold &&
            sampleState.inspectedTids.find(ti.tid) ==
                sampleState.inspectedTids.end()) {
          sampleState.inspectedTids.insert(ti.tid);
          std::string stackSig =
              QueryCallstackAudioSignature(hProcess, ti.hThread, 4);
          if (!stackSig.empty()) {
            if (stackSig.find("Roon Socket") != std::string::npos) {
              sampleState.socketCandidateTids.insert(ti.tid);
            } else {
              sampleState.audioCandidateTids.insert(ti.tid);
            }
          }
        }
      }
    }

    DWORD prevSlot0 =
        (rule.activeAudioTids.size() >= 1 && rule.activeAudioTids[0] != 0 &&
         curAliveSet.count(rule.activeAudioTids[0]) > 0)
            ? rule.activeAudioTids[0]
            : (curAliveSet.count(rule.activeAudioTid) > 0 ? rule.activeAudioTid
                                                          : 0);

    bool slot0Paused =
        (prevSlot0 != 0 && rule.suspendedAudioTids.count(prevSlot0) > 0);

    // roonmedia.dll 候補から最大 Delta を選出 (RAAT PCM)
    DWORD nextSlot0 = prevSlot0;
    ULONG64 maxSlot0Delta = 0;
    if (!slot0Paused) {
      DWORD bestSlot0 = 0;
      for (const auto &ti : threadInfos) {
        if (sampleState.audioCandidateTids.count(ti.tid) > 0) {
          ULONG64 d = threadDeltas[ti.tid];
          if (d > maxSlot0Delta) {
            maxSlot0Delta = d;
            bestSlot0 = ti.tid;
          }
        }
      }
      if (bestSlot0 != 0 &&
          (prevSlot0 == 0 || maxSlot0Delta >= deltaThreshold ||
           threadDeltas[prevSlot0] == 0)) {
        nextSlot0 = bestSlot0;
      }
    }

    if (nextSlot0 != 0) {
      rule.activeAudioTids = {nextSlot0};
      candTid = nextSlot0;
      maxDelta = threadDeltas[candTid];
    }
  } else if (IsRaatServerProcess(rule.processName)) {
    // === AppType: 4 (RAATServer 単一スロットモード):
    // WaitReason == 4 (DelayExecution) のスレッドを直接選出 (Delta無視・3秒周期は共通ゲートで制御) ===
    std::unordered_set<DWORD> curAliveSet;
    for (const auto &ti : threadInfos) {
      curAliveSet.insert(ti.tid);
    }

    DWORD prevSlot0 =
        (rule.activeAudioTids.size() >= 1 && rule.activeAudioTids[0] != 0 &&
         curAliveSet.count(rule.activeAudioTids[0]) > 0)
            ? rule.activeAudioTids[0]
            : (curAliveSet.count(rule.activeAudioTid) > 0 ? rule.activeAudioTid
                                                          : 0);

    bool slot0Paused =
        (prevSlot0 != 0 && rule.suspendedAudioTids.count(prevSlot0) > 0);

    DWORD nextSlot0 = prevSlot0;
    if (!slot0Paused && prevSlot0 == 0) {
      auto waitReasons = QueryProcessThreadsWaitReason(pid);
      for (const auto &ti : threadInfos) {
        auto wrIt = waitReasons.find(ti.tid);
        if (wrIt != waitReasons.end() &&
            wrIt->second == 4 /* DelayExecution */) {
          nextSlot0 = ti.tid;
          break; // Delta無視、WaitReason == 4 のスレッドを1本即特定
        }
      }
    }

    if (nextSlot0 != 0) {
      rule.activeAudioTids = {nextSlot0};
      candTid = nextSlot0;
    }
  } else if (IsVoicemeeterProcess(rule.processName)) {
    // === AppType: 5 (Voicemeeter 全エディション 2行モード):
    // Slot 0 (親行): ws2_32.dll 保持スレッド 最大 Delta (VBAN - <procName>)
    // Slot 1 (子行): 非GUI・非BelowNormal・非Slot0 最大 Delta
    // (VAIO/AUX & DSP -) ===
    double effectiveRuleDelta =
        (rule.cyclesDelta > 0.0) ? rule.cyclesDelta : 1.0;
    ULONG64 deltaThreshold = CalculateDeltaThreshold(
        effectiveRuleDelta, m_config.defaultCyclesDelta, intervalMs);

    std::unordered_map<DWORD, ULONG64> threadDeltas;
    std::unordered_set<DWORD> curAliveSet;
    for (const auto &ti : threadInfos) {
      curAliveSet.insert(ti.tid);
    }

    DWORD prevSlot0 =
        (rule.activeAudioTids.size() >= 1 && rule.activeAudioTids[0] != 0 &&
         curAliveSet.count(rule.activeAudioTids[0]) > 0)
            ? rule.activeAudioTids[0]
            : 0;
    DWORD prevSlot1 =
        (rule.activeAudioTids.size() >= 2 && rule.activeAudioTids[1] != 0 &&
         curAliveSet.count(rule.activeAudioTids[1]) > 0)
            ? rule.activeAudioTids[1]
            : 0;

    bool slot0Paused =
        (prevSlot0 != 0 && rule.suspendedAudioTids.count(prevSlot0) > 0);
    bool slot1Paused =
        (prevSlot1 != 0 && rule.suspendedAudioTids.count(prevSlot1) > 0);

    bool slot0Bypassed = (rule.slotBypassed.size() > 0 && rule.slotBypassed[0]);
    bool slot1Bypassed = (rule.slotBypassed.size() > 1 && rule.slotBypassed[1]);

    for (const auto &ti : threadInfos) {
      ULONG64 curCycle = 0;
      if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
        auto prevIt = sampleState.lastCycles.find(ti.tid);
        ULONG64 d = 0;
        if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
          d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
        }
        sampleState.lastCycles[ti.tid] = curCycle;
        sampleState.latestDelta[ti.tid] = d;
        threadDeltas[ti.tid] = d;

        // 各スレッドにつき生涯1回のみ 32KB スタック走査を実行
        if (rule.appType != -1 && d >= deltaThreshold &&
            sampleState.inspectedTids.find(ti.tid) ==
                sampleState.inspectedTids.end()) {
          sampleState.inspectedTids.insert(ti.tid);
          std::string vmSig =
              QueryCallstackAudioSignature(hProcess, ti.hThread, 5);
          if (vmSig.find("[WS2]") != std::string::npos)
            sampleState.socketCandidateTids.insert(ti.tid);
          if (vmSig.find("[GUI]") != std::string::npos)
            sampleState.vmGuiTids.insert(ti.tid);
        }
      }
    }

    // Slot 0: VBAN (ws2_32.dll) 最大 Delta
    DWORD nextSlot0 = prevSlot0;
    ULONG64 maxSlot0Delta = 0;
    if (!slot0Paused && !slot0Bypassed) {
      DWORD bestSlot0 = 0;
      for (const auto &ti : threadInfos) {
        if (slot1Paused && ti.tid == prevSlot1)
          continue;
        if (sampleState.socketCandidateTids.count(ti.tid) > 0) {
          ULONG64 d = threadDeltas[ti.tid];
          if (d >= deltaThreshold && d > maxSlot0Delta) {
            maxSlot0Delta = d;
            bestSlot0 = ti.tid;
          }
        }
      }
      if (bestSlot0 != 0 &&
          (prevSlot0 == 0 || maxSlot0Delta >= deltaThreshold ||
           threadDeltas[prevSlot0] == 0)) {
        nextSlot0 = bestSlot0;
      }
    }

    // Slot 1: VAIO/AUX & DSP (メインエンジン: 非GUI・非BelowNormal・非Slot0 最大 Delta)
    DWORD nextSlot1 = (prevSlot1 != nextSlot0) ? prevSlot1 : 0;
    ULONG64 maxSlot1Delta = 0;
    if (!slot1Paused && !slot1Bypassed) {
      DWORD bestSlot1 = 0;
      for (const auto &ti : threadInfos) {
        if (ti.tid == nextSlot0)
          continue;
        if (sampleState.socketCandidateTids.count(ti.tid) > 0 ||
            sampleState.vmGuiTids.count(ti.tid) > 0)
          continue;
        if (ti.priority == THREAD_PRIORITY_BELOW_NORMAL)
          continue;
        ULONG64 d = threadDeltas[ti.tid];
        if (d >= deltaThreshold && d > maxSlot1Delta) {
          maxSlot1Delta = d;
          bestSlot1 = ti.tid;
        }
      }
      if (bestSlot1 != 0 &&
          (nextSlot1 == 0 || maxSlot1Delta >= deltaThreshold ||
           threadDeltas[nextSlot1] == 0)) {
        nextSlot1 = bestSlot1;
      }
    }

    if (nextSlot0 != 0 || nextSlot1 != 0) {
      rule.activeAudioTids = {nextSlot0, nextSlot1};
      candTid = (nextSlot0 != 0) ? nextSlot0 : nextSlot1;
      maxDelta = threadDeltas[candTid];
    }
  } else {
    // === 通常モード: スタック走査によるシグネチャ照合 & 最大 Delta 選定 ===
    for (const auto &ti : threadInfos) {
      ULONG64 curCycle = 0;
      if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
        auto prevIt = sampleState.lastCycles.find(ti.tid);
        ULONG64 d = 0;
        if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
          d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
        }
        sampleState.lastCycles[ti.tid] = curCycle;
        sampleState.latestDelta[ti.tid] = d;

        // 秒換算 CyclesDelta の 1 ターン閾値
        ULONG64 deltaThreshold = CalculateDeltaThreshold(
            rule.cyclesDelta, m_config.defaultCyclesDelta, intervalMs);

        // 未検査スレッドのモジュール/シグネチャ検査 (CyclesDelta
        // 以上のスレッドに対してのみ検査、AppType: -1 はバイパス)
        if (rule.appType != -1 && d >= deltaThreshold &&
            sampleState.inspectedTids.find(ti.tid) ==
                sampleState.inspectedTids.end()) {
          sampleState.inspectedTids.insert(ti.tid);
          std::string stackSig =
              QueryCallstackAudioSignature(hProcess, ti.hThread, rule.appType);

          if (rule.appType == 3) {
            // モード 3: DAW型高負荷アプリ (Nuendo, Cubase 等)
            // 初回: シグネチャ合致で候補登録、さらに timeclit に flag = 1
            // を刻印
            if (!stackSig.empty()) {
              sampleState.audioCandidateTids.insert(ti.tid);
              bool isTimeCritical =
                  (ti.priority == THREAD_PRIORITY_TIME_CRITICAL ||
                   ti.basePri >= 15);
              if (isTimeCritical) {
                m_threadFlags[pid][ti.tid] = 1; // 救済用フラグ
              }
            }
          } else {
            // モード 1: 非 Chromium (ゲーム・Spotify等)
            if (!stackSig.empty()) {
              sampleState.audioCandidateTids.insert(ti.tid);
            }
          }
        }

        // 判定: (AppType: -1 || オーディオ候補) かつ (flag == 1 || timeclit)
        // かつ (d > 0) audit_process.py 準拠: シグネチャ合致(またはAppType:
        // -1)かつ稼働中のスレッドの中から最大 Delta を選定
        bool isCandidate = (rule.appType == -1) ||
                           (sampleState.audioCandidateTids.count(ti.tid) > 0);
        bool isEligible = true;
        if (rule.appType == 3) {
          isEligible = (m_threadFlags[pid][ti.tid] == 1 ||
                        ti.priority == THREAD_PRIORITY_TIME_CRITICAL ||
                        ti.basePri >= 15);
        }
        if (isCandidate && isEligible && d > 0) {
          if (d > maxDelta) {
            maxDelta = d;
            candTid = ti.tid;
          }
        }
      }
    }
    if (candTid != 0) {
      rule.activeAudioTids = {candTid};
    }
  }

  if (candTid != 0) {
    // 確定！
    identifiedAudioTid = candTid;
    identifiedAudioLabel = "PID " + std::to_string(pid) + " / TID " +
                           std::to_string(identifiedAudioTid);
    m_trackedAudioThreads[pid].clear();
    m_trackedAudioThreads[pid][identifiedAudioTid] = identifiedAudioLabel;
    for (DWORD t : rule.activeAudioTids) {
      if (t != 0) {
        m_trackedAudioThreads[pid][t] =
            "PID " + std::to_string(pid) + " / TID " + std::to_string(t);
      }
    }
    bool wasIsolated = rule.isAudioIsolated;
    DWORD prevPid = rule.activePid;
    rule.isAudioIsolated = true;
    rule.activePid = pid;
    rule.activeAudioTid = identifiedAudioTid;
    rule.detectedThreadName = identifiedAudioLabel;
    rule.searchPhase = 0;
    rule.searchTurns = 0;
    bool isDynamic = (phaseName && strcmp(phaseName, "DynamicTracking") == 0);
    if (!isDynamic || !wasIsolated || prevPid != pid ||
        prevActiveTid != identifiedAudioTid ||
        prevActiveTids != rule.activeAudioTids) {
      ResetDynamicPriorityFetch(rule, -1);
      WakeUpSteadyRule(rule, 3);
      for (const auto &ti : threadInfos) {
        m_appliedIdealCores.erase(ti.tid);
      }
      stateChanged = true;
      char hLog[128];
      snprintf(hLog, sizeof(hLog),
               "Audio thread identified in %s: PID=%lu, TID=%lu, count=%zu, "
               "delta=%llu",
               phaseName, pid, identifiedAudioTid, rule.activeAudioTids.size(),
               maxDelta);
      LogDebug(hLog);
    }
    return true;
  }

  return false;
}

bool ThreadIsolator::ScanAndIsolate() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_config.rules.empty() && m_config.unlistedMaskMode == 0)
    return false;

  EnableDebugPrivilegeOnce();

  bool stateChanged = false;
  int coreCount = GetSystemCoreCount();

  // 1. 実行中の全プロセスを取得 (動的スキップ制御 & 低速ポーリング)
  // (A) 監視有効な登録アプリがすべて起動中の場合は SNAPPROCESS をスキップ (0
  // 回) (B) 未起動アプリが存在する場合は INI x 10 (例: 500ms設定なら5秒)
  // のスロー周期でのみ発行
  static std::unordered_map<std::string, std::vector<DWORD>>
      s_cachedRunningProcesses;
  static std::unordered_map<DWORD, int> s_cachedCurrentPidThreadCounts;
  static int s_processScanCountdown = 0;

  bool hasUnstartedApp = false;
  for (const auto &r : m_config.rules) {
    if (!r.isBypassed) {
      if (!r.isRunning || (r.appType == 2 && r.audioServicePid == 0)) {
        hasUnstartedApp = true;
        break;
      }
    }
  }

  static bool s_unlistedHasPending = false;
  bool shouldScanProcesses = false;
  if (s_cachedRunningProcesses.empty() || s_unlistedHasPending) {
    shouldScanProcesses = true;
    s_processScanCountdown = 10;
  } else if (hasUnstartedApp || m_config.unlistedMaskMode != 0) {
    s_processScanCountdown--;
    if (s_processScanCountdown <= 0) {
      shouldScanProcesses = true;
      s_processScanCountdown = 10;
    }
  } else {
    // 全アプリ起動中: スキップ (0 回)
    s_processScanCountdown = 10;
  }

  // SNAPPROCESS スキップ時は、既存プロセスの死活を GetExitCodeProcess
  // で高速確認 (数ナノ秒)
  if (!shouldScanProcesses) {
    for (auto it = s_cachedRunningProcesses.begin();
         it != s_cachedRunningProcesses.end(); ++it) {
      for (auto pIt = it->second.begin(); pIt != it->second.end();) {
        HANDLE hP = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pIt);
        if (!hP) {
          pIt = it->second.erase(pIt);
          shouldScanProcesses = true;
        } else {
          DWORD exitCode = 0;
          if (GetExitCodeProcess(hP, &exitCode) && exitCode != STILL_ACTIVE) {
            CloseHandle(hP);
            pIt = it->second.erase(pIt);
            shouldScanProcesses = true;
          } else {
            CloseHandle(hP);
            ++pIt;
          }
        }
      }
    }
  }

  if (shouldScanProcesses) {
    HANDLE hSnapProc = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapProc != INVALID_HANDLE_VALUE) {
      s_cachedRunningProcesses.clear();
      s_cachedCurrentPidThreadCounts.clear();
      PROCESSENTRY32 pe;
      pe.dwSize = sizeof(pe);
      if (Process32First(hSnapProc, &pe)) {
        do {
          std::string procNameLower = ToLowerA(pe.szExeFile);
          s_cachedRunningProcesses[procNameLower].push_back(pe.th32ProcessID);
          s_cachedCurrentPidThreadCounts[pe.th32ProcessID] =
              static_cast<int>(pe.cntThreads);
        } while (Process32Next(hSnapProc, &pe));
      }
      CloseHandle(hSnapProc);
    }
  }

  const auto &runningProcesses = s_cachedRunningProcesses;
  const auto &currentPidThreadCounts = s_cachedCurrentPidThreadCounts;

  if (shouldScanProcesses && m_config.unlistedMaskMode != 0 &&
      m_activeUnlistedMask != 0) {
    std::unordered_set<DWORD> allSnapPids;
    for (const auto &kv : runningProcesses) {
      for (DWORD p : kv.second) {
        allSnapPids.insert(p);
      }
    }
    EraseDeadPidSet(m_unlistedMaskedPids, allSnapPids);

    auto whitelist = BuildUnlistedWhitelist(true);
    DWORD myPid = GetCurrentProcessId();
    int changedBatchCount = 0;
    const int kMaxUnlistedChangesPerTurn = 1; // 1ターンあたり最大1プロセスまで変更 (時間分散)
    bool anyRemaining = false;

    for (const auto &kv : runningProcesses) {
      if (whitelist.count(kv.first) > 0)
        continue;
      for (DWORD pid : kv.second) {
        if (pid <= 4 || pid == myPid || m_unlistedMaskedPids.count(pid) > 0)
          continue;

        if (changedBatchCount >= kMaxUnlistedChangesPerTurn) {
          anyRemaining = true;
          break;
        }

        m_unlistedMaskedPids.insert(pid);
        if (SetProcessAffinityByPid(pid, m_activeUnlistedMask)) {
          m_unlistedApplyCount++;
          stateChanged = true;
          changedBatchCount++;
        }
      }
      if (anyRemaining) {
        break;
      }
    }
    s_unlistedHasPending = anyRemaining;
  }

  if (m_config.rules.empty())
    return stateChanged;

  // 1 秒あたりのターン数および定常 3 秒周期ターン数計算 (500ms設定時は steadyPeriodTurns = 6)
  int intervalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
  int turnsPerSec = 1000 / intervalMs;
  if (turnsPerSec < 1)
    turnsPerSec = 1;
  const int steadyPeriodTurns = std::max(1, 3000 / intervalMs);

  // 登録ルールのうち現在実行中かつ監視有効 (!isBypassed) の対象 PID を抽出
  // ※定常状態 (AppType 0, 2 以外) のスキップターンでは対象 PID を除外し、カーネル API 呼び出しをゼロにする
  std::vector<bool> ruleSteadySkipped(m_config.rules.size(), false);
  std::unordered_set<DWORD> activeTargetPids;
  for (size_t ri = 0; ri < m_config.rules.size(); ++ri) {
    auto &r = m_config.rules[ri];
    if (r.isBypassed)
      continue; // 監視除外プロセスのスレッドスキャン誘発を防止

    auto rIt = runningProcesses.find(ToLowerA(r.processName));
    if (rIt != runningProcesses.end() && !rIt->second.empty()) {
      if (IsRuleInSteadyState(r)) {
        if (r.steadyWaitTurns > 0) {
          r.steadyWaitTurns--;
          ruleSteadySkipped[ri] = true;
          continue;
        }
        r.steadyWaitTurns = (steadyPeriodTurns > 1) ? (steadyPeriodTurns - 1) : 0;
      } else {
        r.steadyWaitTurns = 0;
      }

      if (r.appType == 2) {
        // Chromium 系: スレッド走査が必要なのは特定済みの AudioService PID のみ
        if (r.audioServicePid != 0) {
          activeTargetPids.insert(r.audioServicePid);
        }
      } else {
        // 非 Chromium 系: 全 PID を対象
        for (DWORD p : rIt->second)
          activeTargetPids.insert(p);
      }
    }
  }

  std::unordered_map<DWORD, std::vector<THREADENTRY32>> processThreads;
  std::unordered_map<DWORD, std::unordered_map<DWORD, DWORD_PTR>>
      processThreadAffinities;

  // 全件 Not running または定常スキップの時はスレッド走査をスキップ
  // NtGetNextThread + NtQueryInformationThread(ThreadBasicInformation) により、
  // 対象 PID のスレッド TID と AffinityMask を 1 パスで同時取得
  if (!activeTargetPids.empty()) {
    EnsureNtLoaded();
    if (s_pfnNtGetNextThread) {
      for (DWORD pid : activeTargetPids) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (hProc) {
          HANDLE hCurThread = NULL;
          HANDLE hNextThread = NULL;
          while (s_pfnNtGetNextThread(hProc, hCurThread,
                                      THREAD_QUERY_INFORMATION, 0, 0,
                                      &hNextThread) == 0) {
            if (hCurThread) {
              CloseHandle(hCurThread);
            }
            hCurThread = hNextThread;

            DWORD tid = 0;
            DWORD_PTR affMask = 0;
            LONG basePri = 0;
            if (s_pfnNtQueryInformationThread) {
              THREAD_BASIC_INFO_RAW tbi = {0};
              ULONG retLen = 0;
              if (s_pfnNtQueryInformationThread(
                      hCurThread, 0 /* ThreadBasicInformation */, &tbi,
                      sizeof(tbi), &retLen) == 0) {
                tid = static_cast<DWORD>(tbi.UniqueThreadId);
                affMask = tbi.AffinityMask;
                basePri = tbi.BasePriority;
              }
            }
            if (tid == 0) {
              tid = GetThreadId(hCurThread);
            }
            if (tid != 0) {
              THREADENTRY32 te = {0};
              te.dwSize = sizeof(te);
              te.th32OwnerProcessID = pid;
              te.th32ThreadID = tid;
              te.tpBasePri = basePri;
              processThreads[pid].push_back(te);
              if (affMask != 0) {
                processThreadAffinities[pid][tid] = affMask;
              }
            }
          }
          if (hCurThread) {
            CloseHandle(hCurThread);
          }
          CloseHandle(hProc);
        }
        if (processThreads[pid].empty()) {
          QueryProcessThreadsContextSwitches(pid, &processThreads[pid]);
        }
      }
    }
  }

  // 初回起動時: 前回の終了時ステータス記録と照合 (起動時引き継ぎ)
  static bool s_initialMatchDone = false;
  if (!s_initialMatchDone) {
    s_initialMatchDone = true;
    bool needSave = false;

    for (auto &rule : m_config.rules) {
      if (IsAudioDgProcess(rule.processName)) {
        if (rule.lastAudioPid != 0 || rule.lastAudioTid != 0 ||
            !rule.lastAudioTids.empty()) {
          rule.lastAudioPid = 0;
          rule.lastAudioTid = 0;
          rule.lastAudioTids.clear();
          needSave = true;
        }
        continue;
      }
      // 検証用: INI の前回 PID/TID からの無検証高速復帰ロジックをスキップ
      if (false && rule.lastAudioPid != 0 &&
          (!rule.lastAudioTids.empty() || rule.lastAudioTid != 0)) {
        if (rule.lastAudioTids.empty() && rule.lastAudioTid != 0) {
          rule.lastAudioTids.push_back(rule.lastAudioTid);
        }

        bool matched = false;
        auto rIt = runningProcesses.find(ToLowerA(rule.processName));
        if (rIt != runningProcesses.end() && !rule.isBypassed) {
          bool pidAlive = false;
          for (DWORD p : rIt->second) {
            if (p == rule.lastAudioPid) {
              pidAlive = true;
              break;
            }
          }
          if (pidAlive) {
            // スレッド数の検証:
            // マルチスロット (AppType: 4 Roon / AppType: 5 Voicemeeter) は
            // targetCount 個。それ以外は 1 個。
            size_t targetCount = GetRuleSlotCount(rule);

            // PID が 1個、TID が n個、TIDコア#が別個に n個。全て合致でスキップ
            if (rule.lastAudioTids.size() == targetCount) {
              auto ptIt = processThreads.find(rule.lastAudioPid);
              if (ptIt != processThreads.end()) {
                std::unordered_set<DWORD> livingTids;
                for (const auto &te : ptIt->second) {
                  livingTids.insert(te.th32ThreadID);
                }

                bool allSlotsMatched = true;
                for (size_t s = 0; s < targetCount; ++s) {
                  DWORD expectedTid = rule.lastAudioTids[s];
                  if (livingTids.find(expectedTid) == livingTids.end()) {
                    allSlotsMatched = false;
                    break;
                  }

                  // スロット s の期待オーディオコアマスク
                  DWORD_PTR expectedMask = 0;
                  if (s < rule.audioSlotMasks.size() && rule.audioSlotMasks[s] != 0) {
                    expectedMask = rule.audioSlotMasks[s];
                  } else if (s < rule.audioCores.size()) {
                    int c = rule.audioCores[s];
                    if (c >= 0 && c < 64)
                      expectedMask = (1ULL << c);
                  } else if (rule.audioAffinityMask != 0) {
                    expectedMask = rule.audioAffinityMask;
                  } else {
                    expectedMask = MakeCoreMask(rule.audioCore);
                  }

                  HANDLE hThread =
                      OpenThread(THREAD_QUERY_INFORMATION, FALSE, expectedTid);
                  if (hThread) {
                    DWORD_PTR curAff = QueryThreadAffinityMask(hThread);
                    CloseHandle(hThread);
                    if (curAff != expectedMask) {
                      allSlotsMatched = false;
                      break;
                    }
                  } else {
                    allSlotsMatched = false;
                    break;
                  }
                }

                if (allSlotsMatched) {
                  matched = true;
                  rule.activePid = rule.lastAudioPid;
                  rule.activeAudioTids = rule.lastAudioTids;
                  rule.activeAudioTid = rule.lastAudioTids[0];
                  rule.isAudioIsolated = true;
                  rule.isRunning = true;
                  WakeUpSteadyRule(rule, 2);
                  rule.detectedThreadName =
                      "PID " + std::to_string(rule.lastAudioPid) + " / TID " +
                      std::to_string(rule.activeAudioTid);
                  for (DWORD t : rule.activeAudioTids) {
                    m_trackedAudioThreads[rule.lastAudioPid][t] =
                        "PID " + std::to_string(rule.lastAudioPid) + " / TID " +
                        std::to_string(t);
                  }
                  if (rule.appType == 2) {
                    rule.audioServicePid = rule.lastAudioPid;
                    m_chromiumThreadTracks[rule.lastAudioPid]
                                          [rule.activeAudioTid]
                                              .flag = 2;
                    m_chromiumAudioStates[rule.lastAudioPid].initialEvaluated =
                        true;
                  }
                  stateChanged = true;
                }
              }
            }
          }
        }

        if (!matched) {
          // 不一致・不在・未起動: 照合用記録を消去
          rule.lastAudioPid = 0;
          rule.lastAudioTid = 0;
          rule.lastAudioTids.clear();
          needSave = true;
        }
      }
    }

    if (needSave) {
      m_initialNeedSave = true;
    }
  }

  // 終了したプロセスのトラッキング情報をクリーンアップ
  std::unordered_set<DWORD> allActivePids;
  for (const auto &kv : runningProcesses) {
    for (DWORD p : kv.second)
      allActivePids.insert(p);
  }
  EraseDeadPidKeys(m_trackedAudioThreads, allActivePids);
  EraseDeadPidKeys(m_prevThreadCpuTimes, allActivePids);
  EraseDeadPidKeys(m_cachedProcessTids, allActivePids);
  EraseDeadPidKeys(m_lastProcessThreadCount, allActivePids);
  EraseDeadPidKeys(m_audioDgAppliedTids, allActivePids);
  EraseDeadPidKeys(m_audioDgLastPrio, allActivePids);
  EraseDeadPidSet(m_highQosAppliedPids, allActivePids);

  // 2. 各登録ルールのプロセスを検査・アフィニティ制御
  for (size_t ruleIdx = 0; ruleIdx < m_config.rules.size(); ++ruleIdx) {
    auto &rule = m_config.rules[ruleIdx];
    std::string targetLower = ToLowerA(rule.processName);
    auto it = runningProcesses.find(targetLower);

    if (it == runningProcesses.end() || it->second.empty()) {
      if (rule.isRunning) {
        ResumeAudioThreadIfSuspended(rule);
        if (rule.activePid != 0) {
          m_samplingStates.erase(rule.activePid);
          m_audioDgAppliedTids.erase(rule.activePid);
          m_audioDgLastPrio.erase(rule.activePid);
          m_cachedProcessTids.erase(rule.activePid);
          m_lastProcessThreadCount.erase(rule.activePid);
        }
        rule.isRunning = false;
        rule.activePid = 0;
        rule.activeAudioTid = 0;
        rule.activeAudioTids.clear();
        rule.detectedThreadName = rule.isBypassed ? "Bypassed" : "";
        rule.isAudioIsolated = false;
        rule.hasIntruderThreads = false;
        rule.wasHalfAutoPromoted = false;
        rule.chromiumScanAttempted = false;
        rule.searchPhase = 0;
        rule.searchTurns = 0;
        rule.steadyWaitTurns = 0;
        rule.postIsolationGraceTurns = 0;
        for (size_t s = 0; s < rule.normalPrioForceApplied.size(); ++s) {
          rule.normalPrioForceApplied[s] = false;
        }
        for (size_t s = 0; s < rule.actualAudioBasePriorities.size(); ++s) {
          rule.actualAudioBasePriorities[s] = 0;
        }
        for (size_t s = 0; s < rule.actualThreadRelativePrios.size(); ++s) {
          rule.actualThreadRelativePrios[s] = 0;
        }
        ResetDynamicPriorityFetch(rule, -1);
        if (rule.audioServicePid != 0) {
          ClearChromiumTrackingState(rule.audioServicePid);
          rule.audioServicePid = 0;
        }
        stateChanged = true;
      } else {
        for (size_t s = 0; s < rule.normalPrioForceApplied.size(); ++s) {
          if (rule.normalPrioForceApplied[s]) {
            rule.normalPrioForceApplied[s] = false;
            stateChanged = true;
          }
        }
      }
      continue;
    }

    rule.isRunning = true;

    // 監視除外・ペンディング (チェックON時)
    // はスレッド走査・アフィニティ制御・優先度制御を完全スキップ
    if (rule.isBypassed) {
      if (IsAudioDgProcess(rule.processName) && !m_audioDgAppliedTids.empty()) {
        RestoreAudioDgProcessLocked(rule.activePid);
      }
      if (rule.detectedThreadName != "Bypassed") {
        rule.detectedThreadName = "Bypassed";
        stateChanged = true;
      }
      rule.isAudioIsolated = false;
      rule.hasIntruderThreads = false;
      rule.wasHalfAutoPromoted = false;
      rule.chromiumScanAttempted = false;
      rule.searchPhase = 0;
      rule.searchTurns = 0;
      rule.steadyWaitTurns = 0;
      rule.postIsolationGraceTurns = 0;
      rule.activeAudioTid = 0;
      rule.activeAudioTids.clear();
      for (size_t s = 0; s < rule.normalPrioForceApplied.size(); ++s) {
        rule.normalPrioForceApplied[s] = false;
      }
      for (size_t s = 0; s < rule.actualAudioBasePriorities.size(); ++s) {
        rule.actualAudioBasePriorities[s] = 0;
      }
      for (size_t s = 0; s < rule.actualThreadRelativePrios.size(); ++s) {
        rule.actualThreadRelativePrios[s] = 0;
      }
      ResetDynamicPriorityFetch(rule, -1);
      if (rule.audioServicePid != 0) {
        ClearChromiumTrackingState(rule.audioServicePid);
        rule.audioServicePid = 0;
      }
      for (DWORD pid : it->second) {
        m_chromiumMaskedPids.erase(pid);
        m_chromiumScannedPids.erase(pid);
        m_trackedAudioThreads.erase(pid);
        m_prevThreadCpuTimes.erase(pid);
        m_samplingStates.erase(pid);
        m_audioDgAppliedTids.erase(pid);
        m_audioDgLastPrio.erase(pid);
        m_cachedProcessTids.erase(pid);
        m_lastProcessThreadCount.erase(pid);
      }
      continue;
    }

    // 定常状態 (AppType 0, 2 以外) の 3 秒周期スキップターン:
    // プロセス生存確認 (Not running) と Bypass 判定のみ終えた状態でカーネル API 操作を全スキップ
    if (ruleIdx < ruleSteadySkipped.size() && ruleSteadySkipped[ruleIdx]) {
      continue;
    }

    // 初回・専用種別判定 (0: 未判定/audiodg.exe固定, 1: 非Chromium, 2:
    // Chromium系, 3: DAW型高負荷, 4: Roon専用, 5: Voicemeeter専用)
    if (IsAudioDgProcess(rule.processName)) {
      rule.appType = 0;
    } else if (IsRoonApplianceProcess(rule.processName) ||
               IsRaatServerProcess(rule.processName) ||
               IsRoonBridgeProcess(rule.processName)) {
      rule.appType = 4;
    } else if (IsVoicemeeterProcess(rule.processName)) {
      rule.appType = 5;
    } else if (rule.appType == 0) {
      HANDLE hFirstProc = OpenProcess(
          PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, it->second[0]);
      if (hFirstProc) {
        if (DetectChromiumExe(hFirstProc)) {
          rule.appType = 2; // Chromium 系
        }
        CloseHandle(hFirstProc);
      }
      if (rule.appType == 0) {
        // Time Critical (優先度 15 / basePri >= 15) スレッド数を計数
        int tcCount = 0;
        auto ptIt = processThreads.find(it->second[0]);
        if (ptIt != processThreads.end()) {
          for (const auto &te : ptIt->second) {
            HANDLE hTh =
                OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hTh) {
              int pri = GetThreadPriority(hTh);
              if (pri == THREAD_PRIORITY_TIME_CRITICAL || te.tpBasePri >= 15) {
                tcCount++;
              }
              CloseHandle(hTh);
            }
          }
        }
        if (tcCount >= 2) {
          rule.appType = 3; // DAW 型高負荷アプリ (Nuendo, Cubase 等)
        } else {
          rule.appType = 1; // 通常の非 Chromium (ゲーム・一般アプリ)
        }
      }
      char cLog[128];
      snprintf(cLog, sizeof(cLog), "AppType detect: '%s' -> appType=%d",
               rule.processName.c_str(), rule.appType);
      LogDebug(cLog);
    }

    int totalThreadCount = 0;
    bool audioDetectedInAny = false;
    std::string primaryAudioThreadName = "";
    DWORD primaryAudioPid = 0;
    DWORD primaryAudioTid = 0;

    // コアマスクの計算 (複数コア指定に対応)
    DWORD_PTR audioMask = rule.audioAffinityMask ? rule.audioAffinityMask
                                                 : MakeCoreMask(rule.audioCore);
    DWORD_PTR baseNormal =
        (rule.normalAffinityMask != 0)
            ? rule.normalAffinityMask
            : (m_config.normalAffinityMask ? m_config.normalAffinityMask
                                           : GetFullCoreMask(coreCount));
    // 通常スレッドのアフィニティマスクからオーディオコア群を物理的に完全除外
    DWORD_PTR normalMask = baseNormal & ~audioMask;
    if (normalMask == 0) {
      normalMask = MakeDefaultNormalMask(coreCount, rule.audioCore);
    }
    DWORD_PTR parentMask = audioMask | baseNormal;

    // ── audiodg.exe 専用パス (AppType: 0): PID 全体に
    // audioMask・優先度クラス・HighQoS を適用し、 全 TID
    // (およびポーリングごとの新規 TID) にスレッド優先度を適用 ──
    if (IsAudioDgProcess(rule.processName)) {
      DWORD latestIsolatedPid = 0;
      int latestPidThreadCount = 0;
      bool accessDenied = false;
      for (DWORD pid : it->second) {
        auto ptIt = processThreads.find(pid);
        int pidThreadCount = (ptIt != processThreads.end())
                                 ? static_cast<int>(ptIt->second.size())
                                 : 0;
        latestPidThreadCount = pidThreadCount;

        HANDLE hProc = OpenProcess(
            PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProc) {
          hProc = OpenProcess(PROCESS_SET_INFORMATION |
                                  PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, pid);
        }
        if (!hProc) {
          if (GetLastError() == ERROR_ACCESS_DENIED) {
            accessDenied = true;
          }
        }

        if (hProc) {
          // 1. HighQoS 適用 (新規検出PIDに対し初回1回のみ)
          if (m_highQosAppliedPids.insert(pid).second) {
            ApplyProcessHighQoS(hProc);
          }

          // 2. PID に audioMask を適用
          DWORD_PTR currentProcMask = 0, sysMask = 0;
          if (GetProcessAffinityMask(hProc, &currentProcMask, &sysMask)) {
            if (currentProcMask != audioMask) {
              if (SetProcessAffinityMask(hProc, audioMask)) {
                stateChanged = true;
              }
            }
          } else {
            SetProcessAffinityMask(hProc, audioMask);
          }

          // 3. PID にプロセス優先度クラスを適用 (Col 3 明示変更時のみ SetPriorityClass を実行)
          int targetAudioPrio = (!rule.audioPriorities.empty())
                                    ? rule.audioPriorities[0]
                                    : rule.audioPriority;
          DWORD curPrioClass = GetPriorityClass(hProc);
          bool procPrioSet = false;
          if (rule.processPriorityModified && rule.processPriorityClass != 0) {
            if (curPrioClass != 0 &&
                curPrioClass != rule.processPriorityClass) {
              if (SetPriorityClass(hProc, rule.processPriorityClass)) {
                procPrioSet = true;
                stateChanged = true;
              } else if (rule.processPriorityClass == REALTIME_PRIORITY_CLASS) {
                if (SetPriorityClass(hProc, HIGH_PRIORITY_CLASS)) {
                  procPrioSet = true;
                  stateChanged = true;
                }
              }
              if (procPrioSet) {
                curPrioClass = GetPriorityClass(hProc);
              }
            }
          }
          if (curPrioClass != 0) {
            if (rule.actualProcessPriorityClass != curPrioClass) {
              rule.actualProcessPriorityClass = curPrioClass;
              stateChanged = true;
            }
            if (!rule.processPriorityModified) {
              rule.processPriorityClass = curPrioClass;
            }
          }

          CloseHandle(hProc);

          // Col 3 変更直後かつ Fixed 指定時は全 TID に無条件で NtSetInformationThread を再適用
          if (procPrioSet && IsFixedThreadPriority(targetAudioPrio) &&
              ptIt != processThreads.end()) {
            int absPrio = DecodeFixedThreadPriority(targetAudioPrio);
            for (const auto &te : ptIt->second) {
              HANDLE hThFix = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                                         FALSE, te.th32ThreadID);
              if (hThFix) {
                ApplyAbsoluteThreadPriority(hThFix, absPrio);
                CloseHandle(hThFix);
              }
            }
          }

          // 4. 全 TID の監視 & 新規 TID へのスレッド優先度適用
          // 新仕様 (Plan 107): targetAudioPrio == 0 (Normal) の場合は PID 優先度のままとみなし、
          // TID に一切アタッチしない（SetThreadPriority ループをスキップ）。
          // ワンショットリセット要求 (ピッカー Normal 選択) の処理
          if (!rule.resetPriorityToZeroRequested.empty() &&
              rule.resetPriorityToZeroRequested[0]) {
            if (ptIt != processThreads.end()) {
              for (const auto &te : ptIt->second) {
                HANDLE hTh = OpenThread(THREAD_SET_INFORMATION, FALSE,
                                        te.th32ThreadID);
                if (hTh) {
                  SetThreadPriority(hTh, THREAD_PRIORITY_NORMAL);
                  CloseHandle(hTh);
                }
              }
            }
            rule.audioPriority = 0;
            if (!rule.audioPriorities.empty())
              rule.audioPriorities[0] = 0;
            targetAudioPrio = 0;
            rule.resetPriorityToZeroRequested[0] = false;
            ResetDynamicPriorityFetch(rule, 0);
            if (!rule.actualAudioBasePriorities.empty()) {
              rule.actualAudioBasePriorities[0] = 0;
            }
            if (!rule.actualThreadRelativePrios.empty()) {
              rule.actualThreadRelativePrios[0] = 0;
            }
            stateChanged = true;
          }

          auto &appliedTids = m_audioDgAppliedTids[pid];
          bool isInitialPid = appliedTids.empty();
          bool prioChanged =
              (m_audioDgLastPrio.find(pid) == m_audioDgLastPrio.end() ||
               m_audioDgLastPrio[pid] != targetAudioPrio);

          if (ptIt != processThreads.end()) {
            std::unordered_set<DWORD> aliveTids;
            for (const auto &te : ptIt->second) {
              aliveTids.insert(te.th32ThreadID);
            }
            EraseDeadPidSet(appliedTids, aliveTids);

            for (const auto &te : ptIt->second) {
              DWORD tid = te.th32ThreadID;
              bool isNewTid = (appliedTids.find(tid) == appliedTids.end());
              if (targetAudioPrio != 0 && (isNewTid || prioChanged)) {
                HANDLE hTh = OpenThread(THREAD_SET_INFORMATION |
                                            THREAD_QUERY_INFORMATION,
                                        FALSE, tid);
                if (!hTh) {
                  hTh = OpenThread(THREAD_SET_INFORMATION |
                                       THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, tid);
                }
                if (!hTh) {
                  hTh = OpenThread(THREAD_SET_INFORMATION, FALSE, tid);
                }
                if (hTh) {
                  if (IsFixedThreadPriority(targetAudioPrio)) {
                    ApplyAbsoluteThreadPriority(
                        hTh, DecodeFixedThreadPriority(targetAudioPrio));
                  } else {
                    SetThreadPriority(hTh, targetAudioPrio);
                  }
                  CloseHandle(hTh);
                }
                if (isNewTid && !isInitialPid) {
                  rule.applyCount++;
                  stateChanged = true;
                }
              }
              if (isNewTid) {
                appliedTids.insert(tid);
              }
            }
          }

          if (isInitialPid) {
            rule.applyCount++;
            stateChanged = true;
          }
          if (isInitialPid || prioChanged || procPrioSet) {
            ResetDynamicPriorityFetch(rule, 0);
          }
          m_audioDgLastPrio[pid] = targetAudioPrio;
          // audiodg.exe は PPL 保護のため WalkSystemProcessThreads で ContextSwitches 最上位 (音声本体) の Dyn Pri を取得
          // (確定・変更直後 + 3秒後に1回のみ再取得し完了)
          if (ShouldFetchDynamicPriority(rule, 0)) {
            int bestDp = 0;
            ULONG bestCtxSw = 0;
            DWORD bestTid = 0;
            bool foundAny = false;
            WalkSystemProcessThreads(
                pid, [&](DWORD tid, const SYSTEM_THREAD_INFORMATION_RAW &th) {
                  if (th.Priority > 0) {
                    if (!foundAny || th.ContextSwitches > bestCtxSw) {
                      foundAny = true;
                      bestCtxSw = th.ContextSwitches;
                      bestDp = static_cast<int>(th.Priority);
                      bestTid = tid;
                    }
                  }
                });
            int bestRel = 0;
            if (bestTid != 0) {
              HANDLE hThRel = OpenThread(THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, bestTid);
              if (hThRel) {
                int r = GetThreadPriority(hThRel);
                if (r != THREAD_PRIORITY_ERROR_RETURN) {
                  bestRel = r;
                }
                CloseHandle(hThRel);
              }
            }
            int actualDynPri = bestDp;
            if (actualDynPri <= 0) {
              if (IsFixedThreadPriority(targetAudioPrio)) {
                actualDynPri = DecodeFixedThreadPriority(targetAudioPrio);
              } else {
                DWORD effClass = (rule.processPriorityClass != 0)
                                     ? rule.processPriorityClass
                                     : NORMAL_PRIORITY_CLASS;
                int rank = ati::FindRankByPrio(effClass, targetAudioPrio);
                const auto *entry = ati::GetMatrixEntryByRank(rank);
                actualDynPri =
                    entry ? entry->basePriority : (targetAudioPrio + 8);
              }
            }
            if (rule.actualAudioBasePriorities.empty()) {
              rule.actualAudioBasePriorities.resize(1, 0);
            }
            if (rule.actualThreadRelativePrios.empty()) {
              rule.actualThreadRelativePrios.resize(1, 0);
            }
            rule.actualAudioBasePriorities[0] = actualDynPri;
            rule.actualThreadRelativePrios[0] = bestRel;
            stateChanged = true;
          }
          latestIsolatedPid = pid;
        }
      }
      totalThreadCount = latestPidThreadCount;

      if (latestIsolatedPid != 0) {
        std::string label = "PID " + std::to_string(latestIsolatedPid) + " / -";
        if (!rule.isAudioIsolated || rule.activePid != latestIsolatedPid ||
            rule.detectedThreadName != label) {
          rule.isAudioIsolated = true;
          rule.activePid = latestIsolatedPid;
          rule.activeAudioTid = 0;
          rule.activeAudioTids.clear();
          rule.detectedThreadName = label;
          rule.searchPhase = 0;
          rule.searchTurns = 0;
          stateChanged = true;
        }
      } else {
        std::string failLabel = accessDenied ? "Need Admin" : "Searching...";
        if (rule.isAudioIsolated || rule.detectedThreadName != failLabel) {
          rule.detectedThreadName = failLabel;
          rule.isAudioIsolated = false;
          rule.activeAudioTid = 0;
          rule.activeAudioTids.clear();
          rule.searchPhase = 1;
          rule.searchTurns = 0;
          stateChanged = true;
        }
      }

      if (UpdateDisplayThreadCount(rule, totalThreadCount))
        stateChanged = true;
      if (stateChanged)
        LogRuleStateChange(rule);
      continue;
    }

    // ── Chromium 専用パス: Audio Service のみスレッド走査、他は AffinityMask
    // のみ ──
    if (rule.appType == 2) {
      DWORD audioServicePid = rule.audioServicePid;

      // キャッシュ済み Audio Service PID の生存チェック
      if (audioServicePid != 0) {
        bool alive = false;
        for (DWORD pid : it->second) {
          if (pid == audioServicePid) {
            alive = true;
            break;
          }
        }
        if (!alive) {
          ClearChromiumTrackingState(audioServicePid);
          audioServicePid = 0;
          rule.audioServicePid = 0;
          rule.detectedThreadName = "sleeping...";
          rule.isAudioIsolated = false;
          rule.activeAudioTid = 0;
          rule.chromiumScanAttempted = true;
          stateChanged = true;
          LogDebug(("Chromium AudioService PID expired, returned to " +
                    rule.detectedThreadName)
                       .c_str());
        }
      }

      // Audio Service PID 未特定時: 初動時または 5秒周期のプロセス一覧更新時
      // (shouldScanProcesses) に走査 平常ターンは走査せずコストゼロ化を維持
      if (audioServicePid == 0) {
        if (shouldScanProcesses || !rule.chromiumScanAttempted) {
          rule.chromiumScanAttempted = true;
          for (DWORD pid : it->second) {
            HANDLE hProc = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!hProc)
              continue;

            std::string cmdLine = QueryProcessCommandLine(hProc);
            CloseHandle(hProc);
            if (cmdLine.find("audio.mojom.AudioService") != std::string::npos) {
              audioServicePid = pid;
              rule.audioServicePid = pid;
              m_chromiumEvictedPids.erase(pid);
              m_chromiumThreadTracks.erase(pid);
              m_chromiumAudioStates.erase(pid);
              rule.searchPhase = 1;
              rule.searchTurns = 0;
              rule.activePid = pid;
              rule.detectedThreadName =
                  "PID " + std::to_string(pid) + " / Searching...";
              char asLog[128];
              snprintf(asLog, sizeof(asLog),
                       "Chromium AudioService found: PID=%lu", pid);
              LogDebug(asLog);
              break;
            }
          }
        }
      }

      // 新規出現した子プロセスにのみ normalMask を適用 (差分検出)
      {
        // 死亡PIDのクリーンアップ
        EraseDeadPidSet(m_chromiumMaskedPids, allActivePids);

        for (DWORD pid : it->second) {
          if (pid == audioServicePid)
            continue;
          if (m_chromiumMaskedPids.count(pid))
            continue;
          HANDLE hProc = OpenProcess(
              PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
          if (!hProc)
            continue;
          SetProcessAffinityMask(hProc, normalMask);
          CloseHandle(hProc);
          m_chromiumMaskedPids.insert(pid);
        }
      }

      // Audio Service が特定されていない場合は sleeping... に遷移して待機
      if (audioServicePid == 0) {
        if (rule.detectedThreadName != "sleeping...") {
          rule.detectedThreadName = "sleeping...";
          stateChanged = true;
        }
        rule.searchPhase = 4;
        rule.searchTurns = 0;
        rule.isAudioIsolated = false;
        rule.activeAudioTid = 0;
        if (rule.currentThreadCount != 0) {
          rule.currentThreadCount = 0;
          stateChanged = true;
        }
        continue;
      }

      // Audio Service プロセスのスレッドを走査
      auto ptIt = processThreads.find(audioServicePid);
      if (ptIt == processThreads.end() || ptIt->second.empty()) {
        if (rule.detectedThreadName != "sleeping...") {
          rule.detectedThreadName = "sleeping...";
          stateChanged = true;
        }
        rule.isAudioIsolated = false;
        rule.activeAudioTid = 0;
        if (rule.currentThreadCount != 0) {
          rule.currentThreadCount = 0;
          stateChanged = true;
        }
        continue;
      }

      HANDLE hAsProc = OpenProcess(
          PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
          FALSE, audioServicePid);
      if (!hAsProc) {
        if (rule.detectedThreadName != "sleeping...") {
          rule.detectedThreadName = "sleeping...";
          stateChanged = true;
        }
        rule.isAudioIsolated = false;
        rule.activeAudioTid = 0;
        continue;
      }

      // 親プロセスのアフィニティを拡張
      DWORD_PTR currentProcMask = 0, sysMask = 0;
      if (GetProcessAffinityMask(hAsProc, &currentProcMask, &sysMask)) {
        if ((currentProcMask & parentMask) != parentMask) {
          SetProcessAffinityMask(hAsProc, parentMask);
        }
      }

      // Audio Service プロセスに対して HighQoS (Power Throttling / EcoQoS 解除)
      // を新規検出PIDに初回1回のみ適用 (親プロセスの HighQoS
      // 状態はカーネルの自動継承機構により配下の全オーディオスレッドに波及)
      if (m_highQosAppliedPids.insert(audioServicePid).second) {
        ApplyProcessHighQoS(hAsProc);
      }

      totalThreadCount = static_cast<int>(ptIt->second.size());
      auto &tracks = m_chromiumThreadTracks[audioServicePid];

      // 手順 1: 初期スレッド登録
      // (cycles数上位1位・2位比較によりTEB最若を親呼出制御スレッドとして flag=1
      // 除外)
      bool hasFlag1 = false;
      for (const auto &pair : tracks) {
        if (pair.second.flag == 1) {
          hasFlag1 = true;
          break;
        }
      }

      if (!hasFlag1) {
        struct ThreadCycleInfo {
          DWORD tid = 0;
          ULONG64 cycle = 0;
          PVOID teb = nullptr;
        };
        std::vector<ThreadCycleInfo> cycleInfos;

        for (const auto &te : ptIt->second) {
          HANDLE hThread =
              OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          if (hThread) {
            ULONG64 cycle = 0;
            QueryThreadCycleTime(hThread, &cycle);
            PVOID teb = QueryThreadTebBaseAddress(hThread);

            if (tracks.find(te.th32ThreadID) == tracks.end()) {
              tracks[te.th32ThreadID].flag = 0;
            }
            tracks[te.th32ThreadID].lastCycles = cycle;
            tracks[te.th32ThreadID].lastDelta = 0;
            cycleInfos.push_back({te.th32ThreadID, cycle, teb});
            CloseHandle(hThread);
          }
        }

        // cycles数 (総サイクル数) 降順でソート
        std::sort(cycleInfos.begin(), cycleInfos.end(),
                  [](const ThreadCycleInfo &a, const ThreadCycleInfo &b) {
                    return a.cycle > b.cycle;
                  });

        // cycles数 降順 1 位のスレッドを親制御スレッド (flag = 1)
        // として確定し、Excluded (normalMask) を適用
        if (!cycleInfos.empty()) {
          DWORD ctrlTid = cycleInfos[0].tid;
          tracks[ctrlTid].flag = 1;
          m_chromiumEvictedPids.insert(audioServicePid);

          // Excluded CPU# (normalMask) を親制御スレッドに適用
          HANDLE hCtrl =
              OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                         FALSE, ctrlTid);
          if (hCtrl) {
            if (EvacuateNormalThreadSettings(hCtrl, ctrlTid, normalMask)) {
              stateChanged = true;
            }
            CloseHandle(hCtrl);
          }

          char mLog[160];
          snprintf(
              mLog, sizeof(mLog),
              "Chromium parent control thread marked as non-audio and isolated "
              "to Excluded (normalMask): PID=%lu, TID=%lu (Cycles=%llu)",
              audioServicePid, ctrlTid,
              static_cast<unsigned long long>(cycleInfos[0].cycle));
          LogDebug(mLog);
        }
      }

      // 手順 2: 新探索フロー & 確定後の全 flag=0 定常監視・一位争奪
      auto &audioState = m_chromiumAudioStates[audioServicePid];
      int activeAudioCount = 0;
      DWORD lastAudioTid = 0;

      // 生存スレッドの TID セットを構築し、OS上で終了したスレッドのみ tracks
      // から削除
      std::unordered_set<DWORD> aliveTids;
      for (const auto &te : ptIt->second) {
        aliveTids.insert(te.th32ThreadID);
      }
      for (auto itTr = tracks.begin(); itTr != tracks.end();) {
        if (aliveTids.find(itTr->first) == aliveTids.end()) {
          itTr = tracks.erase(itTr);
        } else {
          ++itTr;
        }
      }

      // 新スレッド (初登場) を検知した場合、サイクルベースラインを記録
      // (無条件に flag = 0 候補)
      for (const auto &te : ptIt->second) {
        if (tracks.find(te.th32ThreadID) == tracks.end()) {
          HANDLE hTh =
              OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          if (hTh) {
            ULONG64 curC = 0;
            QueryThreadCycleTime(hTh, &curC);
            tracks[te.th32ThreadID].flag = 0;
            tracks[te.th32ThreadID].lastCycles = curC;
            tracks[te.th32ThreadID].lastDelta = 0;
            CloseHandle(hTh);
          }
        }
      }

      // オーディオ確定・隔離ヘルパー:
      // 確定スレッドに audioMask を適用。他スレッドは normalMask へ退避
      // (旧オーディオスレッドは放置)
      auto applyAudioIsolation = [&](DWORD audioTid) {
        DWORD oldTid = rule.activeAudioTid;
        if (rule.isAudioIsolated && oldTid != 0 && oldTid != audioTid) {
          if (tracks.find(oldTid) != tracks.end()) {
            tracks[oldTid].flag = 0;
          }
        }
        tracks[audioTid].flag = 2;
        HANDLE hTh = OpenThread(
            THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, audioTid);
        if (hTh) {
          int assignedCore = rule.audioCore;
          DWORD_PTR assignedMask = audioMask;
          if (!rule.audioCores.empty()) {
            assignedCore = rule.audioCores[0];
            assignedMask = (1ULL << assignedCore);
          }
          int assignedPrio = (!rule.audioPriorities.empty())
                                 ? rule.audioPriorities[0]
                                 : rule.audioPriority;
          if (ApplyAudioThreadSettings(hTh, audioTid, assignedPrio,
                                       assignedCore, assignedMask, rule)) {
            stateChanged = true;
          }
          CloseHandle(hTh);
        }
        for (const auto &te : ptIt->second) {
          if (te.th32ThreadID != audioTid &&
              tracks[te.th32ThreadID].flag == 0) {
            // flag = 0 のスレッドのみ normalMask へ退避 (flag = 1
            // 除外スレッドは変更しない)
            HANDLE hOther =
                OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                           FALSE, te.th32ThreadID);
            if (hOther) {
              if (EvacuateNormalThreadSettings(hOther, te.th32ThreadID,
                                               normalMask)) {
                stateChanged = true;
              }
              CloseHandle(hOther);
            }
          }
        }
        activeAudioCount = 1;
        lastAudioTid = audioTid;
        if (rule.activeAudioTid != audioTid) {
          ResetDynamicPriorityFetch(rule, -1);
        }
        rule.isAudioIsolated = true;
        rule.activePid = audioServicePid;
        rule.activeAudioTid = audioTid;
        rule.activeAudioTids = {audioTid};
        rule.detectedThreadName = "PID " + std::to_string(audioServicePid) +
                                  " / TID " + std::to_string(audioTid);
        m_trackedAudioThreads[audioServicePid][audioTid] =
            rule.detectedThreadName;
        rule.searchPhase = 0;
        rule.searchTurns = 0;
        audioState.initialEvaluated = true;
        audioState.samplingActive = false;
        audioState.step = 2;
        audioState.d.clear();
        stateChanged = true;
      };

      // ── Step 2: 待機ループ (全 flag == 0 スレッドの Delta 監視) ──
      if (audioState.step == 2 || audioState.step == 1) {
        audioState.step = 2;
        int normalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
        ULONG64 baseDelta = CalculateDeltaThreshold(
            rule.cyclesDelta, m_config.defaultCyclesDelta, 0);
        ULONG64 minDelta =
            (baseDelta / 5ULL < 1000ULL) ? 1000ULL : (baseDelta / 5ULL);
        ULONG64 step2Threshold = CalculateDeltaThreshold(
            rule.cyclesDelta, m_config.defaultCyclesDelta, normalMs);
        if (step2Threshold < minDelta)
          step2Threshold = minDelta;

        bool triggerSampling = false;

        // flag == 0 の全スレッドの Delta を常時計測
        for (const auto &te : ptIt->second) {
          if (tracks[te.th32ThreadID].flag == 0) {
            HANDLE hTh =
                OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hTh) {
              ULONG64 curC = 0;
              if (QueryThreadCycleTime(hTh, &curC)) {
                if (tracks[te.th32ThreadID].lastCycles > 0 &&
                    curC >= tracks[te.th32ThreadID].lastCycles) {
                  ULONG64 delta = curC - tracks[te.th32ThreadID].lastCycles;
                  tracks[te.th32ThreadID].lastDelta = delta;

                  // 初回・確定後ともに defaultcyclesdelta
                  // 以上の活動検知でサンプリング開始
                  if (delta >= step2Threshold) {
                    triggerSampling = true;
                  }
                }
                tracks[te.th32ThreadID].lastCycles = curC;
              }
              CloseHandle(hTh);
            }
          }
        }

        // 現 flag == 2 スレッドの lastCycles も更新しておく
        if (rule.isAudioIsolated && rule.activeAudioTid != 0) {
          HANDLE hCur =
              OpenThread(THREAD_QUERY_INFORMATION, FALSE, rule.activeAudioTid);
          if (hCur) {
            ULONG64 curC = 0;
            if (QueryThreadCycleTime(hCur, &curC)) {
              if (tracks[rule.activeAudioTid].lastCycles > 0 &&
                  curC >= tracks[rule.activeAudioTid].lastCycles) {
                tracks[rule.activeAudioTid].lastDelta =
                    curC - tracks[rule.activeAudioTid].lastCycles;
              }
              tracks[rule.activeAudioTid].lastCycles = curC;
            }
            CloseHandle(hCur);
          }
        }

        if (triggerSampling) {
          // flag != 1 (flag == 0 および現 flag == 2) のスレッドで配列 d
          // を構築し一位争奪サンプリングへ
          audioState.d.clear();
          std::vector<DWORD> candidateTids;
          for (const auto &te : ptIt->second) {
            if (tracks[te.th32ThreadID].flag != 1) { // 0 or 2
              candidateTids.push_back(te.th32ThreadID);
            }
          }
          if (rule.isAudioIsolated && rule.activeAudioTid != 0) {
            if (std::find(candidateTids.begin(), candidateTids.end(),
                          rule.activeAudioTid) == candidateTids.end()) {
              candidateTids.push_back(rule.activeAudioTid);
            }
          }
          std::sort(candidateTids.begin(), candidateTids.end());

          for (DWORD t : candidateTids) {
            ThreadSamplingEntry entry;
            entry.tid = t;
            memset(entry.deltas, 0, sizeof(entry.deltas));
            entry.deltas[0] = 0; // 0: 生存, -1: 脱落
            entry.lastCycle = tracks[t].lastCycles;
            audioState.d.push_back(entry);
          }

          audioState.k = 1;
          audioState.samplingActive = true;
          audioState.isDecline = rule.isDeclineBoost;
          audioState.step = 3;
        }
        if (!rule.isAudioIsolated) {
          std::string searchLabel =
              "PID " + std::to_string(audioServicePid) + " / Searching...";
          if (rule.detectedThreadName != searchLabel) {
            rule.detectedThreadName = searchLabel;
            stateChanged = true;
          }
        }
      }
      // ── Step 3: ポーリング 1/4 時間間隔 (125ms) での 3 回 Delta 計測 &
      // 一位確定 ── 最新 4 回の計測（起点 + 3回）における 3 つの Delta
      // の合計値が最も高いものを flag = 2 とする
      else if (audioState.step == 3) {
        // k 回目 (1..3) の計測
        for (auto &entry : audioState.d) {
          if (entry.deltas[0] == -1) {
            continue; // 既に脱落マーク済み
          }

          if (aliveTids.find(entry.tid) == aliveTids.end()) {
            entry.deltas[0] = -1; // スレッド消滅
            continue;
          }

          HANDLE hTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.tid);
          if (!hTh) {
            entry.deltas[0] = -1;
            continue;
          }

          ULONG64 curC = 0;
          if (QueryThreadCycleTime(hTh, &curC)) {
            if (entry.lastCycle > 0 && curC >= entry.lastCycle) {
              ULONG64 delta = curC - entry.lastCycle;
              if (audioState.k >= 1 && audioState.k <= 3) {
                entry.deltas[audioState.k] = static_cast<int64_t>(delta);
              }
              if (delta == 0) {
                entry.deltas[0] = -1; // Delta=0 で脱落マーク
              }
            } else {
              if (audioState.k >= 1 && audioState.k <= 3) {
                entry.deltas[audioState.k] = 0;
              }
              entry.deltas[0] = -1; // サイクル未増加で脱落マーク
            }
            entry.lastCycle = curC;
          } else {
            entry.deltas[0] = -1;
          }
          CloseHandle(hTh);
        }

        if (audioState.k < 3) {
          audioState.k++;
        } else {
          // 3 回完了 (最新 4 計測による 3 つの Delta 蓄積完了): 一位決定！
          DWORD bestTid = 0;
          int64_t maxTotalDelta = 0;

          for (const auto &entry : audioState.d) {
            if (entry.deltas[0] == -1)
              continue; // 脱落スレッドを除外

            int64_t totalDelta = 0;
            for (int i = 1; i <= 3; ++i) {
              if (entry.deltas[i] > 0) {
                totalDelta += entry.deltas[i];
              }
            }

            if (totalDelta > maxTotalDelta) {
              maxTotalDelta = totalDelta;
              bestTid = entry.tid;
            }
          }

          if (bestTid != 0 && maxTotalDelta > 0) {
            // 3 つの Delta 合計最大のスレッドを無条件に flag = 2 として採用
            applyAudioIsolation(bestTid);
          } else {
            // 候補なし (全員脱落または0) ➔ WaitLoop (Step 2)
            audioState.samplingActive = false;
            audioState.step = 2;
            audioState.d.clear();
            if (rule.isAudioIsolated) {
              std::string label = "PID " + std::to_string(audioServicePid) +
                                  " / TID " +
                                  std::to_string(rule.activeAudioTid);
              if (rule.detectedThreadName != label) {
                rule.detectedThreadName = label;
                stateChanged = true;
              }
            } else {
              std::string searchLabel =
                  "PID " + std::to_string(audioServicePid) + " / Searching...";
              if (rule.detectedThreadName != searchLabel) {
                rule.detectedThreadName = searchLabel;
                stateChanged = true;
              }
            }
          }
        }
      }

      // 生存しているオーディオ確定スレッド (flag == 2) の確認 &
      // 設定維持・即時反映
      size_t chromiumAudioSlot = 0;
      for (const auto &te : ptIt->second) {
        auto tIt = tracks.find(te.th32ThreadID);
        if (tIt != tracks.end()) {
          if (tIt->second.flag == 2) {
            activeAudioCount++;
            lastAudioTid = te.th32ThreadID;

            int assignedCore = rule.audioCore;
            DWORD_PTR assignedMask = audioMask;
            if (chromiumAudioSlot < rule.audioCores.size()) {
              assignedCore = rule.audioCores[chromiumAudioSlot];
              assignedMask = (1ULL << assignedCore);
            } else if (audioMask != 0) {
              int bitIdx = -1;
              int foundCount = 0;
              for (int c = 0; c < 64; ++c) {
                if ((audioMask & (1ULL << c)) != 0) {
                  if (foundCount == static_cast<int>(chromiumAudioSlot)) {
                    bitIdx = c;
                    break;
                  }
                  foundCount++;
                }
              }
              if (bitIdx >= 0) {
                assignedCore = bitIdx;
                assignedMask = (1ULL << bitIdx);
              }
            }
            size_t slotIdx = chromiumAudioSlot;
            chromiumAudioSlot++;

            // 共通ルーチンで優先度・アフィニティを常時検証・即時反映
            HANDLE hTh =
                OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                           FALSE, te.th32ThreadID);
            if (hTh) {
              // ワンショットリセット要求 (長押し / ピッカー Normal 選択) の処理
              if (slotIdx < rule.resetPriorityToZeroRequested.size() &&
                  rule.resetPriorityToZeroRequested[slotIdx]) {
                SetThreadPriority(hTh, THREAD_PRIORITY_NORMAL);
                rule.resetPriorityToZeroRequested[slotIdx] = false;
                if (slotIdx < rule.audioPriorities.size()) {
                  rule.audioPriorities[slotIdx] = 0;
                }
                if (slotIdx == 0) {
                  rule.audioPriority = 0;
                }
                ResetDynamicPriorityFetch(rule, static_cast<int>(slotIdx));
                if (slotIdx < rule.actualAudioBasePriorities.size()) {
                  rule.actualAudioBasePriorities[slotIdx] = 0;
                }
                stateChanged = true;
              }

              int assignedPrio = (slotIdx < rule.audioPriorities.size())
                                     ? rule.audioPriorities[slotIdx]
                                     : rule.audioPriority;
              if (ApplyAudioThreadSettings(hTh, te.th32ThreadID, assignedPrio,
                                           assignedCore, assignedMask, rule)) {
                stateChanged = true;
              }

              // 実測 Dynamic Priority (Dyn Pri) の更新 (確定・変更直後 + 3秒後に1回のみ再取得し完了)
              if (ShouldFetchDynamicPriority(rule, slotIdx)) {
                int curRel = GetThreadPriority(hTh);
                int actualDynPri = QueryThreadDynamicPriority(hTh);
                if (actualDynPri <= 0) {
                  DWORD effClass = (rule.processPriorityClass != 0)
                                       ? rule.processPriorityClass
                                       : NORMAL_PRIORITY_CLASS;
                  int rank = ati::FindRankByPrio(effClass, curRel);
                  const auto *entry = ati::GetMatrixEntryByRank(rank);
                  actualDynPri = entry ? entry->basePriority : (curRel + 8);
                }
                if (rule.actualAudioBasePriorities.size() <= slotIdx) {
                  rule.actualAudioBasePriorities.resize(slotIdx + 1, 0);
                }
                if (rule.actualThreadRelativePrios.size() <= slotIdx) {
                  rule.actualThreadRelativePrios.resize(slotIdx + 1, 0);
                }
                rule.actualAudioBasePriorities[slotIdx] = actualDynPri;
                rule.actualThreadRelativePrios[slotIdx] = curRel;
                stateChanged = true;
              }
              CloseHandle(hTh);
            }
          } else if (tIt->second.flag != 2) {
            // 非オーディオスレッド (flag == 0, 1) の退避維持
            HANDLE hOther =
                OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                           FALSE, te.th32ThreadID);
            if (hOther) {
              if (EvacuateNormalThreadSettings(hOther, te.th32ThreadID,
                                               normalMask)) {
                stateChanged = true;
              }
              CloseHandle(hOther);
            }
          }
        }
      }

      if (rule.isAudioIsolated && activeAudioCount == 0) {
        // 確定済みオーディオスレッド消滅: 再探索へ遷移
        rule.isAudioIsolated = false;
        rule.activeAudioTid = 0;
        rule.activeAudioTids.clear();
        audioState.step = 2;
        audioState.samplingActive = false;
        audioState.d.clear();
        std::string searchLabel =
            "PID " + std::to_string(audioServicePid) + " / Searching...";
        if (rule.detectedThreadName != searchLabel) {
          rule.detectedThreadName = searchLabel;
          stateChanged = true;
        }
      } else if (!rule.isAudioIsolated) {
        std::string searchLabel =
            "PID " + std::to_string(audioServicePid) + " / Searching...";
        if (rule.detectedThreadName != searchLabel) {
          rule.detectedThreadName = searchLabel;
          stateChanged = true;
        }
      }
      if (activeAudioCount > 0) {
        if (rule.activePid != audioServicePid) {
          rule.activePid = audioServicePid;
          stateChanged = true;
        }
        if (rule.activeAudioTid != lastAudioTid) {
          rule.activeAudioTid = lastAudioTid;
          stateChanged = true;
        }
        std::string label = "PID " + std::to_string(audioServicePid) +
                            " / TID " + std::to_string(lastAudioTid);
        if (rule.detectedThreadName != label) {
          rule.detectedThreadName = label;
          stateChanged = true;
        }

        // プロセス優先度クラスの動的適用 (Chromium Audio Service PID: Col 3 明示変更時のみ SetPriorityClass)
        DWORD curPrioClass = GetPriorityClass(hAsProc);
        if (rule.processPriorityModified && rule.processPriorityClass != 0) {
          if (curPrioClass != 0 && curPrioClass != rule.processPriorityClass) {
            bool procPrioSet = false;
            if (SetPriorityClass(hAsProc, rule.processPriorityClass)) {
              char pLog[128];
              snprintf(pLog, sizeof(pLog),
                       "SetPriorityClass applied to Chromium Audio Service: "
                       "PID=%lu, class=0x%lX",
                       audioServicePid, rule.processPriorityClass);
              LogDebug(pLog);
              procPrioSet = true;
              stateChanged = true;
            } else if (rule.processPriorityClass == REALTIME_PRIORITY_CLASS) {
              if (SetPriorityClass(hAsProc, HIGH_PRIORITY_CLASS)) {
                LogDebug("SetPriorityClass fallback to High applied to "
                         "Chromium Audio Service");
                procPrioSet = true;
                stateChanged = true;
              }
            }
            if (procPrioSet) {
              curPrioClass = GetPriorityClass(hAsProc);
              ResetDynamicPriorityFetch(rule, -1);
              for (size_t s = 0; s < rule.activeAudioTids.size(); ++s) {
                DWORD aTid = rule.activeAudioTids[s];
                if (aTid == 0) continue;
                int sPrio = (s < rule.audioPriorities.size()) ? rule.audioPriorities[s] : rule.audioPriority;
                if (IsFixedThreadPriority(sPrio)) {
                  HANDLE hThFix = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, aTid);
                  if (hThFix) {
                    ApplyAbsoluteThreadPriority(hThFix, DecodeFixedThreadPriority(sPrio));
                    CloseHandle(hThFix);
                  }
                }
              }
            }
          }
        }
        if (curPrioClass != 0) {
          if (rule.actualProcessPriorityClass != curPrioClass) {
            rule.actualProcessPriorityClass = curPrioClass;
            stateChanged = true;
          }
          if (!rule.processPriorityModified) {
            rule.processPriorityClass = curPrioClass;
          }
        }
      }

      CloseHandle(hAsProc);

      if (UpdateDisplayThreadCount(rule, totalThreadCount))
        stateChanged = true;
      if (stateChanged)
        LogRuleStateChange(rule);
      continue;
    }

    // ── 既存パス（Firefox, foobar2000, ゲーム等）──
    bool wasIsolatedBeforeScan = rule.isAudioIsolated;

    // 死亡PIDのクリーンアップ
    EraseDeadPidSet(m_nonChromiumMaskedPids, allActivePids);

    // 同名プロセスの全 PID (マルチプロセス) を走査
    for (DWORD pid : it->second) {
      auto ptIt = processThreads.find(pid);
      if (ptIt == processThreads.end() || ptIt->second.empty())
        continue;

      HANDLE hProcess = OpenProcess(
          PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
          FALSE, pid);
      if (!hProcess) {
        hProcess = OpenProcess(PROCESS_SET_INFORMATION |
                                   PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, pid);
      }
      if (!hProcess) {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "OpenProcess FAILED: pid=%lu, err=%lu",
                 pid, GetLastError());
        LogDebug(errBuf);
        continue;
      }

      // アイドル優先度クラス (IDLE_PRIORITY_CLASS) のプロセスはスキップ
      // (休眠タブ走査負荷の削減: ブラウザ系のみ)
      if (rule.processPriorityClass != IDLE_PRIORITY_CLASS) {
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

      // 親プロセスの Affinity Mask を拡張 (新規出現PIDに対して1度だけ実行)
      if (m_nonChromiumMaskedPids.find(pid) == m_nonChromiumMaskedPids.end()) {
        DWORD_PTR currentProcMask = 0, sysMask = 0;
        if (GetProcessAffinityMask(hProcess, &currentProcMask, &sysMask)) {
          if ((currentProcMask & parentMask) != parentMask) {
            SetProcessAffinityMask(hProcess, parentMask);
          }
        }
        m_nonChromiumMaskedPids.insert(pid);
      }

      // 全登録プロセスに対して HighQoS (Power Throttling / EcoQoS 解除)
      // を新規検出PIDに初回1回のみ適用 (親プロセスの HighQoS
      // 状態はカーネルの自動継承機構により配下の全スレッドに波及)
      if (m_highQosAppliedPids.insert(pid).second) {
        ApplyProcessHighQoS(hProcess);
      }

      std::unordered_set<DWORD> aliveTids;
      std::vector<LocalThreadInfo> threadInfos;

      DWORD identifiedAudioTid = 0;
      std::string identifiedAudioLabel = "";

      // 前回セッション／キャッシュ済みPID/TIDトラッキングから事前解決
      // (ループ前にオーディオTIDを確定させ、重い走査をスキップ)
      {
        auto trackPidIt = m_trackedAudioThreads.find(pid);
        if (trackPidIt != m_trackedAudioThreads.end() &&
            !trackPidIt->second.empty()) {
          if (rule.activeAudioTids.empty() && !IsMultiSlotRule(rule)) {
            for (const auto &pair : trackPidIt->second) {
              if (pair.first != 0) {
                rule.activeAudioTids.push_back(pair.first);
              }
            }
          }
          for (DWORD at : rule.activeAudioTids) {
            if (at != 0) {
              identifiedAudioTid = at;
              auto lblIt = trackPidIt->second.find(at);
              if (lblIt != trackPidIt->second.end()) {
                identifiedAudioLabel = lblIt->second;
              }
              break;
            }
          }
        }
      }

      bool isMultiSlot = IsMultiSlotRule(rule);
      size_t targetCount = GetRuleSlotCount(rule);

      // スレッド増減・構成変化の判定
      std::vector<DWORD> currentTids;
      currentTids.reserve(ptIt->second.size());
      for (const auto &te : ptIt->second) {
        currentTids.push_back(te.th32ThreadID);
      }

      bool threadsChanged = true;
      auto cntIt = m_lastProcessThreadCount.find(pid);
      auto cacheIt = m_cachedProcessTids.find(pid);
      if (cntIt != m_lastProcessThreadCount.end() &&
          cntIt->second == static_cast<int>(currentTids.size()) &&
          cacheIt != m_cachedProcessTids.end() &&
          cacheIt->second == currentTids) {
        threadsChanged = false;
      }

      // オーディオ確定スロットがすべて現在の生存スレッドに含まれているか判定
      bool allSlotsReady = false;
      if (rule.isAudioIsolated) {
        std::unordered_set<DWORD> curTidSet(currentTids.begin(), currentTids.end());
        if (isMultiSlot) {
          size_t satisfied = 0;
          for (size_t s = 0; s < targetCount; ++s) {
            bool bypassed = (s < rule.slotBypassed.size() && rule.slotBypassed[s]);
            bool hasAliveTid = (s < rule.activeAudioTids.size() &&
                                rule.activeAudioTids[s] != 0 &&
                                curTidSet.count(rule.activeAudioTids[s]) > 0);
            if (bypassed || hasAliveTid) satisfied++;
          }
          allSlotsReady = (satisfied >= targetCount);
        } else {
          allSlotsReady = (rule.activeAudioTid != 0 &&
                           curTidSet.count(rule.activeAudioTid) > 0);
        }
      }

      // AppType == 1 のみ: NtGetNextThread 走査時に同時取得した AffinityMask を参照し、
      // 同一 PID の非オーディオスレッドがオーディオコア群に同居しているかを直接判定
      auto affMapIt = processThreadAffinities.find(pid);
      bool anyOtherThreadOnAudioCore = false;
      if (rule.appType == 1 && allSlotsReady) {
        DWORD_PTR checkAudioMask = isMultiSlot ? 0 : audioMask;
        if (isMultiSlot) {
          for (size_t s = 0; s < targetCount; ++s) {
            if (s < rule.slotBypassed.size() && rule.slotBypassed[s])
              continue;
            if (s < rule.audioSlotMasks.size() && rule.audioSlotMasks[s] != 0) {
              checkAudioMask |= rule.audioSlotMasks[s];
            } else if (s < rule.audioCores.size() && rule.audioCores[s] >= 0 &&
                       rule.audioCores[s] < 64) {
              checkAudioMask |= (1ULL << rule.audioCores[s]);
            }
          }
        } else {
          for (int c : rule.audioCores) {
            if (c >= 0 && c < 64)
              checkAudioMask |= (1ULL << c);
          }
        }

        if (checkAudioMask != 0) {
          std::unordered_set<DWORD> audioCheckSet;
          for (size_t s = 0; s < rule.activeAudioTids.size(); ++s) {
            if (rule.activeAudioTids[s] != 0) {
              audioCheckSet.insert(rule.activeAudioTids[s]);
            }
          }
          if (audioCheckSet.empty() && identifiedAudioTid != 0) {
            audioCheckSet.insert(identifiedAudioTid);
          }

          for (const auto &te : ptIt->second) {
            if (audioCheckSet.count(te.th32ThreadID) > 0)
              continue;
            DWORD_PTR tAff = 0;
            if (affMapIt != processThreadAffinities.end()) {
              auto tAffIt = affMapIt->second.find(te.th32ThreadID);
              if (tAffIt != affMapIt->second.end()) {
                tAff = tAffIt->second;
              }
            }
            if (tAff == 0) {
              HANDLE hCheck =
                  OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
              if (hCheck) {
                tAff = QueryThreadAffinityMask(hCheck);
                CloseHandle(hCheck);
              }
            }
            if ((tAff & checkAudioMask) != 0) {
              anyOtherThreadOnAudioCore = true;
              break;
            }
          }
        }
      }

      bool canSkipNormalThreads =
          (!threadsChanged && allSlotsReady &&
           !(rule.appType == 1 &&
             (rule.hasIntruderThreads || anyOtherThreadOnAudioCore)));

      // 1. スレッド情報収集 (定常時は通常スレッドの走査をスキップし、確定オーディオスレッドのみ最小限維持)
      if (canSkipNormalThreads) {
        totalThreadCount += static_cast<int>(currentTids.size());
        for (DWORD t : currentTids) {
          aliveTids.insert(t);
        }

        std::unordered_set<DWORD> audioTargetTids;
        for (DWORD at : rule.activeAudioTids) {
          if (at != 0 && aliveTids.count(at) > 0) {
            audioTargetTids.insert(at);
          }
        }
        if (audioTargetTids.empty() && identifiedAudioTid != 0 &&
            aliveTids.count(identifiedAudioTid) > 0) {
          audioTargetTids.insert(identifiedAudioTid);
        }

        for (DWORD at : audioTargetTids) {
          HANDLE hThread =
              OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                         FALSE, at);
          if (hThread) {
            int priority = GetThreadPriority(hThread);
            DWORD_PTR currentAff = 0;
            if (affMapIt != processThreadAffinities.end()) {
              auto tAffIt = affMapIt->second.find(at);
              if (tAffIt != affMapIt->second.end()) {
                currentAff = tAffIt->second;
              }
            }
            if (currentAff == 0) {
              currentAff = QueryThreadAffinityMask(hThread);
            }
            threadInfos.push_back({at, 0, priority, "", hThread, currentAff});
          }
        }
      } else {
        for (const auto &te : ptIt->second) {
          totalThreadCount++;
          aliveTids.insert(te.th32ThreadID);

          HANDLE hThread =
              OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                         FALSE, te.th32ThreadID);
          if (!hThread)
            continue;

          int priority = GetThreadPriority(hThread);
          DWORD_PTR currentAff = 0;
          if (affMapIt != processThreadAffinities.end()) {
            auto tAffIt = affMapIt->second.find(te.th32ThreadID);
            if (tAffIt != affMapIt->second.end()) {
              currentAff = tAffIt->second;
            }
          }
          if (currentAff == 0) {
            currentAff = QueryThreadAffinityMask(hThread);
          }

          threadInfos.push_back(
              {te.th32ThreadID, te.tpBasePri, priority, "", hThread, currentAff});

          // (A) 検出済みPID/TIDトラッキングチェック
          auto trackPidIt = m_trackedAudioThreads.find(pid);
          if (trackPidIt != m_trackedAudioThreads.end()) {
            auto tIt = trackPidIt->second.find(te.th32ThreadID);
            if (tIt != trackPidIt->second.end()) {
              if (identifiedAudioTid == 0) {
                identifiedAudioTid = te.th32ThreadID;
                identifiedAudioLabel = tIt->second;
              }
            }
          }
        }
      }

      // 消滅したTIDを activeAudioTids / suspendedAudioTids から即座に除去
      if (isMultiSlot) {
        bool anyAlive = false;
        for (DWORD &at : rule.activeAudioTids) {
          if (at != 0) {
            if (aliveTids.find(at) == aliveTids.end()) {
              at = 0;
            } else {
              anyAlive = true;
            }
          }
        }
        if (!anyAlive) {
          rule.activeAudioTids.clear();
        }
      } else {
        for (auto itAct = rule.activeAudioTids.begin();
             itAct != rule.activeAudioTids.end();) {
          if (*itAct == 0 || aliveTids.find(*itAct) == aliveTids.end()) {
            itAct = rule.activeAudioTids.erase(itAct);
          } else {
            ++itAct;
          }
        }
      }
      for (auto itSus = rule.suspendedAudioTids.begin();
           itSus != rule.suspendedAudioTids.end();) {
        if (aliveTids.find(*itSus) == aliveTids.end()) {
          itSus = rule.suspendedAudioTids.erase(itSus);
        } else {
          ++itSus;
        }
      }
      rule.isAudioThreadSuspended = !rule.suspendedAudioTids.empty();
      if (identifiedAudioTid != 0 &&
          aliveTids.find(identifiedAudioTid) == aliveTids.end()) {
        identifiedAudioTid = 0;
        identifiedAudioLabel = "";
        for (DWORD at : rule.activeAudioTids) {
          if (at != 0) {
            identifiedAudioTid = at;
            identifiedAudioLabel =
                "PID " + std::to_string(pid) + " / TID " + std::to_string(at);
            break;
          }
        }
      }

      // 検出済みPID/TIDトラッキングから生きているオーディオスレッドを収集
      std::vector<DWORD> trackedActiveTids;
      auto trackPidIt = m_trackedAudioThreads.find(pid);
      if (trackPidIt != m_trackedAudioThreads.end()) {
        for (const auto &te : ptIt->second) {
          if (trackPidIt->second.find(te.th32ThreadID) !=
              trackPidIt->second.end()) {
            trackedActiveTids.push_back(te.th32ThreadID);
          }
        }
      }
      if (!trackedActiveTids.empty() && rule.activeAudioTids.empty() &&
          !isMultiSlot) {
        rule.activeAudioTids = trackedActiveTids;
        if (identifiedAudioTid == 0) {
          identifiedAudioTid = trackedActiveTids[0];
          identifiedAudioLabel = "PID " + std::to_string(pid) + " / TID " +
                                 std::to_string(identifiedAudioTid);
        }
      }

      auto countSatisfied = [&rule](size_t totalSlots) -> size_t {
        size_t n = 0;
        for (size_t s = 0; s < totalSlots; ++s) {
          bool bypassed = (s < rule.slotBypassed.size() && rule.slotBypassed[s]);
          bool hasTid =
              (s < rule.activeAudioTids.size() && rule.activeAudioTids[s] != 0);
          if (bypassed || hasTid) {
            ++n;
          }
        }
        return n;
      };

      // 2. スレッド探索フロー
      auto &sampleState = m_samplingStates[pid];
      bool scannedCyclesInTurn = false;

      if (isMultiSlot) {
        // === 複数スロットモード (AppType: 5 Voicemeeter 2スロット):
        // 未確定かつ非Bypassスロットが存在する間のみ10秒間カウントダウン探索 ===
        size_t curCount = countSatisfied(targetCount);
        if (curCount < targetCount) {
          // 新たなスレッドが特定された場合:
          // 次のスレッド探索のためタイマーを10秒にリセット
          if (curCount > rule.lastIdentifiedCount) {
            rule.multiSearchTurns = 10 * turnsPerSec;
            rule.lastIdentifiedCount = curCount;
            rule.multiSearchSleeping = false;
          }

          if (!rule.multiSearchSleeping) {
            if (rule.multiSearchTurns == 0) {
              rule.multiSearchTurns = 10 * turnsPerSec;
              rule.lastIdentifiedCount = curCount;
            }

            // 10秒間継続スキャン (インターバル休止なしで毎ターン計測・判定)
            scannedCyclesInTurn = true;
            TryIdentifyAudioThread(rule, sampleState, threadInfos, hProcess,
                                   pid, audioMask, intervalMs, "Scanning...",
                                   identifiedAudioTid, identifiedAudioLabel,
                                   stateChanged);

            size_t afterCount = countSatisfied(targetCount);
            if (afterCount > curCount) {
              // 新スレッド検出成功！
              rule.lastIdentifiedCount = afterCount;
              rule.multiSearchTurns =
                  10 * turnsPerSec; // 次のスレッドのため再カウントダウン
              if (afterCount >= targetCount) {
                rule.multiSearchTurns = 0;
                rule.multiSearchSleeping = false;
              }
            } else {
              // 未検出: カウントダウン進行
              if (rule.multiSearchTurns > 0) {
                rule.multiSearchTurns--;
              }
              if (rule.multiSearchTurns == 0) {
                // 10秒経過しても未確定スロットが残る場合は休眠フラグへ移行
                rule.multiSearchSleeping = true;
              }
            }
          }
        } else {
          // 全非Bypassスロットが確定済み: 定常時の探索負荷をゼロにする
          rule.multiSearchSleeping = false;
          rule.multiSearchTurns = 0;
        }
      } else {
        // === 通常モード (単一スレッド): Searching... -> Standby 10..3 ->
        // Standby 2..1 -> sleeping... ===
        if (identifiedAudioTid == 0) {
          if (rule.searchPhase == 0) {
            rule.searchPhase = 1;
            rule.searchTurns = 2 * turnsPerSec;
            rule.detectedThreadName = "Searching...";
            stateChanged = true;
          }

          if (rule.appType == 4) {
            // AppType 4 (Roon): 未確定時のみ毎ターン探索
            rule.detectedThreadName = "Searching...";
            scannedCyclesInTurn = true;
            TryIdentifyAudioThread(rule, sampleState, threadInfos, hProcess,
                                   pid, audioMask, intervalMs, "Searching...",
                                   identifiedAudioTid, identifiedAudioLabel,
                                   stateChanged);
          } else if (rule.searchPhase == 1) {
            scannedCyclesInTurn = true;
            TryIdentifyAudioThread(rule, sampleState, threadInfos, hProcess,
                                   pid, audioMask, intervalMs, "Searching...",
                                   identifiedAudioTid, identifiedAudioLabel,
                                   stateChanged);
          } else if (rule.searchPhase == 2) {
            // Standby 10..3
          } else if (rule.searchPhase == 3) {
            scannedCyclesInTurn = true;
            TryIdentifyAudioThread(rule, sampleState, threadInfos, hProcess,
                                   pid, audioMask, intervalMs, "Standby 2..1",
                                   identifiedAudioTid, identifiedAudioLabel,
                                   stateChanged);
          }
        }
      }

      // 3. アフィニティおよび優先度の適用
      // (オーディオスレッド特定時またはスロットBypass制御時のみ退避・適用を実行)
      std::vector<DWORD> intruderTids;
      bool sawOverlapOnFirstEvac = false;
      DWORD_PTR effectiveNormal = normalMask;

      std::unordered_set<DWORD> audioTidSet;
      std::unordered_set<DWORD> bypassedSlotTidSet;
      for (size_t s = 0; s < rule.activeAudioTids.size(); ++s) {
        DWORD t = rule.activeAudioTids[s];
        if (t == 0)
          continue;
        bool slotByp = (s < rule.slotBypassed.size() && rule.slotBypassed[s]);
        if (slotByp) {
          bypassedSlotTidSet.insert(t);
        } else {
          audioTidSet.insert(t);
        }
      }
      if (audioTidSet.empty() && bypassedSlotTidSet.empty() &&
          identifiedAudioTid != 0 && !isMultiSlot) {
        audioTidSet.insert(identifiedAudioTid);
      }

      DWORD_PTR combinedAudioMask = isMultiSlot ? 0 : audioMask;
      if (isMultiSlot) {
        for (size_t s = 0; s < targetCount; ++s) {
          if (s < rule.slotBypassed.size() && rule.slotBypassed[s])
            continue;
          if (s < rule.audioSlotMasks.size() && rule.audioSlotMasks[s] != 0) {
            combinedAudioMask |= rule.audioSlotMasks[s];
          } else if (s < rule.audioCores.size() && rule.audioCores[s] >= 0 &&
                     rule.audioCores[s] < 64) {
            combinedAudioMask |= (1ULL << rule.audioCores[s]);
          }
        }
      } else {
        for (int c : rule.audioCores) {
          combinedAudioMask |= (1ULL << c);
        }
      }

      if (!audioTidSet.empty() || !bypassedSlotTidSet.empty()) {
        std::vector<int> normalCoreList;
        for (int c = 0; c < 64; ++c) {
          if ((effectiveNormal & (1ULL << c)) != 0) {
            normalCoreList.push_back(c);
          }
        }

        // 退避対象スレッドに未適用または Ideal CPU 未設定のものがあるか確認
        bool needRebalanceIdeal = false;
        std::vector<size_t> evacIndices;
        for (size_t idx = 0; idx < threadInfos.size(); ++idx) {
          const auto &ti = threadInfos[idx];
          if (audioTidSet.count(ti.tid) > 0)
            continue;

          evacIndices.push_back(idx);
          if (ti.currentAffinity != effectiveNormal ||
              m_appliedThreads.find(ti.tid) == m_appliedThreads.end() ||
              m_appliedThreads[ti.tid] != effectiveNormal ||
              m_appliedIdealCores.find(ti.tid) == m_appliedIdealCores.end()) {
            needRebalanceIdeal = true;
          }
        }

        // Cycles Delta 降順で Ideal CPU をラウンドロビン割り当て (例: 4->5->6->7->4->5...)
        std::unordered_map<DWORD, int> targetIdealCores;
        if (!normalCoreList.empty() && !evacIndices.empty()) {
          if (needRebalanceIdeal) {
            if (!scannedCyclesInTurn) {
              for (size_t idx : evacIndices) {
                const auto &ti = threadInfos[idx];
                ULONG64 curC = 0;
                if (QueryThreadCycleTime(ti.hThread, &curC)) {
                  auto prevIt = sampleState.lastCycles.find(ti.tid);
                  ULONG64 d = 0;
                  if (prevIt != sampleState.lastCycles.end() &&
                      prevIt->second > 0) {
                    d = (curC >= prevIt->second) ? (curC - prevIt->second) : 0;
                  }
                  sampleState.lastCycles[ti.tid] = curC;
                  sampleState.latestDelta[ti.tid] = d;
                }
              }
            }
            std::vector<size_t> sortedIndices = evacIndices;
            std::sort(sortedIndices.begin(), sortedIndices.end(),
                      [&](size_t a, size_t b) {
                        DWORD tidA = threadInfos[a].tid;
                        DWORD tidB = threadInfos[b].tid;
                        ULONGLONG dA = sampleState.latestDelta[tidA];
                        ULONGLONG dB = sampleState.latestDelta[tidB];
                        if (dA != dB)
                          return dA > dB;
                        ULONG64 cA = sampleState.lastCycles[tidA];
                        ULONG64 cB = sampleState.lastCycles[tidB];
                        if (cA != cB)
                          return cA > cB;
                        return tidA < tidB;
                      });
            for (size_t rank = 0; rank < sortedIndices.size(); ++rank) {
              DWORD tid = threadInfos[sortedIndices[rank]].tid;
              targetIdealCores[tid] =
                  normalCoreList[rank % normalCoreList.size()];
            }
          } else {
            for (size_t idx : evacIndices) {
              DWORD tid = threadInfos[idx].tid;
              targetIdealCores[tid] = m_appliedIdealCores[tid];
            }
          }
        }

        // まず非オーディオスレッド (通常スレッド群およびスロットBypass対象スレッド)
        // を通常コア群へ退避し、AppType == 1 のみ同一PID内の侵入スレッド群を物理検出
        bool anyFirstTimeEvac = false;
        for (const auto &ti : threadInfos) {
          if (audioTidSet.count(ti.tid) > 0)
            continue;

          int idealCore = -1;
          auto idIt = targetIdealCores.find(ti.tid);
          if (idIt != targetIdealCores.end()) {
            idealCore = idIt->second;
          }

          // 個別Bypass中のスロット確定スレッド:
          // 優先度は一切変更せず (intruderTids に含めず)、除外マスク (effectiveNormal) を適用
          if (bypassedSlotTidSet.count(ti.tid) > 0) {
            if (EvacuateNormalThreadSettings(ti.hThread, ti.tid,
                                             effectiveNormal, idealCore)) {
              stateChanged = true;
            }
            continue;
          }

          bool wasPreviouslyEvacuated =
              (m_appliedThreads.find(ti.tid) != m_appliedThreads.end());
          if (!wasPreviouslyEvacuated) {
            anyFirstTimeEvac = true;
          }

          // AppType == 1 のみスキャン時点 (退避前) の物理アフィニティを検査:
          // 同一PID内の非オーディオスレッドがオーディオコア群 (combinedAudioMask) と重なっており、
          // (A) オーディオコア単独に自己バインドしている、または
          // (B) 退避適用済みであるにもかかわらず再びオーディオコアへ戻って居座っているスレッドを全件収集
          if (rule.appType == 1 && combinedAudioMask != 0 &&
              (ti.currentAffinity & combinedAudioMask) != 0) {
            bool isSpecificToAudio =
                ((ti.currentAffinity & ~combinedAudioMask) == 0);
            if (isSpecificToAudio || wasPreviouslyEvacuated) {
              intruderTids.push_back(ti.tid);
              char dbgBuf[256];
              snprintf(dbgBuf, sizeof(dbgBuf),
                       "IntruderCheck: PID=%lu, TID=%lu, aff=0x%llX, "
                       "audioMask=0x%llX, specific=%d, evac=%d",
                       pid, ti.tid, (unsigned long long)ti.currentAffinity,
                       (unsigned long long)combinedAudioMask, isSpecificToAudio,
                       wasPreviouslyEvacuated);
              LogDebug(dbgBuf);
            } else {
              sawOverlapOnFirstEvac = true;
            }
          }

          // 通常コア群 (effectiveNormal) へ退避・Ideal CPU 分散適用
          if (EvacuateNormalThreadSettings(ti.hThread, ti.tid,
                                           effectiveNormal, idealCore)) {
            stateChanged = true;
          }
        }
        // 新規生成スレッドの初回退避が発生した場合 (AppType == 1):
        // 次ターン (0.5秒後) に 1 回確認走査を実行してから 3 秒定常周期へ戻す
        if (anyFirstTimeEvac && rule.appType == 1) {
          if (rule.postIsolationGraceTurns < 2) {
            rule.postIsolationGraceTurns = 2;
          }
          rule.steadyWaitTurns = 0;
        }
      }

      bool hasIntruders = false;
      if (rule.appType == 1) {
        hasIntruders = rule.hasIntruderThreads;
        if (!canSkipNormalThreads) {
          if (!(rule.hasIntruderThreads && intruderTids.empty() &&
                sawOverlapOnFirstEvac)) {
            hasIntruders = !intruderTids.empty();
            if (rule.hasIntruderThreads != hasIntruders) {
              rule.hasIntruderThreads = hasIntruders;
              stateChanged = true;
            }
          }
          if (hasIntruders && !intruderTids.empty()) {
            char hLog[128];
            snprintf(hLog, sizeof(hLog),
                     "IntrudersSummary: PID=%lu, count=%zu, hasIntruders=%d",
                     pid, intruderTids.size(), hasIntruders);
            LogDebug(hLog);
          }
        }
      } else if (rule.hasIntruderThreads) {
        rule.hasIntruderThreads = false;
        stateChanged = true;
      }

      int curAudioPrio = (!rule.audioPriorities.empty())
                             ? rule.audioPriorities[0]
                             : rule.audioPriority;

      // Half判定時の自動昇格 (AppType == 1 のみ)
      if (rule.appType == 1 && !audioTidSet.empty() && hasIntruders) {
        if (!rule.wasHalfAutoPromoted) {
          if (!IsFixedThreadPriority(curAudioPrio) &&
              curAudioPrio <= THREAD_PRIORITY_IDLE) {
            curAudioPrio = THREAD_PRIORITY_LOWEST;
            rule.audioPriority = THREAD_PRIORITY_LOWEST;
            if (!rule.audioPriorities.empty()) {
              rule.audioPriorities[0] = THREAD_PRIORITY_LOWEST;
            }
            ResetDynamicPriorityFetch(rule, 0);
            stateChanged = true;
            char pLog[128];
            snprintf(pLog, sizeof(pLog),
                     "Auto-promoted audioPriority for PID=%lu from Idle(-15) "
                     "to Lowest(-2) due to Half-Isolated",
                     pid);
            LogDebug(pLog);
          }
          rule.wasHalfAutoPromoted = true;
        }
      }

      // オーディオスレッドの適用優先度 (targetAudioPrio) の決定
      int targetAudioPrio = curAudioPrio;

      // オーディオスレッド群への適用
      // (スロットごとに個別コア・個別サスペンド・個別Bypass制御)
      if (!audioTidSet.empty() || !bypassedSlotTidSet.empty()) {
        // プロセス優先度クラスの動的適用をスレッド優先度適用・Dyn Pri取得より先行実行
        DWORD curPrioClass = GetPriorityClass(hProcess);
        if (rule.processPriorityModified && rule.processPriorityClass != 0) {
          if (curPrioClass != 0 && curPrioClass != rule.processPriorityClass) {
            bool procPrioSet = false;
            if (SetPriorityClass(hProcess, rule.processPriorityClass)) {
              char pLog[128];
              snprintf(pLog, sizeof(pLog),
                       "SetPriorityClass applied: PID=%lu, class=0x%lX", pid,
                       rule.processPriorityClass);
              LogDebug(pLog);
              procPrioSet = true;
              stateChanged = true;
            } else if (rule.processPriorityClass == REALTIME_PRIORITY_CLASS) {
              if (SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS)) {
                LogDebug("SetPriorityClass fallback to High applied");
                procPrioSet = true;
                stateChanged = true;
              }
            }
            if (procPrioSet) {
              curPrioClass = GetPriorityClass(hProcess);
              ResetDynamicPriorityFetch(rule, -1);
              // Col 3 変更直後: Fixed 指定スロットには無条件で NtSetInformationThread を再適用
              for (size_t s = 0; s < rule.activeAudioTids.size(); ++s) {
                DWORD aTid = rule.activeAudioTids[s];
                if (aTid == 0) continue;
                int sPrio = (s < rule.audioPriorities.size()) ? rule.audioPriorities[s] : rule.audioPriority;
                if (IsFixedThreadPriority(sPrio)) {
                  for (const auto &ti : threadInfos) {
                    if (ti.tid == aTid && ti.hThread) {
                      ApplyAbsoluteThreadPriority(ti.hThread, DecodeFixedThreadPriority(sPrio));
                      break;
                    }
                  }
                }
              }
            }
          }
        }
        if (curPrioClass != 0) {
          if (rule.actualProcessPriorityClass != curPrioClass) {
            rule.actualProcessPriorityClass = curPrioClass;
            stateChanged = true;
          }
          if (!rule.processPriorityModified) {
            rule.processPriorityClass = curPrioClass;
          }
        }

        for (size_t s = 0; s < rule.activeAudioTids.size(); ++s) {
          DWORD aTid = rule.activeAudioTids[s];
          if (aTid == 0)
            continue;
          bool isSlotBypassed =
              (s < rule.slotBypassed.size() && rule.slotBypassed[s]);

          int assignedCore = rule.audioCore;
          DWORD_PTR assignedMask = (assignedCore >= 0 && assignedCore < 64)
                                       ? (1ULL << assignedCore)
                                       : audioMask;
          if (s < rule.audioSlotMasks.size() && rule.audioSlotMasks[s] != 0) {
            assignedMask = rule.audioSlotMasks[s];
            if (s < rule.audioCores.size()) {
              assignedCore = rule.audioCores[s];
            } else {
              for (int c = 0; c < 64; ++c) {
                if ((assignedMask & (1ULL << c)) != 0) {
                  assignedCore = c;
                  break;
                }
              }
            }
          } else if (s < rule.audioCores.size()) {
            assignedCore = rule.audioCores[s];
            assignedMask = (1ULL << assignedCore);
          } else if (!IsMultiSlotRule(rule) && audioMask != 0) {
            int bitIdx = -1;
            int foundCount = 0;
            for (int c = 0; c < 64; ++c) {
              if ((audioMask & (1ULL << c)) != 0) {
                if (foundCount == static_cast<int>(s)) {
                  bitIdx = c;
                  break;
                }
                foundCount++;
              }
            }
            if (bitIdx >= 0) {
              assignedCore = bitIdx;
              assignedMask = (1ULL << bitIdx);
            }
          }

          for (const auto &ti : threadInfos) {
            if (ti.tid == aTid) {
              audioDetectedInAny = true;
              if (primaryAudioThreadName.empty()) {
                primaryAudioThreadName = "PID " + std::to_string(pid) +
                                         " / TID " + std::to_string(ti.tid);
                primaryAudioPid = pid;
                primaryAudioTid = ti.tid;
              }
              if (!isSlotBypassed) {
                // ワンショットリセット要求 (長押し / ピッカー Normal 選択) の処理
                if (s < rule.resetPriorityToZeroRequested.size() &&
                    rule.resetPriorityToZeroRequested[s]) {
                  SetThreadPriority(ti.hThread, THREAD_PRIORITY_NORMAL);
                  rule.resetPriorityToZeroRequested[s] = false;
                  if (s < rule.audioPriorities.size()) {
                    rule.audioPriorities[s] = 0;
                  }
                  if (s == 0) {
                    rule.audioPriority = 0;
                    targetAudioPrio = 0;
                  }
                  ResetDynamicPriorityFetch(rule, static_cast<int>(s));
                  if (s < rule.actualAudioBasePriorities.size()) {
                    rule.actualAudioBasePriorities[s] = 0;
                  }
                  stateChanged = true;
                }

                int slotAudioPrio = (s < rule.audioPriorities.size())
                                        ? rule.audioPriorities[s]
                                        : targetAudioPrio;
                if (ApplyAudioThreadSettings(ti.hThread, ti.tid, slotAudioPrio,
                                             assignedCore, assignedMask, rule)) {
                  stateChanged = true;
                }

                // 実測 Dynamic Priority (Dyn Pri) の更新 (確定・変更直後 + 3秒後に1回のみ再取得し完了)
                if (ShouldFetchDynamicPriority(rule, s)) {
                  int curRel = GetThreadPriority(ti.hThread);
                  int actualDynPri = QueryThreadDynamicPriority(ti.hThread);
                  if (actualDynPri <= 0) {
                    DWORD effClass = (rule.processPriorityClass != 0)
                                         ? rule.processPriorityClass
                                         : NORMAL_PRIORITY_CLASS;
                    int rank = ati::FindRankByPrio(effClass, curRel);
                    const auto *entry = ati::GetMatrixEntryByRank(rank);
                    actualDynPri = entry ? entry->basePriority : (curRel + 8);
                  }
                  if (rule.actualAudioBasePriorities.size() <= s) {
                    rule.actualAudioBasePriorities.resize(s + 1, 0);
                  }
                  if (rule.actualThreadRelativePrios.size() <= s) {
                    rule.actualThreadRelativePrios.resize(s + 1, 0);
                  }
                  rule.actualAudioBasePriorities[s] = actualDynPri;
                  rule.actualThreadRelativePrios[s] = curRel;
                  stateChanged = true;
                }
              }
              break;
            }
          }
        }
      }

      // 侵入スレッド群すべてに -1 段階優先度を適用 (AppType == 1 のみ)
      // ※通常コア群へ正常退避できた通常スレッドの優先度には一切手を触れない！
      if (rule.appType == 1) {
        for (DWORD intTid : intruderTids) {
          for (const auto &ti : threadInfos) {
            if (ti.tid == intTid) {
              int effAudioPrio = (!rule.audioPriorities.empty())
                                     ? rule.audioPriorities[0]
                                     : targetAudioPrio;
              if (isMultiSlot) {
                for (size_t s = 0; s < targetCount; ++s) {
                  if (s < rule.slotBypassed.size() && rule.slotBypassed[s])
                    continue;
                  DWORD_PTR sMask = 0;
                  if (s < rule.audioSlotMasks.size() &&
                      rule.audioSlotMasks[s] != 0) {
                    sMask = rule.audioSlotMasks[s];
                  } else if (s < rule.audioCores.size() &&
                             rule.audioCores[s] >= 0 &&
                             rule.audioCores[s] < 64) {
                    sMask = (1ULL << rule.audioCores[s]);
                  }
                  if ((ti.currentAffinity & sMask) != 0) {
                    effAudioPrio = (s < rule.audioPriorities.size())
                                       ? rule.audioPriorities[s]
                                       : targetAudioPrio;
                    break;
                  }
                }
              }

              if (IsFixedThreadPriority(effAudioPrio)) {
                int audioAbs = DecodeFixedThreadPriority(effAudioPrio);
                int intruderAbs = (audioAbs > 1) ? (audioAbs - 1) : 1;
                int curDyn = QueryThreadDynamicPriority(ti.hThread);
                if (curDyn != intruderAbs) {
                  bool ok = ApplyAbsoluteThreadPriority(ti.hThread, intruderAbs);
                  if (!ok && intruderAbs >= 16) {
                    SetThreadPriority(ti.hThread, THREAD_PRIORITY_HIGHEST);
                  }
                  char iLog[160];
                  snprintf(iLog, sizeof(iLog),
                           "Adjusted intruder thread Fixed KPRIORITY to %d "
                           "(audio Fixed %d): PID=%lu, TID=%lu",
                           intruderAbs, audioAbs, pid, ti.tid);
                  LogDebug(iLog);
                }
              } else {
                int stepLowerPrio = GetOneStepLowerPriority(effAudioPrio);
                if (ti.priority != stepLowerPrio) {
                  SetThreadPriority(ti.hThread, stepLowerPrio);
                  char iLog[160];
                  snprintf(iLog, sizeof(iLog),
                           "Adjusted true intruder thread priority to %d (audio "
                           "is %d): PID=%lu, TID=%lu",
                           stepLowerPrio, effAudioPrio, pid, ti.tid);
                  LogDebug(iLog);
                }
              }
              break;
            }
          }
        }
      }

      // ハンドル解放
      for (const auto &ti : threadInfos) {
        CloseHandle(ti.hThread);
      }

      // 消滅したスレッドのトラッキング情報および CPU 時間記録をクリーンアップ
      auto trackCleanIt = m_trackedAudioThreads.find(pid);
      if (trackCleanIt != m_trackedAudioThreads.end()) {
        for (auto tIt = trackCleanIt->second.begin();
             tIt != trackCleanIt->second.end();) {
          if (aliveTids.find(tIt->first) == aliveTids.end()) {
            tIt = trackCleanIt->second.erase(tIt);
          } else {
            ++tIt;
          }
        }
      }

      auto cpuCleanIt = m_prevThreadCpuTimes.find(pid);
      if (cpuCleanIt != m_prevThreadCpuTimes.end()) {
        for (auto itCpu = cpuCleanIt->second.begin();
             itCpu != cpuCleanIt->second.end();) {
          if (aliveTids.find(itCpu->first) == aliveTids.end()) {
            itCpu = cpuCleanIt->second.erase(itCpu);
          } else {
            ++itCpu;
          }
        }
      }

      if (!canSkipNormalThreads) {
        m_lastProcessThreadCount[pid] = static_cast<int>(currentTids.size());
        m_cachedProcessTids[pid] = currentTids;
      }

      CloseHandle(hProcess);
    }

    if (audioDetectedInAny) {
      if (!wasIsolatedBeforeScan || !rule.isAudioIsolated) {
        rule.isAudioIsolated = true;
        WakeUpSteadyRule(rule, 2);
        stateChanged = true;
      } else if (rule.postIsolationGraceTurns > 0) {
        rule.postIsolationGraceTurns--;
        if (rule.postIsolationGraceTurns == 0 && IsRuleInSteadyState(rule)) {
          rule.steadyWaitTurns =
              (steadyPeriodTurns > 1) ? (steadyPeriodTurns - 1) : 0;
        }
      }
      if (rule.activePid != primaryAudioPid) {
        rule.activePid = primaryAudioPid;
        stateChanged = true;
      }
      if (rule.activeAudioTid != primaryAudioTid) {
        ResumeAudioThreadIfSuspended(rule);
        rule.activeAudioTid = primaryAudioTid;
        rule.wasHalfAutoPromoted = false;
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
      if (rule.hasIntruderThreads) {
        rule.hasIntruderThreads = false;
        stateChanged = true;
      }
      if (wasIsolatedBeforeScan) {
        rule.steadyWaitTurns = 0;
        rule.postIsolationGraceTurns = 0;
      } else if (rule.postIsolationGraceTurns > 0) {
        rule.postIsolationGraceTurns--;
        if (rule.postIsolationGraceTurns == 0 && IsRuleInSteadyState(rule)) {
          rule.steadyWaitTurns =
              (steadyPeriodTurns > 1) ? (steadyPeriodTurns - 1) : 0;
        }
      }
      if (rule.activeAudioTid != 0) {
        ResumeAudioThreadIfSuspended(rule);
        rule.activeAudioTid = 0;
        rule.activeAudioTids.clear();
        rule.wasHalfAutoPromoted = false;
        stateChanged = true;
      }
      for (DWORD p : it->second) {
        m_trackedAudioThreads.erase(p);
      }
      // 未特定時のフェーズ進行
      // (全PID走査完了後にプロセスグループ単位で1ターン進行)
      if (rule.appType == 4) {
        // AppType 4 (Roon): sleeping に入らせず Searching... のまま待機
        if (rule.detectedThreadName != "Searching...") {
          rule.detectedThreadName = "Searching...";
          stateChanged = true;
        }
        rule.searchPhase = 1;
        rule.searchTurns = 0;
      } else if (rule.searchPhase == 1) {
        if (rule.detectedThreadName != "Searching...") {
          rule.detectedThreadName = "Searching...";
          stateChanged = true;
        }
        rule.searchTurns--;
        if (rule.searchTurns <= 0) {
          rule.searchPhase = 2;
          rule.searchTurns = 10 * turnsPerSec;
          rule.detectedThreadName = "Standby 10";
          stateChanged = true;
        }
      } else if (rule.searchPhase == 2) {
        int sec = (rule.searchTurns + 1) / 2;
        if (sec < 1)
          sec = 1;
        std::string sName = "Standby " + std::to_string(sec);
        if (rule.detectedThreadName != sName) {
          rule.detectedThreadName = sName;
          stateChanged = true;
        }
        rule.searchTurns--;
        if (rule.searchTurns <= 2 * turnsPerSec) {
          rule.searchPhase = 3;
        }
      } else if (rule.searchPhase == 3) {
        int sec = (rule.searchTurns + 1) / 2;
        if (sec < 1)
          sec = 1;
        std::string sName = "Standby " + std::to_string(sec);
        if (rule.detectedThreadName != sName) {
          rule.detectedThreadName = sName;
          stateChanged = true;
        }
        rule.searchTurns--;
        if (rule.searchTurns <= 0) {
          rule.searchPhase = 4;
          rule.searchTurns = 0;
          rule.detectedThreadName = "sleeping...";
          stateChanged = true;
          m_samplingStates.clear();
        }
      } else if (rule.searchPhase == 4) {
        if (rule.detectedThreadName != "sleeping...") {
          rule.detectedThreadName = "sleeping...";
          stateChanged = true;
        }
      } else {
        rule.detectedThreadName = "Searching...";
        rule.searchPhase = 1;
        rule.searchTurns = 2 * turnsPerSec;
        stateChanged = true;
      }
    }

    if (UpdateDisplayThreadCount(rule, totalThreadCount))
      stateChanged = true;
    if (stateChanged)
      LogRuleStateChange(rule);
  }

  return stateChanged;
}

bool ThreadIsolator::IsAudioDgProcess(const std::string &name) {
  return _stricmp(name.c_str(), "audiodg.exe") == 0 ||
         _stricmp(name.c_str(), "audiodg") == 0;
}

bool ThreadIsolator::IsRoonApplianceProcess(const std::string &name) {
  return _stricmp(name.c_str(), "roonappliance.exe") == 0 ||
         _stricmp(name.c_str(), "roonappliance") == 0;
}

bool ThreadIsolator::IsRaatServerProcess(const std::string &name) {
  return _stricmp(name.c_str(), "raatserver.exe") == 0 ||
         _stricmp(name.c_str(), "raatserver") == 0;
}

bool ThreadIsolator::IsRoonBridgeProcess(const std::string &name) {
  return _stricmp(name.c_str(), "roonbridge.exe") == 0 ||
         _stricmp(name.c_str(), "roonbridge") == 0;
}

bool ThreadIsolator::IsVoicemeeterProcess(const std::string &name) {
  std::string lower = ToLowerA(name);
  if (lower.length() >= 4 && lower.substr(lower.length() - 4) == ".exe") {
    lower = lower.substr(0, lower.length() - 4);
  }
  return lower == "voicemeeter" || lower == "voicemeeter_x64" ||
         lower == "voicemeeterx64" || lower == "voicemeeterpro" ||
         lower == "voicemeeterpro_x64" || lower == "voicemeeterprox64" ||
         lower == "voicemeeter8" || lower == "voicemeeter8_x64" ||
         lower == "voicemeeter8x64";
}

void ThreadIsolator::NormalizeSpecialRule(ProcessRule &rule) {
  if (IsRoonBridgeProcess(rule.processName)) {
    rule.processName = "RAATServer.exe";
    rule.appType = 4;
  } else if (IsRoonApplianceProcess(rule.processName) ||
             IsRaatServerProcess(rule.processName)) {
    rule.appType = 4;
  } else if (IsVoicemeeterProcess(rule.processName)) {
    rule.appType = 5;
  } else if (IsAudioDgProcess(rule.processName)) {
    rule.appType = 0;
  }

  size_t slotCount = GetRuleSlotCount(rule);
  if (rule.audioSlotMasks.size() > slotCount) {
    rule.audioSlotMasks.resize(slotCount);
  }
  if (rule.audioCores.size() > slotCount) {
    rule.audioCores.resize(slotCount);
  }
  if (slotCount > 1) {
    // マルチスロット: スロット数ぶんのマスクおよびコアを常に保持
    if (rule.audioSlotMasks.size() < slotCount) {
      size_t prevSize = rule.audioSlotMasks.size();
      rule.audioSlotMasks.resize(slotCount, 0);
      for (size_t i = prevSize; i < slotCount; ++i) {
        if (i < rule.audioCores.size() && rule.audioCores[i] >= 0 && rule.audioCores[i] < 64) {
          rule.audioSlotMasks[i] = (1ULL << rule.audioCores[i]);
        } else if (rule.audioCore >= 0 && rule.audioCore < 64) {
          rule.audioSlotMasks[i] = (1ULL << rule.audioCore);
        }
      }
    }
    if (rule.audioCores.size() < slotCount) {
      rule.audioCores.resize(slotCount, rule.audioCore);
    }
    // audioSlotMasks から audioCores (先頭コア) および全マスクを同期
    DWORD_PTR totalM = 0;
    for (size_t i = 0; i < slotCount; ++i) {
      totalM |= rule.audioSlotMasks[i];
      for (int c = 0; c < 64; ++c) {
        if ((rule.audioSlotMasks[i] & (1ULL << c)) != 0) {
          rule.audioCores[i] = c;
          break;
        }
      }
    }
    if (totalM != 0) {
      rule.audioAffinityMask = totalM;
    }
    if (!rule.audioCores.empty()) {
      rule.audioCore = rule.audioCores[0];
    }
  } else {
    DWORD_PTR singleMask =
        (rule.audioAffinityMask != 0)
            ? rule.audioAffinityMask
            : ((rule.audioCore >= 0 && rule.audioCore < 64)
                   ? (1ULL << rule.audioCore)
                   : 0);
    if (singleMask != 0) {
      rule.audioAffinityMask = singleMask;
      rule.audioSlotMasks = {singleMask};
    }
  }
  if (rule.audioPriorities.size() > slotCount) {
    rule.audioPriorities.resize(slotCount);
  } else if (slotCount > 1 && rule.audioPriorities.size() < slotCount) {
    rule.audioPriorities.resize(slotCount, rule.audioPriority);
  }
  if (rule.slotBypassed.size() > slotCount) {
    rule.slotBypassed.resize(slotCount);
  } else if (slotCount > 1 && rule.slotBypassed.size() < slotCount) {
    rule.slotBypassed.resize(slotCount, false);
  }
  if (rule.actualAudioBasePriorities.size() < slotCount) {
    rule.actualAudioBasePriorities.resize(slotCount, 0);
  }
  if (rule.actualThreadRelativePrios.size() < slotCount) {
    rule.actualThreadRelativePrios.resize(slotCount, 0);
  }
  if (rule.resetPriorityToZeroRequested.size() < slotCount) {
    rule.resetPriorityToZeroRequested.resize(slotCount, false);
  }
  if (rule.normalPrioForceApplied.size() < slotCount) {
    rule.normalPrioForceApplied.resize(slotCount, false);
  }
  if (rule.prioPollTurns.size() < slotCount) {
    rule.prioPollTurns.resize(slotCount, 0);
  }
  if (rule.prioRefetchTicks.size() < slotCount) {
    rule.prioRefetchTicks.resize(slotCount, 0);
  }
}

size_t ThreadIsolator::GetRuleSlotCount(const ProcessRule &rule) {
  if (IsVoicemeeterProcess(rule.processName)) {
    return 2;
  }
  return 1;
}

bool ThreadIsolator::IsMultiSlotRule(const ProcessRule &rule) {
  return GetRuleSlotCount(rule) > 1;
}

std::unordered_set<std::string>
ThreadIsolator::BuildUnlistedWhitelist(bool includeExclusions) const {
  std::unordered_set<std::string> whitelist = {"ati.exe", "system",
                                               "system idle process"};
  for (const auto &r : m_config.rules) {
    whitelist.insert(ToLowerA(r.processName));
  }
  if (includeExclusions) {
    for (const auto &ex : m_config.unlistedExclusions) {
      whitelist.insert(ToLowerA(ex));
    }
  }
  return whitelist;
}

void ThreadIsolator::RestoreAffinityForProcessNamesLocked(
    const std::unordered_set<std::string> &namesLower) {
  if (namesLower.empty())
    return;
  DWORD_PTR fullMask = GetFullCoreMask(CpuTopology::GetSystemCoreCount());
  if (fullMask == 0)
    return;
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE)
    return;
  PROCESSENTRY32 pe = {sizeof(pe)};
  if (Process32First(hSnap, &pe)) {
    do {
      if (namesLower.count(ToLowerA(pe.szExeFile)) > 0) {
        m_unlistedMaskedPids.erase(pe.th32ProcessID);
        SetProcessAffinityByPid(pe.th32ProcessID, fullMask);
      }
    } while (Process32Next(hSnap, &pe));
  }
  CloseHandle(hSnap);
}

void ThreadIsolator::EnableDebugPrivilegeOnce() {
  if (m_debugPrivilegeEnabled)
    return;
  HANDLE hToken = nullptr;
  if (OpenProcessToken(GetCurrentProcess(),
                       TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
    TOKEN_PRIVILEGES tp = {0};
    LUID luid = {0};
    if (LookupPrivilegeValueA(nullptr, "SeDebugPrivilege", &luid)) {
      tp.PrivilegeCount = 1;
      tp.Privileges[0].Luid = luid;
      tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
      m_debugPrivilegeEnabled = true;
    }
    CloseHandle(hToken);
  }
}

bool ThreadIsolator::ApplyUnlistedMaskLocked(int mode, DWORD_PTR targetMask) {
  EnableDebugPrivilegeOnce();
  if (targetMask == 0)
    return false;

  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE)
    return false;

  m_config.unlistedMaskMode = mode;
  m_activeUnlistedMask = targetMask;
  m_unlistedMaskedPids.clear();
  m_hasUnlistedMaskInOs = true;

  auto whitelist = BuildUnlistedWhitelist(true);
  DWORD myPid = GetCurrentProcessId();
  PROCESSENTRY32 pe = {sizeof(pe)};
  if (Process32First(hSnap, &pe)) {
    do {
      if (pe.th32ProcessID <= 4 || pe.th32ProcessID == myPid)
        continue;
      if (whitelist.count(ToLowerA(pe.szExeFile)) > 0)
        continue;

      m_unlistedMaskedPids.insert(pe.th32ProcessID);
      if (SetProcessAffinityByPid(pe.th32ProcessID, targetMask)) {
        m_unlistedApplyCount++;
      }
    } while (Process32Next(hSnap, &pe));
  }
  CloseHandle(hSnap);
  return true;
}

bool ThreadIsolator::ApplyUnlistedMask(int mode, DWORD_PTR targetMask) {
  std::lock_guard<std::mutex> lock(m_mutex);
  return ApplyUnlistedMaskLocked(mode, targetMask);
}

void ThreadIsolator::StopUnlistedPollingMask() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config.unlistedMaskMode = 0;
}

bool ThreadIsolator::ReleaseUnlistedMask() {
  std::lock_guard<std::mutex> lock(m_mutex);
  EnableDebugPrivilegeOnce();

  m_config.unlistedMaskMode = 0;
  m_activeUnlistedMask = 0;
  m_unlistedMaskedPids.clear();
  m_hasUnlistedMaskInOs = false;

  DWORD_PTR fullMask = GetFullCoreMask(CpuTopology::GetSystemCoreCount());
  if (fullMask == 0)
    return false;

  auto whitelist = BuildUnlistedWhitelist(false);
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE)
    return false;

  DWORD myPid = GetCurrentProcessId();
  PROCESSENTRY32 pe = {sizeof(pe)};
  if (Process32First(hSnap, &pe)) {
    do {
      if (pe.th32ProcessID <= 4 || pe.th32ProcessID == myPid)
        continue;
      if (whitelist.count(ToLowerA(pe.szExeFile)) > 0)
        continue;

      SetProcessAffinityByPid(pe.th32ProcessID, fullMask);
    } while (Process32Next(hSnap, &pe));
  }
  CloseHandle(hSnap);
  return true;
}

bool ThreadIsolator::HasAppliedUnlistedMask() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_hasUnlistedMaskInOs;
}

void ThreadIsolator::UpdateUnlistedExclusions(
    const std::vector<std::string> &exclusions) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config.unlistedExclusions = exclusions;
  if (!m_hasUnlistedMaskInOs && m_config.unlistedMaskMode == 0)
    return;

  std::unordered_set<std::string> exclLower;
  for (const auto &ex : exclusions) {
    if (!ex.empty()) {
      exclLower.insert(ToLowerA(ex));
    }
  }
  RestoreAffinityForProcessNamesLocked(exclLower);
}

DWORD ThreadIsolator::GetUnlistedApplyCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_unlistedApplyCount;
}

UINT ThreadIsolator::GetDesiredPollingIntervalMs() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto &pair : m_chromiumAudioStates) {
    if (pair.second.samplingActive) {
      if (pair.second.isDecline) {
        return 1000;
      }
      int normalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
      int boostMs = normalMs / 4;
      if (boostMs < 50) {
        boostMs = 50;
      }
      return static_cast<UINT>(boostMs);
    }
  }
  return static_cast<UINT>(
      m_config.pollingIntervalMs > 0 ? m_config.pollingIntervalMs : 500);
}

} // namespace ati
