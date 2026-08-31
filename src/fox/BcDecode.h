// BcDecode.h — CPU DXT1/DXT5 (BC1/BC3) → RGBA8888 QImage, for in-app texture
// preview and PNG export. Fox Engine ships ONLY DXT1, DXT5, A8R8G8B8 and L8
// (FtexTool's converter is exhaustive), so those four are the whole surface.
//
// Decoding on the CPU is deliberate: uploading Fox's compressed mips straight
// to glCompressedTexImage2D crashed inside an NVIDIA driver with no catchable
// error during the previous project. A software decode costs milliseconds at
// preview sizes and cannot take the process down.
#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>

namespace fox {
namespace bc {

// `data` is tightly packed block data (no row alignment), top-left origin.
QImage decodeDxt1(const QByteArray& data, int width, int height);
QImage decodeDxt5(const QByteArray& data, int width, int height);

// Uncompressed FTEX payloads.
QImage decodeA8R8G8B8(const QByteArray& data, int width, int height);
QImage decodeL8(const QByteArray& data, int width, int height);

// Decode a whole DDS blob (as produced by FtexFile::assembleDds) — reads the
// header, picks the right decoder, returns mip 0. Null image when unsupported.
QImage decodeDds(const QByteArray& dds);

// How many SLICES a DDS carries, from its header's depth field. 1 for an
// ordinary 2D texture, which is nearly everything.
//
// Fox writes a volume texture as a depth>1 DDS with caps2 = 0 — never as a
// cubemap — so "faces" in the template's sense are slices here, and this is
// what the Textures tab offers to step through.
int ddsSliceCount(const QByteArray& dds);

// Decode ONE slice. Slice 0 is what decodeDds() returns, so a 2D texture
// behaves identically either way. Out-of-range gives a null image.
//
// The layout is D3D's: for each mip level, every slice of that level, one after
// another — so slice N of the top level starts N surfaces in.
QImage decodeDdsSlice(const QByteArray& dds, int slice);

// Startup self-check with hand-built blocks; empty string = pass.
QString selfTest();

}  // namespace bc
}  // namespace fox
