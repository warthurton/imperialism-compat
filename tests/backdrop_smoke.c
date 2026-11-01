/* Exercise artwork/fallback painting in a memory DC; never run the game or open a window. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../src/backdrop.h"

int wmain(int argc, wchar_t **argv)
{
    HMODULE resources;
    Backdrop backdrop = { 0 };
    BITMAPINFO surfaceInfo = { 0 };
    HDC destination;
    HBITMAP surface, previousBitmap;
    void *pixels;
    RECT bounds = { 0, 0, 260, 260 };
    POINT origin;
    BOOL expectArtwork, loaded, hasDecoration = FALSE;
    int x, y;
    if (argc != 3) return 2;
    expectArtwork = !wcscmp(argv[2], L"fixture") || !wcscmp(argv[2], L"game");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    /* Data-only mapping lets a local check read the owner's exe without executing it. */
    resources = LoadLibraryExW(argv[1], NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    loaded = backdrop_load(&backdrop, resources);
    if (loaded != expectArtwork) return 3;
    surfaceInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    surfaceInfo.bmiHeader.biWidth = bounds.right;
    surfaceInfo.bmiHeader.biHeight = -bounds.bottom;
    surfaceInfo.bmiHeader.biPlanes = 1;
    surfaceInfo.bmiHeader.biBitCount = 32;
    surfaceInfo.bmiHeader.biCompression = BI_RGB;
    destination = CreateCompatibleDC(NULL);
    if (!destination) return 4;
    surface = CreateDIBSection(destination, &surfaceInfo, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!surface) return 5;
    previousBitmap = (HBITMAP)SelectObject(destination, surface);
    SetBrushOrgEx(destination, 3, 5, NULL);
    SetDCBrushColor(destination, RGB(1, 2, 3));
    backdrop_paint(&backdrop, destination, &bounds);
    GetBrushOrgEx(destination, &origin);
    if (origin.x != 3 || origin.y != 5 || GetDCBrushColor(destination) != RGB(1, 2, 3)) return 6;
    for (y = 0; y < 128; y++) {
        for (x = 0; x < 128; x++) {
            COLORREF color = GetPixel(destination, x, y);
            if (color != BACKDROP_BROWN) hasDecoration = TRUE;
            if (color != GetPixel(destination, x + 128, y) || color != GetPixel(destination, x, y + 128)) return 7;
        }
    }
    if (hasDecoration != expectArtwork) return 8;
    if (!wcscmp(argv[2], L"fixture") &&
        (GetPixel(destination, 0, 0) != BACKDROP_BROWN || GetPixel(destination, 1, 0) != RGB(110, 63, 26))) return 9;
    backdrop_free(&backdrop);
    backdrop_free(&backdrop); /* Cleanup remains safe after a failed/previous load. */
    if (backdrop.bitmap || backdrop.pattern) return 10;
    SelectObject(destination, previousBitmap);
    DeleteObject(surface);
    DeleteDC(destination);
    if (resources) FreeLibrary(resources);
    puts(expectArtwork ? "native-size backdrop tiling passed" : "plain-brown fallback passed");
    return 0;
}
