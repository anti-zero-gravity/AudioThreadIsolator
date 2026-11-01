# Audio Thread Isolator (ATI) v1.0.7 - Specification & User Manual (spec.md)

## 1. Application Overview

- **Name**: Audio Thread Isolator (ATI)
- **Type**: Windows Resident Utility
- **Purpose**:
  Automatically identifies the audio playback thread from running audio and multimedia processes, isolating it onto designated audio CPU core(s) while evicting all other non-audio threads to separate excluded cores. By preventing high-load threads from sharing cores with audio threads, the waiting time between a thread being queued in the ready queue and actually being executed is eliminated, minimizing micro-jitter in thread processing timing.
- **Automatic HighQoS (Power Throttling OFF) Enforcement**:
  For all processes registered for monitoring, ATI automatically disables Windows Power Throttling (EcoQoS) upon detection, enforcing HighQoS (maximum execution speed and timer resolution preservation). This ensures all threads run at full clock speed without power-saving latency or audio dropouts.

---

## 2. UI Structure and Operations

### 2.1 Main Window
- **Overview**: The primary monitoring and quick-control interface for registered processes (bypass, suspend, priority adjustment). Automatically adapts layout and font scaling across resolution or DPI changes.

#### Control Buttons (Right Panel)
- **`Always on Top`**: Pins the main window to always stay in the foreground.
- **`[Edit]`**: Opens the Thread Isolation Settings dialog (Section 2.5) for the selected process (or double-click row).
- **`[Add]`**: Opens the Process Picker dialog (Section 2.2).
- **`[Remove]`**: Removes the selected process from the monitoring list.
- **`[Settings]`**: Opens the Global Settings dialog (Section 2.4).
- **`[Exit]`**: Exits the application.

#### Process Table (Monitoring List)

| Col | Header | Summary & Role | Operation Details |
|:---:|:---|:---|:---|
| 0 | **`!`** | Toggles monitoring and temporary bypass. Checking this suspends ATI intervention. | Click to toggle |
| 1 | **`Process Name`** | Application executable name without `.exe`.<br>When multi-row expansion is active for Voicemeeter (`AppType: 5`), displays each thread's role label and draws an L-shaped tree line (`└`) on child rows. For Roon (`AppType: 4`), displays the original process name (`RoonAppliance` / `RAATServer`) as a single row. | Double-click to open edit dialog.<br>Click header to sort ascending ("SORT?" prompt). |
| 2 | **`Thread Priority`** | Thread priority level and effective Base Priority (e.g., `Below 9`, `Time Critical 15`, or `Below FG 8 - BG 6` when Process Priority is `Normal` with Foreground/Background variation).<br>When multi-thread expansion is active, each row can independently select priority via the 2D Priority Matrix Picker. | Click cell to select via 2D Priority Matrix Picker |
| 3 | **`Process Priority`** | Priority class applied to the process (PID) hosting the audio thread (e.g., `Normal`, `Above Normal`, `High`). Automatically reads and displays the live process priority upon registration. | Click cell to select via 2D Priority Matrix Picker |
| 4 | **`❚❚`** | Verification pause feature. Suspends the target audio thread to stop audio, confirming if it is the genuine playback thread.<br>When multi-thread expansion is active, each row (TID) can be suspended independently.<br>For `audiodg.exe`, suspends and resumes the entire process (all threads) at once.<br>**(WARNING: Strictly prohibited to use as mute due to risk of app deadlocks)** | Click to toggle suspension |
| 5 | **`Audio Core PID/TID`** | Displays detection state, isolated PID, and TID. | Click operation: Restarts search when sleeping; toggles Bypass when running. |
| 6 | **`TIDs`** | Monitored thread count. Shows `1` for standard confirmed apps, total process thread count during search or for Chromium / `audiodg.exe`, and `0` when sleeping. | Double-click to open edit dialog. |
| 7 | **`Chgs`** | Count of times audio thread was detected and priorities were applied. | Double-click to open edit dialog. |
| 8 | **`Audio`** | Dedicated audio CPU core number (e.g., `#1`). When multi-thread expansion is active, shows the core assigned to each thread. | Double-click to open edit dialog. |
| 9 | **`Excluded`** | List of CPU cores excluded from normal thread placement (e.g., `!#1`). In multi-thread expansion, all slots directly display the held mask. | Double-click to open edit dialog. |

- **Dedicated Multi-Row Expansion (Voicemeeter)**:
  - All Voicemeeter editions (`AppType: 5`, 2 rows) automatically expand into role-specific rows and isolate each network/audio engine thread independently.
  - **10-Second Scan Countdown & `sleeping...` Transition**:
    - Continuously scans for unconfirmed slots for 10 seconds, displaying `Standby 10` down to `Standby 1` on pending rows.
    - Slots remaining undetected after 10 seconds (for example, when VBAN is disabled in Voicemeeter) transition to `sleeping...`. Clicking a sleeping row (Col 0 or Col 5) restarts the 10-second countdown scan while preserving already-confirmed threads.
  - Editing and saving settings via `[Edit]` or double-clicking any sibling row updates all expanded rows of the process simultaneously.
- **Roon Dedicated Continuous Standby (`AppType: 4`)**:
  - `RoonAppliance.exe` and `RAATServer.exe` operate as single-slot (1 row) rules.
  - During silence or pause, they never transition to `sleeping...`; like Chromium (`AppType: 2`), they continuously remain in `Searching...` to immediately isolate newly active threads upon playback resumption.

- **Rank Selection via 2D Priority Matrix Picker**:
  - Clicking the `Thread Priority` (Col 2) or `Process Priority` (Col 3) cell pops up a 7x7 3D visual matrix.
  - Hovering over pillars displays rank details (Base Priority, Rank #, Process Class, Thread Priority), and clicking a pillar (Rank 1 to 49) immediately applies both Process Class and Thread Priority to the active process/threads and saves them to `ATI.ini`.
  - When hovering or selecting `Normal` priority class, both the Foreground and Background rows illuminate simultaneously to reflect Windows' dynamic foreground/background priority switching, displaying both FG/BG effective priorities and an explanatory note in the right-hand HUD panel.

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
| (Half-Isolated state) | ● Flashing Dull Yellow / Teal | Thread unmigratable due to driver constraints co-existing on audio core. Promotes `Idle (-15)` audio threads to `Lowest (-2)` and throttles co-existing threads one level below. | Transitions to Bypass (displays ☑ in ! col) |

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

- **Realtime Priority (Base Priority 16–31)**:
  - Ranks 43–49 in the 2D Priority Matrix Picker (Realtime priority class) are available only when ATI is running as Administrator (automatically falls back to High priority class / max Base 15 in standard user mode).

---

### 2.2 Process Picker Dialog
- Launched via `[Add]` on the main window. Select running processes from the alphabetical list and click `[Done]` to add them to the monitoring list (initialized with global default settings).
- Checking the leading red **`!` column** registers the process in `UnlistedExclusions`, excluding it from Unlisted process masking targets (hovering over the `!` column displays an explanatory note at the bottom).

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
| **Core Assignment Table** | Configures default dedicated audio core (Row 0: `Audio Core`) and excluded cores (Row 1: `Excluded`) with automatic mutual exclusion and minimum 1-core protection. | Select via checkboxes |
| **`Default Audio Core (Mask)`** / **`Excluded (Mask)`** | Specifies default audio core(s) and excluded cores via bitmask (or comma-separated core numbers). | Direct input or table click |
| **`Apply` (Audio Core Row)** | **[Dedicated Core Protection (Exclusive Checkbox)]** Applies an affinity mask excluding the audio core to all non-ATI processes (except `UnlistedExclusions`) and automatically tracks newly spawned processes at `INI×10` polling intervals. | Check to enable (mutually exclusive with Excluded row) / uncheck to stop polling |
| **`Apply` (Excluded Row)** | **[Apply Excluded Mask (Exclusive Checkbox)]** Applies the specified excluded core mask to all non-ATI processes (except `UnlistedExclusions`) and automatically tracks newly spawned processes at `INI×10` polling intervals. | Check to enable (mutually exclusive with Audio Core row) / uncheck to stop polling |
| **`Audio Thread Priority`** | Default priority applied to newly added processes. | Select from dropdown |
| **`Polling Interval (ms)`** | Polling cycle for monitoring thread (`100` / `200` / `500` / `1000` ms). | Number input (auto-corrected) |
| **`Unmask All`** | Becomes active (green) only when both `Apply` checkboxes are unchecked while masks are applied; explicitly clears protection masks from all non-monitored processes and restores all-core OS defaults. | Click to unmask |
| **`OK` / `Cancel`** | Saves (OK) or discards (Cancel) settings and closes. | Click |

---

### 2.5 Thread Isolation Settings Dialog
Opened by double-clicking a process row or clicking `[Edit]` on the main window. Uses the same Core Assignment Table, mask inputs, and thread priority selector as Global Settings (Section 2.4) to configure process-specific settings for the selected `Target Process` and save them to `ATI.ini` upon clicking `OK` (comma-separated multi-core assignment such as `1, 2` is also supported for `Audio Core`).

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
  - The edit dialog edits the slot of the row that was double-clicked (or selected for Edit); multiple cores can be toggled or entered as a hexadecimal mask in the `Audio Core` row.
- **Windows Audio Engine (`audiodg.exe`)**:
  In the Windows Audio Device Graph Isolation process `audiodg.exe` (fixed as `AppType:0`), internal audio threads continuously replace one another. Instead of locking onto a single TID, ATI **applies the configured audio core mask, process priority class, and HighQoS directly to the `audiodg.exe` process (PID), and applies the configured thread priority to all underlying threads (all TIDs)** (displayed as `PID <pid> / -` in Col 5, incrementing `Chgs` by `+1` upon initial PID lock regardless of thread count). On subsequent polling turns, ATI detects newly spawned TIDs and immediately applies the thread priority while incrementing `Chgs` by `+1`. When `audiodg.exe` is removed from the list, switched to Bypass, or when ATI exits, the PID is restored to all cores and `Normal` priority class, and all TIDs are restored to `+2` (`Highest`).
- **Roon Dedicated Control (`AppType: 4` — `RoonAppliance.exe` / `RAATServer.exe` / `RoonBridge.exe`)**:
  - **Server PC (Roon Server / Core)**: Register `RoonAppliance.exe` in the ATI instance running on the server machine.
  - **Renderer PC (Endpoint / Roon Bridge)**: Register `RAATServer.exe` in the ATI instance running on the endpoint machine (registering `RoonBridge.exe` will automatically fall back to `RAATServer.exe`).

---

## 4. Configuration Reference (ATI.ini)

Configuration is saved in `ATI.ini` in the same directory as the executable.

### 4.1 Global Section `[Global]`

| Key | Description | Valid Range | Default |
|:---|:---|:---|:---|
| **`DefaultAudioCore`** | Default audio core assigned to newly registered processes. | `0` to (Max Cores - 1) | `1` (`0` for single-core) |
| **`DefaultAudioPriority`** | Default thread priority assigned to newly registered processes. | `-15` (Idle) to `+15` (Time Critical) | `-15` |
| **`NormalCores`** | Hexadecimal mask of cores allocated to normal threads. | Hex bitmask (e.g., `0x3F3`) | All cores excluding `DefaultAudioCore` |
| **`PollingIntervalMs`** | Normal polling interval in milliseconds. | `100`, `200`, `500`, `1000` | `500` |
| **`BoostPollingIntervalMs`** | Fast polling interval during thread search or state transitions. | `50`, `100`, `250` | `250` |
| **`AlwaysOnTop`** | Main window always-on-top state. | `0` (Disabled), `1` (Enabled) | `0` |
| **`UnlistedMask`** | Automatic affinity protection mode for Unlisted processes. | `0` (OFF), `1` (Exclude Audio Core), `2` (Apply Excluded Mask) | `0` |
| **`UnlistedExclusions`** | Comma-separated list of executable names checked in the `!` column of `Select Process` to exclude from Unlisted mask targets. | Comma-separated exe names | Empty |

### 4.2 Process Section `[Processes]`

Saved per registered process under the executable name as key, with comma-separated parameters:

| Parameter | Description | Valid Values |
|:---|:---|:---|
| **`AudioCore`** | Core or hexadecimal mask assigned to audio thread. Comma-separated for multiple slots/cores (e.g., `AudioCore:11, 3, 0xC00`). | `0` to (Max Cores - 1), hex mask (`0x`...), comma-separated |
| **`AudioPriority`** | Relative priority for audio thread. Comma-separated for multiple threads (e.g., `AudioPriority:-15, -15`). | `-15`, `-2`, `-1`, `0`, `1`, `2`, `15`, comma-separated |
| **`NormalCores`** | Eviction core bitmask assigned to normal threads. | Hex mask starting with `0x` |
| **`Bypass`** | Temporary bypass state (`1`: bypassed, omitted: active). | `1` |
| **`AppType`** | Application classification mode (`0`: `audiodg.exe` dedicated, `1`: Standard Audio, `2`: Chromium, `3`: High-load DAW, `4`: Roon dedicated, `5`: Voicemeeter dedicated). | `0`, `1`, `2`, `3`, `4`, `5` |

---

## 5. Limitations & Notes

1. **Usage of Pause (`❚❚`) Feature**:
   - This feature is strictly for verification to temporarily mute audio and confirm that the selected thread is the genuine playback thread. Forcibly suspending threads can cause application deadlocks or crashes if misused as a general mute function.
2. **Behavior on Temporary Bypass (`!` / Bypass)**:
   - Entering Bypass stops ATI monitoring and active affinity adjustments for that process. Previously applied thread affinities and priorities are maintained and do not revert immediately to default OS scheduling (as an exception, `audiodg.exe` automatically restores to standard settings upon Bypass, rule removal, or application exit).
3. **Single-Instance Enforcement & Self-Protection**:
   - Launching a second instance automatically terminates the previous instance and starts anew. To prevent ATI's own monitoring loop from congesting audio cores, ATI automatically restricts its own process affinity to normal cores.
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
