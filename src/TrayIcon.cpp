#include "TrayIcon.hpp"

#include <atomic>
#include <cstddef>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <thread>
#endif

namespace maskui {
namespace {

#if defined(_WIN32)

constexpr UINT WM_TRAY = WM_APP + 21;
constexpr UINT ID_SHOW = 1;
constexpr UINT ID_HIDE = 2;
constexpr UINT ID_OUTPUT = 3;
constexpr UINT ID_STOP = 4;
constexpr UINT ID_QUIT = 5;

// ITaskbarList3, declared by hand. The SDK header is fine on MSVC and the
// MinGW one disagrees about the interface name, and this is four methods.
struct ITaskbarList3Vtbl;
struct ITaskbarList3 { ITaskbarList3Vtbl* lpVtbl; };
struct ITaskbarList3Vtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(ITaskbarList3*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(ITaskbarList3*);
    ULONG   (STDMETHODCALLTYPE* Release)(ITaskbarList3*);
    HRESULT (STDMETHODCALLTYPE* HrInit)(ITaskbarList3*);
    HRESULT (STDMETHODCALLTYPE* AddTab)(ITaskbarList3*, HWND);
    HRESULT (STDMETHODCALLTYPE* DeleteTab)(ITaskbarList3*, HWND);
    HRESULT (STDMETHODCALLTYPE* ActivateTab)(ITaskbarList3*, HWND);
    HRESULT (STDMETHODCALLTYPE* SetActiveAlt)(ITaskbarList3*, HWND);
    HRESULT (STDMETHODCALLTYPE* MarkFullscreenWindow)(ITaskbarList3*, HWND, BOOL);
    HRESULT (STDMETHODCALLTYPE* SetProgressValue)(ITaskbarList3*, HWND,
                                                  ULONGLONG, ULONGLONG);
    HRESULT (STDMETHODCALLTYPE* SetProgressState)(ITaskbarList3*, HWND, int);
};

const GUID kCLSID_TaskbarList =
    {0x56FDF344, 0xFD6D, 0x11d0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
const GUID kIID_ITaskbarList3 =
    {0xEA1AFB91, 0x9E28, 0x4B86, {0x90, 0xE9, 0x9E, 0x9F, 0x8A, 0x5E, 0xEF, 0xAF}};

struct State {
    std::mutex mutex;
    std::deque<TrayIcon::Command> commands;
    std::thread thread;
    HWND messageWindow = nullptr;
    HWND appWindow = nullptr;
    NOTIFYICONDATAW icon{};
    HICON balloonIcon = nullptr;
    ITaskbarList3* taskbar = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> live{false};
    std::atomic<bool> windowVisible{true};
    std::atomic<bool> simulationRunning{false};
};

State& state() {
    static State value;
    return value;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
        wide.data(), size);
    return wide;
}

template <std::size_t N>
void copyInto(wchar_t (&field)[N], const std::wstring& text) {
    const std::size_t count = (text.size() < N - 1) ? text.size() : N - 1;
    std::wmemcpy(field, text.c_str(), count);
    field[count] = L'\0';
}

void push(TrayIcon::Command command) {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.mutex);
    // A queue rather than a single slot: a double click while a menu command
    // is still unread must not eat the earlier one.
    if (s.commands.size() < 16u) {
        s.commands.push_back(command);
    }
}

void popupMenu(HWND window) {
    State& s = state();
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    const bool visible = s.windowVisible.load();
    AppendMenuW(menu, MF_STRING, visible ? ID_HIDE : ID_SHOW,
                visible ? L"Hide the window" : L"Show the window");
    AppendMenuW(menu, MF_STRING, ID_OUTPUT, L"Open the output folder");
    if (s.simulationRunning.load()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_STOP,
                    L"Stop the simulation (keeps the frames)");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit");

    POINT point{};
    GetCursorPos(&point);
    // Documented dance: without these two the menu will not close on a click
    // somewhere else.
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   point.x, point.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM wParam,
                         LPARAM lParam) {
    switch (message) {
        case WM_TRAY:
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                push(state().windowVisible.load()
                         ? TrayIcon::Command::HideWindow
                         : TrayIcon::Command::ShowWindow);
            } else if (LOWORD(lParam) == WM_RBUTTONUP ||
                       LOWORD(lParam) == WM_CONTEXTMENU) {
                popupMenu(window);
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_SHOW:   push(TrayIcon::Command::ShowWindow); break;
                case ID_HIDE:   push(TrayIcon::Command::HideWindow); break;
                case ID_OUTPUT: push(TrayIcon::Command::OpenOutput); break;
                case ID_STOP:   push(TrayIcon::Command::StopSimulation); break;
                case ID_QUIT:   push(TrayIcon::Command::Quit); break;
                default: break;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

void trayThread(std::wstring tooltip) {
    State& s = state();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = wndProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"FluidSolverUiTrayWindow";
    RegisterClassExW(&cls);

    s.messageWindow = CreateWindowExW(
        0, cls.lpszClassName, L"Fluid Solver UI", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (s.messageWindow == nullptr) {
        s.ready.store(true);
        CoUninitialize();
        return;
    }

    s.icon = NOTIFYICONDATAW{};
    s.icon.cbSize = sizeof(s.icon);
    s.icon.hWnd = s.messageWindow;
    s.icon.uID = 1;
    s.icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s.icon.uCallbackMessage = WM_TRAY;
    // Resource 1 is the application icon, put there by src/ui.rc.in. Asking
    // for the small-icon metric picks the 16x16 drawing out of the .ico
    // instead of squashing the large one.
    s.icon.hIcon = static_cast<HICON>(LoadImageW(
        cls.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    if (s.icon.hIcon == nullptr) {
        // IDI_APPLICATION written out: the macro is the ANSI form unless
        // UNICODE is defined, and this project does not define it.
        s.icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }
    s.balloonIcon = static_cast<HICON>(LoadImageW(
        cls.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    copyInto(s.icon.szTip, tooltip);
    s.live.store(Shell_NotifyIconW(NIM_ADD, &s.icon) != FALSE);

    if (SUCCEEDED(CoCreateInstance(kCLSID_TaskbarList, nullptr,
                                   CLSCTX_INPROC_SERVER, kIID_ITaskbarList3,
                                   reinterpret_cast<void**>(&s.taskbar)))) {
        if (FAILED(s.taskbar->lpVtbl->HrInit(s.taskbar))) {
            s.taskbar->lpVtbl->Release(s.taskbar);
            s.taskbar = nullptr;
        }
    }

    s.ready.store(true);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (s.live.load()) {
        Shell_NotifyIconW(NIM_DELETE, &s.icon);
        s.live.store(false);
    }
    if (s.taskbar != nullptr) {
        s.taskbar->lpVtbl->Release(s.taskbar);
        s.taskbar = nullptr;
    }
    CoUninitialize();
}

#endif   // _WIN32

}   // namespace

TrayIcon::~TrayIcon() {
    detach();
}

void TrayIcon::attach(void* windowHandle, const std::string& tooltip) {
#if defined(_WIN32)
    if (attached_) {
        return;
    }
    State& s = state();
    s.appWindow = static_cast<HWND>(windowHandle);
    s.ready.store(false);
    s.thread = std::thread(trayThread, widen(tooltip));
    // A few milliseconds at most, and the icon has to exist before the first
    // setProgress tries to change its tooltip.
    while (!s.ready.load()) {
        Sleep(5);
    }
    attached_ = true;
#else
    (void)windowHandle;
    (void)tooltip;
#endif
}

void TrayIcon::detach() {
#if defined(_WIN32)
    if (!attached_) {
        return;
    }
    State& s = state();
    if (s.messageWindow != nullptr) {
        PostMessageW(s.messageWindow, WM_CLOSE, 0, 0);
        s.messageWindow = nullptr;
    }
    if (s.thread.joinable()) {
        s.thread.join();
    }
    attached_ = false;
#endif
}

bool TrayIcon::available() const {
#if defined(_WIN32)
    return attached_ && state().live.load();
#else
    return false;
#endif
}

void TrayIcon::setProgress(bool running, double fraction,
                           const std::string& tooltip) {
#if defined(_WIN32)
    if (!attached_) {
        return;
    }
    State& s = state();
    s.simulationRunning.store(running);
    if (s.live.load()) {
        copyInto(s.icon.szTip, widen(tooltip));
        s.icon.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &s.icon);
    }
    if (s.taskbar != nullptr && s.appWindow != nullptr) {
        if (!running) {
            s.taskbar->lpVtbl->SetProgressState(s.taskbar, s.appWindow, 0x0);
        } else if (fraction < 0.0) {
            // Length unknown: the marquee says "working" without claiming to
            // know how far along it is.
            s.taskbar->lpVtbl->SetProgressState(s.taskbar, s.appWindow, 0x1);
        } else {
            s.taskbar->lpVtbl->SetProgressState(s.taskbar, s.appWindow, 0x2);
            const double clamped = fraction > 1.0 ? 1.0 : fraction;
            s.taskbar->lpVtbl->SetProgressValue(
                s.taskbar, s.appWindow,
                static_cast<ULONGLONG>(clamped * 1000.0), 1000ull);
        }
    }
#else
    (void)running;
    (void)fraction;
    (void)tooltip;
#endif
}

void TrayIcon::notify(const std::string& title, const std::string& text,
                      bool problem) {
#if defined(_WIN32)
    State& s = state();
    if (!attached_ || !s.live.load()) {
        return;
    }
    NOTIFYICONDATAW data = s.icon;
    data.uFlags = NIF_INFO;
    if (s.balloonIcon != nullptr && !problem) {
        data.hBalloonIcon = s.balloonIcon;
        data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    } else {
        data.dwInfoFlags = problem ? NIIF_WARNING : NIIF_INFO;
    }
    copyInto(data.szInfoTitle, widen(title));
    copyInto(data.szInfo, widen(text));
    Shell_NotifyIconW(NIM_MODIFY, &data);
#else
    (void)title;
    (void)text;
    (void)problem;
#endif
}

void TrayIcon::setWindowVisible(bool visible) {
#if defined(_WIN32)
    state().windowVisible.store(visible);
#else
    (void)visible;
#endif
}

void TrayIcon::setSimulationRunning(bool running) {
#if defined(_WIN32)
    state().simulationRunning.store(running);
#else
    (void)running;
#endif
}

TrayIcon::Command TrayIcon::takeCommand() {
#if defined(_WIN32)
    if (!attached_) {
        return Command::None;
    }
    State& s = state();
    std::lock_guard<std::mutex> guard(s.mutex);
    if (s.commands.empty()) {
        return Command::None;
    }
    const Command command = s.commands.front();
    s.commands.pop_front();
    return command;
#else
    return Command::None;
#endif
}

}   // namespace maskui
