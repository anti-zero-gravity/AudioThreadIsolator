# EcoQoS Power Throttling — 内部メカニズム調査レポート

> 調査ソース: Microsoft Learn (SetProcessInformation / PROCESS_POWER_THROTTLING_STATE)、Zenn「Windows 11 CPUスケジューラ再入門」(NTTデータテクノロジー)、Intel 64 and IA-32 Architectures Software Developer's Manual (HWP / Thread Director)、ACPI Specification (CPPC v2/v3)、Stack Overflow Windows 11 timer resolution issues

---

## 核心的結論：EcoQoSは「クォンタムの間引き」ではない

**EcoQoS はタイムスライス（クォンタム）の長さを削る仕組みではない。**

スレッドに与えられるクォンタム（例: 3ユニット = 15.625ms）の割り当て自体は、EcoQoS が有効であっても変わらない。
OS スケジューラから見れば「同じ時間だけ席を貸している」状態。

EcoQoSが実際に行うのは以下の**2軸**の制御であり、いずれもクォンタム長の変更ではない：

| 制御軸 | 内容 |
|:---|:---|
| **実行速度の制限** (`EXECUTION_SPEED`) | PPM が EPP（Energy Performance Preference）を最省電力値(255)へ書き換え → CPU周波数を最低P-Stateへクランプ + E-coreへ優先配置 |
| **タイマー解像度の無視** (`IGNORE_TIMER_RESOLUTION`) | `timeBeginPeriod(1)` 等の高精度タイマー要求をカーネルが握りつぶし → 15.625ms以上のTimer Coalescingへ強制引き戻し |

---

## 1. `EXECUTION_SPEED` (0x2) — 実行速度スロットリング

### 何が起きるか
PPM（Processor Power Management）が以下を執行する：

1. **EPP を 255（Max Efficiency）に書き換え** — ACPI `_CPC`（Collaborative Processor Performance Control）経由でCPU内部の自律電力管理機構に「希望性能値 = Minimum」を送出
2. **E-core への優先配置** — Intel Thread Director と連携し、EcoQoSスレッドを高効率コアへスケジューリング。P-coreの空き待ちから隔離

### クォンタムはどうなるか

| 項目 | EcoQoS OFF | EcoQoS ON (EXECUTION_SPEED) |
|:---|:---|:---|
| クォンタム長 (ms) | 15.625ms (3ユニット) | **15.625ms (3ユニット)** ← 同一 |
| 1クォンタム内の処理可能命令数 | 通常 (P-core 5GHz等) | **激減** (E-core 最低P-State) |

- 「3ユニットのうち1ユニットが間引かれて2ユニットになる」というモデルは誤り。
- 3ユニットはそのまま与えられるが、各ユニット内でCPUが実行できるクロックサイクル数がハードウェアレベルで大幅に減少する。

### 影響範囲

| API | 影響範囲 |
|:---|:---|
| `SetProcessInformation` | プロセス内の全スレッド |
| `SetThreadInformation` | 対象スレッド単体 |

**OS論理層**: スレッド（またはプロセス）単位で解除される。  
**ハードウェア物理層**: P-State（動作周波数）は**物理コア単位**または**E-coreクラスタ単位**で制御されるため、同一コアに同居する他スレッドに波及する。

---

## 2. `IGNORE_TIMER_RESOLUTION` (0x4) — タイマー解像度無視

### 何が起きるか

1. `timeBeginPeriod(1)` や `NtSetTimerResolution` の呼び出し自体は**成功コードを返す**（エラーにならない）
2. しかしカーネル内部の「タイマー要求リスト」で当該プロセスからの要求は**マスク（無効化）**される
3. 結果、`Sleep(1)` 等は1msで起床することが保証されなくなり、OSデフォルト（15.625ms）のTimer Coalescingに引き戻される

### 影響範囲: プロセス単位で閉じる（グローバルではない）

| シナリオ | グローバルタイマー解像度 | 当該スレッドの起床タイミング |
|:---|:---|:---|
| 他プロセスが `timeBeginPeriod(1)` を維持 | **1ms のまま** | 1msの割り込みでは起床されない。15.625ms境界または他HighQoSスレッドの起床タイミングに「便乗」 |
| 誰も高精度タイマーを要求していない | 15.625ms | 15.625ms境界 |

> [!WARNING]
> **オーディオスレッドへの致命的影響**: オーディオスレッドは計算を回し続けるのではなく、1周期（1.33ms / 2.67ms / 10ms等）の転送後に `Sleep` や `WaitForSingleObject` で眠るイベント駆動型。このフラグが有効だと、本来1.33ms後に起床すべきスレッドが15ms以上眠り続け、バッファアンダーランが発生する。

---

## 3. 2つのフラグは独立制御可能

`EXECUTION_SPEED` と `IGNORE_TIMER_RESOLUTION` は**直交するフラグ**であり、`ControlMask` と `StateMask` のビット操作で個別に ON/OFF できる。

```cpp
PROCESS_POWER_THROTTLING_STATE State = { 0 };
State.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;

// パターンA: 両方同時に有効化（OS自動EcoQoSの標準挙動）
State.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED 
                  | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
State.StateMask   = PROCESS_POWER_THROTTLING_EXECUTION_SPEED 
                  | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;

// パターンB: EXECUTION_SPEEDだけ解除、タイマー無視は維持
State.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
State.StateMask   = 0;  // StateMask=0 → 解除

// パターンC: 両方とも解除（Power Throttling完全バイパス）
State.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED 
                  | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
State.StateMask   = 0;
```

Windows 11 の OS 自動電力管理（タスクマネージャーの「効率モード」等）では、アプリがバックグラウンドや非アクティブになると両フラグがセットで有効化される。

---

## 4. 混在環境における「Max Win（最大値の勝利）」のハードウェア物理機序

同一コアまたは同一クラスタ内に「EcoQoS解除スレッド（HighQoS）」と「EcoQoS有効スレッド」が同居した際、なぜ周波数が引き上げられるのか、その物理レイヤの機序は以下の通り。

```
【ハードウェア電圧・周波数ドメインの物理制約】
┌────────────────────────────────────────────────────────┐
│ P-core (SMT)                                           │
│ ┌──────────────────────────┐┌────────────────────────┐ │
│ │ 論理スレッド 0 (HighQoS) ││ 論理スレッド 1 (EcoQoS)│ │
│ │ EPP = 0 (最高クロック要求)││ EPP = 255 (省電力要求)│ │
│ └─────────────┬────────────┘└───────────┬────────────┘ │
│               └──────────────┬──────────┘              │
│                              ▼                         │
│               [ CPU 電源管理ユニット (PCU / HWP) ]      │
│               判定: Max Win (最小EPP = 0 を採用)       │
│                              │                         │
│                              ▼                         │
│               物理コア周波数: 5.0 GHz (高電圧供給)     │
│               ★論理スレッド 1 も 5.0 GHz で巻き添え実行│
└────────────────────────────────────────────────────────┘
```

### ① 電圧・周波数ドメイン（VR / PLL）の共有構造
CPUのクロック（周波数）や供給電圧は、1本ごとの論理スレッド単位で個別に昇降させることは物理的に不可能である。
- **P-core（SMT）**: 2つの論理プロセッサが、同一の物理コア、同一の電圧レギュレータ（FIVR/IVR）、同一のPLL（位相同期回路）を完全に共有する。
- **E-coreクラスタ**: 通常4基の物理コアが1組のクラスタを形成し、クラスタ単位で共有L2キャッシュおよび同一の電圧・周波数ドメインを共有する。

### ② ACPI CPPC と PPM による EPP（Energy Performance Preference）集計
Windows 11 の PPM（電源管理エンジン）は、実行可能状態にあるスレッドの QoS 属性を評価し、ACPI CPPC インターフェースを通じてハードウェア（MSR / レジスタ）へ要求を書き込む：
- **HighQoS スレッド実行時**: `Desired Performance` を最大値にし、`EPP = 0〜64`（パフォーマンス優先）を提示。
- **EcoQoS スレッド実行時**: `Desired Performance` を最小値にし、`EPP = 192〜255`（電力効率優先）を提示。

### ③ ハードウェア自律電力管理（Intel HWP / AMD CPPC）の統合アルゴリズム
CPU内部のハードウェアコントローラ（PCU / Punit）は、同一ドメイン内の各コアから上がってくる要求を集計する。
- 1つでも高パフォーマンス要求（EPP = 0）が存在する場合、ハードウェアはドメイン全体の物理ターゲット周波数を最高要求側へクランプする。
- **巻き添え（サイドエフェクト）**: EcoQoS有効スレッドは、自らはバックグラウンド処理を行っているにもかかわらず、物理的に引き上げられた高クロック（高周波数・高電圧）で実行される。これにより、省電力・低発熱化というEcoQoS本来の目的が無効化される。

---

## 5. Windows 11 と Intel Thread Director のスレッドローテーション戦略

OSスケジューラは、この同居による非効率を回避するため、Intel Thread Director（ITD）からのハードウェアテレメトリと連携したマイグレーション（配置・ローテーション）を行う。

### ① コア種別の分離配置
- **HighQoS（EcoQoS解除）スレッド**: P-core優先でスケジューリング。
- **EcoQoS有効スレッド**: E-coreクラスタへ優先配置。P-coreの実行キューからは原則締め出される。

### ② コンテキストスイッチ時のコンフリクト評価と追い出しローテーション
E-coreクラスタ内で両者が混在せざるを得ない場合（コア飽和時など）：
1. **即時プリエンプション**: HighQoSスレッドが待機状態から起床（Ready）した瞬間、同一コア/クラスタで走行中のEcoQoSスレッドを即座に中断（プリエンプト）する。
2. **別クラスタへの追い出しローテーション**: ディスパッチャ（`KiSelectNextThread`）は、走行中のEcoQoSスレッドを「EcoQoSスレッドのみで構成された別のE-coreクラスタ」または空きE-coreへとマイグレーション（移送）させる。

### ③ Thread Director (ITD) クラス判定による昇格の抑止
通常スレッドであれば、重いベクトル演算（AVX2 / 深層学習命令など）を実行すると、ITDがClass 2/Class 3と判定してP-coreへ自動昇格（マイグレーション）させる。
しかし**EcoQoSが有効化されたスレッドに対しては、ITDのクラス判定が高負荷を示してもP-coreへの昇格がブロック**される。E-coreクラスタ内でのローテーションに留め、P-coreのリソースを保護する。

---

## 6. スレッドアフィニティ（CPU親和性）を用いたコア隔離戦略

OS任せのスケジューリングでは、過渡的な負荷スパイク時や全コア高負荷時に同居が発生し、コンテキストスイッチ遅延（L1/L2キャッシュ追い出し）やMax Winの巻き添えが発生する。
これを防ぐための決定的な回避策が、**Win32 APIを用いた物理コア隔離（アフィニティ固定）**である。

### 隔離設計のアーキテクチャ

```
[ CPU コアトポロジ全体 ]
├─ Core 0, 1 (P-core 0, SMT): システム・通常アプリ
├─ Core 2, 3 (P-core 1, SMT): システム・通常アプリ
├─ Core 4    (P-core 2, SMT0) ───★【HighQoS聖域コア】オーディオスレッドのみ配置
├─ Core 5    (P-core 2, SMT1) ───★【休止/遮断】スレッド配置禁止（空打ち）
└─ Core 6〜13 (E-core クラスター群)
     ├─ E-Cluster 0: 一般バックグラウンドタスク
     └─ E-Cluster 1: ★【EcoQoS隔離区画】EcoQoSスレッド群を幽閉（他へ波及させない）
```

1. **HighQoS（オーディオ等）の聖域隔離**:
   - P-coreの物理コア（例: Core 4）にオーディオスレッドをピン留め。
   - ペアとなる論理コア（SMT1 / Core 5）には他スレッドを配置させない。これにより、SMTパイプライン競合およびL1/L2キャッシュスラッシングをゼロ化する。
2. **EcoQoSスレッドのE-coreクラスタ幽閉**:
   - バックグラウンドスレッドに対し、特定E-coreクラスタ（例: Core 10〜13）のみを許可するアフィニティマスクを適用。
   - P-coreや他クラスタへの侵入を物理的に阻止し、Max Winによる他コアクロック上昇を遮断する。

### Win32 C++ による実装コード例

```cpp
#include <windows.h>
#include <iostream>
#include <vector>

// 1. スレッド単位のEcoQoS設定（EXECUTION_SPEED と IGNORE_TIMER_RESOLUTION の両方を制御）
bool ConfigureThreadQoS(HANDLE hThread, bool enableEco) {
    THREAD_POWER_THROTTLING_STATE State = { 0 };
    State.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    State.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED 
                      | THREAD_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;

    if (enableEco) {
        // EcoQoS有効化（省電力・タイマー解像度無視）
        State.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED 
                        | THREAD_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    } else {
        // HighQoS（完全解除・最高パフォーマンス維持・1msタイマー保証）
        State.StateMask = 0;
    }

    return SetThreadInformation(hThread, ThreadPowerThrottling, &State, sizeof(State)) != 0;
}

// 2. プロセッサトポロジを解析し、P-core / E-core のマスクを動的取得
void GetProcessorTopologyMasks(DWORD_PTR& pCoreMask, DWORD_PTR& eCoreMask) {
    pCoreMask = 0;
    eCoreMask = 0;

    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return;

    std::vector<BYTE> buffer(length);
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = 
        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());

    if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        BYTE* ptr = buffer.data();
        while (ptr < buffer.data() + length) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX current = 
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);

            if (current->Relationship == RelationProcessorCore) {
                DWORD_PTR mask = current->Core.GroupMask[0].Mask;
                // EfficiencyClass: 0 = E-core, 1以上 = P-core (Intelハイブリッド)
                if (current->Core.EfficiencyClass == 0) {
                    eCoreMask |= mask;
                } else {
                    pCoreMask |= mask;
                }
            }
            ptr += current->Size;
        }
    }
}

// 3. スレッドアフィニティの適用
bool SetThreadPinning(HANDLE hThread, DWORD_PTR coreMask) {
    // 64論理コア以下のグループ0に対するピン留め
    DWORD_PTR prevMask = SetThreadAffinityMask(hThread, coreMask);
    return prevMask != 0;
}

// 実行例
void SetupAudioAndBackgroundThreads(HANDLE hAudioThread, HANDLE hBackgroundThread) {
    DWORD_PTR pCoreMask = 0;
    DWORD_PTR eCoreMask = 0;
    GetProcessorTopologyMasks(pCoreMask, eCoreMask);

    // [A] オーディオスレッドのセットアップ (HighQoS + P-Core特定の1コアに固定)
    ConfigureThreadQoS(hAudioThread, false); // EcoQoS解除
    // 例: P-Coreの最初の1物理コア（Bit 0x01）にピン留め
    DWORD_PTR dedicatedAudioCore = 0x01; 
    SetThreadPinning(hAudioThread, dedicatedAudioCore);

    // [B] バックグラウンドスレッドのセットアップ (EcoQoS + E-Coreクラスタに幽閉)
    if (eCoreMask != 0) {
        ConfigureThreadQoS(hBackgroundThread, true); // EcoQoS有効
        SetThreadPinning(hBackgroundThread, eCoreMask); // E-Coreクラスタのみで実行許可
    }
}
```

### CPU Sets API（`SetThreadSelectedCpuSets`）との使い分け
- **`SetThreadAffinityMask`（ハードアフィニティ）**:
  - 指定したコア以外での実行をカーネルレベルで厳密に遮断する。
  - オーディオ再生などの極低遅延・リアルタイム保証に必須。
- **`SetThreadSelectedCpuSets`（ソフトアフィニティ / CPU Sets）**:
  - 指定したコアへの割り当てを優先するが、コアが枯渇した際には一時的に他コアでの実行をOSに許容する。
  - バックグラウンドタスクや一般UIスレッドの負荷分散に適している。

---

## 7. ユーザーの疑問・論点への直接回答

### Q1: EcoQoSを特定スレッドだけ解除すると、そのCPU#ではクォンタムは15.625msのまま保証されるのか？
**はい、15.625ms（正確にはOS設定に依存する3ユニット分）のまま。** EcoQoSの有無にかかわらず、クォンタム長は変わらない。EcoQoSが制御するのはクォンタム長ではなく、CPU周波数とタイマー起床タイミング。

### Q2: そのスレッドについてだけ間引きがされなくなる？
**OSの論理層ではスレッド単位。** `SetThreadInformation` で解除すれば、そのスレッドだけがEcoQoSから外れる。ただしハードウェア層（P-State/周波数）は物理コア単位のため、同居スレッドに副作用が波及する（Max Win原則）。

### Q3: 3クォンタムユニットのうち1ユニットが間引かれて2ユニットしか使えなくなるのか？
**いいえ、そのモデルは不正確。** 3ユニットは3ユニットのまま与えられる。EcoQoSが行うのは：
- 各ユニット内でCPUが実行できるクロックサイクル数の物理的削減（周波数クランプ）
- スリープからの起床タイミングの引き延ばし（タイマー解像度無視）

この2つの複合効果が「クォンタムを間引いている」ように見える結果を生むが、OSスケジューラがクォンタムユニット数を減らしているわけではない。

---

## 8. ATI設計への含意と実装ガイドライン

ATI（Audio Thread Isolator）において、オーディオスレッドの確実な保護と省電力タスクの共存を達成するための要件：

| 項目 | 推奨設計 | 理由・物理効果 |
|:---|:---|:---|
| **EcoQoS解除範囲** | `EXECUTION_SPEED` と `IGNORE_TIMER_RESOLUTION` の**両フラグを同時解除 (`StateMask = 0`)** | 片方のみの解除では、クロックが維持されてもタイマー起床が15.625msへ引き戻されて音ちぎれが発生するため。 |
| **コア隔離（アフィニティ）** | 対象オーディオスレッドを **単一のP-core（SMT0）へハードピン留め** | Max Winによる他スレッド巻き添えを根絶し、コンテキストスイッチおよびキャッシュスラッシングをゼロ化。 |
| **SMT兄弟コアの処置** | ペアとなるSMT論理コアから他スレッドを排除（またはマスク除外） | 同一物理コアのパイプライン競合によるマイクロジッターを防止。 |
| **他スレッドのEcoQoS化** | バックグラウンド処理はE-coreクラスタへアフィニティ制限 | P-core側のサーマルバジェットおよび電力枠をオーディオスレッド専用に温存。 |
