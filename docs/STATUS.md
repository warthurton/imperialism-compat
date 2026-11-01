# Status

2026-11-01. Shim documentation, original backdrop and automatic release publication.

Published preview: [v0.1.0-preview.1](https://github.com/warthurton/imperialism-compat/releases/tag/v0.1.0-preview.1),
from commit `02ed0881196e168e2a2cc8598c906a841a0625cf`.
The [tag workflow](https://github.com/warthurton/imperialism-compat/actions/runs/41324720220)
passed and published all three ZIPs plus `SHA256SUMS.txt`; the release is public
and marked as a prerelease. Existing music-stability and live-focus limits below apply.

## Implemented

- Unreleased readability refactor: eight focused C modules, no shared mutable window/input
  globals, named original-call types, beginner-oriented comments and a glossary/walkthrough.
  CI enforces the checked-in formatter and 100-column source/header limit. README documents
  user-visible changes and all INI options; build/CI details moved to `docs/development.md`.
  Existing focus-guard limits remain; no new live acceptance is claimed for this refactor.

- Full display-shim developer guide in `docs/display-shim.md`; all original functions
  annotated and dense local/parameter names expanded without changing input/hook logic.
- Stock backdrop identified as executable bitmap 281: a 128x128 crown/shield/swords
  tile on RGB(84,46,8). `docs/backdrop.md` records the resource and frame-painter evidence.
  The UI thread loads the owner's bitmap at runtime and frees its GDI objects on
  mirror destruction. Invalid/missing artwork falls back to brown; `original_backdrop=0`
  opts out. No game artwork is distributed.
- Native checks passed for synthetic tiling, missing/truncated resources, data-only
  loading without running fixture code, and the installed game's actual resource.
  Display interception/text logging and the worker's unwritable-log path passed.
- Version-tag CI now publishes complete releases (suffix versions are prereleases),
  verifies release checksums, serializes publication by tag and preserves published
  assets on reruns. Five offline publication-guard checks pass.
- The RE repository now references Compat as a pinned Git submodule, deliberately
  separating runtime compatibility/releases from decompilation and game-logic research.
- Current live attempt `compat_backdrop1` stopped at the existing focus guard before
  fixture input. The restored older shim also stopped there (`snapshot_focus_baseline1`),
  so no new live turn/fullscreen acceptance is claimed. The new startup log confirms
  bitmap 281 loaded, a DWM thumbnail registered and the mirror laid out. Both attempts
  ended with the game stopped and settings restored; the Compat harness also restored
  and hash-checked the original DLLs/INIs. Native backdrop painting checks passed.
- Existing display implementation extracted, with a corrected introductory comment.
- Standalone build and native i386 DLL/export/import checks.
- Display smoke cases verify IAT display-mode interception and text logging off/on.
- Music builds use pinned ogg-winmm commit `11c13ecf727fc0b00b46e04acbf503a60e7df1db`.
- GOG-codec variant uses the installed Vorbisfile DLL; current-codec variant statically
  links source-built libogg 1.3.6 and libvorbis 1.3.7.
- Both music variants passed local generated-tone decoding and native CD-audio
  open/status/length/close/process-exit tests. The GOG variant used the installed
  game's codec DLLs in a temporary local test directory.
- Public repository created; initial Windows CI passed at commit `5d43f3e`:
  https://github.com/warthurton/imperialism-compat/actions/runs/41266369364.
- Packaging binds test reports to DLL hashes and rechecks music build inputs,
  including extracted codec trees against their pinned source archives.
  The follow-up passed Windows CI, including all five rejection checks:
  https://github.com/warthurton/imperialism-compat/actions/runs/41267009413.
- A later documentation-only CI run hit a TLS EOF fetching libogg. Downloads now
  retry transient connection failures up to three times; checksum errors still fail
  immediately. Four offline checks cover recovery, retry limits, 404 and checksum failures.
- Local game acceptance passed with the display shim and each music variant: load
  `oil_plant_built`, capture before/idle/after and complete game turn 5 -> 6.
  Both variants passed all ten snapshot checks. The acceptance harness restored
  the original DLLs/INI files (hash checked), stopped the game and restored settings.
  This checks a short game session, not long-term music stability or every display control.

## Limits and next checks

- The music-loss/crash issue appears fixed in observed use, but is not proven fixed in all cases.
- No rapid play/stop, EOF/drain, injected failure or hot-unload tests yet.
- Smoke tests do not verify audible output or actual game transitions.
- Review forum text before posting. Version-tag CI publishes release packages
  automatically after checks pass; branch/PR builds remain ordinary CI artifacts.

The reverse-engineering workspace keeps its own snapshot/portable-core handoff.
