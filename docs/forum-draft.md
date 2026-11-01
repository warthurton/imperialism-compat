# Imperialism Compat — display and music shims for Imperialism (1997), preview

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


Imperialism Compat is a small, unofficial compatibility project for the GOG Windows
release. The display component provides a resizable scaled window and borderless
fullscreen, with mouse input mapped back to the game's original view. F11 or Alt+Enter
toggles fullscreen, and it can keep the desktop resolution unchanged.

There is also an optional rebuilt music wrapper, available in two versions: one uses
the codec DLLs already supplied with the game; the other links current stable
Ogg/Vorbis libraries into the wrapper. Install only one music version.

The music builds are experimental. An intermittent music-loss/crash problem seen in
development is not proven fixed, and updating the codecs is not being presented as a
confirmed solution. Reports with reproducible steps would help narrow it down.

The [source and installation instructions](https://github.com/warthurton/imperialism-compat)
and [Windows CI artifacts](https://github.com/warthurton/imperialism-compat/actions/workflows/build.yml)
are available now, along with the published
[v0.1.0-preview.1 downloads](https://github.com/warthurton/imperialism-compat/releases/tag/v0.1.0-preview.1).
The display shim can reuse the game's faint heraldic background tile, with plain
brown if unavailable; the download includes no game artwork. Native smoke tests
passed. The earlier extracted build completed short in-game turn tests with both
music alternatives; the documented/backdrop follow-up's live retest stopped at a
foreground-focus guard, also seen with the older build. Acceptance coverage is limited.
No game executable, artwork or music is distributed. Back up the original DLLs before
trying it, and restore those backups to remove it.

<!-- Draft for maintainer review. No forum post has been sent. -->
