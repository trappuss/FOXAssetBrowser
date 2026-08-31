// TexturesTab.h — the Textures tab (template §7): every indexed .ftex,
// searchable, decoded in-tool, with everything the file can be asked about.
//
// Three columns, which is the shape §7 describes and the shape the D4 tool
// has: the LIST on the left, the VIEWER in the middle with its channel strip
// underneath, and on the right the panel that answers "what is this and what
// is it on" (view/TextureInfoPanel.h).
//
// The filters are the three §7 names — format, the tags of the assets that USE
// the texture, and orphans-only — and the last two are only possible because
// index/TextureUsers.h has walked the models once.
#pragma once
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QWidget>
#include <QVector>

#include "view/DisplayModeButton.h"
#include "view/FilterChips.h"
#include "view/NPanel.h"

class PreviewPane;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;
namespace fox { class FilterPopup; }
class QListView;
class QSpinBox;
class QTimer;

namespace fox {
class ChannelStrip;
class TextureInfoPanel;
}

class TexListModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit TexListModel(QObject* parent = nullptr);
    // `usedFilter`: 0 = every texture, 1 = only textures a model uses, 2 =
    // only orphans. Ignored until the texture→model sweep is ready, because
    // calling a texture an orphan on a half-built map is a wrong answer, not a
    // provisional one.
    void refresh(const QString& query, bool namedOnly, int usedFilter,
                 const QString& userTag);
    int rowCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    int fileIdxAt(const QModelIndex& index) const;
    // Replace the row set wholesale — the format filter's second pass, which
    // cannot be folded into refresh() because it reads files and refresh() must
    // stay a pure walk of the index.
    void setRows(const QVector<int>& rows);
    // Which row shows this file, or -1. Used to put the selection back where it
    // was after a refresh rebuilt the list.
    int rowOfFile(int fileIdx) const;
    int total() const { return m_rows.size(); }

    // Roles the grid delegate reads. Kept off Qt::DisplayRole so the list view
    // and the grid can show different things from the same model.
    enum Role {
        FileIdxRole = Qt::UserRole + 400,
        StemRole,      // "cm_flat_gry128"
        DirRole,       // the path without the file name
    };

    // ── Drag-out (§7: "drag the image straight out into another
    // application"). The row carries the decoded texture BOTH as image data
    // and as a URL to a .png in the temp folder, because the two kinds of drop
    // target want different things: an image editor takes the pixels, and a
    // file manager or a game engine's asset browser takes a file.
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;

private:
    QVector<int> m_rows;
};

class QMenu;

// One grid cell: the decoded texture above its name, the folder under it in
// smaller type. The decode happens on a worker thread (TexThumbCache); a cell
// whose texture has not arrived draws a placeholder box rather than blocking
// the paint, and repaints itself when the decode lands.
class TexGridDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit TexGridDelegate(QObject* parent = nullptr);
    void setIconSize(int px) { m_icon = px; }
    int iconSize() const { return m_icon; }
    // While this is running a scroll has not settled and paint() must NOT queue
    // the cells it is drawing — they are flying past.
    void setSettleTimer(const QTimer* t) { m_settle = t; }
    QSize cellSize() const;
    void paint(QPainter* p, const QStyleOptionViewItem& o,
               const QModelIndex& i) const override;
    QSize sizeHint(const QStyleOptionViewItem& o,
                   const QModelIndex& i) const override;

private:
    int m_icon = 112;
    const QTimer* m_settle = nullptr;
};

class TexturesTab : public QWidget {
    Q_OBJECT
public:
    explicit TexturesTab(QWidget* parent = nullptr);

    // Contextual entries for the menu-bar Export menu (current texture).
    void populateExportMenu(QMenu* menu);
    // Dev harness: drive the search box and the grid from the command line.
    void setSearchText(const QString& text);
    // Dev harness: drive the §7 filters and the channel. Each returns false
    // when the value names nothing, so a typo in a script fails loudly rather
    // than silently measuring the unfiltered list.
    bool setUsedFilter(const QString& name);
    bool setFormatFilter(const QString& name);
    bool setUserTagFilter(const QString& tag);
    bool setChannel(const QString& name);
    // True once the texture→model sweep has finished — the harness waits on it
    // before measuring a filter that depends on it.
    bool usersReady() const;
    // The sweep has STOPPED, one way or another — finished, or abandoned by a
    // rescan. The harness waits on this rather than on ready(), so an abandoned
    // sweep ends the wait instead of burning its whole timeout.
    bool usersDone() const;
    // Bring the two sweep-backed controls into line with the sweep's state.
    void syncUsersUi();
    void setGridForShot(bool on, int iconPx);
    int matchCount() const;

signals:
    void gameFilterChanged();
    // ASSOCIATED MODELS was double-clicked. MainWindow takes it to the Models
    // tab — §7's "jumps to the Browse tab".
    void openModelRequested(const QString& modelPath);
    // A row the map knows only by hash. There is nothing to jump to, and
    // saying so is better than a double-click that does nothing.
    void unnamedModelActivated(quint64 modelHash);

public slots:
    // --npanel, for this tab. Same contract as the Models tab's.
    QString setPanelsForShot(const QString& keys);
    void onIndexReady(bool ready);
    // The game filter is one global switch drawn in more than one tab. Re-read
    // it and refresh — called when another tab changes it.
    void syncGameFilter();

    // Grid view: switch modes and keep the visible cells decoded.
    void setGridMode(bool on);
    void setIconSize(int px);
    void decodeVisible();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void refresh();
    // The format filter reads .ftex headers, which no index holds — so it is
    // applied LAST, over whatever the other filters left, and remembers what it
    // read for the rest of the session. See the .cpp for why it is bounded.
    // False when the filter was REFUSED (too many rows to read headers for).
    bool applyFormatFilter();
    void rebuildUserTagBox();

    QLineEdit* m_search = nullptr;
    // The funnel and what it holds (template §4). m_gameRow and m_filterRow are
    // built as page rows and then handed to the popup, which reparents them —
    // they are never on the page.
    QToolButton* m_filterBtn = nullptr;
    fox::FilterPopup* m_filterPopup = nullptr;
    QWidget* m_filterRow = nullptr;
    // The popup's footer line: how many textures survived and which filters
    // are doing it. See the definition.
    QString filterSummary() const;
    void refreshFilterButton();

public:
    // --filterpopup: open the funnel so a shot can photograph it.
    bool openFilterPopupForShot();

private:
    QCheckBox* m_namedOnly = nullptr;
    // The display switch replaced the "Grid" checkbox; the chips show what
    // the search box is filtering by. Both are the Models tab's controls, from
    // the same classes.
    fox::DisplayModeButton* m_display = nullptr;
    fox::FilterChips* m_chips = nullptr;
    fox::NPanel* m_npanel = nullptr;
    QComboBox* m_usedBox = nullptr;     // every / used / orphans
    QComboBox* m_userTagBox = nullptr;  // tags of the models that use it
    QComboBox* m_formatBox = nullptr;   // DXT1 / DXT5 / A8R8G8B8 / L8
    fox::ChannelStrip* m_strip = nullptr;
    fox::TextureInfoPanel* m_panel = nullptr;
    QLabel* m_sweepLabel = nullptr;
    QLabel* m_formatNote = nullptr;
    QWidget* m_sliceRow = nullptr;
    QLabel* m_sliceLabel = nullptr;
    QSpinBox* m_sliceBox = nullptr;
    QWidget* m_gameRow = nullptr;
    QVector<QPair<QCheckBox*, int>> m_gameBoxes;
    QLabel* m_count = nullptr;
    QListView* m_list = nullptr;
    TexListModel* m_model = nullptr;
    TexGridDelegate* m_gridDelegate = nullptr;
    QStyledItemDelegate* m_listDelegate = nullptr;
    QTimer* m_thumbTimer = nullptr;
    QTimer* m_thumbRepaint = nullptr;
    PreviewPane* m_preview = nullptr;
    bool m_gridMode = false;
    int m_current = -1;
};
