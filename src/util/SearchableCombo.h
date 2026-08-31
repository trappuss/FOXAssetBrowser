// SearchableCombo.h — a combo box you can type into, showing three lines per
// item: a name, the file it comes from, and its full path.
//
// The builder combos routinely hold hundreds of entries (73 magazines, 62
// receivers, and a full character list), which is more than a plain drop-down
// can be scrolled through usefully. Typing with the popup open filters by
// substring across ALL THREE lines, so "mrs", "ar02" and "collectible/chimera"
// all find the same part.
//
// Items are added with addRichItem(); the three lines are stored as item data
// and drawn by one delegate, so every combo in the app looks the same and the
// colouring comes from the palette rather than hardcoded values (it has to
// stay readable under a dark theme).
#pragma once
#include <QComboBox>
#include <QSet>
#include <QString>
#include <QStyledItemDelegate>

namespace richcombo {

// Item data roles for the three lines.
enum Role {
    NameRole = Qt::UserRole + 100,   // "AM MRS-4" / "ar02_main0_def"
    FileRole,                        // "ar02_main0_def.fmdl"
    PathRole,                        // "/Assets/tpp/weapon/asr/Scenes/…"
    PayloadRole,                     // the caller's value (a file index)
    IconStemRole,                    // model stem to draw the game's icon for
    HeaderRole,                      // true: a group caption, not a choice
    PresetRole,                      // which of the game's numbered presets
                                     // this row is, or -1
    SwatchRole,                      // UI texture path drawn as a colour
                                     // swatch: full-colour art, so it is drawn
                                     // square and NOT tinted the way the white
                                     // line-art part icons are
};

// How tall a part icon is drawn in a list row. The icons are line art on
// transparency, trimmed to their alpha box, so this is the real drawn height.
constexpr int kIconHeight = 42;
constexpr int kIconWidth = 88;
// The closed combo shows the fitted part's icon too, smaller.
constexpr int kClosedIconHeight = 20;
constexpr int kClosedIconWidth = 44;
// A colour swatch is square — the art is a tiling pattern, not a silhouette.
constexpr int kSwatchSize = 34;
constexpr int kClosedSwatchSize = 18;

// Draws name / file / path on three lines in decreasing emphasis.
class RichDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit RichDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

}  // namespace richcombo

class SearchableCombo : public QComboBox {
    Q_OBJECT
public:
    explicit SearchableCombo(QWidget* parent = nullptr);

    // Adds one item. `name` is the headline, `file`/`path` the two subtexts,
    // `payload` whatever the caller wants back from currentPayload().
    void addRichItem(const QString& name, const QString& file, const QString& path,
                     const QVariant& payload);
    // Same, plus the model stem whose in-game icon should be drawn at the left
    // of the row. An empty stem, or a part the game gives no icon, simply draws
    // no icon and the text starts where it otherwise would.
    void addRichItem(const QString& name, const QString& file, const QString& path,
                     const QVariant& payload, const QString& iconStem);
    // Same, but the row's picture is a colour swatch addressed by its own UI
    // texture path (the customize screen's camouflage and paint chips).
    void addSwatchItem(const QString& name, const QString& file,
                       const QString& path, const QVariant& payload,
                       const QString& swatchPath);
    // A plain row (separators, "— none —") with no subtexts.
    void addPlainItem(const QString& text, const QVariant& payload);
    // A group caption — "HANDGUN", "ALL OTHER RECEIVERS". Drawn small, bold
    // and dimmed under a rule, and made unselectable so arrow keys and the
    // mouse skip straight over it to the first real row beneath.
    void addHeaderItem(const QString& text);

    // Give the CURRENT item the game's icon, so the closed combo shows what is
    // fitted. Cheap: one decode per call, cached thereafter.
    void refreshCurrentIcon();

    QVariant currentPayload() const;
    // Select the row whose payload equals `payload`; returns false if absent.
    bool selectPayload(const QVariant& payload);

    // Also forgets which rows drew rendered-model swatches.
    void clear();

    void showPopup() override;
    void hidePopup() override;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void applyFilter(const QString& needle);
    QString m_filter;
    // File indices this combo draws as "model:" swatches, so the
    // thumbnail-ready signal can be ignored unless it is one of ours.
    QSet<int> m_modelSwatches;
};
