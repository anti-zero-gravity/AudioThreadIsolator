# Audio Thread Isolator (ATI) - Management API Invocation Matrix (api_matrix.md)

This document provides a comprehensive technical reference of all Win32 and Native (NT) APIs currently invoked by ATI for process monitoring and thread isolation, detailing invocation conditions, frequency, and CPU overhead across all application operational states. It is strictly synchronized with the latest codebase implementation (`src/isolator.cpp`, `src/main.cpp`) and design specifications (`docs/spec.md`, `docs/spec_ja.md`).

---

## 1. Management API Reference and Overview (Active APIs)

| API Name | Scope | Purpose, Data Acquired, and Action Performed |
| :--- | :--- | :--- |
| **`CreateToolhelp32Snapshot`** | System-wide | `TH32CS_SNAPPROCESS`: Enumerates the process list (invoked once every INI polling interval × 10 when unlaunched target processes exist or when `UnlistedMask` is active; skipped at 0 invocations when all monitored targets are running and `UnlistedMask=0`). |
| **`Process32First` / `Next`** | System-wide | Enumerates process executable names, PIDs, and thread counts only during snapshot acquisition. 0 invocations when all targets are running and `UnlistedMask=0`. |
| **`GetExitCodeProcess`** | Process | Target PID liveness check (process termination detection). Completes in nanoseconds, replacing SNAPPROCESS. |
| **`OpenProcess`** | Process | Opens target process manipulation handles (for querying information, modifying affinity, adjusting priority, and configuring HighQoS). |
| **`CloseHandle`** | Common | Closes and releases opened process handles, thread handles, and snapshot handles. |
| **`SetProcessInformation`** | Process | **[HighQoS Enforcement]** Issued once upon initial PID detection to set `ProcessPowerThrottling` (4) and disable Windows Power Throttling (EcoQoS), removing execution speed capping and maintaining high-resolution timers (1ms). Persists in `EPROCESS` across core affinity changes and inherits to all child threads. |
| **`SetPriorityClass`** | Process | Configures target process priority class (Realtime, High, AboveNormal, Normal, BelowNormal, Idle; Realtime is locked out in the picker when running without administrator privileges). |
| **`GetProcessAffinityMask`** | Process | Retrieves the current execution core affinity mask of the target process. |
| **`SetProcessAffinityMask`** | Process | Expands process execution core mask (normal cores + audio cores), restricts it to normal cores, applies Hard Affinity mask to Unlisted processes, or restores full-core mask on Unmask All. |
| **`NtGetNextThread`** | Thread | Directly enumerates child thread handles from target PID process handle via kernel direct syscall. Avoids system-wide thread scanning by querying only the target PID. |
| **`OpenThread`** | Thread | Opens target thread handles (for acquiring additional access rights and suspend control). |
| **`QueryThreadCycleTime`** | Thread | Retrieves accumulated CPU cycles consumed by thread. Used for computing Cycles Delta per polling cycle, identifying parent dispatch controller thread (1st by descending total cycles) upon initial Chromium registration, routine monitoring, and sampling (k=1..3) evaluation. |
| **`GetThreadPriority`** | Thread | Retrieves thread relative priority value (`-15` to `+15`). |
| **`SetThreadPriority`** | Thread | Sets audio thread priority to designated value, or applies per-slot priority (`audioPriorities[s]`). Also used to demote sibling non-audio threads and suppress intruder threads. |
| **`SetThreadAffinityMask`** | Thread | Pins thread execution core to dedicated audio core or evicts to normal core pool. Differential check triggers invocation only on change (0 invocations / skipped when already evicted). |
| **`SetThreadIdealProcessor`** | Thread | Designates the preferred execution core for the audio thread, while also assigning round-robin ideal processors to normal threads across normal cores to prevent core clustering. |
| **`SuspendThread` / `ResumeThread`** | Thread | Col 4 verification pause toggle (`❚❚`) and per-slot suspend control. Temporarily suspends (mutes/isolates) or resumes audio thread execution. |
| **`NtQuerySystemInformation`** | System/Process | Queries `SystemProcessInformation` (5) to inspect thread `WaitReason` (`WaitReason == 4` DelayExecution) and cumulative `ContextSwitches` / `Priority` for `audiodg.exe` main audio thread selection. Issued once every 3 seconds via the common 3-second gate while `RAATServer.exe` is unidentified (0 once confirmed), and strictly twice (on confirm/change + after 3s; 0 in routine state) for `audiodg.exe` dynamic priority display. |
| **`NtQueryInformationThread`** | Thread | Queries `ThreadBasicInformation` to capture `TID` and `AffinityMask` in a single pass during `NtGetNextThread` enumeration, read actual `Dynamic Priority` strictly twice (upon confirm/change + once a few seconds later; 0 in routine state), and obtain TEB base addresses during stack scanning of unidentified threads. |
| **`ReadProcessMemory`** | Process | Reads thread call stack (up to 32KB block) in batch to scan for audio signatures (`audioses.dll`, `roonmedia.dll`, `ws2_32.dll`, etc.). Used strictly once per thread lifetime on unidentified threads in Type 1 (standard apps), AppType: 4 (RoonAppliance), and AppType: 5 (Voicemeeter); 0 invocations for Chromium and once confirmed. |

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
| **`CreateToolhelp32Snapshot` (Process)** | Once every INI polling time × 10 | **0 (Skipped)** *(Once per INI×10 when UnlistedMask active)* | **0 (Skipped)** *(Once per INI×10 when UnlistedMask active)* | **0 (Skipped)** *(Once per INI×10 when UnlistedMask active)* | **0 (Skipped)** *(Once per INI×10 when UnlistedMask active)* | **0 (Skipped)** *(Once per INI×10 when UnlistedMask active)* | **0 (Skipped)** *(Once per INI×10 when UnlistedMask active)* | Minimal |
| **`Process32First` / `Next`** | Only during snapshot | **0 (Skipped)** *(Only during snapshot)* | **0 (Skipped)** *(Only during snapshot)* | **0 (Skipped)** *(Only during snapshot)* | **0 (Skipped)** *(Only during snapshot)* | **0 (Skipped)** *(Only during snapshot)* | **0 (Skipped)** *(Only during snapshot)* | Minimal |
| **`GetProcessAffinityMask` (Pre-flight)** | Newly spawned Unlisted PIDs only (max 2/turn) | 0 *(New Unlisted PIDs only on snapshot)* | 0 *(New Unlisted PIDs only on snapshot)* | 0 *(New Unlisted PIDs only on snapshot)* | 0 *(New Unlisted PIDs only on snapshot)* | 0 *(New Unlisted PIDs only on snapshot)* | 0 *(New Unlisted PIDs only on snapshot)* | Minimal |
| **`SetProcessAffinityMask` (Unlisted Eviction)** | Only when mask mismatch (max 2/turn) | 0 *(Only on mismatch)* | 0 *(Only on mismatch)* | 0 *(Only on mismatch)* | 0 *(Only on mismatch)* | 0 *(Only on mismatch)* | 0 *(Only on mismatch)* | Low |
| **`GetExitCodeProcess` (Liveness)** | 0 | Monitored process count / sec | Monitored process count / sec | Monitored process count / sec | Monitored process count / sec | Monitored process count / sec | Monitored process count / sec | Infinitesimal (several ns) |
| **`SetProcessInformation` (HighQoS)** | 0 | 0 (When bypassed) | **Once on new PID detect** (0 in steady state) | **0** | **0** | **Once on new PID detect** (0 in steady state) | **0** | Minimal |
| **`NtGetNextThread`** | 0 | 0 (When bypassed) | AudioService threads / sec (~14) | AudioService threads / sec (~14) | AudioService threads / sec (~14) | Target PID threads / turn (once / 3s for `RAATServer`) | **Once / 3s** for target PID (every turn for `audiodg`) | Minimal (Direct kernel) |

---

### [Per-Process & Thread Control Phase (API Invocations per Process State)]

| API Name | Not Running | Bypassed | Chromium Standby (`Standby`) | Chromium Sampling (`Searching`) | Chromium Playing (`WASAPI`) | Non-Chromium Searching (`Scanning`) | Non-Chromium Isolated (`Isolated`) | Single Call Cost |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`OpenProcess`** | 0 | 0 | 1 / sec (AudioService) | 1 / sec (AudioService) | 1 / sec (AudioService) | Each PID / sec | Each PID / sec | Minimal |
| **`GetProcessAffinityMask`** | 0 | 0 | 1 / sec | 1 / sec | 1 / sec | Each PID / sec | Each PID / sec | Minimal |
| **`SetProcessAffinityMask`** | 0 | 0 | Only on parent mask expansion | Only on parent mask expansion | Only on parent mask expansion | Only on newly detected PID | Only on newly detected PID | Low |
| **`SetPriorityClass`** | 0 | 0 | Only on config change | Only on config change | On confirm / change | Only on config change | On confirm / change | Low |
| **`OpenThread`** | 0 | 0 | On initial parent detect / change | Sampling targets (~10–14) | On confirm / change only | All threads / sec (20–40) | 1–2 for confirmed audio threads in routine state / All threads on diff only | Minimal |
| **`QueryThreadCycleTime`** | 0 | 0 | flag=0 threads / sec (~10–14) | 3 samples at polling/4 intervals | flag=0/2 threads / sec (~10–14) | All threads / sec (Delta calc) | 1–2 for confirmed audio threads in routine state / All threads on diff only | Minimal |
| **`GetThreadPriority`** | 0 | 0 | 0 | 0 | 1 / sec for confirmed audio thread (pre-flight check) | All threads / sec | 1–2 for confirmed audio threads in routine state / All threads on diff only | Minimal |
| **`SetThreadPriority`** | 0 | 0 | 0 | 0 | 1 on confirm / change only | On confirm only | 1 / sec (Audio maintain / slot) | Low |
| **`SetThreadAffinityMask` (Audio Core)** | 0 | 0 | 0 | 0 | 1 on confirm / change only | On confirm only | 1 / sec (Audio maintain / slot) | Low |
| **`SetThreadAffinityMask` (Normal Cores Evict)** | 0 | 0 | On initial parent detect (flag=1) | 0 | On flag=0 evict only | Un-evicted threads / sec | **On diff only (0 when evicted)** | Low |
| **`SetThreadIdealProcessor`** | 0 | 0 | 0 | 0 | 1 on confirm / change only | On diff only | **On diff only (Audio maintain + Normal RR)** | Low |
| **`SuspendThread` / `ResumeThread`** | 0 | 0 | 0 | 0 | 0 | 0 | Only on user Col 4 toggle | Low |
| **`NtQuerySystemInformation`** (WaitReason / CtxSw) | 0 | 0 | 0 (N/A) | 0 (N/A) | 0 (N/A) | Once every 6 turns during RAATServer search (5 skipped) | 2 fetches only on `audiodg` Dyn Pri (0 in routine) | Medium |
| **`NtQueryInformationThread`** | 0 | 0 | 0 (N/A) | 0 (N/A) | 2 Dyn Pri fetches only (confirm + 3s, 0 in routine) | Type 1 unidentified threads | Every 3s for TID+Affinity / 2 Dyn Pri fetches only (0 in routine) | Low |
| **`ReadProcessMemory`** (Up to 32KB Stack Scan) | 0 | 0 | 0 (N/A) | 0 (N/A) | 0 (N/A) | Type 1 / AppType: 4, 5 unidentified threads only (once/lifetime) | 0 (Skipped) | Low |
| **`CloseHandle`** | 0 | 0 | Open thread handles count | Open thread handles count | Open thread handles count | 20–45 / sec | 2–5 / sec in routine state / 20–45 on diff only | Minimal |

---

## 3. Low-Overhead & CPU Load Reduction Design Specifications

1. **Single-Shot HighQoS (EcoQoS Disabling) on Initial PID Detection with Kernel Inheritance**:
   - `SetProcessInformation` (`ControlMask = 1 | 4`, `StateMask = 0`) is issued strictly once upon newly detecting a PID across all registered process types (`AppType: 0–5`), with 0 steady-state re-invocations.
   - Leverages Windows kernel `EPROCESS` persistence and parent-to-child thread inheritance to maintain EcoQoS disabling and high-resolution (1ms) timers across all existing and newly created child threads—even across subsequent CPU core mask changes—without re-issuing system calls.
   - For Chromium-based applications, HighQoS is issued once upon detecting the dedicated Audio Service child process.
2. **Direct Thread Enumeration via Low-Level `NtGetNextThread`**:
   - Uses the low-level kernel API `NtGetNextThread` directly on the target process handle to enumerate thread handles belonging strictly to that PID.
   - Restricts scanning exclusively to the threads of monitored processes (14 to several dozen threads), achieving thread discovery with minimal kernel overhead.
3. **Chromium (Type 2) Cycles Delta Multi-Sample Evaluation**:
   - On initial registration, the thread with the highest accumulated CPU cycle time is identified as the parent dispatch controller thread and evicted to Excluded cores.
   - During routine monitoring, when Delta exceeds 3M/s, ATI performs 3 samples at polling/4 (125ms) intervals and isolates the thread with the maximum sum of 3 Deltas across the 4 latest measurements as the audio thread.
4. **Differential Affinity Mask Invocation for Normal Threads**:
   - For sibling threads already evicted to normal cores, `SetThreadAffinityMask` is issued only when a diff is detected (`currentAffinity != effectiveNormal`). When affinities match, the system call is skipped.
5. **Ideal Processor Assignment for Audio Pinning and Normal Thread Balancing**:
   - Confirmed audio playback threads are assigned their dedicated audio core as their ideal processor.
   - Normal sibling threads are also assigned ideal processors in a round-robin distribution across normal cores (`normalMask`) sorted by Cycles Delta, preventing core crowding and issued strictly on diffs.
6. **Fast Liveness Verification and Dynamic Snapshot Skipping**:
   - Process snapshot acquisition is completely skipped (0 invocations) while all monitored target processes are running.
   - Target process liveness is verified in nanoseconds via `GetExitCodeProcess`. `TH32CS_SNAPPROCESS` is issued only when unlaunched targets exist, throttled to once every INI polling interval × 10.
7. **Common 3-Second Steady-State Gate (`AppType: 1, 3, 4, 5` and Unidentified `RAATServer` Standby)**:
   - For isolated steady-state rules except `audiodg.exe` (`AppType: 0`) and Chromium (`AppType: 2`), as well as `RAATServer.exe` during unidentified standby (which uses single-shot `WaitReason == 4` detection without `Cycles Delta`), the common 3-second gate skips 5 out of 6 turns (at 500ms polling), reducing `NtGetNextThread`, `OpenProcess`, `OpenThread`, and `NtQuerySystemInformation` calls to 0 on skipped turns while preserving per-turn process liveness checks.
   - On active check turns, `NtQueryInformationThread(ThreadBasicInformation)` is issued once per thread inside the `NtGetNextThread` loop to retrieve `TID` and `AffinityMask` simultaneously, enabling `AppType: 1` co-resident thread (`Half-Isolated`) detection without redundant handle re-opening.
8. **Two-Stage `Dynamic Priority` Measurement and `audiodg.exe` Main Audio Thread Selection**:
   - Across all `AppType: 0–5`, reading measured `Dynamic Priority` for UI display is governed by `ShouldFetchDynamicPriority` (`GetTickCount64()`), firing **strictly twice**—once immediately upon thread confirmation or priority modification, and once more on the first active scan turn at `>= +3000ms` (~3.0 s for `AppType: 0, 2`; ~3.0–4.5 s for 3-second steady-state gated `AppType: 1, 3, 4, 5`) after temporary OS priority boosts decay—and remaining at 0 invocations thereafter during steady-state operation.
   - For PPL-protected `audiodg.exe` (`AppType: 0`) where `THREAD_QUERY_INFORMATION` is restricted, ATI inspects `WalkSystemProcessThreads` strictly on those two fetch turns and selects the `Priority` of the thread with the highest cumulative `ContextSwitches` (the active audio pump thread), avoiding un-decayed ALPC priority boosts on idle `WrQueue` worker threads.
