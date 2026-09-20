#undef UNICODE
#undef _UNICODE

#include "isolator.h"
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

static ULONG64 CalculateDeltaThreshold(double ruleDelta, double globalDelta, int intervalMs) {
  double d = (ruleDelta > 0.0) ? ruleDelta : globalDelta;
  if (d <= 0.0) d = 5.0;
  d = std::ceil(d * 10.0) / 10.0;
  ULONG64 base = static_cast<ULONG64>(d * 1000000.0);
  if (intervalMs <= 0) return base;
  return (base * static_cast<ULONG64>(intervalMs)) / 1000ULL;
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

  BOOL isWow64 = FALSE;
  IsWow64Process(hProcess, &isWow64);

  DWORD_PTR stackBase = 0;
  DWORD_PTR stackLimit = 0;
  SIZE_T bytesRead = 0;

  if (isWow64) {
    // WOW64 (32bit on 64bit Windows): TEB32 is TEB64 + 0x2000, StackBase is TEB32 + 4
    uintptr_t teb32 = reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 0x2000;
    uint32_t base32 = 0;
    uint32_t limit32 = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(teb32 + 4), &base32, sizeof(base32), &bytesRead) && bytesRead == sizeof(base32)) {
      stackBase = base32;
    }
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(teb32 + 8), &limit32, sizeof(limit32), &bytesRead) && bytesRead == sizeof(limit32)) {
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
      ReadProcessMemory(hProcess,
                        reinterpret_cast<LPCVOID>(
                            reinterpret_cast<uintptr_t>(tbi.TebBaseAddress) + 16),
                        &stackLimit, sizeof(stackLimit), &bytesRead);
    }
  }

  if (stackBase < 0x1000)
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

  std::vector<DWORD_PTR> ptrs;
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

  // モジュール名解決キャッシュ (同一 AllocationBase に対する API
  // 呼び出しの重複を防ぐ)
  std::unordered_map<DWORD_PTR, std::string> modNameCache;

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
        if (turnsPerSec < 1) turnsPerSec = 1;
        rule.detectedThreadName = "Searching...";
        rule.searchPhase = 1;
        rule.searchTurns = 2 * turnsPerSec;
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
  if (turnsPerSec < 1) turnsPerSec = 1;

  rule.searchPhase = 1;
  rule.searchTurns = 2 * turnsPerSec;
  if (rule.audioServicePid != 0) {
    rule.detectedThreadName = "PID " + std::to_string(rule.audioServicePid) + " / Searching...";
    rule.activePid = rule.audioServicePid;
  } else {
    rule.detectedThreadName = "Searching...";
  }
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
  if (ms <= 0) return 100;
  return ((ms + 99) / 100) * 100;
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
      if (!r.isRunning || (r.appType == 2 && r.audioServicePid == 0)) {
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
        HANDLE hNextThread = NULL;
        while (s_pfnNtGetNextThread(hProc, hCurThread, THREAD_QUERY_INFORMATION, 0, 0, &hNextThread) == 0) {
          if (hCurThread) {
            CloseHandle(hCurThread);
          }
          hCurThread = hNextThread;

          DWORD exitCode = 0;
          if (GetExitCodeThread(hCurThread, &exitCode) && exitCode == STILL_ACTIVE) {
            DWORD tid = GetThreadId(hCurThread);
            if (tid != 0) {
              THREADENTRY32 te = {0};
              te.dwSize = sizeof(te);
              te.th32OwnerProcessID = pid;
              te.th32ThreadID = tid;
              processThreads[pid].push_back(te);
            }
          }
        }
        if (hCurThread) {
          CloseHandle(hCurThread);
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
                      if (rule.appType == 2) {
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
        rule.detectedThreadName = rule.isBypassed ? "Bypassed" : "";
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

    // モード 0: 初回自動判定 (0: 未判定, 1: 非Chromium, 2: Chromium系, 3: DAW型高負荷)
    if (rule.appType == 0) {
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
            HANDLE hTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
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

    // ── Chromium 専用パス: Audio Service のみスレッド走査、他は AffinityMask のみ ──
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
          LogDebug(("Chromium AudioService PID expired, returned to " + rule.detectedThreadName).c_str());
        }
      }

      // Audio Service PID 未特定時: 初動時または 5秒周期のプロセス一覧更新時 (shouldScanProcesses) に走査
      // 平常ターンは走査せずコストゼロ化を維持
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
              rule.detectedThreadName = "PID " + std::to_string(pid) + " / Searching...";
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

      // 生存スレッドの TID セットを構築し、OS上で終了したスレッドのみ tracks から削除
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
      for (const auto &te : ptIt->second) {
        if (tracks.find(te.th32ThreadID) == tracks.end()) {
          HANDLE hTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
          if (hTh) {
            ULONG64 curC = 0;
            QueryThreadCycleTime(hTh, &curC);
            tracks[te.th32ThreadID].flag = 0;
            tracks[te.th32ThreadID].lastCycles = curC;
            CloseHandle(hTh);
          }
        }
      }

      // オーディオ確定・隔離ヘルパー:
      // 確定スレッドに audioMask を適用。他スレッド (flag != 2) は normalMask へ退避
      auto applyAudioIsolation = [&](DWORD audioTid) {
        tracks[audioTid].flag = 2;
        HANDLE hTh = OpenThread(
            THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, audioTid);
        if (hTh) {
          SetThreadAffinityMask(hTh, audioMask);
          SetThreadPriority(hTh, rule.audioPriority);
          SetThreadIdealProcessor(hTh, static_cast<DWORD>(rule.audioCore));
          CloseHandle(hTh);
        }
        for (const auto &te : ptIt->second) {
          if (te.th32ThreadID != audioTid && tracks[te.th32ThreadID].flag != 2) {
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
        audioState.samplingActive = false;
        audioState.step = 1;
        audioState.d.clear();
        stateChanged = true;
      };

      // ── Step 1: 初期ベースライン記録 ──
      if (audioState.step == 1) {
        for (const auto &te : ptIt->second) {
          if (tracks[te.th32ThreadID].flag == 0) {
            HANDLE hTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hTh) {
              ULONG64 curC = 0;
              QueryThreadCycleTime(hTh, &curC);
              tracks[te.th32ThreadID].lastCycles = curC;
              tracks[te.th32ThreadID].lastDelta = 0;
              CloseHandle(hTh);
            }
          }
        }
        audioState.step = 2;
        audioState.samplingActive = false;
        audioState.d.clear();
      }
      // ── Step 2: 待機ループ (最小ルーティン) ──
      else if (audioState.step == 2) {
        int normalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
        ULONG64 baseDelta = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, 0);
        ULONG64 minDelta = (baseDelta / 5ULL < 1000ULL) ? 1000ULL : (baseDelta / 5ULL);
        ULONG64 step2Threshold = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, normalMs);
        if (step2Threshold < minDelta) step2Threshold = minDelta;

        bool hasHighDelta = false;
        for (const auto &te : ptIt->second) {
          if (tracks[te.th32ThreadID].flag == 0) {
            HANDLE hTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (hTh) {
              ULONG64 curC = 0;
              if (QueryThreadCycleTime(hTh, &curC)) {
                if (tracks[te.th32ThreadID].lastCycles > 0 && curC >= tracks[te.th32ThreadID].lastCycles) {
                  ULONG64 delta = curC - tracks[te.th32ThreadID].lastCycles;
                  tracks[te.th32ThreadID].lastDelta = delta;
                  if (delta >= step2Threshold) {
                    hasHighDelta = true;
                  }
                }
                tracks[te.th32ThreadID].lastCycles = curC;
              }
              CloseHandle(hTh);
            }
          }
        }

        if (hasHighDelta) {
          // Delta >= 閾値相当を検知: TID 昇順 n で配列 d(n) を構築し Step 3 へ
          audioState.d.clear();
          std::vector<DWORD> flag0Tids;
          for (const auto &te : ptIt->second) {
            if (tracks[te.th32ThreadID].flag == 0) {
              flag0Tids.push_back(te.th32ThreadID);
            }
          }
          std::sort(flag0Tids.begin(), flag0Tids.end());

          for (DWORD t : flag0Tids) {
            ThreadSamplingEntry entry;
            entry.tid = t;
            entry.deltas[0] = static_cast<int64_t>(tracks[t].lastDelta);
            entry.lastCycle = tracks[t].lastCycles;
            audioState.d.push_back(entry);
          }

          audioState.k = 1;
          audioState.samplingActive = true;
          audioState.isDecline = rule.isDeclineBoost;
          audioState.step = 3;

          std::string sLabel = "PID " + std::to_string(audioServicePid) + " / Sampling (1/9)...";
          if (rule.detectedThreadName != sLabel) {
            rule.detectedThreadName = sLabel;
            stateChanged = true;
          }
        }
      }
      // ── Step 3 & Step 4: サンプリング蓄積ループ & 候補絞り込み判定 ──
      else if (audioState.step == 3) {
        int sampleIntervalMs = audioState.isDecline ? 1000 : (m_config.boostPollingIntervalMs > 0 ? m_config.boostPollingIntervalMs : 250);
        ULONG64 baseDelta = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, 0);
        ULONG64 minDelta = (baseDelta / 5ULL < 1000ULL) ? 1000ULL : (baseDelta / 5ULL);
        ULONG64 sampleThreshold = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, sampleIntervalMs);
        if (sampleThreshold < minDelta) sampleThreshold = minDelta;

        // k 回目の Delta 計測
        for (auto &entry : audioState.d) {
          if (entry.deltas[0] == -1) continue; // 脱落済み

          if (aliveTids.find(entry.tid) == aliveTids.end()) {
            entry.deltas[0] = -1; // スレッド消滅脱落
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
              if (audioState.k < 10) {
                entry.deltas[audioState.k] = static_cast<int64_t>(delta);
              }
            } else {
              if (audioState.k < 10) entry.deltas[audioState.k] = 0;
            }
            entry.lastCycle = curC;
          } else {
            entry.deltas[0] = -1;
          }
          CloseHandle(hTh);
        }

        audioState.k++;

        // k < 3 なら Step 3 継続
        if (audioState.k < 3) {
          std::string sLabel = "PID " + std::to_string(audioServicePid) + " / Sampling (" + std::to_string(audioState.k) + "/9)...";
          if (rule.detectedThreadName != sLabel) {
            rule.detectedThreadName = sLabel;
            stateChanged = true;
          }
        } else {
          // ── Step 4: 候補絞り込み判定 ──
          int normalMs = NormalizePollingInterval(m_config.pollingIntervalMs);
          ULONG64 baseDelta4 = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, 0);
          ULONG64 minDelta4 = (baseDelta4 / 5ULL < 1000ULL) ? 1000ULL : (baseDelta4 / 5ULL);
          ULONG64 step2Threshold = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, normalMs);
          if (step2Threshold < minDelta4) step2Threshold = minDelta4;

          // d(a) から順に Δ(0...k-1) がすべて 5M/s 相当の勢いか検査
          for (auto &entry : audioState.d) {
            if (entry.deltas[0] == -1) continue;

            for (int i = 0; i < audioState.k; ++i) {
              ULONG64 req = (i == 0) ? step2Threshold : sampleThreshold;
              if (entry.deltas[i] < static_cast<int64_t>(req)) {
                entry.deltas[0] = -1; // 基準割れ脱落
                break;
              }
            }
          }

          // 残存候補 (!=-1) のカウント
          int survivorCount = 0;
          DWORD survivorTid = 0;
          for (const auto &entry : audioState.d) {
            if (entry.deltas[0] != -1) {
              survivorCount++;
              survivorTid = entry.tid;
            }
          }

          if (survivorCount == 1) {
            // 【確定】1つだけに絞り込み成功！
            applyAudioIsolation(survivorTid);
            char tLog[128];
            snprintf(tLog, sizeof(tLog),
                     "Chromium audio thread verified: PID=%lu, TID=%lu (sampling k=%d)",
                     audioServicePid, survivorTid, audioState.k);
            LogDebug(tLog);
          } else if (survivorCount == 0 || audioState.k > 9) {
            // 全滅 または k > 9 で絞り込めず: リセットして Step 1 へ
            audioState.samplingActive = false;
            audioState.step = 1;
            audioState.d.clear();
          } else {
            // survivorCount > 1: 複数候補残存、Step 3 継続してさらにサンプリング
            std::string sLabel = "PID " + std::to_string(audioServicePid) + " / Sampling (" + std::to_string(audioState.k) + "/9)...";
            if (rule.detectedThreadName != sLabel) {
              rule.detectedThreadName = sLabel;
              stateChanged = true;
            }
          }
        }
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
        // オーディオスレッド消滅: sleeping... に落とさず PID / Searching... を維持して待機
        rule.isAudioIsolated = false;
        rule.searchPhase = 1;
        rule.searchTurns = 0;
        rule.activePid = audioServicePid;
        rule.activeAudioTid = 0;
        std::string searchLabel = "PID " + std::to_string(audioServicePid) + " / Searching...";
        if (rule.detectedThreadName != searchLabel && !audioState.samplingActive) {
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
          std::string label = "PID " + std::to_string(audioServicePid) + " / TID " + std::to_string(lastAudioTid);
          if (rule.detectedThreadName != label) {
            rule.detectedThreadName = label;
            stateChanged = true;
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

          if (rule.ignoreSigRank > 0) {
            // === IgnoreSig モード: スタック走査バイパス & 3回連続 (n位, n+1位) 一致判定 ===
            std::vector<std::pair<ULONG64, DWORD>> highDeltaThreads;
            for (const auto &ti : threadInfos) {
              ULONG64 curCycle = 0;
              if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
                auto prevIt = sampleState.lastCycles.find(ti.tid);
                ULONG64 d = 0;
                if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
                  d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
                }
                sampleState.lastCycles[ti.tid] = curCycle;

                ULONG64 deltaThreshold = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, intervalMs);
                if (d >= deltaThreshold) {
                  highDeltaThreads.push_back({d, ti.tid});
                }
              }
            }
            // Cycles Delta 降順ソート
            std::sort(highDeltaThreads.begin(), highDeltaThreads.end(),
                      [](const std::pair<ULONG64, DWORD> &a, const std::pair<ULONG64, DWORD> &b) {
                        return a.first > b.first;
                      });

            if (highDeltaThreads.size() >= static_cast<size_t>(rule.ignoreSigRank)) {
              DWORD curTidN = highDeltaThreads[rule.ignoreSigRank - 1].second;
              DWORD curTidN1 = (highDeltaThreads.size() >= static_cast<size_t>(rule.ignoreSigRank + 1))
                                   ? highDeltaThreads[rule.ignoreSigRank].second
                                   : 0;

              if (curTidN == rule.lastIgnoreSigTidN && curTidN1 == rule.lastIgnoreSigTidN1) {
                rule.ignoreSigMatchCount++;
                if (rule.ignoreSigMatchCount >= 3) {
                  // 3回連続一致: 境界ブレなし・安定確定！
                  candTid = curTidN;
                  maxDelta = highDeltaThreads[rule.ignoreSigRank - 1].first;
                  rule.ignoreSigMatchCount = 0;
                  rule.lastIgnoreSigTidN = 0;
                  rule.lastIgnoreSigTidN1 = 0;
                }
              } else {
                rule.lastIgnoreSigTidN = curTidN;
                rule.lastIgnoreSigTidN1 = curTidN1;
                rule.ignoreSigMatchCount = 1;
              }
            } else {
              rule.ignoreSigMatchCount = 0;
              rule.lastIgnoreSigTidN = 0;
              rule.lastIgnoreSigTidN1 = 0;
            }
          } else {
            // === 通常モード: 4KB スタック走査によるシグネチャ照合 & 最大 Delta 選定 ===
            for (const auto &ti : threadInfos) {
              ULONG64 curCycle = 0;
              if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
                auto prevIt = sampleState.lastCycles.find(ti.tid);
                ULONG64 d = 0;
                if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
                  d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
                }
                sampleState.lastCycles[ti.tid] = curCycle;

                // 秒換算 CyclesDelta の 1 ターン閾値
                ULONG64 deltaThreshold = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, intervalMs);

                // 未検査スレッドのモジュール/シグネチャ検査 (CyclesDelta 以上のスレッドに対してのみ検査、AppType: -1 はバイパス)
                if (rule.appType != -1 && d >= deltaThreshold && sampleState.inspectedTids.find(ti.tid) == sampleState.inspectedTids.end()) {
                  sampleState.inspectedTids.insert(ti.tid);
                  std::string stackSig = QueryCallstackAudioSignature(hProcess, ti.hThread);

                  if (rule.appType == 3) {
                    // モード 3: DAW型高負荷アプリ (Nuendo, Cubase 等)
                    // 初回: シグネチャ合致で候補登録、さらに timeclit に flag = 1 を刻印
                    if (!stackSig.empty()) {
                      sampleState.audioCandidateTids.insert(ti.tid);
                      bool isTimeCritical = (ti.priority == THREAD_PRIORITY_TIME_CRITICAL || ti.basePri >= 15);
                      if (isTimeCritical) {
                        m_threadFlags[pid][ti.tid] = 1; // 救済用フラグ
                      }
                    }
                  } else {
                    // モード 1: 非 Chromium (ゲーム・Spotify等)
                    // 4KB スタック走査のみ (開始アドレス走査は完全撤廃、pxtone 含む)
                    if (!stackSig.empty()) {
                      sampleState.audioCandidateTids.insert(ti.tid);
                    }
                  }
                }

                // 判定: (AppType: -1 || オーディオ候補) かつ (flag == 1 || timeclit) かつ (d > 0)
                // audit_process.py 準拠: シグネチャ合致(またはAppType: -1)かつ稼働中のスレッドの中から最大 Delta を選定
                bool isCandidate = (rule.appType == -1) || (sampleState.audioCandidateTids.count(ti.tid) > 0);
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
          }
        } else if (rule.searchPhase == 2) {
          // --- Phase 2: Standby 10..3 ---
          // スレッド走査・計測・退避は行わず待機
        } else if (rule.searchPhase == 3) {
          // --- Phase 3: 終盤再確定 Standby 2..1 (2秒間) ---
          DWORD candTid = 0;
          ULONG64 maxDelta = 0;

          if (rule.ignoreSigRank > 0) {
            // === IgnoreSig モード: スタック走査バイパス & 3回連続 (n位, n+1位) 一致判定 ===
            std::vector<std::pair<ULONG64, DWORD>> highDeltaThreads;
            for (const auto &ti : threadInfos) {
              ULONG64 curCycle = 0;
              if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
                auto prevIt = sampleState.lastCycles.find(ti.tid);
                ULONG64 d = 0;
                if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
                  d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
                }
                sampleState.lastCycles[ti.tid] = curCycle;

                ULONG64 deltaThreshold = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, intervalMs);
                if (d >= deltaThreshold) {
                  highDeltaThreads.push_back({d, ti.tid});
                }
              }
            }
            // Cycles Delta 降順ソート
            std::sort(highDeltaThreads.begin(), highDeltaThreads.end(),
                      [](const std::pair<ULONG64, DWORD> &a, const std::pair<ULONG64, DWORD> &b) {
                        return a.first > b.first;
                      });

            if (highDeltaThreads.size() >= static_cast<size_t>(rule.ignoreSigRank)) {
              DWORD curTidN = highDeltaThreads[rule.ignoreSigRank - 1].second;
              DWORD curTidN1 = (highDeltaThreads.size() >= static_cast<size_t>(rule.ignoreSigRank + 1))
                                   ? highDeltaThreads[rule.ignoreSigRank].second
                                   : 0;

              if (curTidN == rule.lastIgnoreSigTidN && curTidN1 == rule.lastIgnoreSigTidN1) {
                rule.ignoreSigMatchCount++;
                if (rule.ignoreSigMatchCount >= 3) {
                  // 3回連続一致: 境界ブレなし・安定確定！
                  candTid = curTidN;
                  maxDelta = highDeltaThreads[rule.ignoreSigRank - 1].first;
                  rule.ignoreSigMatchCount = 0;
                  rule.lastIgnoreSigTidN = 0;
                  rule.lastIgnoreSigTidN1 = 0;
                }
              } else {
                rule.lastIgnoreSigTidN = curTidN;
                rule.lastIgnoreSigTidN1 = curTidN1;
                rule.ignoreSigMatchCount = 1;
              }
            } else {
              rule.ignoreSigMatchCount = 0;
              rule.lastIgnoreSigTidN = 0;
              rule.lastIgnoreSigTidN1 = 0;
            }
          } else {
            // === 通常モード: 4KB スタック走査によるシグネチャ照合 & 最大 Delta 選定 ===
            for (const auto &ti : threadInfos) {
              ULONG64 curCycle = 0;
              if (QueryThreadCycleTime(ti.hThread, &curCycle)) {
                auto prevIt = sampleState.lastCycles.find(ti.tid);
                ULONG64 d = 0;
                if (prevIt != sampleState.lastCycles.end() && prevIt->second > 0) {
                  d = (curCycle >= prevIt->second) ? (curCycle - prevIt->second) : 0;
                }
                sampleState.lastCycles[ti.tid] = curCycle;

                ULONG64 deltaThreshold = CalculateDeltaThreshold(rule.cyclesDelta, m_config.defaultCyclesDelta, intervalMs);
                if (rule.appType != -1 && d >= deltaThreshold && sampleState.inspectedTids.find(ti.tid) == sampleState.inspectedTids.end()) {
                  sampleState.inspectedTids.insert(ti.tid);
                  std::string stackSig = QueryCallstackAudioSignature(hProcess, ti.hThread);
                  if (rule.appType == 3) {
                    if (!stackSig.empty()) {
                      sampleState.audioCandidateTids.insert(ti.tid);
                      if (ti.priority == THREAD_PRIORITY_TIME_CRITICAL || ti.basePri >= 15) {
                        m_threadFlags[pid][ti.tid] = 1;
                      }
                    }
                  } else {
                    if (!stackSig.empty()) {
                      sampleState.audioCandidateTids.insert(ti.tid);
                    }
                  }
                }

                bool isCandidate = (rule.appType == -1) || (sampleState.audioCandidateTids.count(ti.tid) > 0);
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
          }
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
      // 未特定時のフェーズ進行 (全PID走査完了後にプロセスグループ単位で1ターン進行)
      if (rule.searchPhase == 1) {
        if (rule.detectedThreadName != "Searching...") {
          rule.detectedThreadName = "Searching...";
          stateChanged = true;
        }
        rule.searchTurns--;
        if (rule.searchTurns <= 0) {
          rule.searchPhase = 2;
          rule.searchTurns = 8 * turnsPerSec;
          rule.detectedThreadName = "Standby 10";
          stateChanged = true;
        }
      } else if (rule.searchPhase == 2) {
        int sec = 3 + (rule.searchTurns + turnsPerSec - 1) / turnsPerSec;
        std::string sName = "Standby " + std::to_string(sec);
        if (rule.detectedThreadName != sName) {
          rule.detectedThreadName = sName;
          stateChanged = true;
        }
        rule.searchTurns--;
        if (rule.searchTurns <= 0) {
          rule.searchPhase = 3;
          rule.searchTurns = 2 * turnsPerSec;
          rule.detectedThreadName = "Standby 2";
          stateChanged = true;
        }
      } else if (rule.searchPhase == 3) {
        int sec = (rule.searchTurns + turnsPerSec - 1) / turnsPerSec;
        if (sec < 1) sec = 1;
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

bool ThreadIsolator::IsFastPollingRequired() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto &pair : m_chromiumAudioStates) {
    if (pair.second.samplingActive && !pair.second.isDecline) {
      return true;
    }
  }
  return false;
}

UINT ThreadIsolator::GetDesiredPollingIntervalMs() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto &pair : m_chromiumAudioStates) {
    if (pair.second.samplingActive) {
      if (pair.second.isDecline) {
        return 1000;
      }
      int boostMs = m_config.boostPollingIntervalMs > 0 ? m_config.boostPollingIntervalMs : 250;
      return static_cast<UINT>(boostMs);
    }
  }
  return static_cast<UINT>(m_config.pollingIntervalMs > 0 ? m_config.pollingIntervalMs : 500);
}

} // namespace ati
