#include "util/ModFolder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>

#include "app/Config.h"

namespace modfolder {
namespace {

// The asset path as a path RELATIVE to the mod root: "/Assets/tpp/…" becomes
// "Assets/tpp/…". Empty when the path is not one this mount can resolve.
//
// The tests are all about what the loose walk will do with the result, because
// that walk is what turns the file back into a hash. It strips the root and
// hashes what is left, so:
//   • the path has to be a real /Assets name — a hash-only asset extracted as
//     unresolved/<hex>.<ext> would hash to that literal string and override
//     nothing;
//   • it must not contain a ".." segment, or the file lands outside the folder
//     the user pointed at and outside the tree the walk covers. This one is
//     checked rather than trusted: the string came out of an archive's own
//     name table, which is data from a file on disk.
QString relFor(const QString& assetPath)
{
    QString p = assetPath;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.startsWith(QLatin1Char('/'))) p = p.mid(1);
    if (p.isEmpty()) return {};
    if (!p.startsWith(QLatin1String("Assets/"), Qt::CaseInsensitive)) return {};
    for (const QString& seg : p.split(QLatin1Char('/')))
        if (seg == QLatin1String("..") || seg == QLatin1String(".") || seg.isEmpty())
            return {};
    return p;
}

}  // namespace

QString dir()
{
    return Config::modDir();
}

bool active()
{
    const QString d = dir();
    return !d.isEmpty() && QDir(d).exists();
}

QString pathFor(const QString& assetPath)
{
    const QString d = dir();
    if (d.isEmpty()) return {};
    const QString rel = relFor(assetPath);
    if (rel.isEmpty()) return {};
    return QDir(d).absoluteFilePath(rel);
}

bool overrides(const QString& assetPath)
{
    const QString p = pathFor(assetPath);
    return !p.isEmpty() && QFileInfo::exists(p);
}

QString put(const QString& assetPath, const QByteArray& data)
{
    const QString d = dir();
    if (d.isEmpty())
        return QStringLiteral("No mod folder is set — Settings ▸ Folders.");
    const QString rel = relFor(assetPath);
    if (rel.isEmpty())
        return QStringLiteral(
            "'%1' is not a resolved asset name, so a replacement for it could "
            "not be found by name. Only files whose real path this install "
            "knows can be replaced.").arg(assetPath);
    const QString out = QDir(d).absoluteFilePath(rel);
    if (!QDir().mkpath(QFileInfo(out).absolutePath()))
        return QStringLiteral("Could not create %1")
            .arg(QFileInfo(out).absolutePath());
    // Written whole and then renamed over the old one, so an interrupted write
    // cannot leave a truncated asset mounted over a working game file — which
    // would look exactly like a corrupt game rather than like a failed copy.
    const QString tmp = out + QStringLiteral(".part");
    QFile::remove(tmp);
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly))
        return QStringLiteral("Could not write %1: %2").arg(tmp, f.errorString());
    const qint64 n = f.write(data);
    f.close();
    if (n != data.size()) {
        QFile::remove(tmp);
        return QStringLiteral("Short write to %1").arg(tmp);
    }
    QFile::remove(out);
    if (!QFile::rename(tmp, out)) {
        QFile::remove(tmp);
        return QStringLiteral("Could not place %1").arg(out);
    }
    return {};
}

QString putSet(const QVector<QPair<QString, QByteArray>>& files)
{
    if (files.isEmpty()) return QStringLiteral("nothing to install");
    const QString d = dir();
    if (d.isEmpty())
        return QStringLiteral("No mod folder is set — Settings ▸ Folders.");

    // Every path is resolved and every folder made BEFORE a byte is written,
    // so the common failure — one member of the set is not a name this install
    // knows — stops the whole thing before anything has been touched.
    QVector<QPair<QString, QByteArray>> plan;   // absolute out path, data
    plan.reserve(files.size());
    for (const auto& fpair : files) {
        const QString rel = relFor(fpair.first);
        if (rel.isEmpty())
            return QStringLiteral(
                "'%1' is not a resolved asset name, so nothing in this set was "
                "installed.").arg(fpair.first);
        const QString out = QDir(d).absoluteFilePath(rel);
        if (!QDir().mkpath(QFileInfo(out).absolutePath()))
            return QStringLiteral("Could not create %1")
                .arg(QFileInfo(out).absolutePath());
        plan.append({out, fpair.second});
    }

    QStringList written;
    const auto abandon = [&written](const QString& why) {
        for (const QString& t : written) QFile::remove(t);
        return why;
    };
    for (const auto& pr : plan) {
        const QString tmp = pr.first + QStringLiteral(".part");
        QFile::remove(tmp);
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly))
            return abandon(QStringLiteral("Could not write %1: %2")
                               .arg(tmp, f.errorString()));
        const qint64 n = f.write(pr.second);
        f.close();
        written << tmp;
        if (n != pr.second.size())
            return abandon(QStringLiteral("Short write to %1").arg(tmp));
    }
    // Every temporary is on disk and complete. Only now do they become the
    // installed files.
    for (const auto& pr : plan) {
        QFile::remove(pr.first);
        if (!QFile::rename(pr.first + QStringLiteral(".part"), pr.first))
            return abandon(QStringLiteral("Could not place %1").arg(pr.first));
    }
    return {};
}

QString putFile(const QString& assetPath, const QString& sourceFile)
{
    QFile in(sourceFile);
    if (!in.open(QIODevice::ReadOnly))
        return QStringLiteral("Could not read %1: %2")
            .arg(sourceFile, in.errorString());
    const QByteArray data = in.readAll();
    in.close();
    if (data.isEmpty())
        return QStringLiteral("%1 is empty").arg(sourceFile);
    return put(assetPath, data);
}

QString revert(const QString& assetPath)
{
    const QString p = pathFor(assetPath);
    if (p.isEmpty() || !QFileInfo::exists(p)) return {};
    if (!QFile::remove(p))
        return QStringLiteral("Could not remove %1").arg(p);
    // Prune the folders the replacement left behind, up to the mod root. A mod
    // folder that fills with empty /Assets/tpp/chara/… chains after a few
    // reverts reads as "something is still installed" when nothing is.
    QDir walk = QFileInfo(p).absoluteDir();
    const QString root = QDir(dir()).absolutePath();
    while (walk.absolutePath().startsWith(root)
           && walk.absolutePath() != root
           && walk.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        const QString gone = walk.absolutePath();
        if (!walk.cdUp()) break;
        QDir().rmdir(gone);
    }
    return {};
}

namespace { std::function<void()> g_changed; }

void setChangedHook(const std::function<void()>& hook) { g_changed = hook; }

void notifyChanged()
{
    if (g_changed) g_changed();
}

QStringList list()
{
    QStringList out;
    const QString d = dir();
    if (d.isEmpty() || !QDir(d).exists()) return out;
    QString root = QDir(d).absolutePath();
    if (!root.endsWith(QLatin1Char('/'))) root += QLatin1Char('/');
    QDirIterator it(d, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString abs = it.next();
        abs.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (!abs.startsWith(root)) continue;
        // A half-written replacement is not a replacement. put() renames its
        // temporary into place, so a .part left behind is a copy that failed
        // and it must not be reported as installed.
        if (abs.endsWith(QLatin1String(".part"))) continue;
        out << QLatin1Char('/') + abs.mid(root.size());
    }
    out.sort();
    return out;
}

}  // namespace modfolder
