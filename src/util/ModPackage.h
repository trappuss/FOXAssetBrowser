// ModPackage.h — a mod folder leaves this tool as one file, in either of two
// shapes.
//
//   write()      a plain ZIP: the Assets/ tree plus a manifest. Unzip it over
//                a mod folder and you have that folder back. Always available.
//   writeMgsv()  a real SnakeBite .mgsv, which a mod manager installs.
//
// 9x shipped only the first, because `SerialVersion` — the shape of the
// version fields in `metadata.xml` — could not be found and this project does
// not guess at a format. It has since been found, in SnakeBite's own
// `UpdateFile.cs`, and the whole schema is now pinned against the REAL
// serialiser rather than against a reading of the source: the classes were
// compiled and run, and the file they produce is the reference this writer is
// measured against.
//
// ── THE ONE THING A .mgsv CAN SAY THAT WE CANNOT ────────────────────────────
// A SnakeBite mod names each replaced file as either a QarEntry — a file the
// game keeps as its own archive entry — or an FpkEntry, a file the game keeps
// INSIDE an .fpk container, which the installer has to re-pack into that
// container.
//
// Our mod folder is flat asset paths and has no way to express the second. An
// asset whose game copy lives inside a container, written out as a QarEntry,
// produces a mod that installs a file the game will never read: it applies
// cleanly, it changes nothing, and nothing says why. So writeMgsv() REFUSES
// when the folder holds one, and names it. Writing an .fpk is a real piece of
// work (the container format, plus its .fpkd variant) and it is the next step,
// not something to fake here.
#pragma once
#include <QString>
#include <QStringList>

namespace modpackage {

struct Result {
    QString error;            // empty on success
    int files = 0;            // assets packaged
    int inContainer = 0;      // ...whose game copy is an .fpk/.pftxs child
    int notInIndex = 0;       // ...that this install cannot find a game copy of
    qint64 bytes = 0;         // total uncompressed asset bytes
    QString manifest;         // the manifest text (plain ZIP only)
    QStringList blocked;      // assets that stopped a .mgsv, with the reason
};

// What goes in metadata.xml besides the file list.
//
// The two version fields are not decoration — SnakeBite refuses a mod outright
// on either of them:
//
//   sbVersion   must be >= 0.8.0.0 and <= the SnakeBite doing the installing.
//               0.8.0.0 is therefore the widest-compatibility value there is,
//               and it is the default. It must be spelt with all FOUR
//               components, and that is MEASURED rather than reasoned: a mod
//               written with sbVersion "0.8" was deserialised with SnakeBite's
//               own classes and the real gate `modSBVersion < new Version(0,8,0,0)`
//               came back TRUE — System.Version leaves the unspecified
//               components at -1, so "0.8" sorts BELOW "0.8.0.0" and the mod
//               is refused as "no longer compatible".
//   mgsVersion  0.0.0.0 means "any game version" and installs without a
//               warning. A specific version that does not match the user's
//               install produces one, so the default is 0.0.0.0.
struct MgsvMeta {
    QString name;
    QString version = QStringLiteral("1.0.0.0");
    QString author;
    QString website;
    QString description;
    QString mgsVersion = QStringLiteral("0.0.0.0");
    QString sbVersion = QStringLiteral("0.8.0.0");
};

// The plain ZIP. The archive holds every replacement under its own `Assets/…`
// path, plus `manifest.tsv` and a short `README.txt`.
//
// Fails, rather than writing an empty archive, when no mod folder is
// configured or when the folder holds no replacements: an empty package is a
// file that looks like a delivery and is not one.
Result write(const QString& outPath);

// The SnakeBite package: the same `Assets/…` tree plus a `metadata.xml` at the
// archive root, which is exactly where SnakeBite looks for it.
//
// Refuses — naming what stopped it — when the folder holds an asset the game
// keeps inside a container, when a version string is not something
// System.Version will parse, or when the mod has no name.
Result writeMgsv(const QString& outPath, const MgsvMeta& meta);

// Is `text` something System.Version accepts: one to four dot-separated
// non-negative integers. A string it rejects is silently coerced to 0.0.0.0 by
// SnakeBite's own setter, which for sbVersion means the mod is refused at
// install time with a message about an old SnakeBite — so it is checked here,
// where the reason can still be said out loud.
//
// `parts` is how many components it actually had. The mod's own Version is a
// plain string SnakeBite never compares, so any count will do there; the two
// gated fields are compared with System.Version and need all four, for the
// reason measured above.
bool isVersionString(const QString& text, int* parts = nullptr);

}  // namespace modpackage
