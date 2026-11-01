/* Native-size tiled stock backdrop; game assets are not needed at build time. */
#define WIN32_LEAN_AND_MEAN
#include "backdrop.h"
#include <string.h>

/* Release only objects we own; LoadImage deliberately omits LR_SHARED. This is also
 * safe after any partial load failure and before loading a replacement resource. */
void backdrop_free(Backdrop *backdrop)
{
    if (backdrop->pattern)
        DeleteObject(backdrop->pattern);
    if (backdrop->bitmap)
        DeleteObject(backdrop->bitmap);
    memset(backdrop, 0, sizeof *backdrop);
}

/* CMainFrame's WM_ERASEBKGND handler selects RT_BITMAP 281. Validate its known
 * 128x128, uncompressed 8-bit DIB before asking Windows to decode it. A different
 * executable/resource format must degrade to brown, not to an unrelated image.
 * The caller owns resourceModule; this function never loads or executes game code. */
BOOL backdrop_load(Backdrop *backdrop, HMODULE resourceModule)
{
    HRSRC resource;
    HGLOBAL resourceData;
    const void *bytes;
    BITMAPINFOHEADER header;
    BITMAP bitmapInfo;
    DWORD resourceSize, requiredSize;

    backdrop_free(backdrop);
    if (!resourceModule)
        return FALSE;
    resource =
        FindResourceW(resourceModule, MAKEINTRESOURCEW(BACKDROP_RESOURCE_ID), MAKEINTRESOURCEW(2));
    if (!resource)
        return FALSE;
    resourceSize = SizeofResource(resourceModule, resource);
    if (resourceSize < sizeof header)
        return FALSE;
    resourceData = LoadResource(resourceModule, resource);
    bytes = resourceData ? LockResource(resourceData) : NULL;
    if (!bytes)
        return FALSE;
    memcpy(&header, bytes, sizeof header);
    if (header.biSize != sizeof header || header.biWidth != BACKDROP_TILE_SIZE ||
        header.biHeight != BACKDROP_TILE_SIZE || header.biPlanes != 1 || header.biBitCount != 8 ||
        header.biCompression != BI_RGB || (header.biClrUsed != 0 && header.biClrUsed != 256))
        return FALSE;
    /* Width 128 needs no scanline padding; the stock DIB includes all 256 colors. */
    requiredSize = sizeof header + 256 * sizeof(RGBQUAD) + BACKDROP_TILE_SIZE * BACKDROP_TILE_SIZE;
    if (resourceSize < requiredSize)
        return FALSE;

    backdrop->bitmap = (HBITMAP)LoadImageW(resourceModule,
                                           MAKEINTRESOURCEW(BACKDROP_RESOURCE_ID),
                                           IMAGE_BITMAP,
                                           0,
                                           0,
                                           LR_CREATEDIBSECTION);
    if (!backdrop->bitmap)
        return FALSE;
    if (GetObjectW(backdrop->bitmap, sizeof bitmapInfo, &bitmapInfo) != sizeof bitmapInfo ||
        bitmapInfo.bmWidth != BACKDROP_TILE_SIZE || bitmapInfo.bmHeight != BACKDROP_TILE_SIZE) {
        backdrop_free(backdrop);
        return FALSE;
    }
    backdrop->pattern = CreatePatternBrush(backdrop->bitmap);
    if (!backdrop->pattern) {
        backdrop_free(backdrop);
        return FALSE;
    }
    return TRUE;
}

/* Brown is always painted first so failure to create/draw the optional pattern is
 * harmless. Pattern phase is anchored at mirror-client (0,0), matching the stock
 * frame's tile origin. Restore the caller's DC settings, including its brush origin. */
void backdrop_paint(const Backdrop *backdrop, HDC destination, const RECT *clientRect)
{
    COLORREF previousColor = SetDCBrushColor(destination, BACKDROP_BROWN);
    POINT previousOrigin;
    FillRect(destination, clientRect, (HBRUSH)GetStockObject(DC_BRUSH));
    SetDCBrushColor(destination, previousColor);
    if (backdrop->pattern && SetBrushOrgEx(destination, 0, 0, &previousOrigin)) {
        FillRect(destination, clientRect, backdrop->pattern);
        SetBrushOrgEx(destination, previousOrigin.x, previousOrigin.y, NULL);
    }
}
