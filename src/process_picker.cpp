#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include "process_picker.h"
#include "dpi_utils.h"
#include "resource.h"
#include <algorithm>
#include <commctrl.h>
#include <map>
#include <tlhelp32.h>
#include <windows.h>

namespace ati {

struct CaseInsensitiveCompare {
  bool operator()(const std::string &a, const std::string &b) const {
    return _stricmp(a.c_str(), b.c_str()) < 0;
  }
};

struct PickerRow {
  PickedProcess info;
  bool checkExclude = false;
  bool checkSelect = false;
};

static std::vector<PickedProcess> *s_pSelectedOut = nullptr;
static std::vector<std::string> *s_pExcludedInOut = nullptr;
static std::vector<PickerRow> s_pickerRows;
static std::map<std::string, bool, CaseInsensitiveCompare> s_excludedState;
static bool s_hintVisible = false;

static std::string PriorityToString(DWORD prio) {
  switch (prio) {
  case REALTIME_PRIORITY_CLASS:
    return "Realtime";
  case HIGH_PRIORITY_CLASS:
    return "High";
  case ABOVE_NORMAL_PRIORITY_CLASS:
    return "Above Normal";
  case NORMAL_PRIORITY_CLASS:
    return "Normal";
  case BELOW_NORMAL_PRIORITY_CLASS:
    return "Below Normal";
  case IDLE_PRIORITY_CLASS:
    return "Low";
  default:
    return "Normal";
  }
}

static void SetPickerHintVisible(HWND hDlg, bool show) {
  if (!hDlg || s_hintVisible == show)
    return;
  s_hintVisible = show;
  SetDlgItemTextA(
      hDlg, IDC_STATIC_PICKER_HINT,
      show ? "! = Check to exclude from unlisted polling mask targets." : "");
}

static LRESULT CALLBACK PickerHeaderSubclassProc(HWND hWnd, UINT msg,
                                                 WPARAM wParam, LPARAM lParam) {
  WNDPROC pfnOrig =
      reinterpret_cast<WNDPROC>(GetPropA(hWnd, "ATI_OrigPickerHdrProc"));
  LRESULT lRes = pfnOrig ? CallWindowProcA(pfnOrig, hWnd, msg, wParam, lParam)
                         : DefWindowProcA(hWnd, msg, wParam, lParam);

  if (msg == WM_PAINT) {
    RECT rcItem;
    if (SendMessageA(hWnd, HDM_GETITEMRECT, 0,
                     reinterpret_cast<LPARAM>(&rcItem))) {
      HDC hdc = GetDC(hWnd);
      if (hdc) {
        RECT rcFill = rcItem;
        InflateRect(&rcFill, -1, -1);
        FillRect(hdc, &rcFill, GetSysColorBrush(COLOR_BTNFACE));

        HFONT hFont = GetEffectiveFont(hWnd);
        LOGFONTA lf = {0};
        GetObjectA(hFont, sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        HFONT hBoldFont = CreateFontIndirectA(&lf);

        HGDIOBJ oldFont = SelectObject(hdc, hBoldFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 50, 50));

        RECT rcText = rcItem;
        DrawTextA(hdc, "!", 1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(hBoldFont);
        ReleaseDC(hWnd, hdc);
      }
    }
  } else if (msg == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0};
    TrackMouseEvent(&tme);
    POINT pt = {static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam))};
    RECT rcCol0 = {0};
    bool overCol0 = false;
    if (SendMessageA(hWnd, HDM_GETITEMRECT, 0,
                     reinterpret_cast<LPARAM>(&rcCol0))) {
      overCol0 = (pt.x >= rcCol0.left && pt.x < rcCol0.right &&
                  pt.y >= rcCol0.top && pt.y < rcCol0.bottom);
    }
    HWND hList = GetParent(hWnd);
    HWND hDlg = hList ? GetParent(hList) : nullptr;
    SetPickerHintVisible(hDlg, overCol0);
  } else if (msg == WM_MOUSELEAVE) {
    HWND hList = GetParent(hWnd);
    HWND hDlg = hList ? GetParent(hList) : nullptr;
    SetPickerHintVisible(hDlg, false);
  } else if (msg == WM_NCDESTROY) {
    RemovePropA(hWnd, "ATI_OrigPickerHdrProc");
  }

  return lRes;
}

static LRESULT CALLBACK PickerListSubclassProc(HWND hWnd, UINT msg,
                                               WPARAM wParam, LPARAM lParam) {
  WNDPROC pfnOrig =
      reinterpret_cast<WNDPROC>(GetPropA(hWnd, "ATI_OrigPickerListProc"));
  if (msg == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0};
    TrackMouseEvent(&tme);
    POINT pt = {static_cast<short>(LOWORD(lParam)),
                static_cast<short>(HIWORD(lParam))};
    LVHITTESTINFO lvhti = {0};
    lvhti.pt = pt;
    ListView_SubItemHitTest(hWnd, &lvhti);
    int col0W = ListView_GetColumnWidth(hWnd, 0);
    bool overCol0 = (lvhti.iItem >= 0) ? (lvhti.iSubItem == 0)
                                       : (pt.x >= 0 && pt.x < col0W);
    HWND hDlg = GetParent(hWnd);
    SetPickerHintVisible(hDlg, overCol0);
  } else if (msg == WM_MOUSELEAVE) {
    HWND hDlg = GetParent(hWnd);
    SetPickerHintVisible(hDlg, false);
  } else if (msg == WM_NCDESTROY) {
    RemovePropA(hWnd, "ATI_OrigPickerListProc");
  }
  return pfnOrig ? CallWindowProcA(pfnOrig, hWnd, msg, wParam, lParam)
                 : DefWindowProcA(hWnd, msg, wParam, lParam);
}

static void PopulateRunningProcesses(HWND hList) {
  ListView_DeleteAllItems(hList);
  s_pickerRows.clear();

  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE)
    return;

  PROCESSENTRY32 pe;
  pe.dwSize = sizeof(pe);

  std::map<std::string, PickedProcess, CaseInsensitiveCompare> procMap;

  if (Process32First(hSnap, &pe)) {
    do {
      if (pe.th32ProcessID == 0)
        continue;

      std::string exeName = pe.szExeFile;
      std::string baseName = exeName;
      size_t dotPos = baseName.find_last_of('.');
      if (dotPos != std::string::npos) {
        baseName = baseName.substr(0, dotPos);
      }

      if (procMap.find(exeName) == procMap.end()) {
        PickedProcess item;
        item.processName = baseName;
        item.exeFileName = exeName;
        item.currentPriorityClass = NORMAL_PRIORITY_CLASS;

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   pe.th32ProcessID);
        if (hProc) {
          DWORD pc = GetPriorityClass(hProc);
          if (pc != 0) {
            item.currentPriorityClass = pc;
          }
          CloseHandle(hProc);
        }
        procMap[exeName] = item;
      }
    } while (Process32Next(hSnap, &pe));
  }
  CloseHandle(hSnap);

  int index = 0;
  for (const auto &pair : procMap) {
    const auto &item = pair.second;

    PickerRow row;
    row.info = item;
    auto exIt = s_excludedState.find(item.exeFileName);
    row.checkExclude = (exIt != s_excludedState.end() && exIt->second);
    row.checkSelect = false;
    s_pickerRows.push_back(row);

    LVITEMA lvi = {0};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = index;
    lvi.iSubItem = 0;
    lvi.pszText = const_cast<LPSTR>("");
    ListView_InsertItem(hList, &lvi);

    ListView_SetItemText(hList, index, 1,
                         const_cast<LPSTR>(item.processName.c_str()));

    std::string prioStr = PriorityToString(item.currentPriorityClass);
    ListView_SetItemText(hList, index, 2, const_cast<LPSTR>(prioStr.c_str()));

    index++;
  }
}

static void AdjustPickerColumns(HWND hList, float scale) {
  if (!hList)
    return;
  RECT rc;
  GetClientRect(hList, &rc);
  int listW = rc.right - rc.left;
  if (listW <= 0)
    return;

  int w0 = ScaleI(26, scale);
  int w2 = ScaleI(72, scale);
  int w1 = listW - w0 - w2;
  if (w1 < ScaleI(80, scale)) {
    w1 = ScaleI(80, scale);
  }
  ListView_SetColumnWidth(hList, 0, w0);
  ListView_SetColumnWidth(hList, 1, w1);
  ListView_SetColumnWidth(hList, 2, w2);
}

static void AdjustPickerLayout(HWND hDlg, float scale, int cx, int cy) {
  if (cx <= 0 || cy <= 0)
    return;

  HWND hTitle = GetDlgItem(hDlg, IDC_STATIC_PICKER_TITLE);
  HWND hList = GetDlgItem(hDlg, IDC_LIST_RUNNING);
  HWND hHint = GetDlgItem(hDlg, IDC_STATIC_PICKER_HINT);
  HWND hRefresh = GetDlgItem(hDlg, IDC_BTN_REFRESH);
  HWND hDone = GetDlgItem(hDlg, IDC_BTN_SELECT_DONE);
  HWND hCancel = GetDlgItem(hDlg, IDC_BTN_SELECT_CANCEL);

  int marginX = ScaleI(10, scale);
  int marginY = ScaleI(10, scale);
  int titleH = ScaleI(16, scale);
  int hintH = ScaleI(16, scale);
  int btnW = ScaleI(75, scale);
  int btnH = ScaleI(24, scale);
  int gapY = ScaleI(6, scale);

  int contentW = cx - marginX * 2;
  if (contentW < ScaleI(100, scale))
    contentW = ScaleI(100, scale);

  int titleY = marginY;
  int listY = titleY + titleH + gapY;
  int btnY = cy - marginY - btnH;
  int hintY = btnY - gapY - hintH;
  int listH = hintY - gapY - listY;

  if (listH < ScaleI(60, scale))
    listH = ScaleI(60, scale);

  HDWP hdwp = BeginDeferWindowPos(6);
  if (hdwp) {
    if (hTitle)
      hdwp = DeferWindowPos(hdwp, hTitle, NULL, marginX, titleY, contentW,
                            titleH, SWP_NOZORDER | SWP_NOACTIVATE);
    if (hList)
      hdwp = DeferWindowPos(hdwp, hList, NULL, marginX, listY, contentW, listH,
                            SWP_NOZORDER | SWP_NOACTIVATE);
    if (hHint)
      hdwp = DeferWindowPos(hdwp, hHint, NULL, marginX, hintY, contentW, hintH,
                            SWP_NOZORDER | SWP_NOACTIVATE);
    if (hRefresh)
      hdwp = DeferWindowPos(hdwp, hRefresh, NULL, marginX, btnY, btnW, btnH,
                            SWP_NOZORDER | SWP_NOACTIVATE);
    if (hCancel)
      hdwp = DeferWindowPos(hdwp, hCancel, NULL, cx - marginX - btnW, btnY,
                            btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    if (hDone)
      hdwp = DeferWindowPos(hdwp, hDone, NULL,
                            cx - marginX - btnW * 2 - ScaleI(6, scale), btnY,
                            btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    EndDeferWindowPos(hdwp);
  }

  if (hList) {
    AdjustPickerColumns(hList, scale);
  }
}

static INT_PTR CALLBACK ProcessPickerDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                             LPARAM lParam) {
  switch (msg) {
  case WM_INITDIALOG: {
    s_hintVisible = false;
    SetDlgItemTextA(hDlg, IDC_STATIC_PICKER_HINT, "");

    s_excludedState.clear();
    if (s_pExcludedInOut) {
      for (const auto &ex : *s_pExcludedInOut) {
        if (!ex.empty()) {
          s_excludedState[ex] = true;
        }
      }
    }

    HWND hList = GetDlgItem(hDlg, IDC_LIST_RUNNING);

    HFONT hFont = (HFONT)SendMessageA(hDlg, WM_GETFONT, 0, 0);
    if (!hFont)
      hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageA(hList, WM_SETFONT, (WPARAM)hFont, TRUE);

    WNDPROC pfnOrigList = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hList, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(PickerListSubclassProc)));
    if (pfnOrigList) {
      SetPropA(hList, "ATI_OrigPickerListProc",
               reinterpret_cast<HANDLE>(pfnOrigList));
    }

    ListView_SetExtendedListViewStyle(
        hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    HWND hHeader = ListView_GetHeader(hList);
    if (hHeader) {
      SendMessageA(hHeader, WM_SETFONT, (WPARAM)hFont, TRUE);
      WNDPROC pfnOrigHdr = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
          hHeader, GWLP_WNDPROC,
          reinterpret_cast<LONG_PTR>(PickerHeaderSubclassProc)));
      if (pfnOrigHdr) {
        SetPropA(hHeader, "ATI_OrigPickerHdrProc",
                 reinterpret_cast<HANDLE>(pfnOrigHdr));
      }
    }

    float scale = GetDpiScaleForWindow(hDlg);

    LVCOLUMNA lvc = {0};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;

    lvc.iSubItem = 0;
    lvc.fmt = LVCFMT_CENTER;
    lvc.cx = ScaleI(26, scale);
    lvc.pszText = const_cast<LPSTR>("");
    ListView_InsertColumn(hList, 0, &lvc);

    lvc.iSubItem = 1;
    lvc.fmt = LVCFMT_LEFT;
    lvc.cx = ScaleI(150, scale);
    lvc.pszText = const_cast<LPSTR>("Process Name");
    ListView_InsertColumn(hList, 1, &lvc);

    lvc.iSubItem = 2;
    lvc.fmt = LVCFMT_LEFT;
    lvc.cx = ScaleI(72, scale);
    lvc.pszText = const_cast<LPSTR>("Priority");
    ListView_InsertColumn(hList, 2, &lvc);

    PopulateRunningProcesses(hList);

    RECT rcClient;
    GetClientRect(hDlg, &rcClient);
    AdjustPickerLayout(hDlg, scale, rcClient.right - rcClient.left,
                       rcClient.bottom - rcClient.top);
    return TRUE;
  }

  case WM_NOTIFY: {
    LPNMHDR pnm = reinterpret_cast<LPNMHDR>(lParam);
    if (pnm && pnm->idFrom == IDC_LIST_RUNNING) {
      HWND hList = pnm->hwndFrom;
      if (pnm->code == NM_CLICK) {
        LVHITTESTINFO lvhti = {0};
        GetCursorPos(&lvhti.pt);
        ScreenToClient(hList, &lvhti.pt);
        ListView_SubItemHitTest(hList, &lvhti);
        if (lvhti.iItem >= 0 &&
            lvhti.iItem < static_cast<int>(s_pickerRows.size())) {
          if (lvhti.iSubItem == 0) {
            auto &row = s_pickerRows[lvhti.iItem];
            row.checkExclude = !row.checkExclude;
            s_excludedState[row.info.exeFileName] = row.checkExclude;
            ListView_RedrawItems(hList, lvhti.iItem, lvhti.iItem);
            return TRUE;
          } else if (lvhti.iSubItem == 1) {
            auto &row = s_pickerRows[lvhti.iItem];
            row.checkSelect = !row.checkSelect;
            ListView_RedrawItems(hList, lvhti.iItem, lvhti.iItem);
            return TRUE;
          }
        }
      } else if (pnm->code == NM_CUSTOMDRAW) {
        LPNMLVCUSTOMDRAW plvcd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
        switch (plvcd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
          SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
          return TRUE;
        case CDDS_ITEMPREPAINT:
          SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
          return TRUE;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
          int rowIdx = static_cast<int>(plvcd->nmcd.dwItemSpec);
          int subItem = plvcd->iSubItem;
          if (rowIdx >= 0 && rowIdx < static_cast<int>(s_pickerRows.size()) &&
              (subItem == 0 || subItem == 1)) {
            const auto &row = s_pickerRows[rowIdx];
            HDC hdc = plvcd->nmcd.hdc;
            RECT rcSub = {0};
            ListView_GetSubItemRect(hList, rowIdx, subItem, LVIR_BOUNDS,
                                    &rcSub);
            if (subItem == 0) {
              rcSub.right = rcSub.left + ListView_GetColumnWidth(hList, 0);
            }

            UINT state = ListView_GetItemState(hList, rowIdx, LVIS_SELECTED);
            bool isSel = (state & LVIS_SELECTED) != 0;
            HBRUSH hBkBrush =
                GetSysColorBrush(isSel ? COLOR_HIGHLIGHT : COLOR_WINDOW);
            FillRect(hdc, &rcSub, hBkBrush);

            float scale = GetDpiScaleForWindow(hDlg);
            int cbSize = ScaleI(Layout::kCheckboxSize, scale);
            if (cbSize < Layout::kCheckboxSize)
              cbSize = Layout::kCheckboxSize;
            int cellH = rcSub.bottom - rcSub.top;
            int cbY = rcSub.top + (cellH - cbSize) / 2;

            if (subItem == 0) {
              int cellW = rcSub.right - rcSub.left;
              int cbX = rcSub.left + (cellW - cbSize) / 2;
              RECT rcCb = {cbX, cbY, cbX + cbSize, cbY + cbSize};
              UINT uState =
                  DFCS_BUTTONCHECK | (row.checkExclude ? DFCS_CHECKED : 0);
              DrawFrameControl(hdc, &rcCb, DFC_BUTTON, uState);
            } else if (subItem == 1) {
              int cbX = rcSub.left + ScaleI(4, scale);
              RECT rcCb = {cbX, cbY, cbX + cbSize, cbY + cbSize};
              UINT uState =
                  DFCS_BUTTONCHECK | (row.checkSelect ? DFCS_CHECKED : 0);
              DrawFrameControl(hdc, &rcCb, DFC_BUTTON, uState);

              HFONT hFont = GetEffectiveFont(hList);
              HGDIOBJ oldFont = SelectObject(hdc, hFont);
              SetBkMode(hdc, TRANSPARENT);
              SetTextColor(hdc, GetSysColor(isSel ? COLOR_HIGHLIGHTTEXT
                                                  : COLOR_WINDOWTEXT));

              RECT rcText = rcSub;
              rcText.left = cbX + cbSize + ScaleI(4, scale);
              rcText.right = rcSub.right - ScaleI(2, scale);
              DrawTextA(hdc, row.info.processName.c_str(), -1, &rcText,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                            DT_END_ELLIPSIS);
              SelectObject(hdc, oldFont);
            }

            SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
            return TRUE;
          }
          SetWindowLongPtrA(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
          return TRUE;
        }
        }
      }
    }
    break;
  }

  case WM_SIZE: {
    if (wParam != SIZE_MINIMIZED) {
      float scale = GetDpiScaleForWindow(hDlg);
      int cx = LOWORD(lParam);
      int cy = HIWORD(lParam);
      AdjustPickerLayout(hDlg, scale, cx, cy);
    }
    return TRUE;
  }

  case WM_GETMINMAXINFO: {
    LPMINMAXINFO lpMMI = reinterpret_cast<LPMINMAXINFO>(lParam);
    float scale = GetDpiScaleForWindow(hDlg);
    lpMMI->ptMinTrackSize.x = ScaleI(260, scale);
    lpMMI->ptMinTrackSize.y = ScaleI(240, scale);
    return TRUE;
  }

  case WM_DPICHANGED: {
    float scale = GetDpiScaleForWindow(hDlg);
    RECT *prcNew = reinterpret_cast<RECT *>(lParam);
    if (prcNew) {
      SetWindowPos(hDlg, NULL, prcNew->left, prcNew->top,
                   prcNew->right - prcNew->left, prcNew->bottom - prcNew->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT rcClient;
    GetClientRect(hDlg, &rcClient);
    AdjustPickerLayout(hDlg, scale, rcClient.right - rcClient.left,
                       rcClient.bottom - rcClient.top);
    return TRUE;
  }

  case WM_COMMAND: {
    WORD cmdId = LOWORD(wParam);
    if (cmdId == IDC_BTN_REFRESH) {
      HWND hList = GetDlgItem(hDlg, IDC_LIST_RUNNING);
      PopulateRunningProcesses(hList);
      float scale = GetDpiScaleForWindow(hDlg);
      AdjustPickerColumns(hList, scale);
      return TRUE;
    } else if (cmdId == IDC_BTN_SELECT_DONE) {
      if (s_pSelectedOut) {
        s_pSelectedOut->clear();
        for (const auto &row : s_pickerRows) {
          if (row.checkSelect) {
            s_pSelectedOut->push_back(row.info);
          }
        }
      }
      if (s_pExcludedInOut) {
        s_pExcludedInOut->clear();
        for (const auto &kv : s_excludedState) {
          if (kv.second) {
            s_pExcludedInOut->push_back(kv.first);
          }
        }
      }
      EndDialog(hDlg, IDOK);
      return TRUE;
    } else if (cmdId == IDC_BTN_SELECT_CANCEL || cmdId == IDCANCEL) {
      EndDialog(hDlg, IDCANCEL);
      return TRUE;
    }
    break;
  }

  case WM_CLOSE:
    EndDialog(hDlg, IDCANCEL);
    return TRUE;
  }

  return FALSE;
}

bool ShowProcessPickerDialog(HWND hParent, HINSTANCE hInstance,
                             std::vector<PickedProcess> &outSelected,
                             std::vector<std::string> &inoutExcludedExeNames) {
  s_pSelectedOut = &outSelected;
  s_pExcludedInOut = &inoutExcludedExeNames;
  INT_PTR res = DialogBoxA(hInstance, MAKEINTRESOURCEA(IDD_PROCESS_PICKER),
                           hParent, ProcessPickerDlgProc);
  s_pSelectedOut = nullptr;
  s_pExcludedInOut = nullptr;
  return (res == IDOK);
}

} // namespace ati
