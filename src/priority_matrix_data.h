// Auto-generated 49 Priority Matrix Rank Table for ATI (Pure Win32 GDI/GDI+)
#pragma once

#include <windows.h>

namespace ati {

struct PriorityMatrixEntry {
    int rank;                 // 1 .. 49
    int row;                  // 0 .. 6
    int col;                  // 0 .. 6
    DWORD processPriorityClass; // IDLE_PRIORITY_CLASS, etc.
    int threadPriority;       // THREAD_PRIORITY_NORMAL, etc.
    int basePriority;         // 1 .. 31
    const char* processLabel;
    const char* threadLabel;
    const char* formulaStr;   // e.g. "8 + (+1) = 9"
};

static const PriorityMatrixEntry kPriorityMatrixEntries[49] = {
    { 1, 0, 0, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Idle", "Idle (-15)", "4 + (-15) = 1" },
    { 2, 1, 0, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Below Normal", "Idle (-15)", "6 + (-15) = 1" },
    { 3, 2, 0, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Normal (Background)", "Idle (-15)", "8 + (-15) = 1" },
    { 4, 3, 0, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Normal (Foreground)", "Idle (-15)", "8 + (-15) = 1" },
    { 5, 4, 0, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "Above Normal", "Idle (-15)", "10 + (-15) = 1" },
    { 6, 5, 0, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 1, "High", "Idle (-15)", "13 + (-15) = 1" },
    { 7, 0, 1, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 2, "Idle", "Lowest (-2)", "4 + (-2) = 2" },
    { 8, 0, 2, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 3, "Idle", "Below Normal (-1)", "4 + (-1) = 3" },
    { 9, 0, 3, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 4, "Idle", "Normal (0)", "4 + (+0) = 4" },
    { 10, 1, 1, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 4, "Below Normal", "Lowest (-2)", "6 + (-2) = 4" },
    { 11, 0, 4, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 5, "Idle", "Above Normal (+1)", "4 + (+1) = 5" },
    { 12, 1, 2, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 5, "Below Normal", "Below Normal (-1)", "6 + (-1) = 5" },
    { 13, 2, 1, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 5, "Normal (Background)", "Lowest (-2)", "8 + (-2) = 5" },
    { 14, 0, 5, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 6, "Idle", "Highest (+2)", "4 + (+2) = 6" },
    { 15, 1, 3, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 6, "Below Normal", "Normal (0)", "6 + (+0) = 6" },
    { 16, 2, 2, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 6, "Normal (Background)", "Below Normal (-1)", "8 + (-1) = 6" },
    { 17, 1, 4, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 7, "Below Normal", "Above Normal (+1)", "6 + (+1) = 7" },
    { 18, 2, 3, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 7, "Normal (Background)", "Normal (0)", "8 + (+0) = 7" },
    { 19, 3, 1, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 7, "Normal (Foreground)", "Lowest (-2)", "8 + (-2) = 7" },
    { 20, 1, 5, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 8, "Below Normal", "Highest (+2)", "6 + (+2) = 8" },
    { 21, 2, 4, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 8, "Normal (Background)", "Above Normal (+1)", "8 + (+1) = 8" },
    { 22, 3, 2, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 8, "Normal (Foreground)", "Below Normal (-1)", "8 + (-1) = 8" },
    { 23, 4, 1, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 8, "Above Normal", "Lowest (-2)", "10 + (-2) = 8" },
    { 24, 2, 5, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 9, "Normal (Background)", "Highest (+2)", "8 + (+2) = 9" },
    { 25, 3, 3, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 9, "Normal (Foreground)", "Normal (0)", "8 + (+0) = 9" },
    { 26, 4, 2, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 9, "Above Normal", "Below Normal (-1)", "10 + (-1) = 9" },
    { 27, 3, 4, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 10, "Normal (Foreground)", "Above Normal (+1)", "8 + (+1) = 10" },
    { 28, 4, 3, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 10, "Above Normal", "Normal (0)", "10 + (+0) = 10" },
    { 29, 3, 5, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 11, "Normal (Foreground)", "Highest (+2)", "8 + (+2) = 11" },
    { 30, 4, 4, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 11, "Above Normal", "Above Normal (+1)", "10 + (+1) = 11" },
    { 31, 5, 1, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 11, "High", "Lowest (-2)", "13 + (-2) = 11" },
    { 32, 4, 5, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 12, "Above Normal", "Highest (+2)", "10 + (+2) = 12" },
    { 33, 5, 2, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 12, "High", "Below Normal (-1)", "13 + (-1) = 12" },
    { 34, 5, 3, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 13, "High", "Normal (0)", "13 + (+0) = 13" },
    { 35, 5, 4, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 14, "High", "Above Normal (+1)", "13 + (+1) = 14" },
    { 36, 0, 6, IDLE_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Idle", "Time Critical (+15)", "4 + (+15) = 15" },
    { 37, 1, 6, BELOW_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Below Normal", "Time Critical (+15)", "6 + (+15) = 15" },
    { 38, 2, 6, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Normal (Background)", "Time Critical (+15)", "8 + (+15) = 15" },
    { 39, 3, 6, NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Normal (Foreground)", "Time Critical (+15)", "8 + (+15) = 15" },
    { 40, 4, 6, ABOVE_NORMAL_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "Above Normal", "Time Critical (+15)", "10 + (+15) = 15" },
    { 41, 5, 5, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 15, "High", "Highest (+2)", "13 + (+2) = 15" },
    { 42, 5, 6, HIGH_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 15, "High", "Time Critical (+15)", "13 + (+15) = 15" },
    { 43, 6, 0, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_IDLE, 16, "Realtime", "Idle (-15)", "24 + (-15) = 16" },
    { 44, 6, 1, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_LOWEST, 22, "Realtime", "Lowest (-2)", "24 + (-2) = 22" },
    { 45, 6, 2, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_BELOW_NORMAL, 23, "Realtime", "Below Normal (-1)", "24 + (-1) = 23" },
    { 46, 6, 3, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_NORMAL, 24, "Realtime", "Normal (0)", "24 + (+0) = 24" },
    { 47, 6, 4, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_ABOVE_NORMAL, 25, "Realtime", "Above Normal (+1)", "24 + (+1) = 25" },
    { 48, 6, 5, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_HIGHEST, 26, "Realtime", "Highest (+2)", "24 + (+2) = 26" },
    { 49, 6, 6, REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL, 31, "Realtime", "Time Critical (+15)", "24 + (+15) = 31" },
};

inline const PriorityMatrixEntry* GetMatrixEntryByRank(int rank) {
    if (rank >= 1 && rank <= 49) return &kPriorityMatrixEntries[rank - 1];
    return nullptr;
}

inline int FindRankByPrio(DWORD procClass, int threadPrio) {
    for (int i = 0; i < 49; ++i) {
        if (kPriorityMatrixEntries[i].processPriorityClass == procClass &&
            kPriorityMatrixEntries[i].threadPriority == threadPrio) {
            return kPriorityMatrixEntries[i].rank;
        }
    }
    // Default fallback: match thread priority with NORMAL_PRIORITY_CLASS
    for (int i = 0; i < 49; ++i) {
        if (kPriorityMatrixEntries[i].threadPriority == threadPrio &&
            kPriorityMatrixEntries[i].processPriorityClass == NORMAL_PRIORITY_CLASS) {
            return kPriorityMatrixEntries[i].rank;
        }
    }
    return 19; // Rank 19: Normal (BG) x Normal (0) = Base 8
}

} // namespace ati
