# Characters and Customization

MGO 3's avatar system has a page of its own —
[MGO Avatar and Gear](MGO-Avatar-and-Gear) — because it is the deepest of the
three. This page is the general shape: where characters live, how they are
named, how form variation works, and where Survive differs.

## 1. Character asset layout

Characters are organised by short code under `/Assets/tpp/chara/<code>/`. The
largest:

| code | entries | who |
|------|---------|-----|
| `avm` | 5,272 | the MGO avatar system (the largest collection in the game) |
| `sna` | 1,094 | Snake |
| `dds` | 573 | Diamond Dogs soldiers |
| `qui` | 484 | Quiet |
| `svs` | 416 | — |
| `pfs` | 354 | — |
| `dct` | 277 | Code Talker |
| `kaz` | 276 | Kaz |
| `ptn` | 270 | — |
| `hrs` | 269 | horse |
| `chd` | 269 | child soldiers |

Each folder has the same three subfolders:

```
Fox_files/        model, rig and asset definitions
Fox_files/.fpklp/ packed-list variants
Pictures/         textures
Scenes/           assembled part / scene definitions
```

`Scenes/` is where the assembled variants live — the different outfit states.

## 2. Model naming

`<char><n>_<part><n>_<variant>`:

```
sna2_main0_def      Snake, main body, default
sna0_main0_def      Snake, alternate suit
avf0_body0_def      MGO avatar, female, body part 0
avm1_type7_def      MGO avatar, male, head/face preset 7
bsf0_main0_def      Survive, female player base body
skl0_main0_def      first-person arms (TPP) — see the trap below
hat13_main0_def_f   headgear, female fit
```

Part tokens: `main`, `body`, `type`, `arm`, `bdn`, `hair`, `hone`, `cnt`,
`wmcs`, `pacth`, `sub`.
Variant tokens: `def`, `cov`, `v00`–`vNN`, `c00`, plus the fit suffix `_f`.

### Two naming traps

- **`skl0_main0_def` is the first-person arms model** in TPP — 1,378 vertices,
  no body geometry, and it passes every validation check. (In MGO the same stem
  is the never-drawn skeleton carrier. Same name, different thing, different
  game.)
- **`avf0_body0_def` is a torso part, not a character.** Rendered alone it is a
  floating shirt.

**The Phantom Pain names its character parts in the model stem**, which is why
a part list can be built from names alone there and cannot be in MGO, where the
authority is the gear tables.

## 3. FOVA — form variation

FOVA (`.fova`, code 4235) and FV2 (3089) produce visual variants without
duplicating geometry. `/Assets/tpp/fova/` holds 3,025 paths:

| subtree | entries |
|---|---|
| `chara` | 1,704 |
| `weapon` | 632 |
| `common_source` | 352 |
| `mecha` | 226 |
| `item` | 69 |
| `environ` | 40 |

A table does four things, and a reader that handles only the first will be
wrong in visible ways:

1. **Substitute textures** by (material, role) — the common case.
2. **Hide mesh groups** — a hide list. Rare: 8 of 807 tables in one measured
   set, all hiding one group.
3. **Show mesh groups** — a show list, which **wins over** its own hiding.
4. **Attach models** — a hat, a bag, a hair mesh. **478 of 1,895 shipped tables
   attach a model**, 477 of them bound to the wearer's own bones. A `.fv2` is
   not only a texture swap.

Two rules that follow from that:

- Keep a variation's hidden-group set **separate** from any the avatar look
  contributes, and combine them at draw time. One shared set cannot both
  survive a variation change and be undone by deselecting the variation.
- Hold the attached models with the variation, not in the general part list, so
  they live and die with it. Otherwise picking another variation leaves the
  last one's hat behind.

**Resolve a variation against the model's own table.** Material name hashes
repeat across models, so applying another model's `.fv2` substitutes the wrong
textures without failing in any visible way.

## 4. Where the tables live — two different layouts

| game | layout |
|---|---|
| TPP | beside the model, or in a sibling `fova/` folder |
| MGO | one central tree — see [MGO Avatar and Gear §4](MGO-Avatar-and-Gear) |

A tool that knows only the TPP layout silently finds nothing for every MGO
asset. This one cost a long time to notice, because "no variations" is a
plausible answer.

## 5. Survive's differences

Survive keeps TPP's formats while changing the structure around them:

- **A separate asset root** at `/Assets/ssd/` (10,091 paths), while still
  referencing the TPP tree.
- **Character subtrees organised by slot** — `arm`, `avm`, `base`, `body` … —
  rather than by character.
- **`SsdPlayer_layers`** as the player animation set, with `bsf0`/`bsm0` base
  bodies.
- **A shared animation system** with TPP: clip names and speeds match within
  1–3 %.
- **Gear colour is single-channel.** Survive's `GearConfig.lua` has 351 colour
  blocks with zero non-empty Primary/Secondary palettes — at most one
  `DefaultPrimary`. The two-channel per-item system is MGO's alone.

TPP and Survive assets mix if each is rigged against its own game's skeleton,
but cross-game assumptions fail.

**And the rule worth repeating:** Survive ships *different models under the
same stems*, and its copies can resolve under `/Assets/tpp/` paths. Judge which
game an asset belongs to by the **archive it came out of**, never by its path.
An index can decide an archive's game by a majority vote over the asset paths
its named entries resolve to — one install can hold four games and `chunk0.dat`
is a name three of them use.
