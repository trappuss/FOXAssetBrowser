#pragma once
// Portable, self-contained storage. Everything the tool writes (settings INI,
// index cache, logs) lives in a "data" folder beside the executable — no
// Windows registry, no %AppData%. Copy the release folder anywhere (or a USB
// stick) and it runs and remembers its state, leaving zero traces on the host.
// main() points QSettings at data/ so every QSettings() default-ctor call
// resolves here too.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>

namespace AppPaths {

inline QString dataDir()
{
    static const QString d = [] {
        QString base = QCoreApplication::applicationDirPath();
        if (base.isEmpty()) base = QDir::currentPath();
        const QString dir = QDir(base).filePath(QStringLiteral("data"));
        QDir().mkpath(dir);
        return dir;
    }();
    return d;
}

// A file directly inside data/.
inline QString file(const QString& name) { return QDir(dataDir()).filePath(name); }

// ── Caches live in data/cache/, NOT in data/ ────────────────────────────────
// They used to sit beside FOXAssetBrowser.ini, and that cost a user their whole
// configuration: the documented way to force a cold index is to empty the cache,
// the settings file was in the same folder, and clearing one took the other.
// The two failures are indistinguishable from outside — an app with no caches
// and an app with no game folders both start instantly and show nothing.
//
// Anything derived and rebuildable goes here. Anything the user authored stays
// in data/. Then "clear the cache" is a directory, not a filename pattern that
// has to be got right.
inline QString cacheDir()
{
    static const QString d = [] {
        const QString dir = QDir(dataDir()).filePath(QStringLiteral("cache"));
        QDir().mkpath(dir);
        return dir;
    }();
    return d;
}

inline QString cacheFile(const QString& name)
{
    return QDir(cacheDir()).filePath(name);
}

// Move any cache left in data/ by an older build into data/cache/. One rename,
// not a rebuild: the deep-scan cache alone was measured at 88.6 seconds to
// regenerate on a real install, which is not a cost to impose on someone for
// upgrading. Returns how many were moved.
inline int migrateCaches()
{
    QDir data(dataDir());
    const QStringList stale =
        data.entryList({QStringLiteral("fox_*.bin")}, QDir::Files);
    int moved = 0;
    for (const QString& name : stale) {
        const QString to = cacheFile(name);
        if (QFile::exists(to)) { QFile::remove(data.filePath(name)); continue; }
        if (QFile::rename(data.filePath(name), to)) ++moved;
    }
    return moved;
}

// A subdirectory inside data/ (created on demand).
inline QString subDir(const QString& name)
{
    const QString d = QDir(dataDir()).filePath(name);
    QDir().mkpath(d);
    return d;
}

// The dict/ folder beside the exe (dictionaries ship with the tool, read-only,
// so they are NOT under data/).
inline QString dictDir()
{
    QString base = QCoreApplication::applicationDirPath();
    if (base.isEmpty()) base = QDir::currentPath();
    return QDir(base).filePath(QStringLiteral("dict"));
}

}  // namespace AppPaths
