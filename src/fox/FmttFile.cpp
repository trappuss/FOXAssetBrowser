// FmttFile.cpp — see FmttFile.h.
#include "fox/FmttFile.h"

#include <QString>
#include <QtEndian>
#include <cstring>

namespace fox {

namespace {
constexpr int kPresetCount = 256;
constexpr int kPresetSize = 32;

float readFloat(const char* p)
{
    quint32 bits = 0;
    memcpy(&bits, p, 4);
    bits = qFromLittleEndian(bits);
    float f = 0.0f;
    memcpy(&f, &bits, 4);
    return f;
}
}  // namespace

bool FmttFile::isFmtt(const QByteArray& data)
{
    // No magic. The format is a bare array, so the only structural test is the
    // size — and the shipped file is exactly 256 * 32. A file that is a whole
    // number of records and no larger than the cap is accepted; anything else
    // is something other than a preset table.
    return data.size() >= kPresetSize && data.size() % kPresetSize == 0
        && data.size() <= kPresetCount * kPresetSize;
}

bool FmttFile::parse(const QByteArray& data)
{
    m_presets.clear();
    m_readCount = 0;
    m_error.clear();
    if (!isFmtt(data)) {
        m_error = QStringLiteral("not a %1-byte-aligned preset table (%2 bytes)")
                      .arg(kPresetSize)
                      .arg(data.size());
        return false;
    }
    const int n = int(data.size() / kPresetSize);
    m_readCount = n;
    m_presets.reserve(kPresetCount);
    for (int i = 0; i < n; ++i) {
        const char* p = data.constData() + qsizetype(i) * kPresetSize;
        MaterialPreset m;
        m.f0 = readFloat(p + 0x00);
        m.roughnessThreshold = readFloat(p + 0x04);
        m.reflectionDependDiffuse = readFloat(p + 0x08);
        m.anisotropicRoughness = readFloat(p + 0x0C);
        m.specular[0] = readFloat(p + 0x10);
        m.specular[1] = readFloat(p + 0x14);
        m.specular[2] = readFloat(p + 0x18);
        m.translucency = readFloat(p + 0x1C);
        m_presets.append(m);
    }
    // Pad to the full 256 with the dielectric default. An index past the end
    // of a short table then renders as plastic, which is wrong but harmless;
    // leaving it out of range would make every caller bounds-check instead.
    while (m_presets.size() < kPresetCount) m_presets.append(MaterialPreset{});
    return true;
}

MaterialPreset FmttFile::at(int index) const
{
    if (index < 0 || index >= m_presets.size()) return MaterialPreset{};
    return m_presets[index];
}

}  // namespace fox
