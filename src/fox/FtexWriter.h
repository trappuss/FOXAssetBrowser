// FtexWriter.h — the inverse of FtexFile: a DDS back into the engine's own
// .ftex + .N.ftexs pair, for replacing a texture through the mod folder.
//
// IT RE-ENCODES INTO THE ORIGINAL'S OWN LAYOUT, and that is the whole design.
// The 64-byte header is kept verbatim and every mip keeps the ftexs file it
// already lived in; only the pixel payload and the four fields that describe
// where it sits are rewritten. So the result is a shape the game already ships
// and already loads, rather than a shape this tool invented and hopes is
// acceptable — which for a file the game reads at load time is the difference
// between a mod and a crash.
//
// THE CHUNK RULE IS MEASURED, NOT ASSUMED. Over the 283 shipped .ftex in the
// reference pull — 2,795 mips, 4,293 chunks:
//
//   • NOT ONE mip uses chunkCount 0. The reader has a branch for it and the
//     shipped data never takes it, so writing one would be writing a form the
//     engine has never been asked to read. Every mip written here is chunked.
//   • Every non-last chunk is exactly 16,384 bytes — 2,060 of 2,060 — and the
//     last is the remainder. chunkCount = ceil(size / 16384) reproduces the
//     shipped count on every mip in the corpus.
//   • A chunk is zlib-compressed when that is smaller and STORED AS IS
//     otherwise; the reader's test is literally `compressedSize != chunkSize`.
//     801 of the 4,293 shipped chunks are stored, so this is a normal case and
//     not a failure.
//   • The index record's offset is written in the plain form, relative to the
//     mip's own base. 3,514 of the shipped records use it; the 0x80000000
//     variant the other 779 use decodes to the same place, and emitting one
//     consistent form the engine reads in three and a half thousand places is
//     worth more than reproducing a distinction whose purpose is not settled.
#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>

namespace fox {

struct FtexWriteResult {
    QByteArray ftex;               // the .ftex itself
    QMap<int, QByteArray> ftexs;   // file number N -> the ".N.ftexs" payload
    QString error;                 // empty on success
    bool ok() const { return error.isEmpty(); }
};

// Re-encode `dds` into the layout `originalFtex` describes.
//
// The replacement must match the original's PIXEL FORMAT and TOP-MIP SIZE, and
// carry at least as many mips. Those are checked and refused with a sentence
// rather than coerced: a texture silently written at the wrong size is a file
// the game will read as garbage, and there is no way to notice that from
// inside this tool. A DDS with MORE mips than the original is fine — the extra
// small ones are dropped, which is the ordinary case for anything exported
// from a paint program with a full chain.
FtexWriteResult writeFtexLike(const QByteArray& originalFtex,
                              const QByteArray& dds);

// ONE MIP, chunked the way the shipped data chunks them: 16,384-byte pieces,
// each zlib-compressed or stored as-is depending on which is smaller, behind
// an 8-byte index record per piece whose offset is relative to the mip's base.
//
// Exposed because it is exactly what FtexFile::readChunkedMip consumes, and
// the acceptance test drives the two against each other over every mip in the
// install. Testing per FILE would have measured four textures on a partial
// pull — most shipped .ftex keep their two largest mips in .ftexs streams that
// a gear extract does not include — and per MIP measures all of them.
QByteArray chunkMip(const QByteArray& raw, int* chunkCountOut = nullptr);

}  // namespace fox
