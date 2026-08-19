// OpenXR Simulator Manager - system-tray desktop app (pure Win32, no framework).
//
// Gives ordinary users a one-click way to activate the OpenXR Simulator as the
// active OpenXR runtime. Activation writes HKLM\...\ActiveRuntime (machine-wide)
// because on the target machines the OpenXR loader only honors HKLM. Writing
// HKLM requires administrator rights, so the manager self-elevates via UAC at
// startup. Deactivating deletes the HKLM value (falling back to whatever the
// system default was). Quitting the app also deactivates (session-scoped).
//
// Build with /MT (static CRT) so the .exe is self-contained and does not depend
// on a specific MSVCP140 version in the host process -- same rationale as the
// runtime DLL.

// NOTE: WIN32_LEAN_AND_MEAN / NOMINMAX / UNICODE / _UNICODE /
// _CRT_SECURE_NO_WARNINGS are supplied by CMake target_compile_definitions,
// so they are intentionally not redefined here (avoids C4005 warnings).

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

// --- tray icon identity ---
#define WM_TRAYICON (WM_USER + 1)
#define IDM_SETTINGS 1001
#define IDM_EXIT     1002

static const wchar_t* TRAY_CLASS = L"OpenXRSimTrayClass";
static const wchar_t* SETTINGS_CLASS = L"OpenXRSimSettingsClass";
static const wchar_t* TRAY_TITLE = L"OpenXR Simulator Manager";
static NOTIFYICONDATAW g_nid = {0};
static bool g_active = false;
static HWND g_settingsHwnd = nullptr;

// OpenXR registry + json layout (mirrors scripts/register-runtime.ps1).
// HKLM is required: on this machine Godot's OpenXR loader only honors the
// machine-wide ActiveRuntime, not the per-user HKCU one. Writing HKLM needs
// administrator rights, so the manager self-elevates via UAC at startup.
static const HKEY  REG_ROOT  = HKEY_LOCAL_MACHINE;
static const wchar_t* REG_PATH = L"SOFTWARE\\Khronos\\OpenXR\\1";
static const wchar_t* REG_VALUE = L"ActiveRuntime";

// The whole runtime lives next to this .exe (the unzipped folder): the dll is
// already there, and we write active_runtime.json next to it with an absolute
// library_path. HKLM\ActiveRuntime then points at that json. No copy, no
// %LOCALAPPDATA% -- the unzipped folder *is* the runtime folder; moving/deleting
// it is an uninstall (re-enable after moving to repoint HKLM).
static bool BuildRuntimePaths(std::wstring& outJsonPath, std::wstring& outDllPath) {
    wchar_t exePath[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;

    fs::path dir = fs::path(exePath).parent_path();
    fs::path dll = dir / L"openxr_simulator.dll";
    if (!fs::exists(dll)) return false;
    outDllPath  = dll.wstring();
    outJsonPath = (dir / L"active_runtime.json").wstring();
    return true;
}

static bool WriteActiveRuntimeJson(const std::wstring& jsonPath, const std::wstring& dllPath) {
    std::error_code ec;
    fs::create_directories(fs::path(jsonPath).parent_path(), ec);
    // Escape backslashes for JSON.
    std::wstring escaped;
    escaped.reserve(dllPath.size() + 4);
    for (wchar_t c : dllPath) {
        if (c == L'\\') escaped += L"\\\\";
        else escaped += c;
    }
    std::wstring content =
        L"{\n"
        L"    \"file_format_version\": \"1.0.0\",\n"
        L"    \"runtime\": {\n"
        L"        \"library_path\": \"" + escaped + L"\"\n"
        L"    }\n"
        L"}";
    // The OpenXR loader expects a UTF-8 manifest. Convert the wide string to
    // UTF-8 and write it as bytes (no BOM -- the loader tolerates a BOM but
    // plain UTF-8 is the canonical form and avoids any ambiguity).
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, content.c_str(),
                                      (int)content.size(), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return false;
    std::vector<char> utf8(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), (int)content.size(),
                        utf8.data(), utf8Len, nullptr, nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, jsonPath.c_str(), L"wb") != 0 || !f) return false;
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
    return true;
}

static bool Activate() {
    std::wstring jsonPath, dllPath;
    if (!BuildRuntimePaths(jsonPath, dllPath)) return false;

    // Manifest points at the dll by absolute path. (A relative library_path is
    // not resolved relative to the manifest by all loaders, so absolute is the
    // portable choice.) Re-running Enable after moving the folder rewrites this
    // with the new absolute location.
    if (!WriteActiveRuntimeJson(jsonPath, dllPath)) return false;

    // Write HKLM\...\ActiveRuntime = "<jsonPath>" (machine-wide; needs admin,
    // which the manager already has via UAC self-elevation at startup).
    HKEY key = nullptr;
    LONG r = RegCreateKeyExW(REG_ROOT, REG_PATH, 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (r != ERROR_SUCCESS) return false;
    r = RegSetValueExW(key, REG_VALUE, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(jsonPath.c_str()),
                       static_cast<DWORD>((jsonPath.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static void Deactivate() {
    HKEY key = nullptr;
    LONG r = RegOpenKeyExW(REG_ROOT, REG_PATH, 0, KEY_SET_VALUE, &key);
    if (r == ERROR_SUCCESS) {
        RegDeleteValueW(key, REG_VALUE);
        RegCloseKey(key);
    }
}

// Validate that the manifest referenced by HKLM\ActiveRuntime is actually usable:
// the json file exists, it contains an ASCII "library_path" key (a UTF-16
// manifest would not match, which is exactly the corruption we want to detect),
// and the dll it points at exists on disk. If any check fails the activation is
// stale/broken and IsActive reports false so the UI shows Disabled.
static bool ValidateManifest(const std::wstring& jsonPath) {
    if (!fs::exists(jsonPath)) return false;
    // Read as bytes and look for the ASCII "library_path" marker. A valid
    // UTF-8 manifest contains it; a UTF-16 manifest does not.
    std::ifstream f(jsonPath, std::ios::binary);
    if (!f) return false;
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const char marker[] = "library_path";
    auto pos = raw.find(marker);
    if (pos == std::string::npos) return false;
    // Extract the quoted path after the marker.
    auto q1 = raw.find('"', pos + sizeof(marker) - 1);
    if (q1 == std::string::npos) return false;
    auto q2 = raw.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    std::string pathBytes(raw.begin() + q1 + 1, raw.begin() + q2);
    // JSON-escape: unescape \" and \\.
    std::wstring dllPath;
    for (size_t i = 0; i < pathBytes.size(); ++i) {
        if (pathBytes[i] == '\\' && i + 1 < pathBytes.size()) {
            dllPath.push_back((wchar_t)pathBytes[++i]);
        } else {
            dllPath.push_back((wchar_t)(unsigned char)pathBytes[i]);
        }
    }
    return !dllPath.empty() && fs::exists(dllPath);
}

static bool IsActive() {
    HKEY key = nullptr;
    LONG r = RegOpenKeyExW(REG_ROOT, REG_PATH, 0, KEY_QUERY_VALUE, &key);
    if (r != ERROR_SUCCESS) return false;
    wchar_t buf[512] = {0};
    DWORD sz = sizeof(buf);
    r = RegGetValueW(key, nullptr, REG_VALUE, RRF_RT_REG_SZ, nullptr, buf, &sz);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || buf[0] == L'\0') return false;
    // Registry has a value, but the manifest/dll it points at must also be valid.
    return ValidateManifest(buf);
}

// --- settings window look & feel ---
static HFONT  g_hFont      = nullptr;  // base UI font (Segoe UI)
static HFONT  g_hFontBold  = nullptr;  // title font
static HBRUSH g_hBgBrush   = nullptr;  // window background brush
static const int BTN_TOGGLE = 1;
#define MAKEHMENU(id) ((HMENU)(INT_PTR)(id))

static void EnsureGdiResources() {
    if (g_hFont) return;
    g_hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_hFontBold = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_hBgBrush = CreateSolidBrush(RGB(0xF3, 0xF4, 0xF6)); // light gray
}

// Draw the single primary action button with rounded corners and an accent fill.
static void DrawToggleButton(LPDRAWITEMSTRUCT dis) {
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    // Enable (not active) is the primary action -> blue accent to invite click.
    // Disable (active) is a destructive/secondary state -> gray.
    COLORREF fill = g_active ? RGB(0x6B, 0x72, 0x80)   // gray  when active (Disable)
                             : RGB(0x15, 0x65, 0xC0);   // blue  when inactive (Enable)
    if (pressed) fill = g_active ? RGB(0x4B, 0x51, 0x5C) : RGB(0x0D, 0x47, 0xA1);

    HBRUSH b = CreateSolidBrush(fill);
    HPEN   p = CreatePen(PS_NULL, 0, 0);
    HBRUSH oldB = (HBRUSH)SelectObject(dc, b);
    HPEN   oldP = (HPEN)SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 8, 8);
    SelectObject(dc, oldB); SelectObject(dc, oldP);
    DeleteObject(b); DeleteObject(p);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    HFONT oldF = (HFONT)SelectObject(dc, g_hFont);
    DrawTextW(dc, g_active ? L"Disable" : L"Enable",
              -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldF);
}

// Lay out the controls relative to the current client area so nothing is
// clipped or pinned to the window edge.
static void LayoutSettingsControls(HWND hwnd) {
    RECT cr; GetClientRect(hwnd, &cr);
    int w = cr.right;
    int margin = 24;
    int btnW = w - margin * 2;
    int btnH = 40;
    int btnY = cr.bottom - margin - btnH;          // 24px above the bottom edge
    SetWindowPos(GetDlgItem(hwnd, BTN_TOGGLE), nullptr, margin, btnY, btnW, btnH, SWP_NOZORDER);
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            EnsureGdiResources();
            g_settingsHwnd = hwnd;
            CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                            24, 100, 200, 40, hwnd, MAKEHMENU(BTN_TOGGLE),
                            (HINSTANCE)GetModuleHandleW(nullptr), nullptr);
            LayoutSettingsControls(hwnd);
            return 0;
        }
        case WM_SIZE:
            LayoutSettingsControls(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_ERASEBKGND: {
            RECT cr; GetClientRect(hwnd, &cr);
            FillRect((HDC)wParam, &cr, g_hBgBrush);
            return 1;
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            if (dis->CtlID == BTN_TOGGLE) DrawToggleButton(dis);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr; GetClientRect(hwnd, &cr);
            int margin = 24;
            HFONT oldF = (HFONT)SelectObject(dc, g_hFontBold);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x12, 0x12, 0x12));
            RECT title = {margin, 22, cr.right - margin, 48};
            DrawTextW(dc, L"OpenXR Simulator", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, g_hFont);
            RECT sub = {margin, 50, cr.right - margin, 74};
            SetTextColor(dc, RGB(0x70, 0x70, 0x70));
            DrawTextW(dc, L"Active OpenXR runtime for this machine", -1, &sub,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Status line: "Status:" in neutral gray, value in state color.
            // Value color is opposite to the button: active=blue, inactive=gray.
            int statusY = (cr.bottom - 24 - 40) - 34;   // match LayoutSettingsControls
            RECT sLabel = {margin, statusY, cr.right - margin, statusY + 22};
            SetTextColor(dc, RGB(0x70, 0x70, 0x70));
            DrawTextW(dc, L"Status:", -1, &sLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            // Measure "Status:" width to place the value right after it.
            SIZE labelSz = {0};
            GetTextExtentPoint32W(dc, L"Status:  ", 9, &labelSz);
            RECT sValue = {margin + (int)labelSz.cx, statusY, cr.right - margin, statusY + 22};
            SetTextColor(dc, g_active ? RGB(0x15, 0x65, 0xC0) : RGB(0x6B, 0x72, 0x80));
            DrawTextW(dc, g_active ? L"Enabled" : L"Disabled", -1, &sValue,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldF);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == BTN_TOGGLE) {
                if (g_active) {
                    Deactivate();
                    g_active = false;
                } else {
                    g_active = Activate();
                    if (!g_active) {
                        MessageBoxW(hwnd, L"Activation failed: openxr_simulator.dll not found next to this app.",
                                    TRAY_TITLE, MB_OK | MB_ICONERROR);
                    }
                }
                // Status line is self-drawn; just repaint.
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void ShowSettingsDialog() {
    if (g_settingsHwnd && IsWindow(g_settingsHwnd)) {
        ShowWindow(g_settingsHwnd, SW_SHOW);
        SetForegroundWindow(g_settingsHwnd);
        return;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = (HINSTANCE)GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = SETTINGS_CLASS;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    // Compute window size from a desired *client* area so nothing is clipped.
    RECT wr = {0, 0, 360, 200};
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    AdjustWindowRect(&wr, style, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, SETTINGS_CLASS, TRAY_TITLE,
                                style,
                                CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
                                nullptr, nullptr, (HINSTANCE)GetModuleHandleW(nullptr), nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

static void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                ShowTrayMenu(hwnd);
            }
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDM_SETTINGS) {
                ShowSettingsDialog();
            } else if (id == IDM_EXIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DESTROY:
            // Quitting deactivates (session-scoped activation).
            Deactivate();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Returns true if the current process is running elevated (in the admin group).
static bool IsElevated() {
    BOOL f = FALSE;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION e = {0};
        DWORD ret = 0;
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &ret)) {
            f = e.TokenIsElevated;
        }
        CloseHandle(tok);
    }
    return f != FALSE;
}

// Re-launch this executable with runas to obtain admin rights. Returns true if
// the elevated instance was (apparently) started, false if the user refused UAC
// or the launch failed.
static bool RelaunchElevated() {
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    si.lpVerb = L"runas";
    si.lpFile = exe;
    si.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&si)) {
        return false;
    }
    return true;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // HKLM activation needs administrator rights. Self-elevate at startup.
    if (!IsElevated()) {
        if (!RelaunchElevated()) {
            MessageBoxW(nullptr,
                L"OpenXR Simulator Manager needs administrator rights to set the "
                L"machine-wide OpenXR runtime. Please launch it again and accept "
                L"the UAC prompt.",
                L"OpenXR Simulator Manager", MB_OK | MB_ICONERROR);
        }
        return 0;
    }

    // Single-instance guard: only one manager per user session.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\OpenXRSimulatorManager");
    if (hMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        MessageBoxW(nullptr,
            L"OpenXR Simulator Manager is already running. "
            L"Check the system tray (near the clock) for its icon.",
            L"OpenXR Simulator Manager", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = TRAY_CLASS;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    // Hidden message-only window to receive tray callbacks.
    HWND hwnd = CreateWindowExW(0, TRAY_CLASS, TRAY_TITLE, 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    g_active = IsActive();

    // Open the settings window on launch so new users see how to enable it.
    ShowSettingsDialog();

    // Add tray icon.
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, TRAY_TITLE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
