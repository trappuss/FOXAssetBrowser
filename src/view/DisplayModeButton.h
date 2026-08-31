// DisplayModeButton.h — the List / Outliner / Grid switch (template §4).
//
// "Three views of one list, switched from a display dropdown in the header."
// What was here was a checkbox labelled "Grid", which is a two-state control
// standing in for a three-state one, and which said what the OTHER state was
// called only by not being ticked.
//
// It is a QToolButton with a menu rather than a QComboBox for a specific
// reason: the menu carries more than the three modes. §4 asks for per-mode
// OPTIONS — the grid's icon size, whether the outliner remembers what was
// expanded and what it opens automatically — and a combo box has nowhere to
// put them. They go in a submenu under the modes, which is where the same
// options live in every application that has this control.
//
// The mode is persisted as a stable STRING id ("list", "outliner", "grid"),
// never a combo index (§3.1). The old boolean key is read once and translated,
// so a user who had the grid on keeps it — and the key to translate FROM is
// derived from the settings prefix inside this class rather than passed in by
// the caller, because it belongs to the mode this class owns. It was a
// parameter nobody passed, which meant the migration this paragraph promised
// never ran and an upgrade with the grid on landed silently on List.
#pragma once
#include <QString>
#include <QStringList>
#include <QToolButton>

class QAction;
class QMenu;
class QActionGroup;

namespace fox {

// The ids. Free functions rather than an enum, because they are what goes into
// QSettings and what comes back out of it, and an enum would need a converter
// at both ends that could disagree with itself.
namespace displaymode {
inline QString list() { return QStringLiteral("list"); }
inline QString outliner() { return QStringLiteral("outliner"); }
inline QString grid() { return QStringLiteral("grid"); }
// Read `key` (and the legacy boolean at `legacyGridKey`, when given), and
// answer one of the three. An unknown stored value reads as List — the safe
// default, per §3.1.
QString restore(const QString& key, const QString& legacyGridKey = QString());
}  // namespace displaymode

class DisplayModeButton : public QToolButton {
    Q_OBJECT
public:
    // `settingsKey` is where the chosen mode is stored, e.g. "models/display".
    DisplayModeButton(const QString& settingsKey, QWidget* parent = nullptr);

    QString mode() const { return m_mode; }
    void setMode(const QString& id);

    // Where the owner hangs its per-mode options: THIS menu, under a
    // separator after the three modes. It was a submenu; four rows behind a
    // second click is a second place to look for a list that fits.
    QMenu* optionsMenu() const { return m_options; }
    // Empty the owner's section — everything after the three modes. The owner
    // rebuilds its options when the menu opens; it must NOT call clear() on
    // the menu optionsMenu() returns, because that menu is the mode menu.
    void clearOptions();

    // Offer only these modes. The Textures tab has no outliner — there is no
    // per-texture tree to grow — and a mode that resolves to nothing is worse
    // than an absent one. A restricted set that excludes the CURRENT mode
    // falls back to the first offered, so a stale setting cannot strand a tab
    // on a view it does not have.
    void setModes(const QStringList& ids);

Q_SIGNALS:
    void modeChanged(const QString& id);

private:
    void syncFace();

    QString m_key;
    QString m_mode;
    QMenu* m_menu = nullptr;
    QMenu* m_options = nullptr;
    QAction* m_optionsFrom = nullptr;   // the separator the owner's section follows
    QActionGroup* m_group = nullptr;
};

}  // namespace fox
