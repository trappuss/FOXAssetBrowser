// LayerColors.cpp — see LayerColors.h.
#include "index/LayerColors.h"

#include <QImage>
#include <QRegularExpression>
#include <algorithm>

#include "fox/FmdlFile.h"
#include "index/ArchiveIndex.h"
#include "util/Extract.h"

namespace fox {

namespace {
// Where the shipped swatches live. Matched as a SUFFIX of the directory part
// rather than a full path, because the game prefix in front of it varies
// (/Assets/tpp/, /Assets/ssd/, /Assets/mgo/) and a palette that only ever
// looked under tpp/ would come back empty on an install mounted without it.
const char kLayerDir[] = "/common_source/layer/";

// "cm_scol4_c07_lym" → family "cm_scol4", id "c07". Anything that does not
// match this shape is not a colour swatch and is skipped rather than guessed
// at — the same directory also holds one-off layer art for specific models.
const QRegularExpression& swatchRe()
{
    static const QRegularExpression re(
        QStringLiteral("^(cm_(?:scol|camo)\\d+)_(c\\d+)_lym$"));
    return re;
}
}  // namespace

LayerColorCatalog& LayerColorCatalog::instance()
{
    static LayerColorCatalog s;
    return s;
}

void LayerColorCatalog::build()
{
    m_solids.clear();
    m_patterns.clear();
    m_colorCache.clear();
    m_note.clear();

    const ArchiveIndex& ix = ArchiveIndex::instance();
    // A texture reaches the index as both .ftex and .N.ftexs, and a mounted
    // loose folder can shadow an archived copy. Keyed by stem so each swatch
    // is offered exactly once however many entries carry it.
    QHash<QString, LayerSwatch> seen;
    for (const IndexedFile& f : ix.files()) {
        if (!f.path.endsWith(QLatin1String(".ftex"), Qt::CaseInsensitive)) continue;
        if (!f.path.contains(QLatin1String(kLayerDir), Qt::CaseInsensitive))
            continue;
        QString stem = f.path.section(QLatin1Char('/'), -1);
        stem.truncate(stem.lastIndexOf(QLatin1Char('.')));
        const QRegularExpressionMatch m = swatchRe().match(stem);
        if (!m.hasMatch()) continue;
        // Keyed on the stem alone: cm_scol4_c00 is the same colour whichever
        // game's tree it was found in, and offering it twice would give the
        // combo two identical rows that paint identically.
        //
        // A SHADOWED entry never wins over an unshadowed one. On a modded
        // install a replacement archive can carry a copy the game would not
        // load; keeping whichever came first in index order meant the chip
        // and the applied colour could both be the wrong file.
        const auto prev = seen.constFind(stem);
        if (prev != seen.constEnd() && !(prev->shadowed && !f.shadowed)) continue;
        LayerSwatch s;
        s.family = m.captured(1);
        s.id = m.captured(2);
        s.path = f.path;
        s.basePath = f.path;
        s.basePath.truncate(s.basePath.lastIndexOf(QLatin1Char('.')));
        s.pathHash = f.hash;
        s.solid = s.family.contains(QLatin1String("scol"));
        s.shadowed = f.shadowed;
        seen.insert(stem, s);
    }

    for (const LayerSwatch& s : seen)
        (s.solid ? m_solids : m_patterns).append(s);

    const auto byFamilyThenId = [](const LayerSwatch& a, const LayerSwatch& b) {
        if (a.family != b.family) return a.family < b.family;
        return a.id < b.id;
    };
    std::sort(m_solids.begin(), m_solids.end(), byFamilyThenId);
    std::sort(m_patterns.begin(), m_patterns.end(), byFamilyThenId);

    m_note = QStringLiteral("%1 solid colour(s) and %2 pattern(s)")
                 .arg(m_solids.size())
                 .arg(m_patterns.size());
}

QColor LayerColorCatalog::colorOf(const LayerSwatch& s) const
{
    const auto it = m_colorCache.constFind(s.pathHash);
    if (it != m_colorCache.constEnd()) return it.value();

    // Decode at MIP resolution. A solid swatch is one colour at every level so
    // the small mip is exact, and for a pattern the average of a small mip is
    // the average of the pattern — which is all a chip can show.
    fox::FmdlTextureRef ref;
    ref.pathHash = s.pathHash;
    ref.path = s.basePath;
    const QImage img = extract::textureImageFor(ref, /*gzModel=*/false,
                                                /*lowRes=*/true);
    QColor out;   // invalid until proven otherwise
    if (!img.isNull()) {
        const QImage rgb = img.convertToFormat(QImage::Format_RGB888);
        qint64 r = 0, g = 0, b = 0, n = 0;
        for (int y = 0; y < rgb.height(); ++y) {
            const uchar* row = rgb.constScanLine(y);
            for (int x = 0; x < rgb.width(); ++x, ++n) {
                r += row[x * 3 + 0];
                g += row[x * 3 + 1];
                b += row[x * 3 + 2];
            }
        }
        if (n > 0) out = QColor(int(r / n), int(g / n), int(b / n));
    }
    // Only a SUCCESSFUL decode is cached. A texture whose mips were not in the
    // archive this time — a transient read failure, a streamed-away level —
    // would otherwise be remembered as colourless for the life of the process
    // and never retried.
    if (out.isValid()) m_colorCache.insert(s.pathHash, out);
    return out;
}

}  // namespace fox
