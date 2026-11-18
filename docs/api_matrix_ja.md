# Audio Thread Isolator (ATI) - 管理用 API 発行マトリクス一覧

本ドキュメントは、ATI がプロセスの監視・スレッド特定・コア隔離・優先度制御のために発行している全 Win32 / NT Native API の役割・実行コスト粒度、
およびポーリング周期（`INI×10`, `INI×6`, `INI×3`, `INI×1`, `INI/4`, 単発イベント）× `AppType (0〜5)` 別の発行タイミングをまとめた技術資料である。

---

## 1. 全使用 API 一覧・役割・実行コスト粒度

### コスト粒度の分類
- **`OS一括 (1回)`**: 1 回の呼び出しで OS 全域のプロセス・スレッド情報を一括ダンプ取得し、その中から対象 PID の情報を抽出することでスレッド数分のループ発行コストを払わないもの。
- **`PID単位 (1回)`**: 対象プロセス 1 個につき 1 回だけ発行するもの。
- **`確定TID (1〜2回)`**: プロセス内の全スレッドではなく、特定済みのオーディオスレッド（単一スロット 1 本、Voicemeeter 最大 2 本）のみに発行するもの。
- **`全TID (N回)`**: 対象プロセス内の生存スレッド数 `N` 本（例: 15〜50 本）の回数だけループ発行コストを払うもの。

### 1.1 OS 全域・プロセス単位 API（14種）

| API 名 | 対象 | コスト粒度 | 負荷 | 主な役割 |
| :--- | :---: | :---: | :---: | :--- |
| `CreateToolhelp32Snapshot` | OS全域 | OS一括 (1回) | 小 | 全プロセス一覧スナップショット作成 |
| `Process32First / Next` | OS全域 | OS一括 (走査) | 極小 | スナップショットから EXE名・PID 列挙 |
| `NtQuerySystemInformation` | OS全域 | OS一括 (1回) | 中 | 全TIDの `WaitReason`・`CtxSw` 一括取得 |
| `GetExitCodeProcess` | PID | PID単位 (1回) | 極微 | 稼働 PID の死活確認（数ns） |
| `OpenProcess` | PID | PID単位 (1回) | 極小 | 対象 PID の操作ハンドル取得 |
| `CloseHandle` | 共通 | ハンドル単位 | 極小 | プロセス・スレッドハンドルの解放 |
| `SetProcessInformation` | PID | 初回 1回のみ | 極小 | HighQoS 適用（EcoQoS 解除・子継承） |
| `GetProcessAffinityMask` | PID | PID単位 (1回) | 極小 | プロセス実行許可コアマスクの取得 |
| `SetProcessAffinityMask` | PID | 不一致時 (1回) | 小 | プロセス実行許可コアマスクの変更 |
| `GetPriorityClass` | PID | PID単位 (1回) | 極小 | プロセス優先度クラスの実測取得 |
| `SetPriorityClass` | PID | 不一致時 (1回) | 小 | プロセス優先度クラスの設定 |
| `QueryFullProcessImageNameA` | PID | 初回 1回のみ | 小 | EXEパス取得（Chromium 初回判定用） |
| `NtQueryInformationProcess` | 子PID | 未特定時のみ | 小 | PEB 取得（Chromium 子プロセス特定用） |
| `NtSuspendProcess / Resume` | PID | 手動時 (1回) | 小 | `audiodg.exe` プロセス全体の一時停止 |

#### 実行コスト・動作備考（1.1）
- **`CreateToolhelp32Snapshot` + `Process32First / Next`**: 全監視ルールが起動中（`Type 2` の `audioServicePid != 0` を含む）かつ `UnlistedMask=0` かつ強制再スキャン要求なし時は **0回（スキップ）**。`PID <= 4` は除外し、登録済 PID の死亡検出やルール変更・手動復帰時は次ターン即再取得する。
- **`NtQuerySystemInformation` (`SystemProcessInformation = 5`)**: カーネルの `WaitReason`・`ContextSwitches` 取得や PPL 制限下の代替列挙のため **OS 全域の全プロセス・全スレッド情報** を 1 回でダンプ取得し、対象 PID のスレッド情報を一括抽出する。通常環境では 1・2 のみに限定し（`Type 1, 2, 3, 5` と `RoonAppliance.exe` では **生涯 0回**）、PPL 制限環境のみ 3 が発火する：
  1. **`RAATServer.exe`（`Type 4`）**: オーディオスレッド未特定時のみ 3.0 秒に 1 回（`INI × 6`）呼び出して `WaitReason == 4` を探索し、特定・隔離後は **0回**。
  2. **`audiodg.exe`（`Type 0`）`Dyn Pri` 取得**: 初回検出時・優先度変更時の **0秒後＋約3秒後（計2回）** のみ `ContextSwitches` 最上位スレッドの `Dyn Pri` を取得（定常 **0回**）。
  3. **PPL 保護プロセスでのスレッド列挙フォールバック**: `OpenProcess(PROCESS_QUERY_INFORMATION)` が拒否されて `NtGetNextThread` で列挙できない対象（PPL 保護下の `audiodg.exe` 等）のみ、`WalkSystemProcessThreads` で全 TID を取得する。
- **`GetExitCodeProcess`**: カーネル `EPROCESS` の終了コード参照のみで数ナノ秒で完了し、毎ターンのスナップショットを代替する。対象は **`m_config.rules` に登録された稼働 `PID > 4` のみ**（数個）に限定し、未登録アプリやシステムプロセスの終了で不要な `SNAPPROCESS` を誘発しない。
- **`SetProcessInformation` (`ProcessPowerThrottling = 4`)**: 新規 PID 検出時の初回 1 回のみ発行（定常 **0回**）。
 カーネルの自動継承機構により、親プロセスの HighQoS（1ms タイマー維持・速度制限解除）が配下の全スレッドへ恒久波及する。
- **`SetProcessAffinityMask` / `SetPriorityClass`**: 事前に `Get` で現在値と照合し、不一致時のみ発行する（例外として `Type 2` の非音声子 PID 初回退避のみ `Get` なしで `SetProcessAffinityMask` を 1 回発行）。
 Col 3 が非アタッチ（淡緑背景）の時は `SetPriorityClass` は **0回**。

---

### 1.2 スレッド列挙・探索・計測系 API（10種）

| API 名 | 対象 | コスト粒度 | 負荷 | 主な役割 |
| :--- | :---: | :---: | :---: | :--- |
| `NtGetNextThread` | TID | 全TID (N回) | 極小 | 対象 PID 内の全スレッドハンドルを順次取得 |
| `NtQueryInformationThread` | TID | 全TID / 確定TID | 極小 | `TID`・`Affinity`・`DynPri`・`TEB` 取得 |
| `GetThreadId` | TID | 条件時のみ | 極小 | `NtQuery`失敗・解除時の`TID`取得 |
| `OpenThread` | TID | 探索N回 / 定常1〜2回 | 極小 | スレッド個別の操作・計測ハンドル取得 |
| `GetExitCodeThread` | TID | 解除時のみ | 極小 | 解除・終了処理時の生存確認 |
| `QueryThreadCycleTime` | TID | 探索N回 / 定常0回 | 極小 | 累積 CPU サイクル取得（Delta 算出） |
| `IsWow64Process` | PID | 生涯 1回のみ | 極小 | スタック走査時の 32/64bit TEB 構造判定 |
| `ReadProcessMemory` | メモリ | 生涯 1回のみ | 小 | コールスタック最大 32KB の一括読み出し |
| `VirtualQueryEx` | メモリ | 生涯 1回のみ | 小 | スタック上の戻りアドレスの実行属性判定 |
| `GetMappedFileNameA` | メモリ | 生涯 1回のみ | 小 | 戻りアドレスが属する DLL ファイル名特定 |

#### 実行コスト・動作備考（1.2）
- **`NtGetNextThread` + `NtQueryInformationThread` (`ThreadBasicInformation`)**: OS 全域走査を行わず、
 対象 PID 内のスレッド `N` 本のみ直接列挙し、ループ内で `TID` と `AffinityMask` を 1 パス同時回収する（`ApplyAudioThreadSettings` / `EvacuateNormalThreadSettings` へ `knownAffinity` を引き渡して再発行を抑制し、`Type 2` の未使用 `TEB` 取得も削除済）。取得失敗や解除確認時は `GetThreadId` にフォールバックする。
- **`OpenThread` の定常削減**: 非 Chromium（`Type 1, 3, 4, 5`）の隔離確定後および `Type 2` 定常時は、通常スレッド（`Type 2` の `flag=1` と `alreadyEvacuated` 一致スレッドを含む）のオープンをスキップし、確定オーディオスレッド（1〜2本）や未確定候補のみ開く。
- **32KB コールスタック走査（`ReadProcessMemory` 他）**: `Cycles Delta` が閾値を超えた未検査スレッド（`inspectedTids` 未登録）に対し、
 **生涯 1 回のみ** 実行してオーディオ DLL（`audioses.dll`, `asio`, `roonmedia.dll`, `ws2_32.dll` 等）を特定する。隔離確定後および `Type 0, 2`・`RAATServer` では **0回**。

---

### 1.3 スレッド制御系 API（7種）

| API 名 | 対象 | コスト粒度 | 負荷 | 主な役割 |
| :--- | :---: | :---: | :---: | :--- |
| `GetThreadPriority` | TID | 条件時 (0〜2回) | 極小 | Win32 相対優先度（`-15`〜`+15`）取得 |
| `SetThreadPriority` | TID | 不一致時のみ | 小 | Win32 相対優先度の設定 |
| `NtSetInformationThread` | TID | 不一致時のみ | 小 | `Fixed 1〜31` 絶対優先度 (`KPRIORITY`) 設定 |
| `SetThreadAffinityMask` | TID | 未適用TIDのみ | 小 | オーディオコア固定・通常スレッド退避 |
| `SetThreadIdealProcessor` | TID | 未適用TIDのみ | 小 | 最優先コア指定・通常コアへの分散配置 |
| `SuspendThread` | TID | 手動時 (1回) | 小 | Col 4 (`❚❚`) オーディオスレッド一時停止 |
| `ResumeThread` | TID | 手動時 (1回) | 小 | Col 4 (`❚❚`) オーディオスレッド再開 |

#### 実行コスト・動作備考（1.3）
- **非アタッチ時の優先度 API スキップ**: Col 2 が非アタッチ（淡緑背景：`audioPriority == 0`）かつ新規/確認イベント対象外の定常時は、
 `GetThreadPriority` / `SetThreadPriority` / `NtSetInformationThread` はすべて **0回**。
- **`GetThreadPriority` 一括取得の限定**: 通常スレッド列挙時の `GetThreadPriority` 一括取得は未特定探索中の `AppType: 3` (DAW) と `Type 5` (Voicemeeter) のみに限定し、定常隔離中・一般アプリ・`RAATServer` 探索中は **0回**（侵略スレッド抑制・`Dyn Pri` 確認時のみ個別取得）。
- **アフィニティ・Ideal CPU の差分発行**: 適用済みキャッシュ（`m_appliedThreads` / `m_appliedIdealCores`）と実マスク照合により、
 退避・隔離完了後の定常ターンでは `SetThreadAffinityMask` / `SetThreadIdealProcessor` ともに **0回**。

---

## 2. ポーリング周期（5段階＋単発）× `AppType (0〜5)` 早見表

### `AppType (0〜5)` の定義
- **`Type 0`**: `audiodg.exe`（Windows オーディオエンジン専用）
- **`Type 1`**: 一般非 Chromium アプリ（音楽プレイヤー、ゲーム等）
- **`Type 2`**: Chromium 系アプリ（Chrome, Edge, Brave, Vivaldi 等）
- **`Type 3`**: DAW 型高負荷アプリ（Nuendo, Cubase 等 / `TimeCritical` スレッド 2 本以上）
- **`Type 4`**: Roon 専用（`RoonAppliance.exe` / `RAATServer.exe`）
- **`Type 5`**: Voicemeeter 全エディション（2スロット：Slot 0 `VBAN` / Slot 1 `VAIO/AUX & DSP`）

### ポーリング周期別の稼働対象一覧（標準 `INI = 500ms` 基準）

| 周期区分 | 間隔 (`500ms`時) | 対象 AppType | 動作するモード・処理内容 |
| :--- | :---: | :--- | :--- |
| `INI × 10` | 5.0 秒毎 | 全 Type | 未起動アプリ存在時のプロセス発見 |
| `INI × 10` | 5.0 秒毎 | Unlisted | 新規出現 PID の退避（1ターン最大1個） |
| `INI × 10` | 5.0 秒毎 | `Type 2` (Chromium) | `AudioService` 子プロセスの探索 |
| `INI × 6` | 3.0 秒毎 | `Type 1, 3, 4, 5` | 隔離確定後の定常監視（5ターン休止） |
| `INI × 6` | 3.0 秒毎 | `Type 1, 3, 5` | `Standby 3..1` 探索（3回連続未検出で休眠） |
| `INI × 6` | 3.0 秒毎 | `Type 4` (`RAATServer`) | 未特定時の `WaitReason == 4` 探索 |
| `INI × 3` | 1.5 秒間 | `Type 1, 3, 4, 5` | スレッド特定直後 3 ターンの即応確認 |
| `INI × 1` | 500ms 毎 | 全 Type 共通 | 稼働 PID の死活確認 (`GetExitCode`) |
| `INI × 1` | 500ms 毎 | `Type 0` (`audiodg`) | 常時監視（新規生成 TID の即時捕捉） |
| `INI × 1` | 500ms 毎 | `Type 2` (Chromium) | `Step 2` 無音待機・再生維持 |
| `INI × 1` | 500ms 毎 | `Type 1, 3, 4, 5` | 初回 2 秒 `Searching...` 探索（`Type 4` 常時） |
| `INI / 4` | 125ms 毎 | `Type 2` (Chromium) | `Step 3` 高速サンプリング（3回計測） |
| 単発 (1回) | イベント時 | 全 Type | 初回 `HighQoS` / ピッカー / `❚❚` 停止 |
| 単発 (2回) | 0秒 + 約3秒後 | 全 Type | 実測 `Dyn Pri` の 2 段階取得 |

- **スレッド走査が停止する（0回）状態**:
  - 未起動（`Not running`）、監視除外（`Bypassed`）
  - `Type 1, 3, 5` の休眠中（`sleeping...`：ユーザー操作までプロセス死活以外の API 0回。`Type 5` で 1 枠確定＋他枠休眠時は確定 1 本のみ 3 秒監視）
  - `Type 1, 3, 5` の `Standby 3..1`（3秒ターン間）における待機スキップターン（6ターン中 5ターン）
  - `Type 1, 3, 4, 5` の 3 秒定常ゲートにおけるスキップターン（6ターン中 5ターン）
  - `Type 2`（Chromium）の `AudioService` 未特定中（`sleeping...`、`INI × 10` の PEB 探索時のみ）

---

## 3. ポーリング周期別の発行 API 詳細マトリクス

**【表内の記号】**
- **`● 1回`** / **`● N回`**: その周期ごとに毎回発行（`1回`＝PIDまたは確定TID、`N回`＝PID内の全TID数）
- **`▲ 差分`** / **`▲ 条件`**: 条件成立時（マスク不一致、優先度アタッチ中、新規TID、閾値超過時）のみ発行。**定常時は 0回**
- **`-`**: 発行しない（**0回**）

---

### 3.1 【`INI × 10` 周期】（5.0 秒毎：プロセス発見・Unlisted 退避・Chromium 子プロセス探索）

| 発行 API | 未起動あり | Unlisted ON | `Type 2` 子PID未特定 |
| :--- | :---: | :---: | :---: |
| `CreateToolhelp32Snapshot` | ● 1回 | ● 1回 | ● 1回 |
| `Process32First / Next` | ● 全PID | ● 全PID | ● 全PID |
| `OpenProcess` | - | ▲ 最大1個 | ● 子PID分 |
| `GetProcessAffinityMask` | - | ▲ 最大1個 | - |
| `SetProcessAffinityMask` | - | ▲ 不一致時 | ▲ 非音声子PID初回 |
| `NtQueryInformationProcess` | - | - | ● 子PID分 |
| `ReadProcessMemory` (PEB) | - | - | ● 子PID分 |
| `CloseHandle` | ● 1回 | ▲ 最大1個 | ● 子PID分 |

- 全登録アプリが起動済み（`Type 2` の `audioServicePid != 0` 含む）かつ `UnlistedMask == 0` かつ強制再スキャン要求なしの場合、上記 API はすべて **0回**。`PID <= 4`（System Idle / System）は常時除外する。
- 初回スキャン未了時・ルール変更/追加時・手動解除復帰直後・登録済 PID 死亡検出時は `INI × 10` を待たず次ターン即再スキャンする。
- `UnlistedMask` ON 時は、新規 PID が複数あっても **1ターン最大 1 プロセス** ずつ時間分散して退避する。
- `Type 2`（Chromium）の初回探索（`!chromiumScanAttempted`）は `INI × 10` を待たず検出直後に即時 PEB 走査を 1 回実行し、非音声子プロセス初回退避は `Get` なしで `SetProcessAffinityMask` を 1 回発行する。`audio.mojom.AudioService` が特定された時点で PEB 走査は停止（以後 **0回**）する。

---

### 3.2 【`INI × 6` 周期】（3.0 秒毎：`Type 1, 3, 4, 5` 定常隔離中 ＆ `RAATServer` 探索中）

| 発行 API | `Type 1` 定常 | `Type 3, 4` 定常 | `Type 5` 定常 | `RAAT` 未特定 |
| :--- | :---: | :---: | :---: | :---: |
| `NtGetNextThread` | ● N回 | ● N回 | ● N回 | ● N回 |
| `NtQueryInformationThread` | ● N回 | ● N回 | ● N回 | ● N回 |
| `NtQuerySystemInformation` | - | - | - | ● 1回 (一括) |
| `OpenProcess` | ● 2回 | ● 2回 | ● 2回 | ● 2回 |
| `GetPriorityClass` | ● 1回 | ● 1回 | ● 1回 | - |
| `SetPriorityClass` | ▲ 差分 | ▲ 差分 | ▲ 差分 | - |
| `OpenThread` (確定TIDのみ) | ● 1回 | ● 1回 | ● 1〜2回 | ● N回 |
| `OpenThread` (通常スレッド) | ▲ 増減/同居時 | ▲ 増減時のみ | ▲ 増減時のみ | *(上段に含む)* |
| `QueryThreadCycleTime` | ▲ 新規退避時 | ▲ 新規退避時 | ▲ 新規退避時 | - |
| `SetThreadAffinityMask` | ▲ 差分 | ▲ 差分 | ▲ 差分 | ▲ 特定時 |
| `SetThreadIdealProcessor` | ▲ 差分 | ▲ 差分 | ▲ 差分 | ▲ 特定時 |
| `GetThreadPriority` | ▲ アタッチ/確認 | ▲ アタッチ/確認 | ▲ アタッチ/確認 | ▲ 特定/抑制時 |
| `SetThreadPriority` | ▲ アタッチ差分 | ▲ アタッチ差分 | ▲ アタッチ差分 | ▲ 特定/抑制時 |
| `NtSetInformationThread` | ▲ Fixed差分 | ▲ Fixed差分 | ▲ Fixed差分 | ▲ 特定時 |
| `CloseHandle` | ● N+3回〜 | ● N+3回〜 | ● N+3〜4回 | ● 2N+2回 |

- **6ターン中5ターンは休止**: スキップされる 5 ターンは `GetExitCodeProcess`（登録済みルール PID の死活確認）のみとなり、上表の API はすべて **0回**（`Type 1, 3, 5` の `Standby 3..1` 待機ターンも同様）。
- **`OpenProcess`・`CloseHandle` の実数**: 隔離サイクル 1 回あたり `OpenProcess` は `2回`（スレッド列挙用 1回＋隔離適用用 1回）、`CloseHandle` は `NtGetNextThread` が返す `N` 本分のハンドル解放を含めて **`N+3回〜`** となる（非スナップショット時の死活確認 `+1回` は 3.4 参照）。
- **通常スレッドと `GetThreadPriority` のスキップ**: 定常隔離中にスレッド数の増減がない場合（`canSkipNormalThreads == true`、`Type 5` の 1枠確定＋他枠 `sleeping...` 時を含む）、
 通常スレッド `N` 本のハンドルオープンとサイクル計測は **0回** となり、確定オーディオスレッド（1〜2本）のみ開く。また非アタッチ時および `RAATServer` 探索中は `GetThreadPriority` も **0回**（侵略スレッド抑制・`Dyn Pri` 確認時のみ個別取得）。
- **`RAATServer.exe` の省負荷探索**: 未特定時も毎ターン走査せず、
 3.0秒に1回だけ `NtQuerySystemInformation` で全スレッドの `WaitReason == 4` を一括検査する。

---

### 3.3 【`INI × 3` 期間】（1.5 秒間：特定直後 3 ターンの即応猶予）

- `Type 1, 3, 4, 5` がオーディオスレッドを特定した直後の **3 ターン（1.5秒間）**、および UI で設定変更した直後の **2 ターン（1.0秒間）** は、
 3 秒ゲートに入らず **毎ターン（500ms毎）で「3.2 定常隔離中」と同じ確認** を行う。
- 3 ターン経過後、自動的に「3.2（`INI × 6`：3.0秒周期）」へ移行する（なお `INI = 1000ms` 設定時は 3.0秒定常ゲート自体が `INI × 3` となる）。

---

### 3.4 【`INI × 1` 周期】（500ms 毎：常時監視・未特定探索中）

| 発行 API | 全Type死活 | `Type 0` 常時 | `Type 2` 待機/再生 | `Type 1,3,4` 探索 | `Type 5` 探索 |
| :--- | :---: | :---: | :---: | :---: | :---: |
| `GetExitCodeProcess` | ▲ 登録PID分 | - | - | - | - |
| `NtGetNextThread` | - | ● N回 | ● N回 | ● N回 | ● N回 |
| `NtQueryInformationThread` | - | ● N回 | ● N回 | ● N回 | ● N回 |
| `OpenProcess` | ▲ 登録PID分 | ● 2回 | ● 2回 | ● 2回 | ● 2回 |
| `GetProcessAffinityMask` | - | ● 1回 | ● 1回 | ▲ 初回1回 | ▲ 初回1回 |
| `SetProcessAffinityMask` | - | ▲ 差分 | ▲ 差分 | ▲ 初回1回 | ▲ 初回1回 |
| `GetPriorityClass` | - | ● 1回 | ▲ 再生中1回 | ▲ ブラウザ例外 | ▲ 確定枠あり |
| `SetPriorityClass` | - | ▲ 差分 | ▲ 差分 | ▲ 差分 | ▲ 差分 |
| `OpenThread` | - | ▲ 新規TID | ▲ 候補/確定分 | ● N回 | ● N回 |
| `CloseHandle` | ▲ 登録PID分 | ● N+2回〜 | ▲ 候補+N+2 | ● 2N+2回 | ● 2N+2回 |
| `QueryThreadCycleTime` | - | - | ▲ 候補/確定分 | ● N回 (待機0) | ● N回 (待機0) |
| `ReadProcessMemory` (32KB) | - | - | - | ▲ 生涯1回 | ▲ 生涯1回 |
| `VirtualQueryEx` | - | - | - | ▲ 生涯1回 | ▲ 生涯1回 |
| `GetMappedFileNameA` | - | - | - | ▲ 生涯1回 | ▲ 生涯1回 |
| `SetThreadAffinityMask` | - | - | ▲ 差分 | ▲ 特定時 | ▲ 差分 |
| `SetThreadIdealProcessor` | - | - | ▲ 差分 | ▲ 特定時 | ▲ 差分 |
| `GetThreadPriority` | - | ▲ 新規/確認 | ▲ アタッチ/確認 | ▲ DAW探索N回 | ● N回 |
| `SetThreadPriority` | - | ▲ 新規TID | ▲ アタッチ差分 | ▲ 特定/抑制時 | ▲ 特定/抑制時 |
| `NtSetInformationThread` | - | ▲ 新規TID | ▲ Fixed差分 | ▲ 特定時 | ▲ 特定時 |

- **全 Type 死活確認（`!shouldScanProcesses` 時）**: `OpenProcess(QUERY_LIMITED)` + `GetExitCodeProcess` + `CloseHandle` は **`m_config.rules` に登録済の稼働 `PID > 4` のみ**に限定して実行し、`SNAPPROCESS` 実行ターンは **0回**。
- **`Type 0` (`audiodg.exe`)**: 既存 TID は `m_audioDgAppliedTids` でスキップされるため新規 TID がない限り `OpenThread` は **0回**（Col 2 非アタッチ時は新規出現時も優先度 API **0回**）。環境の PPL 保護で `OpenProcess(PROCESS_QUERY_INFORMATION)` が拒否された場合は、`NtGetNextThread` の代わりに `NtQuerySystemInformation(5)` による TID 一括取得へフォールバックする。
- **`Type 2` (`Step 2`)**: `flag == 1`（親制御スレッド）と `alreadyEvacuated` 一致の退避済スレッドは事前キャッシュ照合のみでスキップし、`OpenThread` / `QueryThreadCycleTime` は **0回**（`flag = 0` 候補と `flag = 2` 確定のみ開く）。
- **`Type 1, 3, 4, 5` の探索と休眠・例外**:
  - 特定完了後は 3.3（3 ターン猶予）を経て 3.2（`INI × 6`）へ移るため、この `INI × 1` 表の欄は **0回** になる。
  - `Type 1, 3, 5` の `Standby 3..1` 待機ターン中・`sleeping...` 中および `RAATServer` 探索中は、`QueryThreadCycleTime`・`GetThreadPriority` ともに **0回**（`GetThreadPriority` 一括取得は探索中の `Type 3` DAW と `Type 5` のみ）。
  - `Type 1, 3, 4` の探索中でも、名前に `chrome`/`vivaldi`/`edge`/`brave`/`opera` を含む非 `Idle` ルールのみ優先度復元抑止のため毎ターン `GetPriorityClass` を発行する。

---

### 3.5 【`INI / 4` 周期】（125ms 毎 × 3回：`Type 2` Chromium 高速サンプリング中）

| 発行 API | `Type 2` (1〜2回目) | `Type 2` (3回目確定) | `Type 0, 1, 3, 4, 5` |
| :--- | :---: | :---: | :---: |
| `NtGetNextThread` | ● N回 | ● N回 | - |
| `NtQueryInformationThread` | ● N回 | ● N回 | - |
| `OpenProcess` | ● 2回 | ● 2回 | - |
| `OpenThread` | ● 候補/未退避分 | ● 候補/未退避分 | - |
| `CloseHandle` | ● 候補+N+2回 | ● 候補+N+2回 | - |
| `QueryThreadCycleTime` | ● 候補TID分 | ● 候補TID分 | - |
| `SetThreadAffinityMask` | - | ● 勝者+未退避分 | - |
| `SetThreadIdealProcessor` | - | ● 勝者1本 | - |
| `GetThreadPriority` | - | ▲ アタッチ/確認 | - |
| `SetThreadPriority` | - | ▲ アタッチ差分 | - |
| `NtSetInformationThread` | - | ▲ Fixed差分のみ | - |

- `Type 2` の `Step 2` 待機中に `Delta >= 閾値` を検知した時だけ 125ms × 3回（計375ms）動作し、
 3区間の Delta 合計 1 位を `flag = 2` に確定して `INI × 1`（500ms）へ戻る。
- 計測途中で `Delta == 0` となったスレッドは脱落マーク（`-1`）、`flag = 1`（兄弟音声）と
 `alreadyEvacuated` 一致の退避済スレッドは事前キャッシュでスキップし、`OpenThread` は **0回**。
- **高速サンプリング中の他ルール挙動**: `TIMER_POLLING_ID` 自体が一時的に `INI / 4`（125ms）へ加速されるため、
 3 ターン（375ms）間は他の有効ルールおよび `GetExitCodeProcess` 死活確認も 125ms 間隔で 3 回動作する。

---

### 3.6 【単発・イベント駆動】（初回 1 回 / 0秒＋約 3 秒後の 2 回 / UI 操作時のみ）

| トリガーイベント | 対象 Type | 発行 API | 発行回数 |
| :--- | :---: | :--- | :---: |
| 新規 PID 検出時 | 全 `Type 0〜5` | `SetProcessInformation` (HighQoS) | 初回 1回のみ |
| 初回 `AppType` 判定時 | 未判定アプリ | `QueryFullProcessImageNameA` | 初回 1回のみ |
| Chromium 親制御退避 | `Type 2` | `QueryThreadCycleTime` + `SetAffinity` | 初回 1回のみ |
| 実測 `Dyn Pri` 取得 | `Type 0` (`audiodg`) | `NtQuerySystemInformation` (`CtxSw` 1位) | 0秒 + 約3秒後の 2回 |
| 実測 `Dyn Pri` 取得 | `Type 1〜5` | `NtQueryThread` + `GetThreadPriority` | 0秒 + 約3秒後の 2回 |
| ピッカー優先度変更 | 全 `Type 0〜5` | `SetPriorityClass` / `SetThreadPriority` / `NtSet` | 操作時 1回のみ |
| ピッカー非アタッチ復帰 | 全 `Type 0〜5` | *(発行なし：OS優先度を維持し監視解除)* | 0回 |
| Col 4 (`❚❚`) 停止・再開 | `Type 0` (`audiodg`) | `NtSuspendProcess` / `NtResumeProcess` | 操作時 1回のみ |
| Col 4 (`❚❚`) 停止・再開 | `Type 1〜5` | `SuspendThread` / `ResumeThread` | 操作時 1回のみ |
| `audiodg` 解除・終了時 | `Type 0` (`audiodg`) | 全コア復元 + `NORMAL` + 全TID `+2` | 解除・終了時 1回のみ |

- **`Ctrl` + 終了・再起動時の `audiodg.exe` 非復元**: メイン画面の `[Exit]` ボタンおよびタスクトレイの `Exit` / `Restart` / `Restart as Administrator` を `Ctrl` キー押下中に実行した場合は、`RestoreAudioDgProcessLocked` をスキップして現在のコア・優先度設定を維持する。
- **`Type 1, 3, 4, 5` の 2 回目 `Dyn Pri` 取得**: 3.0 秒経過後の次回 `INI × 6` ターン（特定から約 3.0〜4.5 秒後）に実行される。
  またピッカー等で優先度設定を変更した時も、実測値再確認のため同 2 段階取得が再トリガされる。
