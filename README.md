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
Windows release. The music wrapper builds successfully, but the intermittent
music-loss/crash problem observed during development is **not proven fixed**.

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

## Display controls and settings

- Resize or maximize the mirror window normally.
- Press **F11** or **Alt+Enter** to toggle borderless fullscreen.
- The original view is scaled; this does not create a larger in-game map viewport.

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

## Build and check

For the display code walkthrough, state and coordinate-system reference, and
per-function guide, read [Display shim internals](docs/display-shim.md).

Requires Windows, Python 3.12+, Git and the 32-bit target compiler from
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw). Dependencies are pinned in
[`dependencies.json`](dependencies.json); source/archive downloads are checked
against SHA-256 or an exact Git commit. Add the compiler's `bin` directory to PATH,
set `LLVM_MINGW` to that directory, or let the scripts find the pinned local toolchain.

```powershell
python -I tools/fetch_deps.py --toolchain
python -I tools/test_release.py
python -I build.py
python -I tools/test_display.py
python -I tools/build_music.py
python -I tools/test_music.py
python -I tools/test_package.py
```

For a local ABI and native check using the installed GOG codecs:

```powershell
python -I tools/build_music.py --game-codec "C:\GOG Games\Imperialism\libvorbisfile-3.dll"
python -I tools/test_music.py --game-directory "C:\GOG Games\Imperialism"
```

Builds go to `build/` and packages to `dist/`; scripts do not install into the game.
The codec source lists follow upstream's fixed MinGW targets, compiled with
llvm-mingw and archived with `llvm-ar`. No upstream prebuilt codec libraries are linked.

## CI and releases

GitHub Actions builds on Windows for pushes, pull requests and manual runs. It checks
i386 DLLs and exports, codec dependencies, native display interception/text logging,
generated-tone decoding and music metadata, and package hashes/contents. It uploads
two artifact groups: **display** and **music** (both music variants).

Hosted CI does not receive the game or GOG's codec DLLs. It compiles the GOG-codec
variant and checks its linkage; its native test with those original DLLs is local only.
The generated-tone tests do not assert audible playback, rapid playback transitions,
or in-game stability. See [`docs/STATUS.md`](docs/STATUS.md) for actual validation.

Set `VERSION`, review/merge the change, then push a matching `vVERSION` tag. After
the build and checks pass, CI uploads all packages and **publishes a GitHub Release**.
Versions such as `0.1.0-preview.1` publish as prereleases; plain versions publish as
stable releases. A temporary draft keeps incomplete uploads private. Rerunning a
published tag preserves its existing assets. Branch/PR CI uploads artifacts without
creating a release. Each ZIP has a manifest; `SHA256SUMS.txt` covers
the ZIPs. Music ZIPs include complete corresponding source and build scripts.
Packaging rejects stale test reports and changed music build inputs, and verifies
extracted codec files against the pinned source archives. `tools/test_package.py`
also checks these rejection paths while restoring each temporary change.

## License and provenance

The display shim and our build/test tooling are MIT licensed; see [`LICENSE`](LICENSE).
The music wrapper is the separately licensed GPLv2 upstream ogg-winmm, retaining
its existing copyright notices. Ogg/Vorbis carry their upstream BSD-style notices.
Music packages include those texts and `corresponding-source.zip` containing the
pinned music source, codec source archives and this repository's build scripts.

See [`ORIGIN.md`](ORIGIN.md) for the original display source commit. No binary patch
to `Imperialism.exe` is required. This project is not affiliated with the game's
developers, publisher or GOG.
