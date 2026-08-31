// FtexWriter.cpp — see FtexWriter.h.
#include "fox/FtexWriter.h"

#include <QtEndian>

#include "fox/FoxZlib.h"
#include "fox/FtexFile.h"

namespace fox {
namespace {

// Measured over the shipped corpus: every non-last chunk is exactly this, on
// all 2,060 of them. See the header.
constexpr int kChunk = 16384;

constexpr quint32 kDdsMagic = 0x20534444;

void putU16(QByteArray& b, quint16 v)
{
    char raw[2];
    qToLittleEndian(v, raw);
    b.append(raw, 2);
}
void putU32(QByteArray& b, quint32 v)
{
    char raw[4];
    qToLittleEndian(v, raw);
    b.append(raw, 4);
}

// Bytes one mip occupies, for the four pixel formats FtexFile knows. Block
// formats round UP to whole 4x4 blocks, which is why a 2x2 DXT1 mip is still
// eight bytes and not two — get this wrong and every mip after the first is
// read from the wrong offset.
qint64 mipBytes(int pixelFormatType, int w, int h)
{
    w = qMax(1, w);
    h = qMax(1, h);
    switch (pixelFormatType) {
        case 0: return qint64(w) * h * 4;             // A8R8G8B8
        case 1: return qint64(w) * h;                 // L8
        case 2: return qint64((w + 3) / 4) * ((h + 3) / 4) * 8;    // DXT1
        case 4: return qint64((w + 3) / 4) * ((h + 3) / 4) * 16;   // DXT5
        default: return 0;
    }
}

// What pixel-format type this DDS is, in FTEX's vocabulary, or -1.
int ddsFormatType(const QByteArray& dds)
{
    if (dds.size() < 128) return -1;
    const char* p = dds.constData();
    const quint32 pfFlags = qFromLittleEndian<quint32>(p + 80);
    const quint32 fourCc = qFromLittleEndian<quint32>(p + 84);
    if (pfFlags & 0x4) {                       // DDPF_FOURCC
        if (fourCc == 0x31545844) return 2;    // "DXT1"
        if (fourCc == 0x35545844) return 4;    // "DXT5"
        return -1;
    }
    if (pfFlags & 0x20000) return 1;           // DDPF_LUMINANCE
    if (pfFlags & 0x40) return 0;              // DDPF_RGB
    return -1;
}

}  // namespace

QByteArray chunkMip(const QByteArray& raw, int* chunkCountOut)
{
    const int chunkCount = int((raw.size() + kChunk - 1) / kChunk);
    if (chunkCountOut) *chunkCountOut = chunkCount;
    const int indexBytes = chunkCount * 8;
    QByteArray index, blobs;
    index.reserve(indexBytes);
    for (int c = 0; c < chunkCount; ++c) {
        const int at = c * kChunk;
        const int len = qMin(kChunk, int(raw.size()) - at);
        const QByteArray chunk = raw.mid(at, len);
        QByteArray stored = zlibDeflate(chunk);
        // The format's own rule, and the reader's: a chunk whose stored size
        // EQUALS its decompressed size is not inflated. A compression that did
        // not pay is not a failure, it is the other legal encoding — 801 of the
        // 4,293 shipped chunks are stored — and it has to be taken whenever
        // deflate came out no smaller, or the reader would inflate raw pixels.
        if (stored.isEmpty() || stored.size() >= len) stored = chunk;
        putU16(index, quint16(stored.size()));
        putU16(index, quint16(len));
        // Relative to the mip's base, in the plain form. Never with the high
        // bit set: the reader tests `encoded > 0x80000000`, so a value that
        // reached it would be re-read as the flagged variant and land short.
        const quint32 rel = quint32(indexBytes + blobs.size());
        Q_ASSERT(rel < 0x80000000u);
        putU32(index, rel);
        blobs += stored;
    }
    return index + blobs;
}

FtexWriteResult writeFtexLike(const QByteArray& originalFtex,
                              const QByteArray& dds)
{
    FtexWriteResult r;

    FtexFile orig;
    if (!orig.parse(originalFtex)) {
        r.error = QStringLiteral("the original is not a readable .ftex: %1")
                      .arg(orig.errorString());
        return r;
    }
    if (dds.size() < 128
        || qFromLittleEndian<quint32>(dds.constData()) != kDdsMagic) {
        r.error = QStringLiteral("the replacement is not a DDS file");
        return r;
    }

    const int ddsFmt = ddsFormatType(dds);
    if (ddsFmt < 0) {
        r.error = QStringLiteral(
            "the replacement's pixel format is not one Fox stores "
            "(DXT1, DXT5, A8R8G8B8 or 8-bit luminance)");
        return r;
    }
    if (ddsFmt != orig.pixelFormatType()) {
        r.error = QStringLiteral(
            "format mismatch — this texture is %1 and the replacement is %2. "
            "Re-save the replacement in the same format.")
            .arg(orig.describe().section(QLatin1Char(' '), 0, 0))
            .arg(ddsFmt == 2 ? QStringLiteral("DXT1")
                 : ddsFmt == 4 ? QStringLiteral("DXT5")
                 : ddsFmt == 1 ? QStringLiteral("L8")
                               : QStringLiteral("A8R8G8B8"));
        return r;
    }

    const int ddsH = int(qFromLittleEndian<quint32>(dds.constData() + 12));
    const int ddsW = int(qFromLittleEndian<quint32>(dds.constData() + 16));
    if (ddsW != orig.width() || ddsH != orig.height()) {
        r.error = QStringLiteral(
            "size mismatch — this texture is %1x%2 and the replacement is "
            "%3x%4. The engine reads the size out of the file's header, which "
            "is kept as it was, so a different size would be read as garbage.")
            .arg(orig.width()).arg(orig.height()).arg(ddsW).arg(ddsH);
        return r;
    }

    // ── Cut the DDS payload into mips ────────────────────────────────────
    // Largest first, which is the order assembleDds writes and the order every
    // tool that produces a DDS writes.
    QVector<QByteArray> ddsMips;
    qint64 pos = 128;
    for (int i = 0; i < orig.mipCount(); ++i) {
        const qint64 n = mipBytes(ddsFmt, orig.width() >> i, orig.height() >> i);
        if (n <= 0 || pos + n > dds.size()) break;
        ddsMips.append(QByteArray(dds.constData() + pos, int(n)));
        pos += n;
    }
    if (ddsMips.size() < orig.mipCount()) {
        r.error = QStringLiteral(
            "the replacement has %1 usable mip level(s) and this texture needs "
            "%2. Re-save it with a full mip chain.")
            .arg(ddsMips.size()).arg(orig.mipCount());
        return r;
    }

    // ── Lay each mip out in the file it already lived in ──────────────────
    // The mip-info table is rewritten in place, so the loop walks the ORIGINAL
    // table and keeps `index` and `ftexsFileNumber` from it. Nothing decides
    // which file a mip belongs in; the original already did.
    const int tableAt = 64;
    const int headerBytes = 64 + 16 * orig.mipCount();
    QMap<int, QByteArray> payload;      // file number -> bytes after its header
    QByteArray newHeader = originalFtex.left(headerBytes);
    if (newHeader.size() < headerBytes) {
        r.error = QStringLiteral("the original .ftex is truncated");
        return r;
    }

    for (int i = 0; i < orig.mipCount(); ++i) {
        const FtexMipInfo& info = orig.mipInfos()[i];
        if (info.index >= ddsMips.size()) {
            r.error = QStringLiteral("mip %1 is outside the replacement")
                          .arg(int(info.index));
            return r;
        }
        const QByteArray& raw = ddsMips[info.index];
        const int fileNo = info.ftexsFileNumber;
        QByteArray& dest = payload[fileNo];

        // The mip's base: where its chunk INDEX starts. Every offset in that
        // index is relative to this, which is why it is captured before a byte
        // of the index is written.
        const int base = (fileNo == 0 ? headerBytes : 0) + int(dest.size());

        int chunkCount = 0;
        const QByteArray packed = chunkMip(raw, &chunkCount);
        dest += packed;

        // …and the four fields that say where it all went.
        char* m = newHeader.data() + tableAt + 16 * i;
        qToLittleEndian<qint32>(base, m + 0);
        qToLittleEndian<qint32>(qint32(raw.size()), m + 4);
        qToLittleEndian<qint32>(qint32(packed.size()), m + 8);
        // index (m[12]) and ftexsFileNumber (m[13]) are deliberately untouched.
        qToLittleEndian<qint16>(qint16(chunkCount), m + 14);
    }

    r.ftex = newHeader + payload.value(0);
    for (auto it = payload.constBegin(); it != payload.constEnd(); ++it)
        if (it.key() != 0) r.ftexs.insert(it.key(), it.value());
    return r;
}

}  // namespace fox
