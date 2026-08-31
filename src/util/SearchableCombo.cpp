// SearchableCombo.cpp — see SearchableCombo.h.
#include "util/SearchableCombo.h"

#include "gl/ThumbnailRenderer.h"
#include "index/IconCatalog.h"
#include "util/HoverPreview.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QListView>
#include <QStandardItemModel>
#include <QPainter>
#include <QScreen>

namespace richcombo {

// One swatch resolver for every drawing path. Most swatches are an asset path
// or one of IconCatalog's derived specs; "model:<fileIdx>" is the exception —
// the picture is a RENDER of that model, which only the thumbnail renderer can
// produce, and IconCatalog has no business knowing about OpenGL.
QPixmap swatchPixmap(const QString& spec, int size)
{
    if (spec.startsWith(QLatin1String("model:"))) {
        const int idx = spec.mid(6).toInt();
        for (int sz : {size, 256, 192, 128, 96, 64})
            if (const QPixmap pm =
                    fox::ThumbnailRenderer::instance().cached(idx, sz);
                !pm.isNull())
                return pm.scaled(size, size, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        fox::ThumbnailRenderer::instance().request(idx, qMax(128, size));
        return {};
    }
    return fox::IconCatalog::instance().swatchForPath(spec, size);
}


RichDelegate::RichDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize RichDelegate::sizeHint(const QStyleOptionViewItem& option,
                             const QModelIndex& index) const
{
    const QSize base = QStyledItemDelegate::sizeHint(option, index);
    if (index.data(HeaderRole).toBool()) {
        const QFontMetrics fm(option.font);
        return QSize(base.width(), fm.height() + 12);
    }
    // A plain row (no subtexts) keeps the normal height, so separators and
    // "— none —" do not get three lines of padding.
    if (index.data(FileRole).toString().isEmpty()) return base;
    const QFontMetrics fm(option.font);
    // Room for three lines PLUS descender slack: an underscore sits below the
    // baseline and QPainter clips drawText to the rect it is given, so a rect
    // of exactly font height silently renders "a_b" as "a b".
    const int text = fm.height() * 2 + fm.height() * 4 / 5 + 14;
    int withIcon = text;
    if (!index.data(IconStemRole).toString().isEmpty())
        withIcon = qMax(text, kIconHeight + 8);
    else if (!index.data(SwatchRole).toString().isEmpty())
        withIcon = qMax(text, kSwatchSize + 8);
    return QSize(base.width(), withIcon);
}

void RichDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                         const QModelIndex& index) const
{
    if (index.data(HeaderRole).toBool()) {
        // A caption, not a choice: no highlight, a hairline above it, and the
        // text in the palette's own colour at low strength so it reads as
        // structure rather than as a disabled option.
        painter->save();
        painter->fillRect(option.rect, option.palette.base());
        QColor rule = option.palette.text().color();
        rule.setAlphaF(0.20);
        painter->setPen(rule);
        painter->drawLine(option.rect.left() + 6, option.rect.top() + 1,
                          option.rect.right() - 6, option.rect.top() + 1);
        QFont hf = option.font;
        hf.setBold(true);
        hf.setPointSizeF(hf.pointSizeF() > 0 ? hf.pointSizeF() * 0.82
                                             : hf.pointSize() * 0.82);
        hf.setLetterSpacing(QFont::PercentageSpacing, 112);
        QColor cap = option.palette.text().color();
        cap.setAlphaF(0.62);
        painter->setFont(hf);
        painter->setPen(cap);
        painter->drawText(option.rect.adjusted(8, 3, -6, -1),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(NameRole).toString());
        painter->restore();
        return;
    }
    // A QComboBox separator is a row tagged through this accessibility role.
    // QStyledItemDelegate draws nothing for it — only QComboBox's own private
    // delegate knows about them — so it rendered as a blank gap next to the
    // header captions. Draw the rule ourselves and the two group breaks match.
    if (index.data(Qt::AccessibleDescriptionRole).toString()
        == QLatin1String("separator")) {
        painter->save();
        painter->fillRect(option.rect, option.palette.base());
        QColor rule = option.palette.text().color();
        rule.setAlphaF(0.22);
        painter->setPen(rule);
        const int y = option.rect.center().y();
        painter->drawLine(option.rect.left() + 6, y, option.rect.right() - 6, y);
        painter->restore();
        return;
    }
    const QString file = index.data(FileRole).toString();
    if (file.isEmpty()) {   // plain row — let the base class draw it
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();       // we draw the text ourselves
    // The closed combo shows the current item's decoration; the popup must not
    // draw it a second time next to the one this delegate paints.
    opt.icon = QIcon();
    opt.features &= ~QStyleOptionViewItem::HasDecoration;
    opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter,
                                     opt.widget);

    const bool sel = opt.state & QStyle::State_Selected;
    // Colours come from the palette so the rows stay legible in either theme;
    // the two subtexts are the same hue at decreasing strength rather than
    // arbitrary colours.
    const QColor base = sel ? opt.palette.highlightedText().color()
                            : opt.palette.text().color();
    QColor sub = base;
    sub.setAlphaF(sel ? 0.85 : 0.68);
    QColor faint = base;
    faint.setAlphaF(sel ? 0.70 : 0.45);

    QRect r = opt.rect.adjusted(6, 3, -6, -3);
    // A colour swatch is the game's actual camouflage art: drawn square, at
    // full colour, and never tinted — recolouring a camo pattern to the row's
    // text colour would leave every chip the same flat block.
    const QString swatch = index.data(SwatchRole).toString();
    if (!swatch.isEmpty()) {
        const QPixmap pm =
            swatchPixmap(swatch, kSwatchSize);
        if (!pm.isNull()) {
            const int w = int(pm.width() / pm.devicePixelRatio());
            const int h = int(pm.height() / pm.devicePixelRatio());
            painter->drawPixmap(r.left() + (kSwatchSize - w) / 2,
                                r.top() + (r.height() - h) / 2, pm);
        }
        r.setLeft(r.left() + kSwatchSize + 10);
    }
    // The game's own icon for this part, drawn in a fixed gutter so every row's
    // text starts at the same x whether or not its part has one.
    const QString iconStem = index.data(IconStemRole).toString();
    if (!iconStem.isEmpty()) {
        fox::IconCatalog& icons = fox::IconCatalog::instance();
        QPixmap pm = icons.iconFor(iconStem, kIconHeight);
        if (!pm.isNull()) {
            // Weapon-parts art is white line work on transparency, drawn in
            // game on a dark panel. Recolour it to the row's own text colour so
            // it is visible under a light theme and inverts with the selection,
            // instead of being white-on-white.
            //
            // Only that kind. The suit icons are full-colour photographs of the
            // outfit, and running one through a SourceIn fill replaces the
            // picture with a solid rectangle of the text colour — which is
            // exactly what the avatar's whole wardrobe drew as.
            if (icons.iconIsLineArt(iconStem)) {
                QPixmap tinted(pm.size());
                tinted.setDevicePixelRatio(pm.devicePixelRatio());
                tinted.fill(Qt::transparent);
                QPainter tp(&tinted);
                tp.drawPixmap(0, 0, pm);
                tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
                tp.fillRect(tinted.rect(), base);
                tp.end();
                pm = tinted;
            }
            const int x = r.left() + (kIconWidth - int(pm.width() / pm.devicePixelRatio())) / 2;
            painter->drawPixmap(x, r.top() + (r.height() - kIconHeight) / 2, pm);
        }
        r.setLeft(r.left() + kIconWidth + 8);
    }
    QFont f = opt.font;
    const QFontMetrics fmName(f);
    QFont fSmall = f;
    fSmall.setPointSizeF(f.pointSizeF() > 0 ? f.pointSizeF() * 0.84
                                            : f.pointSize() * 0.84);
    const QFontMetrics fmSmall(fSmall);

    painter->save();
    int y = r.top();
    painter->setFont(f);
    painter->setPen(base);
    painter->drawText(QRect(r.left(), y, r.width(), fmName.height() + 2),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      fmName.elidedText(index.data(NameRole).toString(),
                                        Qt::ElideRight, r.width()));
    y += fmName.height() + 1;
    painter->setFont(fSmall);
    painter->setPen(sub);
    painter->drawText(QRect(r.left(), y, r.width(), fmSmall.height() + 2),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      fmSmall.elidedText(file, Qt::ElideRight, r.width()));
    y += fmSmall.height() + 1;
    painter->setPen(faint);
    // The path is elided from the LEFT: the tail (…/Scenes/foo.fmdl) is what
    // tells them apart, the /Assets/tpp/ head never does.
    painter->drawText(QRect(r.left(), y, r.width(), fmSmall.height() + 2),
                      Qt::AlignLeft | Qt::AlignVCenter,
                      fmSmall.elidedText(index.data(PathRole).toString(),
                                         Qt::ElideLeft, r.width()));
    painter->restore();
}

}  // namespace richcombo

SearchableCombo::SearchableCombo(QWidget* parent) : QComboBox(parent)
{
    auto* view = new QListView(this);
    view->setUniformItemSizes(false);
    setView(view);
    // A "model:" swatch is rendered on a worker thread and lands later. Without
    // this the row that asked for it stays blank until something else happens
    // to repaint the list.
    connect(&fox::ThumbnailRenderer::instance(), &fox::ThumbnailRenderer::ready,
            this, [this](int fileIdx, int) {
                // This signal fires for EVERY thumbnail the models grid renders,
                // and every live combo in the app is connected to it. Scrolling
                // that grid must not walk hundreds of rows in nineteen combos
                // on a tab nobody is looking at, so bail unless this combo is
                // actually showing the model that just landed.
                if (!m_modelSwatches.contains(fileIdx)) return;
                if (QAbstractItemView* v = QComboBox::view())
                    if (v->isVisible()) v->viewport()->update();
                refreshCurrentIcon();
            });
    setItemDelegate(new richcombo::RichDelegate(this));
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(18);
    view->installEventFilter(this);
    installEventFilter(this);
}

void SearchableCombo::addRichItem(const QString& name, const QString& file,
                                  const QString& path, const QVariant& payload,
                                  const QString& iconStem)
{
    addRichItem(name, file, path, payload);
    setItemData(count() - 1, iconStem, richcombo::IconStemRole);
}

void SearchableCombo::addSwatchItem(const QString& name, const QString& file,
                                    const QString& path, const QVariant& payload,
                                    const QString& swatchPath)
{
    addRichItem(name, file, path, payload);
    setItemData(count() - 1, swatchPath, richcombo::SwatchRole);
    if (swatchPath.startsWith(QLatin1String("model:")))
        m_modelSwatches.insert(swatchPath.mid(6).toInt());
}

void SearchableCombo::refreshCurrentIcon()
{
    // A disabled combo is showing something this function did not put there —
    // the red crossed box on a slot the weapon cannot take. Clearing it because
    // the row has no icon stem is exactly wrong, so leave disabled combos
    // alone rather than making every caller remember to.
    if (!isEnabled()) return;
    // Only the selected row gets a decoration, and only when it is selected:
    // building a QIcon for every row would assemble hundreds of .ftex textures
    // the moment a list is populated, which is far too slow. This costs one
    // decode per selection change, and IconCatalog caches it after that.
    const int cur = currentIndex();
    for (int i = 0; i < count(); ++i)
        if (i != cur && !itemIcon(i).isNull()) setItemIcon(i, QIcon());
    if (cur < 0) return;
    const QString sw = itemData(cur, richcombo::SwatchRole).toString();
    if (!sw.isEmpty()) {
        const QPixmap pm = richcombo::swatchPixmap(
            sw, richcombo::kClosedSwatchSize);
        setItemIcon(cur, pm.isNull() ? QIcon() : QIcon(pm));   // never tinted
        setIconSize(QSize(richcombo::kClosedSwatchSize,
                          richcombo::kClosedSwatchSize));
        return;
    }
    const QString stem = itemData(cur, richcombo::IconStemRole).toString();
    if (stem.isEmpty()) {
        setItemIcon(cur, QIcon());
        setIconSize(QSize(0, 0));   // or the empty gutter stays behind
        return;
    }
    fox::IconCatalog& icons = fox::IconCatalog::instance();
    QPixmap pm = icons.iconFor(stem, richcombo::kClosedIconHeight);
    if (pm.isNull()) { setItemIcon(cur, QIcon()); return; }
    // Same rule as the popup rows: recolour white line work so it survives a
    // light theme, and leave a full-colour photograph alone. This is the
    // CLOSED combo and it needs the test just as much — missing it here put a
    // solid black rectangle on every page whose icon comes from the suit
    // table, Snake's included, which is a page with nothing to do with MGO.
    if (icons.iconIsLineArt(stem)) {
        QPixmap tinted(pm.size());
        tinted.setDevicePixelRatio(pm.devicePixelRatio());
        tinted.fill(Qt::transparent);
        QPainter tp(&tinted);
        tp.drawPixmap(0, 0, pm);
        tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tp.fillRect(tinted.rect(), palette().text().color());
        tp.end();
        pm = tinted;
    }
    setItemIcon(cur, QIcon(pm));
    setIconSize(QSize(richcombo::kClosedIconWidth, richcombo::kClosedIconHeight));
}

void SearchableCombo::addRichItem(const QString& name, const QString& file,
                                  const QString& path, const QVariant& payload)
{
    addItem(name, payload);
    const int i = count() - 1;
    setItemData(i, name, richcombo::NameRole);
    setItemData(i, file, richcombo::FileRole);
    setItemData(i, path, richcombo::PathRole);
    setItemData(i, payload, richcombo::PayloadRole);
    // The searchable haystack: all three lines, so any of them can find it.
    setItemData(i, (name + QLatin1Char(' ') + file + QLatin1Char(' ') + path).toLower(),
                Qt::UserRole + 199);
}

void SearchableCombo::addPlainItem(const QString& text, const QVariant& payload)
{
    addItem(text, payload);
    const int i = count() - 1;
    setItemData(i, text, richcombo::NameRole);
    setItemData(i, payload, richcombo::PayloadRole);
    setItemData(i, text.toLower(), Qt::UserRole + 199);
}

void SearchableCombo::addHeaderItem(const QString& text)
{
    addItem(text, QVariant());
    const int i = count() - 1;
    setItemData(i, text, richcombo::NameRole);
    setItemData(i, true, richcombo::HeaderRole);
    // Searchable on its own text, so typing a class name keeps its caption
    // above the rows it introduces instead of orphaning them.
    setItemData(i, text.toLower(), Qt::UserRole + 199);
    // Unselectable: the combo's model is a QStandardItemModel, and clearing the
    // flags is what makes both the mouse and the arrow keys step past it.
    if (auto* m = qobject_cast<QStandardItemModel*>(model()))
        if (QStandardItem* it = m->item(i)) it->setFlags(Qt::NoItemFlags);
}

QVariant SearchableCombo::currentPayload() const
{
    return currentIndex() < 0 ? QVariant() : itemData(currentIndex());
}

bool SearchableCombo::selectPayload(const QVariant& payload)
{
    for (int i = 0; i < count(); ++i)
        if (itemData(i) == payload) { setCurrentIndex(i); return true; }
    return false;
}

void SearchableCombo::clear()
{
    m_modelSwatches.clear();
    QComboBox::clear();
}

void SearchableCombo::showPopup()
{
    m_filter.clear();
    applyFilter(QString());

    // The popup has to be wider than the combo: the combo is sized to the
    // headline, but the popup shows the file name and the full asset path, and
    // at combo width those elide down to nothing. Measure the content and let
    // the list grow up to a share of the screen.
    if (auto* v = view()) {
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() > 0 ? small.pointSizeF() * 0.84
                                                   : small.pointSize() * 0.84);
        const QFontMetrics fmName(font());
        const QFontMetrics fmSmall(small);
        int wanted = 0;
        for (int i = 0; i < count(); ++i) {
            wanted = qMax(wanted, fmName.horizontalAdvance(
                                      itemData(i, richcombo::NameRole).toString()));
            wanted = qMax(wanted, fmSmall.horizontalAdvance(
                                      itemData(i, richcombo::FileRole).toString()));
            wanted = qMax(wanted, fmSmall.horizontalAdvance(
                                      itemData(i, richcombo::PathRole).toString()));
        }
        const int cap = screen() ? int(screen()->availableGeometry().width() * 0.55)
                                 : 900;
        // The icon gutter is content too: without it the path elides by
        // exactly the gutter width the moment icons are switched on.
        bool anyIcon = false, anySwatch = false;
        for (int i = 0; i < count() && !(anyIcon && anySwatch); ++i) {
            anyIcon = anyIcon
                || !itemData(i, richcombo::IconStemRole).toString().isEmpty();
            anySwatch = anySwatch
                || !itemData(i, richcombo::SwatchRole).toString().isEmpty();
        }
        const int gutter = (anyIcon ? richcombo::kIconWidth + 8 : 0)
            + (anySwatch ? richcombo::kSwatchSize + 10 : 0);
        v->setMinimumWidth(
            qBound(width(), wanted + gutter + 40, qMax(cap, width())));
    }
    QComboBox::showPopup();
    // The popup view is created lazily by Qt, so its viewport can only be
    // hooked once it exists. Installing twice is harmless — Qt keeps one entry
    // per (object, filter) pair.
    if (auto* v = view()) {
        v->viewport()->installEventFilter(this);
        v->setMouseTracking(true);
        v->viewport()->setMouseTracking(true);
        v->installEventFilter(this);
    }
}

void SearchableCombo::hidePopup()
{
    hover::Preview::instance().cancel();
    QComboBox::hidePopup();
}

void SearchableCombo::applyFilter(const QString& needle)
{
    // Hiding rows in the view rather than swapping in a proxy model keeps every
    // index stable, so the caller's itemData()/currentIndex() contract — which
    // the whole builder panel is written against — is untouched.
    auto* v = qobject_cast<QListView*>(view());
    if (!v) return;
    int firstVisible = -1;
    for (int i = 0; i < count(); ++i) {
        const bool match = needle.isEmpty()
            || itemData(i, Qt::UserRole + 199).toString().contains(needle);
        v->setRowHidden(i, !match);
        // The row the popup lands on has to be one Enter can choose. A caption
        // is unselectable, so a search that matches only a caption ("all
        // receivers") would otherwise leave the list on a dead row.
        if (match && firstVisible < 0
            && (v->model()->flags(v->model()->index(i, modelColumn(),
                                                    rootModelIndex()))
                & Qt::ItemIsSelectable))
            firstVisible = i;
    }
    if (firstVisible < 0) return;
    const QModelIndex first =
        v->model()->index(firstVisible, modelColumn(), rootModelIndex());
    v->setCurrentIndex(first);
    // Filtering after the popup is laid out leaves it the height of the FULL
    // list, so a two-hit search shows two rows above a screenful of blank.
    // Shrink the container to what is actually visible, THEN scroll — resizing
    // afterwards moves the viewport again and the top row scrolls back off.
    int shown = 0, wantedHeight = 0;
    for (int i = 0; i < count() && shown < 12; ++i) {
        if (v->isRowHidden(i)) continue;
        wantedHeight += v->sizeHintForRow(i);
        ++shown;
    }
    if (auto* container = v->parentWidget()) {
        const int chrome = container->height() - v->height();
        container->resize(container->width(),
                          qMax(48, wantedHeight + qMax(0, chrome) + 4));
    }
    v->scrollTo(first, QAbstractItemView::PositionAtTop);
}

bool SearchableCombo::eventFilter(QObject* obj, QEvent* event)
{
    // Hovering a row in the open list previews whatever art that row carries —
    // a preset portrait, a colour swatch, a part icon — at a size worth
    // looking at, with the asset path underneath. Built lazily: running the
    // pointer down a list of 300 parts must not decode 300 textures.
    if (view() && obj == view()->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            const QModelIndex ix =
                qobject_cast<QListView*>(view())
                    ? qobject_cast<QListView*>(view())->indexAt(me->pos())
                    : QModelIndex();
            if (!ix.isValid()) {
                hover::Preview::instance().cancel();
            } else {
                const QString swatch =
                    ix.data(richcombo::SwatchRole).toString();
                const QString stem = ix.data(richcombo::IconStemRole).toString();
                const QString name = ix.data(richcombo::NameRole).toString();
                const QString path = ix.data(richcombo::PathRole).toString();
                const QString file = ix.data(richcombo::FileRole).toString();
                hover::Preview::instance().requestLazy(
                    QStringLiteral("combo:%1:%2:%3").arg(swatch, stem, name),
                    view()->viewport()->mapToGlobal(me->pos()),
                    [swatch, stem, name, path, file] {
                        hover::Content c;
                        c.name = name;
                        c.path = path.isEmpty() ? file : path;
                        const int sz = hover::Preview::instance().imageSize();
                        if (!swatch.isEmpty())
                            c.image = richcombo::swatchPixmap(swatch, sz);
                        else if (!stem.isEmpty())
                            c.image = fox::IconCatalog::instance().iconFor(
                                stem, sz / 2);
                        if (!swatch.isEmpty()) c.info << swatch;
                        return c;
                    });
            }
        } else if (event->type() == QEvent::Leave
                   || event->type() == QEvent::MouseButtonPress) {
            hover::Preview::instance().cancel();
        }
    }
    if (event->type() == QEvent::Hide && view() && obj == view())
        hover::Preview::instance().cancel();
    if (event->type() != QEvent::KeyPress) return QComboBox::eventFilter(obj, event);
    auto* ke = static_cast<QKeyEvent*>(event);

    // Only filter while the popup is up; closed, the combo keeps its normal
    // keyboard behaviour (arrows change selection).
    if (!view()->isVisible()) return QComboBox::eventFilter(obj, event);

    if (ke->key() == Qt::Key_Backspace) {
        if (!m_filter.isEmpty()) {
            m_filter.chop(1);
            applyFilter(m_filter);
        }
        return true;
    }
    if (ke->key() == Qt::Key_Escape && !m_filter.isEmpty()) {
        m_filter.clear();
        applyFilter(QString());
        return true;
    }
    const QString t = ke->text();
    if (!t.isEmpty() && t[0].isPrint()) {
        m_filter += t.toLower();
        applyFilter(m_filter);
        return true;
    }
    return QComboBox::eventFilter(obj, event);
}
