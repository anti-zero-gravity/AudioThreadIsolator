# Audio Thread Isolator (ATI) - Management API Invocation Matrix (api_matrix.md)

This document provides a comprehensive technical reference of all Win32 and Native (NT) APIs currently invoked by ATI for process monitoring and thread isolation, detailing invocation conditions, frequency, and CPU overhead across all application operational states. It is strictly synchronized with the latest codebase implementation (`src/isolator.cpp`, `src/main.cpp`) and design specifications (`docs/spec.md`, `docs/spec_ja.md`).

---

## 1. Management API Reference and Overview (Active APIs)

| API Name | Scope | Purpose, Data Acquired, and Action Performed |
| :--- | :--- | :--- |
| **`CreateToolhelp32Snapshot`** | System-wide | `TH32CS_SNAPPROCESS`: Enumerates the process list (invoked once every INI polling interval × 10 only when unlaunched target processes exist; skipped at 0 invocations when all monitored targets are running). |
| **`Process32First` / `Next`** | System-wide | Enumerates process executable names, PIDs, and thread counts only during snapshot acquisition. 0 invocations when all targets are running. |
| **`GetExitCodeProcess`** | Process | Target PID liveness check (process termination detection). Completes in nanoseconds, replacing SNAPPROCESS. |
| **`OpenProcess`** | Process | Opens target process manipulation handles (for querying information, modifying affinity, adjusting priority, and configuring HighQoS). |
| **`CloseHandle`** | Common | Closes and releases opened process handles, thread handles, and snapshot handles. |
| **`SetProcessInformation`** | Process | **[HighQoS Enforcement]** Sets `ProcessPowerThrottling` (4) to disable Windows Power Throttling (EcoQoS). Removes execution speed throttling and preserves high-resolution timer accuracy. Process-level HighQoS automatically inherits to all underlying threads via the Windows kernel. |
| **`SetPriorityClass`** | Process | Configures target process priority class (Realtime, High, AboveNormal, Normal, BelowNormal, Idle). Automatically falls back to High if Realtime is denied without administrator privileges. |
| **`GetProcessAffinityMask`** | Process | Retrieves the current execution core affinity mask of the target process. |
| **`SetProcessAffinityMask`** | Process | Expands process execution core mask (normal cores + audio cores) or restricts it to normal cores. |
| **`NtGetNextThread`** | Thread | Directly enumerates child thread handles from target PID process handle via kernel direct syscall. Avoids system-wide thread scanning by querying only the target PID. |
| **`OpenThread`** | Thread | Opens target thread handles (for acquiring additional access rights and suspend control). |
| **`QueryThreadCycleTime`** | Thread | Retrieves accumulated CPU cycles consumed by thread. Used for computing Cycles Delta per polling cycle, identifying parent dispatch controller thread (1st by descending total cycles) upon initial Chromium registration, routine monitoring, and sampling (k=1..3) evaluation. |
| **`GetThreadPriority`** | Thread | Retrieves thread relative priority value (`-15` to `+15`). |
| **`SetThreadPriority`** | Thread | Sets audio thread priority to designated value, or applies per-slot priority (`audioPriorities[s]`). Also used to demote sibling non-audio threads and suppress intruder threads. |
| **`SetThreadAffinityMask`** | Thread | Pins thread execution core to dedicated audio core or evicts to normal core pool. Differential check triggers invocation only on change (0 invocations / skipped when already evicted). |
| **`SetThreadIdealProcessor`** | Thread | Designates designated audio core as preferred/ideal execution core for audio thread (applied exclusively to audio threads). |
| **`SuspendThread` / `ResumeThread`** | Thread | Col 4 verification pause toggle (`❚❚`) and per-slot suspend control. Temporarily suspends (mutes/isolates) or resumes audio thread execution. |
| **`NtQuerySystemInformation`** | System/Process | Queries `SystemProcessInformation` (5) to inspect thread kernel wait reasons (`WaitReason`, specifically `WaitReason == 4` DelayExecution). Used as fallback detection when priority 15 threads are scarce in `IgnoreSig:t`. |
| **`NtQueryInformationThread`** | Thread | Retrieves thread `ThreadBasicInformation` (stack base/limit address range) and TEB base address (used exclusively during Type 1 stack scanning). |
| **`ReadProcessMemory`** | Process | Reads thread call stack (4KB block) in batch to scan for signatures such as `audioses.dll` (used exclusively during Type 1 stack scanning; 0 invocations for Chromium and IgnoreSig:t). |

---

## 2. API Invocation Matrix by Operational State Combination (Per-Second Polling Behavior)

### Definition of States
1. **Not Running**: Target process is not launched (`Not running`)
2. **Bypassed**: Col 0 checkbox is ON (`Bypassed`)
3. **Chromium Sleeping**: AudioService process has not appeared or been identified (`sleeping...`)
4. **Chromium Routine Monitoring (`Standby`)**: AudioService identified but no audio playing (flag=0 routine monitoring)
5. **Chromium Sampling (`Searching...`)**: Delta exceeds threshold (3M), sampling top candidate at polling/4 intervals
6. **Chromium Isolated (`WASAPI`)**: Audio playing, audio thread confirmed and isolated (`flag=2`)
7. **Non-Chromium Searching (`Scanning...`)**: Searching for audio thread in standard application (includes 10-second countdown)
8. **Non-Chromium Isolated (`Isolated`)**: Audio thread identified and isolated in standard application (`Fully / Half-Isolated`)

---

### [Common System-Wide Phase (Process Monitoring, Liveness Check, Thread Enumeration)]

| API Name | Unlaunched Target Exists | All Running (incl. Bypassed) | Chromium Standby (`Standby`) | Chromium Sampling (`Searching`) | Chromium Playing (`WASAPI`) | Non-Chromium Searching (`Scanning`) | Non-Chromium Isolated (`Isolated`) | Single Call Cost |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`CreateToolhelp32Snapshot` (Process)** | Once every INI polling time × 10 | **0 (Skipped)** | **0 (Skipped)** | **0 (Skipped)** | **0 (Skipped)** | **0 (Skipped)** | **0 (Skipped)** | Minimal |
| **`Process32First` / `Next`** | Only during snapshot | **0 (Skipped)** | **0 (Skipped)** | **0 (Skipped)** | **0 (Skipped)** | **0 (Skipped)** | **0 (Skipped)** | Minimal |
| **`GetExitCodeProcess` (Liveness)** | 0 | Monitored process count / sec | Monitored process count / sec | Monitored process count / sec | Monitored process count / sec | Monitored process count / sec | Monitored process count / sec | Infinitesimal (several ns) |
| **`SetProcessInformation` (HighQoS)** | 0 | Running process count / sec | AudioService process count / sec (1) | AudioService process count / sec (1) | AudioService process count / sec (1) | Running process count / sec | Running process count / sec | Minimal |
| **`NtGetNextThread`** | 0 | 0 (When bypassed) | AudioService threads / sec (~14) | AudioService threads / sec (~14) | AudioService threads / sec (~14) | Target PID threads / sec (tens) | Target PID threads / sec (tens) | Minimal (Direct kernel) |

---

### [Per-Process & Thread Control Phase (API Invocations per Process State)]

| API Name | Not Running | Bypassed | Chromium Standby (`Standby`) | Chromium Sampling (`Searching`) | Chromium Playing (`WASAPI`) | Non-Chromium Searching (`Scanning`) | Non-Chromium Isolated (`Isolated`) | Single Call Cost |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`OpenProcess`** | 0 | 0 | 1 / sec (AudioService) | 1 / sec (AudioService) | 1 / sec (AudioService) | Each PID / sec | Each PID / sec | Minimal |
| **`GetProcessAffinityMask`** | 0 | 0 | 1 / sec | 1 / sec | 1 / sec | Each PID / sec | Each PID / sec | Minimal |
| **`SetProcessAffinityMask`** | 0 | 0 | Only on parent mask expansion | Only on parent mask expansion | Only on parent mask expansion | Only on newly detected PID | Only on newly detected PID | Low |
| **`SetPriorityClass`** | 0 | 0 | Only on config change | Only on config change | On confirm / change | Only on config change | On confirm / change | Low |
| **`OpenThread`** | 0 | 0 | On initial parent detect / change | Sampling targets (~10–14) | On confirm / change only | All threads / sec (20–40) | All threads / sec (20–40) | Minimal |
| **`QueryThreadCycleTime`** | 0 | 0 | flag=0 threads / sec (~10–14) | 3 samples at polling/4 intervals | flag=0/2 threads / sec (~10–14) | All threads / sec (Delta calc) | All threads / sec (Delta calc) | Minimal |
| **`GetThreadPriority`** | 0 | 0 | 0 | 0 | 0 | All threads / sec | All threads / sec | Minimal |
| **`SetThreadPriority`** | 0 | 0 | 0 | 0 | 1 on confirm / change only | On confirm only | 1 / sec (Audio maintain / slot) | Low |
| **`SetThreadAffinityMask` (Audio Core)** | 0 | 0 | 0 | 0 | 1 on confirm / change only | On confirm only | 1 / sec (Audio maintain / slot) | Low |
| **`SetThreadAffinityMask` (Normal Cores Evict)** | 0 | 0 | On initial parent detect (flag=1) | 0 | On flag=0 evict only | Un-evicted threads / sec | **On diff only (0 when evicted)** | Low |
| **`SetThreadIdealProcessor`** | 0 | 0 | 0 | 0 | 1 on confirm / change only | On confirm only | 1 / sec (Audio only) | Low |
| **`SuspendThread` / `ResumeThread`** | 0 | 0 | 0 | 0 | 0 | 0 | Only on user Col 4 toggle | Low |
| **`NtQuerySystemInformation`** (WaitReason) | 0 | 0 | 0 (N/A) | 0 (N/A) | 0 (N/A) | Fallback when pri=15 scarce | 0 (Routinely skipped) | Medium |
| **`NtQueryInformationThread`** | 0 | 0 | 0 (N/A) | 0 (N/A) | 0 (N/A) | Type 1 unidentified threads | 0 (Skipped) | Low |
| **`ReadProcessMemory`** (4KB Stack Scan) | 0 | 0 | 0 (N/A) | 0 (N/A) | 0 (N/A) | Type 1 unidentified threads | 0 (Skipped) | Low |
| **`CloseHandle`** | 0 | 0 | Open thread handles count | Open thread handles count | Open thread handles count | 20–45 / sec | 20–45 / sec | Minimal |

---

## 3. Low-Overhead & CPU Load Reduction Design Specifications

1. **Process-Level HighQoS (EcoQoS Disabling) with Kernel Inheritance**:
   - `SetProcessInformation` (`ControlMask = 1 | 4`, `StateMask = 0`) is issued at the process level to registered process handles.
   - Leverages Windows kernel parent-to-child thread inheritance to disable and prevent EcoQoS across all child threads (both existing and newly created) with minimal system call overhead, avoiding thread-by-thread enumeration loops.
   - For Chromium-based applications, HighQoS is applied specifically to the dedicated Audio Service process that handles audio stream rendering.
2. **Direct Thread Enumeration via Low-Level `NtGetNextThread`**:
   - Uses the low-level kernel API `NtGetNextThread` directly on the target process handle to enumerate thread handles belonging strictly to that PID.
   - Restricts scanning exclusively to the threads of monitored processes (14 to several dozen threads), achieving thread discovery with minimal kernel overhead.
3. **Chromium (Type 2) Cycles Delta Multi-Sample Evaluation**:
   - On initial registration, the thread with the highest accumulated CPU cycle time is identified as the parent dispatch controller thread and evicted to Excluded cores.
   - During routine monitoring, when Delta exceeds 3M/s, ATI performs 3 samples at polling/4 (125ms) intervals and isolates the thread with the maximum sum of 3 Deltas across the 4 latest measurements as the audio thread.
4. **Differential Affinity Mask Invocation for Normal Threads**:
   - For sibling threads already evicted to normal cores, `SetThreadAffinityMask` is issued only when a diff is detected (`currentAffinity != effectiveNormal`). When affinities match, the system call is skipped.
5. **Ideal Processor Assignment Exclusively for Confirmed Audio Threads**:
   - Ideal processor configuration (`SetThreadIdealProcessor`) is applied exclusively to confirmed audio playback threads and is not issued for normal non-audio threads.
6. **Fast Liveness Verification and Dynamic Snapshot Skipping**:
   - Process snapshot acquisition is completely skipped (0 invocations) while all monitored target processes are running.
   - Target process liveness is verified in nanoseconds via `GetExitCodeProcess`. `TH32CS_SNAPPROCESS` is issued only when unlaunched targets exist, throttled to once every INI polling interval × 10.
7. **Localized DelayExecution Fallback Query Execution**:
   - `QueryProcessThreadsWaitReason` (`NtQuerySystemInformation`) is issued strictly on an exception basis when priority 15 candidate threads are insufficient under `IgnoreSig:t`, pinpointing audio threads without routine polling load.
