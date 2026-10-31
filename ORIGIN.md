# Source provenance

The display shim was extracted from Wayne Arthurton's MIT-licensed
[imperialism-re](https://github.com/warthurton/imperialism-re) workspace at commit
[`7135f469a41b2f35b341a44b5a10a0e16f1b6a1d`](https://github.com/warthurton/imperialism-re/commit/7135f469a41b2f35b341a44b5a10a0e16f1b6a1d).
The original development history remains there; this repository starts with a focused import.

| Original path | Imported path | Original SHA-256 (working-tree bytes) |
|---|---|---|
| `shim/shim.c` | `src/shim.c` | `d45ec0e975cc7577907749ed8b824495d8e06057f5693cf25a4557304244dff5` |
| `shim/msvfw32.def` | `src/msvfw32.def` | `85a489d5b1f172457c5ca94e4bfbf71e26c0f0afbbf7b135f238e1d151b5493e` |

The initial standalone import replaces the obsolete observation-only comment at the
top of `shim.c`. The display/input implementation is unchanged. Build, packaging,
tests and user documentation are new for the standalone repository.

The retired runtime music-patching archive is outside this repository. The music component
builds the GPLv2 upstream ogg-winmm from its pinned source, with its own license and notices;
it is not covered by the display shim's MIT license. No game executable, assets, saves,
decompiler output, or game-supplied codec binaries are included. Game ownership is required.
