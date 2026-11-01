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
#include "backdrop.h"


#define LOG_LIMIT 40  /* detailed lines per API */

typedef struct {
    const char *dll;       /* Imported DLL name; matching ignores case. */
    const char *name;      /* Imported symbol name; matching is exact. */
    void *hook;           /* Replacement entry point, assigned before patch_iat. */
    void *orig;           /* Destination saved from the executable's IAT slot. */
    volatile LONG count;  /* Atomic diagnostic counter, not every wrapper increments it. */
} Hook;

static FILE *g_log;              /* Adjacent imperialism_gdi_log.txt, truncated on attach. */
static CRITICAL_SECTION g_cs;    /* Serializes log writes/ring updates, not window state. */

/* Write one timestamped diagnostic line. The shared critical section serializes this
 * file with periodic summaries and text-log writes; logging is skipped if opening failed. */
static void logf_(const char *format, ...)
{
    va_list arguments;
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "%8lu ", GetTickCount());
    va_start(arguments, format);
    vfprintf(g_log, format, arguments);
    va_end(arguments);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

/* H/DECL give each intercepted API one descriptor; LOGCALL counts every invocation
 * but prints only its initial LOG_LIMIT calls. ORIG uses the saved executable import.
 * Only the executable's IAT is changed: this DLL's own USER32/GDI calls stay real,
 * which is why helpers can call SetCapture/GetCursorPos/etc. without recursion. */
#define H(n) h_##n
#define DECL(dll, n) static Hook H(n) = { dll, #n, NULL, NULL, 0 }
#define LOGCALL(n, ...) do { \
        LONG c_ = InterlockedIncrement(&H(n).count); \
        if (c_ <= LOG_LIMIT) logf_(#n " " __VA_ARGS__); \
    } while (0)
#define ORIG(n, type) ((type)H(n).orig)

/* ------------------------------------------------------------------------
 * Mirror window and UI-thread state
 *
 * The game's top-level frame stays a normal 640x480-client window, but it is
 * parked directly under a larger "mirror" window that the game thread owns
 * (the frame is the mirror's owner, so the mirror always sits on top and has
 * no taskbar button). The mirror shows a DWM thumbnail of the frame's client
 * area scaled to fit, and translates mouse input back to frame coordinates.
 * Mirror creation, procedures and input routing run on the frame's UI thread.
 * summary_thread is a separate diagnostic worker: it queries window geometry and
 * reads these caches without a state lock. g_cs protects logs, not UI ownership.
 * ---------------------------------------------------------------------- */
static HWND g_frame;       /* Original game frame; owns the presentation window. */
static HWND g_mirror;      /* Owned top-level presentation window, not a child. */
static HWND g_capTarget;   /* Logical HWND returned to the game as its capture target. */
static HWND g_closeDown;   /* Game child whose non-client close button was pressed. */
static HTHUMBNAIL g_thumb; /* DWM registration: source frame, destination mirror. */
static RECT g_dest;                 /* thumbnail rectangle in mirror client coords */
static POINT g_virt;                /* mapped frame-client point + real frame screen origin */
static BOOL g_virtActive;           /* Whether game cursor queries use g_virt. */
static BOOL g_inMenu;               /* Defer focus handoff while the mirror menu is active. */
static BOOL g_mirrorCap;            /* Shim expects physical capture to belong to mirror. */
static BOOL g_fullscreen;           /* Mirror uses borderless monitor presentation. */
static BOOL g_shown;                /* One-time thumbnail/show setup has been requested. */
static BOOL g_tracking;             /* TME_LEAVE request is outstanding for mirror. */
static BOOL g_laidOut;              /* Initial geometry and normal frame size are recorded. */
static BOOL g_inSetCursor;          /* Diagnostic marker around forwarded cursor queries. */
static BOOL g_intScale = TRUE;      /* [display] fullscreen_integer_scale in the ini */
static BOOL g_brownTitle = TRUE;   /* [display] brown_titlebar in the ini: tint the title bar to match the backdrop (Windows 11) */
static BOOL g_blockModeChange = TRUE;  /* [display] block_mode_change in the ini: never change the desktop resolution */
static BOOL g_originalBackdrop = TRUE; /* [display] original_backdrop; brown remains the fallback. */
static Backdrop g_backdrop;            /* Optional stock bitmap/pattern, owned by the UI thread. */
static HMENU g_menu;                /* Original frame menu handle reused by the mirror. */
static LONG g_savedStyle;           /* Windowed mirror style before fullscreen. */
static RECT g_savedRect;            /* Windowed mirror outer rectangle in screen pixels. */
static HHOOK g_msgHook;             /* Frame UI thread's WH_GETMESSAGE hook. */
static WNDPROC g_frameProc;         /* Original frame procedure for subclass chaining. */
static int g_frameW, g_frameH;      /* the frame's normal outer size, recorded at layout */
/* A manual child-caption drag stores its press point and initial outer top-left
 * in frame-client pixels. This assumes the draggable child's parent is the frame. */
static struct { HWND window; POINT startFramePoint, startFramePosition; BOOL active; } g_drag;

static RECT g_src;  /* game view (AmbitGameWindow) in frame client coords: what the mirror shows */

/* Recompute g_src; returns TRUE if it changed. */
/* Refresh the visible source crop in frame-client pixels. Prefer the AmbitGameWindow
 * child intersected with its frame; use the whole frame client before that view exists.
 * Called on the UI thread; returns whether the cached crop changed. */
static BOOL refresh_src(void)
{
    RECT viewRect, frameClientRect, previousSourceRect = g_src;
    HWND gameView = FindWindowExA(g_frame, NULL, "AmbitGameWindow", NULL);
    GetClientRect(g_frame, &frameClientRect);
    if (gameView && GetWindowRect(gameView, &viewRect)) {
        MapWindowPoints(NULL, g_frame, (POINT *)&viewRect, 2);
        IntersectRect(&viewRect, &viewRect, &frameClientRect);
    } else {
        viewRect = frameClientRect;
    }
    g_src = viewRect;
    return memcmp(&previousSourceRect, &g_src, sizeof previousSourceRect) != 0;
}

/* Fit the cached source crop into the mirror client, preserving aspect ratio. Fullscreen
 * optionally rounds scales >= 1 down to an integer; unused space is backdrop. The crop
 * and destination are DWM client-pixel rectangles, not desktop coordinates. */
static void mirror_update(void)
{
    RECT mirrorClientRect, sourceSize;
    double scale, heightScale;
    int scaledWidth, scaledHeight;
    if (!g_mirror || !g_frame) return;
    refresh_src();
    GetClientRect(g_mirror, &mirrorClientRect);
    sourceSize.right = g_src.right - g_src.left;
    sourceSize.bottom = g_src.bottom - g_src.top;
    if (mirrorClientRect.right <= 0 || mirrorClientRect.bottom <= 0 || sourceSize.right <= 0 || sourceSize.bottom <= 0) return;
    scale = (double)mirrorClientRect.right / sourceSize.right;
    heightScale = (double)mirrorClientRect.bottom / sourceSize.bottom;
    if (heightScale < scale) scale = heightScale;
    if (g_fullscreen && g_intScale && scale >= 1.0) scale = (int)scale;  /* whole-number scale: sharp, brown border like the stock look */
    scaledWidth = (int)(sourceSize.right * scale);
    scaledHeight = (int)(sourceSize.bottom * scale);
    g_dest.left = (mirrorClientRect.right - scaledWidth) / 2;
    g_dest.top = (mirrorClientRect.bottom - scaledHeight) / 2;
    g_dest.right = g_dest.left + scaledWidth;
    g_dest.bottom = g_dest.top + scaledHeight;
    if (g_thumb) {
        DWM_THUMBNAIL_PROPERTIES properties;
        memset(&properties, 0, sizeof properties);
        properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = g_dest;
        properties.rcSource = g_src;
        properties.fVisible = TRUE;
        properties.opacity = 255;
        properties.fSourceClientAreaOnly = TRUE;
        DwmUpdateThumbnailProperties(g_thumb, &properties);
    }
}

/* Keep the real frame directly under the mirror's top-left corner. */
/* Park the real frame's outer top-left at the mirror's client origin in screen pixels.
 * Ownership keeps the mirror above the frame; moving the underlying frame keeps the
 * game's real child windows and virtual cursor near the displayed image. */
static void mirror_place_frame(void)
{
    POINT mirrorClientOrigin = { 0, 0 };
    if (!g_mirror || !g_frame) return;
    ClientToScreen(g_mirror, &mirrorClientOrigin);  /* client origin: the window rect includes invisible resize borders */
    SetWindowPos(g_frame, NULL, mirrorClientOrigin.x, mirrorClientOrigin.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Toggle the mirror between its saved window rectangle/style/menu and a borderless
 * popup covering its nearest monitor. The game frame and desktop display mode are not
 * resized here. Refit after flipping the flag because SetWindowPos dispatches WM_SIZE. */
static void mirror_toggle_fullscreen(void)
{
    if (!g_mirror) return;
    if (!g_fullscreen) {
        MONITORINFO monitorInfo;
        monitorInfo.cbSize = sizeof monitorInfo;
        g_savedStyle = GetWindowLongA(g_mirror, GWL_STYLE);
        GetWindowRect(g_mirror, &g_savedRect);
        GetMonitorInfoA(MonitorFromWindow(g_mirror, MONITOR_DEFAULTTONEAREST), &monitorInfo);
        SetMenu(g_mirror, NULL);
        SetWindowLongA(g_mirror, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_mirror, HWND_TOP, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                     monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
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

/* UI-thread WH_GETMESSAGE filter. Consume removed F11/Alt+Enter key-down messages
 * before the game's message loop sees them; bit 30 suppresses autorepeat toggles.
 * Other messages, including key-up, continue through the hook chain. */
static LRESULT CALLBACK msg_hook(int hookCode, WPARAM removalMode, LPARAM messagePointer)
{
    if (hookCode == HC_ACTION && removalMode == PM_REMOVE) {
        MSG *message = (MSG *)messagePointer;
        BOOL isF11 = message->message == WM_KEYDOWN && message->wParam == VK_F11;
        BOOL isAltEnter = message->message == WM_SYSKEYDOWN && message->wParam == VK_RETURN;
        if (isF11 || isAltEnter) {
            if (!(message->lParam & 0x40000000)) mirror_toggle_fullscreen();  /* ignore auto-repeat */
            message->message = WM_NULL;
        }
    }
    return CallNextHookEx(NULL, hookCode, removalMode, messagePointer);
}

/* mirror client point -> frame client point; TRUE if inside the image */
/* Map a packed mirror-client mouse position to a frame-client point using the DWM
 * source/destination rectangles. Return image containment separately: points outside
 * remain extrapolated so an active capture/drag can continue beyond the image. */
static BOOL map_point(LPARAM mirrorPosition, POINT *framePoint)
{
    int destinationWidth = g_dest.right - g_dest.left, destinationHeight = g_dest.bottom - g_dest.top;
    int mirrorX = GET_X_LPARAM(mirrorPosition), mirrorY = GET_Y_LPARAM(mirrorPosition);
    if (refresh_src()) mirror_update();  /* the game moved/resized its view */
    if (destinationWidth <= 0 || destinationHeight <= 0) { framePoint->x = framePoint->y = 0; return FALSE; }
    destinationWidth = g_dest.right - g_dest.left; destinationHeight = g_dest.bottom - g_dest.top;
    framePoint->x = g_src.left + MulDiv(mirrorX - g_dest.left, g_src.right - g_src.left, destinationWidth);
    framePoint->y = g_src.top + MulDiv(mirrorY - g_dest.top, g_src.bottom - g_src.top, destinationHeight);
    return mirrorX >= g_dest.left && mirrorX < g_dest.right && mirrorY >= g_dest.top && mirrorY < g_dest.bottom;
}

/* Deepest child of the frame under p (frame client coords); p becomes target-client coords. */
/* Resolve a frame-client point to a game HWND and convert it in place to that HWND's
 * client coordinates. A valid logical game capture overrides hit testing. Otherwise
 * descend visible children; disabled/transparent children are not explicitly skipped. */
static HWND resolve_target(POINT *targetPoint)
{
    HWND target = g_frame;
    if (g_capTarget && IsWindow(g_capTarget)) {
        MapWindowPoints(g_frame, g_capTarget, targetPoint, 1);
        return g_capTarget;
    }
    for (;;) {
        HWND child = ChildWindowFromPointEx(target, *targetPoint, CWP_SKIPINVISIBLE);
        if (!child || child == target) break;
        MapWindowPoints(target, child, targetPoint, 1);
        target = child;
    }
    return target;
}

/* Route a mirror mouse message to the original child synchronously on the UI thread.
 * Maintain the virtual screen cursor before dispatch, since handlers may query it.
 * Physical capture belongs to the mirror; g_capTarget represents the game's capture.
 * Caption dragging and close-button clicks on game child windows are emulated here. */
static LRESULT mirror_mouse(HWND mirrorWindow, UINT message, WPARAM buttonState, LPARAM mirrorPosition)
{
    POINT framePoint, frameScreenOrigin = { 0, 0 }, targetPoint;
    HWND target;
    BOOL insideImage = map_point(mirrorPosition, &framePoint);
    BOOL buttonDown = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
                message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDBLCLK;
    BOOL buttonUp = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP;

    if (!insideImage && !g_capTarget && !g_mirrorCap && !g_drag.active) { g_virtActive = FALSE; return 0; }
    if (!g_tracking) {
        TRACKMOUSEEVENT trackingRequest = { sizeof trackingRequest, TME_LEAVE, mirrorWindow, 0 };
        TrackMouseEvent(&trackingRequest);
        g_tracking = TRUE;
    }
    ClientToScreen(g_frame, &frameScreenOrigin);
    g_virt.x = frameScreenOrigin.x + framePoint.x;
    g_virt.y = frameScreenOrigin.y + framePoint.y;
    g_virtActive = TRUE;

    if (g_drag.active) {  /* manual caption drag of a game child window */
        if (message == WM_MOUSEMOVE)
            SetWindowPos(g_drag.window, NULL, g_drag.startFramePosition.x + framePoint.x - g_drag.startFramePoint.x,
                         g_drag.startFramePosition.y + framePoint.y - g_drag.startFramePoint.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        else if (message == WM_LBUTTONUP) { g_drag.active = FALSE; ReleaseCapture(); g_mirrorCap = FALSE; }
        return 0;
    }

    targetPoint = framePoint;
    target = resolve_target(&targetPoint);
    if (!g_capTarget && target != g_frame) {  /* non-client hits on game child windows */
        LRESULT hitTest = SendMessageA(target, WM_NCHITTEST, 0, MAKELPARAM(g_virt.x, g_virt.y));
        if (hitTest != HTCLIENT && hitTest != HTNOWHERE && hitTest != HTTRANSPARENT) {
            if (message == WM_LBUTTONDOWN) {
                BringWindowToTop(target);
                if (hitTest == HTCAPTION) {
                    RECT childWindowRect;
                    GetWindowRect(target, &childWindowRect);
                    MapWindowPoints(NULL, g_frame, (POINT *)&childWindowRect, 1);
                    g_drag.window = target; g_drag.active = TRUE;
                    g_drag.startFramePoint = framePoint; g_drag.startFramePosition.x = childWindowRect.left; g_drag.startFramePosition.y = childWindowRect.top;
                    SetCapture(mirrorWindow); g_mirrorCap = TRUE;
                } else if (hitTest == HTCLOSE) {
                    g_closeDown = target; SetCapture(mirrorWindow); g_mirrorCap = TRUE;
                }
            } else if (message == WM_LBUTTONUP && g_closeDown) {
                if (target == g_closeDown && hitTest == HTCLOSE) PostMessageA(target, WM_SYSCOMMAND, SC_CLOSE, 0);
                g_closeDown = NULL; ReleaseCapture(); g_mirrorCap = FALSE;
            }
            return 0;
        }
    }
    if (g_closeDown && buttonUp) { g_closeDown = NULL; }
    if (buttonDown && !GetCapture()) { SetCapture(mirrorWindow); g_mirrorCap = TRUE; }
    SendMessageA(target, message, buttonState, MAKELPARAM(targetPoint.x, targetPoint.y));
    if (buttonUp && g_mirrorCap && !g_capTarget && !(buttonState & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) {
        ReleaseCapture();
        g_mirrorCap = FALSE;
    }
    return 0;
}

/* Window procedure for the owned presentation window. Manage layout/background,
 * mirror-to-game mouse/menu routing, focus handoff, fullscreen restore/minimize, and
 * DWM thumbnail teardown. No independent render loop or keyboard emulation runs here. */
static LRESULT CALLBACK MirrorProc(HWND mirrorWindow, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED: {
        static LONG positionLogCount;
        WINDOWPOS *windowPosition = (WINDOWPOS *)lParam;
        if (InterlockedIncrement(&positionLogCount) <= 60)
            logf_("mirror %s pos=(%d,%d) size=%dx%d flags=%x", message == WM_WINDOWPOSCHANGING ? "poschanging" : "poschanged",
                  windowPosition->x, windowPosition->y, windowPosition->cx, windowPosition->cy, windowPosition->flags);
        break;
    }
    case WM_SHOWWINDOW:
        logf_("mirror WM_SHOWWINDOW show=%d reason=%d", (int)wParam, (int)lParam);
        if (wParam && g_laidOut) { mirror_place_frame(); mirror_update(); }  /* re-park after a restore */
        break;
    case WM_SYSCOMMAND:
        /* The mirror is owned by the frame and has no taskbar button, so minimize the
         * frame (which hides its owned mirror and leaves one taskbar button to restore). */
        if ((wParam & 0xFFF0) == SC_MINIMIZE && g_frame) {
            ShowWindow(g_frame, SW_MINIMIZE);
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        RECT clientRect;
        GetClientRect(mirrorWindow, &clientRect);
        backdrop_paint(&g_backdrop, (HDC)wParam, &clientRect);
        return 1;
    }
    case WM_SIZE:
        mirror_update();
        InvalidateRect(mirrorWindow, NULL, TRUE);
        break;
    case WM_MOVE:
        mirror_place_frame();
        break;
    case WM_GETMINMAXINFO: {
        RECT clientRect;
        MINMAXINFO *limits = (MINMAXINFO *)lParam;
        DefWindowProcA(mirrorWindow, message, wParam, lParam);  /* defaults first, then override */
        /* Client area must never be smaller than the frame it hides (else the frame pokes out
         * from under the mirror). Uses the frame's recorded normal size, not its current one,
         * because the game minimizes/maximizes it around shutdown. */
        if (g_laidOut && g_frameW > 0) {
            clientRect.left = clientRect.top = 0; clientRect.right = g_frameW; clientRect.bottom = g_frameH;
            AdjustWindowRect(&clientRect, GetWindowLongA(mirrorWindow, GWL_STYLE), GetMenu(mirrorWindow) != NULL);
            limits->ptMinTrackSize.x = clientRect.right - clientRect.left;
            limits->ptMinTrackSize.y = clientRect.bottom - clientRect.top;
        }
        return 0;
    }
    case WM_SETFOCUS:
        if (!g_inMenu) PostMessageA(mirrorWindow, WM_APP + 1, 0, 0);
        break;
    case WM_APP + 1:
        if (!g_inMenu && g_frame && !g_mirrorCap && GetFocus() == mirrorWindow) SetFocus(g_frame);
        return 0;
    case WM_ENTERMENULOOP: g_inMenu = TRUE; break;
    case WM_EXITMENULOOP:  g_inMenu = FALSE; PostMessageA(mirrorWindow, WM_APP + 1, 0, 0); break;
    case WM_INITMENU:
    case WM_INITMENUPOPUP:
    case WM_MENUSELECT:
        if (g_frame) SendMessageA(g_frame, message, wParam, lParam);
        break;
    case WM_COMMAND:
        if (g_frame) SendMessageA(g_frame, message, wParam, lParam);
        return 0;
    case WM_CLOSE:
        if (g_frame) SendMessageA(g_frame, WM_CLOSE, 0, 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT mirrorPoint, targetPoint;
            GetCursorPos(&mirrorPoint);
            ScreenToClient(mirrorWindow, &mirrorPoint);
            if (map_point(MAKELPARAM(mirrorPoint.x, mirrorPoint.y), &targetPoint) || g_capTarget) {
                HWND target = resolve_target(&targetPoint);
                LRESULT cursorHandled;
                g_inSetCursor = TRUE;
                cursorHandled = SendMessageA(target, WM_SETCURSOR, (WPARAM)target, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                g_inSetCursor = FALSE;
                {
                    static LONG cursorLogCount;
                    if (InterlockedIncrement(&cursorLogCount) <= 40)
                        logf_("mirror WM_SETCURSOR -> target %p class=%s handled=%ld", target, "", (long)cursorHandled);
                }
                (void)cursorHandled;  /* like Windows: after WM_SETCURSOR the cursor is whatever the game set,
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
        return mirror_mouse(mirrorWindow, message, wParam, lParam);
    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        if (!g_mirrorCap && !g_capTarget) g_virtActive = FALSE;
        return 0;
    case WM_DESTROY:
        backdrop_free(&g_backdrop);
        if (g_thumb) { DwmUnregisterThumbnail(g_thumb); g_thumb = NULL; }
        break;
    }
    return DefWindowProcA(mirrorWindow, message, wParam, lParam);
}

/* Subclass of the game's frame: however it gets maximized (the game does it itself, and
 * Windows restores it that way after minimize), "maximized" must not be bigger than normal. */
/* Subclass the original game frame. Let its handler run first, then cap maximize/
 * tracking dimensions at the recorded normal outer size so the frame stays hidden
 * beneath the mirror. This does not replace the game's drawing or event handler. */
static LRESULT CALLBACK FrameProc(HWND frameWindow, UINT message, WPARAM wParam, LPARAM lParam)
{
    static LONG messageLogCount;
    LRESULT originalResult;
    if ((message == WM_ACTIVATEAPP || message == WM_ACTIVATE || message == WM_SYSCOMMAND || message == WM_SIZE || message == WM_SHOWWINDOW ||
         message == WM_CLOSE || message == WM_KILLFOCUS || message == WM_SETFOCUS || message == WM_NCACTIVATE) && InterlockedIncrement(&messageLogCount) <= 200)
        logf_("frame msg 0x%04x w=%lx l=%lx fg=%p active=%p", message, (unsigned long)wParam, (unsigned long)lParam,
              GetForegroundWindow(), GetActiveWindow());
    originalResult = CallWindowProcA(g_frameProc, frameWindow, message, wParam, lParam);
    if (message == WM_GETMINMAXINFO && g_frameW > 0) {
        MINMAXINFO *limits = (MINMAXINFO *)lParam;
        limits->ptMaxSize.x = limits->ptMaxTrackSize.x = g_frameW;
        limits->ptMaxSize.y = limits->ptMaxTrackSize.y = g_frameH;
    }
    return originalResult;
}

/* Create the hidden owned mirror as soon as the first matching top-level game frame
 * exists. Register its class, optionally tint supported DWM caption attributes, subclass
 * the frame, and install a message hook on this same UI thread. Layout/show are deferred. */
static void mirror_init(HWND frameWindow)
{
    WNDCLASSEXA windowClass;
    g_frame = frameWindow;
    /* Read the running game's own bitmap outside DllMain/loader lock. Nothing is
     * bundled with the shim; missing artwork only removes the optional decoration. */
    if (g_originalBackdrop)
        logf_("stock backdrop resource %d: %s", BACKDROP_RESOURCE_ID,
              backdrop_load(&g_backdrop, GetModuleHandleW(NULL)) ? "loaded" : "unavailable; using brown");
    memset(&windowClass, 0, sizeof windowClass);
    windowClass.cbSize = sizeof windowClass;
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = MirrorProc;
    windowClass.hInstance = GetModuleHandleA(NULL);
    windowClass.hCursor = LoadCursorA(NULL, IDC_ARROW);
    windowClass.hIcon = (HICON)GetClassLongA(frameWindow, GCL_HICON);
    windowClass.hbrBackground = CreateSolidBrush(BACKDROP_BROWN);
    windowClass.lpszClassName = "ImperialismMirror";
    RegisterClassExA(&windowClass);
    g_mirror = CreateWindowExA(0, windowClass.lpszClassName, "Imperialism", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, 0, 656, 539, frameWindow, NULL, windowClass.hInstance, NULL);
    if (g_brownTitle) {
        /* DWMWA_CAPTION_COLOR 35, DWMWA_TEXT_COLOR 36, DWMWA_BORDER_COLOR 34, DWMWA_USE_IMMERSIVE_DARK_MODE 20:
         * only honoured on Windows 11 build 22000+; older systems just return an error and keep the normal bar. */
        COLORREF captionColor = BACKDROP_BROWN, captionTextColor = RGB(236, 214, 160);
        BOOL darkCaption = TRUE;
        DwmSetWindowAttribute(g_mirror, 20, &darkCaption, sizeof darkCaption);   /* light caption buttons on the dark bar */
        DwmSetWindowAttribute(g_mirror, 35, &captionColor, sizeof captionColor);
        DwmSetWindowAttribute(g_mirror, 34, &captionColor, sizeof captionColor);
        DwmSetWindowAttribute(g_mirror, 36, &captionTextColor, sizeof captionTextColor);
    }
    g_frameProc = (WNDPROC)SetWindowLongA(frameWindow, GWL_WNDPROC, (LONG)FrameProc);
    g_msgHook = SetWindowsHookExA(WH_GETMESSAGE, msg_hook, NULL, GetCurrentThreadId());
    logf_("mirror created %p (frame %p) hook=%p", g_mirror, frameWindow, g_msgHook);
}

/* Initial size/position, done once the frame has its real size (it is shown
 * first at a bogus ~1940x39 and resized to 656x539 a moment later). */
/* Choose the initial windowed size only once a plausible game view exists. Pick the
 * largest integer scale from 6 down to 1 fitting the primary work area, add mirror
 * decorations/menu, center it, record the frame's normal outer size, then park/refit. */
static void mirror_layout(void)
{
    RECT frameWindowRect, sourceSize, workArea, mirrorWindowRect;
    DWORD mirrorStyle;
    int integerScale, windowX, windowY;
    if (!g_mirror || g_laidOut) return;
    refresh_src();
    sourceSize.left = sourceSize.top = 0;
    sourceSize.right = g_src.right - g_src.left;   /* the game view, not the whole frame */
    sourceSize.bottom = g_src.bottom - g_src.top;
    if (sourceSize.right < 320 || sourceSize.bottom < 200) return;
    g_laidOut = TRUE;
    g_menu = GetMenu(g_frame);
    if (g_menu) SetMenu(g_mirror, g_menu);
    GetWindowRect(g_frame, &frameWindowRect);
    g_frameW = frameWindowRect.right - frameWindowRect.left;
    g_frameH = frameWindowRect.bottom - frameWindowRect.top;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
    mirrorStyle = GetWindowLongA(g_mirror, GWL_STYLE);
    for (integerScale = 6; integerScale >= 1; integerScale--) {  /* largest integer scale that fits the work area */
        mirrorWindowRect.left = mirrorWindowRect.top = 0; mirrorWindowRect.right = sourceSize.right * integerScale; mirrorWindowRect.bottom = sourceSize.bottom * integerScale;
        AdjustWindowRect(&mirrorWindowRect, mirrorStyle, g_menu != NULL);
        if (mirrorWindowRect.right - mirrorWindowRect.left <= workArea.right - workArea.left && mirrorWindowRect.bottom - mirrorWindowRect.top <= workArea.bottom - workArea.top) break;
    }
    if (integerScale < 1) integerScale = 1;
    mirrorWindowRect.left = mirrorWindowRect.top = 0; mirrorWindowRect.right = sourceSize.right * integerScale; mirrorWindowRect.bottom = sourceSize.bottom * integerScale;
    AdjustWindowRect(&mirrorWindowRect, mirrorStyle, g_menu != NULL);
    windowX = workArea.left + ((workArea.right - workArea.left) - (mirrorWindowRect.right - mirrorWindowRect.left)) / 2;
    windowY = workArea.top + ((workArea.bottom - workArea.top) - (mirrorWindowRect.bottom - mirrorWindowRect.top)) / 2;
    SetWindowPos(g_mirror, NULL, windowX, windowY, mirrorWindowRect.right - mirrorWindowRect.left, mirrorWindowRect.bottom - mirrorWindowRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    mirror_place_frame();
    mirror_update();
    if (g_shown) ShowWindow(g_mirror, SW_SHOW);
    logf_("mirror laid out: frame window %ldx%ld client %ldx%ld menu=%p scale=%d",
          frameWindowRect.right - frameWindowRect.left, frameWindowRect.bottom - frameWindowRect.top, sourceSize.right, sourceSize.bottom, g_menu, integerScale);
}

/* One-time presentation activation after a game ShowWindow call. Register the DWM
 * thumbnail and show only after valid layout; later layout can complete the reveal.
 * A failed DWM registration is logged but has no software-rendering fallback. */
static void mirror_show(void)
{
    if (!g_mirror || g_shown) return;
    g_shown = TRUE;
    if (DwmRegisterThumbnail(g_mirror, g_frame, &g_thumb) != S_OK) g_thumb = NULL;
    logf_("mirror shown: thumb=%p", g_thumb);
    if (g_laidOut) ShowWindow(g_mirror, SW_SHOW);
    else mirror_layout();
}

/* ---- GDI32 diagnostic wrappers ----
 * These wrappers log arguments/results and forward unchanged. They do not scale
 * pixels, replace palettes, resize backing bitmaps or implement rendering. DWM
 * performs the presentation scaling independently of the game's drawing calls. */
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
/* ---- USER32 imports: diagnostics plus window/input virtualization ---- */
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

static BOOL WINAPI w_BitBlt(HDC destinationDC, int destinationX, int destinationY, int width, int height, HDC sourceDC, int sourceX, int sourceY, DWORD rasterOperation)
{
    LOGCALL(BitBlt, "dst=%p (%d,%d %dx%d) src=%p (%d,%d) rop=%08lx", destinationDC, destinationX, destinationY, width, height, sourceDC, sourceX, sourceY, rasterOperation);
    return ORIG(BitBlt, BOOL (WINAPI *)(HDC,int,int,int,int,HDC,int,int,DWORD))(destinationDC, destinationX, destinationY, width, height, sourceDC, sourceX, sourceY, rasterOperation);
}
static BOOL WINAPI w_PatBlt(HDC destinationDC, int destinationX, int destinationY, int width, int height, DWORD rasterOperation)
{
    LOGCALL(PatBlt, "dst=%p (%d,%d %dx%d) rop=%08lx", destinationDC, destinationX, destinationY, width, height, rasterOperation);
    return ORIG(PatBlt, BOOL (WINAPI *)(HDC,int,int,int,int,DWORD))(destinationDC, destinationX, destinationY, width, height, rasterOperation);
}
static int WINAPI w_StretchDIBits(HDC destinationDC, int destinationX, int destinationY, int destinationWidth, int destinationHeight, int sourceX, int sourceY, int sourceWidth, int sourceHeight,
                                  const void *bits, const BITMAPINFO *bitmapInfo, UINT colorUsage, DWORD rasterOperation)
{
    LOGCALL(StretchDIBits, "dst=%p (%d,%d %dx%d) src=(%d,%d %dx%d) bpp=%d use=%u rop=%08lx",
            destinationDC, destinationX, destinationY, destinationWidth, destinationHeight, sourceX, sourceY, sourceWidth, sourceHeight, bitmapInfo ? bitmapInfo->bmiHeader.biBitCount : -1, colorUsage, rasterOperation);
    return ORIG(StretchDIBits, int (WINAPI *)(HDC,int,int,int,int,int,int,int,int,const void*,const BITMAPINFO*,UINT,DWORD))
        (destinationDC, destinationX, destinationY, destinationWidth, destinationHeight, sourceX, sourceY, sourceWidth, sourceHeight, bits, bitmapInfo, colorUsage, rasterOperation);
}
static HBITMAP WINAPI w_CreateDIBSection(HDC deviceContext, const BITMAPINFO *bitmapInfo, UINT colorUsage, void **pixelStorage, HANDLE section, DWORD offset)
{
    LOGCALL(CreateDIBSection, "%ldx%ld bpp=%d clrUsed=%lu use=%u",
            bitmapInfo ? bitmapInfo->bmiHeader.biWidth : 0, bitmapInfo ? bitmapInfo->bmiHeader.biHeight : 0,
            bitmapInfo ? bitmapInfo->bmiHeader.biBitCount : 0, bitmapInfo ? bitmapInfo->bmiHeader.biClrUsed : 0, colorUsage);
    return ORIG(CreateDIBSection, HBITMAP (WINAPI *)(HDC,const BITMAPINFO*,UINT,void**,HANDLE,DWORD))(deviceContext, bitmapInfo, colorUsage, pixelStorage, section, offset);
}
static HBITMAP WINAPI w_CreateDIBitmap(HDC deviceContext, const BITMAPINFOHEADER *header, DWORD initialization, const void *pixels, const BITMAPINFO *bitmapInfo, UINT colorUsage)
{
    LOGCALL(CreateDIBitmap, "%ldx%ld bpp=%d init=%lu", header ? header->biWidth : 0, header ? header->biHeight : 0, header ? header->biBitCount : 0, initialization);
    return ORIG(CreateDIBitmap, HBITMAP (WINAPI *)(HDC,const BITMAPINFOHEADER*,DWORD,const void*,const BITMAPINFO*,UINT))(deviceContext, header, initialization, pixels, bitmapInfo, colorUsage);
}
static HDC WINAPI w_CreateCompatibleDC(HDC sourceDC)
{
    HDC createdDC = ORIG(CreateCompatibleDC, HDC (WINAPI *)(HDC))(sourceDC);
    LOGCALL(CreateCompatibleDC, "from=%p -> %p", sourceDC, createdDC);
    return createdDC;
}
static HBITMAP WINAPI w_CreateCompatibleBitmap(HDC deviceContext, int width, int height)
{
    LOGCALL(CreateCompatibleBitmap, "%dx%d", width, height);
    return ORIG(CreateCompatibleBitmap, HBITMAP (WINAPI *)(HDC,int,int))(deviceContext, width, height);
}
static HPALETTE WINAPI w_CreatePalette(const LOGPALETTE *palette)
{
    LOGCALL(CreatePalette, "entries=%u", palette ? palette->palNumEntries : 0);
    return ORIG(CreatePalette, HPALETTE (WINAPI *)(const LOGPALETTE*))(palette);
}
static HPALETTE WINAPI w_SelectPalette(HDC deviceContext, HPALETTE palette, BOOL forceBackground)
{
    LOGCALL(SelectPalette, "dc=%p pal=%p bg=%d", deviceContext, palette, forceBackground);
    return ORIG(SelectPalette, HPALETTE (WINAPI *)(HDC,HPALETTE,BOOL))(deviceContext, palette, forceBackground);
}
static UINT WINAPI w_RealizePalette(HDC deviceContext)
{
    LOGCALL(RealizePalette, "dc=%p", deviceContext);
    return ORIG(RealizePalette, UINT (WINAPI *)(HDC))(deviceContext);
}
static int WINAPI w_SetStretchBltMode(HDC deviceContext, int stretchMode)
{
    LOGCALL(SetStretchBltMode, "mode=%d", stretchMode);
    return ORIG(SetStretchBltMode, int (WINAPI *)(HDC,int))(deviceContext, stretchMode);
}
static int WINAPI w_GetDeviceCaps(HDC deviceContext, int capabilityIndex)
{
    int capability = ORIG(GetDeviceCaps, int (WINAPI *)(HDC,int))(deviceContext, capabilityIndex);
    LOGCALL(GetDeviceCaps, "index=%d -> %d", capabilityIndex, capability);
    return capability;
}
static BOOL WINAPI w_LineTo(HDC deviceContext, int endX, int endY)
{
    LOGCALL(LineTo, "(%d,%d)", endX, endY);
    return ORIG(LineTo, BOOL (WINAPI *)(HDC,int,int))(deviceContext, endX, endY);
}
/* Intercept only main-executable ChangeDisplaySettingsA imports. With mode blocking
 * enabled, report success for both a mode request and its restore without changing
 * the desktop. Otherwise preserve the original API's arguments and result. */
static LONG WINAPI w_ChangeDisplaySettingsA(DEVMODEA *displayMode, DWORD flags)
{
    LONG changeResult;
    if (g_blockModeChange) {
        /* The game asks for 640x480 (or restores the old mode on exit). We scale in a window instead, so pretend
         * it worked and leave the desktop alone. */
        LOGCALL(ChangeDisplaySettingsA, "%lux%lu flags=%lx -> blocked", displayMode ? displayMode->dmPelsWidth : 0,
                displayMode ? displayMode->dmPelsHeight : 0, flags);
        return DISP_CHANGE_SUCCESSFUL;
    }
    changeResult = ORIG(ChangeDisplaySettingsA, LONG (WINAPI *)(DEVMODEA*,DWORD))(displayMode, flags);
    LOGCALL(ChangeDisplaySettingsA, "%lux%lu bpp=%lu flags=%lx -> %ld",
            displayMode ? displayMode->dmPelsWidth : 0, displayMode ? displayMode->dmPelsHeight : 0, displayMode ? displayMode->dmBitsPerPel : 0, flags, changeResult);
    return changeResult;
}
/* Forward creation, then discover the first unowned top-level window titled exactly
 * Imperialism with a string class name. Class atoms are logged safely but do not
 * trigger discovery. Creating the mirror uses this DLL's own, unpatched USER32 import. */
static HWND WINAPI w_CreateWindowExA(DWORD extendedStyle, LPCSTR className, LPCSTR windowTitle, DWORD style, int windowX, int windowY, int width, int height,
                                     HWND parent, HMENU menu, HINSTANCE instance, LPVOID creationData)
{
    HWND createdWindow = ORIG(CreateWindowExA, HWND (WINAPI *)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID))
        (extendedStyle, className, windowTitle, style, windowX, windowY, width, height, parent, menu, instance, creationData);
    if (createdWindow && !parent && HIWORD(className) != 0 && windowTitle && !strcmp(windowTitle, "Imperialism") && !g_frame) mirror_init(createdWindow);
    if (HIWORD(className) == 0) LOGCALL(CreateWindowExA, "atom=%u style=%08lx ex=%08lx (%d,%d %dx%d) parent=%p -> %p", (unsigned)LOWORD(className), style, extendedStyle, windowX, windowY, width, height, parent, createdWindow);
    else LOGCALL(CreateWindowExA, "class=\"%s\" title=\"%s\" style=%08lx ex=%08lx (%d,%d %dx%d) parent=%p -> %p",
                 className, windowTitle ? windowTitle : "", style, extendedStyle, windowX, windowY, width, height, parent, createdWindow);
    return createdWindow;
}
static HDC WINAPI w_GetDC(HWND window)
{
    HDC deviceContext = ORIG(GetDC, HDC (WINAPI *)(HWND))(window);
    LOGCALL(GetDC, "wnd=%p -> %p", window, deviceContext);
    return deviceContext;
}
static int WINAPI w_ReleaseDC(HWND window, HDC deviceContext)
{
    LOGCALL(ReleaseDC, "wnd=%p dc=%p", window, deviceContext);
    return ORIG(ReleaseDC, int (WINAPI *)(HWND,HDC))(window, deviceContext);
}
static HDC WINAPI w_BeginPaint(HWND window, PAINTSTRUCT *paint)
{
    HDC deviceContext = ORIG(BeginPaint, HDC (WINAPI *)(HWND,PAINTSTRUCT*))(window, paint);
    LOGCALL(BeginPaint, "wnd=%p dc=%p rc=(%ld,%ld,%ld,%ld)", window, deviceContext, paint->rcPaint.left, paint->rcPaint.top, paint->rcPaint.right, paint->rcPaint.bottom);
    return deviceContext;
}
static BOOL WINAPI w_EndPaint(HWND window, const PAINTSTRUCT *paint)
{
    LOGCALL(EndPaint, "wnd=%p", window);
    return ORIG(EndPaint, BOOL (WINAPI *)(HWND,const PAINTSTRUCT*))(window, paint);
}
/* Keep game-originated frame moves anchored beneath the mirror, while forwarding
 * sizing/z-order flags and other HWNDs unchanged. A frame resize may finish deferred
 * first layout; all later frame changes refresh the thumbnail fit. */
static BOOL WINAPI w_SetWindowPos(HWND window, HWND insertAfter, int windowX, int windowY, int width, int height, UINT flags)
{
    BOOL positioned;
    LOGCALL(SetWindowPos, "wnd=%p (%d,%d %dx%d) flags=%x", window, windowX, windowY, width, height, flags);
    if (window == g_frame && g_mirror && !(flags & SWP_NOMOVE)) {  /* keep the frame parked under the mirror */
        POINT mirrorClientOrigin = { 0, 0 };
        ClientToScreen(g_mirror, &mirrorClientOrigin);
        windowX = mirrorClientOrigin.x; windowY = mirrorClientOrigin.y;
    }
    positioned = ORIG(SetWindowPos, BOOL (WINAPI *)(HWND,HWND,int,int,int,int,UINT))(window, insertAfter, windowX, windowY, width, height, flags);
    if (window == g_frame && g_mirror) {  /* frame size may have changed */
        if (g_shown && !g_laidOut) mirror_layout();
        mirror_update();
    }
    return positioned;
}
/* Block game-originated self-minimize requests once the mirror exists. Forward other
 * commands, hide the mirror when the frame hides, and otherwise trigger one-time
 * presentation setup. The mirror's own minimize path bypasses this patched IAT. */
static BOOL WINAPI w_ShowWindow(HWND window, int showCommand)
{
    BOOL previousVisibility;
    LOGCALL(ShowWindow, "wnd=%p cmd=%d", window, showCommand);
    if (g_mirror && window == g_frame &&
        (showCommand == SW_MINIMIZE || showCommand == SW_SHOWMINIMIZED || showCommand == SW_SHOWMINNOACTIVE || showCommand == SW_FORCEMINIMIZE)) {
        logf_("frame self-minimize via ShowWindow(%d) blocked", showCommand);
        return TRUE;
    }
    previousVisibility = ORIG(ShowWindow, BOOL (WINAPI *)(HWND,int))(window, showCommand);
    if (window == g_frame && g_mirror) {
        if (showCommand == SW_HIDE) ShowWindow(g_mirror, SW_HIDE);
        else mirror_show();
    }
    return previousVisibility;
}
/* Something in the game resizes whatever window it thinks is its main one; make sure
 * that is never the mirror. Logged with the caller's address for tracing. */
/* Log game MoveWindow calls and their return addresses. Swallow a move directed at
 * the mirror, which the game's main-window logic must not resize; forward all other
 * targets unchanged. This does not itself re-park game frame moves. */
static BOOL WINAPI w_MoveWindow(HWND window, int windowX, int windowY, int width, int height, BOOL repaint)
{
    LONG callCount = InterlockedIncrement(&H(MoveWindow).count);
    if (callCount <= LOG_LIMIT)
        logf_("MoveWindow wnd=%p (%s) (%d,%d %dx%d) caller=%p", window,
              window == g_mirror ? "MIRROR" : window == g_frame ? "frame" : "other", windowX, windowY, width, height, __builtin_return_address(0));
    if (g_mirror && window == g_mirror) return TRUE;  /* swallow */
    return ORIG(MoveWindow, BOOL (WINAPI *)(HWND,int,int,int,int,BOOL))(window, windowX, windowY, width, height, repaint);
}

/* Log what cursor the game asks for, to check that cursor forwarding really takes effect. */
/* Forward through this DLL's normal SetCursor import and record up to 80 calls.
 * g_inSetCursor identifies synchronous mirror cursor queries for diagnostics only. */
static HCURSOR WINAPI w_SetCursor(HCURSOR cursor)
{
    LONG callCount = InterlockedIncrement(&H(SetCursor).count);
    HCURSOR previousCursor = SetCursor(cursor);
    if (callCount <= 80) logf_("SetCursor %p (prev %p) fromMirrorSetCursor=%d", cursor, previousCursor, g_inSetCursor);
    return previousCursor;
}

/* The game maximizes its frame (FUN_00484d70) and centres a 640x480 view on a backdrop.
 * Keep the frame a normal small window so it can sit under the mirror. */
/* Rewrite game frame maximize to normal presentation, using a local structure copy.
 * Swallow game frame minimize requests; other targets/commands reach USER32 intact.
 * This hook and FrameProc cover distinct placement and maximize-size paths. */
static BOOL WINAPI w_SetWindowPlacement(HWND window, const WINDOWPLACEMENT *placement)
{
    WINDOWPLACEMENT normalPlacement;
    if (window == g_frame && placement) logf_("SetWindowPlacement frame showCmd=%u flags=%x", placement->showCmd, placement->flags);
    if (g_mirror && window == g_frame && placement && placement->showCmd == SW_SHOWMAXIMIZED) {
        normalPlacement = *placement;
        normalPlacement.showCmd = SW_SHOWNORMAL;
        placement = &normalPlacement;
        logf_("frame maximize suppressed");
    } else if (g_mirror && window == g_frame && placement &&
               (placement->showCmd == SW_SHOWMINIMIZED || placement->showCmd == SW_MINIMIZE || placement->showCmd == SW_SHOWMINNOACTIVE)) {
        logf_("frame self-minimize via SetWindowPlacement blocked");
        return TRUE;  /* the game minimizes itself when it loses focus; a windowed game shouldn't */
    }
    return ORIG(SetWindowPlacement, BOOL (WINAPI *)(HWND,const WINDOWPLACEMENT*))(window, placement);
}
/* Virtualize game capture: retain the requested game HWND but give physical mouse
 * capture to the mirror so scaled/outside-image messages still arrive there. Other
 * HWNDs use the real API. Return the previous logical game target for this path. */
static HWND WINAPI w_SetCapture(HWND window)
{
    HWND previousTarget;
    if (g_mirror && window && (window == g_frame || IsChild(g_frame, window))) {  /* route the game's capture through the mirror */
        previousTarget = g_capTarget;
        g_capTarget = window;
        SetCapture(g_mirror);
        g_mirrorCap = TRUE;
        return previousTarget;
    }
    return SetCapture(window);
}
/* Release physical capture and clear the two logical capture flags. Drag/close state
 * is managed by mirror_mouse rather than reset here. */
static BOOL WINAPI w_ReleaseCapture(void)
{
    g_capTarget = NULL;
    g_mirrorCap = FALSE;
    return ReleaseCapture();
}
/* Expose the game's logical capture target when set; otherwise report real capture. */
static HWND WINAPI w_GetCapture(void)
{
    return g_capTarget ? g_capTarget : GetCapture();
}
/* While mirror input is active, expose the mapped frame-space screen cursor to the
 * game. The shim's own GetCursorPos calls still see the real desktop cursor. */
static BOOL WINAPI w_GetCursorPos(POINT *screenPoint)
{
    if (g_mirror && g_virtActive && screenPoint) { *screenPoint = g_virt; return TRUE; }
    return GetCursorPos(screenPoint);
}
/* Expose the mapped virtual screen point in Win32's packed 16-bit coordinate format
 * while mirror input is active; otherwise preserve the real queued-message position. */
static DWORD WINAPI w_GetMessagePos(void)
{
    if (g_mirror && g_virtActive) return MAKELONG(g_virt.x, g_virt.y);
    return GetMessagePos();
}

/* ---- text log: the strings the game draws, with positions (off unless [text_log] enabled=1 in imperialism_shim.ini) ----
 * Lets a test harness READ what is on screen (offer sheets, hot text, dialogs) instead of looking at pixels. One line per string:
 * "<tick> <T|X|B|D> <x>,<y> <text>" in imperialism_text.log; the same string at the same place is not repeated within 1.5 s. */
static BOOL g_textLog;              /* Startup-only INI opt-in. */
static FILE *g_text;                /* Adjacent imperialism_text.log, truncated on attach. */
static struct { unsigned hash; DWORD timestamp; } g_recent[128]; /* Hash/tick ring, guarded by g_cs. */
static unsigned g_recentN;          /* Next ring entry modulo 128; guarded by g_cs. */

/* Record ANSI draw text before forwarding the drawing API. Limit to 400 bytes, replace
 * control bytes with spaces and trim trailing spaces. The rolling hash includes the
 * original bytes and draw position, but not API kind or HDC. Under g_cs, suppress a
 * matching hash for 1500 ms using a 128-slot ring, then append/flush the text log. */
static void text_out(const char *kind, int drawX, int drawY, const char *text, int length)
{
    char line[420];
    int index, outputLength = 0;
    unsigned hash = 2166136261u;
    DWORD timestamp;
    if (!g_textLog || !g_text || !text) return;
    if (length < 0) length = (int)strlen(text);
    if (length > 400) length = 400;
    for (index = 0; index < length; index++) {
        unsigned char character = (unsigned char)text[index];
        line[outputLength++] = (character < 32) ? ' ' : (char)character;
        hash = (hash ^ character) * 16777619u;
    }
    while (outputLength && line[outputLength - 1] == ' ') outputLength--;
    line[outputLength] = 0;
    if (!outputLength) return;
    hash = (hash ^ (unsigned)drawX) * 16777619u;
    hash = (hash ^ (unsigned)drawY) * 16777619u;
    timestamp = GetTickCount();
    EnterCriticalSection(&g_cs);
    for (index = 0; index < 128; index++)
        if (g_recent[index].hash == hash && timestamp - g_recent[index].timestamp < 1500) { LeaveCriticalSection(&g_cs); return; }
    g_recent[g_recentN & 127].hash = hash;
    g_recent[g_recentN & 127].timestamp = timestamp;
    g_recentN++;
    fprintf(g_text, "%lu %s %d,%d %s\n", timestamp, kind, drawX, drawY, line);
    fflush(g_text);
    LeaveCriticalSection(&g_cs);
}

DECL("GDI32.dll", TextOutA);
DECL("GDI32.dll", ExtTextOutA);
DECL("USER32.dll", TabbedTextOutA);
DECL("USER32.dll", DrawTextA);

/* ANSI text taps preserve the original drawing call and return value. Glyph-index
 * ExtTextOut and DrawText layout-only calls are not readable text, so skip those
 * log entries. Coordinates belong to each supplied HDC, not the mirror/desktop.
 * These wrappers intentionally do not use LOGCALL or increment its counters. */
static BOOL WINAPI w_TextOutA(HDC deviceContext, int drawX, int drawY, LPCSTR text, int length)
{
    text_out("T", drawX, drawY, text, length);
    return ORIG(TextOutA, BOOL (WINAPI *)(HDC,int,int,LPCSTR,int))(deviceContext, drawX, drawY, text, length);
}
static BOOL WINAPI w_ExtTextOutA(HDC deviceContext, int drawX, int drawY, UINT options, const RECT *clippingRect, LPCSTR text, UINT length, const INT *advances)
{
    if (!(options & ETO_GLYPH_INDEX)) text_out("X", drawX, drawY, text, (int)length);
    return ORIG(ExtTextOutA, BOOL (WINAPI *)(HDC,int,int,UINT,const RECT*,LPCSTR,UINT,const INT*))(deviceContext, drawX, drawY, options, clippingRect, text, length, advances);
}
static LONG WINAPI w_TabbedTextOutA(HDC deviceContext, int drawX, int drawY, LPCSTR text, int length, int tabCount, const INT *tabStops, int tabOrigin)
{
    text_out("B", drawX, drawY, text, length);
    return ORIG(TabbedTextOutA, LONG (WINAPI *)(HDC,int,int,LPCSTR,int,int,const INT*,int))(deviceContext, drawX, drawY, text, length, tabCount, tabStops, tabOrigin);
}
static int WINAPI w_DrawTextA(HDC deviceContext, LPCSTR text, int length, LPRECT drawRect, UINT format)
{
    if (!(format & DT_CALCRECT) && drawRect) text_out("D", drawRect->left, drawRect->top, text, length);
    return ORIG(DrawTextA, int (WINAPI *)(HDC,LPCSTR,int,LPRECT,UINT))(deviceContext, text, length, drawRect, format);
}

/* Single registration order for patching and summaries. Absent named imports are
 * harmless: their descriptors remain unpatched and are listed as NOT-IMPORTED. */
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

/* Bind the statically declared import descriptors to wrappers before touching the IAT.
 * Descriptors for APIs absent from the executable keep a NULL original pointer. */
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
/* Walk the main executable's PE32 named imports and replace exact API-name matches
 * in matching DLLs (case-insensitive DLL comparison). Temporarily make each slot
 * writable, save its original destination, patch, then restore protection. Ordinal
 * imports, delay imports and other modules are outside this one-time walk. */
static int patch_iat(void)
{
    BYTE *imageBase = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dosHeader = (IMAGE_DOS_HEADER *)imageBase;
    IMAGE_NT_HEADERS *ntHeaders = (IMAGE_NT_HEADERS *)(imageBase + dosHeader->e_lfanew);
    IMAGE_DATA_DIRECTORY importDirectory = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *importDescriptor;
    int patched = 0;

    if (!importDirectory.VirtualAddress) return 0;
    for (importDescriptor = (IMAGE_IMPORT_DESCRIPTOR *)(imageBase + importDirectory.VirtualAddress); importDescriptor->Name; importDescriptor++) {
        const char *importDll = (const char *)(imageBase + importDescriptor->Name);
        IMAGE_THUNK_DATA *nameThunk = (IMAGE_THUNK_DATA *)(imageBase + (importDescriptor->OriginalFirstThunk ? importDescriptor->OriginalFirstThunk : importDescriptor->FirstThunk));
        IMAGE_THUNK_DATA *addressThunk = (IMAGE_THUNK_DATA *)(imageBase + importDescriptor->FirstThunk);
        for (; nameThunk->u1.AddressOfData; nameThunk++, addressThunk++) {
            const IMAGE_IMPORT_BY_NAME *importByName;
            size_t hookIndex;
            if (IMAGE_SNAP_BY_ORDINAL32(nameThunk->u1.Ordinal)) continue;
            importByName = (const IMAGE_IMPORT_BY_NAME *)(imageBase + nameThunk->u1.AddressOfData);
            for (hookIndex = 0; hookIndex < NHOOKS; hookIndex++) {
                Hook *hook = g_hooks[hookIndex];
                DWORD previousProtection;
                if (_stricmp(importDll, hook->dll) || strcmp((const char *)importByName->Name, hook->name)) continue;
                if (!VirtualProtect(&addressThunk->u1.Function, sizeof(void *), PAGE_READWRITE, &previousProtection)) break;
                hook->orig = (void *)addressThunk->u1.Function;
                addressThunk->u1.Function = (DWORD)(ULONG_PTR)hook->hook;
                VirtualProtect(&addressThunk->u1.Function, sizeof(void *), previousProtection, &previousProtection);
                patched++;
                break;
            }
        }
    }
    return patched;
}

/* Append current window/crop/thumbnail geometry to g_log. Caller owns g_cs. The
 * summary worker queries HWND geometry but reads UI-owned caches without a snapshot
 * lock, so the report is diagnostic and may mix adjacent UI states. */
static void diag(void)
{
    RECT mirrorWindowRect, mirrorClientRect, frameWindowRect, frameClientRect;
    SIZE thumbnailSourceSize = { 0, 0 };
    if (!g_mirror || !g_frame) return;
    GetWindowRect(g_mirror, &mirrorWindowRect); GetClientRect(g_mirror, &mirrorClientRect);
    GetWindowRect(g_frame, &frameWindowRect);  GetClientRect(g_frame, &frameClientRect);
    if (g_thumb) DwmQueryThumbnailSourceSize(g_thumb, &thumbnailSourceSize);
    fprintf(g_log, "diag mirror win(%ld,%ld %ldx%ld) client %ldx%ld zoomed=%d | frame win(%ld,%ld %ldx%ld) client %ldx%ld zoomed=%d | thumb src %ldx%ld dest(%ld,%ld %ldx%ld) laidOut=%d\n",
            mirrorWindowRect.left, mirrorWindowRect.top, mirrorWindowRect.right - mirrorWindowRect.left, mirrorWindowRect.bottom - mirrorWindowRect.top, mirrorClientRect.right, mirrorClientRect.bottom, IsZoomed(g_mirror),
            frameWindowRect.left, frameWindowRect.top, frameWindowRect.right - frameWindowRect.left, frameWindowRect.bottom - frameWindowRect.top, frameClientRect.right, frameClientRect.bottom, IsZoomed(g_frame),
            thumbnailSourceSize.cx, thumbnailSourceSize.cy, g_dest.left, g_dest.top, g_dest.right - g_dest.left, g_dest.bottom - g_dest.top, g_laidOut);
    fprintf(g_log, "diag view src(%ld,%ld %ldx%ld)\n", g_src.left, g_src.top, g_src.right - g_src.left, g_src.bottom - g_src.top);
}

/* Write a geometry diagnostic and one row per registered hook under g_cs. A non-NULL
 * saved destination means patched; counters cover only wrappers that increment them,
 * so a zero row is not proof the API was never called. */
static void dump_counts(void)
{
    size_t hookIndex;
    if (!g_log) return; /* An unwritable diagnostic path must not crash the worker. */
    EnterCriticalSection(&g_cs);
    diag();
    fprintf(g_log, "---- call counts ----\n");
    for (hookIndex = 0; hookIndex < NHOOKS; hookIndex++)
        fprintf(g_log, "%-24s %s %ld\n", g_hooks[hookIndex]->name, g_hooks[hookIndex]->orig ? "hooked" : "NOT-IMPORTED", g_hooks[hookIndex]->count);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

/* Process-lifetime diagnostic worker: sleep five seconds and append a summary forever.
 * It does not own or render either window. No shutdown signal/join exists; explicit
 * FreeLibrary/hot unload is unsupported. Normal process exit ends the worker. */
static DWORD WINAPI summary_thread(LPVOID unusedContext)
{
    (void)unusedContext;
    for (;;) { Sleep(5000); dump_counts(); }
    return 0;
}

/* Process attach initializes logging, DPI awareness and the main executable's IAT,
 * reads the adjacent INI once, optionally opens the text log, then starts summaries.
 * Detach only writes a final summary: it does not restore IAT/subclasses/hooks or close
 * all resources. This DLL is intended to stay loaded for the process lifetime. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD notification, LPVOID reservedContext)
{
    (void)reservedContext;
    if (notification == DLL_PROCESS_ATTACH) {
        char logPath[MAX_PATH], *filename;
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(instance);
        GetModuleFileNameA(instance, logPath, MAX_PATH);
        filename = strrchr(logPath, '\\');
        if (filename) strcpy(filename + 1, "imperialism_gdi_log.txt");
        g_log = fopen(logPath, "w");
        SetProcessDPIAware();  /* physical pixels: we do the scaling, not Windows */
        init_hook_table();
        logf_("shim loaded; patched %d imports", patch_iat());
        {
            char iniPath[MAX_PATH], *iniFilename;
            strcpy(iniPath, logPath);
            iniFilename = strrchr(iniPath, '\\');
            if (iniFilename) strcpy(iniFilename + 1, "imperialism_shim.ini");
            g_intScale = GetPrivateProfileIntA("display", "fullscreen_integer_scale", 1, iniPath) != 0;
            g_blockModeChange = GetPrivateProfileIntA("display", "block_mode_change", 1, iniPath) != 0;
            g_brownTitle = GetPrivateProfileIntA("display", "brown_titlebar", 1, iniPath) != 0;
            g_originalBackdrop = GetPrivateProfileIntA("display", "original_backdrop", 1, iniPath) != 0;
            g_textLog = GetPrivateProfileIntA("text_log", "enabled", 0, iniPath) != 0;
            if (g_textLog) {
                char textPath[MAX_PATH], *textFilename;
                strcpy(textPath, logPath);
                textFilename = strrchr(textPath, '\\');
                if (textFilename) strcpy(textFilename + 1, "imperialism_text.log");
                g_text = fopen(textPath, "w");
            }
        }
        CreateThread(NULL, 0, summary_thread, NULL, 0, NULL);
    } else if (notification == DLL_PROCESS_DETACH && g_log) {
        dump_counts();
    }
    return TRUE;
}

/* ---- msvfw32 forwarding: the only export the game imports ---- */
/* Forward the game's sole required MSVFW32 export to the Windows system DLL by an
 * explicit system-directory path, avoiding this proxy's filename. Preserve __cdecl
 * and the export ABI. Cache the module/function for process lifetime; return NULL if
 * loading or resolving fails. This is independent of the separate WINMM music proxy. */
HWND __cdecl MCIWndCreateA(HWND parent, HINSTANCE instance, DWORD style, LPCSTR filename)
{
    static HWND (__cdecl *systemCreate)(HWND, HINSTANCE, DWORD, LPCSTR);
    if (!systemCreate) {
        char systemPath[MAX_PATH];
        HMODULE systemModule;
        GetSystemDirectoryA(systemPath, MAX_PATH);
        strcat(systemPath, "\\msvfw32.dll");
        systemModule = LoadLibraryA(systemPath);
        if (systemModule) systemCreate = (void *)GetProcAddress(systemModule, "MCIWndCreateA");
    }
    return systemCreate ? systemCreate(parent, instance, style, filename) : NULL;
}
