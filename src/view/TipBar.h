// TipBar.h — the dismissible hint strip above a viewport.
//
// The viewport grew a keyboard in this batch — double-click to select, H to
// hide, Shift+H to show everything, Alt+H to isolate, "." to frame, F for
// fullscreen, F1 for the list. Every one of those is invisible: there is no
// button to hover, so a tooltip cannot say it, and a shortcut nobody knows is
// a feature nobody has.
//
// So: one line of text, above the viewport, with an ✕. It is remembered per
// KEY, so dismissing the Models tab's tip does not silently dismiss the
// Customize tab's, and a tab whose tip is new gets to show it again — one
// setting, one key (§3.1).
#pragma once
#include <QString>
#include <QWidget>

class QLabel;

namespace fox {

class TipBar : public QWidget {
    Q_OBJECT
public:
    // `key` is the settings key under `tips/`. `text` is the line. A tip the
    // user has dismissed never builds itself visible.
    TipBar(const QString& key, const QString& text, QWidget* parent = nullptr);

    // Show it again — for a Settings ▸ Interface "show the tips again" action,
    // so a dismissal is never permanent without a way back.
    static void resetAll();

private:
    QString m_key;
    QLabel* m_label = nullptr;
};

}  // namespace fox
