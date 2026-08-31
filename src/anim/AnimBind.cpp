// AnimBind.cpp — see AnimBind.h.
#include "anim/AnimBind.h"

#include <QHash>

#include "anim/RigBind.h"
#include "fox/FmdlFile.h"
#include "fox/FrigFile.h"
#include "index/AnimCatalog.h"
#include "index/ArchiveIndex.h"

namespace animbind {
namespace {

struct Cache {
    const void* indexKey = nullptr;
    int indexCount = -1;
    QHash<QString, Binding> byPath;
};

Cache& cache()
{
    static Cache c;
    return c;
}

}  // namespace

void clearCache()
{
    cache().byPath.clear();
    cache().indexKey = nullptr;
    cache().indexCount = -1;
}

Binding forModel(const QString& modelPath)
{
    Binding b;
    if (modelPath.isEmpty()) return b;

    const fox::ArchiveIndex& index = fox::ArchiveIndex::instance();
    const auto& files = index.files();
    Cache& c = cache();
    if (c.indexKey != files.constData() || c.indexCount != files.size()) {
        c.byPath.clear();
        c.indexKey = files.constData();
        c.indexCount = files.size();
    }
    const auto hit = c.byPath.constFind(modelPath);
    if (hit != c.byPath.constEnd()) return hit.value();

    const int fileIdx = index.fileIndexForPath(modelPath);
    if (fileIdx < 0) return b;

    fox::FmdlFile model;
    if (!model.parse(index.readFile(files[fileIdx]))) return b;

    b.modelPath = modelPath;
    b.label = modelPath.section(QLatin1Char('/'), -1)
                  .section(QLatin1Char('.'), 0, 0);
    const QVector<quint32> bones = fox::modelBoneHashes(model);
    b.bones = bones.size();

    fox::FrigFile frig;
    b.haveRig = rigbind::loadFrigFor(modelPath, &frig, &b.rigVia);
    const fox::FrigFile* rig = b.haveRig ? &frig : nullptr;
    b.ceiling = fox::animBindCeiling(bones, rig);

    // A model with no drivable bones binds to NOTHING, and says so rather than
    // falling back to "show everything". A one-bone prop genuinely has no
    // animations; pretending the whole install applies to it would be the
    // filename-guess this whole module exists to avoid.
    if (b.ceiling > 0) {
        const fox::AnimCatalog& cat = fox::AnimCatalog::instance();
        const float want = fox::animBindThreshold();
        for (const fox::AnimArchive& a : cat.archives()) {
            if (fox::animBindScore(a, bones, rig, nullptr, b.ceiling) < want)
                continue;
            b.archives.insert(a.fileIdx);
            b.clips += a.clips.size();
        }
    }

    // Bounded: the panel asks once per model change and a session can walk
    // hundreds of models. Cleared wholesale rather than by age — this is a
    // lookaside for the row the user just clicked, not a store.
    if (c.byPath.size() > 64) c.byPath.clear();
    c.byPath.insert(modelPath, b);
    return b;
}

}  // namespace animbind
