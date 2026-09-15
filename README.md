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

### 3. Stage 3: Thread Priority & Activity Sampling Detection (Fallback)
For unknown applications or games without thread descriptions, ATI identifies audio playback threads based on thread priority and activity patterns. Detection behavior is distinguished by whether candidate threads are single or multiple:

- **When a single candidate thread exists (MMCSS / Real-time priority)**:
  - Audio engines typically promote their playback threads to real-time priority (`THREAD_PRIORITY_TIME_CRITICAL` (+15) or higher) to avoid dropouts.
  - If exactly one real-time thread exists within the target process, ATI immediately isolates it regardless of the Heuristics ON/OFF setting.
- **When multiple candidate threads exist (e.g., Unity game engine / Target of Heuristics)**:
  - Applications built with game engines such as Unity may spawn multiple unnamed threads sharing the same high priority level (e.g., `THREAD_PRIORITY_HIGHEST` (+2) or BasePri 10).
  - **The "Heuristics" checkbox on the UI controls whether detection among these multiple candidate threads is enabled or disabled**:
    - **Enabled (ON)**: Samples CPU execution time of candidate threads at regular intervals, statistically isolating the thread exhibiting continuous, steady audio buffer activity (10ms–15ms periodicity).
    - **Disabled (OFF)**: Skips CPU sampling to maintain an ultra-low-overhead mode (0.00% CPU usage) in Standby state.
- Users can also specify a custom target thread priority (`TargetPriority`) on a per-process basis.

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
