# Audio Thread Isolator (ATI) - Management API Invocation Matrix

This technical reference documents all Win32 and NT Native APIs invoked by ATI for process discovery, thread identification, core isolation, and priority management, along with their per-call cost granularity and invocation timing across all polling tiers (`INI×10`, `INI×6`, `INI×3`, `INI×1`, `INI/4`, one-shot events) and `AppType (0..5)`.

---

## 1. Active API Inventory, Roles, and Cost Granularity

### Cost Granularity Categories
- **`OS-Batch (1)`**: Dumps OS-wide process/thread info in a single call and extracts target PID records from the buffer without per-thread syscall loops.
- **`Per-PID (1)`**: Invoked once per target process PID.
- **`Fixed TID (1-2)`**: Invoked only on identified audio threads (1 per single-slot app, up to 2 for Voicemeeter).
- **`All TIDs (N)`**: Invoked in a loop across all `N` live threads inside the target PID (e.g., 15-50 threads).

### 1.1 OS-Wide & Process-Level APIs (14 Entries)

| API Name | Scope | Cost Granularity | Overhead | Primary Role |
| :--- | :---: | :---: | :---: | :--- |
| `CreateToolhelp32Snapshot` | OS-Wide | OS-Batch (1) | Low | Capture snapshot of all running processes |
| `Process32First / Next` | OS-Wide | OS-Batch (Scan) | Tiny | Enumerate EXE names and PIDs from snapshot |
| `NtQuerySystemInformation` | OS-Wide | OS-Batch (1) | Medium | Bulk query `WaitReason`, `CtxSw`, `DynPri` |
| `GetExitCodeProcess` | PID | Per-PID (1) | Negligible | Verify registered PID liveness (nanoseconds) |
| `OpenProcess` | PID | Per-PID (1) | Tiny | Acquire target process handle |
| `CloseHandle` | Common | Per-Handle | Tiny | Release process, thread, and snapshot handles |
| `SetProcessInformation` | PID | Once (Initial) | Tiny | Apply HighQoS (disable EcoQoS, inherit child) |
| `GetProcessAffinityMask` | PID | Per-PID (1) | Tiny | Read current process execution core mask |
| `SetProcessAffinityMask` | PID | On Mismatch (1) | Low | Update process execution core affinity mask |
| `GetPriorityClass` | PID | Per-PID (1) | Tiny | Query current process priority class |
| `SetPriorityClass` | PID | On Mismatch (1) | Low | Apply configured process priority class |
| `QueryFullProcessImageNameA` | PID | Once (Initial) | Low | Resolve EXE path for initial Chromium check |
| `NtQueryInformationProcess` | Child | Unresolved Only | Low | Read PEB to locate Chromium AudioService PID |
| `NtSuspendProcess / Resume` | PID | Manual Only (1) | Low | Suspend/resume entire `audiodg.exe` process |

#### Runtime Cost & Behavior Notes (1.1)
- **`CreateToolhelp32Snapshot` + `Process32First / Next`**: **0 calls (skipped)** when all rules are running (including `Type 2` with `audioServicePid != 0`), `UnlistedMask == 0`, and no forced rescan is requested. Excludes `PID <= 4`; triggers immediate rescan on rule termination, rule update, or manual restore.
- **`NtQuerySystemInformation` (`SystemProcessInformation = 5`)**: Dumps OS-wide process/thread data to retrieve `WaitReason` and `ContextSwitches` (unavailable via `NtQueryInformationThread`) or act as PPL fallback. In standard environments it runs only in cases 1 & 2 below (**0 lifetime calls** for `Type 1, 2, 3, 5` and `RoonAppliance.exe`), and case 3 triggers only under PPL access restriction:
  1. **`RAATServer.exe` (`Type 4`)**: Polls once every 3.0s (`INI × 6`) only while unresolved to find `WaitReason == 4` (**0 calls** once isolated).
  2. **`audiodg.exe` (`Type 0`) `Dyn Pri`**: Called twice (`0s + ~3s`) on initial detection or priority change to query `Dyn Pri` of the top `ContextSwitches` thread (**0 calls** in steady state).
  3. **PPL-Protected Thread Enumeration Fallback**: If `OpenProcess(PROCESS_QUERY_INFORMATION)` is denied and `NtGetNextThread` fails (e.g., PPL-restricted `audiodg.exe`), falls back to `WalkSystemProcessThreads` to enumerate TIDs.
- **`GetExitCodeProcess`**: Reads the kernel `EPROCESS` exit code directly instead of taking per-turn snapshots. Restricted strictly to running `PID > 4` entries registered in `m_config.rules`, preventing unrelated app or system exits from triggering `SNAPPROCESS`.
- **`SetProcessInformation` (`ProcessPowerThrottling = 4`)**: Issued once on initial PID detection (**0 calls** in steady state). Kernel inheritance propagates HighQoS (1ms timer resolution, no throttling) to all child threads.
- **`SetProcessAffinityMask` / `SetPriorityClass`**: Compares against `Get` first and issues `Set` only on mismatch (exception: initial evacuation of `Type 2` non-audio child PIDs calls `SetProcessAffinityMask` once without `Get`). `SetPriorityClass` is **0 calls** when Col 3 is unattached (light green).

---

### 1.2 Thread Enumeration, Search & Profiling APIs (10 Entries)

| API Name | Scope | Cost Granularity | Overhead | Primary Role |
| :--- | :---: | :---: | :---: | :--- |
| `NtGetNextThread` | TID | All TIDs (N) | Tiny | Sequentially enumerate handles inside PID |
| `NtQueryInformationThread` | TID | All / Fixed TID | Tiny | Read `TID`, `Affinity`, `DynPri`, and `TEB` |
| `GetThreadId` | TID | Conditional | Tiny | Fallback `TID` lookup on `NtQuery` fail / reset |
| `OpenThread` | TID | N Scan / 1-2 Steady | Tiny | Acquire thread handle for control/profiling |
| `GetExitCodeThread` | TID | Cleanup Only | Tiny | Check thread liveness on unmask/exit restore |
| `QueryThreadCycleTime` | TID | N Scan / 0 Steady | Tiny | Read cumulative CPU cycles for Delta |
| `IsWow64Process` | PID | Once (Lifetime) | Tiny | Detect 32/64-bit TEB layout for stack walk |
| `ReadProcessMemory` | Memory | Once (Lifetime) | Low | Read up to 32KB thread call stack |
| `VirtualQueryEx` | Memory | Once (Lifetime) | Low | Check executable region of return address |
| `GetMappedFileNameA` | Memory | Once (Lifetime) | Low | Identify DLL holding call stack frames |

#### Runtime Cost & Behavior Notes (1.2)
- **`NtGetNextThread` + `NtQueryInformationThread` (`ThreadBasicInformation`)**: Enumerates only the `N` threads inside the target PID in 1 pass, collecting both `TID` and `AffinityMask` (passed via `knownAffinity` to `ApplyAudioThreadSettings` / `EvacuateNormalThreadSettings` to suppress duplicate syscalls; unused `Type 2` TEB query removed). Falls back to `GetThreadId` on query failure or cleanup.
- **Steady-State `OpenThread` Reduction**: Once isolated, `Type 1, 3, 4, 5` skip normal threads (opening only 1-2 fixed audio TIDs), and `Type 2` skips `flag=1` siblings plus cache-verified `alreadyEvacuated` normal threads.
- **32KB Call Stack Walk (`ReadProcessMemory`, etc.)**: Runs **once in lifetime** per uninspected thread (`inspectedTids`) exceeding the `Cycles Delta` threshold to identify audio DLLs (`audioses.dll`, `asio`, `roonmedia.dll`, `ws2_32.dll`, etc.). **0 calls** after isolation and for `Type 0, 2`, and `RAATServer`.

---

### 1.3 Thread Control APIs (7 Entries)

| API Name | Scope | Cost Granularity | Overhead | Primary Role |
| :--- | :---: | :---: | :---: | :--- |
| `GetThreadPriority` | TID | Conditional (0-2) | Tiny | Query Win32 relative priority (`-15` to `+15`) |
| `SetThreadPriority` | TID | On Mismatch | Low | Apply Win32 relative thread priority |
| `NtSetInformationThread` | TID | On Mismatch | Low | Apply `Fixed 1..31` absolute `KPRIORITY` |
| `SetThreadAffinityMask` | TID | Unapplied Only | Low | Pin audio thread / evict normal threads |
| `SetThreadIdealProcessor` | TID | Unapplied Only | Low | Set preferred audio core / spread normal cores |
| `SuspendThread` | TID | Manual Only (1) | Low | Suspend Col 4 (`❚❚`) audio thread |
| `ResumeThread` | TID | Manual Only (1) | Low | Resume Col 4 (`❚❚`) audio thread |

#### Runtime Cost & Behavior Notes (1.3)
- **Unattached Priority API Skip**: When Col 2 is unattached (light green: `audioPriority == 0`) in steady state outside new-TID or `Dyn Pri` checks, `GetThreadPriority`, `SetThreadPriority`, and `NtSetInformationThread` are all **0 calls**.
- **Scoped `GetThreadPriority` Enumeration**: Bulk per-thread `GetThreadPriority` during discovery runs only for unconfirmed `AppType: 3` (DAW) and `Type 5` (Voicemeeter), staying **0 calls** during steady isolation, general apps, and `RAATServer` search (queried individually only on intruder demotion or `Dyn Pri` checks).
- **Differential Affinity & Ideal CPU**: Matched against `m_appliedThreads` / `m_appliedIdealCores` caches, keeping `SetThreadAffinityMask` and `SetThreadIdealProcessor` at **0 calls** during steady state.

---

## 2. Polling Tiers (5 Tiers + One-Shot) × `AppType (0..5)` Quick Reference

### `AppType (0..5)` Definitions
- **`Type 0`**: `audiodg.exe` (Windows Audio Device Graph Isolation)
- **`Type 1`**: General non-Chromium apps (media players, games, etc.)
- **`Type 2`**: Chromium-based apps (Chrome, Edge, Brave, Vivaldi, etc.)
- **`Type 3`**: DAW / multi-priority apps (Nuendo, Cubase, etc. / >=2 `TimeCritical` threads)
- **`Type 4`**: Roon suite (`RoonAppliance.exe` / `RAATServer.exe`)
- **`Type 5`**: Voicemeeter editions (2 slots: Slot 0 `VBAN` / Slot 1 `VAIO/AUX & DSP`)

### Active Scope by Polling Tier (Baseline `INI = 500ms`)

| Tier | Interval (`500ms`) | Target AppType | Active Mode / Operation |
| :--- | :---: | :--- | :--- |
| `INI × 10` | Every 5.0s | All Types | Process discovery while targets not running |
| `INI × 10` | Every 5.0s | Unlisted | Evacuate new PIDs (max 1 process per tick) |
| `INI × 10` | Every 5.0s | `Type 2` (Chromium) | Discover `AudioService` child process via PEB |
| `INI × 6` | Every 3.0s | `Type 1, 3, 4, 5` | Steady monitoring after isolation (5 skips) |
| `INI × 6` | Every 3.0s | `Type 1, 3, 5` | `Standby 3..1` search (sleep after 3 misses) |
| `INI × 6` | Every 3.0s | `Type 4` (`RAATServer`) | `WaitReason == 4` search while unresolved |
| `INI × 3` | For 1.5s | `Type 1, 3, 4, 5` | 3-tick fast verify right after identification |
| `INI × 1` | Every 500ms | All Types | Registered PID liveness (`GetExitCodeProcess`) |
| `INI × 1` | Every 500ms | `Type 0` (`audiodg`) | Continuous monitoring (catch new TIDs fast) |
| `INI × 1` | Every 500ms | `Type 2` (Chromium) | `Step 2` silence standby / playback maintain |
| `INI × 1` | Every 500ms | `Type 1, 3, 4, 5` | Initial 2s `Searching...` (`Type 4` continuous) |
| `INI / 4` | Every 125ms | `Type 2` (Chromium) | `Step 3` high-speed sampling (3 iterations) |
| One-Shot (1) | On Event | All Types | Initial `HighQoS` / Picker / `❚❚` Suspend |
| One-Shot (2) | 0s + ~3s | All Types | 2-stage actual `Dyn Pri` acquisition |

- **States Where Thread Scanning Stops (0 Calls)**:
  - Not running (`Not running`) or excluded (`Bypassed`)
  - `Type 1, 3, 5` asleep (`sleeping...`: **0 calls** beyond PID liveness until user action; if `Type 5` has 1 slot isolated while the other sleeps, only the isolated TID is checked every 3s)
  - Wait ticks between `Standby 3..1` 3s checks in `Type 1, 3, 5` (5 skipped out of 6 ticks)
  - Skip ticks in the 3s steady gate for `Type 1, 3, 4, 5` (5 skipped out of 6 ticks)
  - `Type 2` (Chromium) while `AudioService` is unresolved (`sleeping...`, PEB scan at `INI × 10` only)

---

## 3. Detailed API Matrix by Polling Tier

**Legend:**
- **`● 1`** / **`● N`**: Always called on every active tick in this tier (`1` = per-PID/fixed TID, `N` = all TIDs in PID)
- **`▲ Cond`** / **`▲ Diff`**: Called only when condition holds (mismatch, attached, new TID, threshold). **0 in steady state**
- **`-`**: Not invoked (**0 calls**)

---

### 3.1 `INI × 10` Tier (Every 5.0s: Process Discovery, Unlisted Eviction, Chromium Subprocess)

| Invoked API | Unlaunched Rules | Unlisted ON | `Type 2` Unresolved |
| :--- | :---: | :---: | :---: |
| `CreateToolhelp32Snapshot` | ● 1 | ● 1 | ● 1 |
| `Process32First / Next` | ● All PIDs | ● All PIDs | ● All PIDs |
| `OpenProcess` | - | ▲ Max 1 | ● Child PIDs |
| `GetProcessAffinityMask` | - | ▲ Max 1 | - |
| `SetProcessAffinityMask` | - | ▲ Mismatch | ▲ 1st Non-Audio |
| `NtQueryInformationProcess` | - | - | ● Child PIDs |
| `ReadProcessMemory` (PEB) | - | - | ● Child PIDs |
| `CloseHandle` | ● 1 | ▲ Max 1 | ● Child PIDs |

- All listed APIs are **0 calls** when every registered rule is running (including `Type 2` `audioServicePid != 0`), `UnlistedMask == 0`, and no forced rescan is requested. `PID <= 4` (System Idle / System) is always excluded.
- Immediate snapshot execution also triggers on initial startup (`!s_initialProcessScanDone`), rule add/update or manual un-bypass (`m_forceProcessSnapshot`), and registered rule PID exit without waiting for `INI × 10`.
- With `UnlistedMask` enabled, newly discovered PIDs are evacuated at **max 1 process per tick**.
- `Type 2` (Chromium) runs its first PEB scan immediately upon detection (`!chromiumScanAttempted`) without waiting for `INI × 10`, initializes non-audio children via 1 direct `SetProcessAffinityMask` (no `Get`), and stops PEB scans (**0 calls**) once `audio.mojom.AudioService` is found.

---

### 3.2 `INI × 6` Tier (Every 3.0s: `Type 1, 3, 4, 5` Steady State & `RAATServer` Search)

| Invoked API | `Type 1` Steady | `Type 3, 4` Steady | `Type 5` Steady | `RAAT` Searching |
| :--- | :---: | :---: | :---: | :---: |
| `NtGetNextThread` | ● N | ● N | ● N | ● N |
| `NtQueryInformationThread` | ● N | ● N | ● N | ● N |
| `NtQuerySystemInformation` | - | - | - | ● 1 (Batch) |
| `OpenProcess` | ● 2 | ● 2 | ● 2 | ● 2 |
| `GetPriorityClass` | ● 1 | ● 1 | ● 1 | - |
| `SetPriorityClass` | ▲ Diff | ▲ Diff | ▲ Diff | - |
| `OpenThread` (Fixed TIDs) | ● 1 | ● 1 | ● 1-2 | ● N |
| `OpenThread` (Normal TIDs) | ▲ Delta/Shared | ▲ Count Delta | ▲ Count Delta | *(in row above)* |
| `QueryThreadCycleTime` | ▲ New Evict | ▲ New Evict | ▲ New Evict | - |
| `SetThreadAffinityMask` | ▲ Diff | ▲ Diff | ▲ Diff | ▲ On Match |
| `SetThreadIdealProcessor` | ▲ Diff | ▲ Diff | ▲ Diff | ▲ On Match |
| `GetThreadPriority` | ▲ Attach/Chk | ▲ Attach/Chk | ▲ Attach/Chk | ▲ Match/Demote |
| `SetThreadPriority` | ▲ Attach Diff | ▲ Attach Diff | ▲ Attach Diff | ▲ Match/Demote |
| `NtSetInformationThread` | ▲ Fixed Diff | ▲ Fixed Diff | ▲ Fixed Diff | ▲ On Match |
| `CloseHandle` | ● N+3+ | ● N+3+ | ● N+3-4 | ● 2N+2 |

- **5 Out of 6 Ticks Paused**: Skipped ticks execute only `GetExitCodeProcess` liveness checks for registered PIDs; all APIs in the table above stay at **0 calls** (likewise on `Standby 3..1` wait ticks).
- **`OpenProcess` & `CloseHandle` Actual Counts**: Each active isolation pass uses `2` `OpenProcess` handles (1 enumeration + 1 isolation), and `CloseHandle` closes `N` handles from `NtGetNextThread` plus `2` process handles plus `OpenThread` handles (`N+3+`; `+1` for liveness in non-snapshot ticks as shown in 3.4).
- **Skipping Normal Threads & `GetThreadPriority`**: When thread count is unchanged (`canSkipNormalThreads == true`, including `Type 5` 1-slot isolated + other slot sleeping), opening/profiling `N` normal threads is skipped (**0 calls**). `GetThreadPriority` is also **0 calls** when unattached in steady state or while `RAATServer` is searching (queried individually only on intruder demotion or `Dyn Pri` check).
- **`RAATServer.exe` Low-Cost Search**: Queries `NtQuerySystemInformation` once every 3.0s while unresolved to inspect `WaitReason == 4` across threads.

---

### 3.3 `INI × 3` Period (For 1.5s: 3-Tick Fast Verification Right After Identification)

- For **3 ticks (1.5s)** immediately after `Type 1, 3, 4, 5` identifies an audio thread—or **2 ticks (1.0s)** right after UI setting edits—the 3s steady gate is bypassed to run the same `3.2 Steady` pass on every `INI` tick (500ms).
- After 3 ticks, it transitions automatically into `3.2` (`INI × 6`: 3.0s interval; note when `INI = 1000ms`, the 3.0s gate itself equals `INI × 3`).

---

### 3.4 `INI × 1` Tier (Every 500ms: Continuous Monitoring & Active Discovery)

| Invoked API | All Liveness | `Type 0` Active | `Type 2` Wait/Play | `Type 1,3,4` Scan | `Type 5` Scan |
| :--- | :---: | :---: | :---: | :---: | :---: |
| `GetExitCodeProcess` | ▲ Reg PIDs | - | - | - | - |
| `NtGetNextThread` | - | ● N | ● N | ● N | ● N |
| `NtQueryInformationThread` | - | ● N | ● N | ● N | ● N |
| `OpenProcess` | ▲ Reg PIDs | ● 2 | ● 2 | ● 2 | ● 2 |
| `GetProcessAffinityMask` | - | ● 1 | ● 1 | ▲ 1st Only | ▲ 1st Only |
| `SetProcessAffinityMask` | - | ▲ Diff | ▲ Diff | ▲ 1st Only | ▲ 1st Only |
| `GetPriorityClass` | - | ● 1 | ▲ Active 1 | ▲ Browser Ex | ▲ 1+ Isolated |
| `SetPriorityClass` | - | ▲ Diff | ▲ Diff | ▲ Diff | ▲ Diff |
| `OpenThread` | - | ▲ New TID | ▲ Cand/Fixed | ● N | ● N |
| `CloseHandle` | ▲ Reg PIDs | ● N+2+ | ▲ Cand+N+2 | ● 2N+2 | ● 2N+2 |
| `QueryThreadCycleTime` | - | - | ▲ Cand/Fixed | ● N (0 Wait) | ● N (0 Wait) |
| `ReadProcessMemory` (32KB) | - | - | - | ▲ 1x Lifetime | ▲ 1x Lifetime |
| `VirtualQueryEx` | - | - | - | ▲ 1x Lifetime | ▲ 1x Lifetime |
| `GetMappedFileNameA` | - | - | - | ▲ 1x Lifetime | ▲ 1x Lifetime |
| `SetThreadAffinityMask` | - | - | ▲ Diff | ▲ On Match | ▲ Diff |
| `SetThreadIdealProcessor` | - | - | ▲ Diff | ▲ On Match | ▲ Diff |
| `GetThreadPriority` | - | ▲ New/Chk | ▲ Attach/Chk | ▲ DAW N Only | ● N |
| `SetThreadPriority` | - | ▲ New TID | ▲ Attach Diff | ▲ Match/Demote | ▲ Match/Demote |
| `NtSetInformationThread` | - | ▲ New TID | ▲ Fixed Diff | ▲ On Match | ▲ On Match |

- **Liveness Pass (`!shouldScanProcesses`)**: `OpenProcess(QUERY_LIMITED)` + `GetExitCodeProcess` + `CloseHandle` run strictly on active `PID > 4` entries in `m_config.rules` (**0 calls** on ticks running `SNAPPROCESS`).
- **`Type 0` (`audiodg.exe`)**: Existing TIDs are skipped via `m_audioDgAppliedTids`, keeping `OpenThread` at **0 calls** unless new TIDs appear (and Col 2 unattached keeps priority APIs at **0 calls** even on new TIDs). Where PPL blocks `OpenProcess(PROCESS_QUERY_INFORMATION)`, enumeration falls back to `NtQuerySystemInformation(5)`.
- **`Type 2` (`Step 2`)**: `flag == 1` (parent dispatch) and cache-matched `alreadyEvacuated` threads are skipped prior to `OpenThread`, keeping `OpenThread` and `QueryThreadCycleTime` at **0 calls** except for active `flag = 0` candidates and `flag = 2`.
- **`Type 1, 3, 4, 5` Search, Sleep & Exceptions**:
  - Once identified, they transition through 3.3 (3 ticks) into 3.2 (`INI × 6`), dropping all per-type columns above to **0 calls**.
  - `QueryThreadCycleTime` and `GetThreadPriority` stay at **0 calls** during `Standby 3..1` wait ticks, `sleeping...`, and `RAATServer` searches (bulk `GetThreadPriority` runs only in `Type 3` DAW and `Type 5` searches).
  - Even during `Type 1, 3, 4` search, non-`Idle` rules whose names contain `chrome`, `vivaldi`, `edge`, `brave`, or `opera` invoke `GetPriorityClass` each tick to keep priority at Normal.

---

### 3.5 `INI / 4` Tier (Every 125ms × 3: `Type 2` Chromium High-Speed Sampling)

| Invoked API | `Type 2` (Tick 1-2) | `Type 2` (Tick 3 Final) | `Type 0, 1, 3, 4, 5` |
| :--- | :---: | :---: | :---: |
| `NtGetNextThread` | ● N | ● N | - |
| `NtQueryInformationThread` | ● N | ● N | - |
| `OpenProcess` | ● 2 | ● 2 | - |
| `OpenThread` | ● Cand/Unevict | ● Cand/Unevict | - |
| `CloseHandle` | ● Cand+N+2 | ● Cand+N+2 | - |
| `QueryThreadCycleTime` | ● Cand TIDs | ● Cand TIDs | - |
| `SetThreadAffinityMask` | - | ● Win+Unevict | - |
| `SetThreadIdealProcessor` | - | ● Winner 1 | - |
| `GetThreadPriority` | - | ▲ Attach/Chk | - |
| `SetThreadPriority` | - | ▲ Attach Diff | - |
| `NtSetInformationThread` | - | ▲ Fixed Diff | - |

- Triggered during `Type 2` `Step 2` when `Delta >= threshold`, running 3 times at 125ms (375ms total), crowning the highest cumulative Delta thread as `flag = 2`, and returning to `INI × 1` (500ms).
- Threads dropping to `Delta == 0` are marked (`-1`), while `flag = 1` siblings and cache-matched `alreadyEvacuated` threads are skipped (**0** `OpenThread` calls).
- **Behavior of Other Rules During Acceleration**: Because `TIMER_POLLING_ID` itself temporarily speeds up to `INI / 4` (125ms), other active rules and `GetExitCodeProcess` liveness checks also tick 3 times at 125ms intervals during those 375ms.

---

### 3.6 One-Shot & Event-Driven Invocations (Initial 1 / 0s + ~3s / UI Actions Only)

| Trigger Event | Target Type | Invoked API | Call Count |
| :--- | :---: | :--- | :---: |
| New PID Detected | All `Type 0..5` | `SetProcessInformation` (HighQoS) | Once on 1st Detect |
| Initial `AppType` Probe | Unclassified | `QueryFullProcessImageNameA` | Once on 1st Detect |
| Chromium Parent Evict | `Type 2` | `QueryThreadCycleTime` + `SetAffinity` | Once on 1st Detect |
| Actual `Dyn Pri` Check | `Type 0` (`audiodg`) | `NtQuerySystemInformation` (Top `CtxSw`) | Twice (0s + ~3s) |
| Actual `Dyn Pri` Check | `Type 1..5` | `NtQueryThread` + `GetThreadPriority` | Twice (0s + ~3s) |
| Picker Priority Change | All `Type 0..5` | `SetPriorityClass` / `SetThreadPriority` / `NtSet` | Once per Action |
| Picker Unattach Reset | All `Type 0..5` | *(None: preserves OS priority, stops tracking)* | 0 calls |
| Col 4 (`❚❚`) Suspend/Resume | `Type 0` (`audiodg`) | `NtSuspendProcess` / `NtResumeProcess` | Once per Action |
| Col 4 (`❚❚`) Suspend/Resume | `Type 1..5` | `SuspendThread` / `ResumeThread` | Once per Action |
| `audiodg` Release / Exit | `Type 0` (`audiodg`) | Restore All Cores + `NORMAL` + All TIDs `+2` | Once on Release/Exit |

- **Skipping `audiodg.exe` Restore via `Ctrl` + Exit/Restart**: Holding `Ctrl` while invoking **`[Exit]`** or tray **`Exit`** / **`Restart`** / **`Restart as Administrator`** skips `RestoreAudioDgProcessLocked` and preserves active cores and priorities.
- **2nd `Dyn Pri` Check for `Type 1, 3, 4, 5`**: Executes on the first `INI × 6` tick after 3.0s elapse (roughly 3.0-4.5s post-identification). Changing priority settings via picker also re-triggers this 2-stage check.

