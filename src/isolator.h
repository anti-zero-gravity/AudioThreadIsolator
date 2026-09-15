#pragma once

#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

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
    std::string processName;          // 実行ファイル名 (例: "mpv.exe")
    int audioCore;                    // 隔離先コア番号 (代表コア #0 始まり、例: 3)
    DWORD_PTR audioAffinityMask;      // 隔離先オーディオコアマスク (0: audioCoreから単一生成、複数コア指定可)
    int audioPriority;                // オーディオスレッド適用優先度 (デフォルト: -15 / THREAD_PRIORITY_IDLE)
    DWORD_PTR normalAffinityMask = 0; // 通常スレッド退避先マスク (0: Global設定のnormalAffinityMaskを自動継承)
    DWORD targetPriority;             // 特定優先度フィルタ (0: 指定なし/自動判定)
    DWORD processPriorityClass;       // プロセス優先度 (0: 変更なし)
    
    // 監視除外・ペンディングフラグ (チェックONで監視・アイソレートから除外、デフォルト: false)
    bool isBypassed = false;
    // 個別ヒューリスティック探索フラグ (デフォルト: true)
    bool enableHeuristics = true;

    // リアルタイム動的状態 (GUI表示用)
    DWORD applyCount = 0;                 // 累計再生スレッド隔離回数
    int currentThreadCount = 0;           // 検出された総スレッド数 (アフィニティ制御対象スレッド規模)
    std::string detectedThreadName = "";  // 検出された再生スレッド名 (例: "ao/wasapi")
    DWORD activePid = 0;                  // 検出中のプロセスID (0: 未起動)
    DWORD activeAudioTid = 0;             // 検出・隔離中のオーディオスレッドID
    bool isAudioThreadSuspended = false;  // オーディオスレッド手動一時停止(テスト検証)中か
    bool isRunning = false;               // 現在稼働中か
    bool isAudioIsolated = false;         // オーディオスレッド検出・通常スレッド退避完了か
    bool hasIntruderThreads = false;      // オーディオ専有コアへの侵入・同居スレッドを検知・制圧中か
};


// 全体設定
struct GlobalConfig {
    int defaultAudioCore;             // デフォルト隔離コア (代表コア #0 始まり)
    DWORD_PTR defaultAudioAffinityMask; // デフォルト隔離オーディオコアマスク (0: defaultAudioCoreから単一生成)
    int defaultAudioPriority;         // デフォルトオーディオ優先度 (-15: THREAD_PRIORITY_IDLE)
    DWORD_PTR normalAffinityMask;     // 通常スレッド退避先マスク (0: 隔離コア以外の全コア自動)
    int pollingIntervalMs;            // 監視周期 (デフォルト 500ms)
    bool enableHeuristics = false;    // 非MMCSSスレッド探索用ヒューリスティック監視有効化 (デフォルト: false)
    std::vector<ProcessRule> rules;   // 監視プロセス一覧
};

class ThreadIsolator {
public:
    ThreadIsolator();
    ~ThreadIsolator();

    void Initialize(const GlobalConfig& config);
    void UpdateConfig(const GlobalConfig& config);
    GlobalConfig GetConfig();

    // 500ms タイマーごとにメインループから呼び出される走査・隔離関数
    bool ScanAndIsolate();

    // ルール管理
    void AddRule(const ProcessRule& rule);
    void RemoveRule(size_t index);
    void UpdateRule(size_t index, const ProcessRule& rule);
    void ToggleProcessHeuristics(size_t index);
    void ToggleProcessBypass(size_t index);
    bool ToggleSuspendAudioThread(size_t index);
    void ResumeAllSuspendedThreads();
    std::vector<ProcessRule> GetRulesSnapshot();

    // システム情報ヘルパー
    static int GetSystemCoreCount();
    static DWORD_PTR GetFullCoreMask(int coreCount);
    static DWORD_PTR MakeCoreMask(int coreIndex);
    static DWORD_PTR MakeDefaultNormalMask(int coreCount, int isolatedCore);

    // ヒューリスティック状態テキスト取得 (GUI ステータスバー表示用)
    std::string GetHeuristicsStatusText() const;

private:
    mutable std::mutex m_mutex;
    GlobalConfig m_config;
    std::string m_heuristicsStatusText;
    
    // キー: TID, 値: 適用済みマスク
    std::unordered_map<DWORD, DWORD_PTR> m_appliedThreads;

    // 永続オーディオスレッドトラッキング (キー: PID, 値: (キー: TID, 値: スレッド表示名))
    std::unordered_map<DWORD, std::unordered_map<DWORD, std::string>> m_trackedAudioThreads;

    // スレッドCPU時間サンプリング (キー: PID, 値: (キー: TID, 値: 前回計測の合計CPU時間))
    std::unordered_map<DWORD, std::unordered_map<DWORD, ULONGLONG>> m_prevThreadCpuTimes;

    // 10秒間サンプリング状態保持 (キー: PID)
    struct ProcessSamplingState {
        int sampleTurns = 0; // 0 〜 20 ターン (約10秒間)
        std::unordered_map<DWORD, ULONGLONG> lastCpuTime;
        std::unordered_map<DWORD, ULONGLONG> accumulatedDelta;
    };
    std::unordered_map<DWORD, ProcessSamplingState> m_samplingStates;

    // GetThreadDescription 関数ポインタ
    typedef HRESULT(WINAPI* PFN_GetThreadDescription)(HANDLE, PWSTR*);
    PFN_GetThreadDescription m_pfnGetThreadDescription;

    std::string QueryThreadNameA(HANDLE hThread);
    bool IsNamedAudioThread(const std::string& threadName);
};

} // namespace ati
