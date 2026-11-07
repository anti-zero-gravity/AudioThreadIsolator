#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <cmath>
#include <algorithm>

#include "priority_matrix_picker.h"
#include "priority_matrix_data.h"
#include "resource.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

namespace ati {

static ULONG_PTR s_gdiplusToken = 0;
static HWND s_hPickerWnd = nullptr;
static PriorityPickerCallback s_onSelectedCallback = nullptr;
static PickerMode s_pickerMode = PickerMode::ThreadCol2;
static bool s_isAdmin = true;
static float s_scale = 1.0f;

// Committed & live hover/wheel state
static int s_committedRow = 2;       // 0=Idle, 1=Below, 2/3=Normal, 4=Above, 5=High, 6=Realtime
static int s_committedCol = 3;       // 0..6 when relative, -1 when Fixed
static int s_committedFixedPrio = 0; // 1..31 when Fixed
static int s_hoveredRawRow = -1;
static int s_hoveredRawCol = -1;
static bool s_isCtrlPressed = false;
static bool s_isMouseOverLeftMatrix = false;
static bool s_wheelActive = false;
static int s_wheelPrio = 8;

static const int kPriorityMatrix[7][7] = {
    {  1,  2,  3,  4,  5,  6, 15 }, // Row 0: Idle (Base 4)
    {  1,  4,  5,  6,  7,  8, 15 }, // Row 1: Below Normal (Base 6)
    {  1,  5,  6,  7,  8,  9, 15 }, // Row 2: Normal BG (Base 7)
    {  1,  7,  8,  9, 10, 11, 15 }, // Row 3: Normal FG (Base 9)
    {  1,  8,  9, 10, 11, 12, 15 }, // Row 4: Above Normal (Base 10)
    {  1, 11, 12, 13, 14, 15, 15 }, // Row 5: High (Base 13)
    { 16, 22, 23, 24, 25, 26, 31 }  // Row 6: Realtime (Base 24)
};

static const int kProcessBases[7] = { 4, 6, 7, 9, 10, 13, 24 };
static const int kColThreadPrios[7] = {
    THREAD_PRIORITY_IDLE,
    THREAD_PRIORITY_LOWEST,
    THREAD_PRIORITY_BELOW_NORMAL,
    THREAD_PRIORITY_NORMAL,
    THREAD_PRIORITY_ABOVE_NORMAL,
    THREAD_PRIORITY_HIGHEST,
    THREAD_PRIORITY_TIME_CRITICAL
};

static const wchar_t* kRowProcessNames[7] = {
    L"Idle",
    L"Below Normal",
    L"Normal (Background)",
    L"Normal (Foreground)",
    L"Above Normal",
    L"High",
    L"Realtime"
};

static const wchar_t* kShortProcessNames[7] = {
    L"Idle",
    L"Below Normal",
    L"Normal",
    L"Normal",
    L"Above Normal",
    L"High",
    L"Realtime"
};

static const wchar_t* kColThreadNames[7] = {
    L"Idle (-15)",
    L"Lowest (-2)",
    L"Below Normal (-1)",
    L"Normal (0)",
    L"Above Normal (+1)",
    L"Highest (+2)",
    L"Time Critical (+15)"
};

static int ProcessClassToRow(DWORD procClass) {
    switch (procClass) {
    case IDLE_PRIORITY_CLASS:         return 0;
    case BELOW_NORMAL_PRIORITY_CLASS: return 1;
    case NORMAL_PRIORITY_CLASS:       return 2;
    case ABOVE_NORMAL_PRIORITY_CLASS: return 4;
    case HIGH_PRIORITY_CLASS:         return 5;
    case REALTIME_PRIORITY_CLASS:     return 6;
    default:                          return 2;
    }
}

static DWORD RowToProcessClass(int row) {
    switch (row) {
    case 0:  return IDLE_PRIORITY_CLASS;
    case 1:  return BELOW_NORMAL_PRIORITY_CLASS;
    case 2:
    case 3:  return NORMAL_PRIORITY_CLASS;
    case 4:  return ABOVE_NORMAL_PRIORITY_CLASS;
    case 5:  return HIGH_PRIORITY_CLASS;
    case 6:  return REALTIME_PRIORITY_CLASS;
    default: return NORMAL_PRIORITY_CLASS;
    }
}

static int ThreadPrioToCol(int threadPrio) {
    for (int c = 0; c < 7; ++c) {
        if (kColThreadPrios[c] == threadPrio) return c;
    }
    return 3;
}

static int ClampRowForAdmin(int r) {
    if (!s_isAdmin && r == 6) return 5;
    return r;
}

// 282 x 340 hit map raw binary buffer (95,880 bytes)
static std::vector<BYTE> s_hitMapData;
static const int HIT_MAP_W = 282;
static const int HIT_MAP_H = 340;

// GDI+ Image Cache
static std::unordered_map<int, Gdiplus::Image*> s_compositeCache;
static Gdiplus::Image* s_pBaseDarkImg = nullptr;
static std::wstring s_uiPartsDirW;

static void FindUiPartsDirectory() {
    if (!s_uiPartsDirW.empty()) return;

    char exePathA[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exePathA, MAX_PATH);
    PathRemoveFileSpecA(exePathA);

    std::string candidates[] = {
        std::string(exePathA) + "\\ui_parts",
        std::string(exePathA) + "\\demo\\ui_parts",
        "c:\\Users\\YK-PC\\.gemini\\antigravity-ide\\scratch\\ATI\\ui_parts",
        "c:\\Users\\YK-PC\\.gemini\\antigravity-ide\\scratch\\ATI\\demo\\ui_parts"
    };

    for (const auto& path : candidates) {
        std::string hitBin = path + "\\hit_map.bin";
        if (PathFileExistsA(hitBin.c_str())) {
            int len = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
            s_uiPartsDirW.resize(len - 1);
            MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &s_uiPartsDirW[0], len);
            break;
        }
    }
}

static Gdiplus::Image* LoadImageFromResource(int resId) {
    HINSTANCE hInst = GetModuleHandleA(nullptr);
    HRSRC hRsrc = FindResourceA(hInst, MAKEINTRESOURCEA(resId), RT_RCDATA);
    if (!hRsrc) return nullptr;

    HGLOBAL hRes = LoadResource(hInst, hRsrc);
    if (!hRes) return nullptr;

    LPVOID pData = LockResource(hRes);
    DWORD dwSize = SizeofResource(hInst, hRsrc);
    if (!pData || dwSize == 0) return nullptr;

    IStream* pStream = SHCreateMemStream(reinterpret_cast<const BYTE*>(pData), dwSize);
    if (!pStream) return nullptr;

    Gdiplus::Image* img = Gdiplus::Image::FromStream(pStream);
    pStream->Release();
    if (img && img->GetLastStatus() == Gdiplus::Ok) {
        return img;
    }
    if (img) delete img;
    return nullptr;
}

static bool LoadHitMapFromResource() {
    HINSTANCE hInst = GetModuleHandleA(nullptr);
    HRSRC hRsrc = FindResourceA(hInst, MAKEINTRESOURCEA(IDR_MATRIX_HITMAP), RT_RCDATA);
    if (!hRsrc) return false;

    HGLOBAL hRes = LoadResource(hInst, hRsrc);
    if (!hRes) return false;

    LPVOID pData = LockResource(hRes);
    DWORD dwSize = SizeofResource(hInst, hRsrc);
    if (!pData || dwSize == 0) return false;

    s_hitMapData.assign(reinterpret_cast<const BYTE*>(pData), reinterpret_cast<const BYTE*>(pData) + dwSize);
    return !s_hitMapData.empty();
}

static void LoadHitMapBin() {
    if (!s_hitMapData.empty()) return;
    if (LoadHitMapFromResource()) return;

    // フォールバック (外部ファイル探索)
    FindUiPartsDirectory();
    if (s_uiPartsDirW.empty()) return;

    std::wstring binPath = s_uiPartsDirW + L"\\hit_map.bin";
    std::ifstream ifs(binPath.c_str(), std::ios::binary | std::ios::ate);
    if (ifs.is_open()) {
        std::streamsize size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        s_hitMapData.resize(static_cast<size_t>(size));
        if (ifs.read(reinterpret_cast<char*>(s_hitMapData.data()), size)) {
            // loaded
        }
    }
}

static Gdiplus::Image* GetBaseDarkImage() {
    if (!s_pBaseDarkImg) {
        s_pBaseDarkImg = LoadImageFromResource(IDR_MATRIX_BASE_DARK);
        if (!s_pBaseDarkImg) {
            FindUiPartsDirectory();
            std::wstring p = s_uiPartsDirW + L"\\base_dark_layer.png";
            s_pBaseDarkImg = Gdiplus::Image::FromFile(p.c_str());
        }
    }
    return s_pBaseDarkImg;
}

static Gdiplus::Image* GetRowImage(int row) {
    int rowIdx = 2;
    if (row == 0) rowIdx = 0;
    else if (row == 1) rowIdx = 1;
    else if (row == 2 || row == 3) rowIdx = 2;
    else if (row == 4) rowIdx = 3;
    else if (row == 5) rowIdx = 4;
    else if (row == 6) rowIdx = 5;

    int cacheKey = IDR_MATRIX_ROW_BASE + rowIdx;
    auto it = s_compositeCache.find(cacheKey);
    if (it != s_compositeCache.end()) {
        return it->second;
    }

    Gdiplus::Image* img = LoadImageFromResource(cacheKey);
    if (img && img->GetLastStatus() == Gdiplus::Ok) {
        s_compositeCache[cacheKey] = img;
        return img;
    }

    FindUiPartsDirectory();
    wchar_t fname[64];
    swprintf(fname, 64, L"\\full_composites\\matrix_row_%d.png", rowIdx);
    std::wstring fullPath = s_uiPartsDirW + fname;
    img = Gdiplus::Image::FromFile(fullPath.c_str());
    if (img && img->GetLastStatus() == Gdiplus::Ok) {
        s_compositeCache[cacheKey] = img;
        return img;
    }
    return GetBaseDarkImage();
}

static Gdiplus::Image* GetCellImage(int row, int col) {
    if (col < 0 || col > 6 || row < 0 || row > 6) {
        return GetRowImage(row);
    }

    bool isNormalPair = (row == 2 || row == 3);
    if (isNormalPair) {
        int cacheKey = IDR_MATRIX_NORMAL_COL_BASE + col;
        auto it = s_compositeCache.find(cacheKey);
        if (it != s_compositeCache.end()) return it->second;

        Gdiplus::Image* img = LoadImageFromResource(cacheKey);
        if (img && img->GetLastStatus() == Gdiplus::Ok) {
            s_compositeCache[cacheKey] = img;
            return img;
        }

        FindUiPartsDirectory();
        wchar_t fname[64];
        swprintf(fname, 64, L"\\full_composites\\matrix_normal_col_%d.png", col);
        std::wstring fullPath = s_uiPartsDirW + fname;
        img = Gdiplus::Image::FromFile(fullPath.c_str());
        if (img && img->GetLastStatus() == Gdiplus::Ok) {
            s_compositeCache[cacheKey] = img;
            return img;
        }
        return GetRowImage(row);
    }

    const PriorityMatrixEntry* entry = GetMatrixEntryByRowCol(row, col);
    if (!entry) return GetRowImage(row);

    int rank = entry->rank;
    int cacheKey = IDR_MATRIX_RANK_BASE + rank;
    auto it = s_compositeCache.find(cacheKey);
    if (it != s_compositeCache.end()) return it->second;

    Gdiplus::Image* img = LoadImageFromResource(cacheKey);
    if (img && img->GetLastStatus() == Gdiplus::Ok) {
        s_compositeCache[cacheKey] = img;
        return img;
    }

    FindUiPartsDirectory();
    wchar_t fname[64];
    swprintf(fname, 64, L"\\full_composites\\matrix_rank_%02d.png", rank);
    std::wstring fullPath = s_uiPartsDirW + fname;
    img = Gdiplus::Image::FromFile(fullPath.c_str());
    if (img && img->GetLastStatus() == Gdiplus::Ok) {
        s_compositeCache[cacheKey] = img;
        return img;
    }
    return GetRowImage(row);
}

struct ActivePickerState {
    int displayRow;   // 0..6
    int displayCol;   // 0..6 or -1 when Fixed or Row-only
    int displayPrio;  // 1..31
    bool isFixed;     // true when priority is outside the 7 relative values of displayRow
    bool isRowOnly;   // true in ProcessCol3 mode
};

static ActivePickerState ResolveActiveState() {
    ActivePickerState st = {};
    if (s_pickerMode == PickerMode::ProcessCol3) {
        int targetRow = (s_hoveredRawRow >= 0) ? ClampRowForAdmin(s_hoveredRawRow) : s_committedRow;
        int repCol = (s_committedCol >= 0) ? s_committedCol : 3;
        st.displayRow = targetRow;
        st.displayCol = -1;
        st.displayPrio = kPriorityMatrix[targetRow][repCol];
        st.isFixed = false;
        st.isRowOnly = true;
        return st;
    }

    // ThreadCol2 Mode
    if (s_wheelActive) {
        int baseRow = (s_isCtrlPressed && s_hoveredRawRow >= 0) ? ClampRowForAdmin(s_hoveredRawRow) : s_committedRow;
        int matchedCol = -1;
        for (int c = 0; c < 7; ++c) {
            if (kPriorityMatrix[baseRow][c] == s_wheelPrio) {
                matchedCol = c;
                break;
            }
        }
        st.displayRow = baseRow;
        st.displayCol = matchedCol;
        st.displayPrio = s_wheelPrio;
        st.isFixed = (matchedCol == -1);
        st.isRowOnly = false;
        return st;
    }

    if (s_hoveredRawRow >= 0 && s_hoveredRawCol >= 0) {
        int activeRow = s_isCtrlPressed ? ClampRowForAdmin(s_hoveredRawRow) : s_committedRow;
        int activeCol = s_hoveredRawCol;
        st.displayRow = activeRow;
        st.displayCol = activeCol;
        st.displayPrio = kPriorityMatrix[activeRow][activeCol];
        st.isFixed = false;
        st.isRowOnly = false;
        return st;
    }

    // No hover: show current committed state
    int baseRow = s_committedRow;
    if (s_committedCol == -1 && s_committedFixedPrio > 0) {
        st.displayRow = baseRow;
        st.displayCol = -1;
        st.displayPrio = s_committedFixedPrio;
        st.isFixed = true;
        st.isRowOnly = false;
        return st;
    }

    int col = (s_committedCol >= 0) ? s_committedCol : 3;
    st.displayRow = baseRow;
    st.displayCol = col;
    st.displayPrio = kPriorityMatrix[baseRow][col];
    st.isFixed = false;
    st.isRowOnly = false;
    return st;
}

static void CommitAndClose(HWND hWnd) {
    ActivePickerState st = ResolveActiveState();
    PriorityPickerResult res = {};
    if (s_pickerMode == PickerMode::ProcessCol3) {
        res.applyProcess = true;
        res.processPriorityClass = RowToProcessClass(st.displayRow);
        res.applyThread = false;
        res.threadPriority = 0;
    } else {
        res.applyProcess = (s_isCtrlPressed && s_hoveredRawRow >= 0);
        res.processPriorityClass = RowToProcessClass(st.displayRow);
        res.applyThread = true;
        if (st.isFixed) {
            res.threadPriority = EncodeFixedThreadPriority(st.displayPrio);
        } else {
            int col = (st.displayCol >= 0 && st.displayCol <= 6) ? st.displayCol : 3;
            res.threadPriority = kColThreadPrios[col];
        }
    }

    if (s_onSelectedCallback) {
        s_onSelectedCallback(res);
    }
    DestroyWindow(hWnd);
}

static void ResetAndClose(HWND hWnd) {
    PriorityPickerResult res = {};
    res.resetToUnattached = true;
    if (s_onSelectedCallback) {
        s_onSelectedCallback(res);
    }
    DestroyWindow(hWnd);
}

void InitializeGdiPlus() {
    if (s_gdiplusToken == 0) {
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&s_gdiplusToken, &gdiplusStartupInput, nullptr);
    }
}

void ShutdownGdiPlus() {
    if (s_pBaseDarkImg) {
        delete s_pBaseDarkImg;
        s_pBaseDarkImg = nullptr;
    }
    for (auto& pair : s_compositeCache) {
        delete pair.second;
    }
    s_compositeCache.clear();
    s_hitMapData.clear();

    if (s_gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(s_gdiplusToken);
        s_gdiplusToken = 0;
    }
}

// ════════════════════════════════════════════════
// Window Procedure for Priority Matrix Picker
// ════════════════════════════════════════════════
static LRESULT CALLBACK PickerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        LoadHitMapBin();
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int clientW = rcClient.right - rcClient.left;
        int clientH = rcClient.bottom - rcClient.top;

        // Double buffering
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, clientW, clientH);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // Background: #090b10
        HBRUSH hBgBrush = CreateSolidBrush(RGB(9, 11, 16));
        FillRect(memDC, &rcClient, hBgBrush);
        DeleteObject(hBgBrush);

        Gdiplus::Graphics g(memDC);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        ActivePickerState st = ResolveActiveState();

        // 1. Draw 3D Matrix Image on Left Panel
        int pad = static_cast<int>(14 * s_scale);
        int imgW = static_cast<int>(HIT_MAP_W * s_scale);
        int imgH = static_cast<int>(HIT_MAP_H * s_scale);

        Gdiplus::Image* pImg = nullptr;
        if (st.isRowOnly || st.isFixed || st.displayCol < 0) {
            pImg = GetRowImage(st.displayRow);
        } else {
            pImg = GetCellImage(st.displayRow, st.displayCol);
        }

        if (pImg) {
            g.DrawImage(pImg, pad, pad, imgW, imgH);
        } else {
            Gdiplus::Pen penBorder(Gdiplus::Color(255, 60, 70, 90), 1.0f);
            g.DrawRectangle(&penBorder, pad, pad, imgW, imgH);
        }

        // Left canvas border frame
        Gdiplus::Pen framePen(Gdiplus::Color(50, 255, 255, 255), 1.0f);
        g.DrawRectangle(&framePen, pad - 1, pad - 1, imgW + 2, imgH + 2);

        // 2. Draw Right HUD Card
        int hudLeft = pad + imgW + static_cast<int>(14 * s_scale);
        int hudTop = pad;
        int hudW = clientW - hudLeft - pad;
        int hudH = imgH;

        // HUD Card Background (#0f131c)
        Gdiplus::SolidBrush cardBg(Gdiplus::Color(255, 15, 19, 28));
        g.FillRectangle(&cardBg, hudLeft, hudTop, hudW, hudH);

        Gdiplus::Pen cardBorder(Gdiplus::Color(255, 38, 46, 61), 1.0f);
        g.DrawRectangle(&cardBorder, hudLeft, hudTop, hudW, hudH);

        bool isNormalPair = (st.displayRow == 2 || st.displayRow == 3);
        int textPadX = hudLeft + static_cast<int>(16 * s_scale);
        int curY = hudTop + static_cast<int>(14 * s_scale);

        // Sub title
        Gdiplus::Font titleFont(L"Segoe UI", 11.0f * s_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 100, 116, 139));
        g.DrawString(L"CURRENT SELECTION", -1, &titleFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &titleBrush);

        curY += static_cast<int>(20 * s_scale);

        // Large Base Priority Hero Number (Pale green #9ae6b4 when relative in row, White when Fixed)
        wchar_t bpBuf[32];
        if (st.isRowOnly) {
            if (isNormalPair) {
                wcscpy_s(bpBuf, L"9 / 7");
            } else {
                swprintf(bpBuf, 32, L"%d", kProcessBases[st.displayRow]);
            }
        } else if (isNormalPair && !st.isFixed && st.displayCol >= 0) {
            int fgBp = kPriorityMatrix[3][st.displayCol];
            int bgBp = kPriorityMatrix[2][st.displayCol];
            if (fgBp != bgBp) {
                swprintf(bpBuf, 32, L"%d / %d", fgBp, bgBp);
            } else {
                swprintf(bpBuf, 32, L"%d", fgBp);
            }
        } else {
            swprintf(bpBuf, 32, L"%d", st.displayPrio);
        }

        Gdiplus::Font heroFont(L"Segoe UI", 42.0f * s_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Color heroColor = st.isFixed
            ? Gdiplus::Color(255, 255, 255, 255)
            : Gdiplus::Color(255, 154, 230, 180);
        Gdiplus::SolidBrush heroBrush(heroColor);
        g.DrawString(bpBuf, -1, &heroFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &heroBrush);

        // Fixed X position for Dynamic / Realtime aligned to "11 / 9" width so it never shifts horizontally
        Gdiplus::RectF refHeroBounds;
        g.MeasureString(L"11 / 9", -1, &heroFont, Gdiplus::PointF(0, 0), &refHeroBounds);
        float domainX = static_cast<float>(textPadX) + refHeroBounds.Width + 6.0f * s_scale;
        float domainY = static_cast<float>(curY) + 18.0f * s_scale;

        int checkPrio = st.isRowOnly ? kProcessBases[st.displayRow] : st.displayPrio;
        bool isRealtimeDomain = (checkPrio >= 16);
        const wchar_t* domainStr = isRealtimeDomain ? L"Realtime" : L"Dynamic";
        Gdiplus::Font domainFont(L"Segoe UI", 14.0f * s_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Color domainColor = isRealtimeDomain
            ? Gdiplus::Color(255, 252, 211, 77)
            : Gdiplus::Color(255, 154, 230, 180);
        Gdiplus::SolidBrush domainBrush(domainColor);
        g.DrawString(domainStr, -1, &domainFont, Gdiplus::PointF(domainX, domainY), &domainBrush);

        curY += static_cast<int>(50 * s_scale);

        // Separator line
        int innerW = hudW - static_cast<int>(32 * s_scale);
        Gdiplus::Pen sepPen(Gdiplus::Color(40, 255, 255, 255), 1.0f);
        g.DrawLine(&sepPen, textPadX, curY, textPadX + innerW, curY);

        curY += static_cast<int>(12 * s_scale);

        // Detail fields (strictly fixed Y coordinates across all states)
        Gdiplus::Font lblFont(L"Segoe UI", 11.5f * s_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font valFont(L"Segoe UI", 13.5f * s_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font monoFont(L"Consolas", 12.0f * s_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font hintFont(L"Segoe UI", 10.5f * s_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        Gdiplus::SolidBrush lblBrush(Gdiplus::Color(255, 148, 163, 184));
        Gdiplus::SolidBrush valBrush(Gdiplus::Color(255, 241, 245, 249));
        Gdiplus::SolidBrush greenAccentBrush(Gdiplus::Color(255, 154, 230, 180));
        Gdiplus::SolidBrush whiteFixedBrush(Gdiplus::Color(255, 255, 255, 255));

        int rowStepLbl = static_cast<int>(17 * s_scale);
        int rowStepVal = static_cast<int>(24 * s_scale);

        // Process Class
        g.DrawString(L"Process Class:", -1, &lblFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &lblBrush);
        curY += rowStepLbl;
        wchar_t pClassBuf[64];
        if (isNormalPair) {
            wcscpy_s(pClassBuf, L"Normal (Foreground / Background)");
        } else {
            wcscpy_s(pClassBuf, kRowProcessNames[st.displayRow]);
        }
        g.DrawString(pClassBuf, -1, &valFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &valBrush);

        curY += rowStepVal;

        // Thread Priority
        g.DrawString(L"Thread Priority:", -1, &lblFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &lblBrush);
        curY += rowStepLbl;
        wchar_t tPrioBuf[64];
        bool threadIsFixedStyle = false;
        if (st.isRowOnly) {
            if (s_committedCol == -1 && s_committedFixedPrio > 0) {
                swprintf(tPrioBuf, 64, L"Fixed (%d) [Unchanged]", s_committedFixedPrio);
                threadIsFixedStyle = true;
            } else {
                int c = (s_committedCol >= 0 && s_committedCol <= 6) ? s_committedCol : 3;
                swprintf(tPrioBuf, 64, L"%ls [Unchanged]", kColThreadNames[c]);
                threadIsFixedStyle = false;
            }
        } else if (st.isFixed) {
            swprintf(tPrioBuf, 64, L"Fixed (%d)", st.displayPrio);
            threadIsFixedStyle = true;
        } else {
            int c = (st.displayCol >= 0 && st.displayCol <= 6) ? st.displayCol : 3;
            wcscpy_s(tPrioBuf, kColThreadNames[c]);
            threadIsFixedStyle = false;
        }
        g.DrawString(tPrioBuf, -1, &valFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)),
                     threadIsFixedStyle ? &whiteFixedBrush : &greenAccentBrush);

        curY += rowStepVal;

        // Formula
        g.DrawString(L"Base Formula:", -1, &lblFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &lblBrush);
        curY += rowStepLbl;
        wchar_t formBuf[96];
        if (st.isRowOnly) {
            if (isNormalPair) {
                wcscpy_s(formBuf, L"Process Base Priority = 9 (FG) / 7 (BG)");
            } else {
                swprintf(formBuf, 96, L"Process Base Priority = %d", kProcessBases[st.displayRow]);
            }
        } else if (st.isFixed) {
            swprintf(formBuf, 96, L"Fixed KPRIORITY = %d (Process: %ls)", st.displayPrio, kShortProcessNames[st.displayRow]);
        } else {
            int c = (st.displayCol >= 0 && st.displayCol <= 6) ? st.displayCol : 3;
            int tr = kColThreadPrios[c];
            if (isNormalPair && kPriorityMatrix[3][c] != kPriorityMatrix[2][c]) {
                swprintf(formBuf, 96, L"FG: 9 + (%+d) = %d | BG: 7 + (%+d) = %d",
                         tr, kPriorityMatrix[3][c], tr, kPriorityMatrix[2][c]);
            } else {
                int pb = isNormalPair ? 8 : kProcessBases[st.displayRow];
                swprintf(formBuf, 96, L"%d + (%+d) = %d", pb, tr, kPriorityMatrix[st.displayRow][c]);
            }
        }
        Gdiplus::SolidBrush formBrush(Gdiplus::Color(255, 203, 213, 225));
        g.DrawString(formBuf, -1, &monoFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &formBrush);

        // Bottom Instructions (Fixed Y coordinate)
        int btmY = hudTop + hudH - static_cast<int>(22 * s_scale);

        // Normal Priority OS Behavior Note (9.2px font in fixed slot so nothing shifts vertically)
        if (isNormalPair) {
            int noteTop = curY + static_cast<int>(22 * s_scale);
            int noteH = btmY - noteTop - static_cast<int>(4 * s_scale);
            if (noteH > static_cast<int>(20 * s_scale)) {
                float noteLeft = static_cast<float>(textPadX) + 8.0f * s_scale;
                float noteWidth = static_cast<float>(innerW) - 8.0f * s_scale;
                Gdiplus::RectF noteRect(noteLeft, static_cast<float>(noteTop), noteWidth, static_cast<float>(noteH));
                Gdiplus::Font noteFont(L"Segoe UI", 9.2f * s_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush noteBrush(Gdiplus::Color(255, 196, 210, 228));
                const wchar_t* kNormalNote =
                    L"For Normal priority only, Windows automatically changes priority when an app is in front (Foreground) or behind (Background). Processes without a window always use the lower Background priority.";

                Gdiplus::RectF measuredNoteRect;
                g.MeasureString(kNormalNote, -1, &noteFont, noteRect, &measuredNoteRect);

                Gdiplus::Pen accentBarPen(Gdiplus::Color(200, 154, 230, 180), 2.0f * s_scale);
                g.DrawLine(&accentBarPen,
                           static_cast<float>(textPadX),
                           measuredNoteRect.Y + 2.0f * s_scale,
                           static_cast<float>(textPadX),
                           measuredNoteRect.Y + measuredNoteRect.Height - 2.0f * s_scale);

                g.DrawString(kNormalNote, -1, &noteFont, noteRect, nullptr, &noteBrush);
            }
        }

        Gdiplus::SolidBrush hintBrush(Gdiplus::Color(255, 100, 116, 139));
        const wchar_t* kHintText = (s_pickerMode == PickerMode::ProcessCol3)
            ? L"Click to apply Process Class ｜ [Esc] to cancel"
            : L"Click to apply ｜ [Ctrl]+Hover: 2D ｜ Wheel: 1-31";
        g.DrawString(kHintText, -1, &hintFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(btmY)), &hintBrush);

        // Blit to screen
        BitBlt(hdc, 0, 0, clientW, clientH, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mouseX = static_cast<short>(LOWORD(lParam));
        int mouseY = static_cast<short>(HIWORD(lParam));

        int pad = static_cast<int>(14 * s_scale);
        int imgW = static_cast<int>(HIT_MAP_W * s_scale);
        int imgH = static_cast<int>(HIT_MAP_H * s_scale);

        bool insideLeftMatrix = (mouseX >= pad && mouseX < pad + imgW && mouseY >= pad && mouseY < pad + imgH);
        bool needRedraw = false;

        if (insideLeftMatrix) {
            s_isMouseOverLeftMatrix = true;
            bool ctrlNow = ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
            if (s_isCtrlPressed != ctrlNow) {
                s_isCtrlPressed = ctrlNow;
                needRedraw = true;
            }

            float relX = (mouseX - pad) / s_scale;
            float relY = (mouseY - pad) / s_scale;
            int x = static_cast<int>(relX);
            int y = static_cast<int>(relY);
            int rankAtCursor = 0;
            if (x >= 0 && x < HIT_MAP_W && y >= 0 && y < HIT_MAP_H && !s_hitMapData.empty()) {
                rankAtCursor = s_hitMapData[y * HIT_MAP_W + x];
            }

            if (rankAtCursor > 0) {
                const PriorityMatrixEntry* entry = GetMatrixEntryByRank(rankAtCursor);
                if (entry) {
                    if (s_hoveredRawRow != entry->row || s_hoveredRawCol != entry->col || s_wheelActive) {
                        s_hoveredRawRow = entry->row;
                        s_hoveredRawCol = entry->col;
                        s_wheelActive = false;
                        needRedraw = true;
                    }
                }
                SetCursor(LoadCursor(nullptr, IDC_HAND));
            } else {
                if (!s_isCtrlPressed && (s_hoveredRawRow != -1 || s_hoveredRawCol != -1)) {
                    s_hoveredRawRow = -1;
                    s_hoveredRawCol = -1;
                    needRedraw = true;
                }
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
            }
        } else {
            // Outside left matrix (e.g. on right HUD card): lock s_isCtrlPressed and s_hoveredRawRow!
            if (s_isMouseOverLeftMatrix) {
                s_isMouseOverLeftMatrix = false;
                if (!s_isCtrlPressed && (s_hoveredRawRow != -1 || s_hoveredRawCol != -1)) {
                    s_hoveredRawRow = -1;
                    s_hoveredRawCol = -1;
                    needRedraw = true;
                }
            }
            int hudLeft = pad + imgW + static_cast<int>(14 * s_scale);
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            int hudW = (rcClient.right - rcClient.left) - hudLeft - pad;
            bool insideHud = (mouseX >= hudLeft && mouseX < hudLeft + hudW && mouseY >= pad && mouseY < pad + imgH);
            SetCursor(LoadCursor(nullptr, insideHud ? IDC_HAND : IDC_ARROW));
        }

        if (needRedraw) {
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (zDelta == 0) return 0;
        int dir = (zDelta > 0) ? 1 : -1;

        if (s_pickerMode == PickerMode::ProcessCol3) {
            int curRow = (s_hoveredRawRow >= 0) ? ClampRowForAdmin(s_hoveredRawRow) : s_committedRow;
            int nextRow = curRow + dir;
            if (dir > 0 && (curRow == 2 || curRow == 3)) nextRow = 4;
            if (dir < 0 && (curRow == 2 || curRow == 3)) nextRow = 1;
            int maxRow = s_isAdmin ? 6 : 5;
            nextRow = (std::max)(0, (std::min)(maxRow, nextRow));
            s_committedRow = nextRow;
            s_hoveredRawRow = nextRow;
            s_wheelActive = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        // ThreadCol2 Mode: step priority 1..31 (or 1..15 when non-admin)
        if (!s_wheelActive) {
            ActivePickerState cur = ResolveActiveState();
            s_wheelPrio = cur.displayPrio;
            s_wheelActive = true;
        }
        int maxPrio = s_isAdmin ? 31 : 15;
        s_wheelPrio = (std::max)(1, (std::min)(maxPrio, s_wheelPrio + dir));
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int mouseX = static_cast<short>(LOWORD(lParam));
        int mouseY = static_cast<short>(HIWORD(lParam));

        int pad = static_cast<int>(14 * s_scale);
        int imgW = static_cast<int>(HIT_MAP_W * s_scale);
        int imgH = static_cast<int>(HIT_MAP_H * s_scale);
        int hudLeft = pad + imgW + static_cast<int>(14 * s_scale);
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int hudW = (rcClient.right - rcClient.left) - hudLeft - pad;

        bool clickedHud = (mouseX >= hudLeft && mouseX < hudLeft + hudW && mouseY >= pad && mouseY < pad + imgH);
        bool insideLeftMatrix = (mouseX >= pad && mouseX < pad + imgW && mouseY >= pad && mouseY < pad + imgH);
        int rankAtCursor = 0;
        if (insideLeftMatrix && !s_hitMapData.empty()) {
            float relX = (mouseX - pad) / s_scale;
            float relY = (mouseY - pad) / s_scale;
            int x = static_cast<int>(relX);
            int y = static_cast<int>(relY);
            if (x >= 0 && x < HIT_MAP_W && y >= 0 && y < HIT_MAP_H) {
                rankAtCursor = s_hitMapData[y * HIT_MAP_W + x];
            }
        }

        // 1. 柱（バー）の上をクリックした場合はその選択値を確定
        if (rankAtCursor > 0) {
            const PriorityMatrixEntry* entry = GetMatrixEntryByRank(rankAtCursor);
            if (entry) {
                s_hoveredRawRow = entry->row;
                s_hoveredRawCol = entry->col;
                s_wheelActive = false;
            }
            CommitAndClose(hWnd);
            return 0;
        }

        // 2. ホイール回転後に右ペインをクリックした場合はホイール選択値を確定
        if (clickedHud && s_wheelActive) {
            CommitAndClose(hWnd);
            return 0;
        }

        // 3. 柱の外、またはホイールを回さずに右ペインをクリックした場合は
        //    Thread / Process 両方を非アタッチ状態（淡緑背景）へ戻して閉じる
        ResetAndClose(hWnd);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_KEYUP: {
        if (wParam == VK_CONTROL) {
            // Only update Ctrl state when the cursor is inside the left 3D matrix
            if (s_isMouseOverLeftMatrix) {
                bool ctrlNow = (msg == WM_KEYDOWN);
                if (s_isCtrlPressed != ctrlNow) {
                    s_isCtrlPressed = ctrlNow;
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        if (msg == WM_KEYDOWN) {
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hWnd);
                return 0;
            } else if (wParam == VK_RETURN) {
                CommitAndClose(hWnd);
                return 0;
            }
        }
        break;
    }

    case WM_KILLFOCUS: {
        DestroyWindow(hWnd);
        return 0;
    }

    case WM_DESTROY: {
        s_hPickerWnd = nullptr;
        return 0;
    }
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static void RegisterPickerClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXA wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PickerWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "ATI_PriorityMatrixPicker";

    RegisterClassExA(&wc);
    registered = true;
}

HWND ShowPriorityMatrixPicker(
    HWND hParent,
    PickerMode mode,
    DWORD initialProcessClass,
    int initialThreadPrio,
    bool isAdmin,
    PriorityPickerCallback onSelected
) {
    if (s_hPickerWnd && IsWindow(s_hPickerWnd)) {
        DestroyWindow(s_hPickerWnd);
    }

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrA(hParent, GWLP_HINSTANCE));
    RegisterPickerClass(hInst);
    InitializeGdiPlus();

    s_onSelectedCallback = onSelected;
    s_pickerMode = mode;
    s_isAdmin = isAdmin;
    s_committedRow = ClampRowForAdmin(ProcessClassToRow(initialProcessClass));

    if (IsFixedThreadPriority(initialThreadPrio)) {
        s_committedCol = -1;
        s_committedFixedPrio = DecodeFixedThreadPriority(initialThreadPrio);
    } else {
        s_committedCol = ThreadPrioToCol(initialThreadPrio);
        s_committedFixedPrio = 0;
    }

    s_hoveredRawRow = -1;
    s_hoveredRawCol = -1;
    s_isCtrlPressed = false;
    s_isMouseOverLeftMatrix = false;
    s_wheelActive = false;
    s_wheelPrio = (s_committedCol == -1 && s_committedFixedPrio > 0)
        ? s_committedFixedPrio
        : kPriorityMatrix[s_committedRow][s_committedCol];

    // Detect DPI scale from parent window
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    UINT dpi = 96;
    if (hUser32) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        PFN_GetDpiForWindow pfn = (PFN_GetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
        if (pfn) dpi = pfn(hParent);
    }
    s_scale = static_cast<float>(dpi) / 96.0f;

    int w = static_cast<int>(630 * s_scale);
    int h = static_cast<int>(374 * s_scale);

    // Center over parent dialog
    RECT rcParent;
    GetWindowRect(hParent, &rcParent);
    int x = rcParent.left + ((rcParent.right - rcParent.left) - w) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - h) / 2;

    s_hPickerWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "ATI_PriorityMatrixPicker",
        "ATI Priority Matrix Picker",
        WS_POPUP | WS_BORDER | WS_VISIBLE,
        x, y, w, h,
        hParent,
        nullptr,
        hInst,
        nullptr
    );

    if (s_hPickerWnd) {
        SetFocus(s_hPickerWnd);
    }
    return s_hPickerWnd;
}

bool IsPriorityMatrixPickerOpen() {
    return (s_hPickerWnd != nullptr && IsWindow(s_hPickerWnd));
}

void ClosePriorityMatrixPicker() {
    if (s_hPickerWnd && IsWindow(s_hPickerWnd)) {
        DestroyWindow(s_hPickerWnd);
    }
}

} // namespace ati
