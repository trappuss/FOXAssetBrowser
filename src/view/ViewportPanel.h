// ViewportPanel.h — the viewport POPOVERS: Graphics, Lighting, Camera.
//
// Blender's idea, and D4AssetBrowser's before this one: the controls that only
// make sense while you are looking at the thing live ON the thing, not in a
// dialog and not in a strip of checkboxes across the top. It is an overlay
// child of the GLModelWidget, so one class serves all three viewports (Models,
// Customize, the Files preview pane) and none of them has to give up layout
// space for it.
//
// It WAS one card with a tab strip — the "N-panel". Template §5 asks for
// popovers instead: small floating panels positioned next to the button that
// opened them, one topic each. That is what showPopover() does, and it is why
// the tab strip is gone: with three buttons on the viewport bar each opening
// its own topic, a second row of tabs inside the card was a way of getting to
// a page you had already chosen. The N key still toggles the last one.
//
// WHAT IS AND IS NOT IN HERE. Everything on these pages is pure viewport
// state: changing it repaints and nothing else. Deliberately absent is
// "Normal maps", which RELOADS the scene's textures on the tab that owns it —
// a reload driven from a floating panel is how you get two half-loaded scenes.
// It is on the Graphics page all the same — the reload is the OWNING TAB's
// job and each tab watches displayChanged for it — which is the one place
// this paragraph is now out of date about. The shading mode
// (which subsumes wireframe and the PBR switch) is on the bar itself, as the
// row of shading balls.
//
// The OVERLAY switches are here, on Graphics, and they write into the bar's
// ViewportOverlays through its gate — never into the viewport directly. See
// ViewportBar.h for why that rule exists.
#pragma once
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

#include "util/PartMenu.h"

#include "gl/ViewEnvironment.h"

class GLModelWidget;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QToolButton;
class QScrollArea;
class QStackedWidget;
class QBoxLayout;
class QMenu;

namespace fox {

class ViewportPanel;
class ViewportBar;

// Create the viewport's on-screen furniture: the bar (shading balls, channel
// caret, overlay gate, popover buttons), the popovers themselves, the text HUD
// and the shared right-click menu. Every viewport gets them the same way; this
// is that way, written once.
//
// It takes NO toolbar. It used to add a "View settings" button to the owning
// page's toolbar, which opened the same three popovers as the buttons on the
// viewport's own bar a few pixels below it. Everything this function installs
// lives ON the viewport, which is the whole point of §5.
// `extraMenu` adds the OWNING PAGE's entries to the viewport's right-click
// menu. It exists so a page does not install a second handler on the same
// signal, which is two menus in a row rather than one menu with more in it.
ViewportPanel* attachViewportPanel(GLModelWidget* view,
                                   std::function<void(partmenu::Context&, int)> pageContext = {});

// The bar attached to `view` by attachViewportPanel, or null. This is how a
// tab or the devshot harness reaches the overlay gate — there is no second
// path to it on purpose.
ViewportBar* viewportBarFor(GLModelWidget* view);

// Keep a tab's own wireframe/skeleton toggles in step with the view.
//
// The FILES PREVIEW is the one page that still has a toolbar, and two
// controls drive each of those toggles there — its toolbar and this panel —
// so the toolbar cannot assume it is the only thing touching them. The two
// model tabs have no toolbar at all any more and do not call this. Without this the panel's tick leaves that toolbar glyph unlit,
// and the next click on that glyph asks the view for a state it is already in,
// which setWireframe() correctly ignores: a dead click that does nothing and
// looks broken. Either pointer may be null.
void followDisplayState(GLModelWidget* view, QToolButton* wireframe,
                        QToolButton* skeleton);

class ViewportPanel : public QWidget {
    Q_OBJECT
public:
    // Parents itself to `view` and installs an event filter on it: the filter
    // repositions the card when the viewport resizes and answers the N key.
    // Starts hidden.
    explicit ViewportPanel(GLModelWidget* view);

    void setPanelOpen(bool on);
    bool panelOpen() const { return isVisible(); }
    void togglePanel() { setPanelOpen(!panelOpen()); }
    // Which page is shown: 0 Graphics, 1 Lighting, 2 Camera. Opening a page
    // also opens the card.
    void showPage(int page);
    // Open one topic as a POPOVER anchored under `anchor` (template §5).
    // `key` is "graphics" / "lighting" / "camera". False for an unknown key.
    bool showPopover(const QString& key, QWidget* anchor);
    // For the devshot harness and for a settings restore: the page ids by
    // name, so a caller does not have to know the numbering. The old four
    // names still resolve — "display" is Graphics and "world" is Lighting —
    // because they are in recorded harness commands and in this project's own
    // documents.
    static int pageIndexFor(const QString& name);
    // The bar this panel's popovers belong to. Set once by
    // attachViewportPanel; the overlay checkboxes write through it. Defined
    // out of line so this header does not have to include ViewportBar.h,
    // which includes this one.
    void setBar(ViewportBar* bar);

signals:
    // Shown or hidden, however it happened — the N key or
    // the card's own ✕. A toggle button left lit over a closed panel is the
    // kind of small lie that makes a UI feel broken.
    void openChanged(bool open);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    // Switch page WITHOUT opening the panel. showPage() is setPage() plus
    // open; the constructor needs the first half on its own or the card
    // flashes up and is hidden again before the first paint.
    void setPage(int page);
    // The stem a capture's file dialog opens on: whatever the owning tab put
    // on the viewport as the "foxabCaptureName" property, or "viewport".
    QString suggestedName() const;
    void buildGraphicsPage();
    void buildLightingPage();
    void buildCameraPage();
    void buildOverlaysPage();
    // Re-read the overlay checkboxes from the bar's gate.
    void syncOverlayBoxes();
    void reposition();
    void syncFromView();
    void applyEnvironment(int index);

    QPointer<GLModelWidget> m_view;
    QStackedWidget* m_pages = nullptr;
    QScrollArea* m_scroll = nullptr;
    QLabel* m_title = nullptr;
    // Which button this popover was opened from, so it can be positioned next
    // to it. Null means the old top-right corner, which is where the N key
    // (no button involved) still puts it.
    QPointer<QWidget> m_anchor;

    // Graphics
    QComboBox* m_debug = nullptr;
    QLabel* m_debugNote = nullptr;
    QLabel* m_shadingNote = nullptr;
    // The two material switches that used to be glyph toggles on the Models
    // toolbar. Live on the Graphics page beside the channel picker.
    QCheckBox* m_normalMaps = nullptr;
    QCheckBox* m_pbr = nullptr;
    QCheckBox* m_overlayMaster = nullptr;
    // Keyed by the ViewportBar key ("grid", "skeleton", …) so the page and the
    // gate cannot drift apart by a renamed member.
    QVector<QPair<QString, QCheckBox*>> m_overlayBoxes;
    QPointer<ViewportBar> m_bar;

    // Light
    QSlider* m_az = nullptr;
    QSlider* m_el = nullptr;
    QSlider* m_keyGain = nullptr;
    QSlider* m_ambGain = nullptr;
    QSlider* m_exposure = nullptr;
    QCheckBox* m_follow = nullptr;
    QLabel* m_lightRead = nullptr;

    // World
    QComboBox* m_env = nullptr;
    QLabel* m_envNote = nullptr;
    QToolButton* m_bg = nullptr;

    // Camera
    // Camera page: projection, field of view, framing, and the animation GIF.
    QCheckBox* m_ortho = nullptr;
    QSlider* m_fov = nullptr;
    QLabel* m_fovLabel = nullptr;
    QPushButton* m_frameSel = nullptr;
    QCheckBox* m_autoFit = nullptr;
    QPushButton* m_animGif = nullptr;
    QCheckBox* m_turn = nullptr;
    QSlider* m_turnSpeed = nullptr;
    QLabel* m_camRead = nullptr;
    // Enabled only while the viewport has geometry — re-tested on every
    // sceneChanged through syncFromView.
    QVector<QPushButton*> m_captureButtons;

    bool m_syncing = false;
};

}  // namespace fox
