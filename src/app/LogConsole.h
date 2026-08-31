// LogConsole.h — the session log, in the app.
//
// Everything this tool measures it says out loud: the gear join's per-slot
// counts, the animation base it resolved, what the avatar-texture scan found,
// which materials a look retextured. All of that went to
// data\FOXAssetBrowser.log and to Help → Copy log, which means the only way to
// read it while using the app was to open the file in another program and
// watch it grow.
//
// So: a non-modal window showing the tail as it happens, with a text filter and
// per-level switches. It is a READER, not a second log — the lines are the ones
// the handler already formatted, so the console and the file can never disagree
// about what was said, and closing the window loses nothing.
//
// Non-modal and owned by the main window, so it stays up beside the tab you are
// working in — which is the whole point of having it in the app at all.
#pragma once
#include <QDialog>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QTimer;
class QLabel;

namespace fox {

class LogConsole : public QDialog {
    Q_OBJECT
public:
    explicit LogConsole(QWidget* parent = nullptr);

    // Show, raise and scroll to the end. Safe to call on an already-open
    // console — Help → Log console is a "bring it to me" action, not a toggle.
    void showConsole();

private:
    void appendLine(int level, const QString& text);
    // Rebuild the view from the retained lines under the current filter. The
    // retained set is the same bounded tail AppLog keeps, so a filter change
    // cannot resurrect a line the log itself has already dropped.
    void refilter();
    bool passes(int level, const QString& text) const;
    void updateHeader();

    QPlainTextEdit* m_view = nullptr;
    QLineEdit* m_filter = nullptr;
    QCheckBox* m_info = nullptr;
    QCheckBox* m_warn = nullptr;
    QCheckBox* m_crit = nullptr;
    QCheckBox* m_follow = nullptr;
    QLabel* m_header = nullptr;

    // Level + text, in arrival order, bounded exactly as AppLog's tail is: the
    // console must be able to re-filter without asking the handler to replay,
    // and it must not become the thing that keeps a session's worth of strings
    // alive.
    QVector<QPair<int, QString>> m_lines;
    int m_shown = 0;
    // Lines arrived while the window was hidden, so the document is behind
    // m_lines and has to be rebuilt before it is shown again.
    bool m_stale = false;
    QTimer* m_filterTimer = nullptr;
};

}  // namespace fox
