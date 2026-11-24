// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// The DemoTools2 window: its own small window drawn with OpenGL, next to / on top of CS2.
//
// Why not inside the game's frame: HLAE hooks CS2's DirectX 11 swap chain, device and render targets. Creating our own
// swap chain or render target view there replaces objects HLAE is tracking and crashed the game (1.2/1.3). A separate
// OpenGL window shares nothing with that, and needs no input hacks: it simply gets the mouse when you click it.
#include "render.h"
#include "skins.h"
#include "hotkeys.h"
#include "ui.h"
#include "util.h"

#include <dwmapi.h>

#include <GL/gl.h>
#include "imgui.h"
#include "backends/imgui_impl_opengl2.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace Menu { void Draw(bool fillWindow); }

namespace Render {
bool open = false;
static HWND g_wnd = nullptr;
static HWND g_gameWnd = nullptr;
static bool g_topmost = true;
static float g_newScale = 0;   // set when the window moved to a screen with another DPI

// Only this thread's windows become DPI-aware (sharp text on high-DPI screens); CS2's own window is not affected.
static void MakeThreadDpiAware() {
    using SetCtxFn = HANDLE(WINAPI*)(HANDLE);
    if (auto fn = (SetCtxFn)GetProcAddress(GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext"))
        fn((HANDLE)(intptr_t)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}
static float WindowScale(HWND w) {
    using GetDpiFn = UINT(WINAPI*)(HWND);
    if (auto fn = (GetDpiFn)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"))
        if (UINT dpi = fn(w)) return dpi / 96.f;
    return 1.f;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_MOUSEWHEEL && ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_MOUSEWHEEL:   // smooth scrolling: the menu glides to the new position (see Ui::SmoothScroll)
            Ui::AddWheel((float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA);
            return 0;
        case 0x02E0: {        // WM_DPICHANGED: moved to a screen with another scale
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            g_newScale = HIWORD(wp) / 96.f;
            return 0;
        }
        case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); open = false; return 0;   // closing only hides it
        case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool OurProcess(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return w && pid == GetCurrentProcessId();
}

static void Show(bool show) {
    open = show;
    if (show) {
        ShowWindow(g_wnd, SW_SHOWNORMAL);
        SetWindowPos(g_wnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(g_wnd);
    } else {
        ShowWindow(g_wnd, SW_HIDE);
        if (g_gameWnd) SetForegroundWindow(g_gameWnd);
    }
}

static DWORD WINAPI Thread(LPVOID) {
    Step step("menu window");
    FirstRunBegin("menu-setup");
    MakeThreadDpiAware();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc), CS_OWNDC, WndProc, 0, 0, inst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"DemoTools2Menu", nullptr};
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST, L"DemoTools2Menu", L"DemoTools2 - CS2 demo tools (Insert to show/hide)",
                            WS_OVERLAPPEDWINDOW, 120, 120, 1120, 720, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) { Log("ERROR: could not create the menu window"); return 0; }
    float scale = WindowScale(g_wnd);
    if (scale > 1.01f) {   // created at 96-dpi size: grow it to the same size in real pixels
        RECT r;
        GetWindowRect(g_wnd, &r);
        SetWindowPos(g_wnd, nullptr, 0, 0, (int)((r.right - r.left) * scale), (int)((r.bottom - r.top) * scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    {   // dark title bar (Windows 10 20H1+ / 11), rounded corners on Windows 11
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(g_wnd, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(g_wnd, 19, &dark, sizeof(dark));
        int round = 2;   // DWMWCP_ROUND
        DwmSetWindowAttribute(g_wnd, 33, &round, sizeof(round));
        COLORREF cap = RGB(0x10, 0x13, 0x17);
        DwmSetWindowAttribute(g_wnd, 35, &cap, sizeof(cap));   // caption colour (Windows 11)
    }

    HDC dc = GetDC(g_wnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { Log("ERROR: no OpenGL pixel format"); return 0; }
    HGLRC gl = wglCreateContext(dc);
    if (!gl || !wglMakeCurrent(dc, gl)) { Log("ERROR: could not create an OpenGL context"); return 0; }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplWin32_InitForOpenGL(g_wnd);
    ImGui_ImplOpenGL2_Init();
    Ui::LoadFonts();
    Ui::ApplyTheme(scale);

    // Draw in step with the screen (smooth, no tearing). Without it, fall back to ~60 frames a second.
    using SwapIntervalFn = BOOL(WINAPI*)(int);
    bool vsync = false;
    if (auto si = (SwapIntervalFn)wglGetProcAddress("wglSwapIntervalEXT")) vsync = si(1) != FALSE;
    FirstRunEnd("menu-setup");
    Log("menu window ready - press Insert in CS2 to show it");

    Show(true);   // show once at start so you can see DemoTools2 is running
    bool keyWas = false;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

        // Insert (or the key set in DemoTools2.cfg) toggles the window while CS2 or the window itself is in front.
        HWND fg = GetForegroundWindow();
        if (OurProcess(fg) && fg != g_wnd) g_gameWnd = fg;
        bool down = OurProcess(fg) && (GetAsyncKeyState(Skins::settings.menuKey) & 0x8000);
        if (down && !keyWas) Show(!open);
        keyWas = down;
        Hotkeys::Poll(OurProcess(fg) && fg != g_wnd);   // cinematics hotkeys, only while CS2 itself is in front

        if (!open || IsIconic(g_wnd)) { Sleep(30); continue; }
        if (g_newScale > 0) { Ui::ApplyTheme(g_newScale); g_newScale = 0; }

        Step frame("drawing the menu");
        FirstRunBegin("menu-frame");
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Ui::BeginFrame();
        Menu::Draw(true);
        Ui::EndFrame();
        ImGui::Render();
        RECT rc;
        GetClientRect(g_wnd, &rc);
        glViewport(0, 0, rc.right - rc.left, rc.bottom - rc.top);
        glClearColor(0x10 / 255.f, 0x13 / 255.f, 0x17 / 255.f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(dc);
        FirstRunEnd("menu-frame");
        // Smooth while you use it; light when it just sits behind the game (keeps CS2's frame rate up).
        POINT cur;
        RECT wr;
        const bool inUse = GetForegroundWindow() == g_wnd || (GetCursorPos(&cur) && GetWindowRect(g_wnd, &wr) && PtInRect(&wr, cur));
        if (!inUse) Sleep(33);
        else if (!vsync) Sleep(14);
    }
}

bool Install() {
    HANDLE t = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
    if (!t) return false;
    CloseHandle(t);
    return true;
}
}  // namespace Render
