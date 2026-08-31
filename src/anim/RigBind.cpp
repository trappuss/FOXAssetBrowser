// RigBind.cpp — see RigBind.h.
#include "anim/RigBind.h"

#include "fox/Fox2File.h"
#include "index/ArchiveIndex.h"

using fox::ArchiveIndex;

namespace rigbind {
namespace {

bool loadFrigPath(const QString& frigPath, fox::FrigFile* out)
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    // Through the index's own map. This was a full linear walk of every entry
    // comparing QStrings, run twice per model load — once for the rig the
    // .parts names and once more if that missed.
    const int i = index.fileIndexForPath(frigPath);
    if (i < 0) return false;
    const QByteArray data = index.readFile(index.files()[i]);
    return !data.isEmpty() && out->parse(data);
}

}  // namespace

// modelFile → (gameRigFile, boundVia) map, built ONCE per index generation.
// Scanning every .parts costs seconds on a full game; per-model-click that
// would stall the GUI, so the sweep runs once and clicks hit the map.
struct RigMapCache {
    const void* indexKey = nullptr;   // files() vector identity
    int indexCount = -1;
    QHash<QString, QPair<QString, QString>> modelToRig;   // path → (rig, via)
};

static const RigMapCache& rigMap()
{
    static RigMapCache cache;
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();
    if (cache.indexKey == files.constData() && cache.indexCount == files.size())
        return cache;

    cache.indexKey = files.constData();
    cache.indexCount = files.size();
    cache.modelToRig.clear();
    for (int i = 0; i < files.size(); ++i) {
        if (ArchiveIndex::extensionOf(files[i]) != QLatin1String("parts"))
            continue;
        const QByteArray pd = index.readFile(files[i]);
        if (pd.isEmpty() || !fox::Fox2File::isFox2(pd)) continue;
        fox::Fox2File fx;
        if (!fx.parse(pd)) continue;
        for (const fox::Fox2Entity& e : fx.entities()) {
            if (e.className != QLatin1String("ModelDescription")) continue;
            const fox::Fox2Property* mf = e.find(QStringLiteral("modelFile"));
            const fox::Fox2Property* rig = e.find(QStringLiteral("gameRigFile"));
            if (!mf || mf->values.isEmpty() || !rig || rig->values.isEmpty())
                continue;
            const QString model = mf->values[0].toString();
            if (!model.isEmpty() && !cache.modelToRig.contains(model))
                cache.modelToRig.insert(
                    model, {rig->values[0].toString(), files[i].path});
        }
    }
    return cache;
}

bool loadFrigFor(const QString& modelPath, fox::FrigFile* out, QString* boundVia)
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    const auto& files = index.files();

    // 1) ModelDescription binding (cached sweep).
    if (!modelPath.isEmpty()) {
        const auto it = rigMap().modelToRig.constFind(modelPath);
        if (it != rigMap().modelToRig.constEnd()
            && loadFrigPath(it.value().first, out)) {
            if (boundVia) *boundVia = it.value().second;
            return true;
        }
    }

    // 2) humanoid heuristic.
    int best = -1, bestScore = -1;
    for (int i = 0; i < files.size(); ++i) {
        if (ArchiveIndex::extensionOf(files[i]) != QLatin1String("frig")) continue;
        const QString& p = files[i].path;
        int score = 1;
        if (p.endsWith(QLatin1String("human_finger.frig"))) score = 3;
        else if (p.contains(QLatin1String("human"))) score = 2;
        if (score > bestScore) { bestScore = score; best = i; }
        if (score == 3) break;
    }
    if (best >= 0 && loadFrigPath(files[best].path, out)) {
        if (boundVia) *boundVia = QStringLiteral("heuristic");
        return true;
    }
    return false;
}

}  // namespace rigbind
