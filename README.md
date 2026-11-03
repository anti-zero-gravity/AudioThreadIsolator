[English](README.md) | [日本語](README.ja.md)

---

# Audio Thread Isolator (ATI)

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-F16061?style=flat-square&logo=ko-fi&logoColor=white)](https://ko-fi.com/antizerogravity)

**Audio Thread Isolator (ATI)** is a lightweight Windows background utility that automatically detects, isolates, and optimizes real-time audio playback threads (WASAPI, ASIO, etc.) to dedicated CPU cores.

By isolating critical audio threads onto dedicated cores and migrating heavy background threads (UI rendering, decoding, network I/O, etc.) to other cores, ATI effectively minimizes buffer underruns, audio dropouts, and micro-jitter caused by CPU core contention.

---

## Key Features

- **Ultra-Lightweight Native Architecture**:
  - Implemented in modern C++17 using pure Win32 API and Common Controls.
  - Zero external dependencies or runtime requirements (no .NET or VC++ runtimes).
  - Single compact binary (~1.15 MB) with negligible memory footprint (~1–2 MB) and near-zero CPU usage.
- **Audio Thread Detection Engine**:
  - Automatically identifies real-time playback threads across media players, browsers (Chromium / Vivaldi / Chrome), DAWs, and audio applications via thread descriptions, module inspection, and application-specific profiles.
- **Multi-Core Affinity Support**:
  - Assign playback threads to one or multiple dedicated CPU cores (e.g., `#1` or `#1, #2`) to handle heavy real-time audio workloads without dropouts.
- **Always on Top & Window Resizing**:
  - Compact right-side controls with an instant `Always on Top` checkbox.
  - Fully resizable window layout (`WS_THICKFRAME`) with automatic DPI scaling (Per-Monitor V2).
- **System Tray Integration**:
  - Seamlessly minimize or close (`×`) to the system notification area (system tray).
  - Supports auto-start on Windows boot via registry integration.
- **Realtime Priority (Base 16–31) & Expanded Control Scope via Administrator Launch**:
  - Elevate seamlessly via the taskbar system tray context menu (`Restart as Administrator`).
  - Automatically enables `SeIncreaseBasePriorityPrivilege` and `SeDebugPrivilege`, unlocking the Windows kernel Realtime priority class (Base Priority 16–31) in the 2D Priority Matrix Picker (silently falls back to High priority when not running as Administrator).
  - **Control Scope by Launch Mode**: Launching in Administrator mode grants access to system services and elevated processes, increasing the number of processes eligible for Unlisted Hard Affinity masking and reducing processes (such as `audiodg.exe` or elevated games/apps) that would otherwise fall into `sleeping...` due to insufficient thread inspection permissions.
- **Automatic Hard Affinity Eviction & Per-Process Exclusion for Unlisted Processes**:
  - Checking either mutually exclusive `Apply` checkbox in Global Settings (Exclude `Default Audio Core` or Apply `Excluded` mask) applies `SetProcessAffinityMask` to all non-monitored processes and automatically masks newly spawned PIDs at `INI×10` polling intervals (displaying `● <count> Unmonitored Processes Masked` at the head of the main window legend).
  - Processes checked in the leading red `!` column of `Select Process` (`[Add]`) are individually excluded from unlisted polling mask targets (`UnlistedExclusions`).
  - Unchecking `Apply` stops polling for new processes while keeping existing masks; clicking the green `Unmask All` button explicitly restores OS default full-core affinity across all non-monitored processes.

---

## How It Works

ATI inspects threads within registered processes using the following detection methods:

### 1. Thread Description Keyword Matching
- Queries thread descriptions via Windows 10+ native API (`GetThreadDescription`).
- Matches against known audio keywords and patterns:
  - **Media Players**: `ao/*` (e.g., `ao/wasapi`, `ao/pipewire`).
  - **Chromium Browsers**: `wasapi_render_thread`, `AudioOutputDevice`, `AudioThread`, `CrAudio`.
  - **Generic Keywords**: `audio`, `playback`, `wasapi`, `asio`.


### 2. Module Inspection (Start Address & Stack Pointers)
- For specialized audio software (DAWs, VST hosts, editors) that do not set thread descriptions, ATI queries thread start addresses (`Win32StartAddress`) and stack references.
- Determines whether the thread was spawned by an audio driver or related module:
  - **ASIO Driver DLLs**: Automatically reads installed ASIO driver paths from the Windows Registry (`HKLM\SOFTWARE\ASIO`, etc.) to match active driver modules.
  - **Windows Audio Core DLLs**: Matches core audio modules such as `audioses.dll`, `dmusic.dll`, `avrt.dll`, etc.

### 3. CPU Cycle Delta Sampling
- When threads cannot be identified by name alone, ATI samples thread CPU cycle consumption (`QueryThreadCycleTime`) over polling intervals to evaluate active playback threads.

### 4. Application-Specific Profiles
- **Windows Audio Engine (`audiodg.exe`)**:
  - Because internal threads change dynamically, ATI applies the dedicated audio core mask, process priority class, and HighQoS to the entire process (PID), enforcing designated thread priority across all active TIDs (requires Administrator mode due to `LOCAL SERVICE` privilege).
- **Chromium Browsers**:
  - Automatically identifies the dedicated Audio Service utility process and isolates its playback thread.
- **Voicemeeter**:
  - Automatically expands and manages both network I/O and main audio engine threads across dedicated rows.
- **Roon**:
  - Directly identifies and isolates the core PCM streaming thread in a single slot.

---

## Usage & Recommended Setup

### 1. Prerequisites (Designating Dedicated Audio CPU#)
Under default settings, once a registered application is detected and monitored, ATI isolates and protects only real-time audio playback threads onto default CPU `#1`, while all other threads (UI rendering, decoding, etc.) are distributed and migrated to other CPU# cores by Windows scheduler policy.

To maximize the benefits of ATI, we strongly recommend preparing **dedicated CPU# cores exclusively reserved for real-time audio processing** on multi-core / multi-threaded processors:

1. **Migrating Device Interrupts (MSI)**:
   - Use [MsiAffinityUtility-v9](https://github.com/anti-zero-gravity/MsiAffinityUtility-v9) to configure the interrupt affinity of all MSI-supported devices (including GPUs) away from your chosen audio CPU# cores (non-MSI devices remain unverified), and restart Windows immediately afterward.
2. **Guidelines for CPU# Selection**:
   - Choose a CPU# other than `#0` (and other than `#0` and `#1` when Hyper-Threading is enabled) for real-time audio threads to avoid contention with heavy OS and system interrupt workloads.
3. **Onboard & Network Audio Considerations**:
   - **Onboard HD-Audio**: We recommend designating audio CPU# cores or preparing a dedicated CPU# exclusively for the HD-Audio device.
   - **Network Audio Streaming (NAA, etc.)**: When streaming audio data over LAN to devices such as a Network Audio Adapter (NAA), we recommend assigning a dedicated CPU# for the network interface controller.

### 2. Initial Configuration in ATI
- **DefaultAudioCore**:
  - Specify the dedicated audio CPU# (comma-separated values supported for multiple cores).
- **ExcludedCores**:
  - Specify the CPU# mask permitted for all regular processes and background threads, excluding the dedicated audio CPU# (and any dedicated LAN CPU#).
- **Automatic Unlisted Process Masking (`Apply` Checkboxes / `Unmask All`)**:
  - Checking either `Apply` checkbox in Global Settings applies Hard Affinity protection to all unlisted processes and automatically tracks newly launched processes.
  - To exclude specific non-monitored processes from unlisted polling masks, open `[Add]` (`Select Process`), check the leading red **`!` column**, and click `[Done]`.
  - For full UI and configuration details, see the [User Manual & Specification (docs/spec.md)](docs/spec.md).

### 3. Thread Priority & Sound Quality Philosophy
- **Advantages of Low Priority Operation (Idle)**:
  - Guided by the design philosophy that "Priority 1 (Idle) yields the highest sound fidelity by minimizing jitter and CPU contention," ATI sets `Idle (-15)` as the default audio thread priority. Setting the processor quantum to ["Short / Variable / 1:1"](https://hackmd.io/@PsD5syoNTWq95HbQ6lFbtA/BkMrmGLqMx#2-3-Quantum-Length-Table-and-Tuning-Quick-Reference-by-Use-Case) is strongly recommended.
- **Flexible Per-Process Customization**:
  - Depending on your specific Windows system configuration and background load, audio dropouts may occur. In addition to global default settings, ATI allows independent per-process adjustment of both process priority and audio thread priority.
- **Catering to Diverse Listening Preferences**:
  - While ATI's core philosophy centers on "the lowest priority that guarantees dropout-free playback achieves the highest audio quality," the 2D Priority Matrix Picker also enables instant elevation up to `Realtime (31)`. This allows users to audition and compare subtle sound quality characteristics across the entire priority spectrum in real time.

### 4. Tips (Application-Specific Configuration)
- **Roon**:
  - Roon consists of multiple distinct processes (`Roon.exe`, `RoonAppliance.exe`, `RoonServer.exe`, and `RAATServer.exe`).
  - **Server PC (Roon Server / Core)**: Register `RoonAppliance.exe` in the ATI instance running on the server machine.
  - **Renderer PC (Endpoint / Roon Bridge)**: Register `RAATServer.exe` in the ATI instance running on the endpoint machine (registering `RoonBridge.exe` will automatically fall back to `RAATServer.exe`).
  - (*Note: `Roon.exe` handles GUI and `RoonServer.exe` manages system services; neither performs real-time audio streaming.)
- **Voicemeeter + VBAN Receptor (Network Audio Configuration)**:
  - Recommended environment when running Voicemeeter on the server PC and VBAN Receptor on the renderer PC with both threads executing at low priority:
    - **Network Connection**: Direct Ethernet LAN cable connection between Server PC and Renderer PC
    - **NIC Advanced Properties**: Enable all hardware offloads, disable Flow Control, set Speed & Duplex to "10M FULL"
    - **ATI Core Allocation**: The recommended number of assigned Audio Cores for Voicemeeter's `VAIO/AUX & DSP` slot is 4 cores or more (e.g. multi-core allocation via hex mask).

---

## Download

Download the pre-compiled standalone binary (`ATI.exe`) from the [Releases](https://github.com/anti-zero-gravity/AudioThreadIsolator/releases) page.

---

## Building and Installation

### Requirements
- **OS**: Windows 10 / Windows 11 (64-bit)
- **Compiler**: MinGW-w64 `g++` (C++17 / UCRT runtime), `windres`, and `strip`

### Setting Up on a Clean / Non-Developer Windows Machine
If building on a fresh Windows environment or via an automated AI agent without pre-installed developer tools, install the MinGW-w64 toolchain using Windows Package Manager (`winget`):

1. **Install MinGW-w64 (WinLibs UCRT)**:
   Run the following command in PowerShell:
   ```powershell
   winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e --accept-source-agreements --accept-package-agreements
   ```
2. **Refresh Environment Path**:
   Restart PowerShell, or reload the PATH in the current session:
   ```powershell
   $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
   ```
3. **Verify Installation**:
   Confirm that `g++ --version` prints the compiler details.

### Build Instructions
Execute the automated build script in the repository root (handles resource compilation, full static linking, and binary stripping):

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

*Alternatively, to compile manually via CLI without the script:*
```powershell
mkdir build -ErrorAction SilentlyContinue
windres src/resource.rc -O coff -o build/resource.res
g++ -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ src/main.cpp src/process_picker.cpp src/isolator.cpp src/priority_matrix_picker.cpp build/resource.res -lcomctl32 -lshlwapi -ldwmapi -lpsapi -lgdiplus -o ATI.exe
strip ATI.exe
```

Upon successful compilation, a single standalone executable `ATI.exe` (~4.1 MB) will be generated in the root directory.

---

## License
MIT License.
