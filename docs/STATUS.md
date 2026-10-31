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
- CI workflow and source-inclusive packages prepared; hosted CI not yet run.

## Limits and next checks

- The intermittent music-loss/crash issue remains unresolved.
- No rapid play/stop, EOF/drain, injected failure or hot-unload tests yet.
- Smoke tests do not verify audible output or actual game transitions.
- Run local game acceptance with each variant and restored original DLLs/settings.
- Run hosted CI after creating/pushing the standalone repository; review draft release
  and forum text before publishing.

The reverse-engineering workspace keeps its own snapshot/portable-core handoff.
