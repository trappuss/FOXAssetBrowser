// StatusLine.h — the one line every tab reports itself on (template §15).
//
// What this replaces: a QLabel under each viewport. The Models tab had one
// carrying "…/avf0_type0_def.fmdl — v2.04 — 15 meshes, 290 bones, 12
// materials, 3907 triangles · 0 base textures · loaded in 2 ms", the Customize
// tab had another, and both sat between the model and the bottom of the
// window taking two lines of height off the viewport permanently — for a
// message that is interesting for about four seconds after a load.
//
// The window already HAS a line for this: the status bar, where the index
// reports its progress and where every export reports itself through
// ExportNotifier. This is the same idea for per-tab status, and it follows the
// same shape deliberately — one singleton, tabs call it, MainWindow is the
// only listener.
//
// `source` is the widget reporting. MainWindow shows the message only while
// that widget is inside the CURRENT tab, so switching tabs cannot leave the
// Models tab's last load sitting under the Customize tab. Passing null means
// "from nowhere in particular" and always shows.
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

namespace fox {

class StatusLine : public QObject {
    Q_OBJECT
public:
    static StatusLine& instance()
    {
        static StatusLine s;
        return s;
    }

    void report(QWidget* source, const QString& text)
    {
        Q_EMIT reported(source, text);
    }

Q_SIGNALS:
    void reported(QWidget* source, const QString& text);

private:
    StatusLine() = default;
};

}  // namespace fox
