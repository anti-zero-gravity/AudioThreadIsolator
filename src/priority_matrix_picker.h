#pragma once

#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>

namespace ati {

enum class PickerMode {
    ThreadCol2 = 0,
    ProcessCol3 = 1
};

struct PriorityPickerResult {
    bool applyProcess;
    DWORD processPriorityClass;
    bool applyThread;
    int threadPriority; // relative (-15..+15) or Fixed (101..131)
    bool resetToUnattached; // true when clicking outside pillars or clicking right pane without wheeling
};

using PriorityPickerCallback = std::function<void(const PriorityPickerResult&)>;

// Fixed thread priority (KPRIORITY 1..31) encoding helpers
#ifndef ATI_FIXED_PRIO_HELPERS
#define ATI_FIXED_PRIO_HELPERS
inline bool IsFixedThreadPriority(int v) { return v >= 101 && v <= 131; }
inline int DecodeFixedThreadPriority(int v) { return v - 100; }
inline int EncodeFixedThreadPriority(int p) { return 100 + p; }
#endif

// GDI+ のライフサイクル管理
void InitializeGdiPlus();
void ShutdownGdiPlus();

// ピッカーウィンドウを親ダイアログに重ねて表示する
HWND ShowPriorityMatrixPicker(
    HWND hParent,
    PickerMode mode,
    DWORD initialProcessClass,
    int initialThreadPrio,
    bool isAdmin,
    PriorityPickerCallback onSelected
);

// ピッカーの開閉状態確認および手動クローズ
bool IsPriorityMatrixPickerOpen();
void ClosePriorityMatrixPicker();

} // namespace ati
