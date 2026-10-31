# Imperialism Compat preview

Two optional components for Imperialism (1997):

- **Display:** scaled resizable window, borderless fullscreen, input mapping and preserved desktop resolution.
- **Music:** source-built ogg-winmm, with either the existing GOG codecs or statically linked libogg 1.3.6/libvorbis 1.3.7.

Choose only one music ZIP. Back up existing DLLs before installing; restore those backups to uninstall.
The game and its music are required and are not included. Music packages include corresponding source and licenses.

This is a preview. The intermittent music-loss/crash issue observed during development
is not proven fixed. Native smoke tests cover DLL loading, display interception/text
logging, decoding and CD-audio metadata, not comprehensive gameplay or audible playback.
