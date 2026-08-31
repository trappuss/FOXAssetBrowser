// ShadowDisplay.h — one rendering rule for "overridden" files, shared by every
// list/tree that shows index entries.
//
// When two archives carry the same asset (a mod install in a higher-priority
// mount alongside the stock chunk), the index keeps BOTH in the file list but
// resolves lookups to the copy the game loads and marks the other `shadowed`.
// Every view must say so the same way, or one pane contradicts another — so
// the dimming, the suffix and the tooltip live here, not in each model.
#pragma once
#include <QBrush>
#include <QColor>
#include <QString>
#include <QVariant>
#include <Qt>

#include "index/ArchiveIndex.h"

namespace shadowui {

inline QString suffix() { return QStringLiteral("  (overridden)"); }

inline QString tooltip()
{
    return QStringLiteral(
        "Overridden: another archive with higher mount priority contains this "
        "same asset, and that is the copy the game loads — and the copy this "
        "browser resolves to everywhere else.");
}

// A view's data(): return this first; an invalid QVariant means "not
// overridden, carry on with your normal handling".
inline QVariant roleFor(const fox::IndexedFile& f, int role, const QString& text)
{
    if (!f.shadowed) return {};
    if (role == Qt::DisplayRole) return text + suffix();
    if (role == Qt::ForegroundRole) return QBrush(QColor(140, 140, 140));
    if (role == Qt::ToolTipRole) return tooltip();
    return {};
}

}  // namespace shadowui
