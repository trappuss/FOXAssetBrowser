// TipBar.cpp — see TipBar.h.
#include "view/TipBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QToolButton>

namespace fox {

namespace {
QString settingKey(const QString& key)
{
    return QStringLiteral("tips/") + key;
}
}  // namespace

TipBar::TipBar(const QString& key, const QString& text, QWidget* parent)
    : QWidget(parent), m_key(key)
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 2, 4, 2);
    row->setSpacing(6);
    m_label = new QLabel(text, this);
    m_label->setWordWrap(true);
    // Dimmer than body text on purpose: it is a hint, and a hint that competes
    // with the thing it is about is a banner.
    QPalette pal = m_label->palette();
    QColor c = pal.color(QPalette::WindowText);
    c.setAlpha(165);
    pal.setColor(QPalette::WindowText, c);
    m_label->setPalette(pal);
    row->addWidget(m_label, 1);

    auto* close = new QToolButton(this);
    close->setText(QStringLiteral("✕"));
    close->setAutoRaise(true);
    close->setToolTip(QStringLiteral("Hide this tip. Settings ▸ Interface "
                                     "brings the tips back."));
    close->setCursor(Qt::PointingHandCursor);
    row->addWidget(close);
    connect(close, &QToolButton::clicked, this, [this] {
        QSettings().setValue(settingKey(m_key), true);
        hide();
    });

    if (QSettings().value(settingKey(m_key), false).toBool()) hide();
}

void TipBar::resetAll()
{
    QSettings s;
    s.beginGroup(QStringLiteral("tips"));
    const QStringList keys = s.childKeys();
    for (const QString& k : keys) s.remove(k);
    s.endGroup();
}

}  // namespace fox
