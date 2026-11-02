/* Presentation: keep the game at its original size and show a larger live copy.
 * This module owns the two window handles, crop/fit rectangles and window events.
 * It does not depend on MFC: Afx/Ambit are names used by the game, not our types. */
#include "shim_internal.h"
#include "hook_support.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <string.h>
#include "backdrop.h"

/* Two actual Windows windows participate:
 *   game_frame: the original small window. The game still draws/handles input here.
 *   mirror_window: our larger window, showing a live scaled copy of the first.
 *
 * DWM (Desktop Window Manager) is Windows' desktop compositor. Its "thumbnail"
 * service can display a live view of one window inside another. We choose the
 * source crop and destination rectangle; we do not rebuild the game's renderer.
 *
 * "Owned window" is a Windows relationship, not a C++ object relationship: the
 * mirror stays above its owner and has no separate taskbar button. It is not a
 * child control. Both windows are serviced by the game's existing UI thread.
 * Only the diagnostic worker reads these caches from another thread, without a
 * snapshot lock; that output is approximate and must never control behavior. */
static HWND game_frame; /* Original game frame; owns the presentation window. */
static HWND mirror_window; /* Owned top-level presentation window, not a child. */
static HTHUMBNAIL thumbnail; /* DWM registration: source frame, destination mirror. */
static RECT destination_rect; /* thumbnail rectangle in mirror client coords */
static BOOL is_menu_open; /* Defer focus handoff while the mirror menu is active. */
static BOOL is_fullscreen; /* Mirror uses borderless monitor presentation. */
static BOOL has_shown_mirror; /* One-time thumbnail/show setup has been requested. */
static BOOL has_initial_layout; /* Initial geometry and normal frame size are recorded. */
static BOOL use_integer_scale = TRUE; /* [display] fullscreen_integer_scale in the ini */
/* Startup settings: read once, before the game starts creating its windows. */
static BOOL use_brown_titlebar = TRUE;
static BOOL block_display_mode_change = TRUE;
static BOOL use_original_backdrop = TRUE;
static BOOL use_test_hook; /* [test_hook] enabled=1: honor WM_TEST_CLICK (see shim_internal.h) */
static Backdrop backdrop; /* Optional stock bitmap/pattern, owned by the UI thread. */
static HMENU game_menu; /* Original frame menu handle reused by the mirror. */
static LONG windowed_style; /* Windowed mirror style before fullscreen. */
static RECT windowed_rect; /* Windowed mirror outer rectangle in screen pixels. */
static HHOOK message_hook; /* Frame UI thread's WH_GETMESSAGE hook. */
static WNDPROC original_frame_proc; /* Original frame procedure for subclass chaining. */
static int frame_width, frame_height; /* the frame's normal outer size, recorded at layout */

/* RECT uses left/top/right/bottom edges, not x/y/width/height. Right and bottom
 * are excluded: width is right-left. Both rectangles use pixels inside a window's
 * drawing area ("client" pixels), with (0,0) below its menu/title bar.
 * source_rect belongs to the original frame; destination_rect belongs to the mirror. */
static RECT source_rect;

enum { MESSAGE_RETURN_FOCUS_TO_GAME = WM_APP + 1 };
#define KEY_WAS_ALREADY_DOWN (1UL << 30)
#define SYSTEM_COMMAND_MASK 0xFFF0

/* Refresh the visible source crop in frame-client pixels. Prefer the AmbitGameWindow
 * child intersected with its frame; use the whole frame client before that view exists.
 * Called on the UI thread; returns whether the cached crop changed. */
static BOOL refresh_src(void)
{
    RECT viewRect, frameClientRect, previousSourceRect = source_rect;
    HWND gameView = FindWindowExA(game_frame, NULL, "AmbitGameWindow", NULL);
    GetClientRect(game_frame, &frameClientRect);
    if (gameView && GetWindowRect(gameView, &viewRect)) {
        /* GetWindowRect gives desktop coordinates. Translate both corners into
         * frame coordinates, then keep only the part the frame can actually show.
         * NULL in MapWindowPoints means the desktop coordinate system. */
        MapWindowPoints(NULL, game_frame, (POINT *)&viewRect, 2);
        IntersectRect(&viewRect, &viewRect, &frameClientRect);
    } else {
        viewRect = frameClientRect;
    }
    source_rect = viewRect;
    return memcmp(&previousSourceRect, &source_rect, sizeof previousSourceRect) != 0;
}

/* Fit the cached source crop into the mirror client, preserving aspect ratio. Fullscreen
 * optionally rounds scales >= 1 down to an integer; unused space is backdrop. The crop
 * and destination are DWM client-pixel rectangles, not desktop coordinates. */
static void mirror_update(void)
{
    RECT mirrorClientRect, sourceSize;
    double scale, heightScale;
    int scaledWidth, scaledHeight;
    if (!mirror_window || !game_frame)
        return;
    refresh_src();
    GetClientRect(mirror_window, &mirrorClientRect);
    sourceSize.right = source_rect.right - source_rect.left;
    sourceSize.bottom = source_rect.bottom - source_rect.top;
    if (mirrorClientRect.right <= 0 || mirrorClientRect.bottom <= 0 || sourceSize.right <= 0 ||
        sourceSize.bottom <= 0)
        return;
    scale = (double)mirrorClientRect.right / sourceSize.right;
    heightScale = (double)mirrorClientRect.bottom / sourceSize.bottom;
    if (heightScale < scale)
        scale = heightScale;
    /* Pick the smaller width/height scale so the whole picture fits. Fullscreen
     * can round it down to 1x, 2x, ...; centering leaves the unused brown borders. */
    if (is_fullscreen && use_integer_scale && scale >= 1.0)
        scale = (int)scale; /* whole-number scale: sharp, brown border like the stock look */
    scaledWidth = (int)(sourceSize.right * scale);
    scaledHeight = (int)(sourceSize.bottom * scale);
    destination_rect.left = (mirrorClientRect.right - scaledWidth) / 2;
    destination_rect.top = (mirrorClientRect.bottom - scaledHeight) / 2;
    destination_rect.right = destination_rect.left + scaledWidth;
    destination_rect.bottom = destination_rect.top + scaledHeight;
    if (thumbnail) {
        /* DWM wants flags saying which fields we changed. SOURCE is the crop
         * in the original frame; DESTINATION is where its scaled copy appears.
         * SOURCECLIENTAREAONLY excludes the original title bar and window border. */
        DWM_THUMBNAIL_PROPERTIES properties;
        memset(&properties, 0, sizeof properties);
        properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE |
                             DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = destination_rect;
        properties.rcSource = source_rect;
        properties.fVisible = TRUE;
        properties.opacity = 255;
        properties.fSourceClientAreaOnly = TRUE;
        DwmUpdateThumbnailProperties(thumbnail, &properties);
    }
}

/* Park the real frame's outer top-left at the mirror's client origin in screen pixels.
 * Ownership keeps the mirror above the frame; moving the underlying frame keeps the
 * game's real child windows and virtual cursor near the displayed image. */
static void mirror_place_frame(void)
{
    POINT mirrorClientOrigin = {0, 0};
    if (!mirror_window || !game_frame)
        return;
    ClientToScreen(
        mirror_window,
        &mirrorClientOrigin); /* client origin: the window rect includes invisible resize borders */
    /* Keep the original out of sight beneath the mirror. The SWP flags mean:
     * leave its size, stacking order and keyboard activation unchanged. */
    SetWindowPos(game_frame,
                 NULL,
                 mirrorClientOrigin.x,
                 mirrorClientOrigin.y,
                 0,
                 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Toggle the mirror between its saved window rectangle/style/menu and a borderless
 * popup covering its nearest monitor. The game frame and desktop display mode are not
 * resized here. Refit after flipping the flag because SetWindowPos dispatches WM_SIZE. */
static void mirror_toggle_fullscreen(void)
{
    if (!mirror_window)
        return;
    if (!is_fullscreen) {
        /* A window "style" is a set of flags describing borders/buttons, not a
         * visual theme. Save the windowed flags/rectangle/menu before removing
         * them and expanding the mirror to its monitor. No display-mode change. */
        MONITORINFO monitorInfo;
        monitorInfo.cbSize = sizeof monitorInfo;
        windowed_style = GetWindowLongA(mirror_window, GWL_STYLE);
        GetWindowRect(mirror_window, &windowed_rect);
        GetMonitorInfoA(MonitorFromWindow(mirror_window, MONITOR_DEFAULTTONEAREST), &monitorInfo);
        SetMenu(mirror_window, NULL);
        SetWindowLongA(mirror_window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(mirror_window,
                     HWND_TOP,
                     monitorInfo.rcMonitor.left,
                     monitorInfo.rcMonitor.top,
                     monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                     monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        SetWindowLongA(mirror_window, GWL_STYLE, windowed_style);
        SetMenu(mirror_window, game_menu);
        SetWindowPos(mirror_window,
                     NULL,
                     windowed_rect.left,
                     windowed_rect.top,
                     windowed_rect.right - windowed_rect.left,
                     windowed_rect.bottom - windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    is_fullscreen = !is_fullscreen;
    mirror_update(); /* the size message above ran before the flag flipped */
    InvalidateRect(mirror_window, NULL, TRUE);
    diagnostics_log("fullscreen=%d", is_fullscreen);
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
            /* Windows sets bit 30 for repeat key-downs. Toggle only on the first
             * press, then change the event to WM_NULL (an intentional no-op) so
             * the game does not also act on the shortcut. */
            if (!(message->lParam & KEY_WAS_ALREADY_DOWN))
                mirror_toggle_fullscreen(); /* ignore auto-repeat */
            message->message = WM_NULL;
        }
    }
    return CallNextHookEx(NULL, hookCode, removalMode, messagePointer);
}

/* Map a packed mirror-client mouse position to a frame-client point using the DWM
 * source/destination rectangles. Return image containment separately: points outside
 * remain extrapolated so an active capture/drag can continue beyond the image. */
BOOL display_map_point(LPARAM mirrorPosition, POINT *framePoint)
{
    int destinationWidth = destination_rect.right - destination_rect.left,
        destinationHeight = destination_rect.bottom - destination_rect.top;
    int mirrorX = GET_X_LPARAM(mirrorPosition), mirrorY = GET_Y_LPARAM(mirrorPosition);
    if (refresh_src())
        mirror_update(); /* the game moved/resized its view */
    if (destinationWidth <= 0 || destinationHeight <= 0) {
        framePoint->x = framePoint->y = 0;
        return FALSE;
    }
    destinationWidth = destination_rect.right - destination_rect.left;
    destinationHeight = destination_rect.bottom - destination_rect.top;
    /* Invert the drawing transform: remove the border offset, divide out the
     * scale, then add the source crop's offset in the original frame. MulDiv
     * performs the multiply/divide with integer rounding, as in the original.
     * A point outside the picture is still mapped, so an ongoing drag can finish. */
    framePoint->x = source_rect.left + MulDiv(mirrorX - destination_rect.left,
                                              source_rect.right - source_rect.left,
                                              destinationWidth);
    framePoint->y = source_rect.top + MulDiv(mirrorY - destination_rect.top,
                                             source_rect.bottom - source_rect.top,
                                             destinationHeight);
    return mirrorX >= destination_rect.left && mirrorX < destination_rect.right &&
           mirrorY >= destination_rect.top && mirrorY < destination_rect.bottom;
}

/* The reverse of display_map_point: where in the mirror does a point of the original
 * frame appear? Used by the test hook so a harness can name spots in game pixels.
 * The point is aimed at the middle of its scaled pixel, so rounding in the forward
 * mapping cannot move it to a neighbour. */
static LPARAM game_point_to_mirror(int frameX, int frameY)
{
    int sourceWidth = source_rect.right - source_rect.left,
        sourceHeight = source_rect.bottom - source_rect.top;
    int destinationWidth = destination_rect.right - destination_rect.left,
        destinationHeight = destination_rect.bottom - destination_rect.top;
    int mirrorX, mirrorY;
    if (refresh_src())
        mirror_update();
    if (sourceWidth <= 0 || sourceHeight <= 0)
        return MAKELPARAM(0, 0);
    /* source_rect is the game's client area measured from the frame window's top-left
     * (the DWM thumbnail's source), so a game pixel is simply its distance from the
     * source rectangle's own top-left corner. */
    mirrorX = destination_rect.left + MulDiv(2 * frameX + 1, destinationWidth, 2 * sourceWidth);
    mirrorY = destination_rect.top + MulDiv(2 * frameY + 1, destinationHeight, 2 * sourceHeight);
    return MAKELPARAM(mirrorX, mirrorY);
}

/* Windows calls this function whenever something happens to our window. The
 * message number (WM_*) identifies the event; wParam/lParam carry event-specific
 * data. A return here means we handled that event. Unhandled events go to
 * DefWindowProcA, Windows' default handler, so normal window behavior still works.
 *
 * Window procedure for the owned presentation window. Manage layout/background,
 * mirror-to-game mouse/menu routing, focus handoff, fullscreen restore/minimize, and
 * DWM thumbnail teardown. No independent render loop or keyboard emulation runs here. */
static LRESULT CALLBACK mirror_window_proc(HWND mirrorWindow,
                                           UINT message,
                                           WPARAM wParam,
                                           LPARAM lParam)
{
    switch (message) {
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED: {
        static LONG positionLogCount;
        WINDOWPOS *windowPosition = (WINDOWPOS *)lParam;
        if (InterlockedIncrement(&positionLogCount) <= 60)
            diagnostics_log("mirror %s pos=(%d,%d) size=%dx%d flags=%x",
                            message == WM_WINDOWPOSCHANGING ? "poschanging" : "poschanged",
                            windowPosition->x,
                            windowPosition->y,
                            windowPosition->cx,
                            windowPosition->cy,
                            windowPosition->flags);
        break;
    }
    case WM_SHOWWINDOW:
        diagnostics_log("mirror WM_SHOWWINDOW show=%d reason=%d", (int)wParam, (int)lParam);
        if (wParam && has_initial_layout) {
            mirror_place_frame();
            mirror_update();
        } /* re-park after a restore */
        break;
    case WM_SYSCOMMAND:
        /* The mirror is owned by the frame and has no taskbar button, so minimize the
         * frame (which hides its owned mirror and leaves one taskbar button to restore). */
        if ((wParam & SYSTEM_COMMAND_MASK) == SC_MINIMIZE && game_frame) {
            ShowWindow(game_frame, SW_MINIMIZE);
            return 0;
        }
        break;
    case WM_ERASEBKGND: {
        /* Windows supplies a drawing surface (HDC) in wParam. Paint the space
         * behind the live DWM picture; this does not repaint the game itself. */
        RECT clientRect;
        GetClientRect(mirrorWindow, &clientRect);
        backdrop_paint(&backdrop, (HDC)wParam, &clientRect);
        return 1;
    }
    case WM_SIZE:
        /* The user resized/maximized us: refit the live view, then ask Windows
         * to repaint the newly exposed border. InvalidateRect schedules a paint. */
        mirror_update();
        InvalidateRect(mirrorWindow, NULL, TRUE);
        break;
    case WM_MOVE:
        mirror_place_frame();
        break;
    case WM_GETMINMAXINFO: {
        /* lParam points to Windows' proposed size limits. Set a minimum large
         * enough to cover the original window hidden underneath this one. */
        RECT clientRect;
        MINMAXINFO *limits = (MINMAXINFO *)lParam;
        DefWindowProcA(mirrorWindow, message, wParam, lParam); /* defaults first, then override */
        /* Client area must never be smaller than the frame it hides (else the frame pokes out
         * from under the mirror). Uses the frame's recorded normal size, not its current one,
         * because the game minimizes/maximizes it around shutdown. */
        if (has_initial_layout && frame_width > 0) {
            clientRect.left = clientRect.top = 0;
            clientRect.right = frame_width;
            clientRect.bottom = frame_height;
            AdjustWindowRect(&clientRect,
                             GetWindowLongA(mirrorWindow, GWL_STYLE),
                             GetMenu(mirrorWindow) != NULL);
            limits->ptMinTrackSize.x = clientRect.right - clientRect.left;
            limits->ptMinTrackSize.y = clientRect.bottom - clientRect.top;
        }
        return 0;
    }
    case WM_SETFOCUS:
        /* Keyboard focus is the window that receives typing. Our larger copy
         * should not consume the game's keys. Queue a private message so the
         * focus change finishes first; do not disturb an open menu or mouse drag. */
        if (!is_menu_open)
            PostMessageA(mirrorWindow, MESSAGE_RETURN_FOCUS_TO_GAME, 0, 0);
        break;
    case MESSAGE_RETURN_FOCUS_TO_GAME:
        if (!is_menu_open && game_frame && !input_has_mirror_capture() &&
            GetFocus() == mirrorWindow)
            SetFocus(game_frame);
        return 0;
    case WM_ENTERMENULOOP:
        is_menu_open = TRUE;
        break;
    case WM_EXITMENULOOP:
        is_menu_open = FALSE;
        PostMessageA(mirrorWindow, MESSAGE_RETURN_FOCUS_TO_GAME, 0, 0);
        break;
    case WM_INITMENU:
        /* The mirror displays the game's menu handle, but the game still owns
         * menu decisions. Forward preparation/selection events to its frame. */
    case WM_INITMENUPOPUP:
    case WM_MENUSELECT:
        if (game_frame)
            SendMessageA(game_frame, message, wParam, lParam);
        break;
    case WM_COMMAND:
        if (game_frame)
            SendMessageA(game_frame, message, wParam, lParam);
        return 0;
    case WM_CLOSE:
        if (game_frame)
            SendMessageA(game_frame, WM_CLOSE, 0, 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
            return input_relay_cursor(mirrorWindow, lParam);
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        return input_relay_mouse(mirrorWindow, message, wParam, lParam);
    case WM_TEST_CLICK:
        /* Test hook: replay a real-looking left click at the game pixel in lParam. Move,
         * press and release are separate events, as from a mouse, with a short pause so
         * the game sees the hover before the press. A click that opens a modal pop-up
         * keeps running inside the release call until that pop-up closes. */
        if (use_test_hook) {
            LPARAM at = game_point_to_mirror(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            POINT back;
            BOOL inside = display_map_point(at, &back);
            diagnostics_log("test click game=(%d,%d) mirror=(%d,%d) back=(%ld,%ld) inside=%d "
                            "dest=(%ld,%ld,%ld,%ld) src=(%ld,%ld,%ld,%ld)",
                            GET_X_LPARAM(lParam),
                            GET_Y_LPARAM(lParam),
                            GET_X_LPARAM(at),
                            GET_Y_LPARAM(at),
                            back.x,
                            back.y,
                            (int)inside,
                            destination_rect.left,
                            destination_rect.top,
                            destination_rect.right,
                            destination_rect.bottom,
                            source_rect.left,
                            source_rect.top,
                            source_rect.right,
                            source_rect.bottom);
            input_relay_mouse(mirrorWindow, WM_MOUSEMOVE, 0, at);
            Sleep(60);
            input_relay_mouse(mirrorWindow, WM_LBUTTONDOWN, MK_LBUTTON, at);
            Sleep(60);
            input_relay_mouse(mirrorWindow, WM_LBUTTONUP, 0, at);
        }
        return 0;
    case WM_MOUSELEAVE:
        input_mouse_leave();
        return 0;
    case WM_DESTROY:
        backdrop_free(&backdrop);
        if (thumbnail) {
            DwmUnregisterThumbnail(thumbnail);
            thumbnail = NULL;
        }
        break;
    }
    return DefWindowProcA(mirrorWindow, message, wParam, lParam);
}

/* "Subclass" here means wrap an existing window callback; it does not mean C++
 * inheritance. original_frame_proc is the game's old callback. Call it first so
 * the game keeps its normal behavior, then adjust just its allowed outer size.
 *
 * Subclass the original game frame. Let its handler run first, then cap maximize/
 * tracking dimensions at the recorded normal outer size so the frame stays hidden
 * beneath the mirror. This does not replace the game's drawing or event handler. */
static LRESULT CALLBACK frame_window_proc(HWND frameWindow,
                                          UINT message,
                                          WPARAM wParam,
                                          LPARAM lParam)
{
    static LONG messageLogCount;
    LRESULT originalResult;
    if ((message == WM_ACTIVATEAPP || message == WM_ACTIVATE || message == WM_SYSCOMMAND ||
         message == WM_SIZE || message == WM_SHOWWINDOW || message == WM_CLOSE ||
         message == WM_KILLFOCUS || message == WM_SETFOCUS || message == WM_NCACTIVATE) &&
        InterlockedIncrement(&messageLogCount) <= 200)
        diagnostics_log("frame msg 0x%04x w=%lx l=%lx fg=%p active=%p",
                        message,
                        (unsigned long)wParam,
                        (unsigned long)lParam,
                        GetForegroundWindow(),
                        GetActiveWindow());
    originalResult = CallWindowProcA(original_frame_proc, frameWindow, message, wParam, lParam);
    if (message == WM_GETMINMAXINFO && frame_width > 0) {
        MINMAXINFO *limits = (MINMAXINFO *)lParam;
        limits->ptMaxSize.x = limits->ptMaxTrackSize.x = frame_width;
        limits->ptMaxSize.y = limits->ptMaxTrackSize.y = frame_height;
    }
    return originalResult;
}

/* Create the hidden owned mirror as soon as the first matching top-level game frame
 * exists. Register its class, optionally tint supported DWM caption attributes, subclass
 * the frame, and install a message hook on this same UI thread. Layout/show are deferred. */
static void mirror_init(HWND frameWindow)
{
    WNDCLASSEXA windowClass;
    game_frame = frameWindow;
    /* Read the running game's own bitmap outside DllMain/loader lock. Nothing is
     * bundled with the shim; missing artwork only removes the optional decoration. */
    if (use_original_backdrop)
        diagnostics_log("stock backdrop resource %d: %s",
                        BACKDROP_RESOURCE_ID,
                        backdrop_load(&backdrop, GetModuleHandleW(NULL))
                            ? "loaded"
                            : "unavailable; using brown");
    memset(&windowClass, 0, sizeof windowClass);
    windowClass.cbSize = sizeof windowClass;
    /* A window class tells Windows which callback, cursor and icon to use when
     * creating a window of that kind. This is a Win32 registration record, not
     * a C++ class. Our actual window is created from this record just below. */
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = mirror_window_proc;
    windowClass.hInstance = GetModuleHandleA(NULL);
    windowClass.hCursor = LoadCursorA(NULL, IDC_ARROW);
    windowClass.hIcon = (HICON)GetClassLongA(frameWindow, GCL_HICON);
    windowClass.hbrBackground = CreateSolidBrush(BACKDROP_BROWN);
    windowClass.lpszClassName = "ImperialismMirror";
    RegisterClassExA(&windowClass);
    /* Passing frameWindow as the owner keeps the preview above the real frame.
     * WS_OVERLAPPEDWINDOW asks for normal resize borders/buttons. We leave it
     * hidden until the game's startup has settled on a usable source size. */
    mirror_window = CreateWindowExA(0,
                                    windowClass.lpszClassName,
                                    "Imperialism",
                                    WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT,
                                    0,
                                    656,
                                    539,
                                    frameWindow,
                                    NULL,
                                    windowClass.hInstance,
                                    NULL);
    if (use_brown_titlebar) {
        /* DWMWA_CAPTION_COLOR 35, DWMWA_TEXT_COLOR 36, DWMWA_BORDER_COLOR 34,
         * DWMWA_USE_IMMERSIVE_DARK_MODE 20: only honoured on Windows 11 build 22000+; older systems
         * just return an error and keep the normal bar. */
        COLORREF captionColor = BACKDROP_BROWN, captionTextColor = RGB(236, 214, 160);
        BOOL darkCaption = TRUE;
        DwmSetWindowAttribute(mirror_window,
                              20,
                              &darkCaption,
                              sizeof darkCaption); /* light caption buttons on the dark bar */
        DwmSetWindowAttribute(mirror_window, 35, &captionColor, sizeof captionColor);
        DwmSetWindowAttribute(mirror_window, 34, &captionColor, sizeof captionColor);
        DwmSetWindowAttribute(mirror_window, 36, &captionTextColor, sizeof captionTextColor);
    }
    original_frame_proc =
        (WNDPROC)SetWindowLongA(frameWindow, GWL_WNDPROC, (LONG)frame_window_proc);
    message_hook = SetWindowsHookExA(WH_GETMESSAGE, msg_hook, NULL, GetCurrentThreadId());
    diagnostics_log(
        "mirror created %p (frame %p) hook=%p", mirror_window, frameWindow, message_hook);
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
    if (!mirror_window || has_initial_layout)
        return;
    refresh_src();
    sourceSize.left = sourceSize.top = 0;
    sourceSize.right =
        source_rect.right - source_rect.left; /* the game view, not the whole frame */
    sourceSize.bottom = source_rect.bottom - source_rect.top;
    if (sourceSize.right < 320 || sourceSize.bottom < 200)
        return;
    has_initial_layout = TRUE;
    game_menu = GetMenu(game_frame);
    if (game_menu)
        SetMenu(mirror_window, game_menu);
    GetWindowRect(game_frame, &frameWindowRect);
    frame_width = frameWindowRect.right - frameWindowRect.left;
    frame_height = frameWindowRect.bottom - frameWindowRect.top;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
    mirrorStyle = GetWindowLongA(mirror_window, GWL_STYLE);
    for (integerScale = 6; integerScale >= 1;
         integerScale--) { /* largest integer scale that fits the work area */
        mirrorWindowRect.left = mirrorWindowRect.top = 0;
        mirrorWindowRect.right = sourceSize.right * integerScale;
        mirrorWindowRect.bottom = sourceSize.bottom * integerScale;
        AdjustWindowRect(&mirrorWindowRect, mirrorStyle, game_menu != NULL);
        if (mirrorWindowRect.right - mirrorWindowRect.left <= workArea.right - workArea.left &&
            mirrorWindowRect.bottom - mirrorWindowRect.top <= workArea.bottom - workArea.top)
            break;
    }
    if (integerScale < 1)
        integerScale = 1;
    mirrorWindowRect.left = mirrorWindowRect.top = 0;
    mirrorWindowRect.right = sourceSize.right * integerScale;
    mirrorWindowRect.bottom = sourceSize.bottom * integerScale;
    AdjustWindowRect(&mirrorWindowRect, mirrorStyle, game_menu != NULL);
    windowX =
        workArea.left +
        ((workArea.right - workArea.left) - (mirrorWindowRect.right - mirrorWindowRect.left)) / 2;
    windowY =
        workArea.top +
        ((workArea.bottom - workArea.top) - (mirrorWindowRect.bottom - mirrorWindowRect.top)) / 2;
    SetWindowPos(mirror_window,
                 NULL,
                 windowX,
                 windowY,
                 mirrorWindowRect.right - mirrorWindowRect.left,
                 mirrorWindowRect.bottom - mirrorWindowRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    mirror_place_frame();
    mirror_update();
    if (has_shown_mirror)
        ShowWindow(mirror_window, SW_SHOW);
    diagnostics_log("mirror laid out: frame window %ldx%ld client %ldx%ld menu=%p scale=%d",
                    frameWindowRect.right - frameWindowRect.left,
                    frameWindowRect.bottom - frameWindowRect.top,
                    sourceSize.right,
                    sourceSize.bottom,
                    game_menu,
                    integerScale);
}

/* One-time presentation activation after a game ShowWindow call. Register the DWM
 * thumbnail and show only after valid layout; later layout can complete the reveal.
 * A failed DWM registration is logged but has no software-rendering fallback. */
static void mirror_show(void)
{
    if (!mirror_window || has_shown_mirror)
        return;
    has_shown_mirror = TRUE;
    if (DwmRegisterThumbnail(mirror_window, game_frame, &thumbnail) != S_OK)
        thumbnail = NULL;
    diagnostics_log("mirror shown: thumb=%p", thumbnail);
    if (has_initial_layout)
        ShowWindow(mirror_window, SW_SHOW);
    else
        mirror_layout();
}

DECLARE_HOOK("USER32.dll", ChangeDisplaySettingsA);
DECLARE_HOOK("USER32.dll", CreateWindowExA);
DECLARE_HOOK("USER32.dll", SetWindowPos);
DECLARE_HOOK("USER32.dll", ShowWindow);
DECLARE_HOOK("USER32.dll", MoveWindow);
DECLARE_HOOK("USER32.dll", SetWindowPlacement);

static LONG WINAPI hooked_ChangeDisplaySettingsA(DEVMODEA *displayMode, DWORD flags)
{
    typedef LONG(WINAPI * OriginalFunction)(DEVMODEA *, DWORD);
    OriginalFunction original = (OriginalFunction)hook_ChangeDisplaySettingsA.original;

    LONG changeResult;
    if (block_display_mode_change) {
        /* The game asks for 640x480 (or restores the old mode on exit). We scale in a window
         * instead, so pretend it worked and leave the desktop alone. */
        LOG_HOOK(ChangeDisplaySettingsA,
                 "%lux%lu flags=%lx -> blocked",
                 displayMode ? displayMode->dmPelsWidth : 0,
                 displayMode ? displayMode->dmPelsHeight : 0,
                 flags);
        return DISP_CHANGE_SUCCESSFUL;
    }
    changeResult = original(displayMode, flags);
    LOG_HOOK(ChangeDisplaySettingsA,
             "%lux%lu bpp=%lu flags=%lx -> %ld",
             displayMode ? displayMode->dmPelsWidth : 0,
             displayMode ? displayMode->dmPelsHeight : 0,
             displayMode ? displayMode->dmBitsPerPel : 0,
             flags,
             changeResult);
    return changeResult;
}

static HWND WINAPI hooked_CreateWindowExA(DWORD extendedStyle,
                                          LPCSTR className,
                                          LPCSTR windowTitle,
                                          DWORD style,
                                          int windowX,
                                          int windowY,
                                          int width,
                                          int height,
                                          HWND parent,
                                          HMENU menu,
                                          HINSTANCE instance,
                                          LPVOID creationData)
{
    typedef HWND(WINAPI * OriginalFunction)(
        DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
    OriginalFunction original = (OriginalFunction)hook_CreateWindowExA.original;

    HWND createdWindow = original(extendedStyle,
                                  className,
                                  windowTitle,
                                  style,
                                  windowX,
                                  windowY,
                                  width,
                                  height,
                                  parent,
                                  menu,
                                  instance,
                                  creationData);
    if (createdWindow && !parent && HIWORD(className) != 0 && windowTitle &&
        !strcmp(windowTitle, "Imperialism") && !game_frame)
        mirror_init(createdWindow);
    if (HIWORD(className) == 0)
        LOG_HOOK(CreateWindowExA,
                 "atom=%u style=%08lx ex=%08lx (%d,%d %dx%d) parent=%p -> %p",
                 (unsigned)LOWORD(className),
                 style,
                 extendedStyle,
                 windowX,
                 windowY,
                 width,
                 height,
                 parent,
                 createdWindow);
    else
        LOG_HOOK(CreateWindowExA,
                 "class=\"%s\" title=\"%s\" style=%08lx ex=%08lx (%d,%d %dx%d) parent=%p -> %p",
                 className,
                 windowTitle ? windowTitle : "",
                 style,
                 extendedStyle,
                 windowX,
                 windowY,
                 width,
                 height,
                 parent,
                 createdWindow);
    return createdWindow;
}

static BOOL WINAPI hooked_SetWindowPos(
    HWND window, HWND insertAfter, int windowX, int windowY, int width, int height, UINT flags)
{
    typedef BOOL(WINAPI * OriginalFunction)(HWND, HWND, int, int, int, int, UINT);
    OriginalFunction original = (OriginalFunction)hook_SetWindowPos.original;

    BOOL positioned;
    LOG_HOOK(SetWindowPos,
             "wnd=%p (%d,%d %dx%d) flags=%x",
             window,
             windowX,
             windowY,
             width,
             height,
             flags);
    if (window == game_frame && mirror_window &&
        !(flags & SWP_NOMOVE)) { /* keep the frame parked under the mirror */
        POINT mirrorClientOrigin = {0, 0};
        ClientToScreen(mirror_window, &mirrorClientOrigin);
        windowX = mirrorClientOrigin.x;
        windowY = mirrorClientOrigin.y;
    }
    positioned = original(window, insertAfter, windowX, windowY, width, height, flags);
    if (window == game_frame && mirror_window) { /* frame size may have changed */
        if (has_shown_mirror && !has_initial_layout)
            mirror_layout();
        mirror_update();
    }
    return positioned;
}

static BOOL WINAPI hooked_ShowWindow(HWND window, int showCommand)
{
    typedef BOOL(WINAPI * OriginalFunction)(HWND, int);
    OriginalFunction original = (OriginalFunction)hook_ShowWindow.original;

    BOOL previousVisibility;
    LOG_HOOK(ShowWindow, "wnd=%p cmd=%d", window, showCommand);
    if (mirror_window && window == game_frame &&
        (showCommand == SW_MINIMIZE || showCommand == SW_SHOWMINIMIZED ||
         showCommand == SW_SHOWMINNOACTIVE || showCommand == SW_FORCEMINIMIZE)) {
        diagnostics_log("frame self-minimize via ShowWindow(%d) blocked", showCommand);
        return TRUE;
    }
    previousVisibility = original(window, showCommand);
    if (window == game_frame && mirror_window) {
        if (showCommand == SW_HIDE)
            ShowWindow(mirror_window, SW_HIDE);
        else
            mirror_show();
    }
    return previousVisibility;
}

static BOOL WINAPI
hooked_MoveWindow(HWND window, int windowX, int windowY, int width, int height, BOOL repaint)
{
    typedef BOOL(WINAPI * OriginalFunction)(HWND, int, int, int, int, BOOL);
    OriginalFunction original = (OriginalFunction)hook_MoveWindow.original;

    LONG callCount = InterlockedIncrement(&hook_MoveWindow.call_count);
    if (callCount <= DIAGNOSTIC_LOG_LIMIT)
        diagnostics_log("MoveWindow wnd=%p (%s) (%d,%d %dx%d) caller=%p",
                        window,
                        window == mirror_window ? "MIRROR"
                        : window == game_frame  ? "frame"
                                                : "other",
                        windowX,
                        windowY,
                        width,
                        height,
                        __builtin_return_address(0));
    if (mirror_window && window == mirror_window)
        return TRUE; /* swallow */
    return original(window, windowX, windowY, width, height, repaint);
}

static BOOL WINAPI hooked_SetWindowPlacement(HWND window, const WINDOWPLACEMENT *placement)
{
    typedef BOOL(WINAPI * OriginalFunction)(HWND, const WINDOWPLACEMENT *);
    OriginalFunction original = (OriginalFunction)hook_SetWindowPlacement.original;

    WINDOWPLACEMENT normalPlacement;
    if (window == game_frame && placement)
        diagnostics_log(
            "SetWindowPlacement frame showCmd=%u flags=%x", placement->showCmd, placement->flags);
    if (mirror_window && window == game_frame && placement &&
        placement->showCmd == SW_SHOWMAXIMIZED) {
        normalPlacement = *placement;
        normalPlacement.showCmd = SW_SHOWNORMAL;
        placement = &normalPlacement;
        diagnostics_log("frame maximize suppressed");
    } else if (mirror_window && window == game_frame && placement &&
               (placement->showCmd == SW_SHOWMINIMIZED || placement->showCmd == SW_MINIMIZE ||
                placement->showCmd == SW_SHOWMINNOACTIVE)) {
        diagnostics_log("frame self-minimize via SetWindowPlacement blocked");
        return TRUE; /* the game minimizes itself when it loses focus; a windowed game shouldn't */
    }
    return original(window, placement);
}

HWND display_frame_window(void)
{
    return game_frame;
}

HWND display_mirror_window(void)
{
    return mirror_window;
}

void display_configure(const DisplayOptions *options)
{
    use_integer_scale = options->integer_scale;
    block_display_mode_change = options->block_mode_change;
    use_brown_titlebar = options->brown_titlebar;
    use_original_backdrop = options->original_backdrop;
    use_test_hook = options->test_hook;
}

void display_write_diagnostics(FILE *diagnostic_file)
{
    RECT mirrorWindowRect, mirrorClientRect, frameWindowRect, frameClientRect;
    SIZE thumbnailSourceSize = {0, 0};
    if (!mirror_window || !game_frame)
        return;
    GetWindowRect(mirror_window, &mirrorWindowRect);
    GetClientRect(mirror_window, &mirrorClientRect);
    GetWindowRect(game_frame, &frameWindowRect);
    GetClientRect(game_frame, &frameClientRect);
    if (thumbnail)
        DwmQueryThumbnailSourceSize(thumbnail, &thumbnailSourceSize);
    fprintf(
        diagnostic_file,
        "diag mirror win(%ld,%ld %ldx%ld) client %ldx%ld zoomed=%d | frame win(%ld,%ld %ldx%ld) "
        "client %ldx%ld zoomed=%d | thumb src %ldx%ld dest(%ld,%ld %ldx%ld) laidOut=%d\n",
        mirrorWindowRect.left,
        mirrorWindowRect.top,
        mirrorWindowRect.right - mirrorWindowRect.left,
        mirrorWindowRect.bottom - mirrorWindowRect.top,
        mirrorClientRect.right,
        mirrorClientRect.bottom,
        IsZoomed(mirror_window),
        frameWindowRect.left,
        frameWindowRect.top,
        frameWindowRect.right - frameWindowRect.left,
        frameWindowRect.bottom - frameWindowRect.top,
        frameClientRect.right,
        frameClientRect.bottom,
        IsZoomed(game_frame),
        thumbnailSourceSize.cx,
        thumbnailSourceSize.cy,
        destination_rect.left,
        destination_rect.top,
        destination_rect.right - destination_rect.left,
        destination_rect.bottom - destination_rect.top,
        has_initial_layout);
    fprintf(diagnostic_file,
            "diag view src(%ld,%ld %ldx%ld)\n",
            source_rect.left,
            source_rect.top,
            source_rect.right - source_rect.left,
            source_rect.bottom - source_rect.top);
}

/* Called once before the executable import table is changed. Each row keeps
 * the real Windows address separate from our replacement function. */
HookTable display_hook_table(void)
{
    static Hook *const entries[] = {&hook_ChangeDisplaySettingsA,
                                    &hook_CreateWindowExA,
                                    &hook_SetWindowPos,
                                    &hook_ShowWindow,
                                    &hook_MoveWindow,
                                    &hook_SetWindowPlacement};
    hook_ChangeDisplaySettingsA.replacement = hooked_ChangeDisplaySettingsA;
    hook_CreateWindowExA.replacement = hooked_CreateWindowExA;
    hook_SetWindowPos.replacement = hooked_SetWindowPos;
    hook_ShowWindow.replacement = hooked_ShowWindow;
    hook_MoveWindow.replacement = hooked_MoveWindow;
    hook_SetWindowPlacement.replacement = hooked_SetWindowPlacement;
    HookTable table = {entries, sizeof entries / sizeof entries[0]};
    return table;
}
