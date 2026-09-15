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
  - Seamlessly minimize to the system notification area (`To Tray`).
  - Supports auto-start on Windows boot via registry integration.

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
- **Intruder Thread Suppression & Normal Thread Priority Protection**:
  - After migrating normal threads to the default core mask, ATI physically re-checks their affinity. Only threads actively persisting on the dedicated audio core (e.g., self-binding NVIDIA display driver threads) are classified as "true intruders".
  - Normal threads (main, render, input) that migrate successfully retain their original OS/game priority completely untouched (preventing unintended `-15` degradation).
  - When an intruder is cohabiting (Half-Isolated), the audio thread is automatically elevated if set to `-15` (`-15` ➔ `-2` Lowest), while the intruder is demoted to the tier directly below (`-15`), maintaining audio priority dominance.

### Persistent Thread Tracking Mechanism
- Once a thread is identified and isolated, ATI registers its Thread ID (TID) in an active tracking table.
- Even if ATI adjusts the thread's priority (e.g., to `Idle (-15)` for energy efficiency or specific tuning), the engine maintains continuous tracking and core isolation throughout the application's lifecycle, preventing detection oscillation.

## Download

Download the pre-compiled standalone binary (`ATI.exe`) from the [Releases](https://github.com/anti-zero-gravity/AudioThreadIsolator/releases) page.

---

## Building and Installation

### Requirements
- **OS**: Windows 10 / Windows 11 (64-bit)
- **Compiler**: MinGW-w64 `g++` (supporting C++17 and UCRT) or MSVC

### Build via PowerShell
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```
The compiled standalone executable `ATI.exe` will be generated in the root directory.

---

## License
MIT License.
