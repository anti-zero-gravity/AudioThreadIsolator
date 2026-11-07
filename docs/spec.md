# Audio Thread Isolator (ATI) v2.0.0 - Specification & User Manual (spec.md)

## 1. Application Overview

- **Name**: Audio Thread Isolator (ATI)
- **Type**: Windows Resident Utility
- **Purpose**:
  Automatically identifies the audio playback thread from running audio and multimedia processes, isolating it onto designated audio CPU core(s) while evicting all other non-audio threads to separate excluded cores. By preventing high-load threads from sharing cores with audio threads, the waiting time between a thread being queued in the ready queue and actually being executed is eliminated, minimizing micro-jitter and stutter in thread processing timing.
- **Automatic HighQoS (Power Throttling OFF) Enforcement**:
  For all processes registered for monitoring, ATI issues HighQoS once upon PID detection to disable Windows Power Throttling (EcoQoS / Efficiency Mode). This removes Windows' forced execution speed capping and timer coalescing (15.625ms relaxation), maintaining high-resolution timers (1ms).

---

## 2. UI Structure and Operations

### 2.1 Main Window
- **Overview**: The primary monitoring and quick-control interface for registered processes (bypass, suspend, priority adjustment). Automatically adapts layout and font scaling across resolution or DPI changes.

#### Control Buttons (Right Panel)
- **`Always on Top`**: Pins the main window to always stay in the foreground.
- **`[Edit]`**: Opens the Thread Isolation Settings dialog (Section 2.5) for the selected process (or click Col 1 / 6 / 7 / 8 / 9 in the process table).
- **`[Add]`**: Opens the Process Picker dialog (Section 2.2).
- **`[Remove]`**: Removes the selected process from the monitoring list.
- **`[Settings]`**: Opens the Global Settings dialog (Section 2.4).
- **`[Exit]`**: Exits the application.
- **Column Width & Window Height Drag Adjustment**: Dragging table header borders to resize column widths and dragging the top/bottom edges of the main window to resize vertical height are automatically saved to `ATI.ini`.

#### Process Table (Monitoring List)

| Col | Header | Summary & Role | Operation Details |
|:---:|:---|:---|:---|
| 0 | **`!`** | Toggles monitoring and temporary bypass. Checking this suspends ATI intervention. | Click to toggle (pre-configures Bypass when not running, restarts search when sleeping, or toggles per-slot mask on Voicemeeter rows) |
| 1 | **`Process Name`** | Application executable name without `.exe`.<br>When multi-row expansion is active for Voicemeeter (`AppType: 5`), displays each thread's role label and draws an L-shaped tree line (`└`) on child rows. For Roon (`AppType: 4`), displays the original process name (`RoonAppliance` / `RAATServer`) as a single row. | Click to open edit dialog.<br>Click header to sort ascending ("SORT?" prompt). |
| 2 | **`Thread Priority`** | Thread priority level and effective priority (e.g., `Below 9`, `Time Critical 15`, or `Fixed 19` when outside the 7 relative steps).<br>Initially (when untouched), ATI does not attach to thread priority and displays the measured dynamic priority on a pale green background. Switches to a white background once modified via the picker.<br>To minimize monitoring overhead, the effective priority number is queried once immediately upon thread confirmation or priority change, and once more a few seconds later (~3–4.5 s) after temporary OS priority boosts decay, with zero polling thereafter during steady-state operation. | Click to open 2D Priority Matrix Picker in Thread Priority Mode |
| 3 | **`Process Priority`** | Priority class of the process (PID) (e.g., `Normal`, `Above Normal`, `High`).<br>Initially (when untouched), ATI does not attach to process priority and displays the live measured process priority on a pale green background. Switches to a white background once modified via the picker. | Click to open 2D Priority Matrix Picker in Row Selection Mode |
| 4 | **`❚❚`** | Verification pause feature. Suspends the target audio thread to stop audio, confirming if it is the actual playback thread.<br>When multi-thread expansion is active, each row (TID) can be suspended independently.<br>For `audiodg.exe`, suspends and resumes the entire process (all threads) at once.<br>**(WARNING: Strictly prohibited to use as mute due to risk of app deadlocks)** | Click to toggle suspension |
| 5 | **`Audio Core PID/TID`** | Displays detection state, isolated PID, and TID. | Click to restart search when sleeping, or toggle Bypass when running / standing by |
| 6 | **`TIDs`** | Monitored thread count. Shows `1` for standard confirmed apps, total process thread count during search or for Chromium / `audiodg.exe`, and `0` when sleeping. | Click to open edit dialog |
| 7 | **`Chgs`** | Count of times audio thread was detected and priorities were applied. | Click to open edit dialog |
| 8 | **`Audio`** | Dedicated audio CPU core number (e.g., `#1`). When multi-thread expansion is active, shows the core assigned to each thread. | Click to open edit dialog |
| 9 | **`Excluded`** | List of CPU cores excluded from normal thread placement (e.g., `!#1`). In multi-thread expansion, all slots directly display the held mask. | Click to open edit dialog |

- **Dedicated Multi-Row Expansion (Voicemeeter)**:
  - All Voicemeeter editions (`AppType: 5`, 2 rows) automatically expand into role-specific rows and isolate each network/audio engine thread independently.
  - **10-Second Scan Countdown & `sleeping...` Transition**:
    - Continuously scans for unconfirmed slots for 10 seconds, displaying `Standby 10` down to `Standby 1` on pending rows.
    - Slots remaining undetected after 10 seconds (for example, when VBAN is disabled in Voicemeeter) transition to `sleeping...`. Clicking a sleeping row (Col 0 or Col 5) restarts the 10-second countdown scan while preserving already-confirmed threads.
  - Editing and saving settings via `[Edit]` or clicking a cell on any sibling row updates all expanded rows of the process simultaneously.
- **Roon Dedicated Continuous Standby (`AppType: 4`)**:
  - `RoonAppliance.exe` and `RAATServer.exe` operate as single-slot (1 row) rules.
  - During silence or pause, they never transition to `sleeping...`; like Chromium (`AppType: 2`), they continuously remain in `Searching...` to immediately isolate newly active threads upon playback resumption.

- **2D Priority Matrix Picker**:
  - **Clicking `Process Priority` (Col 3) (Process Priority / Row Selection Mode)**:
    - Hovering in the left 3D matrix selects **rows (Process Priority Classes) only**, without moving the thread priority column (the `Realtime` row cannot be selected in non-administrator mode).
    - Scrolling the mouse wheel steps through the Process Priority Classes (rows) one level up or down.
    - Click a pillar (bar) in the left pane, click the right pane after scrolling the wheel, or press `Enter` to confirm and apply immediately (if any slot has a `Fixed` thread priority in Col 2, that fixed priority is automatically re-applied right after changing the process priority class).
    - **Resetting to Unattached State (Pale Green Background)**: Clicking outside the 3D pillars (bars) inside the picker, or clicking the right pane without scrolling the mouse wheel, resets both `Thread Priority` (Col 2) and `Process Priority` (Col 3) back to the initial unattached state (pale green background). Press `Esc` or click outside the picker to cancel.
  - **Clicking `Thread Priority` (Col 2) (Thread Priority Mode)**:
    - **Standard Hover (without `Ctrl`)**: Remains **locked to the current Process Priority Class row**, selecting only horizontally across the 7 relative thread priorities (`Idle (-15)` to `Time Critical (+15)`), even if the cursor moves vertically outside the row.
    - **Holding `Ctrl` + Hovering Left Matrix (2D Selection)**: Holding **`Ctrl` while hovering inside the left 3D matrix unlocks the row**, allowing simultaneous 2D selection of both Process Priority Class (Col 3, row) and Thread Priority (Col 2, column) (moving the cursor onto the right pane preserves the selected row state).
    - **Right-Pane Number & Mouse Wheel Control**: Scrolling the mouse wheel increments or decrements the effective priority number in the right pane **by `1` across `1–31` (`1–15` in non-administrator mode)**. Values matching one of the current process class's 7 relative steps apply as that relative priority, while intermediate values outside the 7 steps (e.g., `11–14` under `Normal`) apply directly as **`Fixed <value>`** (re-hovering a bar in the left matrix returns to bar selection).
    - **Confirm / Reset to Unattached / Cancel**: Click a pillar (bar) in the left pane, click the right-pane number card after scrolling the wheel, or press `Enter` to confirm and apply immediately. Clicking outside the 3D pillars or clicking the right pane without scrolling the mouse wheel resets both `Thread Priority` and `Process Priority` back to their initial unattached state (pale green background). Press `Esc` or click outside the picker to cancel.
  - **`Normal` Priority Class Display**: When hovering or selecting `Normal` priority class, both the Foreground and Background rows illuminate simultaneously to reflect Windows' dynamic foreground/background priority switching, displaying both FG/BG effective priorities and an explanatory note in the right pane.
  - Confirming a priority in the 2D picker changes the cell background from green to white and starts applying priority changes. To stop priority monitoring, click outside the priority bars or click the right pane in the 2D picker. While ATI does not restore the initial startup priority of a running application, restarting the registered application confirms that its native priority remains untouched.

#### Status Displays (Col 5) and Indicators

| Display Text | Indicator Dot | Meaning | Behavior on Cell Click |
|:---|:---|:---|:---|
| `Not running` | None (Unchecked `☐` / `☑`) | Application is not running (standby). | Check to pre-configure Bypass before launch |
| `Searching...` | ● Coral Pink | Searching for audio thread. | Toggles Bypass |
| `Need Admin` | ● Coral Pink | Process requires administrator privileges (e.g. `audiodg.exe`), but ATI is running without elevation. Relaunch ATI as Administrator. | Toggles Bypass |
| `Standby` / `Standby 10..1` | ● Coral Pink | Continuous monitoring standby. Awaiting audio output. | Transitions to Bypass (displays ☑ in ! col) |
| `sleeping...` | ● Flashing Muted Purple / Red | Sleeping due to silence / inactivity. | Immediately restarts search |
| `PID <pid> / TID <tid>` | ● Cyan / Teal | Audio thread identified and isolated onto audio core (displayed as `PID <pid> / -` for `audiodg.exe`). | Transitions to Bypass (displays ☑ in ! col) |
| `PID <pid> / Searching...` | ● Coral Pink | Chromium audio process identified; searching internal threads. | Transitions to Bypass (displays ☑ in ! col) |
| (Half-Isolated state) | ● Flashing Dull Yellow / Teal | In Standard Audio apps (`AppType: 1`), one or more non-audio threads of the same process remain co-resident on the audio core due to driver/engine behavior. Promotes `Idle (-15)` audio threads to `Lowest (-2)` and sets all co-existing non-audio threads one level below the audio thread (including `Fixed <N-1>` when `Fixed` priority is used). | Transitions to Bypass (displays ☑ in ! col) |

- **Leading Counter in Bottom Legend Bar**:
  - Only while an `Apply` checkbox for Unlisted process protection is checked in Global Settings (Section 2.4), the leftmost position of the main window legend bar displays **`● <count> Unmonitored Processes Masked`**, showing the cumulative number of non-monitored processes masked since startup (hidden when unchecked).

#### Priority Settings and Windows Base Priority

Under standard applications (Normal process priority class), each thread priority setting corresponds to Windows Base Priority (1–15) as follows:

| ATI Setting | Base Priority | Characteristics |
|:---|:---:|:---|
| **`Idle (-15)`** | **1** | Runs only during CPU idle time (ultra-low load, lowest jitter). |
| **`Lowest (-2)`** | **6** | Below normal threads. Base value during Half-Isolated co-existing thread promotion. |
| **`Below Normal (-1)`** | **7** | Slightly below normal threads. |
| **`Normal (0)`** | **8** | Standard OS thread priority. |
| **`Above Normal (+1)`** | **9** | One level above standard threads. |
| **`Highest (+2)`** | **10** | Preferentially scheduled ahead of normal threads. |
| **`Time Critical (+15)`** | **15** | Highest priority accessible to standard non-elevated applications. |

- **Realtime Priority (Effective Priority 16–31)**:
  - The `Realtime` priority class and effective priorities `16–31` (including mouse-wheel numeric selection) are selectable in the picker only when ATI is running as Administrator (in non-administrator mode, the `Realtime` row and values `16` and above are locked and cannot be selected).

---

### 2.2 Process Picker Dialog
- Launched via `[Add]` on the main window. Click running process rows to toggle selection checkboxes and click `[Done]` to add them to the monitoring list (core masks initialize from global defaults, while thread/process priorities start unattached on a pale green background). Click `[Refresh]` to rescan running processes, or `[Cancel]` to close without adding.
- Clicking the leading red **`!` column** registers the process in `UnlistedExclusions`, excluding it from Unlisted process masking targets (hovering over the `!` column displays an explanatory note at the bottom).

### 2.3 System Tray Behavior
- Clicking the `×` button or minimizing tucks the window into the system notification area (system tray).
- **Left-Click Tray Icon**: Restores and brings the main window to the foreground.
- **Right-Click Menu**:
  - **`Open Settings`**: Restores and displays the main window.
  - **`Start with Windows`**: Toggles automatic startup with Windows.
  - **`Restart`**: Restarts the application normally.
  - **`Restart as Administrator`**: Restarts with elevated UAC privileges (automatically hidden if already running as Administrator).
  - **`Exit`**: Exits the application.

---

### 2.4 Global Settings Dialog
Opened via `[Settings]` on the main window. Manages default values for newly added processes and system-wide dedicated core protection masks.

| Item | Summary & Role | Operation Details |
|:---|:---|:---|
| **Core Assignment Table** | Configures default dedicated audio core (Row 0: `Audio Core`) and excluded cores (Row 1: `Excluded`) with automatic mutual exclusion and minimum 1-core protection. | Click cell to toggle (mouse wheel over table scrolls horizontally) |
| **`Default Audio Core (Mask)`** / **`Excluded (Mask)`** | Specifies default audio core(s) and excluded cores via bitmask (or comma-separated core numbers). | Direct input or table click |
| **`Apply` (Audio Core Row)** | **[Dedicated Core Protection (Exclusive Checkbox)]** Applies an affinity mask excluding the audio core to all non-ATI processes (except `UnlistedExclusions`) and automatically tracks newly spawned processes at `INI×10` polling intervals. | Check to enable (mutually exclusive with Excluded row) / uncheck to stop polling |
| **`Apply` (Excluded Row)** | **[Apply Excluded Mask (Exclusive Checkbox)]** Applies the specified excluded core mask to all non-ATI processes (except `UnlistedExclusions`) and automatically tracks newly spawned processes at `INI×10` polling intervals. | Check to enable (mutually exclusive with Audio Core row) / uncheck to stop polling |
| **`Polling Interval (ms)`** | Polling cycle for monitoring thread (`100` / `200` / `500` / `1000` ms). | Number input (auto-corrected) |
| **`Unmask All`** | Becomes active (green) only when both `Apply` checkboxes are unchecked while masks are applied; explicitly clears protection masks from all non-monitored processes and restores all-core OS defaults. | Click to unmask |
| **`OK` / `Cancel`** | Saves (OK) or discards (Cancel) settings and closes. | Click |

---

### 2.5 Thread Isolation Settings Dialog
Opened by clicking a target cell (Col 1 / 6 / 7 / 8 / 9) in the main window's process table or clicking `[Edit]`. Uses the same Core Assignment Table (click cell to toggle ON/OFF, scroll mouse wheel over table to scroll horizontally), mask inputs, and thread priority dropdown as Global Settings (Section 2.4) to configure process-specific settings for the selected `Target Process` and save them to `ATI.ini` upon clicking `OK` (comma-separated multi-core assignment such as `1, 2` is also supported for `Audio Core`).

---

## 3. Application-Specific Detection Behavior

- **Chromium Browsers (Chrome, Edge, Brave, Vivaldi, etc.)**:
  Automatically identifies the internal dedicated audio service process across multi-process browser trees, tracking and isolating active audio output threads in real time.
- **Standard Audio & Media Apps (foobar2000, mpv, Firefox, etc.)**:
  Identifies playback streaming threads from CPU activity and audio processing characteristics, automatically transitioning to sleep (`sleeping...`) during silent standby.
- **Voicemeeter Series (`AppType: 5`)**:
  Automatically recognizes all Voicemeeter editions—Standard (`voicemeeter` / `voicemeeter_x64`), Banana (`voicemeeterpro` / `voicemeeterpro_x64`), and Potato (`voicemeeter8` / `voicemeeter8_x64`)—and expands into **2 rows** upon registration:
  - Row 1 (Parent): `VBAN - <ProcessName>` (network audio streaming thread)
  - Row 2 (`└`): `VAIO/AUX & DSP -` (main audio engine handling physical Hardware OUT, virtual I/O, mixing, and DSP processing)
  - **2-Line Independent INI Format**: Saved and restored in `ATI.ini` as two lines (`<ProcessName>(1)=...`, `<ProcessName>(2)=...`), storing each slot's `AudioCore` (single core number or hex mask such as `0xC0C`), `AudioPriority`, and `Bypass:1` independently. If either of the two lines is missing on startup due to a manual editing mistake, the entry is ignored; if missing upon exit, both lines are cleared from `ATI.ini`.
  - **Per-Slot Bypass (Mask Switch)**: Clicking the `!` column (Col 0) or `PID / TID` column (Col 5) on either of the two rows toggles Bypass ON / OFF for that individual slot. When Bypass is ON, the slot's thread keeps its priority unchanged while the normal eviction mask (`NormalCores`) is immediately applied; toggling Bypass OFF immediately reapplies the slot's dedicated audio core mask.
  - The edit dialog edits the slot of the row that was clicked (or selected for `[Edit]`); multiple cores can be toggled or entered as a hexadecimal mask in the `Audio Core` row.
- **Windows Audio Engine (`audiodg.exe`)**:
  In the Windows Audio Device Graph Isolation process `audiodg.exe` (fixed as `AppType:0`), internal audio threads continuously replace one another. Instead of locking onto a single TID, ATI **applies the configured audio core mask, process priority class, and HighQoS (once upon PID detection) directly to the `audiodg.exe` process (PID), and applies the configured thread priority to all underlying threads (all TIDs)** (displayed as `PID <pid> / -` in Col 5, showing the effective priority of the active main audio thread rather than idle standby threads in Col 2, and incrementing `Chgs` by `+1` upon initial PID lock regardless of thread count). On subsequent polling turns, ATI detects newly spawned TIDs and immediately applies the thread priority while incrementing `Chgs` by `+1`. When `audiodg.exe` is removed from the list, switched to Bypass, or when ATI exits, the PID is restored to all cores and `Normal` priority class, and all TIDs are restored to `+2` (`Highest`).
- **Roon Dedicated Control (`AppType: 4` — `RoonAppliance.exe` / `RAATServer.exe` / `RoonBridge.exe`)**:
  - **Server PC (Roon Server / Core)**: Register `RoonAppliance.exe` in the ATI instance running on the server machine.
  - **Renderer PC (Endpoint / Roon Bridge)**: Register `RAATServer.exe` in the ATI instance running on the endpoint machine (registering `RoonBridge.exe` will automatically fall back to `RAATServer.exe`).
- **3-Second Steady-State Monitoring (`AppType: 1, 3, 4, 5`)**:
  Except for `audiodg.exe` (`AppType: 0`) and Chromium (`AppType: 2`) where audio threads frequently rotate, processes that have stabilized after audio thread isolation (as well as `RAATServer.exe` during standby) automatically throttle thread enumeration and affinity/priority maintenance checks to once every ~3 seconds while continuing to detect process exit at the standard polling interval (UI edits and manual re-search apply immediately).

---

## 4. Configuration Reference (ATI.ini)

Configuration is saved in `ATI.ini` in the same directory as the executable.

### 4.1 Global Section `[Global]`

| Key | Description | Valid Range | Default |
|:---|:---|:---|:---|
| **`DefaultAudioCore`** | Default audio core (or comma-separated multi-core list) assigned to newly registered processes. | `0` to (Max Cores - 1) | `1` (`0` for single-core) |
| **`NormalCores`** | Hexadecimal mask of cores allocated to normal threads. | Hex bitmask (e.g., `0x3F3`) | All cores excluding `DefaultAudioCore` |
| **`PollingIntervalMs`** | Normal polling interval in milliseconds. | `100`, `200`, `500`, `1000` | `500` |
| **`BoostPollingIntervalMs`** | Fast polling interval during thread search or state transitions. | `50`, `100`, `250` | `250` |
| **`AlwaysOnTop`** | Main window always-on-top state. | `0` (Disabled), `1` (Enabled) | `0` |
| **`ColumnWidths`** | Saved column widths of the main window process table (all 10 columns). | Comma-separated integers (10 values) | Default column widths |
| **`WindowHeight`** | Saved vertical window height of the main window (in pixels). | Positive integer | Auto-calculated height |
| **`UnlistedMask`** | Automatic affinity protection mode for Unlisted processes. | `0` (OFF), `1` (Exclude Audio Core), `2` (Apply Excluded Mask) | `0` |
| **`UnlistedExclusions`** | Comma-separated list of executable names checked in the `!` column of `Select Process` to exclude from Unlisted mask targets. | Comma-separated exe names | Empty |

### 4.2 Process Section `[Processes]`

Saved per registered process under the executable name as key, with comma-separated parameters:

| Parameter | Description | Valid Values |
|:---|:---|:---|
| **`AudioCore`** | Core or hexadecimal mask assigned to audio thread. Comma-separated for multiple slots/cores (e.g., `AudioCore:11, 3, 0xC00`). | `0` to (Max Cores - 1), hex mask (`0x`...), comma-separated |
| **`AudioPriority`** | Priority for audio thread (7 relative steps or `F`-prefixed fixed priority). Comma-separated for multiple slots (e.g., `AudioPriority:-15, F19`). | `-15`, `-2`, `-1`, `0`, `1`, `2`, `15`, or `F1` to `F31`, comma-separated |
| **`ProcessPriority`** | Process priority class explicitly selected via the picker (omitted when untouched, keeping the process's native priority). | `Idle`, `Below Normal`, `Normal`, `Above Normal`, `High`, `Realtime` |
| **`NormalCores`** | Eviction core bitmask assigned to normal threads. When multiple cores are enabled, normal threads retain the multi-core affinity mask while their Ideal CPU (`SetThreadIdealProcessor`) is automatically distributed in round-robin order sorted by `Cycles Delta` descending. | Hex mask starting with `0x` |
| **`Bypass`** | Temporary bypass state (`1`: bypassed, omitted: active). | `1` |
| **`AppType`** | Application classification mode (`0`: `audiodg.exe` dedicated, `1`: Standard Audio, `2`: Chromium, `3`: High-load DAW, `4`: Roon dedicated, `5`: Voicemeeter dedicated). | `0`, `1`, `2`, `3`, `4`, `5` |

---

## 5. Limitations & Notes

1. **Usage of Pause (`❚❚`) Feature**:
   - This feature is strictly for verification to temporarily mute audio and confirm that the selected thread is the actual playback thread. Forcibly suspending threads can cause application deadlocks or crashes if misused as a general mute function.
2. **Behavior on Temporary Bypass (`!` / Bypass)**:
   - Entering Bypass stops ATI monitoring and active affinity adjustments for that process. Previously applied thread affinities and priorities are maintained and do not revert immediately to default OS scheduling (as an exception, `audiodg.exe` automatically restores to standard settings upon Bypass, rule removal, or application exit).
3. **Single-Instance Enforcement & Self-Protection**:
   - Multiple instances are prevented; launching ATI while an instance is already running restores the existing instance's system tray icon (recovering from Explorer crashes if the icon disappeared), activates its main window to the foreground, and immediately exits the new instance. To prevent ATI's own monitoring loop from congesting audio cores, ATI automatically restricts its own process affinity to normal cores.
4. **Control Scope by Launch Mode (Standard vs. Administrator)**:
   - Launching ATI as Administrator (e.g., via **`Restart as Administrator`** in the tray menu) unlocks `Realtime` priority (Base 16–31) and expands the range of controllable processes.
   - **Broader Unlisted Mask Coverage**: System services and elevated processes that are skipped in standard user mode due to OS access restrictions become eligible for affinity masking.
   - **Fewer Processes Falling into `sleeping...`**: Processes whose threads cannot be inspected in standard user mode due to insufficient permissions (such as `audiodg.exe` or elevated games/apps) can be properly detected and isolated instead of falling into `sleeping...`.
5. **Unmaskable Processes (Windows Protected Processes)**:
   Due to Windows kernel protection mechanisms (such as PPL: Protected Process Light), the following system processes deny external affinity modification even under Administrator privileges and are automatically skipped:
   - **Kernel & Virtual Memory Management**: `System` (PID 4), `Registry`, `Secure System`, `Memory Compression`
   - **Core Subsystems**: `smss.exe`, `csrss.exe`, `wininit.exe`, `winlogon.exe`
   - **Security & Authentication Subsystems**: `lsass.exe`, `services.exe`, `SecurityHealthService.exe`
   - **Antimalware**: `MsMpEng.exe` (Microsoft Defender), `NisSrv.exe`
   - **Graphics, Font, & Driver Isolation Hosts**: `dwm.exe`, `fontdrvhost.exe`, `WUDFHost.exe`
   - **Protected System `svchost.exe` Instances**: Protected instances hosting security/system events (e.g., RPCSS, DcomLaunch, BrokerInfrastructure)
6. **Normal Thread Ideal CPU Auto-Distribution Scope & Triggers**:
   When multiple cores are assigned to `NormalCores`, normal threads maintain their multi-core affinity mask while their Ideal CPU (preferred core) is distributed in round-robin order sorted by `Cycles Delta` descending.
   - **Difference between Affinity Mask and Ideal CPU**:
     - **Affinity Mask (Hard Constraint)**: The set of CPU cores on which the thread is permitted to run. The thread cannot execute outside these cores.
     - **Ideal CPU (Scheduling Hint)**: The preferred single core among the permitted cores where the OS scheduler prioritizes running the thread.
   - **Distribution Example**:
     If cores `#0` to `#3` (4 cores) are assigned to `NormalCores`, all normal threads share the affinity mask for `#0` to `#3`, while Ideal CPUs are sequentially assigned starting from the thread with the highest CPU load: `#0 -> #1 -> #2 -> #3 -> #0 -> #1...`. This prevents heavy background threads from clustering onto a single core.
   - **Applicable Targets**: Registered processes possessing standard normal-thread eviction logic, including Standard Audio (`AppType: 1`), High-load DAWs (`AppType: 3`), Roon (`AppType: 4`), and Voicemeeter (`AppType: 5`).
   - **Excluded Targets**:
     - **`audiodg.exe` (`AppType: 0`)**: Excluded because all threads in the process are isolated onto the dedicated audio core mask.
     - **Chromium Browsers (`AppType: 2`)**: Excluded from bulk sorting due to its dedicated isolation path; newly spawned threads are simply assigned to available normal cores sequentially.
     - **Unlisted Processes**: Only process-level affinity masks are applied; per-thread Ideal CPU scheduling is not performed.
   - **Rebalance Triggers**:
     To avoid CPU cache invalidation penalties, each thread's Ideal CPU is kept stable during steady states. Re-sorting and redistribution only occur when newly spawned threads are detected, when the active audio thread changes or is confirmed, or when rules/configurations are modified by the user (thread termination alone does not trigger rebalance).
7. **Nature of Periodically Spawned Background Processes and the Importance of Eviction**:
   The `Unmonitored Processes Masked` counter on the main window gradually increases over time even while the PC is idle. This occurs because Windows and background utilities routinely spawn short-lived processes.
   - **Identity and Purpose of Spawned Processes**:
     - **Windows Maintenance & Diagnostic Tasks**: Programs like `CompatTelRunner.exe` (compatibility telemetry), `DeviceCensus.exe` (hardware inventory), and `taskhostw.exe` (Task Scheduler worker) briefly execute for OS self-diagnostics and servicing.
     - **Automatic Updaters**: Updaters such as `GoogleUpdate.exe`, `MicrosoftEdgeUpdate.exe`, and `MpCmdRun.exe` (Defender signatures) periodically start to check for security updates and patches.
     - **Tool and Subsystem Helpers**: Helper processes like `conhost.exe` and ephemeral worker tasks spawned during command execution or IDE background operations.
   - **Why Eviction is Essential**:
     Under default Windows behavior, any newly spawned process is granted permission to execute across all CPU cores, including dedicated audio cores. If left unchecked, unexpected bursts of telemetry or maintenance tasks will consume cycles on audio cores, causing jitter and buffer underruns. ATI detects these and immediately evicts them to normal cores.
   - **Soft Eviction (Glitch-Free Protection)**:
     To prevent process eviction from interfering with time-critical audio playback, ATI implements pre-flight affinity verification (skipping redundant API calls on child processes that have already inherited the mask) combined with temporal rate-limiting across turns.
