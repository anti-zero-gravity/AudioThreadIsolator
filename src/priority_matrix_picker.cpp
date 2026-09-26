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
static PrioritySelectedCallback s_onSelectedCallback = nullptr;
static int s_selectedRank = 1;
static int s_hoveredRank = 0;
static float s_scale = 1.0f;

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

static Gdiplus::Image* GetRankImage(int rank) {
    if (rank < 1 || rank > 49) {
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

    auto it = s_compositeCache.find(rank);
    if (it != s_compositeCache.end()) {
        return it->second;
    }

    // まず内部リソース (RCDATA 3101..3149) から読み込み
    Gdiplus::Image* img = LoadImageFromResource(IDR_MATRIX_RANK_BASE + rank);
    if (img && img->GetLastStatus() == Gdiplus::Ok) {
        s_compositeCache[rank] = img;
        return img;
    }

    // フォールバック (外部ファイル探索)
    FindUiPartsDirectory();
    wchar_t fname[64];
    swprintf(fname, 64, L"\\full_composites\\matrix_rank_%02d.png", rank);
    std::wstring fullPath = s_uiPartsDirW + fname;

    img = Gdiplus::Image::FromFile(fullPath.c_str());
    if (img && img->GetLastStatus() == Gdiplus::Ok) {
        s_compositeCache[rank] = img;
        return img;
    }
    return nullptr;
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

        // 1. Draw 3D Matrix Image on Left Panel
        int pad = static_cast<int>(14 * s_scale);
        int imgW = static_cast<int>(HIT_MAP_W * s_scale);
        int imgH = static_cast<int>(HIT_MAP_H * s_scale);

        int activeRank = (s_hoveredRank > 0) ? s_hoveredRank : s_selectedRank;
        Gdiplus::Image* pImg = GetRankImage(activeRank);
        if (pImg) {
            g.DrawImage(pImg, pad, pad, imgW, imgH);
        } else {
            // Fallback outline if images not found
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

        // HUD Typography & Details
        const PriorityMatrixEntry* entry = GetMatrixEntryByRank(activeRank);
        if (entry) {
            int textPadX = hudLeft + static_cast<int>(16 * s_scale);
            int curY = hudTop + static_cast<int>(14 * s_scale);

            // Sub title
            Gdiplus::Font titleFont(L"Segoe UI", 11.0f * s_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 100, 116, 139));
            g.DrawString(L"CURRENT SELECTION", -1, &titleFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &titleBrush);

            curY += static_cast<int>(20 * s_scale);

            // Large Base Priority Hero Number & Rank
            wchar_t bpBuf[16];
            swprintf(bpBuf, 16, L"%d", entry->basePriority);
            Gdiplus::Font heroFont(L"Segoe UI", 42.0f * s_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush heroBrush(Gdiplus::Color(255, 255, 255, 255));
            g.DrawString(bpBuf, -1, &heroFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &heroBrush);

            // Measure Hero Number width dynamically so Rank label never collides
            Gdiplus::RectF heroBounds;
            g.MeasureString(bpBuf, -1, &heroFont, Gdiplus::PointF(0, 0), &heroBounds);
            float rankX = static_cast<float>(textPadX) + heroBounds.Width + 10.0f * s_scale;
            float rankY = static_cast<float>(curY) + 16.0f * s_scale;

            // Rank label next to base priority
            wchar_t rankBuf[32];
            swprintf(rankBuf, 32, L"Rank #%d / 49", entry->rank);
            Gdiplus::Font rankFont(L"Segoe UI", 14.0f * s_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush rankBrush(Gdiplus::Color(255, 96, 165, 250));
            g.DrawString(rankBuf, -1, &rankFont, Gdiplus::PointF(rankX, rankY), &rankBrush);

            curY += static_cast<int>(50 * s_scale);

            // Separator line
            Gdiplus::Pen sepPen(Gdiplus::Color(40, 255, 255, 255), 1.0f);
            g.DrawLine(&sepPen, textPadX, curY, textPadX + hudW - static_cast<int>(32 * s_scale), curY);

            curY += static_cast<int>(14 * s_scale);

            // Detail fields (UnitPixel for strict DPI tracking)
            Gdiplus::Font lblFont(L"Segoe UI", 12.0f * s_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font valFont(L"Segoe UI", 14.0f * s_scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font monoFont(L"Consolas", 12.5f * s_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font hintFont(L"Segoe UI", 11.5f * s_scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

            Gdiplus::SolidBrush lblBrush(Gdiplus::Color(255, 148, 163, 184));
            Gdiplus::SolidBrush valBrush(Gdiplus::Color(255, 241, 245, 249));
            Gdiplus::SolidBrush accentBrush(Gdiplus::Color(255, 56, 189, 248));

            // Process Class
            g.DrawString(L"Process Class:", -1, &lblFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &lblBrush);
            curY += static_cast<int>(18 * s_scale);
            wchar_t pClassBuf[64];
            MultiByteToWideChar(CP_ACP, 0, entry->processLabel, -1, pClassBuf, 64);
            g.DrawString(pClassBuf, -1, &valFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &valBrush);

            curY += static_cast<int>(26 * s_scale);

            // Thread Priority
            g.DrawString(L"Thread Priority:", -1, &lblFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &lblBrush);
            curY += static_cast<int>(18 * s_scale);
            wchar_t tPrioBuf[64];
            MultiByteToWideChar(CP_ACP, 0, entry->threadLabel, -1, tPrioBuf, 64);
            g.DrawString(tPrioBuf, -1, &valFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &accentBrush);

            curY += static_cast<int>(26 * s_scale);

            // Formula
            g.DrawString(L"Base Formula:", -1, &lblFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &lblBrush);
            curY += static_cast<int>(18 * s_scale);
            wchar_t formBuf[64];
            MultiByteToWideChar(CP_ACP, 0, entry->formulaStr, -1, formBuf, 64);
            Gdiplus::SolidBrush formBrush(Gdiplus::Color(255, 203, 213, 225));
            g.DrawString(formBuf, -1, &monoFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(curY)), &formBrush);

            // Bottom Instructions
            int btmY = hudTop + hudH - static_cast<int>(24 * s_scale);
            Gdiplus::SolidBrush hintBrush(Gdiplus::Color(255, 100, 116, 139));
            g.DrawString(L"Click to apply ｜ [Esc] to cancel", -1, &hintFont, Gdiplus::PointF(static_cast<float>(textPadX), static_cast<float>(btmY)), &hintBrush);
        }

        // Blit to screen
        BitBlt(hdc, 0, 0, clientW, clientH, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mouseX = LOWORD(lParam);
        int mouseY = HIWORD(lParam);

        int pad = static_cast<int>(14 * s_scale);
        int imgW = static_cast<int>(HIT_MAP_W * s_scale);
        int imgH = static_cast<int>(HIT_MAP_H * s_scale);

        int rankAtCursor = 0;
        if (mouseX >= pad && mouseX < pad + imgW && mouseY >= pad && mouseY < pad + imgH) {
            float relX = (mouseX - pad) / s_scale;
            float relY = (mouseY - pad) / s_scale;
            int x = static_cast<int>(relX);
            int y = static_cast<int>(relY);
            if (x >= 0 && x < HIT_MAP_W && y >= 0 && y < HIT_MAP_H && !s_hitMapData.empty()) {
                rankAtCursor = s_hitMapData[y * HIT_MAP_W + x];
            }
        }

        if (rankAtCursor != s_hoveredRank) {
            s_hoveredRank = rankAtCursor;
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        if (rankAtCursor > 0) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
        } else {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (s_hoveredRank > 0) {
            s_selectedRank = s_hoveredRank;
            if (s_onSelectedCallback) {
                s_onSelectedCallback(s_selectedRank);
            }
            DestroyWindow(hWnd);
            return 0;
        }
        return 0;
    }

    case WM_KEYDOWN: {
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hWnd);
            return 0;
        } else if (wParam == VK_RETURN) {
            if (s_onSelectedCallback) {
                s_onSelectedCallback(s_selectedRank);
            }
            DestroyWindow(hWnd);
            return 0;
        } else if (wParam == VK_UP || wParam == VK_LEFT) {
            s_selectedRank = (std::max)(1, s_selectedRank - 1);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        } else if (wParam == VK_DOWN || wParam == VK_RIGHT) {
            s_selectedRank = (std::min)(49, s_selectedRank + 1);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_KILLFOCUS: {
        // Destroy picker when focus is lost (clicking outside)
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

HWND ShowPriorityMatrixPicker(HWND hParent, int initialRank, PrioritySelectedCallback onSelected) {
    if (s_hPickerWnd && IsWindow(s_hPickerWnd)) {
        DestroyWindow(s_hPickerWnd);
    }

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrA(hParent, GWLP_HINSTANCE));
    RegisterPickerClass(hInst);
    InitializeGdiPlus();

    s_onSelectedCallback = onSelected;
    s_selectedRank = (initialRank >= 1 && initialRank <= 49) ? initialRank : 19;
    s_hoveredRank = 0;

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
