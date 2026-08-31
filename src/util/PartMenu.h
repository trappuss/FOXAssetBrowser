// PartMenu.h — ONE builder for the part menu (docs/CONTEXT_MENUS.md §4).
//
// §4: "One builder, six entry points." In this tool the entry points are the
// 3D viewport's right-click and the PARTS panel's, in each of the tabs that
// show a model. They were two independent implementations of the same menu —
// and SceneTree's carried the comment "THE SAME THREE CASES AS THE VIEWPORT
// (see installViewportContextMenu)", which is a comment pointing at its own
// duplicate. Menus built independently drift; in D4 six part-menu entry points
// had converged on three different subsets.
//
// ── How a caller uses it ───────────────────────────────────────────────────
// Fill in what you can resolve and leave the rest empty. §0's third law:
// "A menu never shows an action it cannot perform. Callers supply only what
// they can resolve; empty strings and null callbacks are omitted." So a
// viewport that cannot invert visibility simply leaves `invert` null and no
// Invert row appears — rather than a permanently grey one that teaches nobody
// anything.
//
// EVERY VALUE IS SNAPSHOTTED into the Context before the menu is built,
// because §4's last rule is that `exec()` spins a nested event loop and the
// index can move-assign under it. Nothing in here holds a pointer into a
// container a background thread owns.
#pragma once
#include <QSet>
#include <QString>

#include <functional>

class QMenu;

namespace partmenu {

struct Context {
    // ── The subject ────────────────────────────────────────────────────
    QSet<int> subject;       // the parts the menu acts on; empty = none hit
    int activePart = -1;     // the ONE part, for single-subject actions
    bool hasGeometry = false;
    bool anyHidden = false;  // is anything hidden right now (for Show all)
    bool subjectHidden = false;   // …and is the SUBJECT hidden (Hide/Show)

    // ── What only the page can resolve ─────────────────────────────────
    QString modelName;       // "uam12_main0_def"
    QString partName;        // the active part's MATERIAL name — §1 is explicit
                             // that this is not a file name
    QString partTag;         // "[SIM]" / "[FX]" and the like, appended to the
                             // header; empty for an ordinary part
    QString filePath;        // the model's asset path
    QString fileHash;        // "0x…" — Fox's identity, D4's "SNO"
    QString localisedName;   // the in-game name, when this asset has one
    qint64 modelTris = 0;    // for "Export model (12,345 tris)"
    qint64 partTris = 0;

    // ── What the page can DO ───────────────────────────────────────────
    // A null callback means the action is not offered at all.
    std::function<void()> framePart;
    std::function<void()> selectPart;      // must NOT move the camera (§4)
    std::function<void(bool)> setHidden;   // true = hide the subject
    std::function<void()> isolatePart;
    std::function<void()> showAll;
    std::function<void()> hideAll;
    std::function<void()> invert;
    std::function<void()> exportModel;     // prompts
    std::function<void()> exportModelLast; // silent, to the remembered folder
    std::function<void()> exportPart;
    std::function<void()> exportPartLast;
};

// Append §4's blocks to `menu`, in §4's order. Adds separators only between
// blocks that actually produced rows, so a menu missing a block does not carry
// the gap where it would have been.
void build(QMenu* menu, const Context& ctx);

}  // namespace partmenu
