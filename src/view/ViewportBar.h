// ViewportBar.h — the controls that sit ON the viewport (template §5).
//
// Blender's arrangement, which D4AssetBrowser adopted and this is the port of:
// a row of SHADING BALLS top-right that says what you are looking at, a caret
// beside them that picks a single material channel to look at instead, and
// small buttons that open POPOVERS — Graphics, Lighting, Camera — anchored to
// the button that opened them. Not a dialog, and not a panel that takes layout
// space away from the model.
//
// ── The master overlay gate (template §3.3) ──────────────────────────────
//
// `ViewportOverlays` is the gate. Every overlay in the viewport is switched
// through it and never directly, which is the rule this file exists to make
// cheap to obey. The bug it prevents is specific and has bitten D4: a settings
// replay, a scene load or a "restore my layout" path calls setShowSkeleton(true)
// on its own, and an overlay the user switched off comes back with no one
// having asked for it. Here there is exactly one place that can turn an
// overlay on — reapply() — and it will not do so while the master switch is
// off, so a replay can write whatever it likes into the desired state and the
// screen still shows what the user chose.
#pragma once
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

#include "gl/GLModelWidget.h"

class GLModelWidget;
class QBoxLayout;
class QMenu;
class QToolButton;

namespace fox {

class ViewportPanel;

// The desired overlay state for ONE viewport, plus the master switch over it.
// Held by value on the bar; the tabs reach it through the bar.
struct ViewportOverlays {
    bool master = true;      // the gate itself
    bool stats = false;
    bool grid = false;
    bool axes = false;
    bool skeleton = false;
    bool boneNames = false;
    bool connectPoints = false;
    // The axis gizmo (view/ViewportGizmo.h). ON by default, unlike every other
    // overlay here: it is a CONTROL, not an annotation — it is how you get to
    // an axis view and to orthographic — and a control that ships switched off
    // is a control nobody finds. §3.6: defaults are chosen.
    bool gizmo = true;
    // The selection silhouette. An overlay by the same definition as the rest
    // — something drawn OVER the model that is not the model — so it obeys the
    // master switch: turning overlays off gives you the render, with nothing
    // of the tool in it. ON by default, because a selection you cannot see is
    // not a selection.
    bool selection = true;
};

// The bar itself: a floating strip of controls parented to the viewport.
class ViewportBar : public QWidget {
    Q_OBJECT
public:
    // Parents itself to `view`, positions itself top-right and follows the
    // viewport's resizes. `panel` supplies the three popovers; may be null,
    // in which case only the shading balls and the channel caret are built.
    ViewportBar(GLModelWidget* view, ViewportPanel* panel);

    // THE ONLY WAY overlays reach the viewport. Pushes the desired state
    // through the master switch: everything off while the switch is off, the
    // desired state when it is on. Idempotent, and safe to call from a
    // settings replay — which is the point.
    void reapplyOverlays();

    ViewportOverlays& overlays() { return m_overlays; }
    const ViewportOverlays& overlays() const { return m_overlays; }
    // Set one overlay's DESIRED state and reapply. Never touches the master.
    void setOverlay(const QString& key, bool on);
    // The master switch. Turning it off hides every overlay without forgetting
    // which ones were on, which is what makes it a gate rather than a "clear".
    void setOverlaysMaster(bool on);

    // Dev harness: open one popover by name ("graphics", "lighting",
    // "camera"), so a screenshot can show it. False when there is no such one.
    bool openPopover(const QString& name);

    // One overlay's stable key and what the panel calls it. THE table — the
    // Overlays page is built from it, so the list of overlays exists once.
    struct OverlayDef { QString key; QString label; };
    static const QVector<OverlayDef>& overlayDefs();
    bool overlayState(const QString& key) const;
    // Dev harness: pop the overlay caret's menu WITHOUT blocking, so a
    // headless run can photograph it. exec() would wait for a dismissal
    // that is never coming.
    // Dev harness: open the Overlays PAGE and report what it offers —
    // "Show overlays=1 | Statistics=0 | …".
    QString openOverlayMenuForShot();
    // Dev harness: send `n` wheel steps to the channel caret and report where
    // the channel ended up. The list used to WRAP, so scrolling past the end
    // silently landed back at the top — invisible in a screenshot, and the
    // reason the caret felt like an infinite scroller.
    QString scrollChannelForShot(int steps);
    // Dev harness: the channel menu, open, for the same reason.
    bool openChannelMenu();

signals:
    // The gate was reapplied. The Files preview's Skeleton glyph — the last
    // one left in the application — follows the DESIRED
    // state from here rather than the viewport's actual one: with the master
    // switch off, asking for the skeleton changes nothing in the viewport, so
    // GLModelWidget emits no displayChanged and followDisplayState never
    // corrects the button — which left it lit over an empty screen.
    void overlaysChanged(const ViewportOverlays& state);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;


private:
    void reposition();
    void syncFromView();
    void buildChannelMenu();

    QPointer<GLModelWidget> m_view;
    QPointer<ViewportPanel> m_panel;
    QVector<QToolButton*> m_balls;     // one per ShadingMode, in order
    QToolButton* m_channel = nullptr;
    QToolButton* m_overlayBtn = nullptr;
    QVector<QToolButton*> m_popoverBtns;
    QMenu* m_channelMenu = nullptr;
    ViewportOverlays m_overlays;
    bool m_syncing = false;
};

}  // namespace fox
