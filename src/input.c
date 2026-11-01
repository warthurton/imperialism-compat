/* The player points at the enlarged mirror, but the game still draws and handles
 * clicks in its original, smaller windows. This file translates between the two.
 *
 * A "client" point is measured from the inside top-left of one window, excluding
 * its border and title bar. A "screen" point is measured across the desktop.
 * The route is mirror-client -> game-frame-client -> chosen game-child-client.
 * Cursor-query functions need that same game point expressed in screen coordinates.
 *
 * All of this state belongs to the game's UI thread. Calls such as SetCapture below
 * reach real Windows: only the game's imported-function addresses are replaced,
 * not this DLL's own imports. Thus a replacement can call Windows without itself
 * being called again through the same hook. */
#include "shim_internal.h"
#include "hook_support.h"
#include <windowsx.h>

/* "Capture" means keep delivering mouse events to one window after the pointer
 * leaves it. Windows must send those events to the visible mirror, while the game
 * must believe its original button/slider owns them. Keep both identities. */
static HWND game_capture_target; /* Original game window promised the continuing mouse events. */
static HWND pressed_close_target; /* Child whose close button began the current press. */
static POINT virtual_cursor_position; /* Unscaled game point, expressed in desktop coordinates. */
static BOOL has_virtual_cursor; /* Cursor queries may use that translated game position. */
static BOOL has_mirror_capture; /* The shim asked Windows to capture for the mirror. */
static BOOL is_tracking_mouse; /* Windows has been asked to notify us when the mouse leaves. */
static BOOL is_forwarding_cursor; /* Log whether a cursor change came from our forwarded query. */
/* A manual child-caption drag stores its press point and initial outer top-left
 * in frame-client pixels. This assumes the draggable child's parent is the frame. */
static struct {
    HWND window;
    POINT startFramePoint, startFramePosition;
    BOOL active;
} child_drag;

/* Find which original game window should receive the event. The input point starts
 * relative to the game frame; on return it is relative to the chosen window's inside.
 * A captured slider keeps receiving the drag even when another control is underneath
 * the pointer. Without capture, walk down the visible child windows to the deepest
 * one containing the point. Only invisible children are excluded by this search. */
static HWND resolve_target(POINT *targetPoint)
{
    HWND game_frame = display_frame_window();
    HWND target = game_frame;
    if (game_capture_target && IsWindow(game_capture_target)) {
        MapWindowPoints(game_frame, game_capture_target, targetPoint, 1);
        return game_capture_target;
    }
    /* Each search expects coordinates relative to its current parent, not the
     * desktop or the outer frame. Translate again at every level before descending. */
    for (;;) {
        HWND child = ChildWindowFromPointEx(target, *targetPoint, CWP_SKIPINVISIBLE);
        if (!child || child == target)
            break;
        MapWindowPoints(target, child, targetPoint, 1);
        target = child;
    }
    return target;
}

/* A Windows mouse "message" names the event, such as left-button-up or movement.
 * Its buttonState argument separately describes buttons/modifier keys still held;
 * mirrorPosition packs the pointer's x/y coordinates relative to the mirror's inside.
 * Translate the point and deliver the event to the original game window. */
LRESULT
input_relay_mouse(HWND mirrorWindow, UINT message, WPARAM buttonState, LPARAM mirrorPosition)
{
    HWND game_frame = display_frame_window();
    POINT framePoint, frameScreenOrigin = {0, 0}, targetPoint;
    HWND target;
    BOOL insideImage = display_map_point(mirrorPosition, &framePoint);
    BOOL buttonDown = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                      message == WM_MBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
                      message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDBLCLK;
    BOOL buttonUp = message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP;

    /* Brown/artwork borders are not game controls. Ignore a fresh event there, but
     * keep an existing drag alive: discarding its outside release could leave a
     * slider or title-bar drag permanently pressed. The mapper deliberately gives
     * an extrapolated game point even when it reports "outside the image". */
    if (!insideImage && !game_capture_target && !has_mirror_capture && !child_drag.active) {
        has_virtual_cursor = FALSE;
        return 0;
    }
    if (!is_tracking_mouse) {
        TRACKMOUSEEVENT trackingRequest = {sizeof trackingRequest, TME_LEAVE, mirrorWindow, 0};
        TrackMouseEvent(&trackingRequest);
        is_tracking_mouse = TRUE;
    }
    /* Some game handlers ask Windows for the pointer instead of using the point
     * carried by the event. Prepare the translated answer BEFORE calling a handler:
     * SendMessage runs that handler immediately and it can call our query hooks or
     * change capture before SendMessage returns. The real frame's desktop origin
     * makes this answer compatible with the game's own screen-to-window conversion. */
    ClientToScreen(game_frame, &frameScreenOrigin);
    virtual_cursor_position.x = frameScreenOrigin.x + framePoint.x;
    virtual_cursor_position.y = frameScreenOrigin.y + framePoint.y;
    has_virtual_cursor = TRUE;

    /* A title-bar drag is handled here rather than asking Windows to drag the
     * hidden, unscaled window using the player's enlarged-screen distance. Apply
     * the displacement between two unscaled frame points to its starting position. */
    if (child_drag.active) {
        if (message == WM_MOUSEMOVE)
            SetWindowPos(
                child_drag.window,
                NULL,
                child_drag.startFramePosition.x + framePoint.x - child_drag.startFramePoint.x,
                child_drag.startFramePosition.y + framePoint.y - child_drag.startFramePoint.y,
                0,
                0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        else if (message == WM_LBUTTONUP) {
            child_drag.active = FALSE;
            ReleaseCapture();
            has_mirror_capture = FALSE;
        }
        return 0;
    }

    targetPoint = framePoint;
    target = resolve_target(&targetPoint);
    /* "Non-client" means the window's decorations: title bar, border and close
     * button, rather than the controls inside it. Ask the original child what lies
     * under the pointer; unlike ordinary mouse events, this question takes SCREEN
     * coordinates. A game control that already captured the mouse takes precedence. */
    if (!game_capture_target && target != game_frame) {
        LRESULT hitTest =
            SendMessageA(target,
                         WM_NCHITTEST,
                         0,
                         MAKELPARAM(virtual_cursor_position.x, virtual_cursor_position.y));
        if (hitTest != HTCLIENT && hitTest != HTNOWHERE && hitTest != HTTRANSPARENT) {
            if (message == WM_LBUTTONDOWN) {
                BringWindowToTop(target);
                if (hitTest == HTCAPTION) {
                    RECT childWindowRect;
                    GetWindowRect(target, &childWindowRect);
                    MapWindowPoints(NULL, game_frame, (POINT *)&childWindowRect, 1);
                    child_drag.window = target;
                    child_drag.active = TRUE;
                    child_drag.startFramePoint = framePoint;
                    child_drag.startFramePosition.x = childWindowRect.left;
                    child_drag.startFramePosition.y = childWindowRect.top;
                    SetCapture(mirrorWindow);
                    has_mirror_capture = TRUE;
                } else if (hitTest == HTCLOSE) {
                    /* Remember the press, but close only if the release also lands
                     * on this same close button. Moving away cancels the click. */
                    pressed_close_target = target;
                    SetCapture(mirrorWindow);
                    has_mirror_capture = TRUE;
                }
            } else if (message == WM_LBUTTONUP && pressed_close_target) {
                if (target == pressed_close_target && hitTest == HTCLOSE)
                    PostMessageA(target, WM_SYSCOMMAND, SC_CLOSE, 0);
                pressed_close_target = NULL;
                ReleaseCapture();
                has_mirror_capture = FALSE;
            }
            return 0;
        }
    }
    if (pressed_close_target && buttonUp) {
        pressed_close_target = NULL;
    }
    /* Keep receiving the matching release outside the mirror even when the game
     * handler never asks to capture. If that handler does ask, its hook records the
     * original target while leaving Windows' actual capture on the visible mirror. */
    if (buttonDown && !GetCapture()) {
        SetCapture(mirrorWindow);
        has_mirror_capture = TRUE;
    }
    SendMessageA(target, message, buttonState, MAKELPARAM(targetPoint.x, targetPoint.y));
    /* The handler above may have changed logical capture. Release only our spare
     * physical capture when no game target and no held mouse button still need it. */
    if (buttonUp && has_mirror_capture && !game_capture_target &&
        !(buttonState & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) {
        ReleaseCapture();
        has_mirror_capture = FALSE;
    }
    return 0;
}

/* Windows asks the visible mirror which pointer shape to show. Let the original
 * game control choose instead, using the real pointer to locate that control.
 * This is a shape-selection query, not a new movement event. */
LRESULT input_relay_cursor(HWND mirrorWindow, LPARAM lParam)
{
    if (LOWORD(lParam) == HTCLIENT) {
        POINT mirrorPoint, targetPoint;
        GetCursorPos(&mirrorPoint);
        ScreenToClient(mirrorWindow, &mirrorPoint);
        if (display_map_point(MAKELPARAM(mirrorPoint.x, mirrorPoint.y), &targetPoint) ||
            game_capture_target) {
            HWND target = resolve_target(&targetPoint);
            LRESULT cursorHandled;
            is_forwarding_cursor = TRUE;
            cursorHandled = SendMessageA(
                target, WM_SETCURSOR, (WPARAM)target, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            is_forwarding_cursor = FALSE;
            {
                static LONG cursorLogCount;
                if (InterlockedIncrement(&cursorLogCount) <= 40)
                    diagnostics_log("mirror WM_SETCURSOR -> target %p class=%s handled=%ld",
                                    target,
                                    "",
                                    (long)cursorHandled);
            }
            /* The game can set a pointer shape while returning "not handled".
             * Preserve that shape anyway; letting the mirror's default handler run
             * now could immediately replace it with the ordinary arrow. */
            (void)cursorHandled;
            return TRUE;
        }
        /* Outside the game image, the surrounding border behaves like an ordinary
         * window, so use the standard arrow rather than a leftover game cursor. */
        SetCursor(LoadCursorA(NULL, IDC_ARROW));
        return TRUE;
    }

    return FALSE;
}
DECLARE_HOOK("USER32.dll", SetCapture);
DECLARE_HOOK("USER32.dll", ReleaseCapture);
DECLARE_HOOK("USER32.dll", GetCapture);
DECLARE_HOOK("USER32.dll", GetCursorPos);
DECLARE_HOOK("USER32.dll", GetMessagePos);
DECLARE_HOOK("USER32.dll", SetCursor);

/* The game asks for capture on one of its original windows. Promise it that logical
 * target, but ask Windows to send continuing physical events to the mirror, where
 * they can still be scaled correctly. Match SetCapture's previous-owner return value
 * using the previous GAME target, so the caller never sees our presentation window. */
static HWND WINAPI hooked_SetCapture(HWND window)
{
    HWND mirror_window = display_mirror_window();
    HWND game_frame = display_frame_window();
    HWND previousTarget;
    if (mirror_window && window &&
        (window == game_frame ||
         IsChild(game_frame, window))) { /* route the game's capture through the mirror */
        previousTarget = game_capture_target;
        game_capture_target = window;
        SetCapture(mirror_window);
        has_mirror_capture = TRUE;
        return previousTarget;
    }
    /* Calls for windows outside this game's frame keep normal Windows behavior. */
    return SetCapture(window);
}

/* End both sides of the agreement: forget the game's logical owner and release
 * Windows' physical capture. The return value is Windows' success/failure result. */
static BOOL WINAPI hooked_ReleaseCapture(void)
{
    game_capture_target = NULL;
    has_mirror_capture = FALSE;
    return ReleaseCapture();
}

/* A captured game control must see itself as the owner, even though Windows really
 * sends the events to the mirror. With no logical owner, report Windows' real answer. */
static HWND WINAPI hooked_GetCapture(void)
{
    return game_capture_target ? game_capture_target : GetCapture();
}

/* Answer the game as if the pointer were over its original, smaller view. The value
 * is still a SCREEN position, as this Windows function promises, so existing game
 * code can convert it to whichever child window it needs. Outside translated input,
 * or without an output pointer, leave the ordinary API behavior to Windows. */
static BOOL WINAPI hooked_GetCursorPos(POINT *screenPoint)
{
    HWND mirror_window = display_mirror_window();
    if (mirror_window && has_virtual_cursor && screenPoint) {
        *screenPoint = virtual_cursor_position;
        return TRUE;
    }
    return GetCursorPos(screenPoint);
}

/* This second cursor-query API returns packed x/y desktop coordinates instead of
 * filling a POINT. Give it the same translated position as GetCursorPos so game
 * code does not get two conflicting answers about one forwarded mouse event. */
static DWORD WINAPI hooked_GetMessagePos(void)
{
    HWND mirror_window = display_mirror_window();
    if (mirror_window && has_virtual_cursor)
        return MAKELONG(virtual_cursor_position.x, virtual_cursor_position.y);
    return GetMessagePos();
}

/* Shape changes already work on the UI thread shared by game and mirror. Forward
 * unchanged, returning the previous shape, and only add a limited diagnostic record. */
static HCURSOR WINAPI hooked_SetCursor(HCURSOR cursor)
{
    LONG callCount = InterlockedIncrement(&hook_SetCursor.call_count);
    HCURSOR previousCursor = SetCursor(cursor);
    if (callCount <= 80)
        diagnostics_log("SetCursor %p (prev %p) fromMirrorSetCursor=%d",
                        cursor,
                        previousCursor,
                        is_forwarding_cursor);
    return previousCursor;
}

/* The display's focus handoff must not interrupt a drag the mirror is carrying. */
BOOL input_has_mirror_capture(void)
{
    return has_mirror_capture;
}

/* Windows delivered the requested leave notification. Start tracking again on the
 * next mouse event; stop supplying a translated cursor unless an active capture
 * still needs the drag to continue beyond the mirror's visible bounds. */
void input_mouse_leave(void)
{
    is_tracking_mouse = FALSE;
    if (!has_mirror_capture && !game_capture_target)
        has_virtual_cursor = FALSE;
}

/* Register which six Windows calls the game should enter through this file. Another
 * module patches the GAME'S import table once at startup; this DLL's own Windows
 * calls stay untouched. The table describes routing, not a recurring input loop. */
HookTable input_hook_table(void)
{
    static Hook *const entries[] = {&hook_SetCapture,
                                    &hook_ReleaseCapture,
                                    &hook_GetCapture,
                                    &hook_GetCursorPos,
                                    &hook_GetMessagePos,
                                    &hook_SetCursor};
    hook_SetCapture.replacement = hooked_SetCapture;
    hook_ReleaseCapture.replacement = hooked_ReleaseCapture;
    hook_GetCapture.replacement = hooked_GetCapture;
    hook_GetCursorPos.replacement = hooked_GetCursorPos;
    hook_GetMessagePos.replacement = hooked_GetMessagePos;
    hook_SetCursor.replacement = hooked_SetCursor;
    HookTable table = {entries, sizeof entries / sizeof entries[0]};
    return table;
}
