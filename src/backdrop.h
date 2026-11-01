/* Original game artwork is loaded at runtime; this project ships no bitmap. */
#ifndef IMPERIALISM_BACKDROP_H
#define IMPERIALISM_BACKDROP_H

#include <windows.h>

#define BACKDROP_BROWN RGB(84, 46, 8)
#define BACKDROP_RESOURCE_ID 281
#define BACKDROP_TILE_SIZE 128

/* UI-thread owned. Start zero-initialized; release both GDI objects on destruction. */
typedef struct {
    HBITMAP bitmap;
    HBRUSH pattern;
} Backdrop;

/* A missing, unsupported or invalid bitmap leaves this object usable as plain brown. */
BOOL backdrop_load(Backdrop *backdrop, HMODULE resourceModule);
void backdrop_paint(const Backdrop *backdrop, HDC destination, const RECT *clientRect);
void backdrop_free(Backdrop *backdrop);

#endif
