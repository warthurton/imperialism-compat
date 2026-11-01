/* Drawing observers: log how the game draws, then call the same Windows API.
 * These hooks do not resize pictures; Windows composition does that in display.c.
 *
 * An HDC (device context) identifies a drawing surface and its current pen, brush,
 * colors and clipping settings. It can describe a window OR a bitmap in memory.
 * HBITMAP and HPALETTE are handles for images and indexed-color tables.
 *
 * Each wrapper follows the same recipe: record arguments, then call "original"
 * with those arguments unchanged. OriginalFunction gives the saved address its
 * exact C signature. WINAPI also specifies who removes arguments from the stack
 * on 32-bit Windows; preserving it is essential, not just a naming convention. */
#include "shim_internal.h"
#include "hook_support.h"

DECLARE_HOOK("GDI32.dll", BitBlt);
DECLARE_HOOK("GDI32.dll", PatBlt);
DECLARE_HOOK("GDI32.dll", StretchDIBits);
DECLARE_HOOK("GDI32.dll", CreateDIBSection);
DECLARE_HOOK("GDI32.dll", CreateDIBitmap);
DECLARE_HOOK("GDI32.dll", CreateCompatibleDC);
DECLARE_HOOK("GDI32.dll", CreateCompatibleBitmap);
DECLARE_HOOK("GDI32.dll", CreatePalette);
DECLARE_HOOK("GDI32.dll", SelectPalette);
DECLARE_HOOK("GDI32.dll", RealizePalette);
DECLARE_HOOK("GDI32.dll", SetStretchBltMode);
DECLARE_HOOK("GDI32.dll", GetDeviceCaps);
DECLARE_HOOK("GDI32.dll", LineTo);
DECLARE_HOOK("USER32.dll", GetDC);
DECLARE_HOOK("USER32.dll", ReleaseDC);
DECLARE_HOOK("USER32.dll", BeginPaint);
DECLARE_HOOK("USER32.dll", EndPaint);

/* Copy an existing rectangle between drawing surfaces. PatBlt instead fills a
 * rectangle using the destination's current brush. The wrappers preserve the original
 * drawing arguments and return values; Windows still performs the requested drawing. */
static BOOL WINAPI hooked_BitBlt(HDC destinationDC,
                                 int destinationX,
                                 int destinationY,
                                 int width,
                                 int height,
                                 HDC sourceDC,
                                 int sourceX,
                                 int sourceY,
                                 DWORD rasterOperation)
{
    typedef BOOL(WINAPI * OriginalFunction)(HDC, int, int, int, int, HDC, int, int, DWORD);
    OriginalFunction original = (OriginalFunction)hook_BitBlt.original;

    LOG_HOOK(BitBlt,
             "dst=%p (%d,%d %dx%d) src=%p (%d,%d) rop=%08lx",
             destinationDC,
             destinationX,
             destinationY,
             width,
             height,
             sourceDC,
             sourceX,
             sourceY,
             rasterOperation);
    return original(destinationDC,
                    destinationX,
                    destinationY,
                    width,
                    height,
                    sourceDC,
                    sourceX,
                    sourceY,
                    rasterOperation);
}

static BOOL WINAPI hooked_PatBlt(HDC destinationDC,
                                 int destinationX,
                                 int destinationY,
                                 int width,
                                 int height,
                                 DWORD rasterOperation)
{
    typedef BOOL(WINAPI * OriginalFunction)(HDC, int, int, int, int, DWORD);
    OriginalFunction original = (OriginalFunction)hook_PatBlt.original;

    LOG_HOOK(PatBlt,
             "dst=%p (%d,%d %dx%d) rop=%08lx",
             destinationDC,
             destinationX,
             destinationY,
             width,
             height,
             rasterOperation);
    return original(destinationDC, destinationX, destinationY, width, height, rasterOperation);
}

/* A DIB is a bitmap described by a header, optional palette and raw pixels in
 * memory. The game's own call can resize it; our wrapper observes that call only. */
static int WINAPI hooked_StretchDIBits(HDC destinationDC,
                                       int destinationX,
                                       int destinationY,
                                       int destinationWidth,
                                       int destinationHeight,
                                       int sourceX,
                                       int sourceY,
                                       int sourceWidth,
                                       int sourceHeight,
                                       const void *bits,
                                       const BITMAPINFO *bitmapInfo,
                                       UINT colorUsage,
                                       DWORD rasterOperation)
{
    typedef int(WINAPI * OriginalFunction)(
        HDC, int, int, int, int, int, int, int, int, const void *, const BITMAPINFO *, UINT, DWORD);
    OriginalFunction original = (OriginalFunction)hook_StretchDIBits.original;

    LOG_HOOK(StretchDIBits,
             "dst=%p (%d,%d %dx%d) src=(%d,%d %dx%d) bpp=%d use=%u rop=%08lx",
             destinationDC,
             destinationX,
             destinationY,
             destinationWidth,
             destinationHeight,
             sourceX,
             sourceY,
             sourceWidth,
             sourceHeight,
             bitmapInfo ? bitmapInfo->bmiHeader.biBitCount : -1,
             colorUsage,
             rasterOperation);
    return original(destinationDC,
                    destinationX,
                    destinationY,
                    destinationWidth,
                    destinationHeight,
                    sourceX,
                    sourceY,
                    sourceWidth,
                    sourceHeight,
                    bits,
                    bitmapInfo,
                    colorUsage,
                    rasterOperation);
}

/* These create bitmap storage. A DIB section additionally exposes its pixel
 * pointer to the application; CreateDIBitmap initializes an image from given data. */
static HBITMAP WINAPI hooked_CreateDIBSection(HDC deviceContext,
                                              const BITMAPINFO *bitmapInfo,
                                              UINT colorUsage,
                                              void **pixelStorage,
                                              HANDLE section,
                                              DWORD offset)
{
    typedef HBITMAP(WINAPI *
                    OriginalFunction)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
    OriginalFunction original = (OriginalFunction)hook_CreateDIBSection.original;

    LOG_HOOK(CreateDIBSection,
             "%ldx%ld bpp=%d clrUsed=%lu use=%u",
             bitmapInfo ? bitmapInfo->bmiHeader.biWidth : 0,
             bitmapInfo ? bitmapInfo->bmiHeader.biHeight : 0,
             bitmapInfo ? bitmapInfo->bmiHeader.biBitCount : 0,
             bitmapInfo ? bitmapInfo->bmiHeader.biClrUsed : 0,
             colorUsage);
    return original(deviceContext, bitmapInfo, colorUsage, pixelStorage, section, offset);
}

static HBITMAP WINAPI hooked_CreateDIBitmap(HDC deviceContext,
                                            const BITMAPINFOHEADER *header,
                                            DWORD initialization,
                                            const void *pixels,
                                            const BITMAPINFO *bitmapInfo,
                                            UINT colorUsage)
{
    typedef HBITMAP(WINAPI * OriginalFunction)(
        HDC, const BITMAPINFOHEADER *, DWORD, const void *, const BITMAPINFO *, UINT);
    OriginalFunction original = (OriginalFunction)hook_CreateDIBitmap.original;

    LOG_HOOK(CreateDIBitmap,
             "%ldx%ld bpp=%d init=%lu",
             header ? header->biWidth : 0,
             header ? header->biHeight : 0,
             header ? header->biBitCount : 0,
             initialization);
    return original(deviceContext, header, initialization, pixels, bitmapInfo, colorUsage);
}

/* "Compatible" means a scratch drawing surface/bitmap in the source device's
 * format. It does not copy the source image or create another visible window. */
static HDC WINAPI hooked_CreateCompatibleDC(HDC sourceDC)
{
    typedef HDC(WINAPI * OriginalFunction)(HDC);
    OriginalFunction original = (OriginalFunction)hook_CreateCompatibleDC.original;

    HDC createdDC = original(sourceDC);
    LOG_HOOK(CreateCompatibleDC, "from=%p -> %p", sourceDC, createdDC);
    return createdDC;
}

static HBITMAP WINAPI hooked_CreateCompatibleBitmap(HDC deviceContext, int width, int height)
{
    typedef HBITMAP(WINAPI * OriginalFunction)(HDC, int, int);
    OriginalFunction original = (OriginalFunction)hook_CreateCompatibleBitmap.original;

    LOG_HOOK(CreateCompatibleBitmap, "%dx%d", width, height);
    return original(deviceContext, width, height);
}

/* Older graphics store a color index rather than a full RGB value per pixel.
 * Create/Select/RealizePalette create, choose and apply the index-to-color table.
 * Observing these calls helps diagnose colors without changing the game's palette. */
static HPALETTE WINAPI hooked_CreatePalette(const LOGPALETTE *palette)
{
    typedef HPALETTE(WINAPI * OriginalFunction)(const LOGPALETTE *);
    OriginalFunction original = (OriginalFunction)hook_CreatePalette.original;

    LOG_HOOK(CreatePalette, "entries=%u", palette ? palette->palNumEntries : 0);
    return original(palette);
}

static HPALETTE WINAPI hooked_SelectPalette(HDC deviceContext,
                                            HPALETTE palette,
                                            BOOL forceBackground)
{
    typedef HPALETTE(WINAPI * OriginalFunction)(HDC, HPALETTE, BOOL);
    OriginalFunction original = (OriginalFunction)hook_SelectPalette.original;

    LOG_HOOK(SelectPalette, "dc=%p pal=%p bg=%d", deviceContext, palette, forceBackground);
    return original(deviceContext, palette, forceBackground);
}

static UINT WINAPI hooked_RealizePalette(HDC deviceContext)
{
    typedef UINT(WINAPI * OriginalFunction)(HDC);
    OriginalFunction original = (OriginalFunction)hook_RealizePalette.original;

    LOG_HOOK(RealizePalette, "dc=%p", deviceContext);
    return original(deviceContext);
}

static int WINAPI hooked_SetStretchBltMode(HDC deviceContext, int stretchMode)
{
    typedef int(WINAPI * OriginalFunction)(HDC, int);
    OriginalFunction original = (OriginalFunction)hook_SetStretchBltMode.original;

    LOG_HOOK(SetStretchBltMode, "mode=%d", stretchMode);
    return original(deviceContext, stretchMode);
}

static int WINAPI hooked_GetDeviceCaps(HDC deviceContext, int capabilityIndex)
{
    typedef int(WINAPI * OriginalFunction)(HDC, int);
    OriginalFunction original = (OriginalFunction)hook_GetDeviceCaps.original;

    int capability = original(deviceContext, capabilityIndex);
    LOG_HOOK(GetDeviceCaps, "index=%d -> %d", capabilityIndex, capability);
    return capability;
}

static BOOL WINAPI hooked_LineTo(HDC deviceContext, int endX, int endY)
{
    typedef BOOL(WINAPI * OriginalFunction)(HDC, int, int);
    OriginalFunction original = (OriginalFunction)hook_LineTo.original;

    LOG_HOOK(LineTo, "(%d,%d)", endX, endY);
    return original(deviceContext, endX, endY);
}

/* GetDC/ReleaseDC borrow and return a window's drawing surface. BeginPaint/EndPaint
 * do similar work while responding to a paint event, including Windows' tracking
 * of which damaged area needs redrawing. Always preserve that real bookkeeping. */
static HDC WINAPI hooked_GetDC(HWND window)
{
    typedef HDC(WINAPI * OriginalFunction)(HWND);
    OriginalFunction original = (OriginalFunction)hook_GetDC.original;

    HDC deviceContext = original(window);
    LOG_HOOK(GetDC, "wnd=%p -> %p", window, deviceContext);
    return deviceContext;
}

static int WINAPI hooked_ReleaseDC(HWND window, HDC deviceContext)
{
    typedef int(WINAPI * OriginalFunction)(HWND, HDC);
    OriginalFunction original = (OriginalFunction)hook_ReleaseDC.original;

    LOG_HOOK(ReleaseDC, "wnd=%p dc=%p", window, deviceContext);
    return original(window, deviceContext);
}

static HDC WINAPI hooked_BeginPaint(HWND window, PAINTSTRUCT *paint)
{
    typedef HDC(WINAPI * OriginalFunction)(HWND, PAINTSTRUCT *);
    OriginalFunction original = (OriginalFunction)hook_BeginPaint.original;

    HDC deviceContext = original(window, paint);
    LOG_HOOK(BeginPaint,
             "wnd=%p dc=%p rc=(%ld,%ld,%ld,%ld)",
             window,
             deviceContext,
             paint->rcPaint.left,
             paint->rcPaint.top,
             paint->rcPaint.right,
             paint->rcPaint.bottom);
    return deviceContext;
}

static BOOL WINAPI hooked_EndPaint(HWND window, const PAINTSTRUCT *paint)
{
    typedef BOOL(WINAPI * OriginalFunction)(HWND, const PAINTSTRUCT *);
    OriginalFunction original = (OriginalFunction)hook_EndPaint.original;

    LOG_HOOK(EndPaint, "wnd=%p", window);
    return original(window, paint);
}

/* Called once before the executable import table is changed. Each row keeps
 * the real Windows address separate from our replacement function. */
HookTable graphics_hook_table(void)
{
    static Hook *const entries[] = {&hook_BitBlt,
                                    &hook_PatBlt,
                                    &hook_StretchDIBits,
                                    &hook_CreateDIBSection,
                                    &hook_CreateDIBitmap,
                                    &hook_CreateCompatibleDC,
                                    &hook_CreateCompatibleBitmap,
                                    &hook_CreatePalette,
                                    &hook_SelectPalette,
                                    &hook_RealizePalette,
                                    &hook_SetStretchBltMode,
                                    &hook_GetDeviceCaps,
                                    &hook_LineTo,
                                    &hook_GetDC,
                                    &hook_ReleaseDC,
                                    &hook_BeginPaint,
                                    &hook_EndPaint};
    hook_BitBlt.replacement = hooked_BitBlt;
    hook_PatBlt.replacement = hooked_PatBlt;
    hook_StretchDIBits.replacement = hooked_StretchDIBits;
    hook_CreateDIBSection.replacement = hooked_CreateDIBSection;
    hook_CreateDIBitmap.replacement = hooked_CreateDIBitmap;
    hook_CreateCompatibleDC.replacement = hooked_CreateCompatibleDC;
    hook_CreateCompatibleBitmap.replacement = hooked_CreateCompatibleBitmap;
    hook_CreatePalette.replacement = hooked_CreatePalette;
    hook_SelectPalette.replacement = hooked_SelectPalette;
    hook_RealizePalette.replacement = hooked_RealizePalette;
    hook_SetStretchBltMode.replacement = hooked_SetStretchBltMode;
    hook_GetDeviceCaps.replacement = hooked_GetDeviceCaps;
    hook_LineTo.replacement = hooked_LineTo;
    hook_GetDC.replacement = hooked_GetDC;
    hook_ReleaseDC.replacement = hooked_ReleaseDC;
    hook_BeginPaint.replacement = hooked_BeginPaint;
    hook_EndPaint.replacement = hooked_EndPaint;
    HookTable table = {entries, sizeof entries / sizeof entries[0]};
    return table;
}
