# Imperialism Compat preview

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
