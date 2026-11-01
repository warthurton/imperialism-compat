# Stock backdrop: evidence and implementation

The display shim can fill the space around the scaled game view with the game's
own brown heraldic pattern. It loads the bitmap from the running executable;
this repository and its release packages contain no game artwork. This note
separates observations of the GOG executable from the new implementation in
[`src/backdrop.c`](../src/backdrop.c) and
[`src/backdrop.h`](../src/backdrop.h).

## Confirmed in the original executable

The stock Windows frame uses **`RT_BITMAP` resource 281 (`0x119`) inside
`Imperialism.exe`**. This is an integer resource ID, not a filename in the
`Data/*.gob` containers. Inspection of that resource found:

| Property | Observed value |
|---|---|
| Dimensions | 128 by 128 pixels |
| DIB header | 40-byte `BITMAPINFOHEADER`; one plane |
| Pixels | 8-bit indexed color, `BI_RGB` (uncompressed), positive height |
| Palette | 256 `RGBQUAD` entries |
| Resource length | 17,448 bytes: 40 header + 1,024 palette + 16,384 pixels |
| Main brown | RGB(84, 46, 8), present in 15,574 of the 16,384 pixels |
| Main watermark shade | RGB(110, 63, 26), present in 702 pixels |

The image contains faint crown/shield/crossed-sword motifs and no text. Nearby
resources 272 and 273 have brighter, stippled variations; the frame painter
selects **281**. Resource 274 is uniformly RGB(84, 46, 8), independently matching
the shim's plain-brown fallback.

The evidence is the executable's resource directory and machine-code inspection,
not recovered original C++ source. Addresses below identify the inspected GOG
build and are documentation only; the shim does not call or patch them.

| Evidence | Address and behavior |
|---|---|
| Frame message map | `0x648640`; its `WM_ERASEBKGND` entry at `0x6486f0` points through thunk `0x402ea0` to `0x4859d0` |
| Default selection | Frame construction at `0x484af0` / `0x484bf0` sets `+0xc0` to `0x0100005f`; this selects the bitmap branch in the erase handler |
| Bitmap selection | The handler lazily stores a `CDib` at frame `+0xc4`; the instruction at `0x485a7e` supplies resource `0x119` |
| Resource loading | Thunk `0x40158c` reaches `0x47c080`, which uses `FindResourceA` with module `NULL`, resource 281 and type 2, then `LoadResource` |
| Tiling | `0x485ae3` through `0x485b3c` starts at client (0,0) and advances both axes by 128 pixels |
| Drawing | Thunk `0x408cbf` reaches `0x47ab60`: `StretchDIBits` at the bitmap's native dimensions, `DIB_RGB_COLORS`, `SRCCOPY` |

Thus the original pattern repeats at native size from the frame's client origin.
It is not stretched to cover the screen, centered as a single image, alpha-blended
or drawn with a transparent color key. These findings identify this Windows
build's behavior; they do not establish identical assets in every game edition.

## Shim behavior

`mirror_init` attempts the load on the game's UI thread when it creates the
presentation window. Loading and GDI object creation happen outside `DllMain`.
The module handle comes from `GetModuleHandleW(NULL)`, so normal runtime use
neither opens another executable nor searches for an external asset.

`backdrop_load` validates the resource header, dimensions, format, palette count
and minimum byte length before asking Windows to decode it. It uses
[`LoadImageW`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadimagew)
with `IMAGE_BITMAP`, zero requested dimensions and `LR_CREATEDIBSECTION`. These
options retain the resource size and avoid mapping it to the display's palette.
The returned bitmap dimensions are checked again.

The shim uses a color
[`CreatePatternBrush`](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createpatternbrush)
brush to repeat the bitmap. This is a new implementation of the observed opaque
tiling behavior, rather than a copy of the game's `CDib` renderer. The color
pattern covers its destination pixels, matching the original `SRCCOPY` policy;
there is no transparency or watermark extraction. `backdrop_paint` anchors the
brush at mirror-client (0,0), regardless of the scaled thumbnail's position, and
restores the caller's brush origin and DC brush color afterward.

Every paint first fills plain RGB(84, 46, 8). Missing or unsupported resources,
validation/load failures, bitmap/brush creation failures, or failure to apply the
optional pattern leave that brown base as the fallback. Setting
`[display] original_backdrop=0` skips the resource load and keeps the same brown
fill. The setting is read at startup.

The UI thread owns both GDI objects. `LR_SHARED` is deliberately omitted, and
`backdrop_free` deletes both the pattern brush and bitmap, then clears their
handles. The mirror's `WM_DESTROY` handler calls it. Partial-load failures and
replacement loads use the same cleanup path.

## Local inspection and validation

A standalone local test may read the owner's installed executable with
[`LoadLibraryExW`](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)
using `LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE`. This maps it
for resource access without executing its code; release that test mapping with
`FreeLibrary`. This is for local tests only. The installed shim already runs
inside the game and uses its existing module handle. Neither approach requires
distributing extracted bitmaps or the executable.

Useful test assertions are:

- A synthetic, nonuniform 128-by-128 indexed resource repeats exactly every 128
  pixels on both axes, including a destination whose dimensions are not tile
  multiples; changing the DC's incoming brush origin does not shift the result.
- Missing, truncated or incompatible resources render only the fallback brown;
  the opt-out also produces plain brown in the presentation window.
- Painting preserves caller DC settings. Repeated load/free and partial failures
  clear both owned handles without growing the process's GDI-object count.
- An optional local resource-only check validates resource 281 and its palette;
  an in-game check verifies the exposed borders during resize and fullscreen.

These are validation expectations, not a claim that every failure path or game
edition has been tested. See [STATUS.md](STATUS.md) for completed checks. Hosted
tests should use generated artwork rather than copied game pixels.
