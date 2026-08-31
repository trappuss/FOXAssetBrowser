// IconCatalog.cpp — see IconCatalog.h.
#include "index/IconCatalog.h"

#include <QImage>
#include <QPainter>
#include <QPen>

#include "fox/BcDecode.h"
#include "fox/FoxHash.h"
#include "index/ArchiveIndex.h"
#include "index/EquipCatalog.h"
#include "index/NameCatalog.h"
#include "util/Extract.h"

namespace fox {

IconCatalog& IconCatalog::instance()
{
    static IconCatalog cache;
    return cache;
}

void IconCatalog::reset()
{
    m_cache.clear();
    // Both tables, or a rescan would keep answering "line art" for a stem whose
    // icon has been replaced by a mod with something else entirely.
    m_lineArt.clear();
}

QPixmap IconCatalog::iconFor(const QString& modelStem, int height)
{
    if (modelStem.isEmpty() || height <= 0) return {};
    const QString key = modelStem + QLatin1Char('@') + QString::number(height);
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return it.value();
    // A failed decode is cached as a null pixmap on purpose: without that, a
    // part with no icon would be re-assembled from the archives on every single
    // repaint of the list it sits in.
    const QPixmap p = decode(modelStem, height);
    m_cache.insert(key, p);
    return p;
}

QPixmap IconCatalog::swatchForPath(const QString& assetPathNoExt, int size)
{
    if (assetPathNoExt.isEmpty() || size <= 0) return {};
    const QString key = QStringLiteral("\x02") + assetPathNoExt
        + QLatin1Char('@') + QString::number(size);
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return it.value();
    const QPixmap p = decodePath(assetPathNoExt, size);
    m_cache.insert(key, p);
    return p;
}

// ── Derived swatches ────────────────────────────────────────────────────────
//
// Some appearance choices ship no icon of their own. Survive's avatar UI has
// artwork for face presets, hairstyles, eyebrows and features
// (/ui/texture/Avatar_mgo/{base,hair,eyeblow,deco}) and for NOTHING else —
// there is no shipped hair-colour chip, skin-tone chip, wrinkle thumbnail or
// beard picture. (The richer /ui/texture/Avatar set exists in the name
// dictionary but is not in the Survive archives; I scanned for it.)
//
// Rather than show a raw 512x512 face UV map as a row icon, which is what
// handing the texture path straight in does, a caller can ask for a DERIVED
// swatch with a small spec in place of a path:
//
//   crop:<x>,<y>,<w>,<h>:<path>   the given normalised sub-rect of the texture
//   avg:<x>,<y>,<w>,<h>:<path>    a flat chip of that region's average colour
//   alpha:<path>                  alpha-trimmed to its content, over a neutral
//                                 backing (hair and beard maps are strand
//                                 atlases; the colour and density is the point)
//
// The rects are in UV space so they survive a texture shipping at a different
// resolution. Everything else about the swatch path — caching, sizing — is
// unchanged, so a spec can be handed to anything that takes a swatch path.
namespace {
// QString::number with an explicit format is C-locale; arg(double) is not
// guaranteed to be. A spec is parsed by splitting on ',', so one comma decimal
// separator from a European locale would not merely misplace the crop, it
// would make the whole spec unparseable and the row would lose its icon.
QString num(qreal v) { return QString::number(v, 'g', 6); }
}  // namespace

QString IconCatalog::cropSpec(const QString& path, qreal x, qreal y, qreal w,
                              qreal h)
{
    if (path.isEmpty()) return {};
    return QStringLiteral("crop:") + num(x) + QLatin1Char(',') + num(y)
        + QLatin1Char(',') + num(w) + QLatin1Char(',') + num(h)
        + QLatin1Char(':') + path;
}

QString IconCatalog::avgSpec(const QString& path, qreal x, qreal y, qreal w,
                             qreal h)
{
    if (path.isEmpty()) return {};
    return QStringLiteral("avg:") + num(x) + QLatin1Char(',') + num(y)
        + QLatin1Char(',') + num(w) + QLatin1Char(',') + num(h)
        + QLatin1Char(':') + path;
}

QString IconCatalog::alphaSpec(const QString& path)
{
    if (path.isEmpty()) return {};
    return QStringLiteral("alpha:") + path;
}

QPixmap IconCatalog::derivedSwatch(const QString& spec, int size)
{
    const int firstColon = spec.indexOf(QLatin1Char(':'));
    if (firstColon < 0) return {};
    const QString verb = spec.left(firstColon);
    QString rest = spec.mid(firstColon + 1);
    QRectF uv(0, 0, 1, 1);
    if (verb == QLatin1String("crop") || verb == QLatin1String("avg")) {
        const int second = rest.indexOf(QLatin1Char(':'));
        if (second < 0) return {};
        const QStringList n = rest.left(second).split(QLatin1Char(','));
        if (n.size() != 4) return {};
        uv = QRectF(n[0].toDouble(), n[1].toDouble(), n[2].toDouble(),
                    n[3].toDouble());
        rest = rest.mid(second + 1);
    } else if (verb != QLatin1String("alpha")) {
        return {};
    }
    const QImage img = decodeImage(rest);
    if (img.isNull()) return {};
    if (img.width() < 4 || img.height() < 4) return QPixmap::fromImage(img);

    QRect px(int(uv.x() * img.width()), int(uv.y() * img.height()),
             qMax(1, int(uv.width() * img.width())),
             qMax(1, int(uv.height() * img.height())));
    px = px.intersected(img.rect());
    if (px.isEmpty()) return {};

    if (verb == QLatin1String("avg")) {
        // Average only the OPAQUE pixels: a hair atlas is mostly empty, and
        // averaging the transparent parts in would wash every colour out.
        quint64 r = 0, g = 0, b = 0, n = 0;
        const QImage cut = img.copy(px).convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < cut.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(cut.constScanLine(y));
            for (int x = 0; x < cut.width(); ++x) {
                if (qAlpha(line[x]) < 128) continue;
                r += quint32(qRed(line[x]));
                g += quint32(qGreen(line[x]));
                b += quint32(qBlue(line[x]));
                ++n;
            }
        }
        if (!n) return {};
        QPixmap pm(size, size);
        pm.fill(QColor(int(r / n), int(g / n), int(b / n)));
        return pm;
    }

    QImage cut;
    if (verb == QLatin1String("alpha")) {
        // Trim to the content box so a mostly-empty atlas fills its chip.
        const QImage a = img.convertToFormat(QImage::Format_ARGB32);
        int x0 = a.width(), y0 = a.height(), x1 = -1, y1 = -1;
        for (int y = 0; y < a.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(a.constScanLine(y));
            for (int x = 0; x < a.width(); ++x)
                if (qAlpha(line[x]) >= 64) {
                    x0 = qMin(x0, x); y0 = qMin(y0, y);
                    x1 = qMax(x1, x); y1 = qMax(y1, y);
                }
        }
        if (x1 < x0 || y1 < y0) return {};
        cut = a.copy(QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
        QImage backed(cut.size(), QImage::Format_ARGB32);
        backed.fill(QColor(206, 178, 158));   // a neutral skin backing
        QPainter bp(&backed);
        bp.drawImage(0, 0, cut);
        bp.end();
        cut = backed;
    } else {
        cut = img.copy(px);
    }
    // Expand to fill the square, then take the MIDDLE of the result. Taking
    // the top-left instead quietly shows the left third of whatever was asked
    // for, which on a face crop is not the feature the caller aimed at.
    const QImage big = cut.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                  Qt::SmoothTransformation);
    return QPixmap::fromImage(big.copy((big.width() - size) / 2,
                                       (big.height() - size) / 2, size, size));
}

QImage IconCatalog::decodeImage(const QString& assetPathNoExt)
{
    const ArchiveIndex& index = ArchiveIndex::instance();
    const IndexedFile* f = index.findByHash(
        hashFileNameWithExtension(assetPathNoExt + QLatin1String(".ftex")));
    if (!f) return {};
    const QByteArray dds = extract::assembleFtexToDds(*f);
    if (dds.isEmpty()) return {};
    return bc::decodeDds(dds);
}

QPixmap IconCatalog::decodePath(const QString& assetPathNoExt, int size)
{
    // A derived spec carries its verb before the first colon; a real asset path
    // starts with '/', so the two can never be confused.
    if (!assetPathNoExt.startsWith(QLatin1Char('/')))
        return derivedSwatch(assetPathNoExt, size);
    const ArchiveIndex& index = ArchiveIndex::instance();
    const IndexedFile* f = index.findByHash(
        hashFileNameWithExtension(assetPathNoExt + QLatin1String(".ftex")));
    if (!f) return {};
    const QByteArray dds = extract::assembleFtexToDds(*f);
    if (dds.isEmpty()) return {};
    const QImage img = bc::decodeDds(dds);
    if (img.isNull()) return {};
    return QPixmap::fromImage(img.scaled(size, size, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
}

QPixmap IconCatalog::crossedOut(int height)
{
    const QString key = QStringLiteral("\x01crossed@%1").arg(height);
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return it.value();

    const int w = height * 9 / 5;   // the game's box is wider than it is tall
    QPixmap pm(w, height);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor red(0xD1, 0x3A, 0x2A);
    QPen pen(red);
    pen.setWidthF(qMax(1.4, height / 14.0));
    p.setPen(pen);
    const QRectF box(pen.widthF(), pen.widthF(), w - 2 * pen.widthF(),
                     height - 2 * pen.widthF());
    p.drawRect(box);
    p.drawLine(box.topLeft(), box.bottomRight());
    p.end();

    m_cache.insert(key, pm);
    return pm;
}

QPixmap IconCatalog::decode(const QString& modelStem, int height)
{
    QString iconPath = NameCatalog::instance().iconPathFor(modelStem);
    // Outfits are not in the weapon-parts table every other icon comes from —
    // they are in the DEVELOPMENT table, keyed by suit family rather than by
    // model stem, which is why the avatar's whole wardrobe used to draw with
    // no picture at all.
    if (iconPath.isEmpty())
        iconPath = EquipCatalog::instance().suitIcon(
            modelStem.section(QLatin1Char('_'), 0, 0));
    if (iconPath.isEmpty()) return {};

    const ArchiveIndex& index = ArchiveIndex::instance();
    const IndexedFile* f = index.findByHash(
        hashFileNameWithExtension(iconPath + QLatin1String(".ftex")));
    if (!f) return {};

    const QByteArray dds = extract::assembleFtexToDds(*f);
    if (dds.isEmpty()) return {};
    QImage img = bc::decodeDds(dds);
    if (img.isNull()) return {};

    // Trim the transparent margin. The icons are authored on a fixed canvas
    // with the art centred, so an untrimmed magazine and an untrimmed receiver
    // scale to wildly different apparent sizes.
    if (img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_ARGB32);
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(row[x]) < 8) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < x0 || y1 < y0) return {};   // fully transparent
    img = img.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1);

    // Is this line art or a photograph? The weapon-parts icons are white line
    // work on transparency, drawn in game over a dark panel, and a row has to
    // recolour them or they are white-on-white under a light theme. The SUIT
    // icons are not line art at all — they are full-colour photographs of the
    // outfit — and recolouring one paints a solid block where the picture was.
    //
    // The test is SATURATION, and only saturation. Measured on the shipped art:
    //
    //   weapon-parts art (ar02 barrel, am00, ba05, sk08)   100.0% grey
    //   ui_st_dld_alp   TUXEDO, the closest photograph      87.2% grey
    //   ui_st_dlb_alp                                       74.8%
    //   ui_st_dlc_alp / dla / sna5 / dle                12–31%
    //
    // Brightness was the obvious axis and it is the wrong one: a first attempt
    // that also required the pixel to be bright classified only 54% of that
    // barrel icon as line art and stopped recolouring it, which left white art
    // on a white row.
    //
    // The cut sits at 95%, between the two clusters rather than beside one of
    // them: 5 points below the line art and 7.8 above the near-monochrome
    // tuxedo. It was 90%, which is only 2.8 points off that tuxedo — one
    // black-and-white DLC icon away from being painted as a solid block.
    //
    // Sampled rather than exhaustive: this runs once per icon and a 1024x512
    // source would otherwise be half a million reads for a yes/no.
    {
        qint64 seen = 0, grey = 0;
        const int step = qMax(1, qMin(img.width(), img.height()) / 32);
        for (int y = 0; y < img.height(); y += step) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); x += step) {
                if (qAlpha(row[x]) < 128) continue;
                ++seen;
                const int r = qRed(row[x]), g = qGreen(row[x]), b = qBlue(row[x]);
                if (qMax(r, qMax(g, b)) - qMin(r, qMin(g, b)) <= 12) ++grey;
            }
        }
        // A wholly transparent sample says nothing; treat it as line art, which
        // is what every icon was assumed to be before this.
        m_lineArt.insert(modelStem, seen == 0 || grey * 100 >= seen * 95);
    }

    return QPixmap::fromImage(
        img.scaledToHeight(height, Qt::SmoothTransformation));
}

bool IconCatalog::iconIsLineArt(const QString& modelStem) const
{
    return m_lineArt.value(modelStem, true);
}

}  // namespace fox
