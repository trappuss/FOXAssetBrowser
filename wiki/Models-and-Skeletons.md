# Models and Skeletons

## 1. FMDL

`.fmdl` (extension code 4244) is the model container: geometry, materials,
texture references and skeleton. TPP ships 19,138, Survive 15,024, GZ 1,017.

Structure that matters:

- **Bones** — an ordered list with `NameIndex`, `ParentIndex`, and both
  `LocalPosition` and `WorldPosition` as Vector4.
- **Meshes** — position, normal, tangent, up to four UV sets, vertex colour,
  triangle indices, material index, skin info.
- **Skin** — a palette of bone indices, plus per-vertex indices into that
  palette and four weights.
- **MaterialTextures** — per material, a set of (role, hash, path) texture
  references.
- **Material parameters** — per material, a named vector list
  (`MatParamIndex_0`, `Incidence_*`, `Tension*` …) that drives multi-material
  surfaces and rim terms. **Zeros in there are data, not padding**: seventeen
  materials in one shipped set bind a sub-normal map and then set its blend to
  zero deliberately.

### The bind pose has no rotation

Fox bone rest positions carry translation only. That is why an exporter can
publish a bone transform as three floats without losing anything, and why the
inverse bind matrix is a translation rather than a full matrix.

### Hierarchy invariant

Translation-only rest means this must hold for every bone:

```
worldPosition == parent.worldPosition + localPosition
```

It is a free validity check on any hierarchy you have parsed.

### The waist is the origin, not the floor

`SKL_000_WAIST` sits at `(0, 0, 0)`; feet rest near `y = −0.96`. Characters are
authored about the pelvis. Y-up.

## 2. Bone naming

Bones are `SKL_<number>_<TAG>`. The numbering is stable: `0xx` spine and head,
`01x`/`02x` arms, `03x`/`04x` legs, `5xx`/`6xx` helpers.

Two suffixes carry meaning:

- **`_HLP`** — help bones. Muscle and twist correctives driven by the FRDV
  operator list. 64 in the bone dictionary.
- **`_SIM`** — simulation bones. Physics-driven secondary motion for cloth,
  straps, hair and pouches. 130 entries.

Names come from `bone_dictionary.txt` (561) and the model's own string table
via `fmdl_dictionary.txt` (20,659). Without the dictionaries staged, bones read
as `bone_<hex>`.

Bone names are matched by **StrCode32**, not by string, wherever the match has
to be dictionary-independent — the connect-point parent lookup does exactly
this.

## 3. Skeleton sizes

| Model | Bones | What it is |
|-------|-------|------------|
| `skl0_main0_def` | 121 | TPP first-person arms model |
| `sna2_main0_def` | 138 | Snake, GZ fatigues |
| `avf0_body0_def` | 116 | MGO female avatar base body |
| `bsf0_main0_def` | 126 | Survive female player body |
| `skl0_main0_def` (MGO) | 486 | the MGO avatar skeleton carrier — male |
| `skl0_main0_def_f` (MGO) | 489 | …and female |

Bone count varies with each model's helper and simulation bones; the deforming
core is shared.

### The `skl0_main0_def` trap

Despite the generic name, TPP's is the **complete player skeleton with only arm
meshes** — 1,378 vertices, no body, and it passes every validity check. Snake's
body is `sna2_main0_def`. Likewise `avf0_body0_def` is one part of an MGO
avatar, not a character: rendered alone it is a floating shirt.

MGO's `skl0` is a different animal again — see
[MGO Avatar and Gear](MGO-Avatar-and-Gear). It is the model the game poses and
never draws.

## 4. FRIG — the rig

`.frig` (2276) turns animation channels into a pose:

- **rig units** (18 for the human rig) and **segments** (56)
- **bone drives** — animation track → bone mappings
- **IK jobs** — foot planting and the like

The canonical human rig `/Assets/fox/rig/frig/human_finger` resolves to **53
bones** across TPP, MGO, GZ and Survive character models. Rig quality is the
single best signal that an export is sound.

### The 8-bone trap

Generic core bones are shared by a great many humanoid skeletons, so a
minimum-match threshold of 8 is far too permissive — a 15-bone prop skeleton
will match a full player animation set. Rank candidates by matched-bone count
and take the **best**, never the first above a threshold. `foxanimrip`'s
`--all-sets` still has this bug and says so; 209 TPP sets bind to a 15-bone
stand-in whose clips animate no legs.

## 5. FRDV — help bone operators

`.frdv` (6588) carries help-bone operator lists — 23 to 32 operators on player
models — that compute corrective bones from the pose: twist distribution,
shoulder correctives.

They must be evaluated **after the base pose and before skinning**. Skip it and
you get subtly wrong poses — collapsed elbows, pinched shoulders — with nothing
reporting an error.

## 6. FCNP — connect points

`.fcnp` names the points one model hangs off another: a suppressor on a muzzle,
headgear on a head. Each point carries a position, a quaternion and a **parent
bone name**, and the parent is matched by StrCode32 so it works without a
dictionary.

Connect points are the game's own answer to a part whose skeleton has no bone
in common with the wearer's — which is a real and common shape, not an edge
case. See §7.

## 7. Parts whose skeleton shares nothing with the body

A bone-level scan of 417 skinned models in one reference pull:

| root bone | models with >1 bone |
|---|---|
| `SKL_000_WAIST` | 291 |
| `SKL_000_ROOT` | 41 |
| `SKL_400_HEADROOT` | 2 |
| `SKL_002_CHEST` | 2 |
| `SKL_004_HEAD` | 2 |

**41 models root at `SKL_000_ROOT`, and not one of them shares a single bone
with the body.** Their hierarchies have no relationship to the character's at
all; their placement comes from the socket and from nothing else. Also measured
and worth recording because it is the obvious next worry: **there is not one
multi-root model in the whole tree** — 417 of 417 have exactly one root.

Twenty MGO headgear models — ten per gender (`hat0,1,2,3,7,14,15,16,19` and
`reh1`) — root at `SKL_000_ROOT`, which the player skeleton does not carry, so
a naive load lands them at the model origin, on the floor. The game's own
answer is `skl0`'s connect point: **`CNP_HEAD` at (0, 0.1029, 0.0108)**, the
ten centimetres between the crown and the mouth.

**The two genders hang that point off different parents** — `skl0_main0_def_f`
off `SKL_004_HEAD`, `skl0_main0_def` off `SKL_400_HEADROOT`. Use the connect
point's **own** parent bone, with the slot's anchor only as a fallback.
Requiring the slot's bone to match silently dropped the offset for the man
while the woman looked right, which is the worst shape a bug can have.

## 8. Composing a character, and where it goes wrong

A Fox character is several models rigged to one skeleton. Posing the assembly
means electing a **host** — the part with the most bones — and deciding, per
part, how it rides:

| regime | when | how |
|---|---|---|
| HOST | the elected host | its own palette |
| BORROWED | every bone resolves against the host | the host's matrices, by name |
| CARRIED | nothing the clip drives, or socket-anchored | rigidly, on the anchor bone |
| OWN | its own hierarchy, unresolved bones | its own palette |

A part reaching the connect-point or slot-anchor path is one **no part in the
scene carries the root bone of**. Its whole chain hangs off the model origin,
so posing it from the clip cannot be right however many of its bones happen to
share a *name* with a driven one — the chain still sits at the origin. It is
carried by the socket, and `driven > 0` must not override that.

### The rest-alignment offset, and what it is NOT evidence of

A part's own bind pose can disagree with the skeleton it shares. The correction
`d` is measured against the anchor's **bind** position and belongs **in front
of** the bone's matrix, not behind it:

```
v · translate(d) · palette[b]
```

Behind it, the vertex is rotated about the wrong origin and drifts by exactly
`d·R − d` — "fine in T-pose, disconnected once anything plays", which is
precisely the symptom that gets reported.

**[open] The residual that reports this as fixed measures the wrong thing.**
It is computed as

```
landed   = the part's root bone world position, through its own root pose
target   = the anchor bone's bind world position
residual = |landed − target|
```

Both terms are **bone** positions. It says nothing about where the *vertices*
ended up — and it is computed through the same matrices the correction was
folded into, so it reports success whether or not the correction was the right
size. A wardrobe sweep across 145 items reported "116 seated, 0 floating" for a
scene in which two accessories were visibly half a metre off the head. See
[Open Questions](Open-Questions).

## 9. Practical decode order

1. Stage the name dictionaries.
2. Parse the FMDL — bones, meshes, skin, material and texture references.
3. Resolve the `.frig` rig and the `.frdv` help-bone operators.
4. For a clip: decode GANI, resolve tracks to bone indices, solve bone drives,
   solve IK jobs, then evaluate help-bone operators.
5. Skin with the resolved palette.

Steps 3 and 4 are where errors are silent rather than loud.
