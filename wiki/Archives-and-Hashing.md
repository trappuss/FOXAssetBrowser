# Archives and Hashing

## 1. The archives

Game data lives in large archive files under `master/`:

- **`.dat`** — QAR/SQAR archives (`00.dat`, `01.dat`, `chunk0.dat`, `data1.dat`)
- **`.g0s`** — the Ground Zeroes equivalent, with its own hash scheme
- **`.qar`** — the same container format under another name
- **texture archives** — separate files holding the streamed high-resolution mips

Nested inside those are containers:

- **`.fpk` / `.fpkd`** — packed asset bundles (18,336 and 18,268 in TPP)
- **`.pftxs`** — packed texture archives (4,144 in TPP)

Fox also mounts numbered `master\<N>\00.dat` / `01.dat` slots. All four names
appear in `mgsvtpp.exe`, and mod managers put replacement assets in the
numbered slots — which is why **mount priority is a real property**, not an
implementation detail. Two archives can carry the same hash; the higher
priority is the copy the game loads.

## 2. Names are hashes

Fox Engine stores files as 64-bit **PathFileNameCode** values, not paths:

```
63          51                                              0
+-----------+-----------------------------------------------+
| ext code  |        low 51 bits of CityHash64(path)         |
+-----------+-----------------------------------------------+
     13 bits                     51 bits
```

The path is lowercased with the extension stripped from the **first** `.`, so
`foo.1.ftexs` hashes on `foo` with extension text `1.ftexs`. A flag bit
(`0x4000000000000`) marks paths outside `/Assets/`.

**Consequence:** an archive cannot be listed by name — only asked whether a
hash exists. Recovering a name needs a dictionary.

### The hash, verified against shipped bytes

Every tool in this space says its hash is "a port of GzsTool's `Hashing.cs`".
That is a claim about provenance, not a measurement, and checking it against
your own reader is circular — the reader and the writer would be wrong
together.

**A PFTXS pack is not circular.** It stores the full 64-bit code of every
texture it holds, written by Konami. Run our hash against those:

```
4 packs: 17 group hashes, 34 sub-entry hashes
group hashes whose top 13 bits are our 'ftex' extension code  : 17 of 17
sub-entry extension codes  0x2ad x17 -> ftex   0x1658 x17 -> 1.ftexs
426,756 dictionary lines hashed
names that reproduce a SHIPPED pack hash                      : 3 exact

  /Assets/tpp/effect/vfx_pic/dust/fx_dstrocblr02_ks_alp_clp.ftex
      -> 0x1569cbada656ffca      (the pack's own first group hash)
  /Assets/tpp/effect/vfx_pic/flare/fx_flr01_tm_alp_clp.ftex
  /Assets/tpp/common_source/flat/cm_flat_nrm.ftex
```

Both halves check out: the 13-bit extension code on 34 of 34 entries, and the
full 64 bits reproduced **from a name** on three of them. Only three resolve
because that test used a subset of the dictionaries; three exact 64-bit matches
is not a coincidence anyone needs to argue about.

Each PFTXS group is a `.ftex` plus its streams, which is why the sub-entry
codes split exactly 17 `ftex` + 17 `1.ftexs`.

## 3. Extension codes

Common codes in TPP and Survive (stable between the two games):

| Code | Ext | Code | Ext | Code | Ext |
|------|-----|------|-----|------|-----|
| 71 | gskl | 2609 | fox2 | 5533 | xml |
| 239 | qar | 2629 | fpk | 5719 | txt |
| 685 | ftex | 3131 | fsm | 5727 | pftxs |
| 783 | lani | 3296 | mtar | 5785 | fclo |
| 796 | lua | 3527 | spch | 5980 | sbp |
| 1172 | geom | 3609 | json | 6407 | sani |
| 1591 | fox | 3832 | subp | 6588 | frdv |
| 1682 | sim | 4235 | fova | 6589 | lng |
| 1752 | bnk | 4244 | fmdl | 6686 | aig |
| 2276 | frig | 4752 | mog | 7164 | htre |
| 2311 | aib | 5180 | nta | 7189 | parts |
| 2481 | vfxdata | 5387 | clo | 7314 | tgt |
| 3089 | fv2 | 5527 | ph | 7347 | ftexs |
| 164 | fmtt | | | 7359 | gpfp |
| 1439 | fsop | | | 7415 | fsml |
| | | | | 7594 | fpkd |
| | | | | 7684 | nav2 |
| | | | | 7741 | lba |
| | | | | 8069 | mas |
| | | | | 8074 | gani |

Streamed mip levels get their own codes — `0x2ad` = `ftex`, `0x1658` =
`1.ftexs`, `0x16ae` = `2.ftexs`, `0xae3` = `3.ftexs`, all measured above.

**Ground Zeroes encodes differently.** Its observed codes are small and
near-sequential — an enumerated table, not a hash. A `.g0s` is Ground Zeroes by
construction; nothing else ships that format.

## 4. The trap: containers hide everything interesting

Files inside FPK containers carry no meaningful extension code at top level —
a naive walk reports code `0`.

TPP extension histogram, top level only:

| Code | Extension | Count |
|------|-----------|-------|
| 0 | (container-internal) | 305,065 |
| 685 | ftex | 114,192 |
| 5720 | .1.ftexs | 114,192 |
| 2629 | fpk | 18,336 |
| 7594 | fpkd | 18,268 |
| 5727 | pftxs | 4,144 |
| 3131 | fsm | 239 |
| 8074 | gani | 1 |

19,138 models and 1,236 animation sets in TPP appear **absent** from that list.
They are all one level down. A deep scan that opens each container once and
records its child listing finds **1,111,128** children on a full install.

Two things make the deep pass tractable: FPK entries carry real **path
strings**, so container contents need no dictionary at all; and the listing
caches cleanly, keyed on every archive's `(path, size, mtime)`.

**A loose folder must NOT be container-scanned**, and this is a correctness
rule rather than an optimisation. A loose mount reads a file whole, with no
child step — so enumerating an `.fpk`'s children there would index entries that
all read back as the *container's* bytes, and because a loose mount outranks
the archives, those bogus children would win the hash lookup against the real
assets. Nothing is lost: a loose folder is extracted assets, so whatever is
inside an `.fpk` there is already sitting beside it as a real file.

## 5. Dictionaries

| File | Lines | Purpose |
|------|-------|---------|
| `qar_dictionary.txt` | 388,376 | asset paths (29 MB) |
| `mtar_dictionary.txt` | — | animation archive and clip names |
| `fmdl_dictionary.txt` | 20,659 | model-internal strings, bone names |
| `gzs_dictionary.txt` | 4,703 | Ground Zeroes paths |
| `fpk_dictionary.txt` | 2,062 | FPK-internal names |
| `bone_dictionary.txt` | 561 | skeleton bone names |

kapuragu's **mgsv-lookup-strings** is the fuller corpus, organised by tool and
data type.

**Limitations that bite:**

- Coverage is partial — roughly a third of textures stay unnamed.
- A dictionary entry is a *known path*, not proof the file shipped.
- **Different hash types need different dictionaries.** `PathFileNameCode`
  addresses files; `StrCode32` / `StrCode64` hash strings *inside* files — bone
  names, parameter names, node names. A dictionary is only useful against the
  scheme it was built for.
- Every declared texture path reads back empty on an install without the
  dictionary, so **test the hash, never the path text**. A material that names
  a real map and a material that names nothing look identical if you compare
  strings.

## 6. Asset tree layout

TPP by volume:

| Path | Entries |
|------|---------|
| `/Assets/tpp/level` | 106,440 |
| `/Assets/tpp/environ` | 59,406 |
| `/Assets/tpp/ui` | 36,550 |
| `/Assets/tpp/sound` | 30,417 |
| `/Assets/tpp/motion` | 29,443 |
| `/Assets/tpp/pack` | 16,924 |
| `/Assets/tpp/common_source` | 15,995 |
| `/Assets/tpp/chara` | 13,255 |
| `/Assets/tpp/fova` | 3,025 |

Top-level trees: `tpp` (338,791), `tpptest` (31,237), `ssd` (10,091),
`foxtest` (3,824), `mgo` (2,444), `sh` (1,059), `fox` (652).

```
/Assets/tpp/chara/<code>/Fox_files/     models and rigs
/Assets/tpp/chara/<code>/Pictures/      textures
/Assets/tpp/chara/<code>/Scenes/        assembled part / scene definitions

/Assets/tpp/motion/mtar/<group>/<set>          animation archives
/Assets/tpp/motion/motion_graph/<group>/<set>  motion graphs
```

## 7. What a mounted `mgo\` folder actually holds

Read straight from the entry tables of the MGO chunk with a throwaway SQAR
reader — `chunk0.dat` (3,814 entries) and `texture0.dat` (19,977):

```
chunk0    fpk 1363   fpkd 1363   pftxs 847   lua 146   sbp 79
          fmtt 2     fsop 4      ffnt 7      bnk 1     dat 1   gani 1
texture0  ftex 1758  1.ftexs 1758   + two further ftexs streams
```

**There is no `.lng2` anywhere in either file.** The language tables live in
TPP's own `chunk*.dat`, one level up from `mgo\`. An install configured with
only `MGS_TPP\mgo\` can never show an in-game name — which is a configuration
fact worth saying out loud rather than a bug to hunt.

**A warning about partial copies.** A working copy of `mgo_chunk0.dat` can be
1.84 GB apparent and ~10 MB allocated — a sparse file. A measurement taken
against one of those produced the claim *"MGO ships no female head at all"*,
which went into a catalogue as fact and is **false**. Check that the archive
you are measuring is actually there.
