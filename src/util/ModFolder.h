// ModFolder.h — the one place that knows what a replacement asset looks like
// on disk.
//
// THE WHOLE WRITE STORY OF THIS TOOL IS A MOUNT, NOT AN EDIT.
//
// A mod is a directory of replacement assets laid out exactly the way the
// extractor writes them — <mod>/Assets/<game>/… — mounted over the game's
// archives at a higher priority than any of them (ArchiveIndex, 1100). The
// index already models that: IndexedFile::shadowed is "a higher-priority mount
// carries this hash", and fileIndexForPath already returns the copy that WINS.
// So replacing a file is a copy into a folder, reverting it is a delete, and
// nothing in the viewport, the exporter or the customizer had to learn that a
// file might be a replacement.
//
// The alternative — writing back into chunk0.dat — means re-encrypting and
// re-packing a four-gigabyte SQAR, and a bug there does not produce a wrong
// pixel, it produces a game that will not boot. There is no way to measure a
// write into a file you have just destroyed, which is the whole reason this
// module exists in this shape.
//
// ONLY NAMED ASSETS CAN BE REPLACED, and that is enforced rather than assumed.
// The loose walk derives an asset's hash from its PATH under the mod root; a
// file whose name this install cannot resolve is extracted as
// unresolved/<hex>.<ext>, and a replacement written there would hash to that
// literal path and override nothing at all. Silently doing nothing is the
// worst outcome available, so putFile() refuses it and says why.
#pragma once
#include <QByteArray>
#include <QString>
#include <QPair>
#include <QStringList>
#include <QVector>
#include <functional>

namespace modfolder {

// The configured mod folder, or empty. Does not check that it exists.
QString dir();
// True when a mod folder is configured AND present on disk.
bool active();

// Where this asset's replacement lives. Empty when no folder is configured,
// when the asset path is not a resolved /Assets/… name, or when the path would
// escape the root — the last of which is checked rather than trusted, because
// the path reaching here came out of an archive's own name table.
QString pathFor(const QString& assetPath);

// Is there a replacement on disk for this asset right now?
bool overrides(const QString& assetPath);

// Install a replacement. Creates the parent folders. Returns an empty string
// on success, or a sentence naming what stopped it.
QString put(const QString& assetPath, const QByteArray& data);
QString putFile(const QString& assetPath, const QString& sourceFile);

// Install SEVERAL files as one unit, all or nothing.
//
// A texture is not one file. A .ftex carries only the small mips; the large
// ones live in .1.ftexs, .2.ftexs and so on, and the DDS mip order is ftexs
// file number DESCENDING. Installing three of four leaves a texture mounted
// with mismatched mips — a file the engine reads as garbage, from a mod folder
// that looks correctly populated. So every file is written to a temporary
// first, and they are moved into place only once all of them are written; a
// failure anywhere removes the temporaries and changes nothing.
QString putSet(const QVector<QPair<QString, QByteArray>>& files);

// Remove one replacement. Empty string on success, INCLUDING when there was
// nothing to remove — "revert" is a statement about the end state, and a menu
// entry that errors because the thing is already the way you asked for it is
// noise.
QString revert(const QString& assetPath);

// ── "the folder changed, look again" ────────────────────────────────────────
// Installing or reverting a replacement means nothing until the index has
// walked the folder and decided which copy wins — a written file with a stale
// index shows the user the game's copy of an asset they just replaced, with no
// way to tell which one they are looking at. That rescan lives in MainWindow
// and stays there: this is the hook it installs, so the action sites can ask
// for a rescan without a second spelling of what a rescan is.
void setChangedHook(const std::function<void()>& hook);
void notifyChanged();

// Every replacement in the folder, as asset paths, sorted. Walks the tree; the
// caller is the mod panel and the --moddump harness flag, neither of which is
// on a hot path.
QStringList list();

}  // namespace modfolder
