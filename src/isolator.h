#pragma once

#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


namespace ati {

// コアトポロジー情報 (QITuner 準拠)
struct CoreInfo {
  int logicalIndex;
  BYTE efficiencyClass;
  std::string label; // "P0", "P1", "E4", "E0", "#0" 等
};

class CpuTopology {
public:
  static std::vector<CoreInfo> GetCpuCores();
  static bool CheckIfAllECoresCpu();
  static int GetSystemCoreCount();
};

// 1つの監視対象プロセスに対する設定・状態
struct ProcessRule {
  std::string processName;     // 実行ファイル名 (例: "mpv.exe")
  int audioCore;               // 隔離先コア番号 (代表コア #0 始まり、例: 3)
  DWORD_PTR audioAffinityMask; // 隔離先オーディオコアマスク (0:
                               // audioCoreから単一生成、複数コア指定可)
  int audioPriority; // オーディオスレッド適用優先度 (デフォルト: -15 /
                     // THREAD_PRIORITY_IDLE)
  DWORD_PTR normalAffinityMask = 0; // 通常スレッド退避先マスク (0:
                                    // Global設定のnormalAffinityMaskを自動継承)
  DWORD targetPriority;       // 特定優先度フィルタ (0: 指定なし/自動判定)
  DWORD processPriorityClass; // プロセス優先度 (0: 変更なし)

  // 監視除外・ペンディングフラグ
  // (チェックONで監視・アイソレートから除外、デフォルト: false)
  bool isBypassed = false;
  // 個別ヒューリスティック探索フラグ (デフォルト: true)
  bool enableHeuristics = true;
  // appType アプリ種別判定 (0: 未判定, 1: 非Chromium, 2: Chromium系, 3:
  // DAW型高負荷)
  int appType = 0;
  bool ignoreSig =
      false; // シグネチャ非依存モード (TimeCritical 15 + Cycles Delta > 50M)
  int ignoreSigCount = 0; // シグネチャ非依存モード スレッド数 (0: 無効, 1: 1スレッド, 2以上: t個スレッド)
  std::vector<DWORD> activeAudioTids; // 検出・確定されたオーディオスレッドID群 (TID昇順)
  std::vector<int> audioCores; // 各スレッド用の個別隔離先コア番号 (空の場合は audioCore を使用)
  std::vector<int> audioPriorities; // 各スレッド用の個別適用優先度 (空の場合は audioPriority を使用)
  std::unordered_set<DWORD> suspendedAudioTids; // 個別スレッド一時停止管理セット
  int multiSearchTurns = 0;       // IgnoreSig 複数スレッド探索用カウントダウンターン数 (10秒 = 10 * turnsPerSec)
  size_t lastIdentifiedCount = 0; // 前回確認時の確定オーディオスレッド数
  bool multiSearchSleeping = false; // 10秒経過しても次スレッドが未検出の場合の休眠フラグ (以降の行は同時に sleeping)
  double cyclesDelta = 0.0; // 個別 Cycles Delta 走査閾値 (M/s, 0.0: Global継承)
  bool isDeclineBoost =
      false; // Decline:1 (ポーリングブーストを拒否し1000msで計測)
  DWORD audioServicePid =
      0; // Chromiumモード: 特定済み Audio Service の PID (ランタイムのみ)

  // リアルタイム動的状態 (GUI表示用)
  DWORD applyCount = 0; // 累計再生スレッド隔離回数
  int currentThreadCount =
      0; // 検出された総スレッド数 (アフィニティ制御対象スレッド規模)
  std::string detectedThreadName =
      "";                   // 検出された再生スレッド名 (例: "ao/wasapi")
  DWORD activePid = 0;      // 検出中のプロセスID (0: 未起動)
  DWORD activeAudioTid = 0; // 検出・隔離中のオーディオスレッドID
  bool isAudioThreadSuspended =
      false;              // オーディオスレッド手動一時停止(テスト検証)中か
  bool isRunning = false; // 現在稼働中か
  bool isAudioIsolated =
      false; // オーディオスレッド検出・通常スレッド退避完了か
  bool hasIntruderThreads =
      false; // オーディオ専有コアへの侵入・同居スレッドを検知・制圧中か
  bool wasHalfAutoPromoted =
      false; // Half-Isolated判定によるLowest(-2)への自動昇格が実行済みか
  bool chromiumScanAttempted =
      false; // Chromium起動時/Bypass復帰時のAudioService特定走査を試行済みか(未特定時はStandbyへ移行し毎秒走査を抑止)

  // 前回終了時ステータス記録と照合 (起動時引き継ぎ用)
  DWORD lastAudioPid = 0; // 前回終了時ステータス記録: オーディオPID
  DWORD lastAudioTid = 0; // 前回終了時ステータス記録: オーディオTID (単一互換用)
  std::vector<DWORD> lastAudioTids; // 前回終了時ステータス記録: オーディオTID群 (IgnoreSig 複数スレッド対応)

  // 新探索フロー状態管理
  int searchPhase = 0; // 0: 未開始/確定完了, 1: Searching...(2秒/4ターン), 2:
                       // カウントダウン(Standby 10..3), 3:
                       // 終盤再確定(Standby 2..1), 4: sleeping...
  int searchTurns = 0; // 各フェーズでの残りターン数 (500ms単位)
};

// 全体設定
struct GlobalConfig {
  int defaultAudioCore; // デフォルト隔離コア (代表コア #0 始まり)
  DWORD_PTR defaultAudioAffinityMask; // デフォルト隔離オーディオコアマスク (0:
                                      // defaultAudioCoreから単一生成)
  int defaultAudioPriority;     // デフォルトオーディオ優先度 (-15:
                                // THREAD_PRIORITY_IDLE)
  DWORD_PTR normalAffinityMask; // 通常スレッド退避先マスク (0:
                                // 隔離コア以外の全コア自動)
  int pollingIntervalMs;        // 監視周期 (デフォルト 1000ms)
  int boostPollingIntervalMs = 250; // ブースト監視周期 (デフォルト 250ms)
  double defaultCyclesDelta =
      5.0; // 4KBスタック走査を発行する秒換算負荷閾値 (M/s, デフォルト 5.0)
  std::string rawDefaultCyclesDeltaStr =
      ""; // INIの生文字列 (空欄状態の保持・再出力用)
  bool enableHeuristics = false; // 非MMCSSスレッド探索用ヒューリスティック監視有効化
                                 // (デフォルト: false)
  std::vector<int>
      columnWidths; // 各カラム幅 (論理ピクセル, 空の場合はデフォルト)
  int windowHeight =
      0; // ウィンドウ全体の高さ (論理ピクセル, 0 の場合は自動計算)
  std::vector<ProcessRule> rules; // 監視プロセス一覧
};

class ThreadIsolator {
public:
  ThreadIsolator();
  ~ThreadIsolator();

  void Initialize(const GlobalConfig &config);
  void UpdateConfig(const GlobalConfig &config);
  GlobalConfig GetConfig();

  // 500ms タイマーごとにメインループから呼び出される走査・隔離関数
  bool ScanAndIsolate();

  // ルール管理
  void AddRule(const ProcessRule &rule);
  void RemoveRule(size_t index);
  void UpdateRule(size_t index, const ProcessRule &rule);
  void ToggleProcessHeuristics(size_t index);
  void ToggleProcessBypass(size_t index);
  void RestartSearch(size_t index);
  void ResumeMultiSearch(size_t index);
  bool ToggleSuspendAudioThread(size_t index, DWORD tid = 0);
  bool IsAudioThreadSuspended(size_t index, DWORD tid) const;
  void ResumeAllSuspendedThreads();
  std::vector<ProcessRule> GetRulesSnapshot();
  void SortRulesByName();

  // 起動時照合によるINI更新要求チェック
  bool CheckAndClearInitialNeedSave();

  // ポーリング間隔の正規化 (100, 200, 500, 1000 ms のみに限定)
  static int NormalizePollingInterval(int ms);

  // システム情報ヘルパー
  static int GetSystemCoreCount();
  static DWORD_PTR GetFullCoreMask(int coreCount);
  static DWORD_PTR MakeCoreMask(int coreIndex);
  static DWORD_PTR MakeDefaultNormalMask(int coreCount, int isolatedCore);

  // Chromium Audio Service モード: プロセスのコマンドラインを取得
  static std::string QueryProcessCommandLine(HANDLE hProcess);
  // Chromium判定: exe ファイルから "chromeos" 文字列を検索
  static bool DetectChromiumExe(HANDLE hProcess);

  // ヒューリスティック状態テキスト取得 (GUI ステータスバー表示用)
  std::string GetHeuristicsStatusText() const;

private:
  mutable std::mutex m_mutex;
  GlobalConfig m_config;
  std::string m_heuristicsStatusText;
  bool m_initialNeedSave = false;

  // ── リファクタリング: 共通ヘルパーメソッド ──
  // Chromium 系 PID 追跡状態の一括クリア
  void ClearChromiumTrackingState(DWORD pid);
  // Suspend 中オーディオスレッドの安全な Resume
  static void ResumeAudioThreadIfSuspended(ProcessRule &rule);
  // 表示用スレッドカウントの算出・更新 (戻り値: stateChanged)
  static bool UpdateDisplayThreadCount(ProcessRule &rule, int totalThreadCount);
  // ルール状態変更ログ出力
  static void LogRuleStateChange(const ProcessRule &rule);

  // オーディオスレッドへの優先度・Ideal
  // Processor・アフィニティマスク適用共通ルーチン (Type 1 & Type 2 共通)
  bool ApplyAudioThreadSettings(HANDLE hThread, DWORD tid, int targetAudioPrio,
                                int audioCore, DWORD_PTR audioMask,
                                ProcessRule &rule);

  // 通常スレッドへのアフィニティマスク退避適用共通ルーチン (Type 1 & Type 2
  // 共通)
  bool EvacuateNormalThreadSettings(HANDLE hThread, DWORD tid,
                                    DWORD_PTR normalMask);

  // キー: TID, 値: 適用済みマスク
  std::unordered_map<DWORD, DWORD_PTR> m_appliedThreads;

  // Chromium モード: アフィニティ適用済み子プロセス PID セット (差分検出用)
  std::unordered_set<DWORD> m_chromiumMaskedPids;

  // 非Chromium: 親プロセスマスク拡張済み PID セット
  // (差分検出・定常時API呼び出し削減用)
  std::unordered_set<DWORD> m_nonChromiumMaskedPids;

  // Chromium モード: コマンドライン走査済み子プロセス PID セット
  // (毎秒総当たり走査防止)
  std::unordered_set<DWORD> m_chromiumScannedPids;

  // Chromium Audio Service スレッド追従管理 (テスト仕様: 蓄積保持)
  struct ChromiumThreadTrackInfo {
    uint8_t flag = 0; // 0: 未検査, 1: 非オーディオ, 2: オーディオ (確定)
    ULONG64 lastCycles = 0;
    ULONG64 lastDelta = 0;
  };
  struct ThreadSamplingEntry {
    DWORD tid = 0;
    int64_t deltas[10] = {0}; // Δ(0) ... Δ(9)。脱落時は deltas[0] = -1
    ULONG64 lastCycle = 0;
  };
  struct ChromiumAudioState {
    int step = 1; // 1: 初期ベースライン, 2: 待機ループ, 3: サンプリング中
    int k = 0;    // サンプリングインデックス (1..9)
    std::vector<ThreadSamplingEntry> d; // d(n)
    bool samplingActive =
        false; // Step 3 サンプリング中 (タイマー加速/Decline制御用)
    bool isDecline = false;
    bool initialEvaluated = false;
  };
  std::unordered_map<DWORD, std::unordered_map<DWORD, ChromiumThreadTrackInfo>>
      m_chromiumThreadTracks;
  std::unordered_map<DWORD, ChromiumAudioState> m_chromiumAudioStates;
  std::unordered_set<DWORD> m_chromiumEvictedPids;

  // スレッド属性フラグ (キー: PID, 値: (キー: TID, 値: フラグ 1:timeclit救済))
  std::unordered_map<DWORD, std::unordered_map<DWORD, uint8_t>> m_threadFlags;

  // 永続オーディオスレッドトラッキング (キー: PID, 値: (キー: TID, 値:
  // スレッド表示名))
  std::unordered_map<DWORD, std::unordered_map<DWORD, std::string>>
      m_trackedAudioThreads;

  // スレッドCPU時間サンプリング (キー: PID, 値: (キー: TID, 値:
  // 前回計測の合計CPU時間))
  std::unordered_map<DWORD, std::unordered_map<DWORD, ULONGLONG>>
      m_prevThreadCpuTimes;

  struct ProcessSamplingState {
    std::unordered_map<DWORD, ULONG64> lastCycles; // サイクルタイム前回計測値
    std::unordered_map<DWORD, ULONGLONG> lastCpuTime;
    std::unordered_map<DWORD, ULONGLONG> accumulatedDelta; // 累積デルタ
    std::unordered_map<DWORD, ULONGLONG> latestDelta; // 直近ターンの瞬間デルタ
    std::unordered_set<DWORD>
        audioCandidateTids; // オーディオ関連と判定された候補 TID
    std::unordered_set<DWORD> inspectedTids; // スタック検査済み TID (生涯1回)
  };
  std::unordered_map<DWORD, ProcessSamplingState> m_samplingStates;

  // ── スレッド走査基本情報 ──
  struct LocalThreadInfo {
    DWORD tid;
    LONG basePri;
    int priority;
    std::string threadName;
    HANDLE hThread;
    DWORD_PTR currentAffinity;
  };

  // 候補 6: Phase 1 / Phase 3 スレッド探索・確定共通ルーチン
  bool TryIdentifyAudioThread(ProcessRule &rule, ProcessSamplingState &sampleState,
                              const std::vector<LocalThreadInfo> &threadInfos,
                              HANDLE hProcess, DWORD pid, DWORD_PTR audioMask,
                              int intervalMs, const char *phaseName,
                              DWORD &identifiedAudioTid,
                              std::string &identifiedAudioLabel,
                              bool &stateChanged);

  // スレッドスナップショット遅延用キャッシュ (キー: PID)
  std::unordered_map<DWORD, std::vector<DWORD>> m_cachedProcessTids;
  std::unordered_map<DWORD, int> m_lastProcessThreadCount;

public:
  // ヒューリスティック探索中 (500ms タイマー要求中) かどうか判定
  bool IsHeuristicsActive() const;
  // 250ms 高速検証タイマー要求中かどうか判定
  bool IsFastPollingRequired() const;
  // 動的タイマー要求周期 (ms) を取得
  UINT GetDesiredPollingIntervalMs() const;
};

} // namespace ati
