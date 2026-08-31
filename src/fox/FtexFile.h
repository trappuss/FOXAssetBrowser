// FtexFile.h — Fox Engine .ftex texture + its streamed .N.ftexs mip files,
// assembled back into a standard .dds. Port of FtexTool (Atvaark), the
// reference converter.
//
// Layout facts (FtexTool):
//   • .ftex = 64-byte header + mipCount × 16-byte mip infos. PixelFormatType:
//     0 = A8R8G8B8, 1 = luminance(8), 2 = DXT1, 4 = DXT5. Nothing else ships.
//   • Each mip info says WHICH file its data lives in (ftexsFileNumber: 0 = the
//     .ftex itself, N = "<name>.N.ftexs") and where (offset, chunkCount).
//   • A mip is either one raw block (chunkCount 0) or chunked: 8-byte records
//     {u16 compressedSize, u16 chunkSize, u32 encodedOffset}, zlib per chunk,
//     offsets relative to the mip's base (0x80000000 flags "relative to the
//     index record" for single uncompressed chunks).
//   • DDS mip order = ftexs file number DESCENDING (high-numbered files hold
//     the large mips), mip-info order within a file.
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>
#include <functional>

namespace fox {

struct FtexMipInfo {
    qint32 offset = 0;
    qint32 decompressedSize = 0;
    qint32 size = 0;
    quint8 index = 0;
    quint8 ftexsFileNumber = 0;
    qint16 chunkCount = 0;
};

class FtexFile {
public:
    static bool isFtex(const QByteArray& data);

    bool parse(const QByteArray& ftexData);

    qint16 pixelFormatType() const { return m_pixelFormatType; }
    qint16 width() const { return m_width; }
    qint16 height() const { return m_height; }
    qint16 depth() const { return m_depth; }
    quint8 mipCount() const { return m_mipCount; }
    quint8 ftexsFileCount() const { return m_ftexsFileCount; }
    qint32 textureType() const { return m_textureType; }
    const QVector<FtexMipInfo>& mipInfos() const { return m_mipInfos; }
    QString errorString() const { return m_error; }

    // ONE MIP, read out of whichever file holds it.
    //
    // Split out of assembleDds rather than left inline because it is the exact
    // inverse of what FtexWriter's chunker produces, and the round-trip test
    // over the shipped corpus has to drive the two against each other directly
    // — a texture whose .N.ftexs streams are not all present cannot be
    // assembled at all, and on a partial pull that is nearly every one of them.
    // Testing per FILE would have measured four textures; testing per MIP
    // measures every mip in the install.
    //
    // `base` is the mip's own offset in `src`; every offset in its chunk index
    // is relative to it. `ok` is false when the record runs off the end or a
    // chunk fails to inflate.
    static QByteArray readChunkedMip(const QByteArray& src, qint64 base,
                                     int chunkCount, bool* ok = nullptr);

    // Assemble the full texture. `ftexsProvider(N)` must return the COMPLETE
    // content of "<name>.N.ftexs" (N ≥ 1); pass the .ftex's own bytes for the
    // inline mips (number 0). Missing high-mip files degrade gracefully: the
    // result keeps whatever mips could be read, `missingMips` (optional)
    // reports how many were dropped.
    QByteArray assembleDds(const QByteArray& ftexData,
                           const std::function<QByteArray(int)>& ftexsProvider,
                           int* missingMips = nullptr) const;

    // A human-readable one-liner for list panes ("DXT1 2048x2048, 11 mips, 2 ftexs").
    QString describe() const;

private:
    qint16 m_pixelFormatType = 0;
    qint16 m_width = 0;
    qint16 m_height = 0;
    qint16 m_depth = 0;
    quint8 m_mipCount = 0;
    quint8 m_ftexsFileCount = 0;
    qint32 m_textureType = 0;
    QVector<FtexMipInfo> m_mipInfos;
    QString m_error;
};

}  // namespace fox
