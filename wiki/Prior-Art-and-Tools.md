# Prior Art and Tools

Fox Engine has been picked apart by a number of people over the years. This
page exists so the next researcher starts from what already exists rather than
from zero — and so credit sits where it belongs.

Anything in this handbook that came from one of these projects says so at the
point it is used.

---

## GzsTool — Atvaark

**The archive reference.** QAR/SQAR, FPK/FPKD and PFTXS structure, the header
and section-table decryption, both per-entry payload ciphers, and the
`PathFileNameCode` hashing in `Hashing.cs`.

FOX Asset Browser's archive readers are **direct C++ ports** of it, byte for
byte, because it is the implementation that round-trips the shipped archives.
If you are writing an archive reader, port this rather than reverse-engineering
the container again.

- https://github.com/Atvaark/GzsTool

## FtexTool — Atvaark

**The texture reference.** FTEX/FTEXS layout, the mip/stream split and the DDS
mapping. The FTEX *writer* documented in
[Textures and Materials](Textures-and-Materials) is the exact inverse of this
reader, which is the only reason writing one was tractable.

- https://github.com/Atvaark/FtexTool

## FoxBrowser

**The viewer that showed what was possible**, and the decoder
[foxanimrip](https://github.com/trappuss/FOXanimrip) drives: FMDL, GANI, FRIG,
FRDV and FTEX, with a rig solve and an FBX writer.

FOX Asset Browser shares no code with it and does not require it. If you want
to look at a single asset quickly, it remains an excellent tool, and it is worth
endorsing.

- https://www.nexusmods.com/metalgearsolidvtpp/mods/2531

## SnakeBite — topher-au and contributors

**The mod manager, and the `.mgsv` package format.** The metadata schema in
[Modding and Packaging](Modding-and-Packaging) is read straight from its
source, and its own classes are what verify a package this project writes.

Two things worth knowing before you read that source: `SerialVersion` is
defined in `UpdateFile.cs`, not beside `ModEntry`; and the install gates in
`ModManager.cs` are what decide whether a mod is accepted, so they are what a
writer has to satisfy.

- https://github.com/topher-au/SnakeBite

## FoxKit-3 — Joey35233

A Unity-based Fox Engine toolkit, and **the most precise public account of the
animation track format**. `FoxKit/Assets/Fox/Anim/Playback/TrackData.cs` on the
`anim-dev` branch documents, in working code:

- the `TrackData` / `TrackMiniData` header layouts
- the `SegmentType` enum — `Quat, Float, Vector2, Vector3, Vector4, QuatDiff, VectorDiff`
- per-track `ComponentBitSize` and the unaligned bit reader
- the quaternion encoding, half-precision float decode, and the
  `PlaybackRate = 1/(60000/1001)` constant that pins playback to 59.94 fps

Read it before writing any GANI parser.

- https://github.com/Joey35233/FoxKit-3

## fox_engine_mtar_tools_blender — mctrollin

A Blender add-on that **imports *and* exports MTAR**, which matters: most tools
are read-only, so this is the one that proves a round trip is possible.

Format knowledge it makes public: MTAR as a container of embedded GANI files
with header-based version detection; the **GANI1 / GANI2** split; GANI2's
Layout Track in the negative frame range; and the fact that clips carry motion
events, motion points and shader parameters, not just bone curves.

Its stated limitations are informative in their own right — no big-endian
support, not all track types, no twist-bone reconstruction, and repeated
import/export degrades data, which is what you would predict from per-track
bit-packed precision.

- https://github.com/mctrollin/fox_engine_mtar_tools_blender

## mgsv-lookup-strings — kapuragu

**Validated dictionaries for reversing hashed names**, organised by tool
(FmdlTool, GzsTool, LangTool, …) and by `<file type>\<data type>\<game>`,
alongside raw strings scraped from the executables.

This is the answer to the anonymous-file problem. Fox stores no paths, only
hashes, so recovering a name means having the string beforehand, and this is
the community's accumulated corpus. `_HashStringMatches.txt` files take you
from a bare hash to a candidate string.

If a third of your textures are coming out hash-named, a fuller dictionary is
the fix, and this is where to get one.

- https://github.com/kapuragu/mgsv-lookup-strings

## Fox_Parser — Frostyman758

A broad multi-format parser — QAR, FPK, MTAR, FTEX, PFTXS, FV2, G0S, SPCH, STP,
RDF, FOX, FSOP, HLSL, TCVP, TWPF, SBP — with **a test suite per file type**,
built so behaviour cannot silently change against the original.

Notable for reaching formats this handbook lists as unexplored, `.fsop`
(compiled shaders) among them. If you are chasing one of the
[Open Questions](Open-Questions), check here first.

- https://github.com/Frostyman758/Fox_Parser

## foxanimrip

**This handbook's other half.** A headless bulk ripper driving FoxBrowser's
decoders, with a Blender add-on and the locomotion research — including the
measured player speed table, recovered from the stance foot because the cycles
carry no root travel.

- https://github.com/trappuss/FOXanimrip

---

## Which to reach for

| goal | start with |
|---|---|
| browse an install, build a character, extract in bulk | FOX Asset Browser |
| rip thousands of animations, or measure motion | foxanimrip |
| look at one asset quickly | FoxBrowser |
| write an archive reader | GzsTool |
| write a texture reader or writer | FtexTool |
| write a GANI parser | FoxKit-3 `TrackData.cs` |
| get animation back *into* the game | mctrollin's MTAR tools |
| install or package a mod | SnakeBite |
| recover hashed names | mgsv-lookup-strings |
| a format nobody here covers | Fox_Parser |

## On hash types

Three schemes appear across these tools, and confusing them wastes time:

- **PathFileNameCode** — 64-bit, addresses files in the archives: 51 bits of
  CityHash64 over the lowercased extensionless path plus a 13-bit extension
  code. See [Archives and Hashing](Archives-and-Hashing).
- **StrCode32** and **StrCode64** — hash *strings inside* files: bone names,
  parameter names, node names. These are what a bone dictionary resolves, and
  what a connect point's parent lookup uses so it works without a dictionary.

A dictionary is only useful against the hash type it was built for, which is
why mgsv-lookup-strings separates them by tool and data type.
