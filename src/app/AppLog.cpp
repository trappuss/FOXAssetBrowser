#include "app/AppLog.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <cstdio>

#include "app/AppPaths.h"

namespace {

QFile g_logFile;
QMutex g_logMutex;
QStringList g_tail;
constexpr int kTailMax = 4000;

void handler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    QString line;
    {
    QMutexLocker lock(&g_logMutex);
    const char* lvl = "INFO";
    switch (type) {
    case QtDebugMsg: lvl = "DBG "; break;
    case QtWarningMsg: lvl = "WARN"; break;
    case QtCriticalMsg: lvl = "CRIT"; break;
    case QtFatalMsg: lvl = "FATAL"; break;
    default: break;
    }
    line = QStringLiteral("%1 %2  %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             QLatin1String(lvl), msg);
    if (g_logFile.isOpen()) {
        QTextStream(&g_logFile) << line << '\n';
        g_logFile.flush();   // flush-per-line: a crash must not eat the tail
    }
    fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
    g_tail.append(line);
    if (g_tail.size() > kTailMax) g_tail.remove(0, g_tail.size() - kTailMax);
    }
    // OUTSIDE the lock, deliberately. A receiver connected directly — which is
    // one line of somebody else's code away — would otherwise run under
    // g_logMutex, and any log call it made (a Qt font-fallback warning is the
    // realistic one) would deadlock on a non-recursive mutex against itself.
    // The cost is that two threads logging at the same instant can reach a
    // console in the other order; the FILE, which is the record, stays ordered
    // because its write is inside the lock.
    emit AppLog::Notifier::instance().line(int(type), line);
}

}  // namespace

namespace AppLog {

Notifier& Notifier::instance()
{
    // DELIBERATELY LEAKED. A function-local static would be destroyed at exit,
    // and — being constructed on the first log line — destroyed BEFORE the
    // file and tail beside it, which are namespace-scope in this same
    // translation unit. The handler is never uninstalled, so a qWarning from a
    // later static destructor or from Qt's own teardown would then emit on a
    // destroyed object, quietly, with the file half of the handler still
    // working. One never-freed QObject at exit is the cheaper of the two.
    static Notifier* n = new Notifier;
    return *n;
}

QString filePath()
{
    return AppPaths::file(QStringLiteral("FOXAssetBrowser.log"));
}

void install()
{
    {
        QMutexLocker lock(&g_logMutex);
        g_logFile.setFileName(filePath());
        g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    }
    qInstallMessageHandler(handler);
}

QStringList tail()
{
    QMutexLocker lock(&g_logMutex);
    return g_tail;
}

}  // namespace AppLog
