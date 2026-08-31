// TextureUsers.h — which MODELS use a texture, and in what role (template §7).
//
// The Textures tab can say everything about a .ftex except the one thing
// anyone actually asks: *what is this on?* The information exists — every
// .fmdl names its materials and every material names its textures by
// PathCode64 — but only in the forward direction, so answering the reverse
// question means walking every model once and keeping the result.
//
// Which is what this does: ONE sweep on a worker thread, its result cached to
// `data\fox_texusers_v<N>.bin` and fingerprinted exactly like the container
// cache, so it happens once per install rather than once per launch. Nothing
// waits for it — the panel says how far it has got and fills in when it lands.
//
// WHY A FULL SWEEP AND NOT SOMETHING CLEVERER. There is no index from texture
// to model in the game data. A per-texture query would have to read every
// model anyway, so the only choice is whether that cost is paid once or every
// time; measured at roughly a millisecond per model, once is obviously right.
// It also buys the two filters §7 asks for and a lazy query could not offer at
// all: "orphans only" and "filter by the tags of the assets that use it".
//
// The key is the FULL 64-bit PathFileNameCode — extension bits included — which
// is what an FMDL material actually stores and what `IndexedFile::hash` holds.
// (The comment on FmdlTextureRef::pathHash calls it extension-less; measured,
// it is not, and masking with kPathMask here would make every lookup miss.)
// It is deliberately NOT the file index: indices are renumbered by every
// rescan, and this map outlives one.
#pragma once
#include <QHash>
#include <QMutex>
#include <QSet>
#include <functional>
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>

namespace fox {

// The cache file's version. Declared HERE rather than in the .cpp because
// ArchiveIndex::pruneOldCaches has to count up to it, and a hand-copied
// duplicate in another translation unit is a number that silently stops being
// maintained — which is the exact failure the pruning block was written to
// prevent.
constexpr quint32 kTexUsersCacheVersion = 1;

// One place a texture is used: a model, one of its materials, and the slot.
struct TextureUse {
    quint64 modelHash = 0;    // the model's own PathCode64
    QString modelPath;        // resolved, or "" for a hashed-only model
    QString material;         // the material instance's name
    QString shader;           // its shader, for the role column's tooltip
    QString role;             // "Base_Tex_SRGB", "NormalMap_Tex_NRM", …
};

class TextureUsers : public QObject {
    Q_OBJECT
public:
    static TextureUsers& instance();

    // Failed is a REAL state and is now actually assigned: a sweep abandoned
    // because the index was rebuilt under it used to leave `Building` set for
    // ever, and anything waiting on ready() waited for ever with it.
    enum class State { Idle, Building, Ready, Failed };
    State state() const { return m_state.load(); }
    bool ready() const { return m_state.load() == State::Ready; }
    // Progress for the panel's one line. Both zero before the sweep starts.
    int done() const { return m_done.load(); }
    int total() const { return m_total.load(); }

    // Everything that uses this texture, by its extension-less PathCode64.
    // Empty for an unused texture AND for an unfinished sweep, so callers must
    // check ready() before calling a texture an orphan.
    QVector<TextureUse> usesOf(quint64 textureHash) const;
    // How many distinct MODELS use it — the number the list column wants,
    // without building the vector.
    int userCount(quint64 textureHash) const;
    // Every texture hash whose users satisfy `modelOk`. ONE pass over the map,
    // which is why this exists rather than a lookup per texture: the Textures
    // tab's "tags of the models that use it" filter runs over a quarter of a
    // million rows, and asking per row copied a vector under a mutex each time.
    QSet<quint64> texturesWhere(
        const std::function<bool(quint64 modelHash)>& modelOk) const;

    // How many textures the sweep saw referenced at all, and how many models
    // it read. For the status line.
    int textureCount() const;
    // Guarded like everything else the sweep publishes. It was a bare int
    // written on the worker thread and read on the GUI thread, and it is the
    // number in the sentence a user reads to decide whether a texture is
    // genuinely an orphan.
    int modelCount() const;
    // Models this sweep could not take a texture reference OUT of — Ground
    // Zeroes string-table models store their textures by NAME and leave the
    // path hash at zero, so they contribute nothing in this direction. Nonzero
    // means "no model uses it" is not the whole truth, and the panel says so
    // instead of stating a confident wrong answer.
    int opaqueModelCount() const;

    // Start the sweep if it is not already running or done. Safe to call
    // repeatedly — the index-ready signal does, once per rebuild.
    void build();
    // Stop before the singletons this leans on are destroyed. Called from the
    // same place the thumbnail caches are shut down: a detached sweep that
    // outlives QApplication locks a destroyed mutex and reads a destroyed
    // ArchiveIndex, and a harness run quits mid-sweep every time.
    void shutdown();
    // The archives changed: drop everything and (if the caller wants) sweep
    // again. Called from the same place the other catalogues are invalidated.
    void reset();

Q_SIGNALS:
    // Every few hundred models, so a panel can show progress without polling.
    void progress(int done, int total);
    void finished(bool ok);

private:
    TextureUsers() = default;
    void sweep();                       // runs on the worker thread
    bool loadCache(const QString& fingerprint);
    void saveCache(const QString& fingerprint) const;

    QMutex* mutexPtr() const;   // see the .cpp — one static mutex

    QHash<quint64, QVector<TextureUse>> m_byTexture;
    int m_models = 0;         // guarded by the mutex
    int m_opaqueModels = 0;   // guarded by the mutex
    std::atomic<State> m_state{State::Idle};
    // A sweep is in flight. Distinct from State: reset() puts the state back to
    // Idle so the UI stops trusting the map, but the thread is still running,
    // and starting a second one over every model in the install is not what
    // "rescan" should mean.
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<int> m_done{0};
    std::atomic<int> m_total{0};
    std::atomic<quint64> m_generation{0};
};

}  // namespace fox
