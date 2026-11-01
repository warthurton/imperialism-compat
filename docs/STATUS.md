# Status

2026-10-31. Initial standalone preparation; no public release published.

## Implemented

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
- Local game acceptance passed with the display shim and each music variant: load
  `oil_plant_built`, capture before/idle/after and complete game turn 5 -> 6.
  Both variants passed all ten snapshot checks. The acceptance harness restored
  the original DLLs/INI files (hash checked), stopped the game and restored settings.
  This checks a short game session, not long-term music stability or every display control.

## Limits and next checks

- The intermittent music-loss/crash issue remains unresolved.
- No rapid play/stop, EOF/drain, injected failure or hot-unload tests yet.
- Smoke tests do not verify audible output or actual game transitions.
- Review release packages and forum text before publishing. No release tag or
  public release has been created. CI artifacts are available from the workflow.

The reverse-engineering workspace keeps its own snapshot/portable-core handoff.
