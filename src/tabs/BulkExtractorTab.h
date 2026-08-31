// BulkExtractorTab.h — filtered mass extraction across every indexed archive
// (template §8).
//
// The tab holds ONLY RUN CONTROLS: what to run over (the filter, or a queue you
// built by hand), whether to skip what a previous run already wrote, how many
// workers, and start/pause/cancel. Every *export option* — what a .ftex or a
// .wem turns into on the way out, what folders the run writes into, what the
// files are called — lives in Settings ▸ Export, shared with every other export
// path. Two of them used to be checkboxes here, which meant a bulk run and a
// right-click export could disagree about whether a texture becomes a .dds.
//
// The pieces that are not UI live beside it: `export/BulkLedger.h` for the
// manifest and the failure list, `util/ExportLayout.h` for the folders, and
// `extract::blobcache` for the run-scoped decode cache.
#pragma once
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <atomic>

#include "util/Extract.h"
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

namespace fox {
class BulkManifest;
class BulkFailureLog;
}

class BulkExtractorTab : public QWidget {
    Q_OBJECT
public:
    explicit BulkExtractorTab(QWidget* parent = nullptr);
    ~BulkExtractorTab() override;

    // Export-menu presets: prefill the filter fields (does not start the run —
    // the user reviews the match count and output folder first).
    void applyPreset(const QString& query, const QString& ext);

    // True while worker threads hold references into the index — a rescan
    // must not swap the file tables under them.
    bool extracting() const { return m_running.load(); }

    // ── The headless harness (§8, and the project's verification bar) ────
    // Configure and start a run from the command line, and say when it is
    // over. The run machinery is the biggest thing in this tool that a script
    // could not reach, which meant "does only-new actually skip anything" had
    // no answer but a click-through.
    void configureRun(const QString& query, const QString& ext,
                      const QString& outDir, int workers, bool overwrite,
                      bool useQueue);
    void startRun();          // as if Extract had been pressed
    void cancelForShot();     // …as if Cancel had been
    void pauseForShot(bool on);   // …and Pause / Resume
    void addMatchesToQueue(); // as if Add matches had been pressed

signals:
    // Emitted once, when the workers have drained and the ledgers are written.
    void runFinished(int written, int failed, int skipped);

public:

public slots:
    void onIndexReady(bool ready);

protected:
    // Esc cancels a run (§8: "a working Cancel (and Esc)"). It does NOT close
    // or clear anything when nothing is running, because a tab that swallows
    // Esc is a tab whose search box cannot be cleared with it.
    void keyPressEvent(QKeyEvent* e) override;

private:
    // The files the FILTER selects, in index order.
    QVector<int> matchedFiles() const;
    // What the run will actually process: the filter's matches, or the queue.
    QVector<int> runSet() const;
    void updateMatchCount();
    void startExtraction();
    void cancelExtraction();
    void togglePause();
    void pollProgress();
    // Console lines are appended by workers into m_pending and drained here,
    // because a worker thread must not touch a widget.
    void drainConsole();
    void log(const QString& line);

    // ── The queue (§8: "survives filter changes, mode switches and restarts")
    // Stored as HEX HASHES, not file indices: a rescan renumbers every file,
    // and an index restored from a stale number would extract whatever now
    // happens to sit at that row.
    void loadQueue();
    void saveQueue() const;
    void queueAddMatches();
    void queueRemoveMatches();
    void queueClear();
    // Both counts from ONE index pass. They were two, and every queue click ran
    // four passes over a 200,000-row index on the GUI thread.
    struct Counts { int matches = 0; int run = 0; int queuePresent = 0; };
    Counts counts() const;
    void updateQueueLabel();
    void updateQueueLabel(const Counts& c);
    // The ledgers, written whenever a run stops for any reason.
    void finishLedgers(bool completed);

    QLineEdit* m_query = nullptr;
    QComboBox* m_extension = nullptr;
    QComboBox* m_preset = nullptr;
    QCheckBox* m_namedOnly = nullptr;
    QCheckBox* m_includeContainers = nullptr;
    QLabel* m_matchLabel = nullptr;
    QLineEdit* m_outputDir = nullptr;
    QPushButton* m_browseBtn = nullptr;

    QComboBox* m_source = nullptr;        // filter matches | the queue
    QComboBox* m_existing = nullptr;      // only new | overwrite
    QSpinBox* m_workers = nullptr;        // 0 = auto
    QPushButton* m_queueAdd = nullptr;
    QPushButton* m_queueRemove = nullptr;
    QPushButton* m_queueClear = nullptr;
    QLabel* m_queueLabel = nullptr;

    QPushButton* m_startBtn = nullptr;
    QPushButton* m_pauseBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;
    QPlainTextEdit* m_console = nullptr;
    QTimer* m_poll = nullptr;

    QSet<quint64> m_queue;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int> m_done{0};
    std::atomic<int> m_failed{0};
    std::atomic<int> m_skipped{0};
    std::atomic<int> m_total{0};
    std::atomic<int> m_threadsLeft{0};

    // ETA arithmetic. `m_pausedMs` accumulates only COMPLETED pauses; the one
    // in progress is added at read time from m_pauseStart, so an estimate taken
    // while paused does not drift upwards second by second.
    QElapsedTimer m_clock;
    qint64 m_pausedMs = 0;
    qint64 m_pauseStart = -1;

    // Shared with the workers, so they outlive a cancel that returns before the
    // threads drain.
    std::shared_ptr<fox::BulkManifest> m_manifest;
    std::shared_ptr<fox::BulkFailureLog> m_failures;
    std::shared_ptr<QStringList> m_pending;   // console lines, mutex-guarded
    std::shared_ptr<class QMutex> m_pendingMx;
    // The run-scoped decode cache is owned HERE, not by the workers. A worker
    // that owned it released it as its lambda was destroyed — which happens
    // AFTER the m_threadsLeft decrement the rescan guard and the summary both
    // watch, so a rescan could start with a scope still open and the run's
    // cache line could only ever report zero bytes held.
    std::shared_ptr<extract::blobcache::Scope> m_cache;
    std::atomic<int> m_degraded{0};
    // Set by the harness. A harness run must not rewrite the user's export
    // folder or their hand-built queue — the same rule ExportOptions states
    // for the session overrides.
    bool m_harness = false;
    // Read once per run and per filter change, not per matched file: it opens
    // QSettings and parses the INI, and matchedFiles() calls it on every
    // keystroke over every row.
    bool m_assembleFtex = true;
    // Poll ticks since the manifest was last checkpointed (see pollProgress).
    int m_saveTick = 0;
    int m_statusTick = 0;
};
