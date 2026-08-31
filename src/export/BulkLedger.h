// BulkLedger.h — the two files a bulk run leaves beside its output (§8).
//
//   _bulk_manifest.json   what has already been written, so "only new" can
//                         skip it on the next run
//   _bulk_failed.txt      what did not get written, and WHY, one line each
//
// Both live in the OUTPUT folder rather than in settings, which is the whole
// point: the ledger belongs to the folder, so pointing a new install (or a
// second machine) at the same folder picks up where the last run stopped, and
// deleting the folder deletes its history with it. A ledger in the INI would
// claim files were already exported into a folder that no longer exists.
//
// The key is the entry's 64-bit path hash, NOT its path. An unnamed file has no
// path — it is listed under its hash — and a dictionary update that finally
// resolves one would otherwise make it look new and export it a second time.
#pragma once
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

namespace fox {

class BulkManifest {
public:
    // Reads the ledger if one is there. A corrupt or unreadable file is an
    // EMPTY ledger and a warning, never an error: the worst that does is
    // re-export files that were already written, and refusing to run because a
    // JSON file lost a brace would be worse.
    explicit BulkManifest(const QString& outRoot);

    // "Already written" means THIS hash was written to THIS path. Keying on
    // the hash alone was wrong in a way that looked like the tool breaking:
    // change Settings ▸ Export ▸ Group by, re-run the same filter into the same
    // folder, and every file is skipped — so the grouped folders are never
    // created and the summary says "0 written, 5000 skipped" with nothing
    // pointing at the cause. The path is part of what was done, so it is part
    // of the key.
    bool has(quint64 hash, const QString& rel) const;
    // Thread-safe: workers call this as they finish. ONLY for a complete
    // write — a degraded one (a header-only .ftex whose mip streams were not
    // mounted, a .wem that vgmstream could not convert) must NOT be recorded,
    // or "only new" will skip it for ever and the folder stays stuck with
    // unusable files.
    void record(quint64 hash, const QString& rel);
    int size() const;
    // Write it out. Called when the run ends — including when it is cancelled,
    // because the files written before the cancel really are written and a
    // later "only new" must skip them — and periodically DURING a long run, so
    // that closing the app or losing power at 40% of a 50,000-file extraction
    // does not throw away the ledger for all 20,000 files already on disk.
    // A no-op when nothing has been recorded since the last save.
    bool save(QString* error = nullptr);
    bool dirty() const;
    QString path() const { return m_path; }

private:
    QString m_path;
    mutable QMutex m_mx;
    // hash → every path this hash has been written to. A hash can legitimately
    // be written twice (two layouts, two name templates) and both are real.
    QHash<quint64, QStringList> m_seen;
    bool m_dirty = false;
};

class BulkFailureLog {
public:
    explicit BulkFailureLog(const QString& outRoot);

    // One failure, with the reason. Thread-safe.
    void add(const QString& what, const QString& reason);
    int count() const;
    // Write the file, or REMOVE a stale one from an earlier run when this run
    // had no failures — a leftover _bulk_failed.txt listing yesterday's
    // problems next to today's clean output is worse than no file at all.
    //
    // `completed` is false for a cancelled run, and then a stale file is LEFT
    // ALONE: cancelling a fresh run two seconds in deleted the 300 reasons the
    // previous run had written, which existed nowhere else.
    bool flush(bool completed);
    QString path() const { return m_path; }

private:
    QString m_path;
    mutable QMutex m_mx;
    QStringList m_lines;
};

}  // namespace fox
