#pragma once
#ifndef ATI_DPI_UTILS_H
#define ATI_DPI_UTILS_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ati {

// ── DPI スケール取得ヘルパー (Per-Monitor V2 対応 / 96 DPI = 1.0f) ──
inline float GetDpiScaleForWindow(HWND hWnd) {
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        PFN_GetDpiForWindow pfn = (PFN_GetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
        if (pfn) {
            UINT dpi = pfn(hWnd);
            if (dpi > 0) return dpi / 96.0f;
        }
    }
    HDC hdc = GetDC(hWnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hWnd, hdc);
    if (dpi <= 0) dpi = 96;
    return dpi / 96.0f;
}

// ── スケール整数変換ヘルパー ──
inline int ScaleI(int base, float s) {
    return static_cast<int>(base * s);
}

// ── フォント取得ヘルパー (WM_GETFONT -> DEFAULT_GUI_FONT フォールバック) ──
inline HFONT GetEffectiveFont(HWND hWnd) {
    HFONT hFont = reinterpret_cast<HFONT>(SendMessageA(hWnd, WM_GETFONT, 0, 0));
    return hFont ? hFont : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// ── DPI 基準レイアウト定数 (scale = 1.0 時のピクセル値) ──
namespace Layout {
    // ── マージン・間隔 ──
    constexpr int kMainMargin        = 10;   // メインダイアログ外枠マージン
    constexpr int kDialogMargin      = 14;   // 子ダイアログ外枠マージン
    constexpr int kBtnSpacing        =  4;   // ボタン間スペーサー
    constexpr int kCtrlSpacing       =  8;   // コントロール間スペーサー
    constexpr int kChkTopPad         =  6;   // チェックボックス下余白

    // ── ボタン・コントロール寸法 ──
    constexpr int kBtnW              = 80;
    constexpr int kBtnH              = 20;
    constexpr int kChkW              = 105;
    constexpr int kChkH              = 22;

    // ── パネル・ヘッダー ──
    constexpr int kRightColW         = 115;  // 右パネル幅
    constexpr int kBottomLegendH     = 18;   // 下部凡例高さ

    // ── アフィニティテーブル ──
    constexpr int kAffinityItemColW  = 130;  // Item 列幅
    constexpr int kAffinityCoreColW  =  34;  // コア列幅
    constexpr int kCheckboxSize      =  13;  // チェックボックスサイズ

    // ── ダイアログ位置・間隔 ──
    constexpr int kPickerOffset      = 40;   // MatrixPicker 画面中央方向オフセット
    constexpr int kPerAppGap         =  4;   // PerApp ダイアログ隣接間隔

    // ── フォント ──
    constexpr int kFontPt            =  9;   // メインフォントのポイントサイズ

    // ── 最小ウィンドウサイズ ──
    constexpr int kPickerMinW        = 460;
    constexpr int kPickerMinH        = 250;
    constexpr int kPerAppMinW        = 400;
    constexpr int kPerAppMinH        = 260;

    // ── 凡例・インジケータ ──
    constexpr int kDotSize           =  8;
    constexpr int kLegendSpacing     = 12;
    constexpr int kBarW              =  3;   // 優先度インジケータ幅
    constexpr int kBarH              =  9;
    constexpr int kBarGap            =  2;
}

} // namespace ati

#endif // ATI_DPI_UTILS_H
