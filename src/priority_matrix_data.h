// Auto-generated 42 Priority Matrix Rank Table (6x7) for ATI (Pure Win32 GDI/GDI+)
#pragma once

#include <windows.h>

namespace ati {

// Fixed thread priority (KPRIORITY 1..31) encoding helpers
#ifndef ATI_FIXED_PRIO_HELPERS
#define ATI_FIXED_PRIO_HELPERS
inline bool IsFixedThreadPriority(int v) { return v >= 101 && v <= 131; }
inline int DecodeFixedThreadPriority(int v) { return v - 100; }
inline int EncodeFixedThreadPriority(int p) { return 100 + p; }
#endif

inline const char* GetProcessPriorityClassName(DWORD pClass, const char* idleLabel = "Idle") {
    switch (pClass) {
    case REALTIME_PRIORITY_CLASS:     return "Realtime";
    case HIGH_PRIORITY_CLASS:         return "High";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "Above Normal";
    case NORMAL_PRIORITY_CLASS:       return "Normal";
    case BELOW_NORMAL_PRIORITY_CLASS: return "Below Normal";
    case IDLE_PRIORITY_CLASS:         return idleLabel;
    default:                          return "Normal";
    }
}

struct PriorityMatrixEntry {
    int rank;                 // 1 .. 42
    int row;                  // 0 .. 5
    int col;                  // 0 .. 6
    DWORD processPriorityClass; // IDLE_PRIORITY_CLASS, etc.
    int threadPriority;       // THREAD_PRIORITY_NORMAL, etc.
    int basePriority;         // 1 .. 31
    const char* processLabel;
    const char* threadLabel;
    const char* formulaStr;   // e.g. "8 + (+1) = 9"
};

static constexpr int kPriorityMatrixEntryCount = 42;

static const PriorityMatrixEntry kPriorityMatrixEntries[kPriorityMatrixEntryCount] = {
    { 1, 0, 0, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Idle", "Idle (-15)", "4 + (-15) = 1" },
    { 2, 1, 0, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Below Normal", "Idle (-15)", "6 + (-15) = 1" },
    { 3, 2, 0, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Normal", "Idle (-15)", "8 + (-15) = 1" },
    { 4, 3, 0, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Above Normal", "Idle (-15)", "10 + (-15) = 1" },
    { 5, 4, 0, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "High", "Idle (-15)", "13 + (-15) = 1" },
    { 6, 0, 1, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 2, "Idle", "Lowest (-2)", "4 + (-2) = 2" },
    { 7, 0, 2, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 3, "Idle", "Below Normal (-1)", "4 + (-1) = 3" },
    { 8, 0, 3, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 4, "Idle", "Normal (0)", "4 + (+0) = 4" },
    { 9, 1, 1, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 4, "Below Normal", "Lowest (-2)", "6 + (-2) = 4" },
    { 10, 0, 4, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 5, "Idle", "Above Normal (+1)", "4 + (+1) = 5" },
    { 11, 1, 2, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 5, "Below Normal", "Below Normal (-1)", "6 + (-1) = 5" },
    { 12, 0, 5, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 6, "Idle", "Highest (+2)", "4 + (+2) = 6" },
    { 13, 1, 3, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 6, "Below Normal", "Normal (0)", "6 + (+0) = 6" },
    { 14, 2, 1, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 6, "Normal", "Lowest (-2)", "8 + (-2) = 6" },
    { 15, 1, 4, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 7, "Below Normal", "Above Normal (+1)", "6 + (+1) = 7" },
    { 16, 2, 2, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 7, "Normal", "Below Normal (-1)", "8 + (-1) = 7" },
    { 17, 1, 5, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 8, "Below Normal", "Highest (+2)", "6 + (+2) = 8" },
    { 18, 2, 3, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 8, "Normal", "Normal (0)", "8 + (+0) = 8" },
    { 19, 3, 1, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 8, "Above Normal", "Lowest (-2)", "10 + (-2) = 8" },
    { 20, 2, 4, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 9, "Normal", "Above Normal (+1)", "8 + (+1) = 9" },
    { 21, 3, 2, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 9, "Above Normal", "Below Normal (-1)", "10 + (-1) = 9" },
    { 22, 2, 5, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 10, "Normal", "Highest (+2)", "8 + (+2) = 10" },
    { 23, 3, 3, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 10, "Above Normal", "Normal (0)", "10 + (+0) = 10" },
    { 24, 3, 4, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 11, "Above Normal", "Above Normal (+1)", "10 + (+1) = 11" },
    { 25, 4, 1, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 11, "High", "Lowest (-2)", "13 + (-2) = 11" },
    { 26, 3, 5, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 12, "Above Normal", "Highest (+2)", "10 + (+2) = 12" },
    { 27, 4, 2, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 12, "High", "Below Normal (-1)", "13 + (-1) = 12" },
    { 28, 4, 3, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 13, "High", "Normal (0)", "13 + (+0) = 13" },
    { 29, 4, 4, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 14, "High", "Above Normal (+1)", "13 + (+1) = 14" },
    { 30, 0, 6, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Idle", "Time Critical (+15)", "4 + (+15) = 15" },
    { 31, 1, 6, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Below Normal", "Time Critical (+15)", "6 + (+15) = 15" },
    { 32, 2, 6, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Normal", "Time Critical (+15)", "8 + (+15) = 15" },
    { 33, 3, 6, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Above Normal", "Time Critical (+15)", "10 + (+15) = 15" },
    { 34, 4, 5, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 15, "High", "Highest (+2)", "13 + (+2) = 15" },
    { 35, 4, 6, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "High", "Time Critical (+15)", "13 + (+15) = 15" },
    { 36, 5, 0, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 16, "Realtime", "Idle (-15)", "24 + (-15) = 16" },
    { 37, 5, 1, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 22, "Realtime", "Lowest (-2)", "24 + (-2) = 22" },
    { 38, 5, 2, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 23, "Realtime", "Below Normal (-1)", "24 + (-1) = 23" },
    { 39, 5, 3, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 24, "Realtime", "Normal (0)", "24 + (+0) = 24" },
    { 40, 5, 4, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 25, "Realtime", "Above Normal (+1)", "24 + (+1) = 25" },
    { 41, 5, 5, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 26, "Realtime", "Highest (+2)", "24 + (+2) = 26" },
    { 42, 5, 6, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 31, "Realtime", "Time Critical (+15)", "24 + (+15) = 31" },
};

inline const PriorityMatrixEntry* GetMatrixEntryByRank(int rank) {
    if (rank >= 1 && rank <= kPriorityMatrixEntryCount) return &kPriorityMatrixEntries[rank - 1];
    return nullptr;
}

inline const PriorityMatrixEntry* GetMatrixEntryByRowCol(int row, int col) {
    for (int i = 0; i < kPriorityMatrixEntryCount; ++i) {
        if (kPriorityMatrixEntries[i].row == row && kPriorityMatrixEntries[i].col == col) {
            return &kPriorityMatrixEntries[i];
        }
    }
    return nullptr;
}

inline int FindRankByPrio(DWORD procClass, int threadPrio) {
    for (int i = 0; i < kPriorityMatrixEntryCount; ++i) {
        if (kPriorityMatrixEntries[i].processPriorityClass == procClass &&
            kPriorityMatrixEntries[i].threadPriority == threadPrio) {
            return kPriorityMatrixEntries[i].rank;
        }
    }
    // Default fallback: match thread priority with NORMAL_PRIORITY_CLASS
    for (int i = 0; i < kPriorityMatrixEntryCount; ++i) {
        if (kPriorityMatrixEntries[i].threadPriority == threadPrio &&
            kPriorityMatrixEntries[i].processPriorityClass == NORMAL_PRIORITY_CLASS) {
            return kPriorityMatrixEntries[i].rank;
        }
    }
    return 18; // Rank 18: Normal x Normal (0) = Base 8
}

static constexpr int kProcessBasePriorities[6] = { 4, 6, 8, 10, 13, 24 };
static constexpr int kColumnThreadPriorities[7] = {
    THREAD_PRIORITY_IDLE,
    THREAD_PRIORITY_LOWEST,
    THREAD_PRIORITY_BELOW_NORMAL,
    THREAD_PRIORITY_NORMAL,
    THREAD_PRIORITY_ABOVE_NORMAL,
    THREAD_PRIORITY_HIGHEST,
    THREAD_PRIORITY_TIME_CRITICAL
};

inline int GetMatrixBasePriority(int row, int col) {
    const PriorityMatrixEntry* e = GetMatrixEntryByRowCol(row, col);
    return e ? e->basePriority : 8;
}

inline const char* GetRowProcessLabel(int row) {
    const PriorityMatrixEntry* e = GetMatrixEntryByRowCol(row, 3);
    return e ? e->processLabel : "Normal";
}

inline const char* GetColThreadLabel(int col) {
    const PriorityMatrixEntry* e = GetMatrixEntryByRowCol(2, col);
    return e ? e->threadLabel : "Normal (0)";
}

inline int ProcessClassToRow(DWORD procClass) {
    for (int i = 0; i < kPriorityMatrixEntryCount; ++i) {
        if (kPriorityMatrixEntries[i].processPriorityClass == procClass) {
            return kPriorityMatrixEntries[i].row;
        }
    }
    return 2;
}

inline DWORD RowToProcessClass(int row) {
    const PriorityMatrixEntry* e = GetMatrixEntryByRowCol(row, 0);
    return e ? e->processPriorityClass : NORMAL_PRIORITY_CLASS;
}

inline int ThreadPrioToCol(int threadPrio) {
    for (int c = 0; c < 7; ++c) {
        if (kColumnThreadPriorities[c] == threadPrio) return c;
    }
    return 3;
}

} // namespace ati
