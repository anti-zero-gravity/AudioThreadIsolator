# Audio Thread Isolator (ATI) v1.0.4 - Specification & User Manual (spec.md)

*Note: This document is the end-user manual for distribution. Detailed internal architectural specifications and algorithmic logic are documented in [arc.md](file:///c:/Users/YK-PC/.gemini/antigravity-ide/scratch/ATI/arc.md).*

## 1. Application Overview

- **Name**: Audio Thread Isolator (ATI)
- **Type**: Windows Native Resident Utility (C++17 / Pure Win32 API / Comctl32 / GDI+)
- **Binary Architecture**: **Completely Standalone Single .exe File (No folders required)**
  - All resources required for the 7x7 Priority Matrix Overlay Picker (HitMap binary, base background, and all 49 rank 3D composite images; total 51 assets) are fully embedded within the executable as `RCDATA`. Works out of the box with `ATI.exe` alone without needing an external `ui_parts/` folder.
- **Purpose**:
  Automatically identifies the audio playback thread from running audio and multimedia processes, isolating it onto designated audio CPU core(s) while evicting all other non-audio threads to separate excluded cores. By preventing high-load threads from sharing cores with audio threads, micro-jitter (isochronous timing fluctuation from queuing latency) is minimized.
- **Automatic HighQoS (Power Throttling OFF) Enforcement**:
  For all processes registered for monitoring, ATI automatically disables Windows Power Throttling (EcoQoS) upon detection, enforcing HighQoS (maximum execution speed and timer resolution preservation). By applying this at the process level, Windows kernel inheritance ensures all threads (both existing and newly spawned) run at full clock speed without power-saving latency or throttling.

---

## 2. UI Structure and Operations

### 2.1 Main Window
- **Overview**: The primary monitoring and quick-control interface for registered processes (bypass, suspend, priority adjustment).
- **High-DPI & Resolution Adaptive**: Automatically adjusts layout and font size smoothly without distortion during resolution or scaling changes (e.g., dual-mode monitors, 5K 200%).
- **Separation of Concerns**: Daily monitoring and instant tuning are handled on the main screen, while core affinity configuration is managed via the per-process settings dialog (double-click row or `[Edit]`).

#### Control Buttons (Right Panel)
- **`Always on Top`**: Pins the main window to always stay in the foreground.
- **`[Edit]`**: Opens the settings dialog for the selected process (or double-click row).
- **`[Add]`**: Opens a dialog to pick and add running processes to the monitoring list.
- **`[Remove]`**: Removes the selected process from the monitoring list.
- **`[Settings]`**: Opens global settings for system defaults and core protection.
- **`[Exit]`**: Exits the application.

#### System Tray Menu (Right-Click Taskbar Tray Icon)
- **`Open Settings`**: Restores and displays the main window (identical to left-clicking the tray icon).
- **`Start with Windows`**: Toggles automatic startup with Windows.
- **`Restart`**: Restarts the application normally.
- **`Restart as Administrator`**: Restarts the application with elevated administrator privileges (UAC). (*Automatically hidden if already running with admin privileges). When running as administrator, `SeIncreaseBasePriorityPrivilege` is automatically enabled, unlocking `Realtime` priority (Base Priority 16-31).
- **`Exit`**: Exits the application.

#### Process Table (Monitoring List)

| Col | Header | Summary & Role | Operation Details |
|:---:|:---|:---|:---|
| 0 | **`!`** | Toggles monitoring and temporary bypass. Checking this suspends ATI intervention. | Click to toggle |
| 1 | **`Process Name`** | Application executable name without `.exe`.<br>When multiple threads are detected via `IgnoreSig:t`, child rows display an L-shaped tree line (`└`) drawn via GDI to indicate parent-child hierarchy. | Double-click to open edit dialog.<br>Click header to sort ascending. |
| 2 | **`Thread Priority`** | Relative thread priority applied to audio thread.<br>When multi-thread expansion is active, each row (parent/child) can independently select priority via the 2D Priority Matrix Picker. | Click cell to select via 2D Priority Matrix Picker |
| 3 | **`Process Priority`** | Priority class applied to the process (PID) hosting the audio thread. | Click cell to select via 2D Priority Matrix Picker |
| 4 | **`❚❚`** | Verification pause feature. Suspends the target audio thread to stop audio, confirming if it is the genuine playback thread.<br>When multi-thread expansion is active, each row (TID) can be suspended independently.<br>**(WARNING: Strictly prohibited to use as mute due to risk of app deadlocks)** | Click to toggle suspension |
| 5 | **`Audio Core PID/TID`** | Displays detection state, isolated PID, and TID. When multi-thread expansion is active, each TID is displayed sorted in **descending Cycles Delta**. | Click operation: Restarts search when sleeping; toggles Bypass when running (double-click disabled). |
| 6 | **`TIDs`** | Monitored thread count. Shows `1` for standard apps, total app thread count during search, and `0` when sleeping. | Double-click to open edit dialog. |
| 7 | **`Chgs`** | Count of times audio thread was detected and priorities were applied. | Double-click to open edit dialog. |
| 8 | **`Audio`** | Dedicated audio CPU core number (e.g., `#1`). When multi-thread expansion is active, shows the core assigned to each thread. | Double-click to open edit dialog. |
| 9 | **`Excluded`** | List of CPU cores excluded from normal thread placement (e.g., `!#1`).<br>**In multi-thread expansion, since sibling threads share the same parent PID and eviction mask, rows 2 and beyond (subIndex > 0) display `same`.** | Double-click to open edit dialog. |

- **`IgnoreSig:t` Multi-Audio Thread Expansion**:
  - By configuring thread count `t` (natural number) such as `IgnoreSig:2` in `INI`, ATI identifies and isolates the top `t` audio threads.
  - The table expands to display `t` rows sorted in **descending Cycles Delta** (CPU activity order) as if they were distinct entries.
  - The 1st row displays the actual excluded core list in the `Excluded` column; rows 2 and subsequent show `same`.
  - **10-Second Continuous Scan Countdown & Simultaneous `sleeping...` Transition**:
    - Scans every turn for 10 seconds for pending unconfirmed slots without idle intervals.
    - The pending slot currently being searched displays a countdown from `Standby 10` down to `Standby 1`, while subsequent pending slots wait at `Standby 10`.
    - If no subsequent thread is detected within 10 seconds, all unconfirmed slots from the 2nd thread onwards transition **simultaneously to `sleeping...`**.
    - When a subsequent thread is detected, that slot immediately locks to `PID ... / TID ...`, and the next slot **starts counting down from 10**.
    - Clicking a sleeping row (Col 0 or Col 5) immediately restarts the 10-second countdown scan while preserving already-locked threads.
  - **Fallback Identification Logic**:
    - If target thread count `t` cannot be met under standard criteria (Priority 15 and Cycles Delta >= threshold [default 3M/s]), ATI automatically falls back to thread wait-state analysis.
    - Active threads satisfying `WaitReason == DelayExecution` (value 4) and Cycles Delta >= threshold (3M/s) are detected and locked onto remaining slots in descending Cycles Delta (e.g., captures threads in apps like Voicemeeter that do not assign priority 15).
    - If all target slots are satisfied under normal conditions, fallback scanning is skipped to avoid unnecessary overhead.
  - Each expanded row can be suspended independently (`❚❚`).
  - Editing and saving settings via `[Edit]` or double-clicking any sibling row updates all expanded rows of the process simultaneously.

- **Rank Selection via 2D Priority Matrix Overlay Picker**:
  - Clicking the `Thread Priority` (Col 2) or `Process Priority` (Col 3) cell pops up a 7x7 3D visual matrix overlaid on the center of the main window.
  - Hovering over pillars updates HUD information in real-time (Base Priority, Rank #, Process Class, Thread Priority, formula).
  - Clicking a pillar (Rank 1 to 49) commits both Process Class and Thread Priority, applying them immediately to the active process/threads and saving directly to `ATI.ini`.

- **Ascending Sort by Clicking `Process Name` Header**:
  - Clicking the `Process Name` header shows a confirmation prompt ("SORT?") and sorts the list alphabetically upon confirmation.

#### Status Displays (Col 5) and Indicators

| Display Text | Indicator Dot | Meaning | Behavior on Cell Click |
|:---|:---|:---|:---|
| `Not running` | None (Unchecked `☐` / `☑`) | Application is not running (standby). | Check to pre-configure Bypass before launch |
| `Searching...` | ● Coral Pink | Searching for audio thread. | Toggles Bypass |
| `Standby` / `Standby 10..1` | ● Coral Pink | Continuous monitoring standby. Awaiting audio output. | Transitions to Bypass (displays ☑ in ! col) |
| `sleeping...` | ● Flashing Muted Purple / Red | Sleeping due to silence / inactivity. | Immediately restarts search |
| `PID <pid> / TID <tid>` | ● Cyan / Teal | Audio thread identified and isolated onto audio core. | Transitions to Bypass (displays ☑ in ! col) |
| `PID <pid> / Searching...` | ● Coral Pink | Chromium audio process identified; searching internal threads. | Transitions to Bypass (displays ☑ in ! col) |
| (Half-Isolated state) | ● Flashing Dull Yellow / Teal | Thread unmigratable due to driver constraints co-existing on audio core (suppression active). | Transitions to Bypass (displays ☑ in ! col) |

#### Priority Settings and Windows Base Priority

Under standard applications (Normal process priority class), each setting corresponds to Windows Base Priority (1-15) and characteristics as follows:

| ATI Setting | Base Priority | Characteristics |
|:---|:---:|:---|
| **`Idle (-15)`** | **1** | Runs only during CPU idle time (ultra-low load, lowest jitter). |
| **`Lowest (-2)`** | **6** | Below normal threads. Base value during Half-Isolated co-existing thread promotion. |
| **`Below Normal (-1)`** | **7** | Slightly below normal threads. |
| **`Normal (0)`** | **8** | Standard OS thread priority. |
| **`Above Normal (+1)`** | **9** | One level above standard threads. |
| **`Highest (+2)`** | **10** | Preferentially scheduled ahead of normal threads. |
| **`Time Critical (+15)`** | **15** | Highest priority accessible to standard non-elevated applications. |

- **Unlocking Realtime Priority (Base Priority 16-31)**:
  - Ranks 43-49 in the 2D Priority Matrix Picker (Realtime priority class) require the process to run with administrator privileges (`SeIncreaseBasePriorityPrivilege` enabled).
  - When not running as Administrator, ATI and the Windows kernel silently fall back to High priority (clamped to Base Priority 15). Restarting via **`Restart as Administrator`** in the tray menu enables elevated Base Priority from 16 to 31.
- **Instant Application and Settings Saving**:
  Priorities selected via the 2D Priority Matrix Picker are immediately applied to active processes and threads, and automatically saved to `ATI.ini`. Settings can also be adjusted manually via the per-process edit dialog (`[Edit]`).
- **Half-Isolated Co-existing Thread Suppression (Graphics Drivers, etc.)**:
  If an unmigratable thread (e.g., driver threads locked to specific CPU cores) is detected co-existing on the dedicated audio core, ATI automatically promotes `Idle (-15)` audio threads to `Lowest (-2)`. Co-existing threads are throttled to one level below the audio thread priority.

---

### 2.2 Process Picker Dialog
- Launched via `[Add]` from the main window.
- Lists all currently running processes in alphabetical order.
- Select a process and click `[Done]` to add it to the monitoring list (global default settings are applied initially).

### 2.3 System Tray Behavior
- Clicking the `×` button or minimizing tucks the window into the system notification area (system tray).
- **Left-Click Tray Icon**: Brings the main window to the foreground.
- **Right-Click Menu**:
  - `Open Settings`: Opens the main window.
  - `Start with Windows`: Toggles automatic startup with Windows.
  - `Restart`: Restarts the application.
  - `Restart as Administrator`: Restarts with elevated UAC privileges (hidden when already elevated).
  - `Exit`: Terminates the application.

---

### 2.4 Global Settings Dialog
Opened via `[Settings]` on the main window or tray menu. Manages default values for newly added processes and system-wide CPU Sets core protection.

| Item | Summary & Role | Operation Details |
|:---|:---|:---|
| **Core Assignment Table** | Configures default dedicated audio core (Row 0) and excluded cores (Row 1). | Select via checkboxes |
| **`Default Audio Core (Mask)`** | Hexadecimal mask for default audio core. | Direct input or table click |
| **`Apply` (Audio Core Row)** | **[Dedicated Core Protection]** Applies "CPU Sets excluding audio core" to all non-ATI processes across the system, shielding audio cores from external interrupts. | Click to apply system-wide |
| **`Excluded (Mask)`** | Hexadecimal mask for default excluded cores. | Direct input or table click |
| **`Apply` (Excluded Row)** | **[Apply Excluded Mask]** Applies specified excluded core mask to all non-ATI processes. | Click to apply system-wide |
| **`Audio Thread Priority`** | Default priority applied to newly added processes. | Select from dropdown |
| **`Polling Interval (ms)`** | Polling cycle for monitoring thread (`100` / `200` / `500` / `1000` ms). | Number input (auto-corrected) |
| **`Release Mask`** | Clears protection masks from general processes, restoring OS defaults. | Click to release |
| **`OK` / `Cancel`** | Saves (OK) or discards (Cancel) settings and closes. | Click |

---

### 2.5 Thread Isolation Settings Dialog
Opened by double-clicking a process row or clicking `[Edit]` on the main window.

| Item | Summary & Role | Operation Details |
|:---|:---|:---|
| **`Target Process`** | Target executable name (e.g., `msedge.exe`). | Read-only |
| **Core Assignment Table** | Sets process-specific core affinity.<br>- **Row 0 (`Audio Core`)**: Dedicated core pinned for audio playback thread.<br>- **Row 1 (`Excluded`)**: Eviction cores excluded from normal threads.<br>*Automatic mutual exclusion prevents overlapping cores and guarantees at least 1 core for normal threads.* | Select via checkboxes |
| **`Audio Core (Mask)`** | Bitmask for dedicated audio core(s). Comma-separated multi-core specification supported (e.g., `1, 2`). | Direct input or table click |
| **`Excluded (Mask)`** | Bitmask for cores excluded from normal thread placement. | Direct input or table click |
| **`Audio Thread Priority`** | Priority applied to audio playback thread. | Select from dropdown |
| **`OK` / `Cancel`** | Saves to `ATI.ini` (OK) or discards (Cancel) and closes. | Click |

---

## 3. Operational Logic

### 3.1 Efficient Continuous Monitoring
- **Low-Overhead Polling**: Directly scans only registered target processes without taking global OS-wide snapshots, minimizing monitoring CPU overhead.
- **Sleep Detection**: Automatically transitions to sleep (`sleeping...`) during silent standby to eliminate unnecessary CPU cycles.

### 3.2 Core Isolation and Normal Thread Eviction
- **Audio Thread Isolation**: Pins identified audio playback thread to designated audio core (`AudioCore`) with specified priority.
- **Normal Thread Eviction**: Configures remaining non-audio threads within the process to run across eviction cores (`NormalCores`).
- **Excluded Core Protection**: Revokes process affinity permissions on excluded cores, preventing non-audio threads from invading dedicated cores.

### 3.3 Application-Specific Detection
- **Chromium Browsers (Chrome, Edge, Brave, Vivaldi, etc.)**:
  Identifies internal dedicated audio service processes across multi-process browser trees, tracking ephemeral audio output threads in real time.
- **Standard Audio & Media Players (foobar2000, mpv, Firefox, etc.)**:
  Correlates CPU load dynamics and audio module signatures (WASAPI, ASIO, DirectSound) to identify genuine playback threads.
- **Custom Architecture Apps (Voicemeeter, etc.)**:
  Continuously analyzes thread activity ranking and latency wait states to lock audio threads in applications lacking conventional signatures.
- **Voice Communication (Discord, etc.)**:
  Correlates specific module signatures for voice streaming threads to isolate call processing threads.

---

## 4. Configuration Reference (ATI.ini)

Configuration is saved in `ATI.ini` in the same directory as the executable.

### 4.1 Global Section `[Global]`

| Key | Description | Valid Range | Default |
|:---|:---|:---|:---|
| **`DefaultAudioCore`** | Default audio core assigned to newly registered processes. | `0` to (Max Cores - 1) | `5` |
| **`DefaultAudioPriority`** | Default thread priority assigned to newly registered processes. | `-15` (Idle) to `+15` (Time Critical) | `-15` |
| **`NormalCores`** | Hexadecimal mask of cores allocated to normal threads. | Hex bitmask (e.g., `0x1D`) | `0x1D` |
| **`PollingIntervalMs`** | Normal polling interval in milliseconds. | `100`, `200`, `500`, `1000` | `500` |
| **`BoostPollingIntervalMs`** | Fast polling interval during thread search or state transitions. | `50`, `100`, `250` | `250` |
| **`AlwaysOnTop`** | Main window always-on-top state. | `0` (Disabled), `1` (Enabled) | `0` |

### 4.2 Process Section `[Processes]`

Saved per registered process under the executable name as key, with comma-separated parameters:

| Parameter | Description | Valid Values |
|:---|:---|:---|
| **`AudioCore`** | Core assigned to audio thread. Comma-separated for multiple cores (e.g., `AudioCore:11, 10`). | `0` to (Max Cores - 1), comma-separated |
| **`AudioPriority`** | Relative priority for audio thread. Comma-separated for multiple threads (e.g., `AudioPriority:-15, -15`). | `-15`, `-2`, `-1`, `0`, `1`, `2`, `15`, comma-separated |
| **`NormalCores`** | Eviction core bitmask assigned to normal threads. | Hex mask starting with `0x` |
| **`Bypass`** | Temporary bypass state (`1`: bypassed, omitted: active). | `1` |
| **`AppType`** | Application classification mode (`1`: Standard Audio, `2`: Chromium). | `1`, `2` |
| **`IgnoreSig`** | Signature-independent mode. Identifies threads by ① Priority 15 with Cycles Delta >= 3M/s, or ② fallback to DelayExecution wait-reason with Cycles Delta >= 3M/s when priority 15 is absent. Specify `:t` to isolate top `t` threads (default: 1). | `IgnoreSig`, `IgnoreSig:t` (`t` is thread count) |
| **`AudioPid` / `AudioTid`** | Cached PID/TID from previous session for fast recovery. | Numeric |

---

## 5. Limitations & Notes

1. **Usage of Pause (`❚❚`) Feature**:
   - This feature is strictly for verification to temporarily mute audio and confirm that the selected thread is the genuine playback thread.
   - Forcibly suspending threads can cause application deadlocks or crashes if misused as a general mute function.
2. **Behavior on Temporary Bypass (`!` / Bypass)**:
   - Entering Bypass stops ATI monitoring and active affinity adjustments for that process.
   - Previously applied thread affinities and priorities are maintained and do not revert immediately to default OS scheduling.
3. **Single-Instance Enforcement & Self-Protection**:
   - Launching a second instance automatically brings the existing window to the foreground and terminates the new instance.
   - To prevent ATI's own monitoring loop from congesting audio cores, ATI automatically restricts its own process affinity to normal cores.
4. **HighQoS Operation Guarantee for Monitored Processes**:
   - When a monitored process is launched or detected, ATI immediately calls `SetProcessInformation(ProcessPowerThrottling)` on the process handle to disable EcoQoS and enforce HighQoS.
   - This ensures all threads, including audio playback threads isolated onto dedicated cores, operate without OS-induced clock throttling or timer resolution degradation.
