// FoxZlib.h — one inflate helper for every Fox container. QAR entry payloads
// and .ftexs chunks are both standard zlib streams (GzsTool uses Ionic's
// ZlibStream, FtexTool uses SharpZipLib's InflaterInputStream — both the
// zlib-wrapped format, windowBits 15).
#pragma once
#include <QByteArray>

namespace fox {

// Inflate a zlib stream. `expectedSize` (when known) preallocates the output;
// pass 0 to grow dynamically. Returns an empty array on corrupt input.
QByteArray zlibInflate(const QByteArray& compressed, int expectedSize = 0);

// …and the other direction, for writing a replacement texture back into the
// engine's own container. Same wrapped format, windowBits 15, so a stream this
// produces is one zlibInflate reads — which is what the FTEX round-trip test
// checks over the whole shipped corpus rather than on one sample.
//
// Returns empty on failure, which the one caller treats as "store this chunk
// uncompressed" rather than as an error: the format allows either, and 801 of
// the 4,293 chunks in the reference pull are stored as-is precisely because
// compressing them did not pay.
QByteArray zlibDeflate(const QByteArray& raw, int level = 9);

// RAW deflate — the same compressor with NO zlib header or trailer
// (windowBits -15). ZIP stores method-8 members this way, and a ZIP written
// with the wrapped form is one every unzip refuses. Kept beside its sibling
// and named for the difference rather than hidden behind a flag, because the
// two are not interchangeable and a caller that picks the wrong one produces
// a file that looks right and cannot be opened.
QByteArray zlibDeflateRaw(const QByteArray& raw, int level = 9);

}  // namespace fox
