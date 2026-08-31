// FtexFile.cpp — see FtexFile.h.
#include "fox/FtexFile.h"

#include <QMap>
#include <QtEndian>

#include "fox/FoxZlib.h"

namespace fox {
namespace {

constexpr quint64 kFtexMagic = 4612226451348214854ULL;   // "FTEX" 85 EB 01 40

// DDS constants (standard header, as FtexTool writes it).
constexpr quint32 kDdsMagic = 0x20534444;                // "DDS "
constexpr quint32 kDdsdCaps = 0x1, kDdsdHeight = 0x2, kDdsdWidth = 0x4,
                  kDdsdPixelFormat = 0x1000, kDdsdMipMapCount = 0x20000,
                  kDdsdDepth = 0x800000;
constexpr quint32 kDdpfFourCc = 0x4, kDdpfRgb = 0x40, kDdpfAlphaPixels = 0x1,
                  kDdpfLuminance = 0x20000;
constexpr quint32 kCapsTexture = 0x1000, kCapsMipMap = 0x400008;

void putU32(QByteArray& b, quint32 v)
{
    char raw[4];
    qToLittleEndian(v, raw);
    b.append(raw, 4);
}

}  // namespace

bool FtexFile::isFtex(const QByteArray& data)
{
    return data.size() >= 64
        && qFromLittleEndian<quint64>(data.constData()) == kFtexMagic;
}

bool FtexFile::parse(const QByteArray& d)
{
    m_mipInfos.clear();
    m_error.clear();
    if (!isFtex(d)) {
        m_error = QStringLiteral("not an FTEX (magic mismatch)");
        return false;
    }
    const char* p = d.constData();
    m_pixelFormatType = qFromLittleEndian<qint16>(p + 8);
    m_width = qFromLittleEndian<qint16>(p + 10);
    m_height = qFromLittleEndian<qint16>(p + 12);
    m_depth = qFromLittleEndian<qint16>(p + 14);
    m_mipCount = static_cast<quint8>(p[16]);
    // p[17] nrtFlag, p[18..19] unknownFlags, p[20..23] == 1, p[24..27] == 0
    m_textureType = qFromLittleEndian<qint32>(p + 28);
    m_ftexsFileCount = static_cast<quint8>(p[32]);
    // p[33] additional count, p[34..47] zeros, p[48..63] hash

    if (d.size() < 64 + 16 * m_mipCount) {
        m_error = QStringLiteral("truncated mip-info table");
        return false;
    }
    for (int i = 0; i < m_mipCount; ++i) {
        const char* m = p + 64 + 16 * i;
        FtexMipInfo info;
        info.offset = qFromLittleEndian<qint32>(m + 0);
        info.decompressedSize = qFromLittleEndian<qint32>(m + 4);
        info.size = qFromLittleEndian<qint32>(m + 8);
        info.index = static_cast<quint8>(m[12]);
        info.ftexsFileNumber = static_cast<quint8>(m[13]);
        info.chunkCount = qFromLittleEndian<qint16>(m + 14);
        m_mipInfos.append(info);
    }
    return true;
}

QByteArray FtexFile::readChunkedMip(const QByteArray& src, qint64 base,
                                    int chunkCount, bool* ok)
{
    if (ok) *ok = false;
    QByteArray mipData;
    qsizetype indexPos = base;
    for (int c = 0; c < chunkCount; ++c) {
        if (indexPos < 0 || indexPos + 8 > src.size()) return {};
        const quint16 compressedSize =
            qFromLittleEndian<quint16>(src.constData() + indexPos);
        const quint16 chunkSize =
            qFromLittleEndian<quint16>(src.constData() + indexPos + 2);
        const quint32 encoded =
            qFromLittleEndian<quint32>(src.constData() + indexPos + 4);
        indexPos += 8;

        // Both forms mean "relative to the mip's base"; the high bit marks the
        // variant the shipped data uses on 779 of its 4,293 records and decodes
        // to the same place. FtexWriter emits only the plain form.
        const qint64 dataOffset = base
            + (encoded > 0x80000000u ? encoded - 0x80000000u : encoded);
        if (dataOffset < 0 || dataOffset + compressedSize > src.size()) return {};

        QByteArray chunk(src.constData() + dataOffset, compressedSize);
        // The format's own test, and the writer's: equal sizes means the chunk
        // was STORED, not compressed.
        if (compressedSize != chunkSize) {
            chunk = zlibInflate(chunk, chunkSize);
            if (chunk.isEmpty()) return {};
        }
        mipData += chunk;
    }
    if (ok) *ok = true;
    return mipData;
}

QByteArray FtexFile::assembleDds(const QByteArray& ftexData,
                                 const std::function<QByteArray(int)>& ftexsProvider,
                                 int* missingMips) const
{
    if (missingMips) *missingMips = 0;

    // Fetch each source file once. Number 0 = the .ftex itself.
    QMap<int, QByteArray> sources;
    sources.insert(0, ftexData);

    // Read every mip, keyed by its mip index (0 = largest). Emitting in index
    // order is equivalent to FtexTool's file-number-descending concatenation,
    // and — unlike it — stays correct when a streamed file is missing: the
    // header below shrinks to the largest mip actually present instead of
    // lying about the payload.
    QMap<int, QByteArray> mipsByIndex;
    int kept = 0;
    for (const FtexMipInfo& info : m_mipInfos) {
        const int fileNo = info.ftexsFileNumber;
        if (!sources.contains(fileNo))
            sources.insert(fileNo, ftexsProvider ? ftexsProvider(fileNo) : QByteArray());
        const QByteArray& src = sources[fileNo];
        if (src.isEmpty() || info.offset < 0 || info.offset >= src.size()) {
            if (missingMips) ++(*missingMips);
            continue;
        }

        QByteArray mipData;
        if (info.chunkCount == 0) {
            // One raw block of decompressedSize at offset. NOT ONE mip in the
            // 283 shipped textures of the reference pull takes this branch —
            // measured, 2,795 of 2,795 are chunked — which is why FtexWriter
            // never emits it.
            if (info.offset + static_cast<qint64>(info.decompressedSize) > src.size()) {
                if (missingMips) ++(*missingMips);
                continue;
            }
            mipData = QByteArray(src.constData() + info.offset, info.decompressedSize);
        } else {
            bool ok = false;
            mipData = readChunkedMip(src, info.offset, info.chunkCount, &ok);
            if (!ok) {
                if (missingMips) ++(*missingMips);
                continue;
            }
        }
        mipsByIndex.insert(info.index, mipData);
        ++kept;
    }
    if (kept == 0) return {};

    // Contiguous run from the largest surviving mip (a gap would desync the
    // consumer's size ladder — drop everything after it).
    const int firstIndex = mipsByIndex.firstKey();
    int runCount = 0;
    for (auto it = mipsByIndex.constBegin(); it != mipsByIndex.constEnd(); ++it) {
        if (it.key() != firstIndex + runCount) break;
        ++runCount;
    }
    const int outWidth = qMax(1, m_width >> firstIndex);
    const int outHeight = qMax(1, m_height >> firstIndex);

    // ── DDS header ───────────────────────────────────────────────────────────
    QByteArray dds;
    dds.reserve(128);
    putU32(dds, kDdsMagic);
    putU32(dds, 124);                                    // header size
    quint32 flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPixelFormat;
    quint32 caps = kCapsTexture;
    const int depthOut = m_depth > 1 ? m_depth : 0;
    if (depthOut) flags |= kDdsdDepth;
    const int mipOut = runCount > 1 ? runCount : 0;
    if (mipOut) { flags |= kDdsdMipMapCount; caps |= kCapsMipMap; }
    putU32(dds, flags);
    putU32(dds, static_cast<quint32>(outHeight));
    putU32(dds, static_cast<quint32>(outWidth));
    putU32(dds, 0);                                      // pitchOrLinearSize
    putU32(dds, static_cast<quint32>(depthOut));
    putU32(dds, static_cast<quint32>(mipOut));
    for (int i = 0; i < 11; ++i) putU32(dds, 0);         // reserved

    // Pixel format (32 bytes).
    putU32(dds, 32);
    switch (m_pixelFormatType) {
    case 0:   // A8R8G8B8
        putU32(dds, kDdpfRgb | kDdpfAlphaPixels);
        putU32(dds, 0);
        putU32(dds, 32);
        putU32(dds, 0x00FF0000);
        putU32(dds, 0x0000FF00);
        putU32(dds, 0x000000FF);
        putU32(dds, 0xFF000000);
        break;
    case 1:   // 8-bit luminance
        putU32(dds, kDdpfLuminance);
        putU32(dds, 0);
        putU32(dds, 8);
        putU32(dds, 0x000000FF);
        putU32(dds, 0);
        putU32(dds, 0);
        putU32(dds, 0);
        break;
    case 2:   // DXT1
        putU32(dds, kDdpfFourCc);
        putU32(dds, 0x31545844);
        for (int i = 0; i < 5; ++i) putU32(dds, 0);
        break;
    case 4:   // DXT5
    default:  // 4 is the only other shipped value; anything else falls here loudly
        putU32(dds, kDdpfFourCc);
        putU32(dds, 0x35545844);
        for (int i = 0; i < 5; ++i) putU32(dds, 0);
        break;
    }

    putU32(dds, caps);
    putU32(dds, 0);   // caps2
    putU32(dds, 0);   // caps3
    putU32(dds, 0);   // caps4
    putU32(dds, 0);   // reserved2

    // ── Payload: mip index ascending (largest first), contiguous run only ────
    if (missingMips) *missingMips += kept - runCount;
    int emitted = 0;
    for (auto it = mipsByIndex.constBegin();
         it != mipsByIndex.constEnd() && emitted < runCount; ++it, ++emitted)
        dds += it.value();

    return dds;
}

QString FtexFile::describe() const
{
    QString fmt;
    switch (m_pixelFormatType) {
    case 0: fmt = QStringLiteral("A8R8G8B8"); break;
    case 1: fmt = QStringLiteral("L8"); break;
    case 2: fmt = QStringLiteral("DXT1"); break;
    case 4: fmt = QStringLiteral("DXT5"); break;
    default: fmt = QStringLiteral("fmt%1").arg(m_pixelFormatType); break;
    }
    return QStringLiteral("%1 %2x%3, %4 mip%5, %6 ftexs")
        .arg(fmt)
        .arg(m_width)
        .arg(m_height)
        .arg(m_mipCount)
        .arg(m_mipCount == 1 ? QString() : QStringLiteral("s"))
        .arg(m_ftexsFileCount);
}

}  // namespace fox
