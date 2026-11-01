# Changelog

## Unreleased

- Split the display shim into eight focused C modules with private state and explicit contracts.
- Explain Windows concepts inline and in a beginner walkthrough; replace hidden original-call casts
  with named types and enforce 100-column C/header formatting in CI.
- Make README the user guide, including behavior changes and every supported INI option;
  move build/check/CI/release instructions to `docs/development.md`.
- Record the maintainer's observation: music loss/crashes appear fixed, but not proven in all cases.

## 0.1.0-preview.1

- Deliberately extract the display shim and music build tooling from Imperialism RE:
  Compat maintains runtime compatibility and distributable releases; RE owns
  decompilation, state inspection and portable game-logic research. RE links a
  pinned Compat revision as its `compat/` submodule.
- Document the display shim's functions, state, coordinate mapping and hook table;
  replace dense local variable names with names that describe their purpose.
- Restore the stock 128-pixel crown/shield/swords backdrop from the running game's
  bitmap 281, with plain-brown fallback and an `original_backdrop=0` opt-out.
  Artwork remains in the owner's game installation and is not redistributed.
- Keep the diagnostic worker safe when its log path is unwritable.
- Build the pinned upstream music wrapper in GOG-codec and statically linked current-codec variants.
- Add native smoke tests, package manifests/source bundles and Windows CI.
- Publish complete GitHub Releases automatically from matching version tags after
  CI passes; version suffixes mark prereleases and reruns preserve published assets.
- Preserve the known limitation: music loss/crashes appear fixed in observed use, but are not proven fixed in all cases.
