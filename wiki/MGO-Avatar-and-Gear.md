# MGO Avatar and Gear

The deepest part of this handbook, because it is the part that is not written
down anywhere else. **Metal Gear Online 3** builds a player out of
interchangeable parts on a shared skeleton, and every rule about which part,
which colour and which combination lives in tables the game ships. This page is
what those tables say.

Everything here was measured against a real install's `mgo\chunk0.dat` and
`texture0.dat`. Where an earlier reading of ours was wrong, the correction is
left visible and marked.

---

## 1. The skeleton nobody sees

```
/Assets/mgo/chara/base/Scenes/skl0_main0_def.fmdl     486 bones  (male)
/Assets/mgo/chara/base/Scenes/skl0_main0_def_f.fmdl   489 bones  (female)
```

Each with its own `.fcnp` and `.frdv`. **This is the model MGO poses and never
draws.** Everything visible on an avatar is a gear part stacked on it, exactly
as Survive stacks parts on `bsm0`/`bsf0`.

It does not ship as a top-level archive entry — it lives inside the FPK
`/Assets/mgo/pack/collectible/common/col_common_mgo` (936 members), which is
why a name-table pull for it comes back empty and looks like a missing file.

The bare bodies `avm0_body0_def` / `avf0_body0_def` are a stand-in base for an
install without the base pack. The avatar has **no outfit list** of its own;
the DLC suits' `_main0` fits are Snake's.

## 2. Heads and hair

```
Female heads   avf0_type0..type7            8   (differ only by eye shape)
Male heads     avm0_type0..7, avm1_type0..7  16
Horn           avm_hone_v00..v02_cov         3
```

The MGO chunk's avatar packs (`/Assets/mgo/pack/player/avatar/face`) carry
`avf0_type0..7` **and** MGO's own copies of `avm0_type0..7`, all resolving at
`/Assets/mgo/chara/avm/Scenes/`. A female page must use `avf0_type*` and
`avf_hair_*`, not the male's.

**Hair ships twice, and that is the game's answer to hair clipping.** Every
style exists plain and in a COVERED form flattened to sit under headgear:

```
<style>_v00   ↔  <style>_v0_cov
<style>_v01   ↔  <style>_v1_cov
```

Counted over the shipped names: **95 `_v00`, 6 `_v01`, 146 `_v0_cov`, 6
`_v1_cov`**. An earlier revision of these notes listed only the `_cov` forms,
because the extract being measured carried only those — that was the extract,
not the game. Fold each pair into one row and swap models when a headgear slot
fills.

**There is no per-hat mesh-hiding rule in this data.** Every mechanism the
format has was checked, and the absence is the result:

- **FV2 hide/show lists** — 8 of 807 shipped tables carry a hide list, and all
  eight hide the same single group (`0xa9e88501`) on the male head: the
  bandanna. Not one table carries a show list. No headgear item hides anything.
- **DataSet `invisibleMeshNames`** — appears in two files in the entire set,
  both the avatar BODY, and only as a property name.
- **GearConfig.lua** — 24 distinct fields, none about a mesh, hair, hiding or
  visibility.

So do not go looking for one. **The model swap is the mechanism.**

Every head carries a `MESH_bdn_IV` bandanna submesh that stays hidden until the
bandanna item (`hat21_main0_def{_f}`, id `hat_?21`) is equipped, at which point
both the submesh and that model show.

### The beard and brow trap

On MGO's own `avm0_type0_def` (13 materials):

```
mat  8   1401 verts   the face
mat 11    106 verts   brows   Base = cm_flat_white,  nrm/spec = flat placeholders
mat 12    559 verts   beard   Base = cm_flat_white,
                              Translucent_Tex_LIN -> .../Pictures/berd/avm_berd0_a0_trm
```

The only authored `/berd/` path on this head is on the beard's **translucent**
slot. A `/berd/`-on-Base_Tex scan therefore finds nothing — which is why every
Facial Hair choice, clean-shaven included, did nothing on the MGO male.
Identify these two materials by a `/berd/` or `/ebrw/` path in **any** texture
role, never on Base_Tex alone. TPP's own copy of the head *does* bind a real
default beard on Base_Tex; both readings are true of their own data.

**[open] Eyebrows are gender-blind and it is a real defect.** The brow prefix
is resolved once, preferring `avm`, so a female avatar renders `avm_ebrw0_*`
even though 623 `avf_ebrw*` files ship. The genders also ship different counts
— 33 `avm`, 27 `avf` — so fixing it moves the brow list's indices and touches
the preset mapping.

## 3. Skin inside clothing

Nine garments per gender carry a material the artists named `color_skin` — the
fatigues, the BDU and T-shirt, and the three short-sleeved suits (`ins1`,
`res1`, `tes1`); `gls5` and hats 20/22/23 also carry a Skin-shader material.

The test that picks exactly those, across MGO's 195 character models: **a skin
material binds a Translucent map and NO Layer pair; cloth binds the Layer
pair.**

**[scar] Never run clothing through a head's material overrides.** That code
path has eyebrow fallbacks that claim any material binding exactly base +
normal + specular and then paint the eyebrow atlas onto it — or hide its mesh
group. Measured: **31** of MGO's garment and hat materials are that shape, one
of them on the fatigues.

## 4. Where MGO keeps its FOVA tables

**MGO does not put a variation table beside its model.** It keeps one tree:

```
/Assets/mgo/fova/weapon/default/<stem>.fv2                   the plain table
/Assets/mgo/fova/weapon/<class>/<stem>_cam.fv2               camo / clive
/Assets/mgo/fova/weapon/dlc_specialColor/m01_gold/<stem>_m01.fv2
                        …/m02_silver/  …/m03_copper/  …/m68_lightBlue/
/Assets/mgo/fova/chara/{head,hat,chest,body,eyes}/<id>.fv2
```

A tool that looks in the model's own folder and a sibling `fova/` — the **TPP**
layout — finds nothing for any MGO asset and reports "no variation tables",
for a weapon that ships six.

### The join is TWO naming rules, unioned

```
ar00_owep0_def.fmdl  ->  ar00_owep0_def{,_cam,_clv}.fv2
    table = the model's FULL stem plus a variation suffix
    (variant-stem test alone: 1 of 3)

gl02_owep0_def.fmdl  ->  gl02_owep0_{def,m01,m02,m03,m68}.fv2
    the DLC colour packs share only the VARIANT stem
    (full-stem test alone: 1 of 5)
```

**Do not join on hashes alone.** `ar00`'s table ties with **thirty** other
models on material-name hashes, because material name hashes repeat across
weapons. The hash test says a table is *possible*; the naming join says
*which*. Verified exact on twelve models — 6/6, 7/7, 2/2, 5/5, 5/5, 4/4, 4/4,
5/5, 0/0 — with no cross-contamination.

### What a weapon camo table actually substitutes

Measured over 181 shipped MGO weapon `.fv2` tables, 1,445 substitutions:

| family | tables | subs | distinct target textures | material roles |
|---|---|---|---|---|
| `def` | 25 | 243 | 121 | 6 |
| `cam` | 4 | 24 | 19 | 5 |
| `clv` | 4 | 25 | 19 | 6 |
| `mNN` | 113 | 1013 | 134 | 6 |
| `camo_cNN` | 35 | 140 | 35 | **1** |

`cam` and `clv` substitute **disjoint** texture sets — 15 each the other never
touches — and one role decides it: **`clv` writes `Layer_Tex_SRGB` and `cam`
never does.** So `cam` is the camouflage-*capable* base, deliberately leaving
the layer slot free for a `camo_cNN` table to fill, and `clv` fills it itself.

MODEL VARIATION holds **seven** names, not two: `def`, `cam`, `clv`, and the
DLC special colours `m01` (gold), `m02` (silver), `m03` (copper), `m68`
(light blue) — 113 of the 146 rows in that section. And only **4 stems of 35**
ship `cam` or `clv` at all.

## 5. The id → model join: the game ships it, so read it

**This supersedes any "ordinal join" reading.** The install's MGO chunk carries
one fova pack per gear item id:

```
/Assets/mgo/pack/player/fova/<id>.fpk
  └─ /Assets/mgo/fova/chara/<dir>/<id>.fv2      dir = head|hat|chest|body|eyes
```

and that `.fv2`'s external-file table **names the exact model the item wears**.
(Eyewear names the model's `.frdv`, which sits in the same `Scenes/` directory
and therefore shares the model's path hash.) Decoded for all 175 item ids.

What the game's own join says, where earlier readings guessed:

```
inh/reh/teh, rec : id index = model index, main0
hat_?NN -> hatNN, but hat5 and hat9 are main1
eye_?NN -> glsNN (index preserved), gls0 is main1
inc -> inf, tec -> tec; male fits of index 0/1 end "_def0", and the
    shipped inf0/inf1 files are capitalised "Inf0_/Inf1_"
cms_?01/02 -> cmn0/cmn1_main0    (the common SUIT — never tcl)
cmc_?NN -> the cmn chest piece:
    cmc_m01 -> cmn1_chst1,  cmc_f01 -> cmn0_chst0,  cmc_f02 -> cmn1_chst1
    (the two genders read the number differently)
inb/reb/teb -> icl/rcl/tcl, ids 00..02 -> model 1, id 03 -> model 0
    — NEITHER numeric NOR ordinal-ascending.
    CORRECTION: earlier notes said "inb_f00 -> icl0" and
    "teb_f00 -> tcl0 (the BDU)". Both are WRONG. Measured:
    inb_f00 -> icl1, teb_f00 -> tcl1, and the BDU is teb_?03.
ins/res/tes : _?00 -> <fam>0_main0, _?01 -> the mask0 piece
    (helm0 for tes), _?02 -> <fam>1_main0
```

Female fits: an `_f` id names the `_f` model. The female table's `_m`-id hats
(`hat_m05 06 07 08 09 11 12 14`) name the **plain** model — including
`hat_m07`, whose `hat7_main0_def_f` re-fit ships but is referenced by no table
record at all. `hat_f07` does not exist in the file; that model and its
`hat_f07.fv2` are orphaned content. **The table is the authority.**

## 6. The gear tables

`GearConfig.lua` — 416,603 bytes at
`/Assets/mgo/level_asset/config/GearConfig.lua` — keys its Gears section by
**class first** (`Infil`, `Recon`, `Tech`), so Headgear / Base / Chest each
appear three times per gender with heavy overlap. Merged and de-duplicated per
slot:

| slot | items | models |
|---|---|---|
| Headgear | 42 | `inh0-4`, `reh0-4`, `teh0-4`, `ins0_mask0`, `res0_mask0`, `tes0_helm0`, `hat0-23` = 42 |
| Chest | 25 M / 26 F | `inf0-7`, `rec0-7`, `tec0-7` (24) + `cmc`: 1 M / 2 F |
| Base | 14 | `icl0-1`, `rcl0-1`, `tcl0-1`, `ins0-1_main0`, `res0-1_main0`, `tes0-1_main0`, `cmn0-1_main0` = 14 |
| Accessory | 6 | `gls0_main1`, `gls1-5_main0` = 6 |
| **total** | **87 M / 88 F** | |

Both sides match exactly, per gender, per slot. Any shortfall on a full install
is a defect, not a data limit.

**[trap] `_probe\GearConfig.lua` is Survive's, not MGO's.** It declares
`SsdGearConfig`, not `MgoGearConfig`. Anything loading it expecting MGO gear
gets an avatar with no slots. Survive ships a 53 KB file of the same name.

### Every record field

Counted over the 167 item records:

```
Color 203   Primary 191   Secondary 187   Ascension 179
Active / ID / PurchaseID / NameLangTag / DescLangTag / Swatch /
  Requirements / Level / MasterAscension / CurrencyType / Price   167
DefaultPrimary 139   Exclude 85   DefaultSecondary 78   DLC 29
ForceExclude 25   FullSuit 18   Must 12   BaseGearID 3
RevertToDefaultBase 3
```

**There is no field about meshes, hair, hiding or visibility.**

## 7. Two-piece garments: one garment, two rows

Three Chest records carry **`BaseGearID`** naming a Base record, plus
**`RevertToDefaultBase=1`**; each pair names the other in **`Must`** and shares
a `NameLangTag`:

```
cms_f01 (Base) ↔ cmc_f01 (Chest)   palettes 1 + 1  ->  2 dye rows
cms_f02 (Base) ↔ cmc_f02 (Chest)   palettes 1 + 2  ->  3 dye rows
cms_m02 (Base) ↔ cmc_m01 (Chest)   palettes 1 + 2  ->  3 dye rows
```

`cms_m01` is a standalone Base on the male — the man's fatigues have no chest
half. **Fold on `BaseGearID`, never on names.** The chest row must not appear
in the Chest list on its own, and each half keeps its own two dye channels.

## 8. Exclusions and requirements: three fields, none symmetric

- **`Exclude={…}`** — 99 records. **`ForceExclude={…}`** — 27 records (the BDU
  `teb_?03` force-excludes the `cmn` chest pieces `cmc_?0N`). Both mean "not
  worn together"; union them.
  **Beware:** `"Exclude={"` is a substring of `"ForceExclude={"`. An unanchored
  search reads the wrong one.
- The lists are **one-sided in the data** — `inc_m00` names `cms_m01`;
  `cms_m01` names no `inc` id — so enforcement has to check both directions.
- **`Must={…}`** — 12 records, the mirror: cannot be worn *without*. Six are
  the two-piece pairs above (mutual). The other six are one-directional: the
  suit MASKS `ins_?01`, `res_?01`, `tes_?01` each require their suit BODY
  `<fam>_?00`. The body does not require the mask.
- **`FullSuit=1`** marks 18 records, nine per gender. No id appears in more
  than one category; a suit's head and body pieces are separate entries whose
  Exclude lists clear each other.

## 9. Colour: per item, two channels, applied as a fova

Every gear item's `Color` block carries `DefaultPrimary`, `DefaultSecondary`,
`Primary[]`, `Secondary[]` — **its own palette**. The BDU's Primary is about 60
`com_c`/`com_m` ids; its Secondary is just `teb_c06/07/08`.

**Each colour id is itself a fova:**

```
/Assets/mgo/fova/chara/{common,body,head,chest}/<colourId>.fv2
```

whose material substitutions — with the textures in the same pack's `.pftxs` —
*are* the dye. Primary ids rewrite the shared camouflage layer; Secondary ids
the family's own materials. Applying a colour means applying that fv2's
overrides to that one part.

**Colours have no names, and never did.** All **367** colour records carry an
empty `NameLangTag`. No language table can turn `com_c24` into words, because
the game never had a word for it. What the data does carry is
`About={ColorType=…}`: **282 Solid, 85 Pattern, 0 untyped** — "Pattern" is the
camo. **Seven colour ids are declared twice**, so count types from the map
after parsing, never per record; counting per record gives 374 types for 367
ids and an "untyped" figure of minus seven.

**Correction.** An earlier version of these notes claimed the two-channel
system held "for MGO and for Survive both". Survive's own `GearConfig.lua`
measures at 351 colour blocks with **zero** non-empty Primary/Secondary
palettes — its items carry a single `DefaultPrimary` (`cm_c0NN`) at most.
Per-item two-channel colour is MGO's system alone.

## 10. Icons and names

**Every item and every colour names its own icon**, in the record's `Swatch`
field:

```
/Assets/mgo/ui/texture/EquipIcon/gear/<name>_alp.ftex
/Assets/mgo/ui/texture/EquipIcon/color/<id>.ftex
```

Coverage: **175/175 gear items name an icon and 175 decode; 367/367 colours
decode.**

**[trap] GearConfig writes the path WITH `.ftex`.** If your lookup appends
`.ftex` itself, every request asks for `….ftex.ftex` and silently finds
nothing. Strip the extension once, at parse.

### The `.lng2` extension gap

A path hash splits at the **first** dot, so a language table `<stem>.eng.lng2`
has extension text `eng.lng2`. An extension absent from the known-extension
table hashes to **type id 0**, the file resolves as `<path>._unknown`, it is
never marked as named, and a name catalogue that skips unnamed files never sees
a single table. `lng2` and all eight `<language>.lng2` spellings have to be in
the table explicitly.

Ground Zeroes uses the other form (`.lng#eng`) and resolves through the legacy
path instead.

## 11. Defaults, and named skins

From GearConfig's own `Defaults` blocks:

```
Infil   Base = inb_?03 (icl0)   colours com_c20 + inb_c07
Recon   Base = reb_?03 (rcl0)   colours com_c09 + reb_c06
Tech    Base = teb_?03 (tcl0)   colours com_c10 + teb_c08     <- the BDU
```

Base is the BDU (`tcl0_main0_def{_f}`) for both genders; no other slot
defaults. Face is the first head — on MGO that is a plain model list, not a
preset grid (`avatar_presets_women` is **Survive's** table, not MGO's).

Named character skins are complete characters with no slots at all:
`oce0_main1_def` (Ocelot), `qui0_main0_mgo` (Quiet), `sna0_main4_def` (Snake).
