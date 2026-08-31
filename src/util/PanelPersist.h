// PanelPersist.h — splitter layout memory, behind one setting (template §6).
//
// Ported from D4AssetBrowser's `util/PanelPersist.h`. Every tab in this tool
// is a splitter, and every one of them threw away whatever widths the user
// dragged the moment the app closed. This restores them — and, just as
// importantly, does nothing at all when the setting is off, so a person who
// wants the tool to open the same way every time gets that.
//
// **Call bind() AFTER the splitter's code-set setSizes().** That ordering is
// the whole contract: an empty or missing blob leaves the sizes untouched, so
// the default stands until the user has actually dragged something. Binding
// first would have the restore run against a splitter that has not been sized
// yet, and the default would win on every launch.
//
// The key is per splitter and namespaced by tab (`models/splitter`), because
// the tabs hold different panes and one shared blob would restore the Models
// tab's five columns into the Files tab's two.
#pragma once
#include <QByteArray>
#include <QObject>
#include <QSettings>
#include <QSplitter>
#include <QString>

namespace PanelPersist {

inline bool enabled()
{
    return QSettings()
        .value(QStringLiteral("interface/rememberPanels"), true)
        .toBool();
}

inline void bind(QSplitter* s, const QString& key)
{
    if (!s) return;
    const QString countKey = key + QStringLiteral("/panes");
    if (enabled()) {
        const QByteArray blob = QSettings().value(key).toByteArray();
        // THE PANE COUNT, stored beside the blob and checked before it is
        // used. QSplitter::restoreState returns false only on a magic or
        // version mismatch — measured: a 2-pane blob restored into a 3-pane
        // splitter returns TRUE and leaves the third pane 0 px wide, which is
        // indistinguishable from a pane that failed to build. So the count is
        // the guard, and a build that adds or removes a pane falls back to its
        // own default instead of hiding the new one.
        const int savedPanes = QSettings().value(countKey, -1).toInt();
        const bool fits = savedPanes < 0 || savedPanes == s->count();
        // Diagnostics stay in and are bounded (one line per splitter, at
        // construction): "did my layout come back" is otherwise unanswerable
        // from a log, and the answer has three shapes — restored, nothing
        // saved yet, and a saved blob this build's splitter cannot take
        // (restoreState refuses one whose pane count differs, which is
        // exactly what a build that adds a pane produces).
        if (blob.isEmpty()) {
            qInfo("panels: %s — nothing saved yet, using the default",
                  qUtf8Printable(key));
        } else if (!fits) {
            qInfo("panels: %s — saved layout has %d pane(s), this build has "
                  "%d; using the default",
                  qUtf8Printable(key), savedPanes, s->count());
        } else {
            qInfo("panels: %s — %s", qUtf8Printable(key),
                  s->restoreState(blob) ? "restored"
                                        : "saved layout unreadable, using the "
                                          "default");
        }
    } else {
        qInfo("panels: %s — remembering is off, using the default",
              qUtf8Printable(key));
    }
    // Connected regardless of the setting: turning the setting ON mid-session
    // should start remembering from that moment, not from the next launch.
    QObject::connect(s, &QSplitter::splitterMoved, s, [s, key, countKey](int, int) {
        if (!enabled()) return;
        QSettings st;
        st.setValue(key, s->saveState());
        st.setValue(countKey, s->count());
    });
}

}  // namespace PanelPersist
