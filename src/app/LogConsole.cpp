#include "app/LogConsole.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtGlobal>

#include "app/AppLog.h"

namespace fox {
namespace {
// The same bound AppLog's own tail uses. Two different bounds would mean the
// console and Help → Copy log disagree about how far back "the log" goes.
constexpr int kMax = 4000;
// Where AppLog's format puts the level: "HH:mm:ss.zzz LEVEL  message" — twelve
// characters of timestamp, a space, then the level.
constexpr int kLevelCol = 13;

int levelOf(const QString& line)
{
    const QStringView lvl = QStringView(line).mid(kLevelCol, 5);
    if (lvl.startsWith(QLatin1String("WARN"))) return QtWarningMsg;
    if (lvl.startsWith(QLatin1String("CRIT"))) return QtCriticalMsg;
    if (lvl.startsWith(QLatin1String("FATAL"))) return QtFatalMsg;
    if (lvl.startsWith(QLatin1String("DBG"))) return QtDebugMsg;
    return QtInfoMsg;
}
}  // namespace

LogConsole::LogConsole(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Log"));
    // A dialog, but a window: it must stay up while the main window is used,
    // and it must not sit on top of everything else the user owns.
    setWindowFlag(Qt::Window, true);
    setModal(false);
    resize(920, 460);

    auto* v = new QVBoxLayout(this);

    auto* bar = new QHBoxLayout();
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(
        QStringLiteral("Filter — plain text, matched anywhere in the line"));
    m_filter->setClearButtonEnabled(true);
    bar->addWidget(m_filter, 1);
    m_info = new QCheckBox(QStringLiteral("Info"), this);
    m_warn = new QCheckBox(QStringLiteral("Warnings"), this);
    m_crit = new QCheckBox(QStringLiteral("Errors"), this);
    for (QCheckBox* c : {m_info, m_warn, m_crit}) {
        c->setChecked(true);
        bar->addWidget(c);
    }
    m_follow = new QCheckBox(QStringLiteral("Follow"), this);
    m_follow->setChecked(true);
    m_follow->setToolTip(QStringLiteral(
        "Scroll to the newest line as it arrives. Turn it off to read "
        "something while the app keeps logging."));
    bar->addWidget(m_follow);
    v->addLayout(bar);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    // The log is columnar — a timestamp, a level, then the message — and only
    // a fixed-pitch font keeps those columns lined up.
    m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_view->setMaximumBlockCount(kMax);
    v->addWidget(m_view, 1);

    auto* foot = new QHBoxLayout();
    m_header = new QLabel(this);
    foot->addWidget(m_header, 1);
    auto* copy = new QPushButton(QStringLiteral("Copy shown"), this);
    copy->setToolTip(QStringLiteral(
        "Copy exactly what is on screen — the filter included. Help → Copy log "
        "copies the whole tail instead."));
    foot->addWidget(copy);
    auto* openFile = new QPushButton(QStringLiteral("Open log file"), this);
    foot->addWidget(openFile);
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    foot->addWidget(close);
    v->addLayout(foot);

    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_view->toPlainText()); });
    connect(openFile, &QPushButton::clicked, this, [] {
        // The FOLDER, not the file: the log is open for writing with a
        // flush-per-line handler, and handing it to whatever the desktop has
        // registered for .log risks a lock on a file this process is still
        // using. The folder also has the crash reports beside it.
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QFileInfo(AppLog::filePath()).absolutePath()));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    // Debounced. Each keystroke otherwise walks four thousand lines, builds a
    // few hundred kilobytes of QString and re-lays the document out.
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(150);
    connect(m_filterTimer, &QTimer::timeout, this, [this] { refilter(); });
    connect(m_filter, &QLineEdit::textChanged, this,
            [this] { m_filterTimer->start(); });
    for (QCheckBox* c : {m_info, m_warn, m_crit})
        connect(c, &QCheckBox::toggled, this, [this] { refilter(); });

    // CONNECT FIRST, then seed. The other order loses any line logged between
    // the snapshot and the connection; this order can at worst show one twice,
    // and a visible duplicate is a great deal better than a silent hole. (The
    // connection is queued and this is the GUI thread, so nothing is delivered
    // until the constructor returns — the duplicate is the only outcome.)
    //
    // QUEUED because the handler emits from whichever thread logged and this
    // touches widgets. Not AutoConnection: the notifier lives in the GUI
    // thread, so Auto would resolve to a DIRECT call for every GUI-thread log
    // line and run the whole text layout inside the log path.
    connect(&AppLog::Notifier::instance(), &AppLog::Notifier::line, this,
            &LogConsole::appendLine, Qt::QueuedConnection);
    // The level is read at its FIXED OFFSET, not searched for: the handler
    // writes "HH:mm:ss.zzz LEVEL  message", so the level starts at column 13
    // and a message that happens to contain the text "WARN" cannot be
    // mistaken for one.
    for (const QString& line : AppLog::tail())
        m_lines.append({levelOf(line), line});
    refilter();
}

void LogConsole::showConsole()
{
    // Lines that arrived while the window was hidden went into m_lines and not
    // into the document — see appendLine. Rebuild before showing, or the
    // console would come back up missing everything that happened while it was
    // away, which is usually the part worth reading.
    if (m_stale) refilter();
    show();
    raise();
    activateWindow();
    if (m_follow && m_follow->isChecked())
        m_view->verticalScrollBar()->setValue(
            m_view->verticalScrollBar()->maximum());
}

bool LogConsole::passes(int level, const QString& text) const
{
    switch (level) {
        case QtWarningMsg:
            if (m_warn && !m_warn->isChecked()) return false;
            break;
        case QtCriticalMsg:
        case QtFatalMsg:
            if (m_crit && !m_crit->isChecked()) return false;
            break;
        default:
            // Debug rides with Info: this build emits none, and giving a level
            // nobody produces its own switch is a control that does nothing.
            if (m_info && !m_info->isChecked()) return false;
            break;
    }
    const QString needle = m_filter ? m_filter->text().trimmed() : QString();
    return needle.isEmpty() || text.contains(needle, Qt::CaseInsensitive);
}

void LogConsole::appendLine(int level, const QString& text)
{
    m_lines.append({level, text});
    // Trim the RETAINED set and the shown count together. m_shown counting up
    // for ever while m_lines is bounded is how the header came to read
    // "9231 of 4000".
    while (m_lines.size() > kMax) {
        if (passes(m_lines.first().first, m_lines.first().second)) --m_shown;
        m_lines.removeFirst();
    }
    if (!passes(level, text)) return;
    ++m_shown;
    // A HIDDEN console does no layout. The connection stays live once the
    // window has been opened, and a bulk extract logging six figures would
    // otherwise lay the document out that many times for nobody. The line is
    // already in m_lines, and showConsole() rebuilds from those.
    if (!isVisible()) {
        m_stale = true;
        return;
    }
    // Whether we were AT the bottom before appending, not after: appending is
    // what moves the maximum, so asking afterwards always says no.
    QScrollBar* sb = m_view->verticalScrollBar();
    const bool atEnd = sb->value() >= sb->maximum() - 2;
    m_view->appendPlainText(text);
    if (m_follow && m_follow->isChecked() && atEnd)
        sb->setValue(sb->maximum());
    updateHeader();
}

void LogConsole::refilter()
{
    m_stale = false;
    m_shown = 0;
    QStringList keep;
    keep.reserve(m_lines.size());
    for (const auto& p : m_lines) {
        if (!passes(p.first, p.second)) continue;
        keep << p.second;
        ++m_shown;
    }
    // JOINED, not one-newline-per-line: a trailing '\n' makes one more block
    // than there are lines, and setMaximumBlockCount then drops the OLDEST log
    // line to make room for a blank one at the end.
    //
    // One setPlainText rather than a few thousand appendPlainText calls: each
    // append lays out and scrolls, and re-filtering a full tail that way is a
    // visible freeze.
    m_view->setPlainText(keep.join(QLatin1Char('\n')));
    // Only when following. Turning Follow off to read something and then
    // typing one character in the filter used to yank the view back to the
    // end, which is the exact thing Follow was turned off to stop.
    if (!m_follow || m_follow->isChecked())
        m_view->verticalScrollBar()->setValue(
            m_view->verticalScrollBar()->maximum());
    updateHeader();
}

void LogConsole::updateHeader()
{
    if (!m_header) return;
    // The path is rebuilt from QDir every call and never changes; this runs
    // once per appended line.
    static const QString kPath = AppLog::filePath();
    m_header->setText(QStringLiteral("%1 of %2 line(s) — %3")
                          .arg(m_shown)
                          .arg(m_lines.size())
                          .arg(kPath));
}

}  // namespace fox
