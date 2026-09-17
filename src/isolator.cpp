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
        rule.detectedThreadName = (rule.isChromium == 1) ? "Standby" : "Scanning...";
      }
    }
  }
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

  // 1. 実行中の全プロセスを取得 (ANSI)
  HANDLE hSnapProc = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapProc == INVALID_HANDLE_VALUE)
    return false;

  std::unordered_map<std::string, std::vector<DWORD>> runningProcesses;
  std::unordered_map<DWORD, int> currentPidThreadCounts;
  PROCESSENTRY32 pe;
  pe.dwSize = sizeof(pe);

  if (Process32First(hSnapProc, &pe)) {
    do {
      std::string procNameLower = ToLowerA(pe.szExeFile);
      runningProcesses[procNameLower].push_back(pe.th32ProcessID);
      currentPidThreadCounts[pe.th32ProcessID] =
          static_cast<int>(pe.cntThreads);
    } while (Process32Next(hSnapProc, &pe));
  }
  CloseHandle(hSnapProc);

  // 登録ルールのうち現在実行中の対象 PID を抽出
  std::unordered_set<DWORD> activeTargetPids;
  for (const auto &r : m_config.rules) {
    auto rIt = runningProcesses.find(ToLowerA(r.processName));
    if (rIt != runningProcesses.end()) {
      for (DWORD p : rIt->second)
        activeTargetPids.insert(p);
    }
  }

  std::unordered_map<DWORD, std::vector<THREADENTRY32>> processThreads;

  // 全件 Not running の時はスレッドスナップショットを一切呼ばずにスキップ (CPU
  // 0.0%)
  if (!activeTargetPids.empty()) {
    // スレッド数が増減した PID、または未キャッシュの PID が存在するか判定
    bool needThreadSnapshot = false;
    for (DWORD pid : activeTargetPids) {
      int curCount = currentPidThreadCounts[pid];
      auto countIt = m_lastProcessThreadCount.find(pid);
      if (countIt == m_lastProcessThreadCount.end() ||
          countIt->second != curCount ||
          m_cachedProcessTids.find(pid) == m_cachedProcessTids.end()) {
        needThreadSnapshot = true;
        break;
      }
    }

    if (needThreadSnapshot) {
      HANDLE hSnapThread = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (hSnapThread != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (Thread32First(hSnapThread, &te)) {
          do {
            // 対象 PID のスレッドのみを抽出保持
            if (activeTargetPids.count(te.th32OwnerProcessID)) {
              processThreads[te.th32OwnerProcessID].push_back(te);
            }
          } while (Thread32Next(hSnapThread, &te));
        }
        CloseHandle(hSnapThread);
      }

      // TID キャッシュおよびスレッド数記録を更新
      for (DWORD pid : activeTargetPids) {
        m_lastProcessThreadCount[pid] = currentPidThreadCounts[pid];
        auto &tids = m_cachedProcessTids[pid];
        tids.clear();
        auto ptIt = processThreads.find(pid);
        if (ptIt != processThreads.end()) {
          for (const auto &te : ptIt->second) {
            tids.push_back(te.th32ThreadID);
          }
        }
      }
    } else {
      // スレッド数不変時: 保持している TID リストから THREADENTRY32 を再構成
      // (スナップ不要)
      for (DWORD pid : activeTargetPids) {
        auto itC = m_cachedProcessTids.find(pid);
        if (itC != m_cachedProcessTids.end()) {
          for (DWORD tid : itC->second) {
            THREADENTRY32 te = {0};
            te.dwSize = sizeof(te);
            te.th32ThreadID = tid;
            te.th32OwnerProcessID = pid;
            processThreads[pid].push_back(te);
          }
        }
      }
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
        if (rule.audioServicePid != 0) {
          m_chromiumMaskedPids.erase(rule.audioServicePid);
          m_chromiumEvictedPids.erase(rule.audioServicePid);
          m_chromiumThreadTracks.erase(rule.audioServicePid);
          m_chromiumAudioStates.erase(rule.audioServicePid);
          m_chromiumScannedPids.erase(rule.audioServicePid);
          rule.audioServicePid = 0;
        }
        rule.isRunning = false;
        rule.activePid = 0;
        rule.activeAudioTid = 0;
        rule.detectedThreadName = "";
        rule.isAudioIsolated = false;
        rule.wasHalfAutoPromoted = false;
        rule.chromiumScanAttempted = false;
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

      // 手順 1: 初回一括退避 (未退避なら全スレッドを normalMask へ退避しフラグ 0 で登録)
      if (m_chromiumEvictedPids.find(audioServicePid) == m_chromiumEvictedPids.end()) {
        EnsurePsapiLoaded();
        DWORD mainTid = ptIt->second.empty() ? 0 : ptIt->second[0].th32ThreadID;

        for (const auto &te : ptIt->second) {
          HANDLE hThread = OpenThread(
              THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          if (hThread) {
            SetThreadAffinityMask(hThread, normalMask);
            ULONG64 cycle = 0;
            QueryThreadCycleTime(hThread, &cycle);

            // メイン制御スレッド (.exe 起点または先頭スレッド) の特定:
            // 初回に一度だけ判定し、制御スレッド (CrUtilityMain) は flag = 1 (非オーディオ確定) として除外
            uint8_t initFlag = 0;
            bool isMainThread = (te.th32ThreadID == mainTid);
            if (!isMainThread) {
              void *sAddr = QueryThreadStartAddress(hThread);
              if (sAddr && s_pfnGetMappedFileNameA) {
                char mPath[MAX_PATH] = {0};
                MEMORY_BASIC_INFORMATION mbi = {0};
                if (VirtualQueryEx(hAsProc, sAddr, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
                  if (s_pfnGetMappedFileNameA(hAsProc, mbi.AllocationBase, mPath, sizeof(mPath)) > 0) {
                    std::string mod = ToLowerA(mPath);
                    if (mod.find(".exe") != std::string::npos) {
                      isMainThread = true;
                    }
                  }
                }
              }
            }

            if (isMainThread) {
              initFlag = 1;
              char mLog[128];
              snprintf(mLog, sizeof(mLog),
                       "Chromium main control thread marked as non-audio: PID=%lu, TID=%lu",
                       audioServicePid, te.th32ThreadID);
              LogDebug(mLog);
            }

            tracks[te.th32ThreadID].flag = initFlag;
            tracks[te.th32ThreadID].lastCycles = cycle;
            CloseHandle(hThread);
          }
        }
        m_chromiumEvictedPids.insert(audioServicePid);
        char eLog[128];
        snprintf(eLog, sizeof(eLog), "Chromium AudioService initial eviction done: PID=%lu, threads=%zu",
                 audioServicePid, ptIt->second.size());
        LogDebug(eLog);
      }

      // 手順 2: 初回 2000ms 安定化 Delta 判定 & 定常時機械的追従
      auto &audioState = m_chromiumAudioStates[audioServicePid];
      int activeAudioCount = 0;
      DWORD lastAudioTid = 0;

      if (!audioState.initialEvaluated) {
        // --- 初回判定フェーズ (2000ms 安定化走査) ---
        audioState.sampleTurns++;
        if (audioState.sampleTurns < 4) {
          // 2000ms (500ms * 4) 未満: 新規出現スレッドのみ normalMask 退避し、未評価のまま待機
          for (const auto &te : ptIt->second) {
            if (tracks.find(te.th32ThreadID) == tracks.end()) {
              HANDLE hThread = OpenThread(
                  THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
              if (hThread) {
                SetThreadAffinityMask(hThread, normalMask);
                ULONG64 cycle = 0;
                QueryThreadCycleTime(hThread, &cycle);
                tracks[te.th32ThreadID].flag = 0;
                tracks[te.th32ThreadID].lastCycles = cycle;
                CloseHandle(hThread);
              }
            }
          }
        } else {
          // 2000ms 経過: 未検査スレッド (flag == 0) の 2000ms Delta 差分を計測し、首位スレッドを特定
          DWORD topTid = 0;
          ULONG64 maxDelta = 0;

          for (const auto &te : ptIt->second) {
            auto tIt = tracks.find(te.th32ThreadID);
            if (tIt == tracks.end()) {
              HANDLE hThread = OpenThread(
                  THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
              if (hThread) {
                SetThreadAffinityMask(hThread, normalMask);
                tracks[te.th32ThreadID].flag = 1;
                CloseHandle(hThread);
              }
              continue;
            }

            if (tIt->second.flag == 0) {
              HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
              if (hThread) {
                ULONG64 curCycles = 0;
                if (QueryThreadCycleTime(hThread, &curCycles)) {
                  ULONG64 delta = (curCycles >= tIt->second.lastCycles)
                                      ? (curCycles - tIt->second.lastCycles)
                                      : 0;
                  if (topTid == 0 || delta > maxDelta) {
                    maxDelta = delta;
                    topTid = te.th32ThreadID;
                  }
                }
                CloseHandle(hThread);
              }
            }
          }

          if (topTid != 0) {
            // Delta 首位スレッドを無条件で flag = 2 (オーディオ確定) に決定し、アフィニティ・優先度を適用
            tracks[topTid].flag = 2;
            HANDLE hThread = OpenThread(
                THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, topTid);
            if (hThread) {
              SetThreadAffinityMask(hThread, audioMask);
              SetThreadPriority(hThread, rule.audioPriority);
              SetThreadIdealProcessor(hThread, static_cast<DWORD>(rule.audioCore));
              CloseHandle(hThread);
            }
            activeAudioCount++;
            lastAudioTid = topTid;

            char tLog[128];
            snprintf(tLog, sizeof(tLog),
                     "Chromium initial audio thread identified by 2000ms delta top: PID=%lu, TID=%lu, delta=%llu",
                     audioServicePid, topTid, maxDelta);
            LogDebug(tLog);
          }

          // 残りの未検査スレッドはすべて flag = 1 (非オーディオ確定) に移行
          for (auto &kv : tracks) {
            if (kv.second.flag == 0) {
              kv.second.flag = 1;
            }
          }
          audioState.initialEvaluated = true;
        }
      } else {
        // --- 定常時フェーズ (Delta監視全廃・新TID機械的追従) ---
        for (const auto &te : ptIt->second) {
          auto tIt = tracks.find(te.th32ThreadID);
          if (tIt == tracks.end()) {
            // 新規スレッド出現: 機械的に Audio Affinity / 優先度 / IdealProcessor を適用
            HANDLE hThread = OpenThread(
                THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hThread) {
              SetThreadAffinityMask(hThread, audioMask);
              SetThreadPriority(hThread, rule.audioPriority);
              SetThreadIdealProcessor(hThread, static_cast<DWORD>(rule.audioCore));
              CloseHandle(hThread);
            }
            tracks[te.th32ThreadID].flag = 2;
            activeAudioCount++;
            lastAudioTid = te.th32ThreadID;

            char nLog[128];
            snprintf(nLog, sizeof(nLog),
                     "Chromium new ephemeral thread mechanically isolated: PID=%lu, TID=%lu",
                     audioServicePid, te.th32ThreadID);
            LogDebug(nLog);
          } else if (tIt->second.flag == 2) {
            // 既存のオーディオ確定スレッド (現在生存中)
            activeAudioCount++;
            lastAudioTid = te.th32ThreadID;
          }
        }
      }

      CloseHandle(hAsProc);

      // 状態更新 & テスト実装のカラム表示判定
      size_t totalTrackedCount = tracks.size();
      if (totalTrackedCount > 9999) {
        if (rule.detectedThreadName != ">9999") {
          rule.detectedThreadName = ">9999";
          stateChanged = true;
        }
        rule.isAudioIsolated = (activeAudioCount > 0);
      } else if (activeAudioCount > 0) {
        if (!rule.isAudioIsolated) {
          rule.isAudioIsolated = true;
          stateChanged = true;
        }
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
      } else {
        // AudioService 内で音声未再生・待機中: Standby
        if (rule.isAudioIsolated) {
          rule.isAudioIsolated = false;
          stateChanged = true;
        }
        if (rule.detectedThreadName != "Standby") {
          rule.detectedThreadName = "Standby";
          stateChanged = true;
        }
        if (rule.activeAudioTid != 0) {
          rule.activeAudioTid = 0;
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
      continue;
    }

    // ── 既存パス（Firefox, foobar2000, ゲーム等）──

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

      // 2. 未特定の場合：無名スレッド判定 (既存隔離の引き継ぎ /
      // 10秒間サンプリング・最上delta除外・2〜10位候補検査)
      if (identifiedAudioTid == 0) {
        // (C-0) 既存隔離スレッドの引き継ぎ (Adopt)
        for (const auto &ti : threadInfos) {
          if (ti.tid == mainThreadTid)
            continue; // メインスレッドは除外
          if (ti.priority == rule.audioPriority) {
            DWORD_PTR currentAff = QueryThreadAffinityMask(ti.hThread);
            if (currentAff == audioMask) {
              identifiedAudioTid = ti.tid;
              identifiedAudioLabel =
                  "PID " + std::to_string(pid) + " / TID " + std::to_string(identifiedAudioTid);
              m_trackedAudioThreads[pid][identifiedAudioTid] =
                  identifiedAudioLabel;
              char aLog[128];
              snprintf(
                  aLog, sizeof(aLog),
                  "Adopted existing isolated audio thread: PID=%lu, TID=%lu",
                  pid, identifiedAudioTid);
              LogDebug(aLog);
              break;
            }
          }
        }
      }

      // (C) ビルトイン・ヒューリスティック探索エンジン
      // (常時内蔵・自動フォールバック)
      if (identifiedAudioTid == 0) {
        auto &sampleState = m_samplingStates[pid];
        FILETIME cr, ex, kr, ur;

        for (const auto &ti : threadInfos) {
          if (GetThreadTimes(ti.hThread, &cr, &ex, &kr, &ur)) {
            ULONGLONG totalTime =
                ((static_cast<ULONGLONG>(kr.dwHighDateTime) << 32) |
                 kr.dwLowDateTime) +
                ((static_cast<ULONGLONG>(ur.dwHighDateTime) << 32) |
                 ur.dwLowDateTime);
            auto prevIt = sampleState.lastCpuTime.find(ti.tid);
            ULONGLONG d = 0;
            if (prevIt != sampleState.lastCpuTime.end()) {
              d = (totalTime >= prevIt->second) ? (totalTime - prevIt->second)
                                                : 0;
              sampleState.accumulatedDelta[ti.tid] += d;
              sampleState.latestDelta[ti.tid] = d;
            }
            sampleState.lastCpuTime[ti.tid] = totalTime;

            // delta > 0
            // のスレッドが出現した場合、未検査なら生涯で1回だけスタック・モジュール検査
            if (d > 0 && sampleState.inspectedTids.find(ti.tid) ==
                             sampleState.inspectedTids.end()) {
              sampleState.inspectedTids.insert(ti.tid);

              EnsurePsapiLoaded();
              void *sAddr = QueryThreadStartAddress(ti.hThread);
              std::string modName = "";
              if (sAddr && s_pfnGetMappedFileNameA) {
                char mPath[MAX_PATH] = {0};
                MEMORY_BASIC_INFORMATION mbi = {0};
                if (VirtualQueryEx(hProcess, sAddr, &mbi, sizeof(mbi)) &&
                    mbi.AllocationBase) {
                  if (s_pfnGetMappedFileNameA(hProcess, mbi.AllocationBase,
                                              mPath, sizeof(mPath)) > 0) {
                    modName = ToLowerA(mPath);
                  }
                }
              }

              // コールスタックシグネチャも検査 (フォールバック B:
              // スレッド開始アドレスが exe の場合でも救済)
              std::string stackSig =
                  QueryCallstackAudioSignature(hProcess, ti.hThread);

              // オーディオ関連キーワード (dsound, winmm, pxtone, audioses,
              // instantaud, audio, sound, またはスタックシグネチャ検出)
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
          }
        }

        sampleState.sampleTurns++;

        if (sampleState.sampleTurns < sampleState.maxTurns) {
          // 探索中: Thread列に Heuristics と表示
          if (rule.detectedThreadName != "Heuristics") {
            rule.detectedThreadName = "Heuristics";
            stateChanged = true;
          }
        } else {
          // maxTurns 経過: 判定処理
          // オーディオ候補群の中から累積 delta 順にソート
          std::vector<std::pair<DWORD, ULONGLONG>> audioRanking;
          for (DWORD cTid : sampleState.audioCandidateTids) {
            ULONGLONG acc = sampleState.accumulatedDelta[cTid];
            audioRanking.push_back({cTid, acc});
          }
          std::sort(
              audioRanking.begin(), audioRanking.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });

          if (!audioRanking.empty()) {
            DWORD cand1 = audioRanking[0].first;
            bool shouldExtend = false;

            // 候補が2つ以上存在する場合、累積1位 vs
            // 2位の最終瞬間deltaをクロスチェック
            if (audioRanking.size() >= 2) {
              DWORD cand2 = audioRanking[1].first;
              ULONGLONG latest1 = sampleState.latestDelta[cand1];
              ULONGLONG latest2 = sampleState.latestDelta[cand2];

              // 累積1位が最終ターンで2位以下に失速している場合
              // (プリレンダリング後の休止疑い) かつ、まだ最大延長回数
              // (30ターン=15秒) 未満の場合に5秒延長
              if (latest1 < latest2 && sampleState.maxTurns < 30) {
                shouldExtend = true;
                sampleState.maxTurns += 10; // 5秒間 (10ターン) 延長
                char extLog[128];
                snprintf(extLog, sizeof(extLog),
                         "Heuristics: Cand1(TID=%lu, latest=%llu) < "
                         "Cand2(TID=%lu, latest=%llu) -> Extend 5s",
                         cand1, (unsigned long long)latest1, cand2,
                         (unsigned long long)latest2);
                LogDebug(extLog);
              }
            }

            if (shouldExtend) {
              if (rule.detectedThreadName != "Heuristics") {
                rule.detectedThreadName = "Heuristics";
                stateChanged = true;
              }
            } else {
              // 確定！
              identifiedAudioTid = cand1;
              identifiedAudioLabel =
                  "PID " + std::to_string(pid) + " / TID " + std::to_string(identifiedAudioTid);
              m_trackedAudioThreads[pid][identifiedAudioTid] =
                  identifiedAudioLabel;
              char hLog[128];
              snprintf(hLog, sizeof(hLog),
                       "Heuristics MATCH: PID=%lu, TID=%lu (accDelta=%llu, Label='%s')",
                       pid, identifiedAudioTid,
                       (unsigned long long)audioRanking[0].second,
                       identifiedAudioLabel.c_str());
              LogDebug(hLog);
              m_samplingStates.erase(pid); // 探索完了
            }
          } else {
            // オーディオ候補が 0 件 (無音・ミュート時): 強制確定せず Waiting
            // new thread へ移行
            if (rule.detectedThreadName != "Waiting new thread") {
              rule.detectedThreadName = "Waiting new thread";
              stateChanged = true;
            }
            rule.isAudioIsolated = false;
            m_samplingStates.erase(pid);
          }
        }
      }

      // 3. アフィニティおよび優先度の適用
      std::vector<DWORD> intruderTids;
      DWORD_PTR effectiveNormal = normalMask;

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
        if (identifiedAudioTid != 0 && (ti.currentAffinity & audioMask) != 0) {
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
      if (rule.detectedThreadName != "Heuristics" &&
          rule.detectedThreadName != "Waiting new thread") {
        std::string notDetectedName =
            (m_config.enableHeuristics && rule.enableHeuristics) ? "Scanning..."
                                                                 : "Standby";
        if (rule.detectedThreadName != notDetectedName) {
          rule.detectedThreadName = notDetectedName;
          stateChanged = true;
        }
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
      if (r.detectedThreadName == "Heuristics") {
        return true;
      }
      if (r.isChromium == 1) {
        return true;
      }
    }
  }
  return false;
}

} // namespace ati
