#pragma once
// Session log: every qInfo/qWarning line goes to data\FOXAssetBrowser.log
// (truncated each launch — one file that is always the current session) and to
// an in-memory tail for Help → Copy log. install() must run before the first
// log line; everything is mutex-serialised because Qt routes messages from any
// thread through one global handler.
#include <QObject>
#include <QString>
#include <QStringList>

namespace AppLog {

// LIVE LINES, for the in-app log console.
//
// The Qt message handler is a free function called from any thread, and a
// widget can only be touched on the GUI thread — so the handler emits through
// this one QObject and the console connects with a queued connection. One
// notifier, created on first use, so the handler never has to know whether a
// console exists.
//
// `level` is the QtMsgType as an int — the signal crosses a queued connection
// and an int is the plainest thing to marshal; `text` is the fully formatted
// line, the same one the file gets, so the console and the file can never
// disagree about what was said.
//
// The handler emits OUTSIDE its mutex, so a directly-connected receiver cannot
// deadlock the logger by logging. Connect queued anyway if you touch widgets:
// the emit comes from whichever thread logged.
class Notifier : public QObject {
    Q_OBJECT
public:
    static Notifier& instance();

signals:
    void line(int level, const QString& text);

private:
    Notifier() = default;
};

// Install the Qt message handler + open the log file.
void install();

// Absolute path of the log file (shown in Help so bug reports can point at it).
QString filePath();

// The most recent lines (bounded), for clipboard export.
QStringList tail();

}  // namespace AppLog
