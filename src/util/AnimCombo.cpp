// AnimCombo.cpp — see AnimCombo.h.
#include "util/AnimCombo.h"

#include <QVariant>
#include <QVector>

#include "fox/MtarFile.h"
#include "index/AnimCatalog.h"
#include "index/ArchiveIndex.h"
#include "util/SearchableCombo.h"

namespace animcombo {

QString archiveTooltip()
{
    const fox::AnimCatalog& cat = fox::AnimCatalog::instance();
    if (cat.archives().isEmpty())
        return QStringLiteral("Animation archive (.mtar) — none found");
    return QStringLiteral(
               "Animation archive (.mtar) — %1.\nGrouped by game and shelf; "
               "type to filter by name, clip count or path.")
        .arg(cat.note());
}

QString clipTooltip()
{
    return QStringLiteral(
        "Clip (.gani) — grouped by category.\nThe headline is a readable "
        "expansion of the asset's name, which is shown beneath it; typing "
        "filters on both, so \"walk\" and \"wk_lp\" find the same clip.");
}

void fillArchives(SearchableCombo* combo, const QSet<int>* onlyFiles,
                  const QString& scopeNote)
{
    if (!combo) return;
    combo->clear();
    combo->addPlainItem(QStringLiteral("No animation"), -1);

    const fox::AnimCatalog& cat = fox::AnimCatalog::instance();
    QString shelf;
    int shown = 0;
    for (const int ai : cat.order()) {
        const fox::AnimArchive& a = cat.archives()[ai];
        if (onlyFiles && !onlyFiles->contains(a.fileIdx)) continue;
        ++shown;
        const QString here = a.group.isEmpty()
            ? a.game.toUpper()
            : a.game.toUpper() + QStringLiteral(" · ") + a.group;
        if (here != shelf) { combo->addHeaderItem(here); shelf = here; }
        // The clip count is the single most useful thing about an archive
        // before you open it — BuddyDog2_layers holds 155 and pl_hat31 holds
        // 3, and the old list said only their names.
        combo->addRichItem(
            a.stem,
            QStringLiteral("%1 clip%2%3")
                .arg(a.clips.size())
                .arg(a.clips.size() == 1 ? QString() : QStringLiteral("s"),
                     a.v2 ? QString() : QStringLiteral("  ·  v1/GZ")),
            a.path, a.fileIdx);
    }
    // An EMPTY scoped list is a state with a reason, and a combo holding one
    // grey line reads as a broken index. The reason goes IN the list, not only
    // in the tooltip — nobody hovers a control that looks empty.
    if (onlyFiles && shown == 0)
        combo->addHeaderItem(scopeNote.isEmpty()
                                 ? QStringLiteral("nothing in scope")
                                 : scopeNote.section(QLatin1Char('\n'), 0, 0));
    // A NARROWED LIST SAYS SO. A combo that silently holds 71 of 159 archives
    // reads as an install missing its motions; the caption is the difference
    // between a filter and a fault.
    if (onlyFiles && !scopeNote.isEmpty())
        combo->setToolTip(scopeNote + QStringLiteral("\n\n") + archiveTooltip());
    else
        combo->setToolTip(archiveTooltip());
    combo->setEnabled(!cat.archives().isEmpty());
    qInfo("animcombo: %d archive(s) listed of %lld in the catalogue%s", shown,
          static_cast<long long>(cat.archives().size()),
          onlyFiles ? " — scoped" : "");
}

void fillClips(SearchableCombo* combo, const fox::MtarFile& mtar, int fileIdx)
{
    if (!combo) return;
    combo->clear();

    // Reuse the catalogue's categories when it knows this archive. It will not
    // when the archive came from a loose folder the scan never walked, and the
    // free functions give the same answer for one clip that the catalogue gave
    // for all of them — so the fallback is the same rule, not a lesser one.
    const fox::AnimCatalog& cat = fox::AnimCatalog::instance();
    const fox::AnimArchive* known = nullptr;
    if (fileIdx >= 0)
        for (const fox::AnimArchive& a : cat.archives())
            if (a.fileIdx == fileIdx) { known = &a; break; }

    QString hint;
    const auto& files = fox::ArchiveIndex::instance().files();
    if (fileIdx >= 0 && fileIdx < files.size())
        hint = files[fileIdx].path;

    const int nCat = int(fox::AnimCategory::Count);
    QVector<QVector<int>> byCat(nCat);
    QVector<QString> labels(mtar.clips().size());
    QVector<QString> raws(mtar.clips().size());
    for (int i = 0; i < mtar.clips().size(); ++i) {
        QString raw = mtar.clips()[i].name.section(QLatin1Char('/'), -1);
        const int dot = raw.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) raw.truncate(dot);
        raws[i] = raw;
        if (known && i < known->clips.size()) {
            labels[i] = known->clips[i].label;
            byCat[int(known->clips[i].category)].append(i);
        } else {
            labels[i] = fox::animLabelFor(mtar.clips()[i].name);
            byCat[int(fox::animCategoryFor(mtar.clips()[i].name, hint))].append(i);
        }
    }

    // Only caption when there is more than one group. A three-clip archive
    // that is all Weapon does not need a heading telling it so.
    int groups = 0;
    for (int c = 0; c < nCat; ++c)
        if (!byCat[c].isEmpty()) ++groups;

    for (int c = 0; c < nCat; ++c) {
        if (byCat[c].isEmpty()) continue;
        const QString cname = fox::animCategoryName(fox::AnimCategory(c));
        if (groups > 1)
            combo->addHeaderItem(
                QStringLiteral("%1  (%2)").arg(cname).arg(byCat[c].size()));
        for (const int i : byCat[c])
            // name = our expansion, file = the asset's own name, path = the
            // category. The filter matches all three, so the category name is
            // typeable too — "cqc" finds every CQC clip in the archive.
            combo->addRichItem(labels[i].isEmpty() ? raws[i] : labels[i],
                               raws[i], cname, i);
    }
    combo->setEnabled(combo->count() > 0);
    combo->setToolTip(clipTooltip());
}

}  // namespace animcombo
