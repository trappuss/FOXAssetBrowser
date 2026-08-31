// GzsFile.cpp — see GzsFile.h. Ciphers ported byte-for-byte from the removed
// GzsTool code (Utility/Encryption.cs at commit c889fa8).
#include "fox/GzsFile.h"

#include <QFile>
#include <QtEndian>
#include <utility>

namespace fox {
namespace {

constexpr quint32 kFooterMagic = 0x71610000;
constexpr quint32 kKeyMagic = 0xA0F8EFE6;

// The fixed GZ extension-id table (ids 0-106). Index IS the id.
const char* const kGzExtensions[] = {
    "", ".xml", ".json", ".ese", ".fxp", ".fpk", ".fpkd", ".fpkl", ".aib",
    ".frig", ".mtar", ".gani", ".evb", ".evf", ".ag.evf", ".cc.evf", ".fx.evf",
    ".sd.evf", ".vo.evf", ".fsd", ".fage", ".fago", ".fag", ".fagx", ".fagp",
    ".frdv", ".fdmg", ".des", ".fdes", ".aibc", ".mtl", ".fsml", ".fox",
    ".fox2", ".las", ".fstb", ".lua", ".fcnp", ".fcnpx", ".sub", ".fova",
    ".lad", ".lani", ".vfx", ".vfxbin", ".frt", ".gpfp", ".gskl", ".geom",
    ".tgt", ".path", ".fmdl", ".ftex", ".htre", ".tre2", ".grxla", ".grxoc",
    ".mog", ".pftxs", ".nav2", ".bnd", ".parts", ".phsd", ".ph", ".veh",
    ".sdf", ".sad", ".sim", ".fclo", ".clo", ".lng", ".uig", ".uil", ".uif",
    ".uia", ".fnt", ".utxl", ".uigb", ".vfxdb", ".rbs", ".aia", ".aim", ".aip",
    ".aigc", ".aig", ".ait", ".fsm", ".obr", ".obrb", ".lpsh", ".sani", ".rdb",
    ".phep", ".simep", ".atsh", ".txt", ".1.ftexs", ".2.ftexs", ".3.ftexs",
    ".4.ftexs", ".5.ftexs", ".sbp", ".mas", ".rdf", ".wem", ".lba", ".uilb",
};
constexpr int kGzExtensionCount =
    static_cast<int>(sizeof(kGzExtensions) / sizeof(kGzExtensions[0]));

// Outer per-entry stream ("DeEncryptQar"): each byte XORed with bits 16-23 of
// a linear 32-bit sequence seeded by the entry's block offset.
void deEncryptQar(QByteArray& data, quint32 offset)
{
    const qsizetype blockCount = data.size() / 8;
    quint8* p = reinterpret_cast<quint8*>(data.data());
    quint32 low = 101436752u * offset + 12679594u;
    qsizetype bufferOffset = 0;
    for (qsizetype i = 0; i < blockCount; ++i) {
        p[bufferOffset + 0] ^= static_cast<quint8>((low - 12679594u) >> 16);
        p[bufferOffset + 1] ^= static_cast<quint8>((low - 6339797u) >> 16);
        p[bufferOffset + 2] ^= static_cast<quint8>(low >> 16);
        p[bufferOffset + 3] ^= static_cast<quint8>((low + 6339797u) >> 16);
        p[bufferOffset + 4] ^= static_cast<quint8>((low + 12679594u) >> 16);
        p[bufferOffset + 5] ^= static_cast<quint8>((low + 19019391u) >> 16);
        p[bufferOffset + 6] ^= static_cast<quint8>((low + 25359188u) >> 16);
        p[bufferOffset + 7] ^= static_cast<quint8>((low + 31698985u) >> 16);
        bufferOffset += 8;
        low += 50718376u;
    }
    const qsizetype remaining = data.size() & 7;
    const quint32 v5 = 8u * (static_cast<quint32>(blockCount) + 2u * offset);
    quint32 v10 = 6339797u * v5;
    for (qsizetype i = 0; i < remaining; ++i) {
        p[bufferOffset] ^= static_cast<quint8>(v10 >> 16);
        v10 += 6339797u;
        ++bufferOffset;
    }
}

// Inner keyed stream ("DeEncrypt"): 32-bit words XORed with a multiplicative
// congruential sequence; the final 0-3 bytes stay clear.
void deEncrypt(QByteArray& data, quint32 key)
{
    char* raw = data.data();
    qsizetype len = data.size();
    qsizetype offset = 0;
    quint32 v5 = key | ((key ^ 0xFFFFCDECu) << 16);
    const quint32 i = 69069u * key;
    while (len >= 4) {
        quint32 v = qFromLittleEndian<quint32>(raw + offset);
        v ^= v5;
        qToLittleEndian(v, raw + offset);
        v5 = 3u * (i + 23023u * v5);
        offset += 4;
        len -= 4;
    }
}

}  // namespace

QString GzsFile::extensionForId(int extId)
{
    if (extId < 0 || extId >= kGzExtensionCount) return QStringLiteral("?");
    return QLatin1String(kGzExtensions[extId]);
}

int GzsFile::extensionCount()
{
    return kGzExtensionCount;
}

bool GzsFile::isGzs(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 20) return false;
    if (!f.seek(f.size() - 20)) return false;
    quint8 footer[20];
    if (f.read(reinterpret_cast<char*>(footer), 20) != 20) return false;
    return qFromLittleEndian<quint32>(footer + 4) == kFooterMagic
        && qFromLittleEndian<quint32>(footer + 16) == 20u;
}

bool GzsFile::open(const QString& filePath)
{
    m_filePath = filePath;
    m_entries.clear();
    m_error.clear();

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("cannot open: %1").arg(f.errorString());
        return false;
    }
    if (f.size() < 20 || !f.seek(f.size() - 20)) {
        m_error = QStringLiteral("too small for a g0s footer");
        return false;
    }
    quint8 footer[20];
    if (f.read(reinterpret_cast<char*>(footer), 20) != 20) {
        m_error = QStringLiteral("footer read failed");
        return false;
    }
    const quint32 entryCount = qFromLittleEndian<quint32>(footer + 0);
    const quint32 magic = qFromLittleEndian<quint32>(footer + 4);
    const quint32 entryBlockOffset = qFromLittleEndian<quint32>(footer + 8);
    const quint32 footerSize = qFromLittleEndian<quint32>(footer + 16);
    if (magic != kFooterMagic || footerSize != 20u) {
        m_error = QStringLiteral("not a g0s archive (footer magic mismatch)");
        return false;
    }
    if (entryCount > 4000000u) {
        m_error = QStringLiteral("implausible entry count %1").arg(entryCount);
        return false;
    }
    if (!f.seek(static_cast<qint64>(entryBlockOffset) * 16)) {
        m_error = QStringLiteral("bad entry table offset");
        return false;
    }
    m_entries.reserve(static_cast<int>(entryCount));
    for (quint32 i = 0; i < entryCount; ++i) {
        quint8 rec[16];
        if (f.read(reinterpret_cast<char*>(rec), 16) != 16) {
            m_error = QStringLiteral("truncated entry table");
            m_entries.clear();
            return false;
        }
        GzsEntry e;
        e.hash = qFromLittleEndian<quint64>(rec + 0);
        e.offset16 = qFromLittleEndian<quint32>(rec + 8);
        e.size = qFromLittleEndian<quint32>(rec + 12);
        m_entries.append(e);
    }
    return true;
}

void GzsFile::adopt(const QString& filePath, QVector<GzsEntry>&& entries)
{
    m_filePath = filePath;
    m_entries = std::move(entries);
    m_error.clear();
}

QByteArray GzsFile::readEntry(const GzsEntry& entry) const
{
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    if (!f.seek(static_cast<qint64>(entry.offset16) * 16)) return {};
    QByteArray data = f.read(entry.size);
    if (data.size() != static_cast<qsizetype>(entry.size)) return {};

    deEncryptQar(data, entry.offset16);

    if (data.size() >= 8
        && qFromLittleEndian<quint32>(data.constData()) == kKeyMagic) {
        const quint32 key = qFromLittleEndian<quint32>(data.constData() + 4);
        data.remove(0, 8);
        deEncrypt(data, key);
    }
    return data;
}

}  // namespace fox
