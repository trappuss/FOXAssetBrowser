// FilterPopup.h — the funnel popup that HOSTS controls rather than copying them.
//
// Template §4 asks every list-driven tab for a funnel button left of the search
// box, opening a popup that stays open while you tick things. The Models tab has
// one already, but that one is tag-driven: it writes `#tag` into the search box
// and its contents are generated from the index's vocabulary.
//
// The Textures tab's filters are not tags. They are a Named checkbox, four game
// toggles and three combo boxes, all of them already built, already wired and
// already carrying the tab's state. So this popup does not RECREATE them — it
// takes the rows it is given and reparents them. There is exactly one
// `m_formatBox` in the process, and the popup and the tab cannot disagree about
// what is filtered, because there is nothing to keep in step.
//
// A widget lives here or it lives in the header; never both.
#pragma once
#include <QFrame>
#include <QFont>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QPalette>
#include <QWidget>

namespace fox {

class FilterPopup : public QFrame {
    Q_OBJECT
public:
    explicit FilterPopup(QWidget* parent = nullptr)
        : QFrame(parent, Qt::Popup)
    {
        setFrameShape(QFrame::StyledPanel);
        m_lay = new QVBoxLayout(this);
        m_lay->setContentsMargins(8, 8, 8, 8);
        m_lay->setSpacing(6);
    }

    // Take a widget out of wherever it is and put it in here. The caller keeps
    // its pointer and every existing connection keeps working — this moves the
    // widget, it does not replace it.
    void addRow(QWidget* w)
    {
        if (!w) return;
        m_lay->addWidget(w);
        w->show();
    }

    void addHeading(const QString& text)
    {
        auto* l = new QLabel(text, this);
        QFont f = l->font();
        f.setBold(true);
        l->setFont(f);
        // Breathing room ABOVE a heading but not above the first one, so the
        // popup reads as sections rather than as one run of controls. Three
        // combos stacked under one heading called "Used by · user tag ·
        // format" was the version of this the user asked to have fixed.
        if (m_lay->count() > 0) m_lay->addSpacing(4);
        m_lay->addWidget(l);
    }

    // ── The footer the Models tab's popup has and this one did not ───────
    // A filter control that does not say what it left is a control you have to
    // close to evaluate. `setResultText` writes the count line; the Clear
    // button next to it is the way out of a filter combination the user can no
    // longer reconstruct — the Models popup has had both for a while.
    void addFooter()
    {
        if (m_result) return;
        m_lay->addSpacing(6);
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        m_result = new QLabel(this);
        m_result->setWordWrap(true);
        QFont f = m_result->font();
        f.setPointSizeF(qMax(6.0, f.pointSizeF() - 0.5));
        m_result->setFont(f);
        m_result->setForegroundRole(QPalette::Mid);
        row->addWidget(m_result, 1);
        m_clear = new QPushButton(QStringLiteral("Clear filters"), this);
        m_clear->setAutoDefault(false);
        connect(m_clear, &QPushButton::clicked, this,
                [this] { Q_EMIT clearRequested(); });
        row->addWidget(m_clear);
        m_lay->addLayout(row);
    }

    void setResultText(const QString& s)
    {
        if (m_result) m_result->setText(s);
    }

    // Open under the funnel, left-aligned with it, clamped to the screen so a
    // popup opened near the right edge is not drawn half off it — the §5 bug
    // in a different place.
    void showFor(QWidget* anchor)
    {
        if (!anchor) return;
        adjustSize();
        QPoint p = anchor->mapToGlobal(QPoint(0, anchor->height()));
        if (const QScreen* s = anchor->screen()) {
            const QRect r = s->availableGeometry();
            p.setX(qBound(r.left(), p.x(), r.right() - width()));
            p.setY(qBound(r.top(), p.y(), r.bottom() - height()));
        }
        move(p);
        show();
    }

Q_SIGNALS:
    void closed();
    void clearRequested();

protected:
    void hideEvent(QHideEvent* e) override
    {
        QFrame::hideEvent(e);
        Q_EMIT closed();
    }

private:
    QVBoxLayout* m_lay = nullptr;
    QLabel* m_result = nullptr;
    QPushButton* m_clear = nullptr;
};

}  // namespace fox
