#include "app/MgsvMetaDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

MgsvMetaDialog::MgsvMetaDialog(const QString& suggestedName, int assetCount,
                               QWidget* parent)
    : QDialog(parent), m_assetCount(assetCount)
{
    setWindowTitle(QStringLiteral("Package as a SnakeBite mod"));

    m_name = new QLineEdit(suggestedName, this);
    m_version = new QLineEdit(QStringLiteral("1.0.0.0"), this);
    m_author = new QLineEdit(this);
    m_website = new QLineEdit(this);
    m_description = new QPlainTextEdit(this);
    m_description->setPlaceholderText(
        QStringLiteral("What this mod changes. Shown in the mod manager."));
    m_description->setMinimumHeight(70);

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Name"), m_name);
    form->addRow(QStringLiteral("Version"), m_version);
    form->addRow(QStringLiteral("Author"), m_author);
    form->addRow(QStringLiteral("Website"), m_website);
    form->addRow(QStringLiteral("Description"), m_description);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                             | QDialogButtonBox::Cancel,
                                         this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Choose file…"));
    m_ok = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(m_note);
    root->addWidget(buttons);

    // Checked as it is typed rather than at OK: a version string SnakeBite
    // cannot parse is silently coerced to 0.0.0.0 by its own setter, and the
    // mod is then refused at INSTALL time with a message about an old
    // SnakeBite. The moment to catch that is while the caret is still in the
    // field.
    connect(m_name, &QLineEdit::textChanged, this, &MgsvMetaDialog::validate);
    connect(m_version, &QLineEdit::textChanged, this, &MgsvMetaDialog::validate);
    validate();
    resize(460, sizeHint().height());
}

void MgsvMetaDialog::validate()
{
    const bool named = !m_name->text().trimmed().isEmpty();
    const bool versioned = modpackage::isVersionString(m_version->text());
    m_ok->setEnabled(named && versioned);
    if (!named)
        m_note->setText(QStringLiteral(
            "A mod needs a name — it is what the mod manager lists it under."));
    else if (!versioned)
        m_note->setText(QStringLiteral(
            "Version wants one to four numbers separated by dots, such as "
            "1.0.0.0. SnakeBite reads this with System.Version and refuses "
            "anything else."));
    else
        // THE COUNT BELONGS HERE AND NOWHERE ELSE. It was set once in the
        // constructor and then overwritten by this function's first call, so
        // the one number that answers "is my replacement actually in there"
        // was never on screen. Found by opening the grab.
        m_note->setText(QStringLiteral(
            "%1 replacement(s) will be packaged. The mod targets any game "
            "version and any SnakeBite from 0.8 up — nothing here narrows "
            "that.").arg(m_assetCount));
}

void MgsvMetaDialog::setMeta(const modpackage::MgsvMeta& m)
{
    m_name->setText(m.name);
    m_version->setText(m.version);
    m_author->setText(m.author);
    m_website->setText(m.website);
    m_description->setPlainText(m.description);
    validate();
}

modpackage::MgsvMeta MgsvMetaDialog::meta() const
{
    modpackage::MgsvMeta m;
    m.name = m_name->text().trimmed();
    m.version = m_version->text().trimmed();
    m.author = m_author->text().trimmed();
    m.website = m_website->text().trimmed();
    m.description = m_description->toPlainText();
    return m;
}
