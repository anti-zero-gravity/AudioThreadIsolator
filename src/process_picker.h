#pragma once

#undef UNICODE
#undef _UNICODE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace ati {

struct PickedProcess {
    std::string processName;
    std::string exeFileName;
    DWORD currentPriorityClass;
};

// Show Process Picker Dialog
bool ShowProcessPickerDialog(
    HWND hParent, 
    HINSTANCE hInstance, 
    std::vector<PickedProcess>& outSelected
);

} // namespace ati
