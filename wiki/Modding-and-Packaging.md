# Modding and Packaging

How to put a file *back*. This page is specific to
[FOX Asset Browser](https://github.com/trappuss/FOXAssetBrowser), which is the
only one of the two projects that writes anything.

## 1. A mod is a mount, not an edit

The obvious way to replace an asset is to write it into `chunk0.dat`. That is
the wrong way, for a reason that has nothing to do with difficulty:

**The blast radius is the user's game.** Writing a replacement into a chunk
means re-encrypting and re-packing a four-gigabyte SQAR, and a bug there does
not produce a wrong pixel — it produces a game that will not boot. There is
also no way to measure a write into a file you have just destroyed.

The alternative is already modelled by the archive index, because a mod install
is a thing that exists:

```
<mod>/Assets/tpp/chara/…     mounted at priority 1100,
                             above every archive and above a dev loose mount at 1000
```

An index that sorts archives by mount priority and returns the copy that
**wins** already answers "what would the game load". So replacing a file is a
copy into a folder, reverting it is a delete, and **nothing** in the viewport,
the exporter or the composer has to learn that a file might be a replacement.

Two rules that are not optional:

- **Only a named asset can be replaced.** The mount derives a hash from the
  *path*; a hash-only file extracts as `unresolved/<hex>.<ext>`, and a
  replacement written there hashes to that literal string and overrides
  nothing. Silently doing nothing is the worst available outcome — refuse it
  and say why.
- **Write whole, then rename into place.** An interrupted copy that leaves a
  truncated asset mounted over a working game file looks like a corrupt game,
  not a failed copy.

And the check that has to be possible: **is this replacement the copy the index
hands out?** A mod folder full of files that override nothing looks identical
from outside to one that works, and the difference is one lookup.

## 2. Replacing a texture

A texture is not one file — see
[Textures and Materials §9](Textures-and-Materials). The install must be
**atomic across the whole set**: every file written to a temporary first, moved
into place only once all of them are written, and nothing changed if any of
them fails.

The re-encode itself is [Textures and Materials §8](Textures-and-Materials):
keep the original's header verbatim, keep each mip in the stream it already
lived in, chunk at 16,384 bytes, compress-or-store per chunk, and refuse a
format, size or mip-count mismatch rather than being clever about it.

## 3. The SnakeBite `.mgsv` format

A `.mgsv` is **a ZIP** with the mod's `Assets/` tree at the root plus a
`metadata.xml`. Rename it to `.zip` and it opens.

`metadata.xml` is a .NET `XmlSerializer` document. The classes, from SnakeBite's
own source:

```csharp
[XmlType("ModEntry")]
public class ModEntry {
    [XmlAttribute("Name")]    public string Name;
    [XmlAttribute("Version")] public string Version;
    [XmlElement("MGSVersion")] public SerialVersion MGSVersion;
    [XmlElement("SBVersion")]  public SerialVersion SBVersion;
    [XmlAttribute("Author")]  public string Author;
    [XmlAttribute("Website")] public string Website;
    [XmlElement("Description")] public string Description;
    [XmlArray("QarEntries")] public List<ModQarEntry> ModQarEntries;
    [XmlArray("FpkEntries")] public List<ModFpkEntry> ModFpkEntries;
}

[XmlType("QarEntry")]
public class ModQarEntry {
    [XmlAttribute("Hash")]        public ulong  Hash;
    [XmlAttribute("FilePath")]    public string FilePath;
    [XmlAttribute("Compressed")]  public bool   Compressed;
    [XmlAttribute("ContentHash")] public string ContentHash;
}

[XmlType("FpkEntry")]
public class ModFpkEntry {
    [XmlAttribute("FpkFile")]     public string FpkFile;
    [XmlAttribute("FilePath")]    public string FilePath;
    [XmlAttribute("ContentHash")] public string ContentHash;
}

[XmlType("SerialVersion")]
public class SerialVersion {
    Version version = new Version();
    [XmlAttribute("Version")] public string Version { get; set; }  // System.Version.ToString()
}
```

`SerialVersion` is **not** in the file that defines `ModEntry` — it lives in
`UpdateFile.cs`, the updater's version type, which `ModEntry` borrows. There is
no `SerialVersion.cs`. That is where an hour goes.

### What the file looks like

```xml
<?xml version="1.0" encoding="utf-8"?>
<ModEntry xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" Name="example mod" Version="1.0.0.0" Author="someone" Website="">
  <MGSVersion Version="0.0.0.0" />
  <SBVersion Version="0.8.0.0" />
  <Description>what it changes</Description>
  <QarEntries>
    <QarEntry Hash="1542763886552676304" FilePath="/Assets/ssd/chara/arm/Pictures/arm16_main0_def_lbm.ftex" Compressed="false" ContentHash="4000A6FCCB7A86947263867902ED26CF" />
  </QarEntries>
  <FpkEntries />
</ModEntry>
```

Details that a reading of the source will not give you, and that running the
real serialiser will:

| question | answer |
|---|---|
| `Hash` — hex or decimal? | **decimal.** `0x1568ff94b645f7d0` writes as `1542763886552676304`. Hex deserialises as zero. |
| `ContentHash` | MD5 of the whole file, **uppercase** hex |
| `Compressed` | `true` iff the file's extension contains `fpk`; lowercase in the XML |
| namespaces | `xmlns:xsd` and `xmlns:xsi` on the root, both unused, both present |
| attribute order | Name, Version, Author, Website — declaration order |
| an empty list | `<FpkEntries />` — but a **null** list omits the element entirely |
| an empty string element | `<Description />`, self-closed |
| attribute escaping | `"` → `&quot;`, plus `&` `<` `>`; newline → `&#xA;`; an apostrophe is **not** escaped |
| element escaping | `&` `<` `>` only |
| declaration / BOM | `<?xml version="1.0" encoding="utf-8"?>`, no BOM, no trailing newline |
| indent | two spaces, with a space before every `/>` |

### The install gates

SnakeBite refuses a mod on three tests:

```
modSBVersion  >  the installed SnakeBite    ->  "requires a newer version"
modSBVersion  <  0.8.0.0                    ->  "no longer compatible"
modMGSVersion != the installed game version, and != 0.0.0.0  ->  warning
```

So the widest-compatibility values are **`SBVersion = 0.8.0.0`** and
**`MGSVersion = 0.0.0.0`** (which means "any game version" and installs with no
warning).

**`SBVersion` must be spelt with all four components, and this is measured, not
reasoned.** A mod written with `SBVersion = "0.8"` deserialises fine, and the
real gate `modSBVersion < new Version(0,8,0,0)` comes back **true** —
`System.Version` leaves unspecified components at **−1**, so `0.8` sorts
*below* `0.8.0.0` and the mod is refused at install time with a message about
an old SnakeBite that has nothing to do with the cause.

### QarEntry vs FpkEntry — the distinction that decides everything

A **QarEntry** replaces a file the game keeps as its own archive entry. An
**FpkEntry** replaces one it keeps *inside* an `.fpk`, and the installer has to
re-pack it into that container.

This matters far more than it looks. The container layer is where the
interesting files are: a top-level walk of TPP reports **305,065** entries as
container-internal and sees none of its 19,138 models. Writing a
container-resident asset as a QarEntry produces a mod that installs perfectly
and changes nothing, with nothing saying why — the worst outcome available.

A flat mod folder of asset paths cannot express the second kind. The honest
thing is to **refuse and name the files**, and say that writing an `.fpk` is
the missing piece.

MakeBite's own convention, for reference: files in the mod folder become
QarEntries; a folder *named* like an `.fpk`, containing files, becomes an
FpkEntry set, and MakeBite rebuilds the `.fpk` before zipping.

## 4. Verifying a package

Two rules, and neither is optional.

**A ZIP writer that only its own reader accepts is a ZIP writer that does not
work.** Read the archive back with an independent implementation — Python's
`zipfile`, `unzip -t`, PowerShell's `Expand-Archive` — checking every CRC and
comparing every member against the bytes that went in.

**And the metadata has to be checked by the thing that will read it.** For a
`.mgsv` that means compiling SnakeBite's own classes and running
`System.Xml.Serialization` against your file, then running the three install
gates on what comes back. Anything less is a reading of the source, not a test
of the file.

One consequence worth planning for: a ZIP records a modification time per
member. Stamp the clock into it and two packages of an unchanged folder differ
in eight bytes per member and nothing else — which is exactly the shape of
difference that makes a diff worthless. Use each **source file's own date**
instead; it is a real fact about the file, stable while the file is, and it
makes the archive byte-identical across runs.
