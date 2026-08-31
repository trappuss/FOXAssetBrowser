#include "view/DisplayModeButton.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QSettings>

#include "view/ViewGlyphs.h"

namespace fox {

namespace {

// The boolean key an older build wrote for this settings prefix, so the
// migration lives beside the thing it migrates rather than at the call site.
// "models/display" ← "models/grid"; anything else has no legacy form.
QString legacyGridKeyFor(const QString& key)
{
    if (key == QLatin1String("models/display"))
        return QStringLiteral("models/grid");
    if (key == QLatin1String("textures/display"))
        return QStringLiteral("textures/grid");
    return {};
}

}  // namespace

namespace displaymode {

QString restore(const QString& key, const QString& legacyGridKey)
{
    QSettings s;
    const QString stored = s.value(key).toString();
    if (stored == list() || stored == outliner() || stored == grid())
        return stored;
    // Nothing stored under the new key. A build before this one wrote a
    // BOOLEAN "grid" flag, and a user who had the grid on should not have it
    // silently turned off by an upgrade.
    if (!legacyGridKey.isEmpty() && s.contains(legacyGridKey))
        return s.value(legacyGridKey).toBool() ? grid() : list();
    return list();
}

}  // namespace displaymode

namespace {

struct ModeDef {
    QString (*id)();
    const char* label;
    const char* tip;
};

const ModeDef kModes[] = {
    {&displaymode::list, "List",
     "Dense flat rows: one line per model, the whole path visible. The fastest "
     "of the three to scan, and the only one that stays readable at forty "
     "thousand rows."},
    {&displaymode::outliner, "Outliner",
     "A tree: models under the folders they live in, and the loaded model's "
     "row grows children for its parts. Selecting a part selects it in the "
     "viewport, and the other way round."},
    {&displaymode::grid, "Grid",
     "A thumbnail of each model, rendered by this tool and kept. Only what is "
     "on screen is rendered, so scrolling back costs nothing. Ctrl+wheel "
     "resizes the icons."},
};

}  // namespace

DisplayModeButton::DisplayModeButton(const QString& settingsKey, QWidget* parent)
    : QToolButton(parent), m_key(settingsKey)
{
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setPopupMode(QToolButton::InstantPopup);
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus);
    setIcon(foxglyph::toolIcon(17));
    setIconSize(QSize(foxglyph::kSize, foxglyph::kSize));

    m_menu = new QMenu(this);
    m_group = new QActionGroup(m_menu);
    m_group->setExclusive(true);
    for (const ModeDef& d : kModes) {
        QAction* a = m_menu->addAction(QString::fromLatin1(d.label));
        a->setCheckable(true);
        a->setData(d.id());
        a->setToolTip(QString::fromLatin1(d.tip));
        m_group->addAction(a);
        connect(a, &QAction::triggered, this,
                [this, a] { setMode(a->data().toString()); });
    }
    // Tooltips in a menu are off by default in Qt, and these are where each
    // mode is explained — without this the explanations are unreachable.
    m_menu->setToolTipsVisible(true);
    // A SEPARATOR, not a submenu. The options used to sit behind a "Display
    // options" entry, which for four rows is a second click and a second place
    // to look for a list that fits under the modes with room to spare. The
    // owner still calls optionsMenu() and adds to it; what changed is that the
    // menu it gets back IS this menu.
    m_optionsFrom = m_menu->addSeparator();
    m_options = m_menu;
    setMenu(m_menu);

    // The LEGACY KEY, passed rather than merely available. The header claimed
    // "a user who had the grid on keeps it" and the parameter existed for it,
    // and nobody passed one — so `models/grid` was never read and an upgrade
    // with the grid on silently landed on List. The one caller does not get to
    // know the key, because the key belongs to the mode this class owns.
    m_mode = displaymode::restore(m_key, legacyGridKeyFor(m_key));
    syncFace();
}

void DisplayModeButton::setModes(const QStringList& ids)
{
    if (ids.isEmpty()) return;
    for (QAction* a : m_group->actions())
        a->setVisible(ids.contains(a->data().toString()));
    if (!ids.contains(m_mode)) setMode(ids.first());
    else syncFace();
}

void DisplayModeButton::setMode(const QString& id)
{
    if (id != displaymode::list() && id != displaymode::outliner()
        && id != displaymode::grid())
        return;
    if (m_mode == id) { syncFace(); return; }
    m_mode = id;
    QSettings().setValue(m_key, m_mode);
    syncFace();
    Q_EMIT modeChanged(m_mode);
}

// Remove only what the OWNER added — everything after the separator that ends
// the three modes.
//
// This exists because the owner rebuilds its per-mode options every time the
// menu opens, and its first attempt did that with QMenu::clear() on the menu
// optionsMenu() hands back. That menu IS this menu, so clear() deleted List,
// Outliner and Grid along with the options and the display button lost its
// display modes. The owner must not have to know that; it asks for its own
// section to be emptied and this is the only code that knows where that
// section starts.
void DisplayModeButton::clearOptions()
{
    if (!m_menu || !m_optionsFrom) return;
    const QList<QAction*> all = m_menu->actions();
    const int at = all.indexOf(m_optionsFrom);
    if (at < 0) return;
    for (int i = all.size() - 1; i > at; --i) {
        QAction* a = all[i];
        m_menu->removeAction(a);
        // Submenus added by the owner own their own actions; deleting the
        // action is what takes them with it.
        if (a->menu()) a->menu()->deleteLater();
        a->deleteLater();
    }
}

void DisplayModeButton::syncFace()
{
    for (const ModeDef& d : kModes) {
        if (d.id() != m_mode) continue;
        setText(QString::fromLatin1(d.label));
        setToolTip(QStringLiteral("View: %1\n\n%2")
                       .arg(QString::fromLatin1(d.label),
                            QString::fromLatin1(d.tip)));
        break;
    }
    for (QAction* a : m_group->actions()) {
        const bool on = a->data().toString() == m_mode;
        if (on && !a->isChecked()) a->setChecked(true);
        // ── THE MARK IS AN ICON, NOT THE STYLE'S CHECK INDICATOR ────────
        // "No highlights or checkmarks or anything to show what's activated."
        // A checkable QAction's mark is drawn by the style into a check column
        // the menu only reserves under conditions that are not worth relying
        // on, and this application replaces that primitive anyway
        // (util/CheckStyle.h). An ICON always draws, always reserves its
        // column, and lines every row of the menu up whether it is marked or
        // not — which is also the answer to "hard to read what's toggled".
        a->setIcon(on ? foxglyph::toolIcon(29) : QIcon());
    }
}

}  // namespace fox
