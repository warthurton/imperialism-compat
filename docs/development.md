# Building, testing, CI and releases

Run commands from the repository root. For installation, controls and settings,
see the [user guide](../README.md).

## Build and check

For the display code walkthrough, state and coordinate-system reference, and
per-function guide, read [Display shim internals](display-shim.md).

Requires Windows, Python 3.12+, Git and the 32-bit target compiler from
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw). Dependencies are pinned in
[`dependencies.json`](../dependencies.json); source/archive downloads are checked
against SHA-256 or an exact Git commit. Add the compiler's `bin` directory to PATH,
set `LLVM_MINGW` to that directory, or let the scripts find the pinned local toolchain.

```powershell
python -I tools/fetch_deps.py --toolchain
python -I tools/test_release.py
python -I build.py
python -I tools/check_style.py
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
or in-game stability. See [`docs/STATUS.md`](STATUS.md) for actual validation.

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
