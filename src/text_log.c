/* Optional readable-text trace. Windows draws into a device context (HDC),
 * which may be a hidden bitmap; a logged string is a draw request, not a screenshot. */
#include "shim_internal.h"
#include "hook_support.h"
#include <string.h>

/* ---- text log: the strings the game draws, with positions (off unless [text_log] enabled=1 in
 * imperialism_shim.ini) ---- Lets a test harness READ what is on screen (offer sheets, hot text,
 * dialogs) instead of looking at pixels. One line per string:
 * "<tick> <T|X|B|D> <x>,<y> <text>" in imperialism_text.log; the same string at the same place is
 * not repeated within 1.5 s. */
static BOOL is_text_logging_enabled; /* Startup-only INI opt-in. */
static FILE *text_file; /* Adjacent imperialism_text.log, truncated on attach. */
static struct {
    unsigned hash;
    DWORD timestamp;
} recent_text[128]; /* Hash/tick ring, guarded by log_guard. */
static unsigned next_recent_slot; /* Next ring entry modulo 128; guarded by log_guard. */

/* Record ANSI draw text before forwarding the drawing API. Limit to 400 bytes, replace
 * control bytes with spaces and trim trailing spaces. The rolling hash includes the
 * original bytes and draw position, but not API kind or HDC. Under log_guard, suppress a
 * matching hash for 1500 ms using a 128-slot ring, then append/flush the text log. */
static void text_out(const char *kind, int drawX, int drawY, const char *text, int length)
{
    char line[420];
    int index, outputLength = 0;
    unsigned hash = 2166136261u;
    DWORD timestamp;
    if (!is_text_logging_enabled || !text_file || !text)
        return;
    if (length < 0)
        length = (int)strlen(text);
    if (length > 400)
        length = 400;
    for (index = 0; index < length; index++) {
        unsigned char character = (unsigned char)text[index];
        line[outputLength++] = (character < 32) ? ' ' : (char)character;
        hash = (hash ^ character) * 16777619u;
    }
    while (outputLength && line[outputLength - 1] == ' ')
        outputLength--;
    line[outputLength] = 0;
    if (!outputLength)
        return;
    hash = (hash ^ (unsigned)drawX) * 16777619u;
    hash = (hash ^ (unsigned)drawY) * 16777619u;
    timestamp = GetTickCount();
    diagnostics_lock();
    for (index = 0; index < 128; index++)
        if (recent_text[index].hash == hash && timestamp - recent_text[index].timestamp < 1500) {
            diagnostics_unlock();
            return;
        }
    recent_text[next_recent_slot & 127].hash = hash;
    recent_text[next_recent_slot & 127].timestamp = timestamp;
    next_recent_slot++;
    fprintf(text_file, "%lu %s %d,%d %s\n", timestamp, kind, drawX, drawY, line);
    fflush(text_file);
    diagnostics_unlock();
}

DECLARE_HOOK("GDI32.dll", TextOutA);
DECLARE_HOOK("GDI32.dll", ExtTextOutA);
DECLARE_HOOK("USER32.dll", TabbedTextOutA);
DECLARE_HOOK("USER32.dll", DrawTextA);

/* ANSI text taps preserve the original drawing call and return value. Glyph-index
 * ExtTextOut and DrawText layout-only calls are not readable text, so skip those
 * log entries. Coordinates belong to each supplied HDC, not the mirror/desktop.
 * These wrappers intentionally do not use LOG_HOOK or increment its counters. */
static BOOL WINAPI hooked_TextOutA(HDC deviceContext, int drawX, int drawY, LPCSTR text, int length)
{
    typedef BOOL(WINAPI * OriginalFunction)(HDC, int, int, LPCSTR, int);
    OriginalFunction original = (OriginalFunction)hook_TextOutA.original;

    text_out("T", drawX, drawY, text, length);
    return original(deviceContext, drawX, drawY, text, length);
}
static BOOL WINAPI hooked_ExtTextOutA(HDC deviceContext,
                                      int drawX,
                                      int drawY,
                                      UINT options,
                                      const RECT *clippingRect,
                                      LPCSTR text,
                                      UINT length,
                                      const INT *advances)
{
    typedef BOOL(WINAPI *
                 OriginalFunction)(HDC, int, int, UINT, const RECT *, LPCSTR, UINT, const INT *);
    OriginalFunction original = (OriginalFunction)hook_ExtTextOutA.original;

    if (!(options & ETO_GLYPH_INDEX))
        text_out("X", drawX, drawY, text, (int)length);
    return original(deviceContext, drawX, drawY, options, clippingRect, text, length, advances);
}
static LONG WINAPI hooked_TabbedTextOutA(HDC deviceContext,
                                         int drawX,
                                         int drawY,
                                         LPCSTR text,
                                         int length,
                                         int tabCount,
                                         const INT *tabStops,
                                         int tabOrigin)
{
    typedef LONG(WINAPI * OriginalFunction)(HDC, int, int, LPCSTR, int, int, const INT *, int);
    OriginalFunction original = (OriginalFunction)hook_TabbedTextOutA.original;

    text_out("B", drawX, drawY, text, length);
    return original(deviceContext, drawX, drawY, text, length, tabCount, tabStops, tabOrigin);
}
static int WINAPI
hooked_DrawTextA(HDC deviceContext, LPCSTR text, int length, LPRECT drawRect, UINT format)
{
    typedef int(WINAPI * OriginalFunction)(HDC, LPCSTR, int, LPRECT, UINT);
    OriginalFunction original = (OriginalFunction)hook_DrawTextA.original;

    if (!(format & DT_CALCRECT) && drawRect)
        text_out("D", drawRect->left, drawRect->top, text, length);
    return original(deviceContext, text, length, drawRect, format);
}

void text_log_open(const char *path)
{
    is_text_logging_enabled = TRUE;
    text_file = fopen(path, "w");
}

/* Called once before the executable import table is changed. Each row keeps
 * the real Windows address separate from our replacement function. */
HookTable text_hook_table(void)
{
    static Hook *const entries[] = {
        &hook_TextOutA, &hook_ExtTextOutA, &hook_TabbedTextOutA, &hook_DrawTextA};
    hook_TextOutA.replacement = hooked_TextOutA;
    hook_ExtTextOutA.replacement = hooked_ExtTextOutA;
    hook_TabbedTextOutA.replacement = hooked_TabbedTextOutA;
    hook_DrawTextA.replacement = hooked_DrawTextA;
    HookTable table = {entries, sizeof entries / sizeof entries[0]};
    return table;
}
