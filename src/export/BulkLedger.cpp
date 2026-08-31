#include "export/BulkLedger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>

namespace fox {

namespace {
QString manifestName() { return QStringLiteral("_bulk_manifest.json"); }
QString failedName()   { return QStringLiteral("_bulk_failed.txt"); }
}  // namespace

BulkManifest::BulkManifest(const QString& outRoot)
    : m_path(QDir(outRoot).filePath(manifestName()))
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) return;   // no ledger yet: normal
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("bulk: %s is unreadable (%s) — treating it as empty",
                 qUtf8Printable(manifestName()), qUtf8Printable(err.errorString()));
        return;
    }
    const QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        // The hash is stored as a STRING of hex. A 64-bit value does not
        // survive JSON's double, and the top bits of a Fox path hash are
        // exactly the ones that would be lost.
        bool ok = false;
        const quint64 h =
            o.value(QStringLiteral("hash")).toString().toULongLong(&ok, 16);
        if (!ok) continue;
        // "files" is the current shape; "file" is what version 1 wrote, and a
        // ledger written by that build still has to be readable — otherwise
        // upgrading the tool silently re-exports everything the user has.
        QStringList paths;
        const QJsonArray fs = o.value(QStringLiteral("files")).toArray();
        for (const QJsonValue& fv : fs) paths.append(fv.toString());
        const QString one = o.value(QStringLiteral("file")).toString();
        if (!one.isEmpty() && !paths.contains(one)) paths.append(one);
        if (!paths.isEmpty()) m_seen.insert(h, paths);
    }
    qInfo("bulk: %s lists %d already-exported file(s)",
          qUtf8Printable(manifestName()), int(m_seen.size()));
}

bool BulkManifest::has(quint64 hash, const QString& rel) const
{
    QMutexLocker lock(&m_mx);
    const auto it = m_seen.constFind(hash);
    return it != m_seen.constEnd() && it->contains(rel, Qt::CaseInsensitive);
}

void BulkManifest::record(quint64 hash, const QString& rel)
{
    QMutexLocker lock(&m_mx);
    QStringList& paths = m_seen[hash];
    if (!paths.contains(rel, Qt::CaseInsensitive)) paths.append(rel);
    m_dirty = true;
}

bool BulkManifest::dirty() const
{
    QMutexLocker lock(&m_mx);
    return m_dirty;
}

int BulkManifest::size() const
{
    QMutexLocker lock(&m_mx);
    return m_seen.size();
}

bool BulkManifest::save(QString* error)
{
    QMutexLocker lock(&m_mx);
    if (!m_dirty) return true;
    QJsonArray items;
    for (auto it = m_seen.constBegin(); it != m_seen.constEnd(); ++it) {
        QJsonObject o;
        o.insert(QStringLiteral("hash"),
                 QStringLiteral("%1").arg(it.key(), 16, 16, QLatin1Char('0')));
        QJsonArray paths;
        for (const QString& s : it.value()) paths.append(s);
        o.insert(QStringLiteral("files"), paths);
        items.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("tool"), QStringLiteral("FOXAssetBrowser"));
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("updated"),
                QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("items"), items);

    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = out.errorString();
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        if (error) *error = out.errorString();
        return false;
    }
    m_dirty = false;
    return true;
}

BulkFailureLog::BulkFailureLog(const QString& outRoot)
    : m_path(QDir(outRoot).filePath(failedName()))
{
}

void BulkFailureLog::add(const QString& what, const QString& reason)
{
    QMutexLocker lock(&m_mx);
    m_lines.append(what + QStringLiteral("\t") + reason);
}

int BulkFailureLog::count() const
{
    QMutexLocker lock(&m_mx);
    return m_lines.size();
}

bool BulkFailureLog::flush(bool completed)
{
    QMutexLocker lock(&m_mx);
    if (m_lines.isEmpty()) {
        // Nothing failed. Remove a file left by an EARLIER run rather than
        // leaving stale failures beside clean output — but only when this run
        // actually FINISHED. A run cancelled two seconds in has not learnt
        // that the previous run's 300 failures are no longer true, and those
        // reasons exist nowhere else.
        if (completed) QFile::remove(m_path);
        return true;
    }
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly)) return false;
    QByteArray text;
    text += "# FOXAssetBrowser bulk extract — files that did not get written\n";
    text += "# ";
    text += QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8();
    text += "\n# <file>\\t<reason>\n";
    for (const QString& l : m_lines) {
        text += l.toUtf8();
        text += '\n';
    }
    out.write(text);
    return out.commit();
}

}  // namespace fox
