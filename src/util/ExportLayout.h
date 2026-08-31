// ExportLayout.h — how a multi-file export lays its files out on disk.
//
// ONE implementation, shared by every path that writes more than one file
// (template §8). Ported from D4AssetBrowser's `util/ExportLayout.h`.
//
// The layout picks the GROUP FOLDER only. What appears inside each group — the
// files themselves, under whatever relative path the extractor already builds
// for them — is fixed and identical in every mode, which is what makes Flat
// simply "one group, at the root" rather than a special case.
//
// THE RULE IS THE ENTRY POINT, NOT THE ITEM COUNT. The single-file paths —
// Ctrl+E, the context menu's "Export as .glb…", the preview pane's buttons,
// drag-out — do not apply a layout: a global "by Family" silently turning
// Ctrl+E into `sna\foo.glb` is a surprise the caller cannot undo, and drag-out
// rebuilds its own paths so a folder would make the drag carry nothing. The
// batch paths apply it even for a one-row selection, because that IS the right
// answer: extracting one more character into a folder you already grouped by
// family belongs in `sna\` with the rest, not loose at the top.
//
// WHAT IS DIFFERENT FROM D4. D4 groups by "Class" and "Type", which name two
// of its AppearanceMeta tag groups. Fox's equivalent vocabulary is
// `index/ModelTags.h`, whose six categories are measured off the index rather
// than authored — so the modes here ARE category ids (`game`, `category`,
// `family`), and adding a category to ModelTags is all it takes to offer
// another grouping. There is no legacy combo-index key to migrate: this tool
// never shipped one, so `mode()` has no migration branch, and if one is ever
// added it belongs here rather than at each call site.
#pragma once
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>
#include <climits>

#include "index/ArchiveIndex.h"
#include "index/ModelTags.h"
#include "util/Extract.h"

namespace ExportLayout {

// Stored in `export/folderLayout` as a STABLE STRING, never the combo index.
// An index is exactly the identity this codebase warns against persisting:
// inserting a mode reorders every saved value, and every clamp guarding it has
// to be found and corrected by hand each time the list grows.
//
// The tag modes are ModelTags category ids, so a mode IS the group's real name.
// `_model` is a sentinel and starts with '_' so it can never collide with one,
// which are all plain words.
inline QString kFlat()     { return QString(); }
inline QString kGame()     { return QStringLiteral("game"); }
inline QString kType()     { return QStringLiteral("category"); }
inline QString kFamily()   { return QStringLiteral("family"); }
inline QString kModel()    { return QStringLiteral("_model"); }

// The modes offered, in the order a combo should list them, with the label to
// show. One list, so the settings page and any preset cannot disagree about
// what exists.
struct Mode { QString id; QString label; QString hint; };
inline QVector<Mode> modes()
{
    return {
        {kFlat(), QStringLiteral("Flat — everything at the root"),
         QStringLiteral("The output folder holds the files' own relative paths "
                        "and nothing else.")},
        {kGame(), QStringLiteral("By game"),
         QStringLiteral("One folder per game: TPP, MGO3, GZ, Survive.")},
        {kType(), QStringLiteral("By asset type"),
         QStringLiteral("The folder under /Assets/<game>/ — chara, weapon, "
                        "item and the rest.")},
        {kFamily(), QStringLiteral("By family"),
         QStringLiteral("The specific character, weapon or item family — sna, "
                        "avm, hag.")},
        {kModel(), QStringLiteral("One folder per file"),
         QStringLiteral("Each file gets a folder named after it. Useful when "
                        "each export carries its own textures.")},
    };
}

// A tag mode groups through ModelTags, whose map covers MODELS. Handed a
// texture or an audio file it returns nothing, so a run that is mostly not
// models files nearly everything under "_misc" — the option looks like it did
// something and did not. A caller offering these outside a model context
// should say so; the Bulk tab notes it under the combo.
inline bool needsTags(const QString& mode)
{
    return mode == kGame() || mode == kType() || mode == kFamily();
}

// Only ids this build understands. Anything else — a newer build's mode, a
// hand-edited INI — has to read as Flat: an unrecognised value would otherwise
// be taken for a category id, match nothing, and file the whole run under
// "_misc" while the combo (findData → -1) displayed "Flat". Fail to where the
// user pointed, never to somewhere they did not ask for.
inline bool isKnown(const QString& mode)
{
    if (mode.isEmpty()) return true;
    for (const Mode& m : modes())
        if (m.id == mode) return true;
    return false;
}

// The mode in force. One setting, one key (§3.1).
inline QString mode()
{
    const QString m =
        QSettings().value(QStringLiteral("export/folderLayout")).toString();
    return isKnown(m) ? m : kFlat();
}

// ONE sanitiser, used by EVERY folder name this header produces. It was on the
// per-file mode only, which was wrong: a tag is a path segment lifted straight
// off the asset tree, so "aux" and "con" are names the game can perfectly well
// have used, and mkpath then fails for that whole group on Windows with every
// file in it reported as "could not create the folder".
inline QString sanitizeFolder(QString stem, const QString& fallback)
{
    // Path separators first: a tag or a stem carrying one would otherwise make
    // the group a nested path, which is not what either mode promises.
    stem.replace(QLatin1Char('\\'), QLatin1Char('_'));
    stem.replace(QLatin1Char('/'), QLatin1Char('_'));
    stem.replace(QLatin1Char(':'), QLatin1Char('_'));
    // A trailing dot or a reserved device name makes mkpath fail on Windows,
    // and every write into that folder then fails with nothing in the log to
    // say why.
    stem = stem.trimmed();
    while (stem.endsWith(QLatin1Char('.'))) stem.chop(1);
    stem = stem.trimmed();
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    if (kReserved.contains(stem, Qt::CaseInsensitive))
        stem.prepend(QLatin1Char('_'));
    return stem.isEmpty() ? fallback : stem;
}

// Folder name for one file under kModel(): its own stem, sanitized. The stem
// comes from the extractor's relative path rather than the export name
// template, because this mode has to work for every kind of file a bulk run
// writes and only models have a template.
inline QString fileFolder(const fox::IndexedFile& f)
{
    return sanitizeFolder(
        QFileInfo(extract::relativePathFor(f)).completeBaseName(),
        QStringLiteral("file"));
}

// Rank of every tag in one category, lowest first, so a file carrying two of
// them lands in the same folder on every run. Built once per grouping call —
// resolving it per item would turn grouping into an O(files x tags) stall on
// the GUI thread.
inline QHash<QString, int> rankOf(const QString& categoryId)
{
    QHash<QString, int> rank;
    for (const fox::TagCategory& c : fox::ModelTags::instance().categories()) {
        if (c.id != categoryId) continue;
        for (int i = 0; i < c.tags.size(); ++i) rank.insert(c.tags[i].tag, i);
        break;
    }
    return rank;
}

// Which tag value this file belongs to. "_misc" when it carries none — never
// empty, so a run cannot scatter files loose into the parent.
inline QString tagFolderIn(const QHash<QString, int>& rank, int fileIdx)
{
    QString best;
    int bestRank = INT_MAX;
    for (const QString& t : fox::ModelTags::instance().tagsOf(fileIdx)) {
        const auto it = rank.constFind(t);
        if (it != rank.constEnd() && it.value() < bestRank) {
            bestRank = it.value();
            best = t;
        }
    }
    return best.isEmpty() ? QStringLiteral("_misc")
                          : sanitizeFolder(best, QStringLiteral("_misc"));
}

struct Group {
    QString folder;        // empty = the destination itself (Flat)
    QVector<int> items;    // indices into ArchiveIndex::files()
};

// Split `items` into destination groups. Flat returns exactly one group with an
// empty folder, so a caller can always treat the result uniformly instead of
// special-casing it.
inline QVector<Group> group(const QString& mode, const QVector<int>& items)
{
    if (mode.isEmpty() || !isKnown(mode) || items.isEmpty())
        return {Group{QString(), items}};

    // QMap, not QHash: folders come out in a stable order, so the log and the
    // progress read the same way on every run over the same selection.
    QMap<QString, QVector<int>> by;
    if (mode == kModel()) {
        const auto& files = fox::ArchiveIndex::instance().files();
        for (const int i : items) {
            if (i < 0 || i >= files.size()) continue;
            by[fileFolder(files[i])].append(i);
        }
    } else {
        if (!fox::ModelTags::instance().ok())
            return {Group{QString(), items}};   // no vocabulary yet — one
                                                // "_misc" folder helps nobody
        const QHash<QString, int> rank = rankOf(mode);
        if (rank.isEmpty()) return {Group{QString(), items}};
        for (const int i : items) by[tagFolderIn(rank, i)].append(i);
    }
    QVector<Group> out;
    out.reserve(by.size());
    for (auto g = by.constBegin(); g != by.constEnd(); ++g)
        out.append(Group{g.key(), g.value()});
    return out;
}

// The destination for one group, resolved against the run's root.
inline QString folderFor(const QString& root, const Group& g)
{
    return g.folder.isEmpty() ? root : QDir(root).filePath(g.folder);
}

// The group folder for ONE file — for a caller that writes as it goes rather
// than group by group. `rank` is the caller's, hoisted out of its loop.
inline QString folderForFile(const QString& mode, const QHash<QString, int>& rank,
                             int fileIdx, const fox::IndexedFile& f)
{
    if (mode.isEmpty() || !isKnown(mode)) return QString();
    if (mode == kModel()) return fileFolder(f);
    if (rank.isEmpty()) return QString();
    return tagFolderIn(rank, fileIdx);
}

}   // namespace ExportLayout
