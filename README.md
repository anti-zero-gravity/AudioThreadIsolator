[English](README.md) | [日本語](README.ja.md)

---

# Audio Thread Isolator (ATI)

**Audio Thread Isolator (ATI)** is a lightweight Windows background utility that automatically detects, isolates, and optimizes real-time audio playback threads (WASAPI, ASIO, etc.) to dedicated CPU cores.

By isolating critical audio threads onto dedicated cores and migrating heavy background threads (UI rendering, decoding, network I/O, etc.) to other cores, ATI effectively minimizes buffer underruns, audio dropouts, and micro-jitter caused by CPU core contention.

---

## Key Features

- **Ultra-Lightweight Native Architecture**:
  - Implemented in modern C++17 using pure Win32 API and Common Controls.
  - Zero external dependencies or runtime requirements (no .NET or VC++ runtimes).
  - Single compact binary (~1.15 MB) with negligible memory footprint (~1–2 MB) and near-zero CPU usage.
- **Intelligent 3-Stage Audio Thread Detection Engine**:
  - Automatically identifies real-time playback threads across media players, browsers (Chromium / Vivaldi / Chrome), DAWs, and audio editors (iZotope RX, etc.).
- **Multi-Core Affinity Support**:
  - Assign playback threads to one or multiple dedicated CPU cores (e.g., `#1` or `#1, #2`) to handle heavy real-time audio workloads without dropouts.
- **Always on Top & Window Resizing**:
  - Compact right-side controls with an instant `Always on Top` checkbox.
  - Fully resizable window layout (`WS_THICKFRAME`) with automatic DPI scaling (Per-Monitor V2).
- **System Tray Integration**:
  - Seamlessly minimize or close (`×`) to the system notification area (system tray).
  - Supports auto-start on Windows boot via registry integration.
- **Realtime Priority (Base 16–31) Support via Administrator Launch**:
  - Elevate seamlessly via the taskbar system tray context menu (`Restart as Administrator`).
  - Automatically enables `SeIncreaseBasePriorityPrivilege`, unlocking the Windows kernel Realtime priority class (Base Priority 16–31) in the 2D Priority Matrix Picker (silently falls back to High priority when not running as Administrator).

---

## 3-Stage Audio Thread Detection Engine

ATI employs a robust, 3-stage heuristic engine to reliably detect and isolate audio playback threads in both known and unknown applications:

### 1. Stage 1: Thread Description Keyword Matching
- Queries thread descriptions via Windows 10+ native API (`GetThreadDescription`).
- Matches against known audio keywords and patterns:
  - **Media Players**: `ao/*` (e.g., `ao/wasapi`, `ao/pipewire`).
  - **Chromium Browsers**: `wasapi_render_thread`, `AudioOutputDevice`, `AudioThread`, `CrAudio`.
  - **Generic Keywords**: `audio`, `playback`, `wasapi`, `asio`.
- Excludes utility/watchdog threads (e.g., `audio.CrUtilityMain`, `watchdog`) to prevent false positives.

### 2. Stage 2: Module Start Address Inspection
- For specialized audio software (DAWs, VST hosts, editors) that do not set thread descriptions, ATI queries the thread start address (`Win32StartAddress`).
- Determines whether the thread was spawned by an audio driver module:
  - ASIO driver DLLs (e.g., `*asiodriver*.dll`, `vbvmaux_asiodriver64.dll`).
  - Windows Audio Core DLLs (`audioses.dll`, `dmusic.dll`, etc.).

### 3. Stage 3: Call Stack Disassembly Scanning & Enhanced Heuristic Sampling
For unknown applications, games, or engines without thread descriptions (Godot, Unity, DirectSound, WinMM), ATI combines advanced static and dynamic inspection techniques:

- **Stack Base Inspection & Call Stack Disassembly Scanning**:
  - **FMOD / Unity**: Scans memory preceding `StackBase` in the thread TEB to extract embedded plaintext thread descriptions (`FMOD (WASAPI) feeder thread`, etc.).
  - **Godot Engine & Unnamed WASAPI**: Scans return addresses on the thread call stack to inspect instructions near `[rip + disp32]` relative references, identifying core engine symbols like `"audio_driver_wasapi"` or `"AudioDriverWASAPI"` directly (instantly isolated as Rank 95 on the first turn).
- **Enhanced 10-Second Heuristics Sampling (Unrestricted Modules / Top-Delta Exclusion)**:
  - Completely eliminates restrictive module whitelists, scanning all threads including external sound modules (`DSOUND.dll`, `WINMM.dll`, `pxtoneWin32.dll`, etc.).
  - Samples all threads over a 10-second observation window (20 turns at 500ms intervals) to accumulate delta CPU times.
  - **Excludes the #1 highest delta thread** (typically the main game logic / rendering loop).
  - **Evaluates candidates ranking #2 to #10**, verifying audio module signatures and steady polling cycles (10ms–15ms range) to reliably isolate true audio threads.
- **Priority Throttling for Unmigratable Co-existing Threads (Graphics Drivers, etc.) & Thread Protection**:
  - After migrating normal threads to the eviction core mask, ATI physically verifies their thread affinities. It detects threads that remain locked to the dedicated audio core due to driver-level design (e.g., NVIDIA graphics driver threads that resist affinity reassignment).
  - Normal threads (main engine, rendering, input) that successfully migrate retain their original OS/application priority completely untouched (preventing unintended priority degradation).
  - When such an unmigratable thread co-exists on the dedicated audio core (Half-Isolated), the audio thread is automatically elevated if set to `Idle (-15)` (`-15` ➔ `-2` Lowest), while the co-existing driver thread is throttled to one tier below (`-15`), maintaining audio thread scheduling priority and preventing dropouts.

### PID/TID Tracking Mechanism for Previously Detected Sessions
- Once an audio playback thread (TID) or process (PID) is identified and isolated, ATI caches and maintains the entry within its active tracking table and INI configuration.
- In addition to enabling near-instant isolation recovery upon subsequent launches, the engine maintains continuous tracking within the active process even after priority modifications (e.g., set to `Idle (-15)`), preventing detection oscillation.

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
