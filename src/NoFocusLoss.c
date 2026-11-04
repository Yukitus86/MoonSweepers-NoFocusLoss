/*
 * NoFocusLoss - Moon Sweepers mod
 *
 * Keeps the game "focused" while you alt-tab to another window, so your vehicle
 * (cruise control / autopilot / held keys) keeps driving instead of stopping.
 *
 * How it works:
 *   - Ships as a proxy "version.dll". UnityPlayer.dll imports version.dll, so
 *     Windows loads this file from the game folder. All real version.dll
 *     functions are forwarded to C:\Windows\System32\version.dll.
 *   - The Unity window procedure is wrapped. "You lost focus" messages
 *     (WM_ACTIVATE inactive, WM_ACTIVATEAPP false, WM_KILLFOCUS) are hidden
 *     from Unity, so neither the engine, the Input System nor the game scripts
 *     ever see the focus loss (no OnApplicationFocus(false), no input reset,
 *     no pause).
 *   - A few USER32 imports of UnityPlayer.dll are hooked (IAT) so focus queries
 *     report the game window, while the mouse cursor is never trapped and no
 *     keyboard/mouse input is read while another window is really in front.
 *
 * No mod loader required. Works independent of the IL2CPP metadata version.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define NFL_VERSION "1.0.0"

/* ------------------------------------------------------------------------ */
/* Config                                                                   */
/* ------------------------------------------------------------------------ */

static struct {
    int enabled;              /* master switch                                  */
    int keepHeldKeys;         /* keys held while tabbing out stay pressed        */
    int blockBackgroundInput; /* ignore keyboard/mouse while game is in back     */
    int releaseCursor;        /* never confine / warp the cursor in background   */
    int log;                  /* write NoFocusLoss.log                           */
} g_cfg = { 1, 1, 1, 1, 1 };

static wchar_t g_dir[MAX_PATH];
static wchar_t g_logPath[MAX_PATH];
static CRITICAL_SECTION g_logCs;

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

static void Log(const char *fmt, ...)
{
    char buf[1024];
    SYSTEMTIME st;
    va_list ap;
    int n;
    HANDLE f;
    DWORD written;

    if (!g_cfg.log || !g_logPath[0])
        return;

    GetLocalTime(&st);
    n = sprintf_s(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(buf) - 3)
        n = (int)strlen(buf);
    buf[n++] = '\r';
    buf[n++] = '\n';

    EnterCriticalSection(&g_logCs);
    f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, buf, (DWORD)n, &written, NULL);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logCs);
}

static void LoadConfig(void)
{
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, MAX_PATH, L"%sNoFocusLoss.ini", g_dir);
    g_cfg.enabled              = GetPrivateProfileIntW(L"NoFocusLoss", L"Enabled", 1, ini) != 0;
    g_cfg.keepHeldKeys         = GetPrivateProfileIntW(L"NoFocusLoss", L"KeepHeldKeys", 1, ini) != 0;
    g_cfg.blockBackgroundInput = GetPrivateProfileIntW(L"NoFocusLoss", L"BlockBackgroundInput", 1, ini) != 0;
    g_cfg.releaseCursor        = GetPrivateProfileIntW(L"NoFocusLoss", L"ReleaseCursor", 1, ini) != 0;
    g_cfg.log                  = GetPrivateProfileIntW(L"NoFocusLoss", L"Log", 1, ini) != 0;
}

/* ------------------------------------------------------------------------ */
/* version.dll forwarding                                                   */
/* ------------------------------------------------------------------------ */

typedef INT_PTR (WINAPI *Fwd8)(INT_PTR, INT_PTR, INT_PTR, INT_PTR,
                               INT_PTR, INT_PTR, INT_PTR, INT_PTR);
static HMODULE volatile g_realVersion;

static FARPROC RealVersionProc(const char *name)
{
    if (!g_realVersion) {
        wchar_t path[MAX_PATH];
        UINT len = GetSystemDirectoryW(path, MAX_PATH);
        HMODULE m;
        if (len == 0 || len >= MAX_PATH - 16)
            return NULL;
        wcscat_s(path, MAX_PATH, L"\\version.dll");
        m = LoadLibraryW(path);
        if (InterlockedCompareExchangePointer((PVOID volatile *)&g_realVersion, m, NULL) != NULL && m)
            FreeLibrary(m); /* another thread won the race; drop our extra ref */
    }
    return g_realVersion ? GetProcAddress(g_realVersion, name) : NULL;
}

/* x64 has a single calling convention: forwarding 8 integer-sized args is safe
 * for every version.dll export (max 8 params, no floats). */
#define FORWARD(name)                                                              \
    INT_PTR WINAPI p_##name(INT_PTR a, INT_PTR b, INT_PTR c, INT_PTR d,            \
                            INT_PTR e, INT_PTR f, INT_PTR g, INT_PTR h)            \
    {                                                                              \
        static Fwd8 fn;                                                            \
        if (!fn) fn = (Fwd8)RealVersionProc(#name);                                \
        if (!fn) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }                 \
        return fn(a, b, c, d, e, f, g, h);                                         \
    }

FORWARD(GetFileVersionInfoA)
FORWARD(GetFileVersionInfoByHandle)
FORWARD(GetFileVersionInfoExA)
FORWARD(GetFileVersionInfoExW)
FORWARD(GetFileVersionInfoSizeA)
FORWARD(GetFileVersionInfoSizeExA)
FORWARD(GetFileVersionInfoSizeExW)
FORWARD(GetFileVersionInfoSizeW)
FORWARD(GetFileVersionInfoW)
FORWARD(VerFindFileA)
FORWARD(VerFindFileW)
FORWARD(VerInstallFileA)
FORWARD(VerInstallFileW)
FORWARD(VerLanguageNameA)
FORWARD(VerLanguageNameW)
FORWARD(VerQueryValueA)
FORWARD(VerQueryValueW)

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

static HWND volatile    g_hwnd;              /* main Unity window                 */
static WNDPROC volatile g_origClassProc;     /* Unity's proc (class hook)         */
static WNDPROC volatile g_origSubclassProc;  /* Unity's proc (subclass fallback)  */
static volatile LONG    g_fakeFocus;         /* 1 while we hide a real focus loss */
static volatile BYTE    g_keyDown[256];      /* keys Unity currently sees as down */
static DWORD            g_pid;

/* Real USER32 functions */
static ATOM    (WINAPI *real_RegisterClassExW)(const WNDCLASSEXW *);
static ATOM    (WINAPI *real_RegisterClassW)(const WNDCLASSW *);
static HWND    (WINAPI *real_GetForegroundWindow)(void);
static HWND    (WINAPI *real_GetActiveWindow)(void);
static HWND    (WINAPI *real_GetFocus)(void);
static BOOL    (WINAPI *real_ClipCursor)(const RECT *);
static BOOL    (WINAPI *real_SetCursorPos)(int, int);
static SHORT   (WINAPI *real_GetAsyncKeyState)(int);
static BOOL    (WINAPI *real_RegisterRawInputDevices)(PCRAWINPUTDEVICE, UINT, UINT);
static LONG_PTR(WINAPI *real_SetWindowLongPtrW)(HWND, int, LONG_PTR);
static BOOL    (WINAPI *real_SetForegroundWindow)(HWND);
static HWND    (WINAPI *real_SetFocus)(HWND);
static BOOL    (WINAPI *real_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static BOOL    (WINAPI *real_ShowWindow)(HWND, int);

static LRESULT CALLBACK NFL_WndProc(HWND h, UINT m, WPARAM w, LPARAM l);

static BOOL IsOwnWindow(HWND h)
{
    DWORD pid = 0;
    if (!h)
        return FALSE;
    GetWindowThreadProcessId(h, &pid);
    return pid == g_pid;
}

/* TRUE if a window of the game process is really in the foreground. */
static BOOL IsRealForeground(void)
{
    return IsOwnWindow(real_GetForegroundWindow());
}

static BOOL IsUnityClassName(LPCWSTR name)
{
    return name && !IS_INTRESOURCE(name) && _wcsicmp(name, L"UnityWndClass") == 0;
}

static WNDPROC CurrentOrigProc(void)
{
    WNDPROC p = g_origSubclassProc;
    return p ? p : g_origClassProc;
}

/* ------------------------------------------------------------------------ */
/* Focus faking                                                             */
/* ------------------------------------------------------------------------ */

static BOOL IsModifierVk(UINT vk)
{
    switch (vk) {
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_LWIN: case VK_RWIN: case VK_TAB:
        return TRUE;
    }
    return FALSE;
}

/* Send key-up messages to Unity for keys it still believes are held.
 * Modifiers (Alt from Alt+Tab, Win, ...) are always released; other keys only
 * when KeepHeldKeys=0. */
static void ReleaseKeys(HWND h, WNDPROC orig)
{
    UINT vk;
    for (vk = 1; vk < 256; vk++) {
        UINT scan;
        LPARAM lp;
        UINT msg;
        if (!g_keyDown[vk])
            continue;
        if (g_cfg.keepHeldKeys && !IsModifierVk(vk))
            continue;
        g_keyDown[vk] = 0;
        scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        lp = (LPARAM)(1u | (scan << 16) | (1u << 30) | (1u << 31));
        if (vk == VK_RMENU || vk == VK_RCONTROL || vk == VK_RWIN || vk == VK_LWIN)
            lp |= (LPARAM)(1u << 24);
        msg = (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_F10)
                  ? WM_SYSKEYUP : WM_KEYUP;
        CallWindowProcW(orig, h, msg, vk, lp);
    }
}

static void OnFakeDeactivate(HWND h, WNDPROC orig, const char *why)
{
    if (InterlockedExchange(&g_fakeFocus, 1) == 0) {
        if (g_cfg.releaseCursor && real_ClipCursor)
            real_ClipCursor(NULL);
        ReleaseKeys(h, orig);
        Log("Focus loss hidden from game (%s) - game keeps running.", why);
    }
}

static void OnReactivate(void)
{
    if (InterlockedExchange(&g_fakeFocus, 0) == 1)
        Log("Game window is in front again.");
}

static LRESULT CALLBACK NFL_WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig = CurrentOrigProc();
    if (!orig)
        return DefWindowProcW(h, m, w, l);

    /* Track which top-level Unity window is the game window. */
    if (!g_hwnd && (m == WM_NCCREATE || m == WM_CREATE || m == WM_ACTIVATE || m == WM_SHOWWINDOW)
        && !GetParent(h)) {
        g_hwnd = h;
        Log("Game window attached (hwnd=%p).", (void *)h);
    }
    if (m == WM_NCDESTROY && h == g_hwnd) {
        g_hwnd = NULL;
        return CallWindowProcW(orig, h, m, w, l);
    }

    if (!g_cfg.enabled || h != g_hwnd)
        return CallWindowProcW(orig, h, m, w, l);

    switch (m) {
    case WM_ACTIVATEAPP:
        if (!w) {
            OnFakeDeactivate(h, orig, "WM_ACTIVATEAPP");
            return 0;
        }
        OnReactivate();
        break;

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            if (!IsOwnWindow((HWND)l)) {
                OnFakeDeactivate(h, orig, "WM_ACTIVATE");
                return 0;
            }
        } else {
            OnReactivate();
        }
        break;

    case WM_NCACTIVATE:
        /* Let Windows draw the inactive title bar, but don't tell Unity. */
        if (!w && !IsRealForeground())
            return DefWindowProcW(h, m, w, l);
        break;

    case WM_KILLFOCUS:
        if (!IsOwnWindow((HWND)w)) {
            OnFakeDeactivate(h, orig, "WM_KILLFOCUS");
            return 0;
        }
        break;

    case WM_SETFOCUS:
        OnReactivate();
        break;

    case WM_INPUT:
        /* Raw input that arrives only because of RIDEV_INPUTSINK = typed into
         * another app. Drop it so it doesn't control the game. */
        if (g_cfg.blockBackgroundInput && GET_RAWINPUT_CODE_WPARAM(w) == RIM_INPUTSINK
            && !IsRealForeground())
            return DefWindowProcW(h, m, w, l);
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (w < 256) g_keyDown[w] = 1;
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (w < 256) g_keyDown[w] = 0;
        break;
    }
    return CallWindowProcW(orig, h, m, w, l);
}

/* ------------------------------------------------------------------------ */
/* USER32 hooks (installed into UnityPlayer.dll's import table)             */
/* ------------------------------------------------------------------------ */

static ATOM WINAPI H_RegisterClassExW(const WNDCLASSEXW *wc)
{
    if (wc && IsUnityClassName(wc->lpszClassName) && !g_origClassProc) {
        WNDCLASSEXW c = *wc;
        g_origClassProc = wc->lpfnWndProc;
        c.lpfnWndProc = NFL_WndProc;
        Log("Hooked window class UnityWndClass (RegisterClassExW).");
        return real_RegisterClassExW(&c);
    }
    return real_RegisterClassExW(wc);
}

static ATOM WINAPI H_RegisterClassW(const WNDCLASSW *wc)
{
    if (wc && IsUnityClassName(wc->lpszClassName) && !g_origClassProc) {
        WNDCLASSW c = *wc;
        g_origClassProc = wc->lpfnWndProc;
        c.lpfnWndProc = NFL_WndProc;
        Log("Hooked window class UnityWndClass (RegisterClassW).");
        return real_RegisterClassW(&c);
    }
    return real_RegisterClassW(wc);
}

static HWND WINAPI H_GetForegroundWindow(void)
{
    HWND r = real_GetForegroundWindow();
    HWND g = g_hwnd;
    if (g_cfg.enabled && g && !IsOwnWindow(r))
        return g;
    return r;
}

static HWND WINAPI H_GetActiveWindow(void)
{
    HWND r = real_GetActiveWindow();
    HWND g = g_hwnd;
    if (!r && g_cfg.enabled && g && GetWindowThreadProcessId(g, NULL) == GetCurrentThreadId())
        return g;
    return r;
}

static HWND WINAPI H_GetFocus(void)
{
    HWND r = real_GetFocus();
    HWND g = g_hwnd;
    if (!r && g_cfg.enabled && g && GetWindowThreadProcessId(g, NULL) == GetCurrentThreadId())
        return g;
    return r;
}

static BOOL WINAPI H_ClipCursor(const RECT *rc)
{
    if (rc && g_cfg.enabled && g_cfg.releaseCursor && !IsRealForeground())
        return real_ClipCursor(NULL);
    return real_ClipCursor(rc);
}

static BOOL WINAPI H_SetCursorPos(int x, int y)
{
    if (g_cfg.enabled && g_cfg.releaseCursor && !IsRealForeground())
        return TRUE; /* don't yank the mouse away from the other app */
    return real_SetCursorPos(x, y);
}

static SHORT WINAPI H_GetAsyncKeyState(int vk)
{
    if (g_cfg.enabled && g_cfg.blockBackgroundInput && !IsRealForeground())
        return 0;
    return real_GetAsyncKeyState(vk);
}

static BOOL WINAPI H_RegisterRawInputDevices(PCRAWINPUTDEVICE devs, UINT count, UINT size)
{
    RAWINPUTDEVICE local[16];
    UINT i;
    BOOL changed = FALSE;

    if (!g_cfg.enabled || !g_cfg.blockBackgroundInput || !devs || count == 0
        || count > 16 || size != sizeof(RAWINPUTDEVICE))
        return real_RegisterRawInputDevices(devs, count, size);

    memcpy(local, devs, count * sizeof(RAWINPUTDEVICE));
    for (i = 0; i < count; i++) {
        /* Generic desktop: mouse (2) and keyboard (6). Stop them from receiving
         * input that's meant for other applications. */
        if (local[i].usUsagePage == 0x01 && (local[i].usUsage == 0x02 || local[i].usUsage == 0x06)
            && (local[i].dwFlags & (RIDEV_INPUTSINK | RIDEV_EXINPUTSINK))
            && !(local[i].dwFlags & RIDEV_REMOVE)) {
            local[i].dwFlags &= ~(DWORD)(RIDEV_INPUTSINK | RIDEV_EXINPUTSINK);
            changed = TRUE;
        }
    }
    if (changed)
        Log("Removed background-input flag from raw keyboard/mouse registration.");
    return real_RegisterRawInputDevices(local, count, size);
}

static LONG_PTR WINAPI H_SetWindowLongPtrW(HWND h, int idx, LONG_PTR v)
{
    /* If Unity ever replaces its own window proc, keep ours in front of it. */
    if (idx == GWLP_WNDPROC && h && h == g_hwnd && (WNDPROC)v != NFL_WndProc) {
        LONG_PTR prev = (LONG_PTR)CurrentOrigProc();
        g_origSubclassProc = (WNDPROC)v;
        if ((WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) != NFL_WndProc)
            real_SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)NFL_WndProc);
        Log("Unity replaced its window proc - re-wrapped.");
        return prev;
    }
    return real_SetWindowLongPtrW(h, idx, v);
}

/* Unity thinks it still has focus, so it may try to grab the foreground back
 * (e.g. re-activating its window). Block that while another app is in front. */
static BOOL InBackground(void)
{
    /* Only after we actually hid a focus loss - at startup Unity must be able
     * to show and activate its window normally. */
    return g_cfg.enabled && g_hwnd && g_fakeFocus && !IsRealForeground();
}

static void LogBlocked(const char *what)
{
    static volatile LONG count;
    LONG n = InterlockedIncrement(&count);
    if (n <= 20 || (n % 500) == 0)
        Log("Blocked %s from game while in background (#%ld).", what, n);
}

static BOOL WINAPI H_SetForegroundWindow(HWND h)
{
    if (InBackground() && IsOwnWindow(h)) {
        LogBlocked("SetForegroundWindow");
        return TRUE;
    }
    return real_SetForegroundWindow(h);
}

static HWND WINAPI H_SetFocus(HWND h)
{
    if (h && InBackground() && IsOwnWindow(h)) {
        LogBlocked("SetFocus");
        return h; /* pretend focus was already there */
    }
    return real_SetFocus(h);
}

static BOOL WINAPI H_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (h && h == g_hwnd && InBackground()
        && (!(flags & SWP_NOACTIVATE) || !(flags & SWP_NOZORDER))) {
        LogBlocked("SetWindowPos activation/z-order");
        flags |= SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;
    }
    return real_SetWindowPos(h, after, x, y, cx, cy, flags);
}

static BOOL WINAPI H_ShowWindow(HWND h, int cmd)
{
    if (h && h == g_hwnd && InBackground()) {
        switch (cmd) {
        case SW_SHOW: case SW_SHOWNORMAL: case SW_RESTORE: case SW_SHOWDEFAULT:
            LogBlocked("ShowWindow activation");
            cmd = SW_SHOWNA;
            break;
        case SW_SHOWMAXIMIZED: /* can't maximize without activating; leave as is */
            break;
        case SW_MINIMIZE: case SW_SHOWMINIMIZED: case SW_FORCEMINIMIZE:
            /* Unity minimizes fullscreen games on focus loss - don't. */
            LogBlocked("ShowWindow minimize");
            return TRUE;
        }
    }
    return real_ShowWindow(h, cmd);
}

/* ------------------------------------------------------------------------ */
/* IAT patching                                                             */
/* ------------------------------------------------------------------------ */

typedef struct { const char *name; void *hook; void **real; } HookDef;

static HookDef g_hooks[] = {
    { "RegisterClassExW",        (void *)H_RegisterClassExW,        (void **)&real_RegisterClassExW },
    { "RegisterClassW",          (void *)H_RegisterClassW,          (void **)&real_RegisterClassW },
    { "GetForegroundWindow",     (void *)H_GetForegroundWindow,     (void **)&real_GetForegroundWindow },
    { "GetActiveWindow",         (void *)H_GetActiveWindow,         (void **)&real_GetActiveWindow },
    { "GetFocus",                (void *)H_GetFocus,                (void **)&real_GetFocus },
    { "ClipCursor",              (void *)H_ClipCursor,              (void **)&real_ClipCursor },
    { "SetCursorPos",            (void *)H_SetCursorPos,            (void **)&real_SetCursorPos },
    { "GetAsyncKeyState",        (void *)H_GetAsyncKeyState,        (void **)&real_GetAsyncKeyState },
    { "RegisterRawInputDevices", (void *)H_RegisterRawInputDevices, (void **)&real_RegisterRawInputDevices },
    { "SetWindowLongPtrW",       (void *)H_SetWindowLongPtrW,       (void **)&real_SetWindowLongPtrW },
    { "SetForegroundWindow",     (void *)H_SetForegroundWindow,     (void **)&real_SetForegroundWindow },
    { "SetFocus",                (void *)H_SetFocus,                (void **)&real_SetFocus },
    { "SetWindowPos",            (void *)H_SetWindowPos,            (void **)&real_SetWindowPos },
    { "ShowWindow",              (void *)H_ShowWindow,              (void **)&real_ShowWindow },
};
#define HOOK_COUNT (sizeof(g_hooks) / sizeof(g_hooks[0]))

static BOOL ResolveReals(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    size_t i;
    if (!u32)
        return FALSE;
    for (i = 0; i < HOOK_COUNT; i++) {
        *g_hooks[i].real = (void *)GetProcAddress(u32, g_hooks[i].name);
        if (!*g_hooks[i].real)
            return FALSE;
    }
    return TRUE;
}

/* Patch every USER32 import of `mod` that we have a hook for.
 * Returns number of entries pointing at our hooks, or -1 if the import table
 * isn't bound yet (loader still working). */
static int PatchImports(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0;

    if (!dd.VirtualAddress)
        return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *oft, *ft;
        if (_stricmp((const char *)(base + imp->Name), "user32.dll") != 0 || !imp->OriginalFirstThunk)
            continue;
        oft = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        ft  = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            size_t i;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal))
                continue;
            if (ft->u1.Function == oft->u1.AddressOfData)
                return -1; /* not bound yet */
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
            for (i = 0; i < HOOK_COUNT; i++) {
                DWORD old;
                if (strcmp((const char *)ibn->Name, g_hooks[i].name) != 0)
                    continue;
                if ((void *)ft->u1.Function == g_hooks[i].hook) {
                    patched++;
                } else if (VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                    InterlockedExchangePointer((PVOID volatile *)&ft->u1.Function, g_hooks[i].hook);
                    VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
                    patched++;
                }
                break;
            }
        }
    }
    return patched;
}

/* ------------------------------------------------------------------------ */
/* Fallback: subclass the window if the class was registered before we hooked */
/* ------------------------------------------------------------------------ */

static BOOL CALLBACK FindUnityWindow(HWND h, LPARAM out)
{
    wchar_t cls[64];
    if (!IsOwnWindow(h) || GetParent(h))
        return TRUE;
    if (GetClassNameW(h, cls, 64) && IsUnityClassName(cls)) {
        *(HWND *)out = h;
        return FALSE;
    }
    return TRUE;
}

static void TrySubclass(void)
{
    HWND h = NULL;
    WNDPROC cur;
    EnumWindows(FindUnityWindow, (LPARAM)&h);
    if (!h)
        return;
    cur = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (cur == NFL_WndProc) {
        g_hwnd = h; /* class hook is active, window just wasn't tracked yet */
        return;
    }
    g_origSubclassProc = cur;
    g_hwnd = h;
    real_SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)NFL_WndProc);
    Log("Game window subclassed (fallback, hwnd=%p).", (void *)h);
}

static DWORD WINAPI Worker(LPVOID unused)
{
    HMODULE up = GetModuleHandleW(L"UnityPlayer.dll");
    int i, n = -1;
    (void)unused;

    for (i = 0; i < 100 && n < 0; i++) {
        n = PatchImports(up);
        if (n < 0) Sleep(10);
    }
    Log("USER32 hooks active in UnityPlayer.dll: %d", n);

    /* Wait up to 5 minutes for the game window. */
    for (i = 0; i < 3000 && !g_hwnd; i++) {
        Sleep(100);
        if (!g_hwnd && i >= 20)
            TrySubclass();
    }
    if (!g_hwnd)
        Log("WARNING: game window not found - mod inactive.");
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(self);
    InitializeCriticalSection(&g_logCs);
    g_pid = GetCurrentProcessId();

    {
        wchar_t *slash;
        GetModuleFileNameW(self, g_dir, MAX_PATH);
        slash = wcsrchr(g_dir, L'\\');
        if (slash) slash[1] = 0;
    }
    LoadConfig();

    /* Only do anything inside the actual game (not the crash handler etc.). */
    if (!GetModuleHandleW(L"UnityPlayer.dll"))
        return TRUE;

    if (g_cfg.log) {
        HANDLE f;
        swprintf_s(g_logPath, MAX_PATH, L"%sNoFocusLoss.log", g_dir);
        f = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    Log("NoFocusLoss " NFL_VERSION " loaded. Enabled=%d KeepHeldKeys=%d BlockBackgroundInput=%d ReleaseCursor=%d",
        g_cfg.enabled, g_cfg.keepHeldKeys, g_cfg.blockBackgroundInput, g_cfg.releaseCursor);

    if (!g_cfg.enabled)
        return TRUE;

    if (!ResolveReals()) {
        Log("ERROR: could not resolve USER32 functions - mod inactive.");
        return TRUE;
    }

    /* Imports are normally bound before our DllMain runs, so hook right away
     * (before Unity registers its window class). Worker thread retries if not. */
    if (PatchImports(GetModuleHandleW(L"UnityPlayer.dll")) < 0)
        Log("Imports not bound yet, deferring hooks to worker thread.");

    {
        HANDLE t = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
