// MgsvMetaDialog.h — the five things a mod manager shows about a mod, asked
// once, before the file dialog.
//
// A .mgsv is not an export of pixels; it is something a person publishes under
// their own name. The mod manager's list shows Name, Version and Author, so a
// package written with those blank is one nobody can identify in a list of
// thirty — and the fields live inside a zip, where correcting them afterwards
// means unpacking it. Asking costs one dialog.
//
// The two version fields SnakeBite refuses a mod over are deliberately NOT on
// this form. Their safe values are exactly one value each — 0.0.0.0 for the
// game and 0.8.0.0 for SnakeBite, see MgsvMeta — and every other value narrows
// who can install the mod. A field whose only good answer is its default is a
// way to get it wrong.
#pragma once
#include <QDialog>

#include "util/ModPackage.h"

class QLineEdit;
class QPlainTextEdit;

class MgsvMetaDialog : public QDialog {
    Q_OBJECT
public:
    // `suggestedName` seeds the Name field — the caller passes the mod folder's
    // own name, which is usually what the person would have typed.
    explicit MgsvMetaDialog(const QString& suggestedName, int assetCount,
                            QWidget* parent = nullptr);

    modpackage::MgsvMeta meta() const;
    // Fill the form from an existing set of values. Only the harness uses it
    // — it is how the invalid states get photographed, which is the one thing
    // about a form that a log cannot show.
    void setMeta(const modpackage::MgsvMeta& m);

private:
    void validate();

    QLineEdit* m_name = nullptr;
    QLineEdit* m_version = nullptr;
    QLineEdit* m_author = nullptr;
    QLineEdit* m_website = nullptr;
    QPlainTextEdit* m_description = nullptr;
    class QPushButton* m_ok = nullptr;
    class QLabel* m_note = nullptr;
    int m_assetCount = 0;
};
