// NPanel.h — the right-hand panel column (template §6).
//
// One column, one class, both model viewports. What used to be here was three
// SEPARATE panes sitting side by side in the tab's main splitter — PARTS,
// ANIMATIONS, MATERIALS — each toggled by its own button on the toolbar above
// the viewport. Three panes wide is how a 1080p window ends up with a viewport
// narrower than the lists beside it, and a toolbar of panel switches is a
// second place to look for something the panels themselves could say.
//
// So: ONE column, opened by a small arrow on the viewport's right edge, with a
// vertical ICON STRIP down its far side. Every panel is a PanelBox in a
// vertical QSplitter — several stack at once, the handles size them, ▲▼
// reorder them, ✕ hides one, and the strip toggles any of them. This is
// Blender's N-panel and D4AssetBrowser's right-hand column; it is not a new
// idea and it deliberately does not look like one.
//
// WHAT IS PERSISTED, and why it is three keys rather than one blob:
//   <prefix>/open  — which panels are up, as a comma-separated list of stable
//                    string KEYS. Never indices: inserting a panel in the
//                    middle would silently re-point every stored index at its
//                    neighbour (template §3.1).
//   <prefix>/order — the user's ▲▼ ordering, same encoding. Unknown keys are
//                    dropped on read and registered-but-unlisted keys are
//                    appended in registration order, so a build that adds a
//                    panel does not scramble a saved layout.
//   <prefix>/stack — the splitter's own sizes, through PanelPersist, which is
//                    already gated on "remember panel sizes".
// A single blob would have coupled the three: changing your mind about the
// sizes would have rewritten which panels were open.
#pragma once
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QSplitter;
class QToolButton;
class QVBoxLayout;

namespace fox {

class PanelBox;

class NPanel : public QWidget {
    Q_OBJECT
public:
    // `prefix` is the settings prefix, e.g. "models/npanel". Two viewports
    // remember their columns separately because they hold different panels.
    explicit NPanel(const QString& prefix, QWidget* parent = nullptr);

    // Register a panel. `key` is the stable settings id, `glyph` a
    // foxglyph::toolGlyph kind for the strip button. `content` is reparented.
    // Registration order is the default stacking order. Panels start CLOSED;
    // restoreState() decides what is actually up.
    void addPanel(const QString& key, const QString& title, int glyph,
                  QWidget* content, const QString& tip = QString());

    // Apply the remembered open set and ordering. Call once, after every
    // addPanel — a restore that runs mid-registration can only see half the
    // panels and would write that half back out. `fallback` is what comes up
    // the very first run, before anything has been remembered.
    void restoreState(const QStringList& fallback);

    bool isPanelOpen(const QString& key) const;
    void setPanelOpen(const QString& key, bool on);
    void togglePanel(const QString& key) { setPanelOpen(key, !isPanelOpen(key)); }

    // The panel header's label, so the owner can write live counts into it
    // ("PARTS · 3 of 5 shown"). Null for an unregistered key.
    QLabel* titleLabel(const QString& key) const;
    QWidget* panelContent(const QString& key) const;
    QStringList panelKeys() const;

    // The column as a whole. Collapsed leaves ONLY the icon strip, so the
    // panels are still one click away and the arrow is not the only way back —
    // a collapse that hides its own re-open control is a trap.
    bool columnOpen() const { return m_columnOpen; }
    void setColumnOpen(bool on);
    void toggleColumn() { setColumnOpen(!m_columnOpen); }

    // Float the open/close arrow on `viewport`'s right edge (the "small arrow
    // with a grey background"). It follows the viewport's resizes and hides
    // itself while the viewport is in fullscreen, where the column is gone.
    // Dev harness: the stack's current pixel heights, and a forced
    // redistribution, so "can these actually be dragged" is a measurement
    // rather than an opinion. Returns one line describing before and after.
    QString probeSizesForShot();

    void attachToggle(QWidget* viewport);
    // Hide the arrow without forgetting it. The owning tab calls this on the
    // way into a fullscreen viewport: the arrow is a child of the viewport, so
    // a sweep that hides the viewport's SIBLINGS never reaches it, and left up
    // it opens a column that is behind the fullscreen viewport.
    void setToggleVisible(bool on);

Q_SIGNALS:
    void panelOpenChanged(const QString& key, bool open);
    void columnOpenChanged(bool open);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    struct Entry {
        QString key;
        PanelBox* box = nullptr;
        QToolButton* strip = nullptr;
    };

    Entry* find(const QString& key);
    const Entry* find(const QString& key) const;
    void movePanel(const QString& key, int delta);
    void saveOpen() const;
    void saveOrder() const;
    void updateColumnWidth();
    void repositionArrow();
    void syncArrow();

    QString m_prefix;
    QSplitter* m_stack = nullptr;
    QWidget* m_strip = nullptr;
    QVBoxLayout* m_stripLayout = nullptr;
    QVector<Entry> m_entries;
    QPointer<QToolButton> m_arrow;
    QPointer<QWidget> m_viewport;
    bool m_columnOpen = true;
    bool m_restoring = false;
};

}  // namespace fox
