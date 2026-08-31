# Open Questions

What is still unknown, and the specific next experiment for each. Anything that
moves off this page moves onto a page with a number attached.

---

## 1. Accessory placement — the metric was measuring the wrong thing

**Status: reproduced, mechanism unknown. Top of the list.**

Some accessories sit visibly off the character — measured at roughly half a
metre above the head for one MGO/Survive cap — at bind pose *and* posed.
Dropping the rest-alignment correction entirely drops them to the waist, so the
correction is doing something and is not the right amount.

**The check that said it was fixed cannot see it.** The residual is

```
landed   = the part's root bone world position, through its own root pose
target   = the anchor bone's bind world position
residual = |landed − target|
```

Both terms are **bone** positions, and it is computed through the same matrices
the correction was folded into. A wardrobe sweep of 145 items reported "116
seated, 0 floating" for a scene in which two accessories were plainly wrong.

**Next experiment:** a second residual that measures the **mesh** — the part's
posed bounding-box centre against the anchor bone — reported beside the bone
one. Then re-run the sweep on that metric and find out how many of the 145 are
actually wrong. The honest expectation is: more than zero. Only then chase the
mechanism, with numbers.

Do not delete the bone metric. It measures a real thing correctly; it is just
not the thing being complained about.

---

## 2. Does a form variation reach an exported file?

**Status: reasoned yes, measured inconclusive.**

The chain is short and unambiguous by reading — the variation writes the part's
texture list, and the exporter writes that same list. But exporting one weapon
with four different variations applied produced **four byte-identical files**,
and four identical *renders*, because every substitution in that table resolves
to a texture whose name is not in the available dictionaries and whose file is
not in the pull. The variation applied and substituted nothing.

**Next experiment:** on a full install, build a weapon, pick two camouflages,
export both, compare. One command.

---

## 3. Are the weapon camo rows redundant with gear colour?

**Status: mechanism established, redundancy not.**

`camo_cNN` tables substitute exactly one role — `Layer_Tex_SRGB` — which is the
same slot the gear-colour path rewrites. That is the right mechanism for the
claim "camouflages are already in gear colour".

But redundancy needs the two to point at the **same art**, and on the evidence
they do not: the 35 textures the camo tables substitute overlap **zero** with
`def`, `cam`, `clv` or `mNN`, four ways; and none of the 35 is any
`cm_camo3/4` or `cm_scol3/4` swatch under `/common_source/layer/Pictures/`
(checked by hash against 1,600 candidate names — 0 matches).

Their real paths are not in the available dictionaries, so they cannot be named
without a fuller install.

**Next experiment:** a fova census on a full install, filtered to `camo_c=`,
and look at the resolved `filePath` column. If they are `cm_camo4_cNN`
swatches, the camo section really is redundant. If they are per-weapon
textures, it is not.

---

## 4. Has anything written by this tool been loaded by the game?

**Status: no. This is the largest untested claim in the project.**

The FTEX writer matches the shipped chunking on 2,233 of 2,233 mips and the
`.mgsv` metadata round-trips through SnakeBite's own compiled classes with
every install gate passing. Neither the engine nor the mod manager has been
asked.

**Next experiment:** replace one texture, launch the game, look. Then package
one mod, install it with SnakeBite, look.

---

## 5. The FPK writer

**Status: not started, precisely scoped.**

A `.mgsv` cannot carry an asset the game keeps inside an `.fpk` unless the
packager can rebuild that container. Everything else about `.mgsv` is settled;
this is the one remaining piece, and it also unblocks replacing the large
category of assets that live in containers.

---

## 6. `ContentHash` against a shipped MD5

**Status: the check is written and has never had data to run on.**

An FPK entry carries a **16-byte MD5 that Fox itself wrote** for each child —
the perfect independent check that a `ContentHash` pipeline agrees with the
engine's. Both `.fpk` files in the available reference pull are 128-byte stubs
with zero entries.

**Next experiment:** run it on a full install. It is a loop over every FPK
child comparing our MD5 against the stored one.

---

## 7. Eyebrows are gender-blind

**Status: understood, unfixed.**

The brow prefix resolves once, preferring `avm`, so a female MGO avatar renders
male eyebrows although 623 `avf_ebrw*` files ship. The genders ship different
counts (33 `avm`, 27 `avf`), so the fix moves list indices and touches the
preset mapping. It is a real defect with a known cause and a non-trivial blast
radius.

---

## 8. Novelty headgear that roots oddly

`hat4/11/12/13/17/18` root at `SKL_004_HEAD` or `SKL_002_CHEST` rather than the
`SKL_000_ROOT` the other twenty use, and look wrong in renders. Unconfirmed
whether that is the same bug as §1 or a separate one. Likely to be answered by
§1's mesh-space metric.

---

## 9. A fragment with a second root the host does not carry

The borrow path has a fallback for a fragment whose bones do not all resolve.
**No instance exists in 417 models** — every one has exactly one root — so the
fallback is reasoned and unmeasured. It may be dead code, or it may be the
thing that saves a model nobody has loaded yet.

---

## 10. From the predecessor, still open

- **The motion graph.** `/Assets/tpp/motion/motion_graph/` is indexed and its
  format is not decoded. It is the largest single unknown in the animation
  system: the clips are understood, the machine that sequences them is not.
- **`.fsop` compiled shaders.** Indexed, not decoded. Fox_Parser reaches
  further here than either of these projects.
- **The animation-binding threshold on a full corpus.** The empty band from 0.4
  to 0.9 was measured over 108 archives. A full install has around 508.
