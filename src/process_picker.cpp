#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "process_picker.h"
#include "resource.h"
#include <commctrl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <map>

namespace ati {

static std::vector<PickedProcess>* s_pSelectedOut = nullptr;

static float GetDpiScaleForWindow(HWND hWnd) {
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

static std::string PriorityToString(DWORD prio) {
    switch (prio) {
    case REALTIME_PRIORITY_CLASS:     return "Realtime";
    case HIGH_PRIORITY_CLASS:         return "High";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "Above Normal";
    case NORMAL_PRIORITY_CLASS:       return "Normal";
    case BELOW_NORMAL_PRIORITY_CLASS: return "Below Normal";
    case IDLE_PRIORITY_CLASS:         return "Low";
    default:                          return "Normal";
    }
}

static void PopulateRunningProcesses(HWND hList) {
    ListView_DeleteAllItems(hList);

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    struct CaseInsensitiveCompare {
        bool operator()(const std::string& a, const std::string& b) const {
            return _stricmp(a.c_str(), b.c_str()) < 0;
        }
    };
    std::map<std::string, PickedProcess, CaseInsensitiveCompare> procMap;

    if (Process32First(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;
            
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

                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (hProc) {
                    item.currentPriorityClass = GetPriorityClass(hProc);
                    CloseHandle(hProc);
                }
                procMap[exeName] = item;
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);

    int index = 0;
    for (const auto& pair : procMap) {
        const auto& item = pair.second;

        LVITEMA lvi = { 0 };
        lvi.mask = LVIF_TEXT;
        lvi.iItem = index;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<LPSTR>(item.processName.c_str());
        ListView_InsertItem(hList, &lvi);

        ListView_SetItemText(hList, index, 1, const_cast<LPSTR>(item.exeFileName.c_str()));
        
        std::string prioStr = PriorityToString(item.currentPriorityClass);
        ListView_SetItemText(hList, index, 2, const_cast<LPSTR>(prioStr.c_str()));

        index++;
    }
}

static INT_PTR CALLBACK ProcessPickerDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND hList = GetDlgItem(hDlg, IDC_LIST_RUNNING);

        HFONT hFont = (HFONT)SendMessageA(hDlg, WM_GETFONT, 0, 0);
        if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageA(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hHeader = ListView_GetHeader(hList);
        if (hHeader) {
            SendMessageA(hHeader, WM_SETFONT, (WPARAM)hFont, TRUE);
        }

        ListView_SetExtendedListViewStyle(
            hList, 
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER
        );

        float scale = GetDpiScaleForWindow(hDlg);

        LVCOLUMNA lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0;
        lvc.cx = static_cast<int>(120 * scale);
        lvc.pszText = const_cast<LPSTR>("Process Name");
        ListView_InsertColumn(hList, 0, &lvc);

        lvc.iSubItem = 1;
        lvc.cx = static_cast<int>(120 * scale);
        lvc.pszText = const_cast<LPSTR>("File Name");
        ListView_InsertColumn(hList, 1, &lvc);

        lvc.iSubItem = 2;
        lvc.cx = static_cast<int>(75 * scale);
        lvc.pszText = const_cast<LPSTR>("Priority");
        ListView_InsertColumn(hList, 2, &lvc);

        for (int i = 0; i < 3; ++i) {
            int initialW = ListView_GetColumnWidth(hList, i);
            ListView_SetColumnWidth(hList, i, LVSCW_AUTOSIZE_USEHEADER);
            int autoW = ListView_GetColumnWidth(hList, i);
            if (autoW < initialW) {
                ListView_SetColumnWidth(hList, i, initialW);
            } else {
                ListView_SetColumnWidth(hList, i, autoW + static_cast<int>(6 * scale));
            }
        }

        PopulateRunningProcesses(hList);
        return TRUE;
    }

    case WM_COMMAND: {
        WORD cmdId = LOWORD(wParam);
        if (cmdId == IDC_BTN_REFRESH) {
            HWND hList = GetDlgItem(hDlg, IDC_LIST_RUNNING);
            PopulateRunningProcesses(hList);
            return TRUE;
        }
        else if (cmdId == IDC_BTN_SELECT_DONE) {
            HWND hList = GetDlgItem(hDlg, IDC_LIST_RUNNING);
            int count = ListView_GetItemCount(hList);
            if (s_pSelectedOut) {
                s_pSelectedOut->clear();
                for (int i = 0; i < count; ++i) {
                    if (ListView_GetCheckState(hList, i)) {
                        char bufName[MAX_PATH] = { 0 };
                        char bufExe[MAX_PATH] = { 0 };
                        ListView_GetItemText(hList, i, 0, bufName, MAX_PATH);
                        ListView_GetItemText(hList, i, 1, bufExe, MAX_PATH);

                        PickedProcess item;
                        item.processName = bufName;
                        item.exeFileName = bufExe;
                        item.currentPriorityClass = NORMAL_PRIORITY_CLASS;
                        s_pSelectedOut->push_back(item);
                    }
                }
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        else if (cmdId == IDC_BTN_SELECT_CANCEL || cmdId == IDCANCEL) {
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

bool ShowProcessPickerDialog(
    HWND hParent, 
    HINSTANCE hInstance, 
    std::vector<PickedProcess>& outSelected
) {
    s_pSelectedOut = &outSelected;
    INT_PTR res = DialogBoxA(
        hInstance, 
        MAKEINTRESOURCEA(IDD_PROCESS_PICKER), 
        hParent, 
        ProcessPickerDlgProc
    );
    s_pSelectedOut = nullptr;
    return (res == IDOK);
}

} // namespace ati
