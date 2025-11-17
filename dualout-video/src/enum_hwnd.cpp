#include <windows.h>
#include <iostream>

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    wchar_t title[256];
    GetWindowTextW(hwnd, title, 256);
    if (IsWindowVisible(hwnd) && wcslen(title) > 0) {
        std::wcout << L"HWND=0x" << std::hex << (uintptr_t)hwnd
                   << L"  Title=\"" << title << L"\"\n";
    }
    return TRUE;
}

int main() {
    EnumWindows(EnumWindowsProc, 0);
    return 0;
}
