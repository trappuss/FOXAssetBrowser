// Fox2File.cpp — see Fox2File.h. Field order mirrors FoxFile.cs /
// FoxEntity.cs / FoxProperty.cs exactly.
#include "fox/Fox2File.h"

#include <QHash>
#include <QtEndian>

namespace fox {
namespace {

constexpr quint32 kMagic = 0x786f62f2;      // 0xf2 "box"
constexpr quint32 kEntityMagic = 0x746e65;  // "ent"

struct Rd {
    const char* p;
    qsizetype size;
    qsizetype pos = 0;
    bool ok = true;

    bool seek(qsizetype to)
    {
        if (to < 0 || to > size) { ok = false; return false; }
        pos = to;
        return true;
    }
    void align16()
    {
        const qsizetype r = pos % 16;
        if (r) seek(pos + (16 - r));
    }
    quint8 u8()
    {
        if (pos + 1 > size) { ok = false; return 0; }
        return static_cast<quint8>(p[pos++]);
    }
    quint16 u16()
    {
        if (pos + 2 > size) { ok = false; return 0; }
        const quint16 v = qFromLittleEndian<quint16>(p + pos);
        pos += 2;
        return v;
    }
    qint16 s16() { return static_cast<qint16>(u16()); }
    quint32 u32()
    {
        if (pos + 4 > size) { ok = false; return 0; }
        const quint32 v = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        return v;
    }
    qint32 s32() { return static_cast<qint32>(u32()); }
    quint64 u64()
    {
        if (pos + 8 > size) { ok = false; return 0; }
        const quint64 v = qFromLittleEndian<quint64>(p + pos);
        pos += 8;
        return v;
    }
    float f32()
    {
        const quint32 v = u32();
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    double f64()
    {
        const quint64 v = u64();
        double d;
        memcpy(&d, &v, 8);
        return d;
    }
};

// Payload floats per multi-float type (Vector3 is stored as FOUR floats).
int floatCountFor(Fox2Type t)
{
    switch (t) {
    case Fox2Type::Vector3:
    case Fox2Type::Vector4:
    case Fox2Type::Quat:
    case Fox2Type::Color: return 4;
    case Fox2Type::Matrix3: return 9;
    case Fox2Type::Matrix4: return 16;
    default: return 0;
    }
}

// Read ONE value of `t`.
QVariant readValue(Rd& r, Fox2Type t)
{
    switch (t) {
    case Fox2Type::Int8: return static_cast<int>(static_cast<qint8>(r.u8()));
    case Fox2Type::UInt8: return static_cast<uint>(r.u8());
    case Fox2Type::Int16: return static_cast<int>(r.s16());
    case Fox2Type::UInt16: return static_cast<uint>(r.u16());
    case Fox2Type::Int32: return r.s32();
    case Fox2Type::UInt32: return r.u32();
    case Fox2Type::Int64: return static_cast<qlonglong>(r.u64());
    case Fox2Type::UInt64: return static_cast<qulonglong>(r.u64());
    case Fox2Type::Float: return static_cast<double>(r.f32());
    case Fox2Type::Double: return r.f64();
    case Fox2Type::Bool: return r.u8() != 0;
    case Fox2Type::String:
    case Fox2Type::Path:
    case Fox2Type::FilePtr:
        return QVariant::fromValue<qulonglong>(r.u64());   // resolved later
    case Fox2Type::EntityPtr:
    case Fox2Type::EntityHandle:
        return QVariant::fromValue<qulonglong>(r.u64());
    case Fox2Type::EntityLink: {
        // 3 × string hash + u64 handle.
        QVariantList link;
        for (int i = 0; i < 3; ++i)
            link.append(QVariant::fromValue<qulonglong>(r.u64()));
        link.append(QVariant::fromValue<qulonglong>(r.u64()));
        return link;
    }
    case Fox2Type::WideVector3: {
        QVariantList v;
        v << static_cast<double>(r.f32()) << static_cast<double>(r.f32())
          << static_cast<double>(r.f32()) << static_cast<int>(r.u16())
          << static_cast<int>(r.u16());
        return v;
    }
    default: {
        const int n = floatCountFor(t);
        if (n > 0) {
            QVariantList v;
            for (int i = 0; i < n; ++i) v.append(static_cast<double>(r.f32()));
            return v;
        }
        return {};   // PropertyInfo / unknown — skipped via record size
    }
    }
}

}  // namespace

QString Fox2Property::typeName() const
{
    static const char* kNames[] = {
        "int8", "uint8", "int16", "uint16", "int32", "uint32", "int64",
        "uint64", "float", "double", "bool", "String", "Path", "EntityPtr",
        "Vector3", "Vector4", "Quat", "Matrix3", "Matrix4", "Color", "FilePtr",
        "EntityHandle", "EntityLink", "PropertyInfo", "WideVector3"};
    const int t = static_cast<int>(type);
    QString n = t >= 0 && t <= 24 ? QLatin1String(kNames[t])
                                  : QStringLiteral("type%1").arg(t);
    switch (container) {
    case Fox2Container::DynamicArray: n += QStringLiteral("[]"); break;
    case Fox2Container::StringMap: n += QStringLiteral("{map}"); break;
    case Fox2Container::List: n += QStringLiteral("(list)"); break;
    default: break;
    }
    return n;
}

QString Fox2Property::valueText(int i) const
{
    if (i < 0 || i >= values.size()) return {};
    const QVariant& v = values[i];
    if (v.typeId() == QMetaType::QVariantList) {
        QStringList parts;
        for (const QVariant& e : v.toList()) parts << e.toString();
        return QLatin1Char('(') + parts.join(QStringLiteral(", "))
            + QLatin1Char(')');
    }
    return v.toString();
}

const Fox2Property* Fox2Entity::find(const QString& propName) const
{
    for (const Fox2Property& p : statics)
        if (p.name == propName) return &p;
    for (const Fox2Property& p : dynamics)
        if (p.name == propName) return &p;
    return nullptr;
}

bool Fox2File::isFox2(const QByteArray& data)
{
    return data.size() >= 32
        && qFromLittleEndian<quint32>(data.constData()) == kMagic;
}

QString Fox2File::lookup(quint64 hash) const
{
    const auto it = m_strings.constFind(hash);
    if (it != m_strings.constEnd()) return it.value();
    return hash == 0 ? QString() : QStringLiteral("0x%1").arg(hash, 0, 16);
}

bool Fox2File::parse(const QByteArray& data)
{
    m_entities.clear();
    m_strings.clear();
    m_error.clear();

    if (!isFox2(data)) {
        m_error = QStringLiteral("not a Fox2 binary");
        return false;
    }
    Rd r{data.constData(), data.size()};
    r.u32();                            // magic
    r.u32();                            // version (2/3/5 seen)
    const qint32 entityCount = r.s32();
    const qint32 stringTableOffset = r.s32();
    r.s32();                            // data offset (header size)
    r.seek(32);
    if (!r.ok || entityCount < 0 || entityCount > 100000) {
        m_error = QStringLiteral("corrupt Fox2 header");
        return false;
    }

    // ── String table first (it resolves everything else) ────────────────────
    if (stringTableOffset > 0 && stringTableOffset < data.size()) {
        Rd sr{data.constData(), data.size(),
              static_cast<qsizetype>(stringTableOffset)};
        while (sr.ok) {
            const quint64 h = sr.u64();
            if (!sr.ok || h == 0) break;
            const quint32 len = sr.u32();
            if (!sr.ok || len > 4096 || sr.pos + len > sr.size) break;
            m_strings.insert(
                h, QString::fromUtf8(data.constData() + sr.pos,
                                     static_cast<int>(len)));
            sr.pos += len;
        }
    }

    // ── Entities ────────────────────────────────────────────────────────────
    for (int e = 0; e < entityCount && r.ok; ++e) {
        const qsizetype entityStart = r.pos;
        r.s16();                                    // header size (64)
        r.s16();                                    // unknown1
        r.s16();                                    // padding
        const quint32 magic = r.u32();              // "ent" at +6
        if (magic != kEntityMagic) {
            m_error = QStringLiteral("entity %1: bad magic").arg(e);
            return false;
        }
        Fox2Entity ent;
        ent.address = r.u32();
        r.u32();                                    // padding
        r.s32();                                    // unknown2
        r.s32();                                    // unknown5
        ent.version = static_cast<quint16>(r.s16());
        ent.classHash = r.u64();
        const quint16 staticCount = r.u16();
        const quint16 dynCount = r.u16();
        r.s32();                                    // offset (header size)
        r.s32();                                    // static data size
        const qint32 dataSize = r.s32();
        r.align16();                                // header runs to 64 bytes

        ent.className = lookup(ent.classHash);

        const auto readProps = [&](int count, QVector<Fox2Property>* out) {
            for (int i = 0; i < count && r.ok; ++i) {
                const qsizetype propStart = r.pos;
                Fox2Property prop;
                prop.nameHash = r.u64();
                prop.type = static_cast<Fox2Type>(r.u8());
                prop.container = static_cast<Fox2Container>(r.u8());
                const qint16 valueCount = r.s16();
                r.s16();                            // offset (32)
                const quint16 recordSize = r.u16();
                r.seek(r.pos + 16);                 // 16 reserved bytes
                if (!r.ok) return;

                prop.name = lookup(prop.nameHash);
                if (prop.container == Fox2Container::StringMap) {
                    for (int v = 0; v < valueCount && r.ok; ++v) {
                        const quint64 keyHash = r.u64();
                        prop.mapKeys.append(lookup(keyHash));
                        prop.values.append(readValue(r, prop.type));
                        r.align16();
                    }
                } else {
                    for (int v = 0; v < valueCount && r.ok; ++v) {
                        // Zero-byte value types (PropertyInfo/unknown) must
                        // not spin the loop into unbounded allocation — every
                        // stored value must consume file bytes.
                        const qsizetype before = r.pos;
                        prop.values.append(readValue(r, prop.type));
                        if (r.pos == before) { prop.values.removeLast(); break; }
                    }
                }

                // Resolve string-hash values in place.
                for (QVariant& v : prop.values) {
                    if ((prop.type == Fox2Type::String
                         || prop.type == Fox2Type::Path
                         || prop.type == Fox2Type::FilePtr)
                        && v.typeId() == QMetaType::ULongLong) {
                        v = lookup(v.toULongLong());
                    } else if (prop.type == Fox2Type::EntityLink
                               && v.typeId() == QMetaType::QVariantList) {
                        QVariantList link = v.toList();
                        for (int k = 0; k < 3 && k < link.size(); ++k)
                            link[k] = lookup(link[k].toULongLong());
                        v = link;
                    }
                }

                // The record size is authoritative (covers alignment and any
                // payload type this reader doesn't model, e.g. PropertyInfo).
                if (recordSize >= 32
                    && propStart + recordSize <= data.size())
                    r.seek(propStart + recordSize);
                else
                    r.align16();
                out->append(prop);
            }
        };
        readProps(staticCount, &ent.statics);
        readProps(dynCount, &ent.dynamics);

        // Entity dataSize is authoritative for the next entity's position.
        if (dataSize >= 64 && entityStart + dataSize <= data.size())
            r.seek(entityStart + dataSize);
        else
            r.align16();

        m_entities.append(ent);
    }

    if (!r.ok) {
        m_error = QStringLiteral("truncated Fox2 data");
        return false;
    }
    return !m_entities.isEmpty();
}

}  // namespace fox
