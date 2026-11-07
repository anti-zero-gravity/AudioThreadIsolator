[English](README.md) | [日本語](README.ja.md)

---

# Audio Thread Isolator (ATI)

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-F16061?style=flat-square&logo=ko-fi&logoColor=white)](https://ko-fi.com/antizerogravity)

**Audio Thread Isolator (ATI)** is a lightweight Windows background utility that automatically scans, identifies, and isolates audio output threads of music players, video players, web browsers, DAWs, and games onto dedicated CPU cores designated by the user.

ATI was designed to evacuate heavy, noisy regular threads—such as UI rendering, decoding, and network communication—away from dedicated audio cores to keep contention and interference away from the audio thread, allowing listeners to enjoy the distinctive sound quality achieved when lowering the audio thread priority as far as possible.
By preparing an environment coordinated with device AffinityMask and priority settings via MSI Affinity V9 alongside MMCSS registry priority tuning, ATI reduces interrupts both to and from the audio thread, improving PC music playback sound quality across web browsing, gaming, and every other scenario.

---

## Key Features

- **Low-Resource Background Operation**
- **Automatic Playback Thread Detection & Dynamic Priority Display**:
  - Identifies and remembers the audio thread as the configuration target, acquiring and displaying its dynamic priority upon identification.
- **Multi-Core Affinity Support**:
  - Identified threads can be assigned across multiple CPU cores.
- **Intuitive Priority Control via 2D Priority Matrix Picker**
- **Always on Top & Window Resizing**
- **Intuitive GUI & System Tray Integration**:
  - Add target applications via checkboxes from the running process list.
  - Minimize or close (`×`) to the system notification area (system tray).
  - Supports auto-start on Windows boot.
- **Realtime Priority & `audiodg.exe` Management via Restart as Administrator**:
  - Elevate with one click from the system tray right-click menu (`Restart as Administrator`).
  - Automatically enables OS privileges, unlocking the Windows kernel Realtime priority class (Base Priority 16–31) normally restricted under standard user launch.
  - **Control Scope Differences by Launch Mode**:
    - Enables priority configuration and core isolation for the PPL (Protected Process Light) protected `audiodg.exe` process.
    - Increases the number of processes eligible for Unlisted affinity masking.
- **Automatic Affinity Eviction & Per-Process Exclusion for Unlisted Processes**:
  - Checking either mutually exclusive checkbox in Global Settings (Exclude `Default Audio Core` / Apply `Excluded` mask) applies `SetProcessAffinityMask` across all processes not registered in ATI and automatically applies it to newly launched processes at `INI×10` intervals. The cumulative count is displayed at the bottom-left as `● <count> Unmonitored Processes Masked`.
  - Processes checked in the leading `!` column of `[Add]` (`Select Process`) are excluded from unlisted all-process masking.
  - Stopping unlisted all-process monitoring lights up `Unmask All` in green; clicking it clears masks across all processes at once.

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
  - Automatically expands and manages both network I/O and main audio engine threads across 2 dedicated rows.
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
  - Following the tuning policy that "Priority 1 achieves the best sound quality," stutters or audio dropouts can be expected as a result of thread execution being deferred. Setting the processor quantum to ["Short / Variable / 1:1"](https://hackmd.io/@PsD5syoNTWq95HbQ6lFbtA/BkMrmGLqMx#2-3-Quantum-Length-Table-and-Tuning-Quick-Reference-by-Use-Case) is strongly recommended.
- **Flexibility for Diverse Needs**:
  - While ATI is built on the philosophy that "the lowest priority free of audio dropouts delivers superior sound quality," extreme configurations are also possible—such as keeping the process priority at Idle while setting only the audio thread to Priority 31, or conversely setting the process to Realtime while setting the audio thread priority to 1.
  - You can also create unallocated CPU cores left unchecked in both the Audio mask and the Excluded (all other threads) mask. These spare cores are intended for signal paths spanning multiple processes, enabling dedicated 1-process-per-core isolation such as `#4 Playback App → #5 Voicemeeter → #6 audiodg → #7 LAN Device → Remote PC`.

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
