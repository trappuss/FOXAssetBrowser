#include "view/InfoPanel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QMenu>
#include <QTreeWidget>

#include "util/MenuText.h"
#include "util/TableCopy.h"
#include <QVBoxLayout>

#include "view/PanelBox.h"

namespace fox {

InfoPanel::InfoPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(10);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // NO ELISION (§15: "No elided labels anywhere; scroll instead"). A path is
    // exactly the kind of value where the middle is the part you need, and
    // "/Assets/m…def.fmdl" in a 315px column told you nothing at all. Both
    // columns size to their content and the panel scrolls sideways when that
    // does not fit, which is a scrollbar rather than a lie.
    m_tree->setTextElideMode(Qt::ElideNone);
    m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    v->addWidget(m_tree);
    // Greedy: PanelBox gives an Expanding content the whole panel and lets it
    // scroll past, which is what a list of unknown length wants.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    // §12: every detail table gets Copy / Copy all and Ctrl+C. install()
    // alone, NOT installWithMenu — this tree builds its own menu below, and
    // the helper would install a second one over it. The two entries are
    // appended to that menu instead.
    tablecopy::install(m_tree);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& at) {
                QMenu m(this);
                const bool sel = !m_tree->selectedItems().isEmpty();
                // The VALUE of the clicked row — the one thing this panel
                // offers that the shared pair does not, because a row here is
                // a label and a value and it is nearly always the value you
                // came for.
                QAction* one = m.addAction(MenuText::kCopyValue, this,
                                           [this] { copyValue(); });
                one->setEnabled(sel);
                // "Copy selected rows" and "Copy everything" were this panel's
                // own spelling of tablecopy's "Copy" and "Copy all" — the same
                // two actions, over the same tree, under two names. §12 says
                // every table gets THAT pair; this one now does, and its
                // duplicates are gone.
                tablecopy::addMenuActions(&m, m_tree);
                m.exec(m_tree->viewport()->mapToGlobal(at));
            });
    auto* copyAct = new QAction(this);
    copyAct->setShortcut(QKeySequence::Copy);
    copyAct->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(copyAct, &QAction::triggered, this, [this] { copy(true); });
    m_tree->addAction(copyAct);

    setPlaceholder(QStringLiteral("No model loaded."));
}

void InfoPanel::setPlaceholder(const QString& text)
{
    m_placeholder = text;
    if (m_rows == 0) clear();
}

void InfoPanel::clear()
{
    m_tree->clear();
    m_section = nullptr;
    m_rows = 0;
    if (!m_placeholder.isEmpty()) {
        auto* it = new QTreeWidgetItem(m_tree);
        it->setFirstColumnSpanned(true);
        it->setText(0, m_placeholder);
        it->setFlags(Qt::ItemIsEnabled);
        it->setForeground(0, QColor(0x8a, 0x8a, 0x92));
    }
}

void InfoPanel::beginSection(const QString& title)
{
    // The placeholder is not a row and must not survive the first real one.
    if (m_rows == 0 && !m_section && m_tree->topLevelItemCount() == 1
        && m_tree->topLevelItem(0)->childCount() == 0
        && !m_placeholder.isEmpty()
        && m_tree->topLevelItem(0)->text(0) == m_placeholder) {
        delete m_tree->takeTopLevelItem(0);
    }
    m_section = new QTreeWidgetItem(m_tree);
    m_section->setFirstColumnSpanned(true);
    m_section->setText(0, title);
    m_section->setFlags(Qt::ItemIsEnabled);
    QFont f = m_section->font(0);
    f.setBold(true);
    m_section->setFont(0, f);
    m_section->setForeground(0, QColor(0x9d, 0x9d, 0xa8));
    m_section->setExpanded(true);
}

void InfoPanel::addRow(const QString& key, const QString& value,
                       const QString& tip)
{
    if (!m_section) beginSection(QStringLiteral("INFO"));
    auto* it = new QTreeWidgetItem(m_section);
    it->setText(0, key);
    it->setText(1, value);
    if (!tip.isEmpty()) {
        it->setToolTip(0, tip);
        it->setToolTip(1, tip);
    } else if (!value.isEmpty()) {
        // Kept even though nothing elides any more: a long value still needs a
        // horizontal scroll to read in full, and a tooltip is the faster way.
        it->setToolTip(1, value);
    }
    it->setForeground(0, QColor(0x9a, 0x9a, 0xa2));
    ++m_rows;
}

void InfoPanel::finish()
{
    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i)
        if (m_tree->topLevelItem(i)->childCount() == 0
            && m_tree->topLevelItem(i)->text(0) != m_placeholder)
            delete m_tree->takeTopLevelItem(i);
    m_tree->expandAll();
    // The height this panel would LIKE when it first opens, read by
    // PanelBox::preferredHeight. Rows plus section headers at ~18px each.
    setProperty(kPanelWantH, (m_rows + m_tree->topLevelItemCount()) * 18 + 8);
}

QString InfoPanel::asText(bool selectedOnly) const
{
    QStringList out;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* sec = m_tree->topLevelItem(i);
        QStringList rows;
        for (int j = 0; j < sec->childCount(); ++j) {
            QTreeWidgetItem* r = sec->child(j);
            if (selectedOnly && !r->isSelected()) continue;
            rows << (r->text(0) + QLatin1Char('\t') + r->text(1));
        }
        if (rows.isEmpty()) continue;
        out << sec->text(0);
        out << rows;
    }
    return out.join(QLatin1Char('\n'));
}

void InfoPanel::copy(bool selectedOnly)
{
    const QString t = asText(selectedOnly);
    if (!t.isEmpty()) QApplication::clipboard()->setText(t);
}

void InfoPanel::copyValue()
{
    QStringList vals;
    for (QTreeWidgetItem* it : m_tree->selectedItems())
        if (!it->text(1).isEmpty()) vals << it->text(1);
    if (!vals.isEmpty()) QApplication::clipboard()->setText(vals.join(QLatin1Char('\n')));
}

}  // namespace fox
