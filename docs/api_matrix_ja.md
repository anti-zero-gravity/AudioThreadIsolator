# Audio Thread Isolator (ATI) - 管理用 API 発行マトリクス一覧

本ドキュメントは、ATI がプロセスの監視・スレッド隔離のために現在発行している Win32 / NT API の全貌、およびアプリケーション全体の動作状態ごとの API 発行有無・頻度・CPU コストをまとめた技術資料である。最新のコード実装（`src/isolator.cpp`, `src/main.cpp`）および設計仕様書（`spec.md`, `spec_ja.md`）に準拠して同期されている。

---

## 1. 管理用 API 一覧と機能概要（現行使用 API）

| API 名 | 対象種別 | 目的・何が得られるか / 何を行うか |
| :--- | :--- | :--- |
| **`CreateToolhelp32Snapshot`** | OS全域 | `TH32CS_SNAPPROCESS`: プロセス一覧を取得（未起動プロセスが存在する場合、または `UnlistedMask` フラグ有効時は INI設定のポーリング時間×10 ごとに 1 回発行。全監視対象が起動中かつ `UnlistedMask=0` 時は 0 回スキップ）。 |
| **`Process32First` / `Next`** | OS全域 | プロセススナップショット取得時のみ各プロセスの EXE 名、PID、スレッド数を列挙取得。全起動中かつ `UnlistedMask=0` 時は 0 回。 |
| **`GetExitCodeProcess`** | プロセス | 対象 PID の死活確認（プロセス終了判定）。数ナノ秒で完了し SNAPPROCESS を代替。 |
| **`OpenProcess`** | プロセス | 対象プロセスの操作ハンドル（情報取得・アフィニティ変更・優先度変更・HighQoS 設定用）をオープン。 |
| **`CloseHandle`** | 共通 | オープンしたプロセスハンドル、スレッドハンドル、スナップショットハンドルをクローズ解放。 |
| **`SetProcessInformation`** | プロセス | **【HighQoS 保証】** 新規 PID 検出時の初回 1 回のみ `ProcessPowerThrottling` (4) を設定し、Power Throttling（EcoQoS）を解除。実行速度制限の解除および高精度タイマー（1ms）を維持。親プロセスの HighQoS はカーネルの自動継承により配下全スレッドへ波及し、以後のコア変更時も維持される。 |
| **`SetPriorityClass`** | プロセス | 対象プロセスの優先度クラス（Realtime, High, AboveNormal, Normal, BelowNormal, Idle）を設定（非管理者時はピッカー上で Realtime 選択不可）。 |
| **`GetProcessAffinityMask`** | プロセス | 対象プロセスの現在の実行許可コアマスクを取得。 |
| **`SetProcessAffinityMask`** | プロセス | 対象プロセスの実行許可コアマスクを拡張（通常コア ＋ オーディオコア）または通常コアへ制限。さらに Unlisted（未登録）プロセスへの専用コア除外マスク一括/差分適用、および Unmask All による全コア復元に使用。 |
| **`NtGetNextThread`** | スレッド | 対象 PID のプロセスハンドルから、属するスレッドハンドルをカーネル直結で順次取得。OS 全域走査を行わず特定 PID のみ直接列挙。 |
| **`OpenThread`** | スレッド | 対象スレッドの操作ハンドル（追加権限の取得・サスペンド制御用）をオープン。 |
| **`QueryThreadCycleTime`** | スレッド | スレッドが消費した CPU サイクル数の累積値を取得。毎ポーリング周期の Cycles Delta 算出、Chromium 初回登録時の親呼出制御スレッド（Cycles 降順 1 位）特定、定常監視、およびサンプリング（k=1..3）判定に使用。 |
| **`GetThreadPriority`** | スレッド | スレッドの現在の相対優先度値（`-15` 〜 `+15`）を取得。 |
| **`SetThreadPriority`** | スレッド | オーディオスレッドの優先度を指定値へ設定、またはマルチスロット個別優先度（`audioPriorities[s]`）を適用。同居通常スレッドの優先度引き下げや侵入スレッド制圧にも使用。 |
| **`SetThreadAffinityMask`** | スレッド | スレッドの実行コアを単一のオーディオ専用コア、または通常コア群へ固定・退避。差分検知により変更時のみ発行（退避済みは 0 回スキップ）。 |
| **`SetThreadIdealProcessor`** | スレッド | オーディオスレッドに対し指定オーディオコアを最優先実行コアとして指定するとともに、通常スレッド群に対しても通常コア群へラウンドロビン分散割り当てを行い、スレッドの偏りを防止。 |
| **`SuspendThread` / `ResumeThread`** | スレッド | テーブル Col 4 の一時停止（`❚❚`）トグルおよび個別スロットのサスペンド制御。オーディオスレッドの実行を一時中断（ミュート/退避）または再開。 |
| **`NtQuerySystemInformation`** | OS/プロセス | `SystemProcessInformation` (5) を取得し、各スレッドの `WaitReason`（`WaitReason == 4` DelayExecution）および `audiodg.exe` 音声本体スレッド選定用の累計 `ContextSwitches`・`Priority` を取得。`RAATServer.exe` 未特定時は共通 3 秒ゲートで 3 秒に 1 回のみ発行（確定後 0 回）、`audiodg.exe` 実優先度取得時は確定・変更直後＋3 秒後の計 2 回のみ発行（定常 0 回）。 |
| **`NtQueryInformationThread`** | スレッド | `ThreadBasicInformation` により、`NtGetNextThread` 列挙時の `TID`・`AffinityMask` 1 パス同時取得、実測 `Dynamic Priority` の 2 段階限定取得（確定・変更直後＋数秒後の計 2 回、定常 0 回）、および未特定スレッドのスタック走査用 TEB 基底取得に使用。 |
| **`ReadProcessMemory`** | プロセス | スレッドのコールスタック（最大 32KB）を一括読み出し、オーディオシグネチャ（`audioses.dll`, `roonmedia.dll`, `ws2_32.dll` 等）を走査。Type 1（一般アプリ）、AppType: 4（RoonAppliance）、AppType: 5（Voicemeeter）の未特定スレッドに対し生涯1回のみ使用（Chromium および確定後は 0 回スキップ）。 |

---

## 2. 状態の組み合わせ別 API 発行マトリクス（毎秒ポーリング時の挙動）

### 状態の定義
1. **未起動**: 対象プロセスが起動していない状態（`Not running`）
2. **ペンディング中**: Col 0 のチェックボックスが ON の状態（`Bypassed`）
3. **Chromium 待機**: AudioService プロセスが未出現・未特定（`sleeping...`）
4. **Chromium 定常監視 (`Standby`)**: AudioService は特定済みだが音声未再生（flag=0 定常監視中）
5. **Chromium サンプリング中 (`Searching...`)**: Delta が閾値（3M）を超え、ポーリング/4 間隔で上位選定中
6. **Chromium 隔離中 (`WASAPI`)**: 音声再生中・オーディオスレッド確定隔離中（`flag=2`）
7. **非Chromium 探索中 (`Scanning...`)**: 通常アプリでオーディオスレッドを探索中（10秒カウントダウン含む）
8. **非Chromium 隔離中 (`Isolated`)**: 通常アプリでオーディオスレッド特定・隔離済み（`Fully / Half-Isolated`）

---

### 【全体共通処理（プロセス監視・死活確認・スレッド列挙段階）】

| API 名 | 未起動あり | 全起動中 (ペンディング含む) | Chromium待機 (`Standby`) | Chromiumサンプリング (`Searching`) | Chromium再生 (`WASAPI`) | 非Chromium探索 (`Scanning`) | 非Chromium隔離 (`Isolated`) | 単発コスト |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`CreateToolhelp32Snapshot` (Process)** | INIポーリング時間×10毎に1回 | **0 回 (スキップ)** *(UnlistedMask有効時はINI×10毎に1回)* | **0 回 (スキップ)** *(UnlistedMask有効時はINI×10毎に1回)* | **0 回 (スキップ)** *(UnlistedMask有効時はINI×10毎に1回)* | **0 回 (スキップ)** *(UnlistedMask有効時はINI×10毎に1回)* | **0 回 (スキップ)** *(UnlistedMask有効時はINI×10毎に1回)* | **0 回 (スキップ)** *(UnlistedMask有効時はINI×10毎に1回)* | 極小 |
| **`Process32First` / `Next`** | スナップ時のみ | **0 回 (スキップ)** *(スナップ時のみ)* | **0 回 (スキップ)** *(スナップ時のみ)* | **0 回 (スキップ)** *(スナップ時のみ)* | **0 回 (スキップ)** *(スナップ時のみ)* | **0 回 (スキップ)** *(スナップ時のみ)* | **0 回 (スキップ)** *(スナップ時のみ)* | 極小 |
| **`GetProcessAffinityMask` (事前照合)** | 新規Unlisted PID分のみ (1ターン最大2個) | 0 回 *(スナップ時は新規PID分のみ)* | 0 回 *(スナップ時は新規PID分のみ)* | 0 回 *(スナップ時は新規PID分のみ)* | 0 回 *(スナップ時は新規PID分のみ)* | 0 回 *(スナップ時は新規PID分のみ)* | 0 回 *(スナップ時は新規PID分のみ)* | 極小 |
| **`SetProcessAffinityMask` (Unlisted退避)** | マスク不一致時のみ (1ターン最大2個) | 0 回 *(不一致時のみ)* | 0 回 *(不一致時のみ)* | 0 回 *(不一致時のみ)* | 0 回 *(不一致時のみ)* | 0 回 *(不一致時のみ)* | 0 回 *(不一致時のみ)* | 小 |
| **`GetExitCodeProcess` (死活確認)** | 0 回 | 毎秒 登録プロセス数分 | 毎秒 登録プロセス数分 | 毎秒 登録プロセス数分 | 毎秒 登録プロセス数分 | 毎秒 登録プロセス数分 | 毎秒 登録プロセス数分 | 極微 (数ns) |
| **`SetProcessInformation` (HighQoS)** | 0 回 | 0 回 (Bypass時) | **新規PID検出時 1 回のみ** (定常 0 回) | **0 回** | **0 回** | **新規PID検出時 1 回のみ** (定常 0 回) | **0 回** | 極小 |
| **`NtGetNextThread`** | 0 回 | 0 回 (Bypass時) | 毎秒 AudioService分 (約14本) | 毎秒 AudioService分 (約14本) | 毎秒 AudioService分 (約14本) | 毎ターン 対象PID分 (`RAATServer`は3秒に1回) | **3秒に1回** 対象PID分 (`audiodg`は毎ターン) | 極小 (カーネル直結) |

---

### 【個別プロセス・スレッド制御段階（各プロセスの状態ごとの発行内容）】

| API 名 | 未起動 | ペンディング | Chromium待機 (`Standby`) | Chromiumサンプリング (`Searching`) | Chromium再生 (`WASAPI`) | 非Chromium探索 (`Scanning`) | 非Chromium隔離 (`Isolated`) | 単発コスト |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`OpenProcess`** | 0 回 | 0 回 | 毎秒 1 回 (AudioService) | 毎秒 1 回 (AudioService) | 毎秒 1 回 (AudioService) | 毎秒 各PID分 | 毎秒 各PID分 | 極小 |
| **`GetProcessAffinityMask`** | 0 回 | 0 回 | 毎秒 1 回 | 毎秒 1 回 | 毎秒 1 回 | 毎秒 各PID分 | 毎秒 各PID分 | 極小 |
| **`SetProcessAffinityMask`** | 0 回 | 0 回 | 親マスク拡張時のみ | 親マスク拡張時のみ | 親マスク拡張時のみ | 新規出現PID時のみ | 新規出現PID時のみ | 小 |
| **`SetPriorityClass`** | 0 回 | 0 回 | 設定変更時のみ | 設定変更時のみ | 確定時 / 変更時 | 設定変更時のみ | 確定時 / 変更時 | 小 |
| **`OpenThread`** | 0 回 | 0 回 | 初回親特定時 / 変更時 | サンプリング対象分 (10〜14回) | 確定・変更時のみ | 毎秒 全スレッド分 (20〜40回) | 定常時は確定オーディオ分のみ (1〜2回) / 差分時のみ全スレッド | 極小 |
| **`QueryThreadCycleTime`** | 0 回 | 0 回 | 毎秒 flag=0 分 (約10〜14回) | ポーリング/4 ごと 3回計測 | 毎秒 flag=0/2 分 (約10〜14回) | 毎秒 全スレッド分 (Delta算出) | 定常時は確定オーディオ分のみ (1〜2回) / 差分時のみ全スレッド | 極小 |
| **`GetThreadPriority`** | 0 回 | 0 回 | 0 回 | 0 回 | 確定オーディオ分のみ 毎秒 1 回 (差分照合) | 毎秒 全スレッド分 | 定常時は確定オーディオ分のみ (1〜2回) / 差分時のみ全スレッド | 極小 |
| **`SetThreadPriority`** | 0 回 | 0 回 | 0 回 | 0 回 | 確定・変更時のみ 1回 | 確定時のみ | 毎秒 1 回 (オーディオ維持/個別) | 小 |
| **`SetThreadAffinityMask` (オーディオコア)** | 0 回 | 0 回 | 0 回 | 0 回 | 確定・変更時のみ 1回 | 確定時のみ | 毎秒 1 回 (専有コア維持/個別) | 小 |
| **`SetThreadAffinityMask` (通常コア群退避)** | 0 回 | 0 回 | 初回親特定時 (flag=1退避) | 0 回 | flag=0 退避時のみ | 毎秒 未退避スレッド分 | **差分時のみ (退避済みは0回)** | 小 |
| **`SetThreadIdealProcessor`** | 0 回 | 0 回 | 0 回 | 0 回 | 確定・変更時のみ 1回 | 差分時のみ | **差分時のみ (オーディオ維持＋通常コア分散)** | 小 |
| **`SuspendThread` / `ResumeThread`** | 0 回 | 0 回 | 0 回 | 0 回 | 0 回 | 0 回 | ユーザーの Col 4 操作時のみ | 小 |
| **`NtQuerySystemInformation`** (WaitReason / CtxSw) | 0 回 | 0 回 | 0 回 (対象外) | 0 回 (対象外) | 0 回 (対象外) | RAATServer探索時のみ6ターンに1回 (5回スキップ) | `audiodg`の実prio取得2回のみ (定常0回) | 中 |
| **`NtQueryInformationThread`** | 0 回 | 0 回 | 0 回 (対象外) | 0 回 (対象外) | 実prio取得 2 回のみ (確定時+3秒後、定常0回) | Type 1 未特定スレッドのみ | 3秒に1回 TID+Affinity照合 / 実prio取得は2回のみ (定常0回) | 小 |
| **`ReadProcessMemory`** (最大32KBスタック走査) | 0 回 | 0 回 | 0 回 (対象外) | 0 回 (対象外) | 0 回 (対象外) | Type 1 / AppType: 4, 5 未特定スレッドのみ (生涯1回) | 0 回 (スキップ) | 小 |
| **`CloseHandle`** | 0 回 | 0 回 | 対象スレッドオープン分 | 対象スレッドオープン分 | 対象スレッドオープン分 | 毎秒 20〜45 回 | 定常時は 2〜5 回 / 差分時のみ 20〜45 回 | 極小 |

---

## 3. 負荷削減・低オーバーヘッド設計仕様

1. **HighQoS（EcoQoS 解除）の新規 PID 初回 1 回適用と親継承**:
   - 登録プロセス（全 `AppType: 0〜5`）において、新規 PID を検出した初回 1 回のみ `SetProcessInformation`（`ControlMask = 1 | 4`, `StateMask = 0`）をプロセス単位で発行し、定常稼働中の再発行は 0 回とする。
   - Windows カーネルの `EPROCESS` 保持および親継承機構を活用し、コアマスク変更後も再発行なしで配下の全スレッド（既存および新規生成）の EcoQoS 解除と高精度タイマー（1ms）を維持する。
   - Chromium 系アプリにおいては、音声を司る Audio Service 子プロセス検出時に初回 1 回のみ発行する。
2. **`NtGetNextThread` による対象プロセス直接スレッド列挙**:
   - 監視対象 PID のプロセスハンドルからローレベル API `NtGetNextThread` を直接発行し、当該プロセスに属するスレッドハンドルのみを順次取得。
   - 走査対象を監視対象プロセスのスレッド（14〜数十本）のみに限定し、最小限のカーネル負荷でスレッド情報を取得する。
3. **Chromium (Type 2) の Cycles Delta サンプリング判定**:
   - 初回登録時に総サイクル数降順 1 位スレッドを親呼出制御スレッドとして特定（Excluded CPU# 退避）。
   - 以後、定常監視において Delta > 3M/s 到達時にポーリング/4（125ms）×3 回サンプリングを行い、最新 4 計測 3 Delta 合計最大スレッドをオーディオスレッドとして選定・隔離する。
4. **通常スレッドへのアフィニティ差分発行制御**:
   - すでに通常コア群へ退避済みのスレッドに対しては、`currentAffinity != effectiveNormal` の差分検知時のみ `SetThreadAffinityMask` を発行し、アフィニティが一致している場合はシステムコールをスキップする。
5. **オーディオスレッド専有および通常スレッドへの Ideal Processor 分散指定**:
   - オーディオ確定スレッドに対しては指定オーディオコアを最優先実行コア（理想プロセッサ）として固定指定する。
   - 通常スレッド群に対しても、通常コア群（`normalMask`）の範囲内で Cycles Delta 降順に基づくラウンドロビン割り当て（`SetThreadIdealProcessor`）を行い、特定コアへのスレッド集中を防ぎつつ差分時のみ発行する。
6. **プロセス死活確認の高速化とスナップショットの動的スキップ**:
   - 監視対象プロセスが全件稼働中はプロセススナップショット取得をスキップ（0 回）。
   - 既存プロセスの死活確認は `GetExitCodeProcess`（数ナノ秒）で高速に行い、未起動プロセスが存在する場合のみ INI設定のポーリング時間×10 の低頻度でスナップショットを発行する。
7. **共通 3 秒定常ゲート（`AppType: 1, 3, 4, 5` および `RAATServer` 未特定待機）**:
   - `audiodg.exe`（`AppType: 0`）と Chromium（`AppType: 2`）を除く定常隔離済みプロセス、および `Delta` 不要で 1 ショット特定を行う `RAATServer.exe` の未特定待機中は、共通 3 秒ゲート（500ms 設定時は 6 ターン中 5 ターンスキップ）により `NtGetNextThread`・`OpenProcess`・`OpenThread`・`NtQuerySystemInformation` のカーネル API 発行をスキップターンで 0 回にする（プロセス死活確認のみ毎ターン維持）。
   - 実行ターンでは `NtGetNextThread` ループ内で `NtQueryInformationThread(ThreadBasicInformation)` を 1 回発行し、スレッド `TID` と `AffinityMask` を同時に取得することで、`AppType: 1` におけるオーディオコア同居スレッド（`Half-Isolated`）判定を余分なハンドル再オープンなしで行う。
8. **実効優先度（`Dynamic Priority`）の 2 段階限定取得と `audiodg.exe` 音声本体スレッド選定**:
   - 全 `AppType: 0〜5` において、UI 表示用の実測 `Dynamic Priority` 取得は `ShouldFetchDynamicPriority`（`GetTickCount64()` 管理）により、**スレッド確定・優先度変更直後の 1 回目**と、OS の一時的な優先度ブースト減衰を反映するための **`+3000ms` 以降最初の走査ターン（`AppType: 0, 2` は約 3.0 秒後、3 秒定常ゲート対象の `AppType: 1, 3, 4, 5` は約 3.0〜4.5 秒後）の 2 回目**の計 2 回のみで終了し、以後の定常稼働中は 0 回に停止する。
   - PPL 保護により `THREAD_QUERY_INFORMATION` が制限される `audiodg.exe`（`AppType: 0`）では、上記 2 回の取得タイミングにのみ `WalkSystemProcessThreads` で累計 `ContextSwitches` 最上位の音声本体スレッドの `Priority` を参照し、待機ワーカー（`WrQueue`）に残存する ALPC ブースト値の誤表示を防ぐ。
