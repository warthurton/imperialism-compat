# Imperialism Compat — display and music shims for Imperialism (1997), preview

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
are available now. Native smoke tests passed, and both music alternatives completed
a short in-game turn test with the display shim. This is limited acceptance coverage.
No game executable, artwork or music is distributed. Back up the original DLLs before
trying it, and restore those backups to remove it.

<!-- Draft only. Add a reviewed preview-release URL before posting. No forum post has been sent. -->
