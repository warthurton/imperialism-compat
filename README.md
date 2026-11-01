# Imperialism Compat

Display and music compatibility shims for **Imperialism (1997)** on Windows.
An unofficial fan project by Wayne Arthurton. You need your own copy of the game;
these downloads contain no game executable, artwork, music or saves.

**[Download preview v0.1.0-preview.1](https://github.com/warthurton/imperialism-compat/releases/tag/v0.1.0-preview.1)**
— display and music packages, source bundles and checksums.

Compat was deliberately extracted from
[Imperialism RE](https://github.com/warthurton/imperialism-re) to separate two jobs:
this repository maintains runtime display/music compatibility and releases; RE
studies the original executable and develops the portable game-logic reimplementation.
RE references a specific Compat revision through its `compat/` Git submodule.

This is a **preview**. The display implementation was developed against the GOG
Windows release. The music-loss/crash problem **appears fixed in observed use**, but
it has **not been proven fixed in every case**. Please report reproducible failures.


## Music evidence and attribution

The observed improvement came after replacing GOG's bundled music wrapper with
the newer wrapper from the upstream open-source ogg-winmm project. The maintainer
has played with music continuing to work using that rebuilt wrapper **linked
against GOG's existing codec DLLs**. This supports the wrapper update as the
source of the improvement; it does not establish that every music/crash case is fixed.

The alternative build using newer Ogg/Vorbis libraries has only had brief smoke
tests, lasting a few seconds. It has not had enough sustained playback testing
to assess music stability or to attribute any improvement to the newer libraries.
The two builds must not be presented as equally validated for sustained play.

## Components and downloads

| Component | Package | What it does |
|---|---|---|
| Display | `imperialism-compat-display-…-win32.zip` | `msvfw32.dll`: resizable scaled view, maximize, borderless fullscreen, mouse coordinate mapping, desktop resolution preservation, optional text log |
| Music, GOG codecs | `imperialism-compat-music-gog-codecs-…-win32.zip` | Rebuilt `winmm.dll` using the game's existing `libvorbisfile-3.dll`, `libvorbis-0.dll` and `libogg-0.dll` |
| Music, current codecs | `imperialism-compat-music-current-codecs-…-win32.zip` | Rebuilt `winmm.dll` with source-built libogg 1.3.6 and libvorbis 1.3.7 linked statically |

There are two components and two alternative builds of the music component.
Install either component independently, or combine display with **one** music build.
Both music alternatives install the same filename, `winmm.dll`.

The current-codec version supplies its own decoder. The GOG-codec version needs
the three codec DLLs already in the game directory; they are not included here.
Using current codecs is an experiment, not a demonstrated fix for the observed music issue.

## Install and remove

1. Close Imperialism.
2. Back up the existing `msvfw32.dll` and/or `winmm.dll` in the directory containing
   `Imperialism.exe`, keeping the backups outside that directory. Also back up any
   existing shim INI files you want to preserve.
3. For display, copy `msvfw32.dll` and `imperialism_shim.ini` from the display ZIP
   next to `Imperialism.exe`. Merge settings if you already have a customized INI.
4. For music, choose one music ZIP and copy its `winmm.dll` and `winmm.ini` there.
   Keep the game's `Music` directory and existing codec DLLs in place.
5. Start the game normally.

To remove a component, close the game and restore its original DLL and INI backup.
Do not put these DLLs in Windows system directories. These are 32-bit DLLs even
when you run the game on 64-bit Windows.

The builds target modern Windows with UCRT and desktop composition; they have not
been qualified for XP, Wine, macOS or every Windows release.

## What the display shim changes

- Shows the original game view in a resizable, maximizable window, keeping its aspect ratio.
- Offers borderless fullscreen without changing desktop resolution by default.
- Maps mouse positions, button events, cursor shapes and dragging back into the
  original game windows, including child-dialog title bars and close buttons.
- Keeps the original game frame underneath the displayed copy. Suppresses the game's
  own minimize/maximize/resize requests that would expose or disrupt that arrangement;
  minimizing the displayed window still minimizes the original frame for taskbar restore.
- Passes menu commands and keyboard focus back to the game, so its existing handlers run.
- Tiles the original heraldic backdrop in unused space, falling back to plain brown.
- Optionally tints the title bar on supported Windows 11 versions.
- Writes diagnostic API/geometry summaries and optionally records drawn text.
- Forwards the game's video-window export to the genuine Windows MSVFW32 library.

The displayed copy uses Windows desktop composition. Scaling enlarges the existing
picture; it does not add map viewport space or higher-resolution game artwork.
The display component does not change simulation rules or save formats. Music is
provided by the separate, optional `winmm.dll` component described below.

## Display controls and settings

- Resize or maximize the mirror window normally.
- Press **F11** or **Alt+Enter** to toggle borderless fullscreen.
- The original view is scaled; this does not create a larger in-game map viewport.

All display options live in `imperialism_shim.ini` beside the DLL. Values are
read at startup; restart the game after editing. Use `1` for on and `0` for off.

| Section / key | Default | Effect when on; effect when off |
|---|---|---|
| `[display] block_mode_change` | `1` | Preserve desktop resolution; `0` allows the game's normal display-mode requests through. |
| `[display] fullscreen_integer_scale` | `1` | Use whole-number fullscreen magnification when at least 1× fits; `0` uses fractional aspect-preserving scaling to fill more space. |
| `[display] brown_titlebar` | `1` | Request brown title/border colors and light caption text on supported Windows 11; `0` keeps normal system styling. Older Windows versions may ignore the tint. |
| `[display] original_backdrop` | `1` | Tile the game's stock bitmap when available; `0` always uses plain brown. Missing/unsupported artwork also uses brown. |
| `[text_log] enabled` | `0` | `1` records strings the game asks Windows to draw; `0` leaves text capture off. |

Resizing, input translation and F11/Alt+Enter are always enabled; there are no
separate INI toggles for them. Integer scaling affects fullscreen, not normal resizing.
Diagnostic API/geometry logging has no INI switch in the current implementation.

`imperialism_shim.ini` documents all supported settings. Defaults preserve desktop
resolution, use integer scaling in fullscreen where possible, and tint the title
bar on supported Windows 11 builds. Text logging is off by default.

The unused border area tiles the running game's original faint crown/shield/swords
backdrop at its native size. The shim reads bitmap 281 from `Imperialism.exe`; no
game artwork is included in the download. If that bitmap is missing, unsupported
or cannot load, the border stays plain brown. Set `[display] original_backdrop=0`
to always use plain brown. See [Backdrop implementation and evidence](docs/backdrop.md).

`imperialism_gdi_log.txt` is written beside the DLL. If `[text_log] enabled=1`,
`imperialism_text.log` also records strings drawn by the game. Logs are diagnostic
files, not required install assets; review them before attaching them to an issue.
The game directory must be writable for the current diagnostic implementation.
If the diagnostic log cannot be opened, the shim continues without that log.

## Music settings

The upstream [ogg-winmm](https://github.com/ayuanx/ogg-winmm) emulates CD-audio
commands using existing Ogg tracks. `winmm.ini` defaults to `CDDAPath=Music`, with
`CDDAVolume`, `MIDIVolume` and `WAVEVolume` set to 100. Settings are read at startup.
Tracks use names such as `Track02.ogg`; preserve the game's track names and ordering.

These options belong to the `[OGG-WinMM]` section of `winmm.ini`:

| Key | Shipped value | Meaning |
|---|---|---|
| `CDDAPath` | `Music` | Directory containing the Ogg CD-audio tracks. |
| `CDDAVolume` | `100` | CD-audio/music volume percentage, 0–100. |
| `MIDIVolume` | `100` | MIDI volume percentage, 0–100. |
| `WAVEVolume` | `100` | Requested wave-audio volume percentage, 0–100. |

Wave/MIDI volume requests depend on Windows and device support; upstream avoids
some setters on modern Windows to prevent changing master volume. Both music
builds expose the same settings and install as `winmm.dll`; choose one.
The rebuilt wrapper appears to resolve the observed music-loss/crash problem,
but all playback transitions, systems and long-running cases have not been proven.
The codec update alone has not been established as the cause of the improvement.


## Readability and source layout

The shim is designed for readability wherever that preserves correctness and
sound engineering practice. A reader should be able to understand the design
without already knowing MFC or the details of Win32. The game uses MFC; the shim
does not link MFC or require its class hierarchy.

Comments explain Windows concepts at the point they matter, the reason for each
interception, coordinate spaces and ownership. Descriptive names, short functions
and focused C modules keep those explanations close to the code. Readability must
preserve API calling conventions, thread ownership, resource lifetimes and runtime
cost; it is not a reason to replace the game's working behavior with abstractions.

| File | Start here when studying… |
|---|---|
| `src/shim.c` | How Windows loads the proxy, reads settings and forwards the video export |
| `src/display.c` | How the original game window and its larger live preview are managed |
| `src/input.c` | How a click on the preview reaches the correct original child window |
| `src/graphics.c` | How drawing calls are observed without changing their results |
| `src/text_log.c` | How drawing requests become an optional readable text trace |
| `src/hooks.c` | How executable import slots are redirected and original addresses retained |
| `src/diagnostics.c` | How synchronized logs and periodic summaries are written |
| `src/backdrop.c` | How optional stock artwork is loaded, tiled and released |
| `src/shim_internal.h` | The small private contracts between these modules |
| `src/hook_support.h` | The two declaration/logging macros used by API wrappers |

C source and headers target a maximum of **100 columns**. Split long signatures,
calls and conditions; keep one statement per line. `.clang-format` records the
format, and CI checks it with `python -I tools/check_style.py`. Use `--fix` to apply
the same format locally. The beginner walkthrough includes a glossary and a worked
click-coordinate example in [Display shim internals](docs/display-shim.md).

## Development

See [Building, testing, CI and releases](docs/development.md) for toolchain setup,
local checks, CI coverage and publishing.

## License and provenance

The display shim and our build/test tooling are MIT licensed; see [`LICENSE`](LICENSE).
The music wrapper is the separately licensed GPLv2 upstream ogg-winmm, retaining
its existing copyright notices. Ogg/Vorbis carry their upstream BSD-style notices.
Music packages include those texts and `corresponding-source.zip` containing the
pinned music source, codec source archives and this repository's build scripts.

See [`ORIGIN.md`](ORIGIN.md) for the original display source commit. No binary patch
to `Imperialism.exe` is required. This project is not affiliated with the game's
developers, publisher or GOG.
