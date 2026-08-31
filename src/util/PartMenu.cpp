#include "util/PartMenu.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMenu>

#include "app/Config.h"
#include "util/MenuText.h"

namespace partmenu {

namespace {

// A separator only between blocks that actually produced rows. Tracking the
// size the menu had at the end of the last block is the whole trick: a menu
// that adds a separator per block and then adds no rows shows the gap anyway,
// which reads as a missing feature.
struct Blocks {
    QMenu* menu;
    int mark;
    explicit Blocks(QMenu* m) : menu(m), mark(m->actions().size()) {}
    void gap()
    {
        const int now = menu->actions().size();
        if (now > mark && now > 0) menu->addSeparator();
        mark = menu->actions().size();
    }
};

// §1/§6: the value is in the label, an empty one renders as "—", and the
// action is DISABLED rather than hidden.
void copyRow(QMenu* menu, const QString& label, const QString& value)
{
    QAction* a = menu->addAction(MenuText::withCopyValue(label, value), menu,
                                 [value] {
                                     QApplication::clipboard()->setText(value);
                                 });
    a->setEnabled(!value.isEmpty());
}

QString lastDir()
{
    const QString d = Config::exportDir();
    return (!d.isEmpty() && QDir(d).exists()) ? d : QString();
}

}  // namespace

void build(QMenu* menu, const Context& ctx)
{
    if (!menu) return;
    const bool hasPart = ctx.activePart >= 0 && !ctx.subject.isEmpty();
    Blocks b(menu);

    // ── Header ──────────────────────────────────────────────────────────
    // Disabled, and ONLY when a part was hit. It is what tells you which of
    // four selected things the menu is about to act on. Omitted entirely on a
    // click into empty space, where there is no subject to name.
    if (hasPart && !ctx.modelName.isEmpty()) {
        QString title = ctx.modelName;
        if (!ctx.partName.isEmpty())
            title += QStringLiteral("  —  %1").arg(ctx.partName);
        else
            title += QStringLiteral("  —  part %1").arg(ctx.activePart);
        if (!ctx.partTag.isEmpty())
            title += QStringLiteral("  %1").arg(ctx.partTag);
        // Several parts under one right-click: say how many, or the header
        // names one thing while the actions below act on four.
        if (ctx.subject.size() > 1)
            title += QStringLiteral("   (+%1 more)").arg(ctx.subject.size() - 1);
        menu->addAction(title)->setEnabled(false);
    }
    b.gap();

    // ── Export ──────────────────────────────────────────────────────────
    // "To last folder" is OMITTED when nothing is remembered, never offered
    // and then made to open a dialog.
    const QString dir = lastDir();
    if (ctx.exportModelLast && !dir.isEmpty())
        menu->addAction(MenuText::withValue(MenuText::kExportModelLast,
                                            MenuText::condensePath(dir)),
                        menu, ctx.exportModelLast);
    if (ctx.exportModel)
        menu->addAction(
            MenuText::prompts(MenuText::withCount(MenuText::kExportModel,
                                                  int(ctx.modelTris))),
            menu, ctx.exportModel);
    if (hasPart && ctx.exportPartLast && !dir.isEmpty())
        menu->addAction(MenuText::withValue(MenuText::kExportPartLast,
                                            MenuText::condensePath(dir)),
                        menu, ctx.exportPartLast);
    if (hasPart && ctx.exportPart)
        menu->addAction(
            MenuText::prompts(MenuText::withCount(MenuText::kExportPart,
                                                  int(ctx.partTris))),
            menu, ctx.exportPart);
    b.gap();

    // ── Copy ────────────────────────────────────────────────────────────
    // NOT gated on hitting a part. §4 calls this out as a real gap: a
    // right-click on empty viewport space, or on the tree's root row, is still
    // a right-click on the LOADED MODEL, and its name, path and hash are
    // sitting right there. Only the material name is genuinely part-scoped.
    if (hasPart && !ctx.partName.isEmpty())
        copyRow(menu, MenuText::kCopyMaterial, ctx.partName);
    if (!ctx.filePath.isEmpty()) {
        copyRow(menu, MenuText::kCopyPath, ctx.filePath);
        copyRow(menu, MenuText::kCopyFileName,
                ctx.filePath.section(QLatin1Char('/'), -1));
    }
    if (!ctx.fileHash.isEmpty()) copyRow(menu, MenuText::kCopyHash, ctx.fileHash);
    // Offered only where the asset HAS a localised name — for most Fox assets
    // the entry name IS the file name, and two labels for one payload is the
    // duplicate §5 says to drop rather than fake.
    if (!ctx.localisedName.isEmpty())
        copyRow(menu, MenuText::kCopyName, ctx.localisedName);
    b.gap();

    // ── This part ───────────────────────────────────────────────────────
    if (hasPart) {
        const QString what =
            ctx.subject.size() == 1
                ? QStringLiteral("part")
                : QStringLiteral("%1 parts").arg(ctx.subject.size());
        // Built from the vocabulary constants with the count word swapped in,
        // rather than spelled out here: "Frame part" and "Frame 3 parts" are
        // one action with a live subject, not two labels.
        const auto scoped = [&what](const QString& base) {
            QString s = base;
            return s.replace(QLatin1String("part"), what);
        };
        if (ctx.framePart)
            menu->addAction(scoped(MenuText::kFramePart) + QStringLiteral("\t."),
                            menu, ctx.framePart);
        // Selecting must NOT move the camera. Framing is its own action, right
        // above; a select that also framed would make it impossible to pick a
        // part without losing the view you were comparing it against.
        if (ctx.selectPart)
            menu->addAction(scoped(MenuText::kSelectPart), menu,
                            ctx.selectPart);
        // ONE action whose label reflects the CURRENT state — not two actions
        // and not a checkbox.
        if (ctx.setHidden) {
            const bool hidden = ctx.subjectHidden;
            menu->addAction(hidden ? scoped(MenuText::kShowPart)
                                   : scoped(MenuText::kHidePart)
                                         + QStringLiteral("\tH"),
                            menu, [cb = ctx.setHidden, hidden] { cb(!hidden); });
        }
        if (ctx.isolatePart)
            menu->addAction(scoped(MenuText::kIsolatePart)
                                + QStringLiteral("\tShift+H"),
                            menu, ctx.isolatePart);
    }
    b.gap();

    // ── All parts ───────────────────────────────────────────────────────
    // Works with NOTHING under the cursor: a right-click on empty space still
    // gets these three, which is the only way to undo a hide without hunting
    // for the row that did it.
    if (ctx.hasGeometry) {
        if (ctx.showAll && ctx.anyHidden)
            menu->addAction(QStringLiteral("%1\tAlt+H").arg(MenuText::kShowAll),
                            menu, ctx.showAll);
        if (ctx.hideAll) menu->addAction(MenuText::kHideAll, menu, ctx.hideAll);
        if (ctx.invert) menu->addAction(MenuText::kInvert, menu, ctx.invert);
    }
}

}  // namespace partmenu
