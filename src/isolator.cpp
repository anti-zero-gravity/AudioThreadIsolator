#undef UNICODE
#undef _UNICODE

#include "isolator.h"
#include <algorithm>
#include <cctype>
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

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI *pfnNtQueryInformationThread)(
    HANDLE ThreadHandle, ULONG ThreadInformationClass, PVOID ThreadInformation,
    ULONG ThreadInformationLength, PULONG ReturnLength);
static pfnNtQueryInformationThread s_pfnNtQueryInformationThread = nullptr;

static void *QueryThreadStartAddress(HANDLE hThread) {
  if (!s_pfnNtQueryInformationThread) {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll) {
      s_pfnNtQueryInformationThread =
          reinterpret_cast<pfnNtQueryInformationThread>(
              GetProcAddress(hNtdll, "NtQueryInformationThread"));
    }
  }
  if (!s_pfnNtQueryInformationThread)
    return nullptr;

  void *startAddr = nullptr;
  ULONG retLen = 0;
  NTSTATUS status = s_pfnNtQueryInformationThread(
      hThread, 9 /* ThreadQuerySetWin32StartAddress */, &startAddr,
      sizeof(startAddr), &retLen);
  if (status == 0)
    return startAddr;
  return nullptr;
}

typedef NTSTATUS(NTAPI *pfnNtGetNextThread)(
    HANDLE ProcessHandle,
    HANDLE ThreadHandle,
    ACCESS_MASK DesiredAccess,
    ULONG HandleAttributes,
    ULONG Flags,
    PHANDLE NewThreadHandle);
static pfnNtGetNextThread s_pfnNtGetNextThread = nullptr;

static void EnsureNtLoaded() {
  HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
  if (hNtdll) {
    if (!s_pfnNtQueryInformationThread) {
      s_pfnNtQueryInformationThread =
          reinterpret_cast<pfnNtQueryInformationThread>(
              GetProcAddress(hNtdll, "NtQueryInformationThread"));
    }
    if (!s_pfnNtGetNextThread) {
      s_pfnNtGetNextThread = reinterpret_cast<pfnNtGetNextThread>(
          GetProcAddress(hNtdll, "NtGetNextThread"));
    }
  }
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
      s_pfnNtQueryInformationThread =
          reinterpret_cast<pfnNtQueryInformationThread>(
              GetProcAddress(hNtdll, "NtQueryInformationThread"));
    }
  }
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

static std::string QueryFmodOrUnityThreadName(HANDLE hProcess, HANDLE hThread) {
  if (!s_pfnNtQueryInformationThread) {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll) {
      s_pfnNtQueryInformationThread =
          reinterpret_cast<pfnNtQueryInformationThread>(
              GetProcAddress(hNtdll, "NtQueryInformationThread"));
    }
  }
  if (!s_pfnNtQueryInformationThread)
    return "";

  THREAD_BASIC_INFO_RAW tbi = {0};
  ULONG retLen = 0;
  NTSTATUS status = s_pfnNtQueryInformationThread(
      hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
  if (status != 0 || !tbi.TebBaseAddress) {
    return "";
  }

  // 64bit Windows: TEB + 0x08 is StackBase
  DWORD_PTR stackBase = 0;
  SIZE_T bytesRead = 0;
  if (!ReadProcessMemory(
          hProcess,
          reinterpret_cast<LPCVOID>(
              reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 8),
          &stackBase, sizeof(stackBase), &bytesRead) ||
      bytesRead != sizeof(stackBase)) {
    return "";
  }
  if (stackBase < 0x10000)
    return "";

  // Read 512 bytes near bottom of stack: [stackBase - 0x200, stackBase)
  const DWORD scanSize = 0x200;
  BYTE stackBuf[scanSize] = {0};
  LPCVOID scanAddr = reinterpret_cast<LPCVOID>(stackBase - scanSize);
  if (!ReadProcessMemory(hProcess, scanAddr, stackBuf, scanSize, &bytesRead) ||
      bytesRead < 64) {
    return "";
  }

  size_t u64Count = bytesRead / sizeof(DWORD_PTR);
  const DWORD_PTR *ptrs = reinterpret_cast<const DWORD_PTR *>(stackBuf);

  for (size_t i = 0; i < u64Count; ++i) {
    DWORD_PTR p = ptrs[i];
    if (p < 0x10000 || p > 0x7FFFFFFFFFFFULL)
      continue;

    char objBuf[64] = {0};
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(p), objBuf,
                          sizeof(objBuf), &bytesRead) &&
        bytesRead >= 16) {
      for (size_t off = 0; off + 4 <= sizeof(objBuf); ++off) {
        if (memcmp(&objBuf[off], "FMOD", 4) == 0) {
          const char *strStart = &objBuf[off];
          size_t len = strnlen(strStart, sizeof(objBuf) - off);
          return std::string(strStart, len);
        }
      }

      const DWORD_PTR *objPtrs = reinterpret_cast<const DWORD_PTR *>(objBuf);
      DWORD_PTR argPtr = objPtrs[1];
      if (argPtr >= 0x10000 && argPtr <= 0x7FFFFFFFFFFFULL) {
        char argBuf[64] = {0};
        if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(argPtr),
                              argBuf, sizeof(argBuf), &bytesRead) &&
            bytesRead >= 16) {
          for (size_t off = 0; off + 4 <= sizeof(argBuf); ++off) {
            if (memcmp(&argBuf[off], "FMOD", 4) == 0) {
              const char *strStart = &argBuf[off];
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

// コールスタック上のモジュールおよび参照シグネチャ走査 (WASAPI / ASIO /
// DirectSound / Godot 等)
static std::string QueryCallstackAudioSignature(HANDLE hProcess,
                                                HANDLE hThread) {
  if (!s_pfnNtQueryInformationThread) {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (hNtdll) {
      s_pfnNtQueryInformationThread =
          reinterpret_cast<pfnNtQueryInformationThread>(
              GetProcAddress(hNtdll, "NtQueryInformationThread"));
    }
  }
  if (!s_pfnNtQueryInformationThread)
    return "";

  EnsurePsapiLoaded();

  // スタック上のコード参照シグネチャ走査
  THREAD_BASIC_INFO_RAW tbi = {0};
  ULONG retLen = 0;
  NTSTATUS status = s_pfnNtQueryInformationThread(
      hThread, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), &retLen);
  if (status != 0 || !tbi.TebBaseAddress) {
    return "";
  }

  // 64bit Windows: TEB + 0x08 is StackBase, TEB + 0x10 is StackLimit
  DWORD_PTR stackBase = 0;
  DWORD_PTR stackLimit = 0;
  SIZE_T bytesRead = 0;
  if (!ReadProcessMemory(
          hProcess,
          reinterpret_cast<LPCVOID>(
              reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 8),
          &stackBase, sizeof(stackBase), &bytesRead) ||
      bytesRead != sizeof(stackBase)) {
    return "";
  }
  ReadProcessMemory(hProcess,
                    reinterpret_cast<LPCVOID>(
                        reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 16),
                    &stackLimit, sizeof(stackLimit), &bytesRead);

  if (stackBase < 0x10000)
    return "";

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
    return "";
  }

  size_t u64Count = bytesRead / sizeof(DWORD_PTR);
  const DWORD_PTR *ptrs = reinterpret_cast<const DWORD_PTR *>(stackBuf.data());

  // モジュール名解決キャッシュ (同一 AllocationBase に対する API
  // 呼び出しの重複を防ぐ)
  std::unordered_map<DWORD_PTR, std::string> modNameCache;

  for (size_t i = 0; i < u64Count; ++i) {
    DWORD_PTR p = ptrs[i];
    if (p < 0x100000 || p > 0x7FFFFFFFFFFFULL)
      continue;

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
            // (1) InstantAud.ax (CyberLink PowerDVD オーディオ出力)
            if (modName.find("instantaud") != std::string::npos) {
              return "InstantAud.ax (CyberLink)";
            }
            // (2) AUDIOSES.DLL (WASAPI 最前線)
            if (modName.find("audioses") != std::string::npos) {
              return "WASAPI (audioses.dll)";
            }
            // (3) ASIO ドライバ DLL
            if (modName.find("asio") != std::string::npos) {
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
            // (6) WaveOut
            if (modName.find("winmm") != std::string::npos) {
              return "WaveOut (winmm.dll)";
            }
          }
        }

        // 従来の Godot 等の埋め込みバイナリ向けシグネチャ走査 (フォールバック)
        BYTE codeBuf[512] = {0};
        DWORD_PTR codeStart = (p >= 128) ? (p - 128) : p;
        SIZE_T cRead = 0;
        if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(codeStart),
                              codeBuf, sizeof(codeBuf), &cRead) &&
            cRead >= 64) {
          // x64 相対参照 [rip + disp32] の走査
          for (size_t c = 0; c + 7 <= cRead; ++c) {
            if (codeBuf[c] == 0x48 &&
                (codeBuf[c + 1] == 0x8d || codeBuf[c + 1] == 0x8b)) {
              BYTE modrm = codeBuf[c + 2];
              if ((modrm & 0xC7) == 0x05) { // [rip + disp32]
                INT32 disp = *reinterpret_cast<const INT32 *>(&codeBuf[c + 3]);
                DWORD_PTR targetAddr = (codeStart + c + 7) + disp;
                if (targetAddr >= 0x10000 && targetAddr <= 0x7FFFFFFFFFFFULL) {
                  char strBuf[64] = {0};
                  SIZE_T sRead = 0;
                  if (ReadProcessMemory(hProcess,
                                        reinterpret_cast<LPCVOID>(targetAddr),
                                        strBuf, sizeof(strBuf) - 1, &sRead) &&
                      sRead >= 8) {
                    std::string lowerStr = ToLowerA(strBuf);
                    if (lowerStr.find("audio_driver_wasapi") !=
                            std::string::npos ||
                        lowerStr.find("audiodriverwasapi") !=
                            std::string::npos) {
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
          std::string lowerCode(reinterpret_cast<const char *>(codeBuf), cRead);
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
  m_prevThreadCpuTimes.clear();
  m_samplingStates.clear();
  m_chromiumThreadTracks.clear();
  m_chromiumAudioStates.clear();
  m_chromiumEvictedPids.clear();
}

void ThreadIsolator::UpdateConfig(const GlobalConfig &config) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config = config;
  m_appliedThreads.clear();
  m_prevThreadCpuTimes.clear();
  m_samplingStates.clear();
  m_chromiumThreadTracks.clear();
  m_chromiumAudioStates.clear();
  m_chromiumEvictedPids.clear();
}

GlobalConfig ThreadIsolator::GetConfig() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_config;
}

void ThreadIsolator::AddRule(const ProcessRule &rule) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config.rules.push_back(rule);
  m_appliedThreads.clear();
}

void ThreadIsolator::RemoveRule(size_t index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index < m_config.rules.size()) {
    auto &rule = m_config.rules[index];
    if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
      HANDLE hThread =
          OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
      if (hThread) {
        ResumeThread(hThread);
        CloseHandle(hThread);
      }
    }
    m_config.rules.erase(m_config.rules.begin() + index);
  }
}

void ThreadIsolator::UpdateRule(size_t index, const ProcessRule &rule) {
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
    m_config.rules[index].enableHeuristics =
        !m_config.rules[index].enableHeuristics;
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
    auto &rule = m_config.rules[index];
    rule.isBypassed = !rule.isBypassed;
    if (rule.audioServicePid != 0) {
      m_chromiumMaskedPids.erase(rule.audioServicePid);
      m_chromiumEvictedPids.erase(rule.audioServicePid);
      m_chromiumThreadTracks.erase(rule.audioServicePid);
      m_chromiumAudioStates.erase(rule.audioServicePid);
      m_chromiumScannedPids.erase(rule.audioServicePid);
      rule.audioServicePid = 0;
    }
    if (rule.activePid != 0) {
      m_chromiumMaskedPids.erase(rule.activePid);
      m_chromiumScannedPids.erase(rule.activePid);
    }
    rule.chromiumScanAttempted = false;
    rule.wasHalfAutoPromoted = false;
    if (rule.isBypassed) {
      if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
        HANDLE hThread =
            OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
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
        int intervalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
        int turnsPerSec = 1000 / intervalMs;
        if (turnsPerSec < 1) turnsPerSec = 1;
        rule.detectedThreadName = "Searching...";
        rule.searchPhase = 1;
        rule.searchTurns = 2 * turnsPerSec;
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
  if (turnsPerSec < 1) turnsPerSec = 1;

  rule.searchPhase = 1;
  rule.searchTurns = 2 * turnsPerSec;
  rule.detectedThreadName = "Searching...";
  rule.isAudioIsolated = false;
  rule.activeAudioTid = 0;
  rule.chromiumScanAttempted = false;
  if (rule.audioServicePid != 0) {
    m_chromiumAudioStates.erase(rule.audioServicePid);
    m_chromiumThreadTracks.erase(rule.audioServicePid);
  }
  m_samplingStates.erase(rule.activePid);
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
  if (ms <= 150) return 100;
  if (ms <= 350) return 200;
  if (ms <= 750) return 500;
  return 1000;
}

bool ThreadIsolator::ToggleSuspendAudioThread(size_t index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (index >= m_config.rules.size())
    return false;
  auto &rule = m_config.rules[index];
  if (!rule.isRunning || rule.activeAudioTid == 0)
    return false;

  HANDLE hThread =
      OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
  if (!hThread) {
    char errBuf[128];
    snprintf(errBuf, sizeof(errBuf),
             "Failed to OpenThread(TID=%lu) for suspend/resume: err=%lu",
             rule.activeAudioTid, GetLastError());
    LogDebug(errBuf);
    return false;
  }

  if (!rule.isAudioThreadSuspended) {
    DWORD prevCount = SuspendThread(hThread);
    rule.isAudioThreadSuspended = true;
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf),
             "Suspended Audio Thread: TID=%lu (prevCount=%lu)",
             rule.activeAudioTid, prevCount);
    LogDebug(logBuf);
  } else {
    DWORD prevCount = ResumeThread(hThread);
    rule.isAudioThreadSuspended = false;
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf),
             "Resumed Audio Thread: TID=%lu (prevCount=%lu)",
             rule.activeAudioTid, prevCount);
    LogDebug(logBuf);
  }
  CloseHandle(hThread);
  return true;
}

void ThreadIsolator::ResumeAllSuspendedThreads() {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto &rule : m_config.rules) {
    if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
      HANDLE hThread =
          OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
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


bool ThreadIsolator::ScanAndIsolate() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_config.rules.empty())
    return false;

  bool stateChanged = false;
  int coreCount = GetSystemCoreCount();
  std::string heuristicsStatus = "";

  // 1. 実行中の全プロセスを取得 (動的スキップ制御 & 低速ポーリング)
  // (A) 監視有効な登録アプリがすべて起動中の場合は SNAPPROCESS をスキップ (0 回)
  // (B) 未起動アプリが存在する場合は INI x 10 (例: 500ms設定なら5秒) のスロー周期でのみ発行
  static std::unordered_map<std::string, std::vector<DWORD>> s_cachedRunningProcesses;
  static std::unordered_map<DWORD, int> s_cachedCurrentPidThreadCounts;
  static int s_processScanCountdown = 0;

  bool hasUnstartedApp = false;
  for (const auto &r : m_config.rules) {
    if (!r.isBypassed) {
      if (!r.isRunning || (r.isChromium == 1 && r.audioServicePid == 0)) {
        hasUnstartedApp = true;
        break;
      }
    }
  }

  bool shouldScanProcesses = false;
  if (s_cachedRunningProcesses.empty()) {
    shouldScanProcesses = true;
    s_processScanCountdown = 10;
  } else if (hasUnstartedApp) {
    s_processScanCountdown--;
    if (s_processScanCountdown <= 0) {
      shouldScanProcesses = true;
      s_processScanCountdown = 10;
    }
  } else {
    // 全アプリ起動中: スキップ (0 回)
    s_processScanCountdown = 10;
  }

  // SNAPPROCESS スキップ時は、既存プロセスの死活を GetExitCodeProcess で高速確認 (数ナノ秒)
  if (!shouldScanProcesses) {
    for (auto it = s_cachedRunningProcesses.begin(); it != s_cachedRunningProcesses.end(); ++it) {
      for (auto pIt = it->second.begin(); pIt != it->second.end(); ) {
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

  // 登録ルールのうち現在実行中かつ監視有効 (!isBypassed) の対象 PID を抽出
  std::unordered_set<DWORD> activeTargetPids;
  for (const auto &r : m_config.rules) {
    if (r.isBypassed)
      continue; // 監視除外プロセスのスレッドスキャン誘発を防止

    auto rIt = runningProcesses.find(ToLowerA(r.processName));
    if (rIt != runningProcesses.end()) {
      if (r.isChromium == 1) {
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

  // 全件 Not running の時はスレッド走査をスキップ
  // NtGetNextThread (ローレベル API) により、特定済み PID のスレッドのみを直接取得 (SNAPTHREAD 全廃)
  if (!activeTargetPids.empty()) {
    EnsureNtLoaded();
    if (s_pfnNtGetNextThread) {
      for (DWORD pid : activeTargetPids) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProc)
          continue;

        HANDLE hCurThread = NULL;
        while (s_pfnNtGetNextThread(hProc, hCurThread, THREAD_QUERY_INFORMATION, 0, 0, &hCurThread) == 0) {
          DWORD tid = GetThreadId(hCurThread);
          if (tid != 0) {
            THREADENTRY32 te = {0};
            te.dwSize = sizeof(te);
            te.th32OwnerProcessID = pid;
            te.th32ThreadID = tid;
            processThreads[pid].push_back(te);
          }
        }
        CloseHandle(hProc);
      }
    }
  }

  // 1 秒あたりのターン数計算 (100ms=10, 200ms=5, 500ms=2, 1000ms=1)
  int intervalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
  int turnsPerSec = 1000 / intervalMs;
  if (turnsPerSec < 1) turnsPerSec = 1;

  // 初回起動時: 前回の終了時ステータス記録と照合 (起動時引き継ぎ)
  static bool s_initialMatchDone = false;
  if (!s_initialMatchDone) {
    s_initialMatchDone = true;
    bool needSave = false;

    for (auto &rule : m_config.rules) {
      if (rule.lastAudioPid != 0 && rule.lastAudioTid != 0) {
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
            auto ptIt = processThreads.find(rule.lastAudioPid);
            if (ptIt != processThreads.end()) {
              for (const auto &te : ptIt->second) {
                if (te.th32ThreadID == rule.lastAudioTid) {
                  HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                  if (hThread) {
                    DWORD_PTR curAff = QueryThreadAffinityMask(hThread);
                    CloseHandle(hThread);
                    DWORD_PTR audioMask = rule.audioAffinityMask ? rule.audioAffinityMask : MakeCoreMask(rule.audioCore);
                    if (curAff == audioMask) {
                      matched = true;
                      rule.activePid = rule.lastAudioPid;
                      rule.activeAudioTid = rule.lastAudioTid;
                      rule.isAudioIsolated = true;
                      rule.isRunning = true;
                      rule.detectedThreadName = "PID " + std::to_string(rule.lastAudioPid) + " / TID " + std::to_string(rule.lastAudioTid);
                      m_trackedAudioThreads[rule.lastAudioPid][rule.lastAudioTid] = rule.detectedThreadName;
                      if (rule.isChromium == 1) {
                        rule.audioServicePid = rule.lastAudioPid;
                        m_chromiumThreadTracks[rule.lastAudioPid][rule.lastAudioTid].flag = 2;
                        m_chromiumAudioStates[rule.lastAudioPid].initialEvaluated = true;
                      }
                      stateChanged = true;
                    }
                  }
                  break;
                }
              }
            }
          }
        }

        if (!matched) {
          // 不一致・不在・未起動: 照合用記録を消去
          rule.lastAudioPid = 0;
          rule.lastAudioTid = 0;
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
  for (auto itTrack = m_trackedAudioThreads.begin();
       itTrack != m_trackedAudioThreads.end();) {
    if (allActivePids.find(itTrack->first) == allActivePids.end()) {
      itTrack = m_trackedAudioThreads.erase(itTrack);
    } else {
      ++itTrack;
    }
  }
  for (auto itCpu = m_prevThreadCpuTimes.begin();
       itCpu != m_prevThreadCpuTimes.end();) {
    if (allActivePids.find(itCpu->first) == allActivePids.end()) {
      itCpu = m_prevThreadCpuTimes.erase(itCpu);
    } else {
      ++itCpu;
    }
  }
  for (auto itC = m_cachedProcessTids.begin();
       itC != m_cachedProcessTids.end();) {
    if (allActivePids.find(itC->first) == allActivePids.end()) {
      itC = m_cachedProcessTids.erase(itC);
    } else {
      ++itC;
    }
  }
  for (auto itCnt = m_lastProcessThreadCount.begin();
       itCnt != m_lastProcessThreadCount.end();) {
    if (allActivePids.find(itCnt->first) == allActivePids.end()) {
      itCnt = m_lastProcessThreadCount.erase(itCnt);
    } else {
      ++itCnt;
    }
  }

  // 2. 各登録ルールのプロセスを検査・アフィニティ制御
  for (auto &rule : m_config.rules) {
    std::string targetLower = ToLowerA(rule.processName);
    auto it = runningProcesses.find(targetLower);

    if (it == runningProcesses.end() || it->second.empty()) {
      if (rule.isRunning) {
        if (rule.isAudioThreadSuspended && rule.activeAudioTid != 0) {
          HANDLE hThread =
              OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
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
        rule.wasHalfAutoPromoted = false;
        rule.chromiumScanAttempted = false;
        rule.searchPhase = 0;
        rule.searchTurns = 0;
        if (rule.audioServicePid != 0) {
          m_chromiumMaskedPids.erase(rule.audioServicePid);
          m_chromiumEvictedPids.erase(rule.audioServicePid);
          m_chromiumThreadTracks.erase(rule.audioServicePid);
          m_chromiumAudioStates.erase(rule.audioServicePid);
          m_chromiumScannedPids.erase(rule.audioServicePid);
          rule.audioServicePid = 0;
        }
        stateChanged = true;
      }
      continue;
    }

    rule.isRunning = true;

    // 監視除外・ペンディング (チェックON時)
    // はスレッド走査・アフィニティ制御・優先度制御を完全スキップ
    if (rule.isBypassed) {
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
      if (rule.audioServicePid != 0) {
        m_chromiumMaskedPids.erase(rule.audioServicePid);
        m_chromiumEvictedPids.erase(rule.audioServicePid);
        m_chromiumThreadTracks.erase(rule.audioServicePid);
        m_chromiumAudioStates.erase(rule.audioServicePid);
        m_chromiumScannedPids.erase(rule.audioServicePid);
        rule.audioServicePid = 0;
      }
      for (DWORD pid : it->second) {
        m_chromiumMaskedPids.erase(pid);
        m_chromiumScannedPids.erase(pid);
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

    // ── Chromium 専用パス: Audio Service のみスレッド走査、他は AffinityMask
    // のみ ──
    if (rule.isChromium == 1) {
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
          m_chromiumMaskedPids.erase(audioServicePid);
          m_chromiumEvictedPids.erase(audioServicePid);
          m_chromiumThreadTracks.erase(audioServicePid);
          m_chromiumAudioStates.erase(audioServicePid);
          m_chromiumScannedPids.erase(audioServicePid);
          audioServicePid = 0;
          rule.audioServicePid = 0;
          rule.detectedThreadName = "sleeping...";
          rule.isAudioIsolated = false;
          rule.activeAudioTid = 0;
          rule.chromiumScanAttempted = true;
          stateChanged = true;
          LogDebug("Chromium AudioService PID expired, returned to sleeping...");
        }
      }

      // Audio Service PID 未特定: 初動時またはBypass復帰時(!rule.chromiumScanAttempted)のみ走査試行
      // 未特定の場合は sleeping... に移行し、毎秒の全子プロセス走査を停止してコストをゼロ化
      if (audioServicePid == 0) {
        if (!rule.chromiumScanAttempted) {
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
              rule.searchTurns = 2 * turnsPerSec;
              rule.detectedThreadName = "Searching...";
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
        for (auto mIt = m_chromiumMaskedPids.begin();
             mIt != m_chromiumMaskedPids.end();) {
          if (allActivePids.find(*mIt) == allActivePids.end()) {
            mIt = m_chromiumMaskedPids.erase(mIt);
          } else {
            ++mIt;
          }
        }

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

      // Audio Service が特定されていない場合は sleeping... に遷移して仕事を終え待機
      if (audioServicePid == 0) {
        if (rule.detectedThreadName != "sleeping...") {
          rule.detectedThreadName = "sleeping...";
          stateChanged = true;
        }
        rule.searchPhase = 4;
        rule.searchTurns = 0;
        rule.isAudioIsolated = false;
        rule.activeAudioTid = 0;
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

      totalThreadCount = static_cast<int>(ptIt->second.size());
      auto &tracks = m_chromiumThreadTracks[audioServicePid];

      // 手順 1: 初回スレッド登録 (サイクル初期計測のみ実行)
      if (m_chromiumEvictedPids.find(audioServicePid) == m_chromiumEvictedPids.end()) {
        for (const auto &te : ptIt->second) {
          HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          if (hThread) {
            ULONG64 cycle = 0;
            QueryThreadCycleTime(hThread, &cycle);
            tracks[te.th32ThreadID].flag = 0;
            tracks[te.th32ThreadID].lastCycles = cycle;
            CloseHandle(hThread);
          }
        }
        m_chromiumEvictedPids.insert(audioServicePid);
        char eLog[128];
        snprintf(eLog, sizeof(eLog), "Chromium AudioService initial thread registration done: PID=%lu, threads=%zu",
                 audioServicePid, ptIt->second.size());
        LogDebug(eLog);
      }

      // 手順 2: 新探索フロー (Searching... -> Standby 10..3 -> Standby 2..1 -> sleeping...) & 定常時エフェメラル追従
      auto &audioState = m_chromiumAudioStates[audioServicePid];
      int activeAudioCount = 0;
      DWORD lastAudioTid = 0;

      // 新スレッド (初登場) を検知した場合は通常コアへ退避し、サイクルベースラインを記録
      for (const auto &te : ptIt->second) {
        if (tracks.find(te.th32ThreadID) == tracks.end()) {
          HANDLE hTh = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          if (hTh) {
            SetThreadAffinityMask(hTh, normalMask);
            ULONG64 curC = 0;
            QueryThreadCycleTime(hTh, &curC);
            tracks[te.th32ThreadID].flag = 0;
            tracks[te.th32ThreadID].lastCycles = curC;
            CloseHandle(hTh);
          }
        }
      }

      // スパイク防止検証ヘルパー:
      // flag != 2 の新TIDで Delta > 5M を発見後、250ms 間隔で追加2回計測
      // 計3回すべて同一TIDが首位かつ活動中なら真と判定
      auto verifySpikeTriad = [&](DWORD candTid) -> bool {
        for (int r = 0; r < 2; ++r) {
          Sleep(250);
          DWORD rTopTid = 0;
          ULONG64 rMaxDelta = 0;
          for (const auto &te : ptIt->second) {
            HANDLE hTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hTh) {
              ULONG64 curC = 0;
              if (QueryThreadCycleTime(hTh, &curC)) {
                auto &tr = tracks[te.th32ThreadID];
                if (tr.lastCycles > 0 && curC >= tr.lastCycles) {
                  ULONG64 d = curC - tr.lastCycles;
                  if (d > rMaxDelta) {
                    rMaxDelta = d;
                    rTopTid = te.th32ThreadID;
                  }
                }
                tr.lastCycles = curC;
              }
              CloseHandle(hTh);
            }
          }
          if (rTopTid != candTid || rMaxDelta < 1000000ULL) {
            return false;
          }
        }
        return true;
      };

      // オーディオ確定・隔離ヘルパー:
      // 対象スレッドを flag = 2 として audioMask を適用し、他スレッドは flag = 1 として normalMask を適用
      auto applyAudioIsolation = [&](DWORD audioTid) {
        for (const auto &te : ptIt->second) {
          if (te.th32ThreadID == audioTid) {
            tracks[te.th32ThreadID].flag = 2;
            HANDLE hTh = OpenThread(
                THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hTh) {
              SetThreadAffinityMask(hTh, audioMask);
              SetThreadPriority(hTh, rule.audioPriority);
              SetThreadIdealProcessor(hTh, static_cast<DWORD>(rule.audioCore));
              CloseHandle(hTh);
            }
          } else {
            tracks[te.th32ThreadID].flag = 1;
            HANDLE hOther = OpenThread(THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
            if (hOther) {
              SetThreadAffinityMask(hOther, normalMask);
              CloseHandle(hOther);
            }
          }
        }
        activeAudioCount = 1;
        lastAudioTid = audioTid;
        rule.isAudioIsolated = true;
        rule.activePid = audioServicePid;
        rule.activeAudioTid = audioTid;
        rule.detectedThreadName = "PID " + std::to_string(audioServicePid) + " / TID " + std::to_string(audioTid);
        rule.searchPhase = 0;
        rule.searchTurns = 0;
        audioState.initialEvaluated = true;
        stateChanged = true;
      };

      if (!rule.isAudioIsolated) {
        // 未確定状態: 新ステートマシン
        if (rule.searchPhase == 0) {
          rule.searchPhase = 1;
          rule.searchTurns = 2 * turnsPerSec;
          rule.detectedThreadName = "Searching...";
          stateChanged = true;
        }

        if (rule.searchPhase == 1) {
          // --- Phase 1: Searching... (2秒間: 2 * turnsPerSec ターン) ---
          DWORD topTid = 0;
          ULONG64 maxDelta = 0;

          for (const auto &te : ptIt->second) {
            auto &tr = tracks[te.th32ThreadID];
            HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hThread) {
              ULONG64 curCycles = 0;
              if (QueryThreadCycleTime(hThread, &curCycles)) {
                if (tr.lastCycles > 0 && curCycles >= tr.lastCycles) {
                  ULONG64 delta = curCycles - tr.lastCycles;
                  if (delta >= 5000000ULL && delta > maxDelta) {
                    maxDelta = delta;
                    topTid = te.th32ThreadID;
                  }
                }
                tr.lastCycles = curCycles;
              }
              CloseHandle(hThread);
            }
          }

          if (topTid != 0 && verifySpikeTriad(topTid)) {
            // 計3回確認完了: flag = 2 として確定・隔離
            applyAudioIsolation(topTid);
            char tLog[128];
            snprintf(tLog, sizeof(tLog),
                     "Chromium audio thread verified in Searching...: PID=%lu, TID=%lu, delta=%llu",
                     audioServicePid, topTid, maxDelta);
            LogDebug(tLog);
          } else {
            // 5M以上の活動が未出現またはスパイク破棄
            if (rule.detectedThreadName != "Searching...") {
              rule.detectedThreadName = "Searching...";
              stateChanged = true;
            }
            rule.searchTurns--;
            if (rule.searchTurns <= 0) {
              // 2秒経過: カウントダウン Phase 2 へ移行
              rule.searchPhase = 2;
              rule.searchTurns = 8 * turnsPerSec; // 10秒から3秒終了までの8秒間
              rule.detectedThreadName = "Standby 10";
              stateChanged = true;
            }
          }
        } else if (rule.searchPhase == 2) {
          // --- Phase 2: Standby 10..3 (8秒間: 8 * turnsPerSec ターン) ---
          // スレッド走査・計測・退避は一切行わず、秒数表示更新のみで完全休止 (CPU負荷 0.0%)
          int sec = 3 + (rule.searchTurns + turnsPerSec - 1) / turnsPerSec;
          std::string sName = "Standby " + std::to_string(sec);
          if (rule.detectedThreadName != sName) {
            rule.detectedThreadName = sName;
            stateChanged = true;
          }
          rule.searchTurns--;
          if (rule.searchTurns <= 0) {
            // カウントダウン終了: 終盤再確定 Phase 3 へ移行
            rule.searchPhase = 3;
            rule.searchTurns = 2 * turnsPerSec; // 2秒間 (Standby 2..1)
            rule.detectedThreadName = "Standby 2";
            stateChanged = true;
            // 基準サイクルタイムを再サンプリング
            for (const auto &te : ptIt->second) {
              HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
              if (hThread) {
                ULONG64 curCycles = 0;
                if (QueryThreadCycleTime(hThread, &curCycles)) {
                  tracks[te.th32ThreadID].lastCycles = curCycles;
                }
                CloseHandle(hThread);
              }
            }
          }
        } else if (rule.searchPhase == 3) {
          // --- Phase 3: 終盤再確定 Standby 2..1 (2秒間: 2 * turnsPerSec ターン) ---
          int sec = (rule.searchTurns + turnsPerSec - 1) / turnsPerSec;
          if (sec < 1) sec = 1;
          std::string sName = "Standby " + std::to_string(sec);
          if (rule.detectedThreadName != sName) {
            rule.detectedThreadName = sName;
            stateChanged = true;
          }

          DWORD topTid = 0;
          ULONG64 maxDelta = 0;

          for (const auto &te : ptIt->second) {
            auto &tr = tracks[te.th32ThreadID];
            HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hThread) {
              ULONG64 curCycles = 0;
              if (QueryThreadCycleTime(hThread, &curCycles)) {
                if (tr.lastCycles > 0 && curCycles >= tr.lastCycles) {
                  ULONG64 delta = curCycles - tr.lastCycles;
                  if (delta >= 5000000ULL && delta > maxDelta) {
                    maxDelta = delta;
                    topTid = te.th32ThreadID;
                  }
                }
                tr.lastCycles = curCycles;
              }
              CloseHandle(hThread);
            }
          }

          if (topTid != 0 && verifySpikeTriad(topTid)) {
            // 計3回確認完了: flag = 2 として確定・隔離
            applyAudioIsolation(topTid);
            char tLog[128];
            snprintf(tLog, sizeof(tLog),
                     "Chromium audio thread verified in Standby 2..1: PID=%lu, TID=%lu, delta=%llu",
                     audioServicePid, topTid, maxDelta);
            LogDebug(tLog);
          } else {
            rule.searchTurns--;
            if (rule.searchTurns <= 0) {
              // 終盤再確定でも未検出: Phase 4 (sleeping...) へ移行
              rule.searchPhase = 4;
              rule.searchTurns = 0;
              rule.detectedThreadName = "sleeping...";
              rule.isAudioIsolated = false;
              rule.activeAudioTid = 0;
              stateChanged = true;
            }
          }
        } else if (rule.searchPhase == 4) {
          // --- Phase 4: sleeping... (完全放置ナッジ) ---
          if (rule.detectedThreadName != "sleeping...") {
            rule.detectedThreadName = "sleeping...";
            stateChanged = true;
          }
          rule.isAudioIsolated = false;
          rule.activeAudioTid = 0;
        }
      } else {
        // --- 定常時フェーズ: エフェメラルスレッド追従 (3回検証による確実な交代) ---
        DWORD newTopTid = 0;
        ULONG64 newMaxDelta = 0;

        for (const auto &te : ptIt->second) {
          auto &tr = tracks[te.th32ThreadID];
          HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          if (hThread) {
            ULONG64 curCycles = 0;
            if (QueryThreadCycleTime(hThread, &curCycles)) {
              if (tr.lastCycles > 0 && curCycles >= tr.lastCycles) {
                ULONG64 delta = curCycles - tr.lastCycles;
                // flag != 2 の新TIDかつ 5M 超を走査
                if (tr.flag != 2 && delta >= 5000000ULL && delta > newMaxDelta) {
                  newMaxDelta = delta;
                  newTopTid = te.th32ThreadID;
                }
              }
              tr.lastCycles = curCycles;
            }
            CloseHandle(hThread);
          }
        }

        if (newTopTid != 0 && verifySpikeTriad(newTopTid)) {
          // 計3回確認完了: 通過スレッドに flag = 2 を付与し audio affinity を適用 (旧スレッドの退避は行わない)
          tracks[newTopTid].flag = 2;
          HANDLE hTh = OpenThread(
              THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, newTopTid);
          if (hTh) {
            SetThreadAffinityMask(hTh, audioMask);
            SetThreadPriority(hTh, rule.audioPriority);
            SetThreadIdealProcessor(hTh, static_cast<DWORD>(rule.audioCore));
            CloseHandle(hTh);
          }
          activeAudioCount++;
          lastAudioTid = newTopTid;
          rule.activeAudioTid = newTopTid;
          rule.detectedThreadName = "PID " + std::to_string(audioServicePid) + " / TID " + std::to_string(newTopTid);
          stateChanged = true;

          char sLog[128];
          snprintf(sLog, sizeof(sLog),
                   "Chromium ephemeral audio thread isolated: PID=%lu, TID=%lu, delta=%llu",
                   audioServicePid, newTopTid, newMaxDelta);
          LogDebug(sLog);
        }

        // 生存しているオーディオ確定スレッド (flag == 2) の確認
        for (const auto &te : ptIt->second) {
          auto tIt = tracks.find(te.th32ThreadID);
          if (tIt != tracks.end() && tIt->second.flag == 2) {
            activeAudioCount++;
            lastAudioTid = te.th32ThreadID;
          }
        }

        if (activeAudioCount == 0) {
          // オーディオスレッド消滅: 再探索 Searching... へ
          rule.isAudioIsolated = false;
          rule.searchPhase = 1;
          rule.searchTurns = 2 * turnsPerSec;
          rule.detectedThreadName = "Searching...";
          rule.activeAudioTid = 0;
          stateChanged = true;
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
          std::string label = "PID " + std::to_string(audioServicePid) + " / TID " + std::to_string(lastAudioTid);
          if (rule.detectedThreadName != label) {
            rule.detectedThreadName = label;
            stateChanged = true;
          }
        }
      }

      CloseHandle(hAsProc);

      if (rule.currentThreadCount != totalThreadCount) {
        rule.currentThreadCount = totalThreadCount;
        stateChanged = true;
      }
      if (stateChanged) {
        char rLog[256];
        snprintf(
            rLog, sizeof(rLog),
            "Rule State Changed: Proc=%s, PID=%lu, AudioThread='%s', Threads=%d",
            rule.processName.c_str(), rule.activePid,
            rule.detectedThreadName.c_str(), rule.currentThreadCount);
        LogDebug(rLog);
      }
      continue;
    }

    // ── 既存パス（Firefox, foobar2000, ゲーム等）──

    // 死亡PIDのクリーンアップ
    for (auto mIt = m_nonChromiumMaskedPids.begin();
         mIt != m_nonChromiumMaskedPids.end();) {
      if (allActivePids.find(*mIt) == allActivePids.end()) {
        mIt = m_nonChromiumMaskedPids.erase(mIt);
      } else {
        ++mIt;
      }
    }

    // 同名プロセスの全 PID (マルチプロセス) を走査
    for (DWORD pid : it->second) {
      auto ptIt = processThreads.find(pid);
      if (ptIt == processThreads.end() || ptIt->second.empty())
        continue;

      HANDLE hProcess = OpenProcess(
          PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
          FALSE, pid);
      if (!hProcess) {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "OpenProcess FAILED: pid=%lu, err=%lu",
                 pid, GetLastError());
        LogDebug(errBuf);
        continue;
      }

      // アイドル優先度クラス (IDLE_PRIORITY_CLASS) のプロセスはスキップ
      // (休眠タブ走査負荷の削減: ブラウザ系のみ)
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

      DWORD mainThreadTid =
          ptIt->second.empty() ? 0 : ptIt->second[0].th32ThreadID;

      // 永続トラッキングから事前解決
      // (ループ前にオーディオTIDを確定させ、重い走査をスキップ)
      {
        auto trackPidIt = m_trackedAudioThreads.find(pid);
        if (trackPidIt != m_trackedAudioThreads.end() &&
            !trackPidIt->second.empty()) {
          auto firstTracked = trackPidIt->second.begin();
          identifiedAudioTid = firstTracked->first;
          identifiedAudioLabel = firstTracked->second;
        }
      }

      // 1. 各スレッドの基本情報を収集 & 永続トラッキングを判定
      for (const auto &te : ptIt->second) {
        totalThreadCount++;
        aliveTids.insert(te.th32ThreadID);

        HANDLE hThread =
            OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE,
                       te.th32ThreadID);
        if (!hThread)
          continue;

        int priority = GetThreadPriority(hThread);
        DWORD_PTR currentAff = QueryThreadAffinityMask(hThread);

        threadInfos.push_back({te.th32ThreadID, te.tpBasePri, priority,
                               "", hThread, currentAff});

        // (A) 永続トラッキングチェック
        auto trackPidIt = m_trackedAudioThreads.find(pid);
        if (trackPidIt != m_trackedAudioThreads.end()) {
          auto tIt = trackPidIt->second.find(te.th32ThreadID);
          if (tIt != trackPidIt->second.end()) {
            identifiedAudioTid = te.th32ThreadID;
            identifiedAudioLabel = tIt->second;
          }
        }
      }

      // 2. 未特定の場合：新探索フロー (Searching... -> Standby 10..3 -> Standby 2..1 -> sleeping...)
      if (identifiedAudioTid == 0) {
        if (rule.searchPhase == 0) {
          rule.searchPhase = 1;
          rule.searchTurns = 2 * turnsPerSec;
          rule.detectedThreadName = "Searching...";
          stateChanged = true;
        }

        auto &sampleState = m_samplingStates[pid];

        if (rule.searchPhase == 1) {
          // --- Phase 1: Searching... (2秒間: 2 * turnsPerSec ターン) ---
          DWORD candTid = 0;
          ULONG64 maxDelta = 0;

          for (const auto &ti : threadInfos) {
            ULONG64 curCycle = 0;
            if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
              auto prevIt = sampleState.lastCycles.find(ti.tid);
              ULONG64 d = 0;
              if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
                d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
              }
              sampleState.lastCycles[ti.tid] = curCycle;

              // 未検査スレッドのモジュール/シグネチャ検査 (生涯1回)
              if (d > 0 && sampleState.inspectedTids.find(ti.tid) == sampleState.inspectedTids.end()) {
                sampleState.inspectedTids.insert(ti.tid);
                EnsurePsapiLoaded();
                void *sAddr = QueryThreadStartAddress(ti.hThread);
                std::string modName = "";
                if (sAddr && s_pfnGetMappedFileNameA) {
                  char mPath[MAX_PATH] = {0};
                  MEMORY_BASIC_INFORMATION mbi = {0};
                  if (VirtualQueryEx(hProcess, sAddr, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
                    if (s_pfnGetMappedFileNameA(hProcess, mbi.AllocationBase, mPath, sizeof(mPath)) > 0) {
                      modName = ToLowerA(mPath);
                    }
                  }
                }
                std::string stackSig = QueryCallstackAudioSignature(hProcess, ti.hThread);
                if (!stackSig.empty() ||
                    modName.find("dsound.dll") != std::string::npos ||
                    modName.find("winmm.dll") != std::string::npos ||
                    modName.find("pxtone") != std::string::npos ||
                    modName.find("audioses") != std::string::npos ||
                    modName.find("instantaud") != std::string::npos ||
                    modName.find("audio") != std::string::npos ||
                    modName.find("sound") != std::string::npos) {
                  sampleState.audioCandidateTids.insert(ti.tid);
                }
              }

              // 判定: Delta >= 5M、またはオーディオ候補で delta > 0
              if (d >= 5000000ULL || (sampleState.audioCandidateTids.count(ti.tid) && d > 0)) {
                if (d > maxDelta) {
                  maxDelta = d;
                  candTid = ti.tid;
                }
              }
            }
          }

          if (candTid != 0) {
            // 確定！
            identifiedAudioTid = candTid;
            identifiedAudioLabel = "PID " + std::to_string(pid) + " / TID " + std::to_string(identifiedAudioTid);
            m_trackedAudioThreads[pid][identifiedAudioTid] = identifiedAudioLabel;
            rule.isAudioIsolated = true;
            rule.activePid = pid;
            rule.activeAudioTid = identifiedAudioTid;
            rule.detectedThreadName = identifiedAudioLabel;
            rule.searchPhase = 0;
            rule.searchTurns = 0;
            stateChanged = true;
            m_samplingStates.erase(pid);

            char hLog[128];
            snprintf(hLog, sizeof(hLog),
                     "Audio thread identified in Searching...: PID=%lu, TID=%lu, delta=%llu",
                     pid, identifiedAudioTid, maxDelta);
            LogDebug(hLog);
          } else {
            if (rule.detectedThreadName != "Searching...") {
              rule.detectedThreadName = "Searching...";
              stateChanged = true;
            }
            rule.searchTurns--;
            if (rule.searchTurns <= 0) {
              // 2秒経過: カウントダウン Phase 2 へ移行
              rule.searchPhase = 2;
              rule.searchTurns = 8 * turnsPerSec; // 8秒間
              rule.detectedThreadName = "Standby 10";
              stateChanged = true;
            }
          }
        } else if (rule.searchPhase == 2) {
          // --- Phase 2: Standby 10..3 (8秒間: 8 * turnsPerSec ターン) ---
          // スレッド走査・計測・退避は一切行わず、秒数表示更新のみで完全休止 (CPU負荷 0.0%)
          int sec = 3 + (rule.searchTurns + turnsPerSec - 1) / turnsPerSec;
          std::string sName = "Standby " + std::to_string(sec);
          if (rule.detectedThreadName != sName) {
            rule.detectedThreadName = sName;
            stateChanged = true;
          }
          rule.searchTurns--;
          if (rule.searchTurns <= 0) {
            // カウントダウン終了: 終盤再確定 Phase 3 へ移行
            rule.searchPhase = 3;
            rule.searchTurns = 2 * turnsPerSec; // 2秒間 (Standby 2..1)
            rule.detectedThreadName = "Standby 2";
            stateChanged = true;
            // 基準サイクルタイムを再サンプリング
            for (const auto &ti : threadInfos) {
              ULONG64 curCycle = 0;
              if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
                sampleState.lastCycles[ti.tid] = curCycle;
              }
            }
          }
        } else if (rule.searchPhase == 3) {
          // --- Phase 3: 終盤再確定 Standby 2..1 (2秒間: 2 * turnsPerSec ターン) ---
          int sec = (rule.searchTurns + turnsPerSec - 1) / turnsPerSec;
          if (sec < 1) sec = 1;
          std::string sName = "Standby " + std::to_string(sec);
          if (rule.detectedThreadName != sName) {
            rule.detectedThreadName = sName;
            stateChanged = true;
          }

          DWORD candTid = 0;
          ULONG64 maxDelta = 0;

          for (const auto &ti : threadInfos) {
            ULONG64 curCycle = 0;
            if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
              auto prevIt = sampleState.lastCycles.find(ti.tid);
              ULONG64 d = 0;
              if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
                d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
              }
              sampleState.lastCycles[ti.tid] = curCycle;

              if (d >= 5000000ULL || (sampleState.audioCandidateTids.count(ti.tid) && d > 0)) {
                if (d > maxDelta) {
                  maxDelta = d;
                  candTid = ti.tid;
                }
              }
            }
          }

          if (candTid != 0) {
            // 終盤再判定で確定！
            identifiedAudioTid = candTid;
            identifiedAudioLabel = "PID " + std::to_string(pid) + " / TID " + std::to_string(identifiedAudioTid);
            m_trackedAudioThreads[pid][identifiedAudioTid] = identifiedAudioLabel;
            rule.isAudioIsolated = true;
            rule.activePid = pid;
            rule.activeAudioTid = identifiedAudioTid;
            rule.detectedThreadName = identifiedAudioLabel;
            rule.searchPhase = 0;
            rule.searchTurns = 0;
            stateChanged = true;
            m_samplingStates.erase(pid);

            char hLog[128];
            snprintf(hLog, sizeof(hLog),
                     "Audio thread identified in Standby 2..1: PID=%lu, TID=%lu, delta=%llu",
                     pid, identifiedAudioTid, maxDelta);
            LogDebug(hLog);
          } else {
            rule.searchTurns--;
            if (rule.searchTurns <= 0) {
              // 終盤再確定でも未検出: Phase 4 (sleeping...) へ移行
              rule.searchPhase = 4;
              rule.searchTurns = 0;
              rule.detectedThreadName = "sleeping...";
              rule.isAudioIsolated = false;
              rule.activeAudioTid = 0;
              stateChanged = true;
              m_samplingStates.erase(pid);
            }
          }
        } else if (rule.searchPhase == 4) {
          // --- Phase 4: sleeping... (完全放置ナッジ) ---
          if (rule.detectedThreadName != "sleeping...") {
            rule.detectedThreadName = "sleeping...";
            stateChanged = true;
          }
          rule.isAudioIsolated = false;
          rule.activeAudioTid = 0;
        }
      }

      // 3. アフィニティおよび優先度の適用 (オーディオスレッド特定時のみ退避・適用を実行)
      std::vector<DWORD> intruderTids;
      DWORD_PTR effectiveNormal = normalMask;

      if (identifiedAudioTid != 0) {
        // まず非オーディオスレッド (通常スレッド群)
        // を通常コア群へ先行退避し、真の侵入者を物理検出
        for (const auto &ti : threadInfos) {
          if (ti.tid == identifiedAudioTid)
            continue;

          // スキャン時点 (退避前) の物理アフィニティを検査:
          // オーディオコア (audioMask) を実行対象に含んでおり、かつ
          // (A) オーディオコア単独に自己バインドしている、または
          // (B)
          // 以前のサイクルで通常コア群へ退避させたにもかかわらず自らオーディオコアへ再バインドして居座るスレッド
          if ((ti.currentAffinity & audioMask) != 0) {
            bool isSpecificToAudio = ((ti.currentAffinity & ~audioMask) == 0);
            bool wasPreviouslyEvacuated =
                (m_appliedThreads.find(ti.tid) != m_appliedThreads.end());
            if (isSpecificToAudio || wasPreviouslyEvacuated) {
              intruderTids.push_back(ti.tid);
              char dbgBuf[256];
              snprintf(dbgBuf, sizeof(dbgBuf),
                       "IntruderCheck: PID=%lu, TID=%lu, aff=0x%llX, "
                       "audioMask=0x%llX, specific=%d, evac=%d",
                       pid, ti.tid, (unsigned long long)ti.currentAffinity,
                       (unsigned long long)audioMask, isSpecificToAudio,
                       wasPreviouslyEvacuated);
              LogDebug(dbgBuf);
            }
          }

          // 通常コア群 (effectiveNormal) へ退避 (差分適用: すでに一致している場合はシステムコールをスキップ)
          // ※ 通常スレッドに対する SetThreadIdealProcessor は廃止し、Windows 標準スケジューラの動的負荷分散に一任
          if (ti.currentAffinity != effectiveNormal) {
            DWORD_PTR prevMask = SetThreadAffinityMask(ti.hThread, effectiveNormal);
            if (prevMask != 0) {
              m_appliedThreads[ti.tid] = effectiveNormal;
              if (prevMask != effectiveNormal) {
                stateChanged = true;
              }
            }
          }
        }
      }

      bool hasIntruders = !intruderTids.empty();
      rule.hasIntruderThreads = hasIntruders;
      if (hasIntruders) {
        char hLog[128];
        snprintf(hLog, sizeof(hLog),
                 "IntrudersSummary: PID=%lu, count=%zu, hasIntruders=%d", pid,
                 intruderTids.size(), hasIntruders);
        LogDebug(hLog);
      }

      // Half判定時の自動昇格 (プロセス起動時、Bypass復帰時、スレッド再特定時など):
      // 設定が Idle (-15) だったならば、Lowest (-2) に自動昇格
      // ※ユーザーが手動で再び -15 に下げた場合は wasHalfAutoPromoted が true のため再昇格せず維持
      if (identifiedAudioTid != 0 && hasIntruders) {
        if (!rule.wasHalfAutoPromoted) {
          if (rule.audioPriority <= THREAD_PRIORITY_IDLE) {
            rule.audioPriority = THREAD_PRIORITY_LOWEST;
            stateChanged = true;
            char pLog[128];
            snprintf(pLog, sizeof(pLog),
                     "Auto-promoted audioPriority for PID=%lu from Idle(-15) to Lowest(-2) due to Half-Isolated",
                     pid);
            LogDebug(pLog);
          }
          rule.wasHalfAutoPromoted = true;
        }
      }

      // オーディオスレッドの適用優先度 (targetAudioPrio) の決定
      int targetAudioPrio = rule.audioPriority;

      // 侵入者スレッドの適用優先度 (オーディオスレッドの 1段階下に常に連動)
      int intruderPrio = GetOneStepLowerPriority(targetAudioPrio);

      // オーディオスレッドへの適用
      if (identifiedAudioTid != 0) {
        for (const auto &ti : threadInfos) {
          if (ti.tid == identifiedAudioTid) {
            audioDetectedInAny = true;
            if (primaryAudioThreadName.empty()) {
              primaryAudioThreadName = identifiedAudioLabel;
              primaryAudioPid = pid;
              primaryAudioTid = ti.tid;
            }

            // オーディオスレッド優先度設定 (昇格後の targetAudioPrio を適用)
            if (ti.priority != targetAudioPrio) {
              SetThreadPriority(ti.hThread, targetAudioPrio);
            }

            // Ideal Processor をオーディオコアに固定
            SetThreadIdealProcessor(ti.hThread,
                                    static_cast<DWORD>(rule.audioCore));

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
        for (const auto &ti : threadInfos) {
          if (ti.tid == intTid) {
            if (ti.priority != intruderPrio) {
              SetThreadPriority(ti.hThread, intruderPrio);
              char iLog[128];
              snprintf(iLog, sizeof(iLog),
                       "Adjusted true intruder thread priority to %d (audio is "
                       "%d): PID=%lu, TID=%lu",
                       intruderPrio, targetAudioPrio, pid, ti.tid);
              LogDebug(iLog);
            }
            break;
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
          HANDLE hOld =
              OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
          if (hOld) {
            ResumeThread(hOld);
            CloseHandle(hOld);
          }
          rule.isAudioThreadSuspended = false;
        }
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
      if (rule.activeAudioTid != 0) {
        if (rule.isAudioThreadSuspended) {
          HANDLE hOld =
              OpenThread(THREAD_SUSPEND_RESUME, FALSE, rule.activeAudioTid);
          if (hOld) {
            ResumeThread(hOld);
            CloseHandle(hOld);
          }
          rule.isAudioThreadSuspended = false;
        }
        rule.activeAudioTid = 0;
        rule.wasHalfAutoPromoted = false;
        stateChanged = true;
      }
      if (rule.detectedThreadName.empty()) {
        rule.detectedThreadName = "Searching...";
        rule.searchPhase = 1;
        rule.searchTurns = 2 * turnsPerSec;
        stateChanged = true;
      }
    }

    if (rule.currentThreadCount != totalThreadCount) {
      rule.currentThreadCount = totalThreadCount;
      stateChanged = true;
    }

    if (stateChanged) {
      char rLog[256];
      snprintf(
          rLog, sizeof(rLog),
          "Rule State Changed: Proc=%s, PID=%lu, AudioThread='%s', Threads=%d",
          rule.processName.c_str(), rule.activePid,
          rule.detectedThreadName.c_str(), rule.currentThreadCount);
      LogDebug(rLog);
    }
  }

  m_heuristicsStatusText = heuristicsStatus;

  return stateChanged;
}

std::string ThreadIsolator::GetHeuristicsStatusText() const { return ""; }

bool ThreadIsolator::IsHeuristicsActive() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto &r : m_config.rules) {
    if (r.isRunning && !r.isBypassed) {
      if (r.searchPhase == 1 || r.searchPhase == 3) {
        return true;
      }
    }
  }
  return false;
}

} // namespace ati
