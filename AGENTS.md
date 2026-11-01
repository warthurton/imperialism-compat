# Imperialism Compat

Read README.md and docs/STATUS.md first. This repo contains the display shim and
music-wrapper build tooling; the game-logic reimplementation remains in imperialism-re.

- Never commit game files, game codecs/music, decompiler output, compiled DLLs or third-party binaries.
- Our code is MIT; upstream ogg-winmm is GPLv2. Preserve its notices and include complete
  corresponding build source with music artifacts. Fetch dependencies only through pinned sources.
- Use topic branches after the initial repository bootstrap. Keep docs/STATUS.md current.
- Write text/scripts/messages to files with editing tools; shell commands stay short.
- Build/check commands are in README. Run native tests after changing DLL/build code.
- Native smoke tests are process-lifetime hosts. In-game acceptance is separate and must
  restore installed DLLs, settings and stopped-game state even on failure.
- Do not claim that metadata/decoder smoke tests prove audible playback or solve the
  intermittent music-loss problem. Record what was actually exercised.
- Matching version-tag CI publishes GitHub Releases after all checks pass. Versions
  with a suffix publish as prereleases. Keep published assets immutable on reruns.
  Forum posting remains a maintainer action.
