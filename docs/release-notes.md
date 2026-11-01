# Imperialism Compat preview

Two optional components for Imperialism (1997):

- **Display:** scaled resizable window, borderless fullscreen, input mapping and preserved desktop resolution.
- **Stock backdrop:** the game's faint crown/shield/swords tile, loaded from its own
  executable at runtime; plain brown if unavailable, or with `original_backdrop=0`.
- **Music:** source-built ogg-winmm, with either the existing GOG codecs or statically linked libogg 1.3.6/libvorbis 1.3.7.

Choose only one music ZIP. Back up existing DLLs before installing; restore those backups to uninstall.
The game and its music are required and are not included. Music packages include corresponding source and licenses.

Compat was deliberately separated from Imperialism RE so runtime compatibility and
releases can evolve independently of decompilation and portable game-logic research.
The source includes a full display-shim walkthrough and comments for its input,
rendering, hook, logging and forwarding paths. RE consumes Compat as a pinned submodule.

This is a preview. The intermittent music-loss/crash issue observed during development
is not proven fixed. Native smoke tests cover DLL loading, display interception/text
logging, decoding and CD-audio metadata, not comprehensive gameplay or audible playback.
