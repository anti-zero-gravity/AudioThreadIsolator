#pragma once

#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>

namespace ati {

// コールバック型: ユーザーが Rank (1..49) をクリック決定したときに呼び出される
using PrioritySelectedCallback = std::function<void(int rank)>;

// GDI+ のライフサイクル管理
void InitializeGdiPlus();
void ShutdownGdiPlus();

// ピッカーウィンドウを親ダイアログに重ねて表示する
// hParent: 親ウィンドウ (メインダイアログ HWND)
// initialRank: 初期選択 Rank (1..49)
// onSelected: 決定時のコールバック
HWND ShowPriorityMatrixPicker(HWND hParent, int initialRank, PrioritySelectedCallback onSelected);

// ピッカーの開閉状態確認および手動クローズ
bool IsPriorityMatrixPickerOpen();
void ClosePriorityMatrixPicker();

} // namespace ati
