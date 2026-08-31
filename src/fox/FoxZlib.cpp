#include "fox/FoxZlib.h"

#include <zlib.h>

#include <QtGlobal>

namespace fox {

QByteArray zlibInflate(const QByteArray& compressed, int expectedSize)
{
    if (compressed.isEmpty()) return {};

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) return {};

    QByteArray out;
    out.resize(expectedSize > 0 ? expectedSize : qMax(compressed.size() * 4, 64 * 1024));

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
    zs.avail_in = static_cast<uInt>(compressed.size());

    int ret = Z_OK;
    qsizetype written = 0;
    do {
        if (written == out.size()) out.resize(out.size() * 2);
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + written);
        zs.avail_out = static_cast<uInt>(out.size() - written);
        ret = inflate(&zs, Z_NO_FLUSH);
        written = static_cast<qsizetype>(zs.total_out);
        if (ret != Z_OK && ret != Z_STREAM_END) break;
    } while (ret != Z_STREAM_END && (zs.avail_in > 0 || zs.avail_out == 0));

    inflateEnd(&zs);
    if (ret != Z_STREAM_END) return {};
    out.truncate(written);
    return out;
}

namespace {
// One compressor, two framings. `windowBits` is the only difference: 15 is the
// zlib-wrapped stream .ftexs chunks use, -15 is the raw one ZIP wants.
QByteArray deflateWith(const QByteArray& raw, int level, int windowBits)
{
    if (raw.isEmpty()) return {};
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, level, Z_DEFLATED, windowBits, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return {};

    QByteArray out;
    out.resize(static_cast<qsizetype>(deflateBound(&zs, uLong(raw.size()))));
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.constData()));
    zs.avail_in = static_cast<uInt>(raw.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());

    const int ret = deflate(&zs, Z_FINISH);
    const qsizetype written = static_cast<qsizetype>(zs.total_out);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) return {};
    out.truncate(written);
    return out;
}
}  // namespace

QByteArray zlibDeflate(const QByteArray& raw, int level)
{
    return deflateWith(raw, level, 15);
}

QByteArray zlibDeflateRaw(const QByteArray& raw, int level)
{
    return deflateWith(raw, level, -15);
}

}  // namespace fox
