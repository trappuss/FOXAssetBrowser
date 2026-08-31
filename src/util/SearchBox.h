// SearchBox.h — what every search box in the application does (template §4/§15).
//
// "`Ctrl+F` focuses, `Esc` clears, `↓` recalls the last ten searches — in
// every search box." That is one line of the template and four behaviours, and
// four behaviours written per box is four chances to have three of them.
//
// So it is one call. `attach(box, key)` gives a QLineEdit:
//
//   Esc         clear, and hand the focus back to whatever had it
//   ↓           the last ten searches, as a completer popup, newest first
//   remember    a search is recorded when it is COMMITTED — Enter, or the box
//               losing focus with text in it — never on every keystroke, or
//               the history would be the last ten prefixes of one word
//
// `Ctrl+F` is not here: it belongs to the window, which is the thing that
// knows which tab is in front. See `Hotkeys::seq("hotkeys/focusSearch")` and
// MainWindow, which routes it to the current tab's box through focusSearch().
//
// The history is per BOX, keyed by `key`, because the Models tab searches
// model names and the Strings panel searches strings, and one shared list
// would offer each of them the other's vocabulary.
#pragma once
#include <QCompleter>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QStringListModel>

namespace fox {
namespace searchbox {

// How many. The template says ten; ten is also about as many as a popup can
// show without becoming a list to read rather than a list to pick from.
constexpr int kHistory = 10;

inline QStringList history(const QString& key)
{
    return QSettings().value(key).toStringList();
}

inline void remember(const QString& key, const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return;
    QStringList h = history(key);
    h.removeAll(t);          // a repeat moves to the front rather than doubling
    h.prepend(t);
    while (h.size() > kHistory) h.removeLast();
    QSettings().setValue(key, h);
}

// The event filter that gives one box its keys. Parented to the box, so it
// lives exactly as long as the thing it is filtering.
class Keys : public QObject {
public:
    Keys(QLineEdit* box, QString key, QCompleter* completer)
        : QObject(box), m_box(box), m_key(std::move(key)), m_completer(completer)
    {
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o != m_box) return QObject::eventFilter(o, e);
        if (e->type() == QEvent::FocusOut) {
            remember(m_key, m_box->text());
            return false;
        }
        if (e->type() != QEvent::KeyPress) return QObject::eventFilter(o, e);
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape) {
            // Only when there is something to clear. An Esc on an empty box
            // belongs to whatever is behind it — a dialog that wants to close,
            // a run that wants to cancel — and swallowing it here would make
            // those stop working whenever the search box happened to have the
            // focus.
            if (m_box->text().isEmpty()) return false;
            m_box->clear();
            m_box->clearFocus();
            return true;
        }
        if (k->key() == Qt::Key_Down && m_completer) {
            refresh();
            if (m_completer->model()
                && m_completer->model()->rowCount(QModelIndex()) > 0) {
                m_completer->setCompletionPrefix(QString());
                m_completer->complete();
                return true;
            }
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)
            remember(m_key, m_box->text());
        return QObject::eventFilter(o, e);
    }

private:
    void refresh()
    {
        if (auto* m = qobject_cast<QStringListModel*>(m_completer->model()))
            m->setStringList(history(m_key));
    }
    QLineEdit* m_box = nullptr;
    QString m_key;
    QCompleter* m_completer = nullptr;
};

// Give `box` the standard behaviour. `key` is the QSettings key its history
// lives under — one per box, namespaced by tab.
inline void attach(QLineEdit* box, const QString& key)
{
    if (!box) return;
    // The name Ctrl+F looks for. Set HERE rather than at each call site, so a
    // box that has this behaviour is by definition a box the focus shortcut
    // can find, and the two can never be given to different sets of widgets.
    box->setObjectName(QStringLiteral("foxabSearchBox"));
    box->setClearButtonEnabled(true);
    auto* model = new QStringListModel(history(key), box);
    auto* completer = new QCompleter(model, box);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    // UnfilteredPopupCompletion, not the default: ↓ is "show me what I typed
    // before", not "complete what I am typing". A filtered popup over a box
    // the user has already typed three characters into shows the one entry
    // that starts with them, which is never the reason anyone presses ↓.
    completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    completer->setMaxVisibleItems(kHistory);
    // setWidget, NOT setCompleter. A completer INSTALLED on a QLineEdit is
    // driven by the line edit itself: QWidgetLineControl calls complete()
    // after every inserted character, and in unfiltered mode that shows the
    // WHOLE history — so one keystroke dropped ten unrelated past searches
    // over the results, and that popup then owned Up, Down, Enter and Esc.
    // The first Esc closed the popup instead of clearing the box, and Enter
    // could replace what had just been typed with a highlighted old search.
    //
    // Attaching it to the widget without installing it leaves the line edit
    // alone and makes the popup something that happens ONLY when the down
    // arrow asks for it, which is what "↓ recalls the last ten searches"
    // means. The completion has to be inserted by hand for the same reason.
    completer->setWidget(box);
    QObject::connect(
        completer, qOverload<const QString&>(&QCompleter::activated), box,
        [box](const QString& text) { box->setText(text); });
    box->installEventFilter(new Keys(box, key, completer));
}

}  // namespace searchbox
}  // namespace fox
