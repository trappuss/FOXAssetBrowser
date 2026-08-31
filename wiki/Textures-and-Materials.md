# Textures and Materials

## 1. FTEX and the streamed-mip problem

Fox textures are `.ftex` containers (code 685) that hold the *low* mips inline
and keep the high-resolution ones in separate `.ftexs` companion files. TPP
ships over 114,000 pairs.

Read the `.ftex` alone and you get whatever low mips it happens to hold — often
512 px or less for an asset authored at 2048. The companions are frequently in
a different archive entirely (the texture archives), found by hash rather than
by sitting beside the model.

**The mip order across streams is by ftexs file number DESCENDING.** A texture
is `.ftex` + `.1.ftexs` + `.2.ftexs` + `.3.ftexs`, and reassembling them in the
wrong order produces a plausible-looking image with the wrong detail levels.

## 2. The map-suffix vocabulary

Texture roles are carried in the file-name suffix:

| Suffix | Count | Role |
|--------|-------|------|
| `_bsm` | 14,159 | base / albedo |
| `_nrm` | 12,361 | normal |
| `_alp` | 10,375 | alpha |
| `_srm` | 9,822 | specular / roughness (see §5) |
| `_hnm` | 872 | height / detail normal |
| `_mtm` | 806 | material mask |
| `_lym` | — | layer mask |
| `_trm` | — | translucency |

Most exporters carry base and normal only; the rest have to be re-associated by
name or through a sidecar.

## 3. Normal maps are DXT5nm

Two-channel encoding: **X in alpha, Y in green, Z reconstructed.** Correctly
decoded normals show mean RGB near (132, 127, 132) with alpha ≈ 126 — a useful
sanity check on a whole folder at once.

## 4. Compression, and a decode pitfall

Every character texture observed is DXT1 (BC1) or DXT5 (BC3), with complete mip
chains, 128² to 2048².

Uploading compressed mip chains straight to the GPU can take out the driver.
Decoding BC1/BC3 on the CPU is about 150 lines and is reliable everywhere; that
is what this browser does for preview.

## 5. SRM: one texture, three channels — and there is no metalness map

`SpecularMap_Tex_LIN` (`0x6b98b10e`) carries:

- **R = ambient occlusion**
- **G = roughness**
- **B = reflection mask**

"Specular" and "roughness" are not two missing files. They are one file.

**Metalness has no map in Fox and never will.** F0 comes from the FMTT preset
the material selects. "No metalness map" is the format working as designed, not
an extraction failure.

Some materials name an SRM that no archive holds — a dangling reference the
engine substitutes at runtime. A tool may substitute too, but **only** when the
material's own reference leads nowhere: no specular slot, a hash no archive
resolves, or a `common_source/flat` placeholder. A material naming a real map
keeps it. Gear models name their own SRMs and always did; `tcl0` (the BDU)
declares no specular slot at all, and that is authentic.

**Test the hash, never the path text.** On an install without the dictionary
every declared path reads back empty, so a string comparison cannot tell a
material that names a real map from one that names nothing.

## 6. Materials and shader parameters

Material definitions live in `.fmtt` (164) and compiled shaders in `.fsop`
(1439). Each material instance in an FMDL carries:

- a **technique** name (`fox3DDF_Skin_Tension_Dirty`, …)
- a **material name** (`TENSION_LTHIGH`, …)
- a **texture set** by role
- a **parameter list** — `MatParamIndex_0`, `Incidence_*`, `Tension*`, `Edge*`

The technique name is what tells you a surface is skin, cloth, eye or hair, and
the parameters are what drive multi-material blending and rim terms. They are
worth showing to a person: it is the difference between "this looks wrong" and
"this material's sub-normal blend is zero on purpose".

## 7. PFTXS

`.pftxs` (5727) packs many textures into one file — a list of FTEX groups, each
holding the `.ftex` plus its numbered stream files, all hash-named. It is also
the most useful independent record of the hashing scheme in the whole format;
see [Archives and Hashing §2](Archives-and-Hashing).

## 8. Writing an FTEX back

Round-tripping a texture is the one write path that is fully worked out, and
the rules came from a census of 283 shipped `.ftex`, 2,795 mips and 4,293
chunks rather than from the format documentation:

| Fact | Measured |
|---|---|
| pixel formats in the sample | DXT1 ×283 |
| `.ftexs` stream counts | 1 ×1, 2 ×2, 3 ×280 |
| `chunkCount == 0` | **zero occurrences in 2,795 mips** |
| every non-last chunk | exactly **16,384 bytes** — 2,060 of 2,060 |
| chunks stored rather than deflated | 801 of 4,293 — a normal case, not a failure |
| index-record offset form | 3,514 plain, 779 using the `0x80000000` variant |

So: **every mip a writer emits should be chunked**, even though the reader has
a simpler `chunkCount == 0` branch. That branch is a shape the engine has never
been asked to read, and for a file the game loads at runtime that is the
difference between a mod and a crash. `chunkCount = ceil(size / 16384)`; a
chunk is compressed or stored, whichever is smaller, and the reader's own test
is literally `compressedSize != chunkSize`.

The safest writer keeps the original's 64-byte header **verbatim** and each
mip's stream number, rewriting only the payload and the four fields that say
where it sits — and refuses a format change, a size change, or a DDS with too
few mips. Every difference from the shipped file is a way for the game to
reject it.

### The acceptance test that actually works

A whole-file round trip needs every `.N.ftexs` present, which in a partial pull
covers four textures. Four is not a corpus, and a writer that is right about
2,700 mips and wrong about the 4×4 at the end of the chain looks perfect on any
single texture anybody thought to try.

Test **per mip**, and compare the chunk COUNT against the shipped file as well
as round-tripping the bytes:

```
2233 mip(s) re-chunked and read back — 2233 IDENTICAL, 0 different
chunk COUNT vs the shipped file      — 2233 match, 0 differ
```

The second line does work the first cannot, and that is demonstrated rather
than asserted. Setting the chunk size to 8,192 and re-running:

```
2233 mip(s) re-chunked and read back — 2233 IDENTICAL   <- still green
chunk COUNT vs the shipped file      — 1697 match, 536 differ   <- caught it
```

A self-consistency test cannot see a wrong constant, because the reader and the
writer are wrong together. Comparing against the count the game itself shipped
can.

## 9. A texture is not one file

Because a texture is `.ftex` plus N `.ftexs`, **installing one is not one
copy**. Write the whole set or none of it: three of four leaves a texture
mounted with mismatched mips — garbage on screen, out of a mod folder that
looks correctly populated.
