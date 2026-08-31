// Hotkeys.h — the central, rebindable shortcut registry (template §11).
//
// Ported from D4AssetBrowser's `app/Hotkeys.h`. ONE table, shared by the
// Settings ▸ Hotkeys editor (which writes) and MainWindow (which reads and
// applies each as a QAction shortcut). Adding a shortcut is adding a row here
// and nothing else — which is the whole point, and why this is a header with
// no build entry rather than a class somewhere.
//
// The keys are QSettings keys, so a binding is one setting with one key
// (template §3.1) and the editor and the applier cannot disagree about which.
// An EMPTY string means deliberately unbound, and is a real state: the two GIF
// exports ship unbound because they are slow, destructive of a good camera
// angle, and not something anyone wants to hit by accident.
#pragma once
#include <QAction>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QSettings>
#include <QString>
#include <QVector>

namespace Hotkeys {

struct Def {
    QString key;    // QSettings key
    QString label;  // shown in the Settings editor
    QString def;    // default key sequence (portable text; empty = unbound)
    QString hint;   // one line for the editor's tooltip
};

// The shipping defaults are D4's, because a person who uses both tools should
// not have to learn two sets. Where Fox has something D4 does not, the binding
// is new and follows the same shape.
//
// D4's "Export to last folder" and "Animation-loop GIF" are NOT here. This
// tool has neither — there is no export-to-last path and no loop-GIF action —
// and a registry row for a thing that does not exist is a shortcut the editor
// shows as bound, the tooltip describes, and nothing on earth will fire. A
// missing row is honest; a dead key is not. Add the row with the feature.
inline QVector<Def> defs()
{
    return {
        {QStringLiteral("hotkeys/customizeUndo"),
         QStringLiteral("Undo (Customize)"), QStringLiteral("Ctrl+Z"),
         QStringLiteral("Step back through what you built in Customize — an "
                        "equip, a variation, a dye, a hidden submesh. Thirty "
                        "deep. The camera, the panel layout and the loaded "
                        "clip are not undoable: you did not author them.")},
        {QStringLiteral("hotkeys/customizeRedo"),
         QStringLiteral("Redo (Customize)"), QStringLiteral("Ctrl+Shift+Z"),
         QStringLiteral("Step forward again, until the next change you make "
                        "starts a new branch.")},
        {QStringLiteral("hotkeys/exportSelection"),
         QStringLiteral("Export selection…"), QStringLiteral("Ctrl+E"),
         QStringLiteral("Export whatever the current tab has selected, asking "
                        "where to put it.")},
        {QStringLiteral("hotkeys/exportAnimations"),
         QStringLiteral("Export animations only…"),
         QStringLiteral("Ctrl+Shift+A"),
         QStringLiteral("The skeleton and the selected clips, with no mesh — a "
                        "retargeting library.")},
        {QStringLiteral("hotkeys/saveImage"),
         QStringLiteral("Save the viewport image…"),
         QStringLiteral("Ctrl+Shift+I"),
         QStringLiteral("The GL surface as a PNG, at its current size. No "
                        "toolbar, no panels.")},
        {QStringLiteral("hotkeys/turntable"), QStringLiteral("Turntable GIF…"),
         QString(),
         QStringLiteral("One full revolution as an animated GIF. Unbound by "
                        "default: it takes a while and it is not something to "
                        "start by accident.")},
        {QStringLiteral("hotkeys/focusSearch"),
         QStringLiteral("Focus the search box"), QStringLiteral("Ctrl+F"),
         QStringLiteral("Jump to the current tab's search box. Esc clears it "
                        "and gives the focus back.")},
        {QStringLiteral("hotkeys/settings"), QStringLiteral("Settings…"),
         QStringLiteral("Ctrl+,"),
         QStringLiteral("The settings dialog.")},
        {QStringLiteral("hotkeys/rescan"),
         QStringLiteral("Rescan the archives"), QStringLiteral("F5"),
         QStringLiteral("Re-read the configured folders. Use after installing "
                        "a mod or changing the game folder.")},
        {QStringLiteral("hotkeys/viewPanel"),
         QStringLiteral("Viewport settings panel"), QStringLiteral("N"),
         QStringLiteral("Graphics, lighting and camera over the viewport — the "
                        "same card the bar's buttons open. Blender's key.")},
        // ── The viewport's own keys ─────────────────────────────────────
        // Here, not hard-coded in GLModelWidget, for the reason at the top of
        // this file — and for two the viewport taught us directly. A hard-coded
        // key cannot be rebound when it COLLIDES: measured, "&Help" in the menu
        // bar claims Alt+H, and Qt then resolves the pair as an ambiguous
        // overload and fires NEITHER. And a hard-coded handler matched Ctrl+H,
        // Meta+H and Ctrl+Alt+H as if they were H, because it tested for the
        // modifiers it wanted instead of for the sequence it meant.
        // BLENDER'S THREE, in Blender's arrangement. They were Shift+H for
        // "show all" and Alt+H for "isolate", which is the pair the wrong way
        // round: in Blender — and in every tool that copied it — Alt+H is
        // REVEAL and Shift+H hides everything except the selection. Anyone
        // arriving with the muscle memory pressed Alt+H to get their model
        // back and hid the rest of it instead.
        {QStringLiteral("hotkeys/viewHide"),
         QStringLiteral("Hide the selected parts"), QStringLiteral("H"),
         QStringLiteral("Hides everything selected in the viewport. "
                        "Blender's key.")},
        {QStringLiteral("hotkeys/viewShowAll"), QStringLiteral("Show every part"),
         QStringLiteral("Alt+H"),
         QStringLiteral("Undoes every hide, in one press — Blender's reveal. "
                        "If the menu bar has claimed Alt+H on your system, "
                        "rebind it here; that collision is why this row "
                        "exists.")},
        {QStringLiteral("hotkeys/viewIsolate"),
         QStringLiteral("Isolate the selected parts"),
         QStringLiteral("Shift+H"),
         QStringLiteral("Hides everything EXCEPT what is selected.")},
        {QStringLiteral("hotkeys/viewFrame"),
         QStringLiteral("Frame the selected part"), QStringLiteral("."),
         QStringLiteral("Moves the camera to the picked part. The whole scene "
                        "when nothing is picked. Blender's key.")},
        {QStringLiteral("hotkeys/viewFullscreen"),
         QStringLiteral("Fullscreen viewport"), QStringLiteral("F"),
         QStringLiteral("Everything but the viewport gets out of the way. Esc "
                        "or the floating button brings it back.")},
        {QStringLiteral("hotkeys/viewHelp"),
         QStringLiteral("Viewport shortcut list"), QStringLiteral("F1"),
         QStringLiteral("Every viewport key, over the viewport.")},
    };
}

// Resolve one shortcut from settings (falling back to its default). An empty
// result is "unbound" and callers must treat it as such rather than binding an
// empty sequence, which Qt would match against every keystroke.
inline QKeySequence seq(const QString& key, const QString& def)
{
    const QString s = QSettings().value(key, def).toString();
    return s.isEmpty() ? QKeySequence() : QKeySequence(s);
}

// The same, by key alone — the default comes from the table, so a caller does
// not have to carry it.
inline QKeySequence seq(const QString& key)
{
    for (const Def& d : defs())
        if (d.key == key) return seq(d.key, d.def);
    return QKeySequence();
}

// ── How a hotkey finds the action it fires ──────────────────────────────────
// By ROLE, stamped on the QAction, and never by matching its text. Matching
// text was tried and was wrong twice over in one build: "Export selection…"
// looked for an action containing "export" and found "Export settings…"
// instead (so Ctrl+E opened the settings dialog), while "Save the viewport
// image…" looked for "save image" and matched nothing at all, because the
// action is called "Save viewport image…". A label is a thing that gets
// reworded; a role is a thing that gets renamed by a compiler.
//
// Every builder that creates an export action stamps one of these on it with
// setRole(); MainWindow::triggerExportAction finds it with findRole().
namespace Role {
inline const char* kProperty = "foxabExportRole";
// Stamp a role on an action, and find one in a menu tree. Two lines, here
// rather than duplicated at each builder, so the property name has one spelling.
inline QAction* set(QAction* a, const QString& role)
{
    if (a) a->setProperty(kProperty, role);
    return a;
}
inline QAction* find(const QList<QAction*>& acts, const QString& role)
{
    for (QAction* a : acts) {
        if (!a) continue;
        if (a->menu()) {
            if (QAction* hit = find(a->menu()->actions(), role)) return hit;
            continue;
        }
        if (a->property(kProperty).toString() == role) return a;
    }
    return nullptr;
}
inline QString exportSelection()  { return QStringLiteral("export.selection"); }
inline QString exportAnimations() { return QStringLiteral("export.animations"); }
inline QString saveImage()        { return QStringLiteral("capture.image"); }
inline QString turntable()        { return QStringLiteral("capture.turntable"); }
inline QString animGif()          { return QStringLiteral("capture.animgif"); }
}  // namespace Role

}  // namespace Hotkeys
