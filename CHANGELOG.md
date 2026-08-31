# Changelog

## 1.0.0 — first public release

The first release of FOX Asset Browser: a native C++17/Qt6/OpenGL browser,
viewer, character builder and extractor for Fox Engine games, with a modding
path that can put files back.

### Reading

- **SQAR/QAR archives, natively** — both the MGSV format and Survive's
  version-2 variant, with header and section-table decryption and both
  per-entry payload ciphers. `.g0s` for Ground Zeroes, with its own legacy hash
  scheme. Ported byte-for-byte from GzsTool.
- **The deep scan** — `.fpk`, `.fpkd` and `.pftxs` containers opened once,
  their children indexed with real stored paths, and the listing cached against
  every archive's size and mtime. 1,111,128 container children on a full TPP
  install that a top-level walk never sees.
- **Name resolution** — PathFileNameCode hashing against the community
  dictionaries, with the 13-bit extension-code table and the GZ legacy scheme.
  The implementation is verified against the hashes shipped inside PFTXS packs
  rather than against itself.
- **Mount priority** — archives sort by priority, so the copy the index hands
  out is the copy the game would load. Shadowed duplicates are reported.

### Viewing

- Textures: `.ftex` plus its streamed `.N.ftexs` mips reassembled into `.dds`,
  decoded on the CPU for preview, exported as DDS or PNG.
- Models: FMDL with the full material table, FRIG rig units, FRDV help bones,
  MTAR/GANI animation playback, submesh tree, connect points.
- Materials: technique, shader kind, texture set by role, and the per-material
  **shader parameter list**.
- A per-file dependency view in both directions, backed by a cached
  texture→model map.

### Composing

- **Customize** — build a character, a weapon, a vehicle, or compose free-form
  parts. Fixed slots driven by the games' own catalogues, with FOVA form
  variation, MGO 3 per-item two-channel gear colour, saved presets and
  undo/redo.
- Rest-alignment for parts whose bind pose disagrees with the skeleton they
  share, including socket-anchored parts carried on a connect point.
- Right-click a slot or an equipped row to act on that part; export the scene,
  one part, one slot, one variation, or one file per variation.
- Every export comes in a pair: one that asks where, and one that writes into
  the folder you last chose and says which.

### Extracting

- Bulk extract by path substring, extension or resolved-only, with container
  contents included and per-container grouping — an FPK with 200 matched files
  decompresses once, not 200 times.
- glTF `.glb` export: posed, or rigged with one animation per clip.
- Exports are byte-identical across runs, which is checked rather than assumed.

### Modding

- **A mod folder is a mount, not an edit.** Replacements sit in a folder
  mounted above every archive; nothing is ever written into a `.dat`, `.qar` or
  `.fpk`. Reverting is a delete, and deleting the folder is the uninstall.
- **DDS → FTEX** — a texture re-encodes into the original's exact layout, all
  streams installed atomically. Verified on 2,233 of 2,233 mips, with the chunk
  counts matched against the shipped files.
- **Packaging** — export the mod folder as a plain ZIP with a manifest, or as a
  real SnakeBite `.mgsv`. The metadata is byte-identical to what SnakeBite's
  own serialiser produces, verified by compiling its classes and running them
  against the file.

### Tooling

- A headless harness with a flag per measurement, and eleven double-clickable
  scripts that run them and collect the logs.
- `verify-src.py`, a source checker every change passes.
- Fully portable: settings, caches and logs in `data\` beside the exe.

### Known issues

- **No replaced asset has been loaded by the game yet**, and no packaged mod
  has been installed by SnakeBite. Both write paths are verified against this
  tool's own readers and against SnakeBite's own classes; the engine and the
  mod manager have not been asked.
- **Accessory placement in the composer is wrong for some items.** The metric
  that reported it as correct measures the part's root bone rather than its
  geometry. See the wiki's Open Questions.
- Eyebrows on MGO avatars resolve to the male set for both genders.
