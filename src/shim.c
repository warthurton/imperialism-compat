/*
 * Imperialism display shim: a 32-bit proxy msvfw32.dll.
 * A DWM mirror supplies window scaling/fullscreen and maps input back to the
 * game's windows. Selected GDI/USER32 imports are intercepted; the system
 * MCIWndCreateA export is forwarded. Diagnostic text logging is opt-in.
 * See README.md and ORIGIN.md for installation, scope and source provenance.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>


#define LOG_LIMIT 40  /* detailed lines per API */

typedef struct {
    const char *dll;
    const char *name;
    void *hook;
    void *orig;
    volatile LONG count;
} Hook;

static FILE *g_log;
static CRITICAL_SECTION g_cs;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "%8lu ", GetTickCount());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

#define H(n) h_##n
#define DECL(dll, n) static Hook H(n) = { dll, #n, NULL, NULL, 0 }
#define LOGCALL(n, ...) do { \
        LONG c_ = InterlockedIncrement(&H(n).count); \
        if (c_ <= LOG_LIMIT) logf_(#n " " __VA_ARGS__); \
    } while (0)
#define ORIG(n, type) ((type)H(n).orig)

/* ------------------------------------------------------------------------
 * Mirror window (milestone 2)
 *
 * The game's top-level frame stays a normal 640x480-client window, but it is
 * parked directly under a larger "mirror" window that the game thread owns
 * (the frame is the mirror's owner, so the mirror always sits on top and has
 * no taskbar button). The mirror shows a DWM thumbnail of the frame's client
 * area scaled to fit, and translates mouse input back to frame coordinates.
 * Everything runs on the game's UI thread, so no cross-thread input issues.
 * ---------------------------------------------------------------------- */
static HWND g_frame, g_mirror, g_capTarget, g_closeDown;
static HTHUMBNAIL g_thumb;
static RECT g_dest;                 /* thumbnail rectangle in mirror client coords */
static POINT g_virt;                /* cursor position in frame "screen" coords */
static BOOL g_virtActive, g_inMenu, g_mirrorCap, g_fullscreen, g_shown, g_tracking, g_laidOut, g_inSetCursor;
static BOOL g_intScale = TRUE;      /* [display] fullscreen_integer_scale in the ini */
static BOOL g_brownTitle = TRUE;   /* [display] brown_titlebar in the ini: tint the title bar to match the backdrop (Windows 11) */
static BOOL g_blockModeChange = TRUE;  /* [display] block_mode_change in the ini: never change the desktop resolution */
#define BACKDROP_BROWN RGB(84, 46, 8)  /* the game's own frame backdrop colour (sampled from the stock game) */
static HMENU g_menu;
static LONG g_savedStyle;
static RECT g_savedRect;
static HHOOK g_msgHook;
static WNDPROC g_frameProc;
static int g_frameW, g_frameH;      /* the frame's normal outer size, recorded at layout */
static struct { HWND w; POINT startSrc, startPos; BOOL active; } g_drag;

static RECT g_src;  /* game view (AmbitGameWindow) in frame client coords: what the mirror shows */

/* Recompute g_src; returns TRUE if it changed. */
static BOOL refresh_src(void)
{
    RECT r, fc, old = g_src;
    HWND v = FindWindowExA(g_frame, NULL, "AmbitGameWindow", NULL);
    GetClientRect(g_frame, &fc);
    if (v && GetWindowRect(v, &r)) {
        MapWindowPoints(NULL, g_frame, (POINT *)&r, 2);
        IntersectRect(&r, &r, &fc);
    } else {
        r = fc;
    }
    g_src = r;
    return memcmp(&old, &g_src, sizeof old) != 0;
}

static void mirror_update(void)
{
    RECT cr, fc;
    double s, s2;
    int w, h;
    if (!g_mirror || !g_frame) return;
    refresh_src();
    GetClientRect(g_mirror, &cr);
    fc.right = g_src.right - g_src.left;
    fc.bottom = g_src.bottom - g_src.top;
    if (cr.right <= 0 || cr.bottom <= 0 || fc.right <= 0 || fc.bottom <= 0) return;
    s = (double)cr.right / fc.right;
    s2 = (double)cr.bottom / fc.bottom;
    if (s2 < s) s = s2;
    if (g_fullscreen && g_intScale && s >= 1.0) s = (int)s;  /* whole-number scale: sharp, brown border like the stock look */
    w = (int)(fc.right * s);
    h = (int)(fc.bottom * s);
    g_dest.left = (cr.right - w) / 2;
    g_dest.top = (cr.bottom - h) / 2;
    g_dest.right = g_dest.left + w;
    g_dest.bottom = g_dest.top + h;
    if (g_thumb) {
        DWM_THUMBNAIL_PROPERTIES p;
        memset(&p, 0, sizeof p);
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = g_dest;
        p.rcSource = g_src;
        p.fVisible = TRUE;
        p.opacity = 255;
        p.fSourceClientAreaOnly = TRUE;
        DwmUpdateThumbnailProperties(g_thumb, &p);
    }
}

/* Keep the real frame directly under the mirror's top-left corner. */
static void mirror_place_frame(void)
{
    POINT o = { 0, 0 };
    if (!g_mirror || !g_frame) return;
    ClientToScreen(g_mirror, &o);  /* client origin: the window rect includes invisible resize borders */
    SetWindowPos(g_frame, NULL, o.x, o.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void mirror_toggle_fullscreen(void)
{
    if (!g_mirror) return;
    if (!g_fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        g_savedStyle = GetWindowLongA(g_mirror, GWL_STYLE);
        GetWindowRect(g_mirror, &g_savedRect);
        GetMonitorInfoA(MonitorFromWindow(g_mirror, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(g_mirror, NULL);
        SetWindowLongA(g_mirror, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_mirror, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        SetWindowLongA(g_mirror, GWL_STYLE, g_savedStyle);
        SetMenu(g_mirror, g_menu);
        SetWindowPos(g_mirror, NULL, g_savedRect.left, g_savedRect.top,
                     g_savedRect.right - g_savedRect.left, g_savedRect.bottom - g_savedRect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_fullscreen = !g_fullscreen;
    mirror_update();  /* the size message above ran before the flag flipped */
    InvalidateRect(g_mirror, NULL, TRUE);
    logf_("fullscreen=%d", g_fullscreen);
}

static LRESULT CALLBACK msg_hook(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && w == PM_REMOVE) {
        MSG *m = (MSG *)l;
        BOOL f11 = m->message == WM_KEYDOWN && m->wParam == VK_F11;
        BOOL altEnter = m->message == WM_SYSKEYDOWN && m->wParam == VK_RETURN;
        if (f11 || altEnter) {
            if (!(m->lParam & 0x40000000)) mirror_toggle_fullscreen();  /* ignore auto-repeat */
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}

/* mirror client point -> frame client point; TRUE if inside the image */
static BOOL map_point(LPARAM l, POINT *src)
{
    int dw = g_dest.right - g_dest.left, dh = g_dest.bottom - g_dest.top;
    int mx = GET_X_LPARAM(l), my = GET_Y_LPARAM(l);
    if (refresh_src()) mirror_update();  /* the game moved/resized its view */
    if (dw <= 0 || dh <= 0) { src->x = src->y = 0; return FALSE; }
    dw = g_dest.right - g_dest.left; dh = g_dest.bottom - g_dest.top;
    src->x = g_src.left + MulDiv(mx - g_dest.left, g_src.right - g_src.left, dw);
    src->y = g_src.top + MulDiv(my - g_dest.top, g_src.bottom - g_src.top, dh);
    return mx >= g_dest.left && mx < g_dest.right && my >= g_dest.top && my < g_dest.bottom;
}

/* Deepest child of the frame under p (frame client coords); p becomes target-client coords. */
static HWND resolve_target(POINT *p)
{
    HWND t = g_frame;
    if (g_capTarget && IsWindow(g_capTarget)) {
        MapWindowPoints(g_frame, g_capTarget, p, 1);
        return g_capTarget;
    }
    for (;;) {
        HWND c = ChildWindowFromPointEx(t, *p, CWP_SKIPINVISIBLE);
        if (!c || c == t) break;
        MapWindowPoints(t, c, p, 1);
        t = c;
    }
    return t;
}

static LRESULT mirror_mouse(HWND h, UINT m, WPARAM w, LPARAM l)
{
    POINT p, o = { 0, 0 }, cp;
    HWND t;
    BOOL inside = map_point(l, &p);
    BOOL down = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN ||
                m == WM_LBUTTONDBLCLK || m == WM_RBUTTONDBLCLK || m == WM_MBUTTONDBLCLK;
    BOOL up = m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP;

    if (!inside && !g_capTarget && !g_mirrorCap && !g_drag.active) { g_virtActive = FALSE; return 0; }
    if (!g_tracking) {
        TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 };
        TrackMouseEvent(&te);
        g_tracking = TRUE;
    }
    ClientToScreen(g_frame, &o);
    g_virt.x = o.x + p.x;
    g_virt.y = o.y + p.y;
    g_virtActive = TRUE;

    if (g_drag.active) {  /* manual caption drag of a game child window */
        if (m == WM_MOUSEMOVE)
            SetWindowPos(g_drag.w, NULL, g_drag.startPos.x + p.x - g_drag.startSrc.x,
                         g_drag.startPos.y + p.y - g_drag.startSrc.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        else if (m == WM_LBUTTONUP) { g_drag.active = FALSE; ReleaseCapture(); g_mirrorCap = FALSE; }
        return 0;
    }

    cp = p;
    t = resolve_target(&cp);
    if (!g_capTarget && t != g_frame) {  /* non-client hits on game child windows */
        LRESULT hit = SendMessageA(t, WM_NCHITTEST, 0, MAKELPARAM(g_virt.x, g_virt.y));
        if (hit != HTCLIENT && hit != HTNOWHERE && hit != HTTRANSPARENT) {
            if (m == WM_LBUTTONDOWN) {
                BringWindowToTop(t);
                if (hit == HTCAPTION) {
                    RECT wr;
                    GetWindowRect(t, &wr);
                    MapWindowPoints(NULL, g_frame, (POINT *)&wr, 1);
                    g_drag.w = t; g_drag.active = TRUE;
                    g_drag.startSrc = p; g_drag.startPos.x = wr.left; g_drag.startPos.y = wr.top;
                    SetCapture(h); g_mirrorCap = TRUE;
                } else if (hit == HTCLOSE) {
                    g_closeDown = t; SetCapture(h); g_mirrorCap = TRUE;
                }
            } else if (m == WM_LBUTTONUP && g_closeDown) {
                if (t == g_closeDown && hit == HTCLOSE) PostMessageA(t, WM_SYSCOMMAND, SC_CLOSE, 0);
                g_closeDown = NULL; ReleaseCapture(); g_mirrorCap = FALSE;
            }
            return 0;
        }
    }
    if (g_closeDown && up) { g_closeDown = NULL; }
    if (down && !GetCapture()) { SetCapture(h); g_mirrorCap = TRUE; }
    SendMessageA(t, m, w, MAKELPARAM(cp.x, cp.y));
    if (up && g_mirrorCap && !g_capTarget && !(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) {
        ReleaseCapture();
        g_mirrorCap = FALSE;
    }
    return 0;
}

static LRESULT CALLBACK MirrorProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED: {
        static LONG n;
        WINDOWPOS *wp = (WINDOWPOS *)l;
        if (InterlockedIncrement(&n) <= 60)
            logf_("mirror %s pos=(%d,%d) size=%dx%d flags=%x", m == WM_WINDOWPOSCHANGING ? "poschanging" : "poschanged",
                  wp->x, wp->y, wp->cx, wp->cy, wp->flags);
        break;
    }
    case WM_SHOWWINDOW:
        logf_("mirror WM_SHOWWINDOW show=%d reason=%d", (int)w, (int)l);
        if (w && g_laidOut) { mirror_place_frame(); mirror_update(); }  /* re-park after a restore */
        break;
    case WM_SYSCOMMAND:
        /* The mirror is owned by the frame and has no taskbar button, so minimize the
         * frame (which hides its owned mirror and leaves one taskbar button to restore). */
        if ((w & 0xFFF0) == SC_MINIMIZE && g_frame) {
            ShowWindow(g_frame, SW_MINIMIZE);
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        RECT r;
        static HBRUSH brown;
        if (!brown) brown = CreateSolidBrush(BACKDROP_BROWN);
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, brown);
        return 1;
    }
    case WM_SIZE:
        mirror_update();
        InvalidateRect(h, NULL, TRUE);
        break;
    case WM_MOVE:
        mirror_place_frame();
        break;
    case WM_GETMINMAXINFO: {
        RECT r;
        MINMAXINFO *mm = (MINMAXINFO *)l;
        DefWindowProcA(h, m, w, l);  /* defaults first, then override */
        /* Client area must never be smaller than the frame it hides (else the frame pokes out
         * from under the mirror). Uses the frame's recorded normal size, not its current one,
         * because the game minimizes/maximizes it around shutdown. */
        if (g_laidOut && g_frameW > 0) {
            r.left = r.top = 0; r.right = g_frameW; r.bottom = g_frameH;
            AdjustWindowRect(&r, GetWindowLongA(h, GWL_STYLE), GetMenu(h) != NULL);
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;
    }
    case WM_SETFOCUS:
        if (!g_inMenu) PostMessageA(h, WM_APP + 1, 0, 0);
        break;
    case WM_APP + 1:
        if (!g_inMenu && g_frame && !g_mirrorCap && GetFocus() == h) SetFocus(g_frame);
        return 0;
    case WM_ENTERMENULOOP: g_inMenu = TRUE; break;
    case WM_EXITMENULOOP:  g_inMenu = FALSE; PostMessageA(h, WM_APP + 1, 0, 0); break;
    case WM_INITMENU:
    case WM_INITMENUPOPUP:
    case WM_MENUSELECT:
        if (g_frame) SendMessageA(g_frame, m, w, l);
        break;
    case WM_COMMAND:
        if (g_frame) SendMessageA(g_frame, m, w, l);
        return 0;
    case WM_CLOSE:
        if (g_frame) SendMessageA(g_frame, WM_CLOSE, 0, 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT pt, p;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (map_point(MAKELPARAM(pt.x, pt.y), &p) || g_capTarget) {
                HWND t = resolve_target(&p);
                LRESULT handled;
                g_inSetCursor = TRUE;
                handled = SendMessageA(t, WM_SETCURSOR, (WPARAM)t, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                g_inSetCursor = FALSE;
                {
                    static LONG nsc;
                    if (InterlockedIncrement(&nsc) <= 40)
                        logf_("mirror WM_SETCURSOR -> target %p class=%s handled=%ld", t, "", (long)handled);
                }
                (void)handled;  /* like Windows: after WM_SETCURSOR the cursor is whatever the game set,
                                 * whether or not its handler returned TRUE */
                return TRUE;
            }
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        return mirror_mouse(h, m, w, l);
    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        if (!g_mirrorCap && !g_capTarget) g_virtActive = FALSE;
        return 0;
    case WM_DESTROY:
        if (g_thumb) { DwmUnregisterThumbnail(g_thumb); g_thumb = NULL; }
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Subclass of the game's frame: however it gets maximized (the game does it itself, and
 * Windows restores it that way after minimize), "maximized" must not be bigger than normal. */
static LRESULT CALLBACK FrameProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static LONG nlog;
    LRESULT r;
    if ((m == WM_ACTIVATEAPP || m == WM_ACTIVATE || m == WM_SYSCOMMAND || m == WM_SIZE || m == WM_SHOWWINDOW ||
         m == WM_CLOSE || m == WM_KILLFOCUS || m == WM_SETFOCUS || m == WM_NCACTIVATE) && InterlockedIncrement(&nlog) <= 200)
        logf_("frame msg 0x%04x w=%lx l=%lx fg=%p active=%p", m, (unsigned long)w, (unsigned long)l,
              GetForegroundWindow(), GetActiveWindow());
    r = CallWindowProcA(g_frameProc, h, m, w, l);
    if (m == WM_GETMINMAXINFO && g_frameW > 0) {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMaxSize.x = mm->ptMaxTrackSize.x = g_frameW;
        mm->ptMaxSize.y = mm->ptMaxTrackSize.y = g_frameH;
    }
    return r;
}

static void mirror_init(HWND frame)
{
    WNDCLASSEXA wc;
    g_frame = frame;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MirrorProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = (HICON)GetClassLongA(frame, GCL_HICON);
    wc.hbrBackground = CreateSolidBrush(BACKDROP_BROWN);
    wc.lpszClassName = "ImperialismMirror";
    RegisterClassExA(&wc);
    g_mirror = CreateWindowExA(0, wc.lpszClassName, "Imperialism", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, 0, 656, 539, frame, NULL, wc.hInstance, NULL);
    if (g_brownTitle) {
        /* DWMWA_CAPTION_COLOR 35, DWMWA_TEXT_COLOR 36, DWMWA_BORDER_COLOR 34, DWMWA_USE_IMMERSIVE_DARK_MODE 20:
         * only honoured on Windows 11 build 22000+; older systems just return an error and keep the normal bar. */
        COLORREF cap = BACKDROP_BROWN, txt = RGB(236, 214, 160);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(g_mirror, 20, &dark, sizeof dark);   /* light caption buttons on the dark bar */
        DwmSetWindowAttribute(g_mirror, 35, &cap, sizeof cap);
        DwmSetWindowAttribute(g_mirror, 34, &cap, sizeof cap);
        DwmSetWindowAttribute(g_mirror, 36, &txt, sizeof txt);
    }
    g_frameProc = (WNDPROC)SetWindowLongA(frame, GWL_WNDPROC, (LONG)FrameProc);
    g_msgHook = SetWindowsHookExA(WH_GETMESSAGE, msg_hook, NULL, GetCurrentThreadId());
    logf_("mirror created %p (frame %p) hook=%p", g_mirror, frame, g_msgHook);
}

/* Initial size/position, done once the frame has its real size (it is shown
 * first at a bogus ~1940x39 and resized to 656x539 a moment later). */
static void mirror_layout(void)
{
    RECT fw, fc, wa, r;
    DWORD style;
    int k, x, y;
    if (!g_mirror || g_laidOut) return;
    refresh_src();
    fc.left = fc.top = 0;
    fc.right = g_src.right - g_src.left;   /* the game view, not the whole frame */
    fc.bottom = g_src.bottom - g_src.top;
    if (fc.right < 320 || fc.bottom < 200) return;
    g_laidOut = TRUE;
    g_menu = GetMenu(g_frame);
    if (g_menu) SetMenu(g_mirror, g_menu);
    GetWindowRect(g_frame, &fw);
    g_frameW = fw.right - fw.left;
    g_frameH = fw.bottom - fw.top;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    style = GetWindowLongA(g_mirror, GWL_STYLE);
    for (k = 6; k >= 1; k--) {  /* largest integer scale that fits the work area */
        r.left = r.top = 0; r.right = fc.right * k; r.bottom = fc.bottom * k;
        AdjustWindowRect(&r, style, g_menu != NULL);
        if (r.right - r.left <= wa.right - wa.left && r.bottom - r.top <= wa.bottom - wa.top) break;
    }
    if (k < 1) k = 1;
    r.left = r.top = 0; r.right = fc.right * k; r.bottom = fc.bottom * k;
    AdjustWindowRect(&r, style, g_menu != NULL);
    x = wa.left + ((wa.right - wa.left) - (r.right - r.left)) / 2;
    y = wa.top + ((wa.bottom - wa.top) - (r.bottom - r.top)) / 2;
    SetWindowPos(g_mirror, NULL, x, y, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    mirror_place_frame();
    mirror_update();
    if (g_shown) ShowWindow(g_mirror, SW_SHOW);
    logf_("mirror laid out: frame window %ldx%ld client %ldx%ld menu=%p scale=%d",
          fw.right - fw.left, fw.bottom - fw.top, fc.right, fc.bottom, g_menu, k);
}

static void mirror_show(void)
{
    if (!g_mirror || g_shown) return;
    g_shown = TRUE;
    if (DwmRegisterThumbnail(g_mirror, g_frame, &g_thumb) != S_OK) g_thumb = NULL;
    logf_("mirror shown: thumb=%p", g_thumb);
    if (g_laidOut) ShowWindow(g_mirror, SW_SHOW);
    else mirror_layout();
}

/* ---- GDI32 ---- */
DECL("GDI32.dll", BitBlt);
DECL("GDI32.dll", PatBlt);
DECL("GDI32.dll", StretchDIBits);
DECL("GDI32.dll", CreateDIBSection);
DECL("GDI32.dll", CreateDIBitmap);
DECL("GDI32.dll", CreateCompatibleDC);
DECL("GDI32.dll", CreateCompatibleBitmap);
DECL("GDI32.dll", CreatePalette);
DECL("GDI32.dll", SelectPalette);
DECL("GDI32.dll", RealizePalette);
DECL("GDI32.dll", SetStretchBltMode);
DECL("GDI32.dll", GetDeviceCaps);
DECL("GDI32.dll", LineTo);
/* ---- USER32 ---- */
DECL("USER32.dll", ChangeDisplaySettingsA);
DECL("USER32.dll", CreateWindowExA);
DECL("USER32.dll", GetDC);
DECL("USER32.dll", ReleaseDC);
DECL("USER32.dll", BeginPaint);
DECL("USER32.dll", EndPaint);
DECL("USER32.dll", SetWindowPos);
DECL("USER32.dll", ShowWindow);
DECL("USER32.dll", SetCapture);
DECL("USER32.dll", ReleaseCapture);
DECL("USER32.dll", GetCapture);
DECL("USER32.dll", GetCursorPos);
DECL("USER32.dll", GetMessagePos);
DECL("USER32.dll", SetWindowPlacement);
DECL("USER32.dll", MoveWindow);
DECL("USER32.dll", SetCursor);

static BOOL WINAPI w_BitBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop)
{
    LOGCALL(BitBlt, "dst=%p (%d,%d %dx%d) src=%p (%d,%d) rop=%08lx", d, x, y, w, h, s, sx, sy, rop);
    return ORIG(BitBlt, BOOL (WINAPI *)(HDC,int,int,int,int,HDC,int,int,DWORD))(d, x, y, w, h, s, sx, sy, rop);
}
static BOOL WINAPI w_PatBlt(HDC d, int x, int y, int w, int h, DWORD rop)
{
    LOGCALL(PatBlt, "dst=%p (%d,%d %dx%d) rop=%08lx", d, x, y, w, h, rop);
    return ORIG(PatBlt, BOOL (WINAPI *)(HDC,int,int,int,int,DWORD))(d, x, y, w, h, rop);
}
static int WINAPI w_StretchDIBits(HDC d, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs,
                                  const void *bits, const BITMAPINFO *bmi, UINT use, DWORD rop)
{
    LOGCALL(StretchDIBits, "dst=%p (%d,%d %dx%d) src=(%d,%d %dx%d) bpp=%d use=%u rop=%08lx",
            d, xd, yd, wd, hd, xs, ys, ws, hs, bmi ? bmi->bmiHeader.biBitCount : -1, use, rop);
    return ORIG(StretchDIBits, int (WINAPI *)(HDC,int,int,int,int,int,int,int,int,const void*,const BITMAPINFO*,UINT,DWORD))
        (d, xd, yd, wd, hd, xs, ys, ws, hs, bits, bmi, use, rop);
}
static HBITMAP WINAPI w_CreateDIBSection(HDC dc, const BITMAPINFO *bmi, UINT use, void **bits, HANDLE sec, DWORD off)
{
    LOGCALL(CreateDIBSection, "%ldx%ld bpp=%d clrUsed=%lu use=%u",
            bmi ? bmi->bmiHeader.biWidth : 0, bmi ? bmi->bmiHeader.biHeight : 0,
            bmi ? bmi->bmiHeader.biBitCount : 0, bmi ? bmi->bmiHeader.biClrUsed : 0, use);
    return ORIG(CreateDIBSection, HBITMAP (WINAPI *)(HDC,const BITMAPINFO*,UINT,void**,HANDLE,DWORD))(dc, bmi, use, bits, sec, off);
}
static HBITMAP WINAPI w_CreateDIBitmap(HDC dc, const BITMAPINFOHEADER *h, DWORD init, const void *b, const BITMAPINFO *bmi, UINT use)
{
    LOGCALL(CreateDIBitmap, "%ldx%ld bpp=%d init=%lu", h ? h->biWidth : 0, h ? h->biHeight : 0, h ? h->biBitCount : 0, init);
    return ORIG(CreateDIBitmap, HBITMAP (WINAPI *)(HDC,const BITMAPINFOHEADER*,DWORD,const void*,const BITMAPINFO*,UINT))(dc, h, init, b, bmi, use);
}
static HDC WINAPI w_CreateCompatibleDC(HDC dc)
{
    HDC r = ORIG(CreateCompatibleDC, HDC (WINAPI *)(HDC))(dc);
    LOGCALL(CreateCompatibleDC, "from=%p -> %p", dc, r);
    return r;
}
static HBITMAP WINAPI w_CreateCompatibleBitmap(HDC dc, int w, int h)
{
    LOGCALL(CreateCompatibleBitmap, "%dx%d", w, h);
    return ORIG(CreateCompatibleBitmap, HBITMAP (WINAPI *)(HDC,int,int))(dc, w, h);
}
static HPALETTE WINAPI w_CreatePalette(const LOGPALETTE *p)
{
    LOGCALL(CreatePalette, "entries=%u", p ? p->palNumEntries : 0);
    return ORIG(CreatePalette, HPALETTE (WINAPI *)(const LOGPALETTE*))(p);
}
static HPALETTE WINAPI w_SelectPalette(HDC dc, HPALETTE pal, BOOL bg)
{
    LOGCALL(SelectPalette, "dc=%p pal=%p bg=%d", dc, pal, bg);
    return ORIG(SelectPalette, HPALETTE (WINAPI *)(HDC,HPALETTE,BOOL))(dc, pal, bg);
}
static UINT WINAPI w_RealizePalette(HDC dc)
{
    LOGCALL(RealizePalette, "dc=%p", dc);
    return ORIG(RealizePalette, UINT (WINAPI *)(HDC))(dc);
}
static int WINAPI w_SetStretchBltMode(HDC dc, int mode)
{
    LOGCALL(SetStretchBltMode, "mode=%d", mode);
    return ORIG(SetStretchBltMode, int (WINAPI *)(HDC,int))(dc, mode);
}
static int WINAPI w_GetDeviceCaps(HDC dc, int idx)
{
    int r = ORIG(GetDeviceCaps, int (WINAPI *)(HDC,int))(dc, idx);
    LOGCALL(GetDeviceCaps, "index=%d -> %d", idx, r);
    return r;
}
static BOOL WINAPI w_LineTo(HDC dc, int x, int y)
{
    LOGCALL(LineTo, "(%d,%d)", x, y);
    return ORIG(LineTo, BOOL (WINAPI *)(HDC,int,int))(dc, x, y);
}
static LONG WINAPI w_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags)
{
    LONG r;
    if (g_blockModeChange) {
        /* The game asks for 640x480 (or restores the old mode on exit). We scale in a window instead, so pretend
         * it worked and leave the desktop alone. */
        LOGCALL(ChangeDisplaySettingsA, "%lux%lu flags=%lx -> blocked", dm ? dm->dmPelsWidth : 0,
                dm ? dm->dmPelsHeight : 0, flags);
        return DISP_CHANGE_SUCCESSFUL;
    }
    r = ORIG(ChangeDisplaySettingsA, LONG (WINAPI *)(DEVMODEA*,DWORD))(dm, flags);
    LOGCALL(ChangeDisplaySettingsA, "%lux%lu bpp=%lu flags=%lx -> %ld",
            dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, dm ? dm->dmBitsPerPel : 0, flags, r);
    return r;
}
static HWND WINAPI w_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND r = ORIG(CreateWindowExA, HWND (WINAPI *)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID))
        (ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (r && !parent && HIWORD(cls) != 0 && title && !strcmp(title, "Imperialism") && !g_frame) mirror_init(r);
    if (HIWORD(cls) == 0) LOGCALL(CreateWindowExA, "atom=%u style=%08lx ex=%08lx (%d,%d %dx%d) parent=%p -> %p", (unsigned)LOWORD(cls), style, ex, x, y, w, h, parent, r);
    else LOGCALL(CreateWindowExA, "class=\"%s\" title=\"%s\" style=%08lx ex=%08lx (%d,%d %dx%d) parent=%p -> %p",
                 cls, title ? title : "", style, ex, x, y, w, h, parent, r);
    return r;
}
static HDC WINAPI w_GetDC(HWND wnd)
{
    HDC r = ORIG(GetDC, HDC (WINAPI *)(HWND))(wnd);
    LOGCALL(GetDC, "wnd=%p -> %p", wnd, r);
    return r;
}
static int WINAPI w_ReleaseDC(HWND wnd, HDC dc)
{
    LOGCALL(ReleaseDC, "wnd=%p dc=%p", wnd, dc);
    return ORIG(ReleaseDC, int (WINAPI *)(HWND,HDC))(wnd, dc);
}
static HDC WINAPI w_BeginPaint(HWND wnd, PAINTSTRUCT *ps)
{
    HDC r = ORIG(BeginPaint, HDC (WINAPI *)(HWND,PAINTSTRUCT*))(wnd, ps);
    LOGCALL(BeginPaint, "wnd=%p dc=%p rc=(%ld,%ld,%ld,%ld)", wnd, r, ps->rcPaint.left, ps->rcPaint.top, ps->rcPaint.right, ps->rcPaint.bottom);
    return r;
}
static BOOL WINAPI w_EndPaint(HWND wnd, const PAINTSTRUCT *ps)
{
    LOGCALL(EndPaint, "wnd=%p", wnd);
    return ORIG(EndPaint, BOOL (WINAPI *)(HWND,const PAINTSTRUCT*))(wnd, ps);
}
static BOOL WINAPI w_SetWindowPos(HWND wnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    BOOL r;
    LOGCALL(SetWindowPos, "wnd=%p (%d,%d %dx%d) flags=%x", wnd, x, y, w, h, flags);
    if (wnd == g_frame && g_mirror && !(flags & SWP_NOMOVE)) {  /* keep the frame parked under the mirror */
        POINT o = { 0, 0 };
        ClientToScreen(g_mirror, &o);
        x = o.x; y = o.y;
    }
    r = ORIG(SetWindowPos, BOOL (WINAPI *)(HWND,HWND,int,int,int,int,UINT))(wnd, after, x, y, w, h, flags);
    if (wnd == g_frame && g_mirror) {  /* frame size may have changed */
        if (g_shown && !g_laidOut) mirror_layout();
        mirror_update();
    }
    return r;
}
static BOOL WINAPI w_ShowWindow(HWND wnd, int cmd)
{
    BOOL r;
    LOGCALL(ShowWindow, "wnd=%p cmd=%d", wnd, cmd);
    if (g_mirror && wnd == g_frame &&
        (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE)) {
        logf_("frame self-minimize via ShowWindow(%d) blocked", cmd);
        return TRUE;
    }
    r = ORIG(ShowWindow, BOOL (WINAPI *)(HWND,int))(wnd, cmd);
    if (wnd == g_frame && g_mirror) {
        if (cmd == SW_HIDE) ShowWindow(g_mirror, SW_HIDE);
        else mirror_show();
    }
    return r;
}
/* Something in the game resizes whatever window it thinks is its main one; make sure
 * that is never the mirror. Logged with the caller's address for tracing. */
static BOOL WINAPI w_MoveWindow(HWND wnd, int x, int y, int w, int h, BOOL repaint)
{
    LONG c = InterlockedIncrement(&H(MoveWindow).count);
    if (c <= LOG_LIMIT)
        logf_("MoveWindow wnd=%p (%s) (%d,%d %dx%d) caller=%p", wnd,
              wnd == g_mirror ? "MIRROR" : wnd == g_frame ? "frame" : "other", x, y, w, h, __builtin_return_address(0));
    if (g_mirror && wnd == g_mirror) return TRUE;  /* swallow */
    return ORIG(MoveWindow, BOOL (WINAPI *)(HWND,int,int,int,int,BOOL))(wnd, x, y, w, h, repaint);
}

/* Log what cursor the game asks for, to check that cursor forwarding really takes effect. */
static HCURSOR WINAPI w_SetCursor(HCURSOR c)
{
    LONG n = InterlockedIncrement(&H(SetCursor).count);
    HCURSOR prev = SetCursor(c);
    if (n <= 80) logf_("SetCursor %p (prev %p) fromMirrorSetCursor=%d", c, prev, g_inSetCursor);
    return prev;
}

/* The game maximizes its frame (FUN_00484d70) and centres a 640x480 view on a backdrop.
 * Keep the frame a normal small window so it can sit under the mirror. */
static BOOL WINAPI w_SetWindowPlacement(HWND wnd, const WINDOWPLACEMENT *wp)
{
    WINDOWPLACEMENT c;
    if (wnd == g_frame && wp) logf_("SetWindowPlacement frame showCmd=%u flags=%x", wp->showCmd, wp->flags);
    if (g_mirror && wnd == g_frame && wp && wp->showCmd == SW_SHOWMAXIMIZED) {
        c = *wp;
        c.showCmd = SW_SHOWNORMAL;
        wp = &c;
        logf_("frame maximize suppressed");
    } else if (g_mirror && wnd == g_frame && wp &&
               (wp->showCmd == SW_SHOWMINIMIZED || wp->showCmd == SW_MINIMIZE || wp->showCmd == SW_SHOWMINNOACTIVE)) {
        logf_("frame self-minimize via SetWindowPlacement blocked");
        return TRUE;  /* the game minimizes itself when it loses focus; a windowed game shouldn't */
    }
    return ORIG(SetWindowPlacement, BOOL (WINAPI *)(HWND,const WINDOWPLACEMENT*))(wnd, wp);
}
static HWND WINAPI w_SetCapture(HWND wnd)
{
    HWND prev;
    if (g_mirror && wnd && (wnd == g_frame || IsChild(g_frame, wnd))) {  /* route the game's capture through the mirror */
        prev = g_capTarget;
        g_capTarget = wnd;
        SetCapture(g_mirror);
        g_mirrorCap = TRUE;
        return prev;
    }
    return SetCapture(wnd);
}
static BOOL WINAPI w_ReleaseCapture(void)
{
    g_capTarget = NULL;
    g_mirrorCap = FALSE;
    return ReleaseCapture();
}
static HWND WINAPI w_GetCapture(void)
{
    return g_capTarget ? g_capTarget : GetCapture();
}
static BOOL WINAPI w_GetCursorPos(POINT *pt)
{
    if (g_mirror && g_virtActive && pt) { *pt = g_virt; return TRUE; }
    return GetCursorPos(pt);
}
static DWORD WINAPI w_GetMessagePos(void)
{
    if (g_mirror && g_virtActive) return MAKELONG(g_virt.x, g_virt.y);
    return GetMessagePos();
}

/* ---- text log: the strings the game draws, with positions (off unless [text_log] enabled=1 in imperialism_shim.ini) ----
 * Lets a test harness READ what is on screen (offer sheets, hot text, dialogs) instead of looking at pixels. One line per string:
 * "<tick> <T|X|B|D> <x>,<y> <text>" in imperialism_text.log; the same string at the same place is not repeated within 1.5 s. */
static BOOL g_textLog;
static FILE *g_text;
static struct { unsigned h; DWORD t; } g_recent[128];
static unsigned g_recentN;

static void text_out(const char *kind, int x, int y, const char *s, int n)
{
    char buf[420];
    int i, k = 0;
    unsigned h = 2166136261u;
    DWORD now;
    if (!g_textLog || !g_text || !s) return;
    if (n < 0) n = (int)strlen(s);
    if (n > 400) n = 400;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        buf[k++] = (c < 32) ? ' ' : (char)c;
        h = (h ^ c) * 16777619u;
    }
    while (k && buf[k - 1] == ' ') k--;
    buf[k] = 0;
    if (!k) return;
    h = (h ^ (unsigned)x) * 16777619u;
    h = (h ^ (unsigned)y) * 16777619u;
    now = GetTickCount();
    EnterCriticalSection(&g_cs);
    for (i = 0; i < 128; i++)
        if (g_recent[i].h == h && now - g_recent[i].t < 1500) { LeaveCriticalSection(&g_cs); return; }
    g_recent[g_recentN & 127].h = h;
    g_recent[g_recentN & 127].t = now;
    g_recentN++;
    fprintf(g_text, "%lu %s %d,%d %s\n", now, kind, x, y, buf);
    fflush(g_text);
    LeaveCriticalSection(&g_cs);
}

DECL("GDI32.dll", TextOutA);
DECL("GDI32.dll", ExtTextOutA);
DECL("USER32.dll", TabbedTextOutA);
DECL("USER32.dll", DrawTextA);

static BOOL WINAPI w_TextOutA(HDC dc, int x, int y, LPCSTR s, int n)
{
    text_out("T", x, y, s, n);
    return ORIG(TextOutA, BOOL (WINAPI *)(HDC,int,int,LPCSTR,int))(dc, x, y, s, n);
}
static BOOL WINAPI w_ExtTextOutA(HDC dc, int x, int y, UINT opts, const RECT *r, LPCSTR s, UINT n, const INT *dx)
{
    if (!(opts & ETO_GLYPH_INDEX)) text_out("X", x, y, s, (int)n);
    return ORIG(ExtTextOutA, BOOL (WINAPI *)(HDC,int,int,UINT,const RECT*,LPCSTR,UINT,const INT*))(dc, x, y, opts, r, s, n, dx);
}
static LONG WINAPI w_TabbedTextOutA(HDC dc, int x, int y, LPCSTR s, int n, int nt, const INT *tabs, int origin)
{
    text_out("B", x, y, s, n);
    return ORIG(TabbedTextOutA, LONG (WINAPI *)(HDC,int,int,LPCSTR,int,int,const INT*,int))(dc, x, y, s, n, nt, tabs, origin);
}
static int WINAPI w_DrawTextA(HDC dc, LPCSTR s, int n, LPRECT r, UINT fmt)
{
    if (!(fmt & DT_CALCRECT) && r) text_out("D", r->left, r->top, s, n);
    return ORIG(DrawTextA, int (WINAPI *)(HDC,LPCSTR,int,LPRECT,UINT))(dc, s, n, r, fmt);
}

static Hook *g_hooks[] = {
    &H(BitBlt), &H(PatBlt), &H(StretchDIBits), &H(CreateDIBSection), &H(CreateDIBitmap),
    &H(CreateCompatibleDC), &H(CreateCompatibleBitmap), &H(CreatePalette), &H(SelectPalette),
    &H(RealizePalette), &H(SetStretchBltMode), &H(GetDeviceCaps), &H(LineTo),
    &H(ChangeDisplaySettingsA), &H(CreateWindowExA), &H(GetDC), &H(ReleaseDC), &H(BeginPaint),
    &H(EndPaint), &H(SetWindowPos), &H(ShowWindow), &H(SetCapture), &H(ReleaseCapture),
    &H(GetCapture), &H(GetCursorPos), &H(GetMessagePos), &H(SetWindowPlacement), &H(MoveWindow), &H(SetCursor),
    &H(TextOutA), &H(ExtTextOutA), &H(TabbedTextOutA), &H(DrawTextA),
};
#define NHOOKS (sizeof g_hooks / sizeof g_hooks[0])

static void init_hook_table(void)
{
    H(BitBlt).hook = w_BitBlt;                         H(PatBlt).hook = w_PatBlt;
    H(StretchDIBits).hook = w_StretchDIBits;           H(CreateDIBSection).hook = w_CreateDIBSection;
    H(CreateDIBitmap).hook = w_CreateDIBitmap;         H(CreateCompatibleDC).hook = w_CreateCompatibleDC;
    H(CreateCompatibleBitmap).hook = w_CreateCompatibleBitmap;
    H(CreatePalette).hook = w_CreatePalette;           H(SelectPalette).hook = w_SelectPalette;
    H(RealizePalette).hook = w_RealizePalette;         H(SetStretchBltMode).hook = w_SetStretchBltMode;
    H(GetDeviceCaps).hook = w_GetDeviceCaps;           H(LineTo).hook = w_LineTo;
    H(ChangeDisplaySettingsA).hook = w_ChangeDisplaySettingsA;
    H(CreateWindowExA).hook = w_CreateWindowExA;       H(GetDC).hook = w_GetDC;
    H(ReleaseDC).hook = w_ReleaseDC;                   H(BeginPaint).hook = w_BeginPaint;
    H(EndPaint).hook = w_EndPaint;                     H(SetWindowPos).hook = w_SetWindowPos;
    H(ShowWindow).hook = w_ShowWindow;
    H(SetCapture).hook = w_SetCapture;                 H(ReleaseCapture).hook = w_ReleaseCapture;
    H(GetCapture).hook = w_GetCapture;                 H(GetCursorPos).hook = w_GetCursorPos;
    H(GetMessagePos).hook = w_GetMessagePos;           H(SetWindowPlacement).hook = w_SetWindowPlacement;
    H(MoveWindow).hook = w_MoveWindow;
    H(SetCursor).hook = w_SetCursor;
    H(TextOutA).hook = w_TextOutA;                     H(ExtTextOutA).hook = w_ExtTextOutA;
    H(TabbedTextOutA).hook = w_TabbedTextOutA;         H(DrawTextA).hook = w_DrawTextA;
}

/* Patch the main executable's IAT entries for every Hook in the table. */
static int patch_iat(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0;

    if (!dd.VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); imp->Name; imp++) {
        const char *dll = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            const IMAGE_IMPORT_BY_NAME *ibn;
            size_t i;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            ibn = (const IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            for (i = 0; i < NHOOKS; i++) {
                Hook *k = g_hooks[i];
                DWORD old;
                if (_stricmp(dll, k->dll) || strcmp((const char *)ibn->Name, k->name)) continue;
                if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) break;
                k->orig = (void *)iat->u1.Function;
                iat->u1.Function = (DWORD)(ULONG_PTR)k->hook;
                VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                patched++;
                break;
            }
        }
    }
    return patched;
}

static void diag(void)
{
    RECT mr, mc, fr, fc;
    SIZE sz = { 0, 0 };
    if (!g_mirror || !g_frame) return;
    GetWindowRect(g_mirror, &mr); GetClientRect(g_mirror, &mc);
    GetWindowRect(g_frame, &fr);  GetClientRect(g_frame, &fc);
    if (g_thumb) DwmQueryThumbnailSourceSize(g_thumb, &sz);
    fprintf(g_log, "diag mirror win(%ld,%ld %ldx%ld) client %ldx%ld zoomed=%d | frame win(%ld,%ld %ldx%ld) client %ldx%ld zoomed=%d | thumb src %ldx%ld dest(%ld,%ld %ldx%ld) laidOut=%d\n",
            mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top, mc.right, mc.bottom, IsZoomed(g_mirror),
            fr.left, fr.top, fr.right - fr.left, fr.bottom - fr.top, fc.right, fc.bottom, IsZoomed(g_frame),
            sz.cx, sz.cy, g_dest.left, g_dest.top, g_dest.right - g_dest.left, g_dest.bottom - g_dest.top, g_laidOut);
    fprintf(g_log, "diag view src(%ld,%ld %ldx%ld)\n", g_src.left, g_src.top, g_src.right - g_src.left, g_src.bottom - g_src.top);
}

static void dump_counts(void)
{
    size_t i;
    EnterCriticalSection(&g_cs);
    diag();
    fprintf(g_log, "---- call counts ----\n");
    for (i = 0; i < NHOOKS; i++)
        fprintf(g_log, "%-24s %s %ld\n", g_hooks[i]->name, g_hooks[i]->orig ? "hooked" : "NOT-IMPORTED", g_hooks[i]->count);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI summary_thread(LPVOID unused)
{
    (void)unused;
    for (;;) { Sleep(5000); dump_counts(); }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], *slash;
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, path, MAX_PATH);
        slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "imperialism_gdi_log.txt");
        g_log = fopen(path, "w");
        SetProcessDPIAware();  /* physical pixels: we do the scaling, not Windows */
        init_hook_table();
        logf_("shim loaded; patched %d imports", patch_iat());
        {
            char ini[MAX_PATH], *s;
            strcpy(ini, path);
            s = strrchr(ini, '\\');
            if (s) strcpy(s + 1, "imperialism_shim.ini");
            g_intScale = GetPrivateProfileIntA("display", "fullscreen_integer_scale", 1, ini) != 0;
            g_blockModeChange = GetPrivateProfileIntA("display", "block_mode_change", 1, ini) != 0;
            g_brownTitle = GetPrivateProfileIntA("display", "brown_titlebar", 1, ini) != 0;
            g_textLog = GetPrivateProfileIntA("text_log", "enabled", 0, ini) != 0;
            if (g_textLog) {
                char tp[MAX_PATH], *ts;
                strcpy(tp, path);
                ts = strrchr(tp, '\\');
                if (ts) strcpy(ts + 1, "imperialism_text.log");
                g_text = fopen(tp, "w");
            }
        }
        CreateThread(NULL, 0, summary_thread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH && g_log) {
        dump_counts();
    }
    return TRUE;
}

/* ---- msvfw32 forwarding: the only export the game imports ---- */
HWND __cdecl MCIWndCreateA(HWND parent, HINSTANCE inst, DWORD style, LPCSTR file)
{
    static HWND (__cdecl *real)(HWND, HINSTANCE, DWORD, LPCSTR);
    if (!real) {
        char sys[MAX_PATH];
        HMODULE m;
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\msvfw32.dll");
        m = LoadLibraryA(sys);
        if (m) real = (void *)GetProcAddress(m, "MCIWndCreateA");
    }
    return real ? real(parent, inst, style, file) : NULL;
}
