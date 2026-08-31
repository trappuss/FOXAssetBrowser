// BcDecode.cpp — see BcDecode.h. Straight implementations of the D3D BC1/BC3
// block formats (little-endian RGB565 endpoints; BC1's 2-color mode carries
// 1-bit punch-through alpha).
#include "fox/BcDecode.h"

#include <QtEndian>
#include <cstring>

namespace fox {
namespace bc {
namespace {

struct RGBA {
    quint8 r = 0, g = 0, b = 0, a = 255;
};

void expand565(quint16 c, RGBA* out)
{
    const int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    out->r = static_cast<quint8>((r5 << 3) | (r5 >> 2));
    out->g = static_cast<quint8>((g6 << 2) | (g6 >> 4));
    out->b = static_cast<quint8>((b5 << 3) | (b5 >> 2));
    out->a = 255;
}

// One 8-byte BC1 color block → 16 texels. `bc1Mode` enables the 2-color
// punch-through path (BC3's color half always interpolates 4 colors).
void decodeColorBlock(const quint8* blk, RGBA out[16], bool bc1Mode)
{
    const quint16 c0 = qFromLittleEndian<quint16>(blk);
    const quint16 c1 = qFromLittleEndian<quint16>(blk + 2);
    RGBA pal[4];
    expand565(c0, &pal[0]);
    expand565(c1, &pal[1]);
    if (!bc1Mode || c0 > c1) {
        pal[2].r = static_cast<quint8>((2 * pal[0].r + pal[1].r) / 3);
        pal[2].g = static_cast<quint8>((2 * pal[0].g + pal[1].g) / 3);
        pal[2].b = static_cast<quint8>((2 * pal[0].b + pal[1].b) / 3);
        pal[3].r = static_cast<quint8>((pal[0].r + 2 * pal[1].r) / 3);
        pal[3].g = static_cast<quint8>((pal[0].g + 2 * pal[1].g) / 3);
        pal[3].b = static_cast<quint8>((pal[0].b + 2 * pal[1].b) / 3);
    } else {
        pal[2].r = static_cast<quint8>((pal[0].r + pal[1].r) / 2);
        pal[2].g = static_cast<quint8>((pal[0].g + pal[1].g) / 2);
        pal[2].b = static_cast<quint8>((pal[0].b + pal[1].b) / 2);
        pal[3] = RGBA{0, 0, 0, 0};   // punch-through transparent black
    }
    const quint32 bits = qFromLittleEndian<quint32>(blk + 4);
    for (int i = 0; i < 16; ++i)
        out[i] = pal[(bits >> (2 * i)) & 0x3];
}

// One 8-byte BC3/BC4-style interpolated alpha block → 16 alpha values.
void decodeAlphaBlock(const quint8* blk, quint8 out[16])
{
    const int a0 = blk[0], a1 = blk[1];
    quint8 pal[8];
    pal[0] = static_cast<quint8>(a0);
    pal[1] = static_cast<quint8>(a1);
    if (a0 > a1) {
        for (int i = 1; i <= 6; ++i)
            pal[i + 1] = static_cast<quint8>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i <= 4; ++i)
            pal[i + 1] = static_cast<quint8>(((5 - i) * a0 + i * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    quint64 bits = 0;
    std::memcpy(&bits, blk + 2, 6);
    bits = qFromLittleEndian<quint64>(bits);
    for (int i = 0; i < 16; ++i)
        out[i] = pal[(bits >> (3 * i)) & 0x7];
}

QImage decodeBlocks(const QByteArray& data, int width, int height, bool dxt5)
{
    if (width <= 0 || height <= 0) return {};
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    const int blockSize = dxt5 ? 16 : 8;
    const qint64 needed = static_cast<qint64>(bw) * bh * blockSize;
    if (data.size() < needed) return {};

    QImage img(width, height, QImage::Format_RGBA8888);
    const quint8* src = reinterpret_cast<const quint8*>(data.constData());
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const quint8* blk = src + (static_cast<qint64>(by) * bw + bx) * blockSize;
            RGBA texel[16];
            quint8 alpha[16];
            if (dxt5) {
                decodeAlphaBlock(blk, alpha);
                decodeColorBlock(blk + 8, texel, /*bc1Mode=*/false);
                for (int i = 0; i < 16; ++i) texel[i].a = alpha[i];
            } else {
                decodeColorBlock(blk, texel, /*bc1Mode=*/true);
            }
            for (int py = 0; py < 4; ++py) {
                const int y = by * 4 + py;
                if (y >= height) break;
                quint8* dst = img.scanLine(y) + static_cast<qint64>(bx) * 16;
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px;
                    if (x >= width) break;
                    const RGBA& t = texel[py * 4 + px];
                    dst[px * 4 + 0] = t.r;
                    dst[px * 4 + 1] = t.g;
                    dst[px * 4 + 2] = t.b;
                    dst[px * 4 + 3] = t.a;
                }
            }
        }
    }
    return img;
}

}  // namespace

QImage decodeDxt1(const QByteArray& data, int width, int height)
{
    return decodeBlocks(data, width, height, /*dxt5=*/false);
}

QImage decodeDxt5(const QByteArray& data, int width, int height)
{
    return decodeBlocks(data, width, height, /*dxt5=*/true);
}

QImage decodeA8R8G8B8(const QByteArray& data, int width, int height)
{
    if (width <= 0 || height <= 0) return {};
    if (data.size() < static_cast<qint64>(width) * height * 4) return {};
    QImage img(width, height, QImage::Format_RGBA8888);
    const quint8* src = reinterpret_cast<const quint8*>(data.constData());
    for (int y = 0; y < height; ++y) {
        quint8* dst = img.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const quint8* s = src + (static_cast<qint64>(y) * width + x) * 4;
            // Stored BGRA (A8R8G8B8 little-endian) → RGBA.
            dst[x * 4 + 0] = s[2];
            dst[x * 4 + 1] = s[1];
            dst[x * 4 + 2] = s[0];
            dst[x * 4 + 3] = s[3];
        }
    }
    return img;
}

QImage decodeL8(const QByteArray& data, int width, int height)
{
    if (width <= 0 || height <= 0) return {};
    if (data.size() < static_cast<qint64>(width) * height) return {};
    QImage img(width, height, QImage::Format_RGBA8888);
    const quint8* src = reinterpret_cast<const quint8*>(data.constData());
    for (int y = 0; y < height; ++y) {
        quint8* dst = img.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const quint8 v = src[static_cast<qint64>(y) * width + x];
            dst[x * 4 + 0] = v;
            dst[x * 4 + 1] = v;
            dst[x * 4 + 2] = v;
            dst[x * 4 + 3] = 255;
        }
    }
    return img;
}

namespace {

// Bytes one whole surface of this format occupies at this size. Block formats
// round UP to whole 4x4 blocks, which is why a 2x2 BC1 mip is still 8 bytes and
// not two — get this wrong and every slice past the first lands mid-texture.
qint64 surfaceBytes(quint32 pfFlags, quint32 fourCc, quint32 bitCount, int w,
                    int h)
{
    w = qMax(1, w);
    h = qMax(1, h);
    if (pfFlags & 0x4) {
        const qint64 blocks = qint64((w + 3) / 4) * ((h + 3) / 4);
        if (fourCc == 0x31545844u) return blocks * 8;    // DXT1
        if (fourCc == 0x35545844u) return blocks * 16;   // DXT5
        return 0;
    }
    if (bitCount == 32) return qint64(w) * h * 4;
    if (bitCount == 8) return qint64(w) * h;
    return 0;
}

}  // namespace

int ddsSliceCount(const QByteArray& dds)
{
    if (dds.size() < 128) return 0;
    const quint8* p = reinterpret_cast<const quint8*>(dds.constData());
    if (qFromLittleEndian<quint32>(p) != 0x20534444u) return 0;
    const quint32 depth = qFromLittleEndian<quint32>(p + 24);
    // SANITY, not just >1. int(0xFFFFFFFF) is -1, and a negative count made
    // decodeDdsSlice(dds, 0) fail the `slice >= slices` test — so a DDS with
    // junk in dwDepth stopped previewing at all, through a function that used
    // to decode it. Anything that cannot be a real slice count reads as one
    // surface, which is what every 2D texture is.
    if (depth <= 1 || depth > 4096) return 1;
    return int(depth);
}

QImage decodeDdsSlice(const QByteArray& dds, int slice)
{
    if (dds.size() < 128 || slice < 0) return {};
    const quint8* p = reinterpret_cast<const quint8*>(dds.constData());
    if (qFromLittleEndian<quint32>(p) != 0x20534444u) return {};   // "DDS "
    const int height = static_cast<int>(qFromLittleEndian<quint32>(p + 12));
    const int width = static_cast<int>(qFromLittleEndian<quint32>(p + 16));
    const quint32 pfFlags = qFromLittleEndian<quint32>(p + 80);
    const quint32 fourCc = qFromLittleEndian<quint32>(p + 84);
    const quint32 bitCount = qFromLittleEndian<quint32>(p + 88);

    const int slices = ddsSliceCount(dds);
    if (slice >= slices) return {};
    const qint64 one = surfaceBytes(pfFlags, fourCc, bitCount, width, height);
    if (one <= 0) return {};
    const qint64 at = 128 + one * slice;
    if (at < 0 || at + one > dds.size()) return {};
    // fromRawData, not mid(): mid() takes an int here — which wraps NEGATIVE
    // past 2 GB — and copies every byte from the offset to the end of the
    // file on each call, so stepping a large volume re-copied most of it per
    // click. The decoders only read, and `dds` outlives them.
    const QByteArray payload =
        QByteArray::fromRawData(dds.constData() + at, dds.size() - at);
    if (pfFlags & 0x4) {   // FOURCC
        if (fourCc == 0x31545844u) return decodeDxt1(payload, width, height);
        if (fourCc == 0x35545844u) return decodeDxt5(payload, width, height);
        return {};
    }
    if (bitCount == 32) return decodeA8R8G8B8(payload, width, height);
    if (bitCount == 8) return decodeL8(payload, width, height);
    return {};
}

QImage decodeDds(const QByteArray& dds) { return decodeDdsSlice(dds, 0); }

QString selfTest()
{
    // BC1 block: c0 = pure red (0xF800), c1 = pure blue (0x001F), c0 > c1 →
    // 4-color mode. Indices: texel0 = 0 (red), texel1 = 1 (blue),
    // texel2 = 2 (2/3 red), texel3 = 3 (1/3 red), rest 0.
    const quint8 block[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00};
    const QImage img = decodeDxt1(QByteArray(reinterpret_cast<const char*>(block), 8), 4, 4);
    if (img.isNull()) return QStringLiteral("BC1 decode returned null");
    const QRgb t0 = img.pixel(0, 0), t1 = img.pixel(1, 0);
    if (qRed(t0) != 255 || qGreen(t0) != 0 || qBlue(t0) != 0)
        return QStringLiteral("BC1 texel0 wrong: %1,%2,%3")
            .arg(qRed(t0)).arg(qGreen(t0)).arg(qBlue(t0));
    if (qBlue(t1) != 255 || qRed(t1) != 0)
        return QStringLiteral("BC1 texel1 wrong");

    // BC3 alpha block: a0=255, a1=0 (8-value mode), all indices 1 → alpha 0.
    quint8 blk5[16] = {};
    blk5[0] = 255;
    blk5[1] = 0;
    // 3-bit indices all = 1: bits 0b001 repeated → bytes 0x49 0x92 0x24 pattern.
    blk5[2] = 0x49; blk5[3] = 0x92; blk5[4] = 0x24;
    blk5[5] = 0x49; blk5[6] = 0x92; blk5[7] = 0x24;
    std::memcpy(blk5 + 8, block, 8);
    const QImage img5 =
        decodeDxt5(QByteArray(reinterpret_cast<const char*>(blk5), 16), 4, 4);
    if (img5.isNull()) return QStringLiteral("BC3 decode returned null");
    if (qAlpha(img5.pixel(0, 0)) != 0)
        return QStringLiteral("BC3 alpha wrong: %1").arg(qAlpha(img5.pixel(0, 0)));

    // ── Volume slices ───────────────────────────────────────────────────
    // A synthetic 3-slice 2x2 A8R8G8B8 volume, each slice a flat known colour.
    // Built here rather than measured off an asset because NOTHING in the
    // reachable test data has depth > 1 — the arithmetic is the part that can
    // be wrong, and it can be proved without a cubemap to hand.
    {
        QByteArray v(128, '\0');
        auto put = [&v](int off, quint32 x) {
            for (int i = 0; i < 4; ++i) v[off + i] = char((x >> (8 * i)) & 0xFF);
        };
        put(0, 0x20534444u);   // "DDS "
        put(4, 124);
        put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x800000);   // caps|h|w|pf|depth
        put(12, 2);            // height
        put(16, 2);            // width
        put(24, 3);            // depth = 3 slices
        put(76, 32);           // pixel-format size
        put(80, 0x40 | 0x1);   // RGB | ALPHAPIXELS
        put(88, 32);           // bit count
        put(92, 0x00FF0000u);
        put(96, 0x0000FF00u);
        put(100, 0x000000FFu);
        put(104, 0xFF000000u);
        // Three 2x2 BGRA surfaces: slice 0 red, slice 1 green, slice 2 blue.
        const quint8 kSlice[3][4] = {{0, 0, 255, 255}, {0, 255, 0, 255},
                                     {255, 0, 0, 255}};
        for (int s = 0; s < 3; ++s)
            for (int px = 0; px < 4; ++px)
                for (int c = 0; c < 4; ++c)
                    v.append(char(kSlice[s][c]));
        if (ddsSliceCount(v) != 3)
            return QStringLiteral("volume: slice count %1, expected 3")
                .arg(ddsSliceCount(v));
        const int want[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        for (int s = 0; s < 3; ++s) {
            const QImage f = decodeDdsSlice(v, s);
            if (f.isNull())
                return QStringLiteral("volume: slice %1 decoded null").arg(s);
            const QRgb px = f.pixel(1, 1);
            if (qRed(px) != want[s][0] || qGreen(px) != want[s][1]
                || qBlue(px) != want[s][2])
                return QStringLiteral("volume: slice %1 is %2,%3,%4")
                    .arg(s).arg(qRed(px)).arg(qGreen(px)).arg(qBlue(px));
        }
        if (!decodeDdsSlice(v, 3).isNull())
            return QStringLiteral("volume: slice 3 of 3 should be null");
        // And slice 0 must be exactly what decodeDds() gives, so a 2D texture
        // cannot behave differently through the two entry points.
        if (decodeDds(v) != decodeDdsSlice(v, 0))
            return QStringLiteral("volume: decodeDds != slice 0");
    }
    return {};
}

}  // namespace bc
}  // namespace fox
